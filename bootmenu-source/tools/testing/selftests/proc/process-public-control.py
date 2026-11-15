#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Use a non-root controller to change only this test's disposable target."""

import errno
import os
from pathlib import Path
import subprocess
import sys
import threading
import time


INFO = Path("/proc/process_info")
CONTROL = Path("/proc/process_ctl")


def rows():
    lines = INFO.read_bytes().splitlines()
    names = lines[0].decode("ascii").split("\t")
    return [dict(zip(names, line.decode("utf-8", errors="replace").split("\t")))
            for line in lines[1:]]


def request(command):
    fd = os.open(CONTROL, os.O_RDWR | os.O_TRUNC)
    try:
        data = command.encode("ascii")
        assert os.write(fd, data) == len(data)
        os.lseek(fd, 0, os.SEEK_SET)
        result = os.read(fd, 128).decode("ascii")
        assert "error=0" in result
        return result
    finally:
        os.close(fd)


def controller(pid):
    assert os.geteuid() != 0
    number = 0

    def check(name, condition):
        nonlocal number
        number += 1
        print("%s %d - %s" % ("ok" if condition else "not ok", number, name), flush=True)
        assert condition, name

    print("1..6", flush=True)
    target = [r for r in rows() if int(r["tgid"]) == pid]
    leader = next(r for r in target if int(r["tid"]) == pid)
    check("non-root can enumerate target and worker threads", len(target) >= 3)
    cookie = leader["start_ns"]
    policy = leader["policy"]
    request("process %d %s -5\n" % (pid, cookie))
    changed = [r for r in rows() if int(r["tgid"]) == pid]
    check("whole process nice raised without changing policy",
          all(r["nice"] == "-5" and r["policy"] == policy for r in changed))

    worker = next(r for r in changed if int(r["tid"]) != pid)
    tid = int(worker["tid"])
    request("thread %d %s 5\n" % (tid, worker["start_ns"]))
    changed = [r for r in rows() if int(r["tgid"]) == pid]
    check("thread command changes only the selected TID",
          all(r["nice"] == ("5" if int(r["tid"]) == tid else "-5") for r in changed))

    stale = False
    try:
        request("thread %d %d 0\n" % (tid, int(worker["start_ns"]) + 1))
    except OSError as error:
        stale = error.errno == errno.ESTALE
    check("stale start-time token is rejected", stale)

    rejected = True
    for command in ("process %d %s -21\n" % (pid, cookie),
                    "process %d %s 20\n" % (pid, cookie),
                    "thread -1 1 0\n", "thread 0 1 0\n",
                    "thread 1 1 0\nthread 2 2 0", "thread 1 1 0\x00"):
        try:
            request(command)
            rejected = False
        except OSError as error:
            rejected &= error.errno == errno.EINVAL
    check("invalid nice, PID and multi-command inputs are rejected", rejected)
    request("process %d %s 0\n" % (pid, cookie))
    check("target nice restored to zero",
          all(r["nice"] == "0" for r in rows() if int(r["tgid"]) == pid))
    return 0


def target():
    for _ in range(2):
        threading.Thread(target=time.sleep, args=(120,), daemon=True).start()
    print("ready", flush=True)
    time.sleep(120)


def main():
    if not INFO.exists() or not CONTROL.exists():
        print("1..0 # SKIP requires CONFIG_PROCESS_PUBLIC_CONTROL kernel")
        return 4

    def uid(user):
        def drop():
            os.setgroups([])
            os.setgid(user)
            os.setuid(user)
        return drop

    source = Path(__file__).read_text()
    process = None
    try:
        root = os.geteuid() == 0
        process = subprocess.Popen([sys.executable, "-", "--target"],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True,
                                   preexec_fn=uid(65534) if root else None)
        process.stdin.write(source)
        process.stdin.close()
        assert process.stdout.readline().strip() == "ready"
        result = subprocess.run([sys.executable, "-", "--controller", str(process.pid)],
                                input=source, text=True, timeout=45,
                                preexec_fn=uid(65533) if root else None)
        return result.returncode
    except (AssertionError, OSError, subprocess.SubprocessError) as error:
        print("# " + str(error))
        return 1
    finally:
        if process is not None:
            process.terminate()
            process.wait(timeout=5)


if __name__ == "__main__":
    try:
        if sys.argv[1:] == ["--target"]:
            target()
        elif len(sys.argv) == 3 and sys.argv[1] == "--controller":
            sys.exit(controller(int(sys.argv[2])))
        else:
            sys.exit(main())
    except (AssertionError, OSError, ValueError) as error:
        print("# " + str(error), flush=True)
        sys.exit(1)
