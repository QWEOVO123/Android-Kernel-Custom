#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Non-destructive /proc/cpu_ctl smoke test; performs no deliberate hot-unplug."""

import errno
import os
from pathlib import Path
import stat
import subprocess
import sys


NODE = Path("/proc/cpu_ctl")


def cpu_list(value):
    result = set()
    for part in value.split(","):
        if not part:
            continue
        bounds = [int(number) for number in part.split("-")]
        if len(bounds) == 1:
            result.add(bounds[0])
        elif len(bounds) == 2:
            result.update(range(bounds[0], bounds[1] + 1))
        else:
            raise ValueError("invalid CPU list")
    return result


def status():
    return dict(line.split(": ", 1) for line in NODE.read_text().splitlines())


def write(command):
    # O_TRUNC intentionally exercises shell/Android FileOutputStream semantics.
    with NODE.open("wb", buffering=0) as stream:
        written = stream.write(command)
        if written != len(command):
            raise AssertionError("short write")


def worker():
    if os.geteuid() == 0:
        print("1..0 # SKIP worker must be non-root")
        return 4
    number = 0

    def check(name, condition):
        nonlocal number
        number += 1
        print("%s %d - %s" % ("ok" if condition else "not ok", number, name), flush=True)
        if not condition:
            raise AssertionError(name)

    try:
        print("1..6", flush=True)
        initial = status()
        check("non-root can read CPU state",
              set(initial) == {"possible", "present", "online", "active", "hotpluggable"}
              and bool(cpu_list(initial["active"])))
        check("interface is world-readable/writable", stat.S_IMODE(NODE.stat().st_mode) == 0o666)
        first = min(cpu_list(initial["online"]))
        write(("online %d\n" % first).encode())
        check("non-root can write an idempotent online command", first in cpu_list(status()["online"]))
        current = status()["online"]
        write(("set %s\n" % current).encode())
        check("non-root can submit the current CPU mask", bool(cpu_list(status()["active"])))

        malformed_rejected = True
        for command in (b"unknown 0\n", b"set\n", b"set -1\n", b"online -1\n",
                        b"online 0\x00offline 0", b"online 0\noffline 0",
                        b"set 0\noffline 0", b"set 0 offline 0"):
            try:
                write(command)
                malformed_rejected = False
            except OSError as error:
                malformed_rejected &= error.errno in (errno.EINVAL, errno.ERANGE)
        check("malformed and embedded commands are rejected", malformed_rejected)

        errors_correct = True
        missing = max(cpu_list(initial["possible"])) + 1
        for command, expected in ((("online %d\n" % missing).encode(), errno.ENODEV),
                                  (b"online " + b" " * 4096 + b"0", errno.E2BIG)):
            try:
                write(command)
                errors_correct = False
            except OSError as error:
                errors_correct &= error.errno == expected
        check("unavailable CPU and oversized input are rejected", errors_correct)
        return 0
    except (AssertionError, OSError, ValueError) as error:
        print("# " + str(error), flush=True)
        return 1


def main():
    if not NODE.exists():
        print("1..0 # SKIP requires CONFIG_CPU_PUBLIC_CONTROL kernel")
        return 4
    if os.geteuid() != 0:
        return worker()

    def unprivileged():
        os.setgroups([])
        os.setgid(65534)
        os.setuid(65534)

    # Feed the script on stdin so its parent directory need not be accessible
    # to uid 65534. This checks Unix UID access, not an Android app SELinux SID.
    try:
        result = subprocess.run([sys.executable, "-", "--worker"],
                                input=Path(__file__).read_text(), text=True,
                                preexec_fn=unprivileged)
        return result.returncode
    except subprocess.SubprocessError as error:
        print("1..0 # SKIP cannot start unprivileged worker: " + str(error))
        return 4


if __name__ == "__main__":
    sys.exit(worker() if sys.argv[1:] == ["--worker"] else main())
