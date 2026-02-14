#!/usr/bin/env python3

import argparse
import errno
import os
import sys
import time


DEFAULT_PIPE = "/tmp/fallout-cli-in"
DEFAULT_OUTPUT = "/tmp/fallout-cli-out.txt"


def send_command(pipe_path: str, command: str, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    payload = (command + "\n").encode("utf-8")

    while True:
        try:
            fd = os.open(pipe_path, os.O_WRONLY | os.O_NONBLOCK)
            break
        except OSError as exc:
            if exc.errno not in (errno.ENOENT, errno.ENXIO):
                raise
            if time.monotonic() >= deadline:
                raise TimeoutError(f"timed out waiting for pipe: {pipe_path}") from exc
            time.sleep(0.05)

    with os.fdopen(fd, "wb", closefd=True) as pipe:
        pipe.write(payload)
        pipe.flush()


def read_text(path: str) -> str:
    with open(path, "r", encoding="utf-8", errors="replace") as stream:
        return stream.read()


def wait_for_response(output_path: str, previous: str, command: str, timeout: float) -> str:
    deadline = time.monotonic() + timeout
    while True:
        try:
            text = read_text(output_path)
            if text != previous and f"command={command}" in text:
                return text
        except FileNotFoundError:
            pass

        if time.monotonic() >= deadline:
            break

        time.sleep(0.05)

    raise TimeoutError(f"timed out waiting for response file update: {output_path}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Send one command to Fallout CE CLI.")
    parser.add_argument("command", nargs="+", help="Command to send, for example: state")
    parser.add_argument("--pipe", default=DEFAULT_PIPE, help=f"Input pipe path (default: {DEFAULT_PIPE})")
    parser.add_argument("--out", default=DEFAULT_OUTPUT, help=f"Output file path (default: {DEFAULT_OUTPUT})")
    parser.add_argument("--timeout", type=float, default=2.0, help="Timeout in seconds (default: 2.0)")
    args = parser.parse_args()

    command = " ".join(args.command).strip()
    if command == "":
        print("error: empty command", file=sys.stderr)
        return 2

    try:
        previous = read_text(args.out)
    except FileNotFoundError:
        previous = ""

    try:
        send_command(args.pipe, command, args.timeout)
        response = wait_for_response(args.out, previous, command, args.timeout)
    except TimeoutError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    except OSError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    sys.stdout.write(response)
    if not response.endswith("\n"):
        sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
