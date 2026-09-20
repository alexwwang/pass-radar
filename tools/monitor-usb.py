#!/usr/bin/env python3
"""Resilient ESP32-C3 USB Serial/JTAG monitor.

The monitor never toggles DTR/RTS and therefore does not intentionally reset the
board. It continuously discovers /dev/cu.usbmodem* (or an explicit --port),
reopens the device after USB re-enumeration, and writes raw device bytes plus
connection events to a log file.
"""

from __future__ import annotations

import argparse
import glob
import os
import select
import sys
import time
from datetime import datetime, timezone


def stamp() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


def candidates(explicit: str | None) -> list[str]:
    if explicit:
        return [explicit]
    return sorted(glob.glob("/dev/cu.usbmodem*"))


def write_event(out, message: str) -> None:
    line = f"\n[{stamp()}] {message}\n".encode()
    out.write(line)
    out.flush()
    sys.stdout.buffer.write(line)
    sys.stdout.buffer.flush()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="specific device path; default discovers /dev/cu.usbmodem*")
    parser.add_argument("--output", default="/tmp/pass-radar-usb-monitor.log")
    parser.add_argument("--duration", type=float, default=0.0, help="seconds; 0 means until Ctrl-C")
    parser.add_argument("--poll-ms", type=int, default=200)
    args = parser.parse_args()

    if args.poll_ms < 20:
        parser.error("--poll-ms must be at least 20")

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    start = time.monotonic()
    fd: int | None = None
    active: str | None = None

    with open(args.output, "ab", buffering=0) as out:
        write_event(out, f"monitor started output={args.output}")
        try:
            while args.duration <= 0 or time.monotonic() - start < args.duration:
                if fd is None:
                    for path in candidates(args.port):
                        try:
                            # Do not use a tty control library or toggle modem
                            # control lines: opening must not reset the board.
                            fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_NOCTTY)
                            active = path
                            write_event(out, f"connected port={path}")
                            break
                        except OSError:
                            continue
                    if fd is None:
                        time.sleep(args.poll_ms / 1000.0)
                        continue

                try:
                    readable, _, _ = select.select([fd], [], [], args.poll_ms / 1000.0)
                    if not readable:
                        continue
                    data = os.read(fd, 4096)
                    if data:
                        out.write(data)
                        out.flush()
                        sys.stdout.buffer.write(data)
                        sys.stdout.buffer.flush()
                    else:
                        raise OSError("device returned EOF")
                except (OSError, ValueError) as exc:
                    write_event(out, f"disconnected port={active} reason={exc}")
                    try:
                        os.close(fd)
                    except OSError:
                        pass
                    fd = None
                    active = None
        except KeyboardInterrupt:
            write_event(out, "monitor stopped by user")
        finally:
            if fd is not None:
                os.close(fd)
                write_event(out, f"closed port={active}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
