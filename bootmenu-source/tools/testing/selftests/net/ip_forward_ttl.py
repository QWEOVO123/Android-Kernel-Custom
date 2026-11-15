#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Exercise IPv4 forwarding TTL modes on the running kernel using veth/netns."""

import ctypes
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import time


KEY = "net.ipv4.ip_forward_ttl_mode"
SYSCTL = Path("/proc/sys/net/ipv4/ip_forward_ttl_mode")
PUBLIC = Path("/proc/ttl_ctl")
USE_PUBLIC = "--public" in sys.argv[1:]
PAYLOAD = b"ipv4-forward-ttl-selftest"
PORT = 25463
SELF = str(Path(__file__).resolve())


def checksum_valid(header):
    total = sum(struct.unpack("!%dH" % (len(header) // 2), header))
    while total >> 16:
        total = (total & 0xffff) + (total >> 16)
    return total == 0xffff


def receive():
    with socket.socket(socket.AF_INET, socket.SOCK_RAW, socket.IPPROTO_UDP) as sock:
        sock.bind(("198.51.100.2", 0))
        sock.settimeout(2)
        print("ready", flush=True)
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            sock.settimeout(max(0.01, deadline - time.monotonic()))
            try:
                packet = sock.recv(65535)
            except socket.timeout:
                break
            ihl = (packet[0] & 15) * 4
            if len(packet) < ihl + 8:
                continue
            if struct.unpack("!H", packet[ihl + 2:ihl + 4])[0] != PORT:
                continue
            if packet[ihl + 8:] == PAYLOAD:
                print(json.dumps({"ttl": packet[8],
                                  "checksum": checksum_valid(packet[:ihl])}))
                return
        print(json.dumps({"ttl": None, "checksum": None}))


def send(ttl):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(("192.0.2.2", PORT + 1))
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_TTL, ttl)
        sock.sendto(PAYLOAD, ("198.51.100.2", PORT))


def child_default():
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.unshare(0x40000000) != 0:  # CLONE_NEWNET
        raise OSError(ctypes.get_errno(), "unshare(CLONE_NEWNET)")
    print(SYSCTL.read_text().strip())


def public_mode(value=None):
    # Namespace setup is privileged, but accessing the knob must not be.
    if os.geteuid() == 0:
        os.setgroups([])
        os.setgid(65534)
        os.setuid(65534)
    if value is not None:
        PUBLIC.write_text(value + "\n")
    print(PUBLIC.read_text().strip())


def command(*args):
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout.strip()


def run():
    if USE_PUBLIC and not PUBLIC.exists():
        print("1..0 # SKIP requires CONFIG_IP_FORWARD_TTL_PUBLIC_CONTROL kernel")
        return 4
    if not SYSCTL.exists() or not shutil.which("ip") or not shutil.which("sysctl"):
        print("1..0 # SKIP requires patched kernel, iproute2 and sysctl")
        return 4
    if os.geteuid() != 0:
        print("1..0 # SKIP requires root with network namespace capabilities")
        return 4

    prefix = "ttl-mode-%d-" % os.getpid()
    source, router, sink = [prefix + name for name in ("src", "rt", "dst")]
    created = []
    host_mode = SYSCTL.read_text().strip()
    count = 0

    def ns(namespace, *args):
        return command("ip", "netns", "exec", namespace, *args)

    def mode(namespace, value=None):
        if USE_PUBLIC:
            args = [sys.executable, SELF, "--public-mode"]
            if value is not None:
                args.append(str(value))
            return ns(namespace, *args)
        if value is not None:
            ns(namespace, "sysctl", "-qw", "%s=%s" % (KEY, value))
        return ns(namespace, "sysctl", "-n", KEY)

    def check(description, condition):
        nonlocal count
        count += 1
        print("%s %d - %s" % ("ok" if condition else "not ok", count, description), flush=True)
        if not condition:
            raise AssertionError(description)

    def packet(ttl, expected):
        process = subprocess.Popen(
            ["ip", "netns", "exec", sink, sys.executable, SELF, "--receive"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            if process.stdout.readline().strip() != "ready":
                raise RuntimeError("receiver failed: " + process.stderr.read())
            ns(source, sys.executable, SELF, "--send", str(ttl))
            output, errors = process.communicate(timeout=5)
            if process.returncode:
                raise RuntimeError(errors)
            result = json.loads(output)
            return result["ttl"] == expected and (expected is None or result["checksum"])
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()

    try:
        try:
            command("ip", "netns", "add", source)
        except subprocess.CalledProcessError as error:
            print("1..0 # SKIP cannot create netns: " + error.stderr.strip())
            return 4
        created.append(source)
        for name in (router, sink):
            command("ip", "netns", "add", name)
            created.append(name)
        command("ip", "-n", source, "link", "add", "s0", "type", "veth",
                "peer", "name", "r0", "netns", router)
        command("ip", "-n", router, "link", "add", "r1", "type", "veth",
                "peer", "name", "d0", "netns", sink)
        for name in created:
            command("ip", "-n", name, "link", "set", "lo", "up")
        for name, device, address in (
                (source, "s0", "192.0.2.2/24"), (router, "r0", "192.0.2.1/24"),
                (router, "r1", "198.51.100.1/24"), (sink, "d0", "198.51.100.2/24")):
            command("ip", "-n", name, "addr", "add", address, "dev", device)
            command("ip", "-n", name, "link", "set", device, "up")
        command("ip", "-n", source, "route", "add", "default", "via", "192.0.2.1")
        command("ip", "-n", sink, "route", "add", "default", "via", "198.51.100.1")
        ns(router, "sysctl", "-qw", "net.ipv4.ip_forward=1")

        print("1..10", flush=True)
        check("new namespaces default to normal forwarding", all(mode(n) == "0" for n in created))
        check("default: TTL 64 becomes 63 with valid checksum", packet(64, 63))
        check("default: TTL 2 becomes 1 with valid checksum", packet(2, 1))
        check("default: TTL 1 expires", packet(1, None))
        mode(router, 1)
        check("mode is isolated; child netns starts at 0",
              mode(source) == mode(sink) == "0" and
              ns(router, sys.executable, SELF, "--child-default") == "0" and
              mode(router) == "1" and ns(router, "sysctl", "-n", KEY) == "1" and
              SYSCTL.read_text().strip() == host_mode)
        check("preserve: TTL 64 stays 64 with valid checksum", packet(64, 64))
        check("preserve: TTL 2 stays 2 with valid checksum", packet(2, 2))
        check("preserve: TTL 1 still expires", packet(1, None))
        invalid_rejected = True
        for value in ("-1", "2", "garbage"):
            args = ([sys.executable, SELF, "--public-mode", value] if USE_PUBLIC
                    else ["sysctl", "-qw", "%s=%s" % (KEY, value)])
            result = subprocess.run(["ip", "netns", "exec", router, *args], capture_output=True)
            invalid_rejected &= result.returncode != 0 and mode(router) == "1"
        check("invalid values are rejected without changing mode", invalid_rejected)
        mode(router, 0)
        check("switch back to normal without route flush", packet(64, 63))
        return 0
    except (AssertionError, OSError, RuntimeError, ValueError,
            subprocess.SubprocessError) as error:
        print("# " + str(error), flush=True)
        return 1
    finally:
        for name in reversed(created):
            subprocess.run(["ip", "netns", "del", name], capture_output=True)


if __name__ == "__main__":
    if sys.argv[1:] == ["--receive"]:
        receive()
    elif len(sys.argv) == 3 and sys.argv[1] == "--send":
        send(int(sys.argv[2]))
    elif sys.argv[1:] == ["--child-default"]:
        child_default()
    elif len(sys.argv) in (2, 3) and sys.argv[1] == "--public-mode":
        public_mode(sys.argv[2] if len(sys.argv) == 3 else None)
    else:
        sys.exit(run())
