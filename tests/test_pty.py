#!/usr/bin/env python3
"""Small PTY smoke tests for terminal key decoding and UTF-8 prompts."""

import fcntl
import os
import pathlib
import pty
import select
import struct
import subprocess
import tempfile
import termios
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
BINARY = ROOT / "tinyedit"


def spawn_editor(arguments, home):
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 0, 0))
    env = os.environ.copy()
    env.update({"HOME": str(home), "TERM": "xterm-256color"})
    process = subprocess.Popen(
        [str(BINARY), *arguments], stdin=slave, stdout=slave, stderr=slave,
        cwd=ROOT, env=env, close_fds=True,
    )
    os.close(slave)
    return process, master


def read_available(master, timeout=0.4):
    chunks = []
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        ready, _, _ = select.select([master], [], [], 0.05)
        if not ready:
            continue
        try:
            chunks.append(os.read(master, 65536))
        except OSError:
            break
    return b"".join(chunks)


def read_until(master, needle, timeout=2.0):
    """Collect PTY output until a marker arrives or the deadline expires."""
    chunks = []
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        ready, _, _ = select.select([master], [], [], 0.05)
        if not ready:
            continue
        try:
            chunks.append(os.read(master, 65536))
        except OSError:
            break
        output = b"".join(chunks)
        if needle in output:
            return output
    return b"".join(chunks)


def finish(process, master, expect_exit=False):
    try:
        if expect_exit:
            process.wait(timeout=2)
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait()
        os.close(master)


def test_f3(sequence, label, home):
    process, master = spawn_editor([], home)
    read_available(master)
    os.write(master, sequence)
    output = read_until(master, b"tinyedit -- info")
    if b"tinyedit -- info" not in output:
        raise AssertionError(f"{label}: F3 did not open the Info screen")
    os.write(master, b"x")  # close overlay
    read_available(master, 0.2)
    finish(process, master)


def test_ghostty_ctrl_i_is_drained(home):
    target = pathlib.Path(home) / "ctrl-i.txt"
    target.write_bytes(b"")
    process, master = spawn_editor([str(target)], home)
    read_available(master)
    os.write(master, b"\x1b[5;5u")
    read_available(master, 0.2)
    os.write(master, b"x\x13")
    read_available(master, 0.2)
    finish(process, master)
    if target.read_bytes() != b"x\n":
        raise AssertionError("obsolete Ghostty Ctrl-I sequence leaked bytes or swallowed the next key")


def test_very_long_wrapped_line(home):
    target = pathlib.Path(home) / "long-line.txt"
    target.write_bytes((b"word " * 12000) + b"TAIL_SENTINEL\n")
    process, master = spawn_editor([str(target)], home)
    output = read_available(master)
    found = b"TAIL_SENTINEL" in output
    for _ in range(30):
        if found:
            break
        os.write(master, b"\x1b[6~")  # PageDown
        output = read_available(master, 0.08)
        found = b"TAIL_SENTINEL" in output
    finish(process, master)
    if not found:
        raise AssertionError("content beyond the former 512-wrap-segment limit is unreachable")


def main():
    with tempfile.TemporaryDirectory(prefix="tinyedit-tests-") as tmp:
        home = pathlib.Path(tmp)
        test_f3(b"\x1bOR", "SS3", home)
        test_f3(b"\x1b[13~", "CSI", home)
        test_ghostty_ctrl_i_is_drained(home)
        test_very_long_wrapped_line(home)
    print("pty tests: ok")


if __name__ == "__main__":
    main()
