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


def test_regex_replace_all_newline_finishes(home):
    """Regex \\n replacement must split rows and terminate after one pass."""
    target = pathlib.Path(home) / "replace.txt"
    target.write_bytes(b"tiny\\nedit\\nend\n")
    process, master = spawn_editor([str(target)], home)
    read_available(master)

    os.write(master, b"\x06")       # Ctrl-F
    read_until(master, b"Search")
    os.write(master, b"\x07")       # Ctrl-G: regex mode
    os.write(master, b"\\\\n")     # regex matching a literal backslash+n
    os.write(master, b"\x12")       # Ctrl-R
    read_until(master, b"Replace")
    os.write(master, b"\\n\r")      # replacement escape: actual newline
    read_until(master, b"Replace this occurrence?")
    os.write(master, b"a")           # replace all

    output = read_until(master, b"Replaced 2 occurrence(s).")
    os.write(master, b"\x13")        # Ctrl-S
    read_until(master, b"bytes written to disk")
    finish(process, master)
    if b"Replaced 2 occurrence(s)." not in output:
        raise AssertionError("regex newline replace-all did not finish")
    if target.read_bytes() != b"tiny\nedit\nend\n":
        raise AssertionError("regex replacement \\n was not decoded as a line break")


def test_ctrl_w_saves_and_closes_only_file(home):
    target = pathlib.Path(home) / "close-current.txt"
    target.write_bytes(b"old\n")
    process, master = spawn_editor([str(target)], home)
    read_available(master)

    os.write(master, b"X\x17")      # edit, then Ctrl-W
    read_until(master, b"Save changes before closing?")
    os.write(master, b"y")
    output = read_until(master, b"File closed.")
    if b"File closed." not in output or process.poll() is not None:
        finish(process, master)
        raise AssertionError("Ctrl-W exited tinyedit instead of closing the file")

    finish(process, master)
    if target.read_bytes() != b"Xold\n":
        raise AssertionError("Ctrl-W did not save the modified current file")


def test_ctrl_o_discards_then_creates_named_file(home):
    current = pathlib.Path(home) / "current.txt"
    target = pathlib.Path(home) / "created-by-open.txt"
    existing = pathlib.Path(home) / "existing.txt"
    current.write_bytes(b"keep\n")
    existing.write_bytes(b"loaded existing file\n")
    process, master = spawn_editor([str(current)], home)
    read_available(master)

    os.write(master, b"X\x0f")      # edit, then Ctrl-O
    read_until(master, b"Save changes before opening another file?")
    os.write(master, b"n")          # deliberately discard current edit
    read_until(master, b"Open file:")
    os.write(master, os.fsencode(target) + b"\r")
    marker = os.fsencode(target.name)
    output = read_until(master, marker)
    if marker not in output:
        finish(process, master)
        raise AssertionError(
            f"Ctrl-O did not open a missing path as a new file; output tail={output[-500:]!r}"
        )

    os.write(master, b"created\x13")
    read_until(master, b"bytes written to disk")

    os.write(master, b"\x0f")       # Ctrl-O again from a clean document
    read_until(master, b"Open file:")
    os.write(master, os.fsencode(existing) + b"\r")
    output = read_until(master, b"loaded existing file")
    if b"loaded existing file" not in output:
        finish(process, master)
        raise AssertionError("Ctrl-O did not load an existing file")

    finish(process, master)
    if current.read_bytes() != b"keep\n":
        raise AssertionError("Ctrl-O saved changes after the user chose discard")
    if target.read_bytes() != b"created\n":
        raise AssertionError("Ctrl-O did not create the named file on save")


def main():
    with tempfile.TemporaryDirectory(prefix="tinyedit-tests-") as tmp:
        home = pathlib.Path(tmp)
        test_f3(b"\x1bOR", "SS3", home)
        test_f3(b"\x1b[13~", "CSI", home)
        test_ghostty_ctrl_i_is_drained(home)
        test_very_long_wrapped_line(home)
        test_regex_replace_all_newline_finishes(home)
        test_ctrl_w_saves_and_closes_only_file(home)
        test_ctrl_o_discards_then_creates_named_file(home)
    print("pty tests: ok")


if __name__ == "__main__":
    main()
