#!/usr/bin/env python3
"""Follow the kernel log and write every line to disk SYNCHRONOUSLY (fdatasync per line).

Why: when the machine freezes instantly, journald's recent entries are lost (they sit in memory-mapped
pages that were never flushed).  This logger flushes each line to the file before it reads the next one,
so after a freeze the file contains everything the kernel handed over up to the last moment.  It also
writes a "# alive ..." heartbeat line once a second (also fdatasync'ed): the last heartbeat in the file is
the last time this logger was running, which tells you when the freeze happened even if no kernel message
arrived in the last second.

Usage (run it in a SECOND terminal, start it BEFORE the risky test, stop it with Ctrl-C afterwards):

    python3 -I scripts/log-kernel-sync.py [OUTPUT_FILE]

OUTPUT_FILE must be inside the project directory (default:
scratch/isp4-threshold/results/kernel-sync-YYYYmmdd-HHMMSS.log).  Nothing is deleted or overwritten:
the file is opened in append mode.

How it reads the log: `sudo -n journalctl -k -f -n 0 -o short-precise --no-pager`.  This needs a
passwordless sudo rule for `journalctl -k *` set up by the user; -n makes sudo fail instead of asking for a
password.  Reading /dev/kmsg directly is not possible for a normal user
(kernel.dmesg_restrict = 1).  Limits: journald itself sits between the kernel and this program (normally a
few milliseconds), and anything the kernel never printed before the freeze cannot be recovered.
"""
import os
import select
import subprocess
import sys
import time

HEARTBEAT_S = 1.0


def main():
    project = os.path.realpath(os.path.join(os.path.dirname(os.path.realpath(__file__)), ".."))
    if len(sys.argv) > 2:
        sys.exit(__doc__)
    if len(sys.argv) == 2:
        out = os.path.realpath(sys.argv[1])
    else:
        out = os.path.join(project, "scratch", "isp4-threshold", "results",
                           time.strftime("kernel-sync-%Y%m%d-%H%M%S.log"))
    if os.path.commonpath([project, out]) != project:
        sys.exit(f"refusing to write outside the project directory: {out}")

    os.makedirs(os.path.dirname(out), exist_ok=True)
    fd = os.open(out, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o644)
    dfd = os.open(os.path.dirname(out), os.O_RDONLY)
    os.fsync(dfd)            # make the new directory entry durable too
    os.close(dfd)

    def put(text):
        os.write(fd, text.encode() + b"\n")
        os.fdatasync(fd)

    cmd = ["sudo", "-n", "journalctl", "-k", "-f", "-n", "0", "-o", "short-precise", "--no-pager"]
    child = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
    put(f"# started {time.strftime('%F %T')} pid {os.getpid()} reading: {' '.join(cmd)}")
    print(f"logging kernel messages to {out}\n(Ctrl-C to stop)", file=sys.stderr)

    buf = b""
    lines = 0
    last_beat = 0.0
    try:
        while True:
            ready, _, _ = select.select([child.stdout], [], [], HEARTBEAT_S)
            if ready:
                chunk = os.read(child.stdout.fileno(), 65536)
                if not chunk:
                    put(f"# journalctl ended (exit {child.poll()}) after {lines} lines")
                    print(f"journalctl ended (exit {child.poll()}); last output may explain: {buf!r}", file=sys.stderr)
                    return 1
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    put(line.decode(errors="replace"))
                    lines += 1
            now = time.monotonic()
            if now - last_beat >= HEARTBEAT_S:
                put(f"# alive {time.strftime('%F %T')} lines={lines}")
                last_beat = now
    except KeyboardInterrupt:
        put(f"# stopped by user {time.strftime('%F %T')} lines={lines}")
        return 0
    finally:
        child.terminate()
        os.close(fd)


if __name__ == "__main__":
    sys.exit(main())
