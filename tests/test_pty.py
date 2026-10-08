#!/usr/bin/env python3
"""Small PTY smoke tests for terminal key decoding and UTF-8 prompts."""

import fcntl
import os
import pathlib
import pty
import re
import select
import struct
import subprocess
import tempfile
import termios
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
BINARY = ROOT / "bin" / "tinyedit"


def spawn_editor(arguments, home, extra_env=None, cwd=None):
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 0, 0))
    env = os.environ.copy()
    env.update({"HOME": str(home), "TERM": "xterm-256color"})
    if extra_env:
        env.update(extra_env)
    process = subprocess.Popen(
        [str(BINARY), *arguments], stdin=slave, stdout=slave, stderr=slave,
        cwd=cwd or ROOT, env=env, close_fds=True,
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


def test_path_completion(home):
    folder = pathlib.Path(home) / "completion é space"
    folder.mkdir()
    existing = folder / "sample.txt"
    existing.write_text("completion content\n")
    for key in (b"\x0f", b"\x13", b"\x1bOS"):
        process, master = spawn_editor([], home)
        try:
            read_available(master)
            os.write(master, key)
            read_available(master, 0.2)
            os.write(master, (str(folder) + "/sam").encode() + b"\t")
            output = read_until(master, b"sample.txt")
            assert b"sample.txt" in output, "completed path visible in file prompt"
            os.write(master, b"\r")
            if key == b"\x0f":
                assert b"completion content" in read_until(master, b"completion content")
            else:
                read_available(master, 0.2)
                assert existing.read_bytes() == b"", "save accepts completed path"
                existing.write_text("completion content\n")
        finally:
            finish(process, master)


def test_sidebar(home):
    root = pathlib.Path(home) / "tree-fixture"
    root.mkdir()
    branch = root / "branch"
    branch.mkdir()
    leaf = branch / "document é.txt"
    leaf.write_text("tree leaf content\n")
    original = pathlib.Path(home) / "tree-original.txt"
    original.write_text("original content\n")
    process, master = spawn_editor([str(original)], home, cwd=root)
    try:
        read_available(master)
        os.write(master, b"\x05")
        assert b"Files (^E close)" in read_until(master, b"Files (^E close)"), "Ctrl-E opens sidebar"
        os.write(master, b"\x02X\x13")
        assert b"Files (^E close)" in read_until(master, b"bytes written"), "sidebar persists while editing"
        assert original.read_bytes() == b"Xoriginal content\n", "editor retains keyboard focus"
        os.write(master, b"Y\x02\x1b[B\r\x1b[B\r")
        assert b"Save changes before opening another file?" in read_until(master, b"Save changes before opening another file?"), "tree opening protects unsaved edits"
        os.write(master, b"\x1b")
        assert b"XYoriginal" in read_until(master, b"XYoriginal"), "cancel leaves current document intact"
        os.write(master, b"\r")
        read_until(master, b"Save changes before opening another file?")
        os.write(master, b"n")
        output = read_until(master, b"tree leaf content")
        assert b"tree leaf content" in output and b"Files (^E close)" in output, "tree opens file and stays visible"
        os.write(master, b"\x05")
        output = read_available(master)
        assert process.poll() is None, "closing sidebar does not exit editor"
    finally:
        finish(process, master)


def test_binary_open_rejected(home):
    root = pathlib.Path(home) / "binary-open-fixture"
    root.mkdir()
    image = root / "image.png"
    image_bytes = b"\x89PNG\r\n\x1a\n\x00\x00\x00\x0dIHDR"
    image.write_bytes(image_bytes)
    original = pathlib.Path(home) / "binary-open-original.txt"
    original.write_text("keep original document\n")
    for sidebar in (False, True):
        process, master = spawn_editor([str(original)], home, cwd=root)
        try:
            read_available(master)
            if sidebar:
                os.write(master, b"\x05\x1b[B\r")
            else:
                os.write(master, b"\x0f")
                read_until(master, b"Open file:")
                os.write(master, os.fsencode(image) + b"\r")
            output = read_until(master, b"binary files are not supported")
            assert b"binary files are not supported" in output, "PNG rejected with readable error"
            assert b"keep original document" in output, "binary rejection preserves active document"
            assert image.read_bytes() == image_bytes, "binary file remains untouched"
        finally:
            finish(process, master)


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


def test_kitty_f1_f2(home):
    """Ghostty emits short CSI F1/F2 forms while Kitty protocol is active."""
    process, master = spawn_editor([], home)
    try:
        read_available(master)
        os.write(master, b"\x1b[P")
        assert b"tinyedit -- keybindings" in read_until(master, b"tinyedit -- keybindings"), (
            "Kitty CSI F1 did not open Help"
        )
        os.write(master, b"x\x1b[Q")
        assert b"Settings" in read_until(master, b"Settings"), (
            "Kitty CSI F2 did not open Settings"
        )
    finally:
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


def test_kitty_keyboard_mode_is_restored(home):
    """The opt-in Ghostty mode must not remain enabled for the shell."""
    case_home = pathlib.Path(home) / "kitty-keyboard-restore"
    case_home.mkdir()
    target = case_home / "empty.txt"
    target.write_bytes(b"")
    (case_home / ".tinyeditrc").write_text("mac_command_keys = true\n", encoding="utf-8")
    process, master = spawn_editor([str(target)], case_home)
    try:
        output = read_available(master)
        if b"\x1b[>1u" not in output:
            raise AssertionError("Kitty keyboard mode was not enabled")
        os.write(master, b"\x1b[113;9u")  # Cmd-Q: clean buffer exits immediately
        output += read_available(master)
        process.wait(timeout=2)
        output += read_available(master)
        if b"\x1b[<u" not in output:
            raise AssertionError("Kitty keyboard mode was not restored on exit")
        assert output.index(b"\x1b[<u") < output.index(b"\x1b[?1049l"), (
            "Kitty keyboard mode was restored after leaving alternate screen"
        )
    finally:
        finish(process, master)


def test_alternate_screen_lifecycle(home):
    case_home = pathlib.Path(home) / "alternate-screen"
    case_home.mkdir()
    (case_home / ".tinyeditrc").write_text(
        "mouse_enabled = true\n", encoding="utf-8"
    )
    target = case_home / "empty.txt"
    target.write_bytes(b"")
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 0, 0))
    original = termios.tcgetattr(slave)
    env = os.environ.copy()
    env.update({"HOME": str(case_home), "TERM": "xterm-256color"})
    process = subprocess.Popen(
        [str(BINARY), str(target)], stdin=slave, stdout=slave, stderr=slave,
        cwd=ROOT, env=env, close_fds=True,
    )
    try:
        output = read_available(master)
        assert output.count(b"\x1b[?1049h") == 1, "alternate screen was not entered once"
        assert output.index(b"\x1b[?1049h") < output.index(b"\x1b[?2004h")
        assert b"\x1b[?1049l" not in output
        os.write(master, b"\x11")  # Ctrl-Q
        cleanup = read_until(master, b"\x1b[?1049l")
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired as exc:
            raise AssertionError(f"quit did not exit: {read_available(master)!r}") from exc
        cleanup += read_available(master)
        output += cleanup
        assert process.returncode == 0
        assert output.count(b"\x1b[?1049l") == 1, "alternate screen was not left once"
        assert b"\x1b[?1002l" in output and b"\x1b[?1006l" in output
        assert b"\x1b[?2004l" in output
        assert output.index(b"\x1b[?1002l") < output.index(b"\x1b[?2004l")
        assert output.index(b"\x1b[?2004l") < output.index(b"\x1b[?1049l")
        assert b"\x1b[2J" not in cleanup, "main screen was cleared on exit"
        assert termios.tcgetattr(slave) == original, "termios was not restored"
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait()
        os.close(master)
        os.close(slave)


def test_kitty_cmd_z_undoes(home):
    case_home = pathlib.Path(home) / "kitty-cmd-z"
    case_home.mkdir()
    target = case_home / "undo.txt"
    target.write_bytes(b"original\n")
    (case_home / ".tinyeditrc").write_text(
        "mac_command_keys = true\nbackup_interval = 0\n", encoding="utf-8"
    )
    process, master = spawn_editor([str(target)], case_home)
    try:
        read_available(master)
        os.write(master, b"X\x1b[122;9u\x13")  # X, Cmd-Z, Ctrl-S
        assert b"bytes written to disk" in read_until(master, b"bytes written to disk"), (
            "Cmd-Z did not save after undo"
        )
    finally:
        finish(process, master)
    if target.read_bytes() != b"original\n":
        raise AssertionError("Kitty Cmd-Z did not undo the edit")


def test_kitty_cmd_a_selects_all(home):
    case_home = pathlib.Path(home) / "kitty-cmd-a"
    case_home.mkdir()
    target = case_home / "select-all.txt"
    target.write_bytes(b"first\nsecond\n")
    (case_home / ".tinyeditrc").write_text(
        "mac_command_keys = true\nbackup_interval = 0\n", encoding="utf-8"
    )
    process, master = spawn_editor([str(target)], case_home)
    try:
        read_available(master)
        os.write(master, b"\x1b[97;9u\x7f\x13")  # Cmd-A, Backspace, Ctrl-S
        assert b"bytes written to disk" in read_until(master, b"bytes written to disk"), (
            "Cmd-A did not save after deleting the selection"
        )
    finally:
        finish(process, master)
    if target.read_bytes() != b"\n":
        raise AssertionError("Kitty Cmd-A did not select the whole buffer")


def test_shift_click_extends_selection(home):
    case_home = pathlib.Path(home) / "mouse-shift-click"
    case_home.mkdir()
    target = case_home / "selection.txt"
    target.write_bytes(b"abcdef\n")
    (case_home / ".tinyeditrc").write_text(
        "mouse_enabled = true\nshow_line_numbers = 0\nshow_top_bar = 0\n"
        "backup_interval = 0\n",
        encoding="utf-8",
    )
    process, master = spawn_editor([str(target)], case_home)
    try:
        output = read_available(master)
        if b"\x1b[>1s" not in output:
            raise AssertionError("mouse mode did not request Shift mouse reporting")
        # Put the cursor after 'a', then Shift-click after 'd'. SGR mouse
        # reports use modifier bit 4 for Shift with the left button.
        os.write(master, b"\x1b[<0;2;2M\x1b[<0;2;2m")
        os.write(master, b"\x1b[<4;5;2M\x1b[<4;5;2m")
        read_available(master, 0.2)
        os.write(master, b"\x7f\x13")  # Delete selected bcd, then save.
        assert b"bytes written to disk" in read_until(master, b"bytes written to disk"), (
            "Shift-click selection was not saved"
        )
    finally:
        finish(process, master)
    actual = target.read_bytes()
    if actual != b"aef\n":
        raise AssertionError(f"Shift-click did not extend the selection: {actual!r}")


def test_menu_bar_precedes_first_file_row(home):
    """The permanent menu bar must occupy a real terminal row."""
    for show_top_bar in (0, 1):
        case_home = pathlib.Path(home) / f"menu-layout-{show_top_bar}"
        case_home.mkdir()
        target = case_home / "layout.txt"
        target.write_bytes(b"FIRST\nSECOND\n")
        (case_home / ".tinyeditrc").write_text(
            f"show_top_bar = {show_top_bar}\nshow_menu = true\n"
            "show_line_numbers = false\nbackup_interval = 0\n",
            encoding="utf-8",
        )
        process, master = spawn_editor([str(target)], case_home)
        try:
            output = read_available(master)
            if not (0 <= output.find(b"TinyEdit") < output.find(b"FIRST") <
                    output.find(b"SECOND")):
                raise AssertionError("menu bar did not precede the first file row")
            positions = re.findall(rb"\x1b\[(\d+);(\d+)H", output)
            expected_row = 3 if show_top_bar else 2
            if not positions or int(positions[-1][0]) != expected_row:
                raise AssertionError("cursor and first file row use different offsets")
        finally:
            finish(process, master)


def test_menu_restores_editor_background(home):
    """The menu bar must not leak the terminal background into file rows."""
    for show_line_numbers in (0, 1):
        case_home = pathlib.Path(home) / f"menu-background-{show_line_numbers}"
        case_home.mkdir()
        target = case_home / "background.c"
        target.write_bytes(b"\n/* text */\n")
        (case_home / ".tinyeditrc").write_text(
            f"show_line_numbers = {show_line_numbers}\n"
            "show_menu = true\nshow_top_bar = false\n"
            "color_background = gray-dark\ncolor_statusbar = blue-dark\n"
            "backup_interval = 0\n", encoding="utf-8"
        )
        process, master = spawn_editor([str(target)], case_home)
        try:
            output = read_until(master, b"TinyEdit")
            output += read_available(master, 0.2)
            assert b"TinyEdit" in output, "menu bar was not drawn"
            assert b"\x1b[K\x1b[m\r\n\x1b[40m" in output, (
                "menu bar did not restore the editor background before the first row"
            )
        finally:
            finish(process, master)


def test_menu_mouse_navigation(home):
    """Mouse reports must reach the menu while it is open."""
    case_home = pathlib.Path(home) / "menu-mouse"
    case_home.mkdir()
    target = case_home / "menu.txt"
    target.write_bytes(b"sample\n")
    (case_home / ".tinyeditrc").write_text(
        "mouse_enabled = true\nshow_top_bar = false\nshow_menu = true\n"
        "backup_interval = 0\n", encoding="utf-8"
    )
    process, master = spawn_editor([str(target)], case_home)
    try:
        read_available(master)

        def click(col, row):
            os.write(master, f"\x1b[<0;{col};{row}M\x1b[<0;{col};{row}m".encode())
            return read_available(master, 0.2)

        def hover(col, row):
            os.write(master, f"\x1b[<35;{col};{row}M".encode())
            return read_available(master, 0.2)

        opened = click(2, 1)
        assert b"Settings" in opened, "mouse did not open TinyEdit menu"
        assert b"\x1b[?1003h" in opened, "menu did not enable mouse motion"
        assert b"\x1b[7mSettings" in hover(4, 4), "hover did not select Settings"
        assert b"\x1b[7mInfo" in hover(4, 3), "hover did not select Info"
        assert b"Open..." in hover(12, 1), "hover did not switch to File menu"
        assert b"Settings" in click(2, 1), "mouse did not switch back to TinyEdit"
        selected = click(4, 3)
        assert b"tinyedit -- info" in selected, "mouse did not select Info"
        assert b"\x1b[?1003l\x1b[?1002h" in selected, (
            "selecting a command did not restore mouse click reporting"
        )
        os.write(master, b"x")  # close Info
        read_available(master, 0.2)
        click(2, 1)
        closed = click(40, 10)
        assert b"\x1b[?25h" in closed, "outside click did not close menu"
        assert b"\x1b[?1003l\x1b[?1002h" in closed, (
            "outside click did not restore mouse click reporting"
        )
        assert b"\x1b[?1003h" in click(95, 30), (
            "F10 Menu click was closed by its own mouse release"
        )
    finally:
        finish(process, master)


def test_menu_show_invisibles_refreshes_rows(home):
    case_home = pathlib.Path(home) / "menu-invisibles"
    case_home.mkdir()
    target = case_home / "spaces.txt"
    target.write_bytes(b"a b\n")
    (case_home / ".tinyeditrc").write_text(
        "show_menu = true\nshow_invisibles = false\n"
        "backup_interval = 0\n", encoding="utf-8"
    )
    process, master = spawn_editor([str(target)], case_home)
    try:
        read_available(master)
        os.write(master, b"\x1b[21~" + b"\x1b[C" * 3 + b"\x1b[B" * 3 + b"\r")
        output = read_available(master)
        visible_text = re.sub(rb"\x1b\[[0-9;?]*[ -/]*[@-~]", b"", output)
        assert b"a.b" in visible_text, "View toggle did not rebuild rendered spaces"
    finally:
        finish(process, master)


def test_menu_view_toggles_settings(home):
    case_home = pathlib.Path(home) / "menu-view-settings"
    case_home.mkdir()
    target = case_home / "view.c"
    target.write_bytes(b"int value;\n")
    config = case_home / ".tinyeditrc"
    config.write_text("show_menu = true\nbackup_interval = 0\n", encoding="utf-8")
    process, master = spawn_editor([str(target)], case_home)
    try:
        read_available(master)
        os.write(master, b"\x1b[21~" + b"\x1b[C" * 3)
        opened = read_until(master, b"Auto-indent new lines")
        assert b"[x] Syntax highlighting" in opened
        assert b"[x] Auto-indent new lines" in opened

        os.write(master, b"\x1b[B" * 4 + b"\r")
        read_available(master)
        assert "syntax_highlight = false" in config.read_text(encoding="utf-8")

        os.write(master, b"\x1b[21~" + b"\x1b[C" * 3)
        reopened = read_until(master, b"Auto-indent new lines")
        assert b"[ ] Syntax highlighting" in reopened
        os.write(master, b"\x1b[B" * 5 + b"\r")
        read_available(master)
        assert "auto_indent = false" in config.read_text(encoding="utf-8")
    finally:
        finish(process, master)


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


def test_utf8_word_jumps(home):
    """Word jumps across multibyte text must leave editable UTF-8 boundaries."""
    cases = (
        ("forward", "è café foo", b"\x1b[H\x1bfX", "èX café foo"),
        ("backward", "è café foo", b"\x1b[F\x1bbX", "è café Xfoo"),
    )
    for name, initial, keys, expected in cases:
        target = pathlib.Path(home) / f"word-jump-{name}.txt"
        target.write_text(initial, encoding="utf-8")
        process, master = spawn_editor([str(target)], home)
        try:
            read_available(master)
            os.write(master, keys + b"\x13")
            output = read_until(master, b"bytes written to disk")
            assert b"bytes written to disk" in output, f"{name}: save did not finish"
        finally:
            finish(process, master)
        actual = target.read_text(encoding="utf-8")
        assert actual == expected, f"{name}: {actual!r} != {expected!r}"


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


def test_invisible_colors(home):
    """Literal punctuation must not inherit the space/tab marker color."""
    case_home = pathlib.Path(home) / "invisible-colors"
    case_home.mkdir()
    target = case_home / "markers.c"
    # Tabs and UTF-8 make source byte offsets differ from render offsets;
    # the short wrap width also exercises segments starting mid-row.
    target.write_text('"é\t.>$ .>$\t.>$ .>$ .>$"\n// .>$\n', encoding="utf-8")
    for enabled, syntax in ((1, 1), (1, 0), (0, 1)):
        (case_home / ".tinyeditrc").write_text(
            f"show_invisibles = {enabled}\nsyntax_highlight = {syntax}\n"
            "color_invisibles = red-light\ncolor_syntax_string = green-light\n"
            "color_syntax_comment = blue-light\nsoft_wrap = 12\ntab_stop = 4\n",
            encoding="utf-8",
        )
        process, master = spawn_editor([str(target)], case_home)
        try:
            output = read_available(master)
            invisible = b"\x1b[91m"
            reset = b"\x1b[m"
            # Only three real spaces and two tabs in the string, plus
            # one comment space, should receive the invisible color.
            expected_dots, expected_tabs = (4, 2) if enabled else (0, 0)
            assert output.count(invisible + b"." + reset) == expected_dots, (
                "literal dots received invisible color, or space markers lost it"
            )
            assert output.count(invisible + b">" + reset) == expected_tabs, (
                "literal greater-than signs received invisible color, or tab markers lost it"
            )
            assert output.count(invisible + b"$" + reset) == (2 if enabled else 0), (
                "literal dollars received invisible color, or newline markers lost it"
            )
            if syntax:
                for glyph in (b".", b">", b"$"):
                    assert output.count(b"\x1b[92m" + glyph + reset) == 5, (
                        f"string punctuation {glyph!r} lost its syntax color"
                    )
                    assert output.count(b"\x1b[94m" + glyph + reset) == 1, (
                        f"comment punctuation {glyph!r} lost its syntax color"
                    )
        finally:
            finish(process, master)


def test_selection_across_tab(home):
    """Selecting a source tab must highlight its complete rendered width."""
    case_home = pathlib.Path(home) / "selection-tabs"
    case_home.mkdir()
    target = case_home / "selection.txt"
    target.write_bytes(b"A\tBC\n")
    for visible in (0, 1):
        (case_home / ".tinyeditrc").write_text(
            f"show_invisibles = {visible}\nshow_line_numbers = 0\n"
            "show_top_bar = 0\ninsert_spaces_for_tab = 0\ntab_stop = 4\n"
            "syntax_highlight = 0\ncolor_selection = yellow-light\n",
            encoding="utf-8",
        )
        process, master = spawn_editor([str(target)], case_home)
        try:
            read_available(master)
            os.write(master, b"\x14\x1b[C\x1b[C")  # Ctrl-T, select A and tab
            output = read_available(master)
            tab = b">  " if visible else b"   "
            selected = b"\x1b[93m\x1b[7mA" + tab + b"\x1b[mBC"
            assert selected in output, "selection did not cover the complete rendered tab"
        finally:
            finish(process, master)


def test_matching_bracket_highlight(home):
    """The delimiter under the cursor highlights its textual match."""
    case_home = pathlib.Path(home) / "matching-brackets"
    case_home.mkdir()
    target = case_home / "brackets.txt"
    target.write_bytes(b"([x])\n")
    (case_home / ".tinyeditrc").write_text(
        "show_line_numbers = 0\nshow_top_bar = 0\nsyntax_highlight = 0\n"
        "color_selection = yellow-light\n",
        encoding="utf-8",
    )
    process, master = spawn_editor([str(target)], case_home)
    try:
        output = read_available(master)
        for bracket in (b"(", b")"):
            assert b"\x1b[7m" + bracket + b"\x1b[m" in output, (
                "outer nested brackets were not highlighted"
            )
        os.write(master, b"\x1b[C")
        output = read_available(master)
        for bracket in (b"[", b"]"):
            assert b"\x1b[7m" + bracket + b"\x1b[m" in output, (
                "inner nested brackets were not highlighted"
            )
        os.write(master, b"\x1b[F")  # Cursor immediately after the outer closer.
        output = read_available(master)
        for bracket in (b"(", b")"):
            assert b"\x1b[7m" + bracket + b"\x1b[m" in output, (
                "outer brackets were not highlighted from the closer"
            )
    finally:
        finish(process, master)

    target.write_bytes(b"(\n)\n")
    process, master = spawn_editor([str(target)], case_home)
    try:
        output = read_available(master)
        for bracket in (b"(", b")"):
            assert b"\x1b[7m" + bracket + b"\x1b[m" in output, (
                "brackets on separate lines were not highlighted"
            )
    finally:
        finish(process, master)


def test_selection_marks_blank_rows(home):
    """A selected logical newline must remain visible on an empty row."""
    case_home = pathlib.Path(home) / "selection-blank-rows"
    case_home.mkdir()
    target = case_home / "selection.txt"
    target.write_bytes(b"\n\nVISTA\n")
    (case_home / ".tinyeditrc").write_text(
        "show_line_numbers = 0\nshow_top_bar = 0\nshow_invisibles = 0\n"
        "syntax_highlight = 0\ncolor_selection = yellow-light\n",
        encoding="utf-8",
    )
    process, master = spawn_editor([str(target)], case_home)
    try:
        read_available(master)
        os.write(master, b"\x01")  # Ctrl-A
        output = read_available(master)
        selected_cell = b"\x1b[93m\x1b[7m \x1b[m"
        if output.count(selected_cell) < 2:
            raise AssertionError("selected blank rows have no visible terminator cell")
    finally:
        finish(process, master)


def test_regex_finds_logical_newline(home):
    """Regex \\n and \\r both address the normalized row boundary."""
    case_home = pathlib.Path(home) / "regex-newline"
    case_home.mkdir()
    target = case_home / "search.txt"
    target.write_bytes(b"first\r\nsecond\r\n")
    (case_home / ".tinyeditrc").write_text(
        "show_line_numbers = 0\nshow_top_bar = 0\nshow_invisibles = 1\n"
        "syntax_highlight = 0\ncolor_selection = yellow-light\n",
        encoding="utf-8",
    )
    process, master = spawn_editor([str(target)], case_home)
    try:
        read_available(master)
        for query in (b"\\n", b"\\r"):
            os.write(master, b"\x06\x07" + query)  # Ctrl-F, Ctrl-G, regex
            output = read_available(master)
            if b"\x1b[93m\x1b[7m$\x1b[m" not in output:
                raise AssertionError(f"regex {query!r} did not highlight a row boundary")
            os.write(master, b"\x1b")
            read_available(master)
    finally:
        finish(process, master)


def test_bracketed_paste_replaces_selection_atomically(home):
    """Terminal-native paste replaces a selection and is one undo step."""
    case_home = pathlib.Path(home) / "paste-selection"
    case_home.mkdir()
    target = case_home / "paste.txt"
    (case_home / ".tinyeditrc").write_text("backup_interval = 0\n", encoding="utf-8")
    target.write_text("hello world\nsecond line\n", encoding="utf-8")
    process, master = spawn_editor([str(target)], case_home)
    try:
        read_available(master)
        # Select "hello" with TinyEdit's terminal-independent selection mode.
        os.write(master, b"\x14" + b"\x1b[C" * 5)
        read_available(master, 0.2)
        os.write(master, b"\x1b[200~PASTED\nTEXT\x1b[201~")
        assert b"pasted" in read_until(master, b"pasted")
        os.write(master, b"\x13")
        read_until(master, b"bytes written to disk")
        assert target.read_text(encoding="utf-8") == "PASTED\nTEXT world\nsecond line\n"

        # One undo restores both the removed selection and the inserted block.
        os.write(master, b"\x1a\x13")
        read_until(master, b"bytes written to disk")
        assert target.read_text(encoding="utf-8") == "hello world\nsecond line\n"
    finally:
        finish(process, master)


def test_typed_text_replaces_selection(home):
    """Ordinary typing must consume selected text just like paste does."""
    case_home = pathlib.Path(home) / "typed-selection"
    case_home.mkdir()
    target = case_home / "typed.txt"
    target.write_text("hello world\n", encoding="utf-8")
    (case_home / ".tinyeditrc").write_text("backup_interval = 0\n", encoding="utf-8")
    process, master = spawn_editor([str(target)], case_home)
    try:
        read_available(master)
        os.write(master, b"\x14" + b"\x1b[C" * 5 + b"X\x13")
        assert b"bytes written to disk" in read_until(master, b"bytes written to disk")
    finally:
        finish(process, master)
    assert target.read_text(encoding="utf-8") == "X world\n"


def test_shift_down_selects_last_wrapped_line(home):
    """Shift+Down at the final visual row extends selection to line end."""
    case_home = pathlib.Path(home) / "shift-down-eof"
    case_home.mkdir()
    target = case_home / "last-line.txt"
    target.write_text("abcdef\n", encoding="utf-8")
    (case_home / ".tinyeditrc").write_text(
        "soft_wrap = 0\nbackup_interval = 0\n", encoding="utf-8"
    )
    process, master = spawn_editor([str(target)], case_home)
    try:
        read_available(master)
        os.write(master, b"\x1b[1;2BX\x13")
        assert b"bytes written to disk" in read_until(master, b"bytes written to disk")
    finally:
        finish(process, master)
    assert target.read_text(encoding="utf-8") == "X\n"


def test_up_on_first_line_moves_to_start(home):
    """Up at the top boundary acts like Home instead of doing nothing."""
    case_home = pathlib.Path(home) / "up-at-start"
    case_home.mkdir()
    target = case_home / "first-line.txt"
    target.write_text("abcdef\n", encoding="utf-8")
    (case_home / ".tinyeditrc").write_text("backup_interval = 0\n", encoding="utf-8")
    process, master = spawn_editor([str(target)], case_home)
    try:
        read_available(master)
        os.write(master, b"\x1b[F\x1b[AX\x13")  # End, Up, insert, save.
        assert b"bytes written to disk" in read_until(master, b"bytes written to disk")
    finally:
        finish(process, master)
    assert target.read_text(encoding="utf-8") == "Xabcdef\n"


def test_ghostty_shift_enter_is_enter(home):
    """Ghostty Shift+Enter encodings must insert a newline."""
    case_home = pathlib.Path(home) / "ghostty-shift-enter"
    case_home.mkdir()
    target = case_home / "shift-enter.txt"
    target.write_text("ab\n", encoding="utf-8")
    (case_home / ".tinyeditrc").write_text("backup_interval = 0\n", encoding="utf-8")
    process, master = spawn_editor([str(target)], case_home)
    try:
        read_available(master)
        os.write(master, b"\x1b[C\x1b[27;2;13~X\x13")
        assert b"bytes written to disk" in read_until(master, b"bytes written to disk")
    finally:
        finish(process, master)
    assert target.read_text(encoding="utf-8") == "a\nXb\n"


def test_copy_preserves_selection(home):
    """Copy is non-destructive and leaves the copied range selected."""
    case_home = pathlib.Path(home) / "copy-selection"
    case_home.mkdir()
    target = case_home / "copy.txt"
    empty_path = case_home / "empty-path"
    empty_path.mkdir()
    target.write_text("hello world\n", encoding="utf-8")
    (case_home / ".tinyeditrc").write_text(
        "show_line_numbers = 0\nshow_top_bar = 0\n"
        "syntax_highlight = 0\ncolor_selection = yellow-light\n",
        encoding="utf-8",
    )
    # An empty PATH forces TinyEdit's process-local clipboard backend, so the
    # test never reads or overwrites the user's desktop clipboard.
    process, master = spawn_editor(
        [str(target)], case_home, {"PATH": str(empty_path)}
    )
    try:
        read_available(master)
        os.write(master, b"\x14" + b"\x1b[C" * 5)  # select "hello"
        read_available(master, 0.2)
        os.write(master, b"\x03")  # Ctrl-C
        output = read_until(master, b"5 bytes copied")
        selected = b"\x1b[93m\x1b[7mhello\x1b[m world"
        assert selected in output, "Ctrl-C cleared the copied selection"
    finally:
        finish(process, master)


def test_eol_after_trailing_tab(home):
    """The EOL marker follows the full visual width of a trailing tab."""
    case_home = pathlib.Path(home) / "eol-trailing-tab"
    case_home.mkdir()
    target = case_home / "eol.txt"
    source = "\t\nA\t\nAB\t\nABC\t\n"
    target.write_text(source, encoding="utf-8")
    (case_home / ".tinyeditrc").write_text(
        "show_invisibles = true\nshow_line_numbers = 0\nshow_top_bar = 0\n"
        "insert_spaces_for_tab = 0\ntab_stop = 4\nsyntax_highlight = 0\n",
        encoding="utf-8",
    )
    process, master = spawn_editor([str(target)], case_home)
    try:
        expect_rendered_rows(read_available(master), source, True)
    finally:
        finish(process, master)


def setting_index(key):
    """Row index of a setting in the F2 panel, read from settings.c.

    The panel is driven by the descriptor table, so its order is that
    table's order. Looking the row up by key keeps these tests working
    when a new setting is inserted above it -- hardcoded indices
    silently start editing the wrong row instead.
    """
    source = (ROOT / "src" / "settings.c").read_text(encoding="utf-8")
    table = source.split("settingDescriptors[] = {", 1)[1].split("\n};", 1)[0]
    keys = re.findall(r'\{\s*"([^"]+)"', table)
    assert key in keys, f"setting {key!r} not found in settings.c"
    return keys.index(key)


def edit_setting(master, key, keys, save):
    """Edit one F2 setting, exercising either save path or discard."""
    index = setting_index(key)
    os.write(master, b"\x1bOQ")  # F2
    assert b"Settings" in read_until(master, b"Settings")
    os.write(master, b"\x1b[B" * index + keys)
    read_available(master, 0.2)
    if save == "ctrl-s":
        os.write(master, b"\x13")
    elif save == "f2":
        os.write(master, b"\x1bOQ")
    else:
        os.write(master, b"\x1b")
        assert b"Save changes before leaving?" in read_until(
            master, b"Save changes before leaving?"
        )
        os.write(master, b"n" if save == "discard" else b"y")
    return read_available(master)


def expect_rendered_rows(output, text, visible, tab_stop=4):
    """Compare the actual last redraw, including uncolored stale markers."""
    frame = output.rsplit(b"\x1b[?25l", 1)[-1]
    plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", frame)
    expected = []
    for line in text.splitlines():
        rendered = ""
        for char in line:
            if char == "\t":
                width = tab_stop - len(rendered) % tab_stop
                rendered += (">" if visible else " ") + " " * (width - 1)
            elif char == " " and visible:
                rendered += "."
            else:
                rendered += char
        expected.append((rendered + ("$" if visible else "")).encode())
    lines = plain.split(b"\r\n")
    if lines and lines[0].startswith(b" TinyEdit  File  Edit  View  Help "):
        lines = lines[1:]
    actual = lines[:len(expected)]
    assert actual == expected, f"stale row rendering: {actual!r}, expected {expected!r}"


def test_settings_refresh_rows(home, save, initially_visible):
    case_home = pathlib.Path(home) / f"refresh-{save}-{initially_visible}"
    case_home.mkdir()
    (case_home / ".tinyeditrc").write_text(
        f"show_invisibles = {initially_visible}\nshow_line_numbers = 0\n"
        "show_top_bar = 0\ninsert_spaces_for_tab = 0\ntab_stop = 4\n"
        "color_syntax_string = green-light\n",
        encoding="utf-8",
    )
    source = 'alpha beta\t.>$\nsecond\tline tail\n"third row"\n'
    target = case_home / "refresh.c"
    target.write_text(source, encoding="utf-8")
    process, master = spawn_editor([str(target)], case_home)
    try:
        expect_rendered_rows(read_available(master), source, initially_visible)
        os.write(master, b"X")  # snapshot under the original settings
        expect_rendered_rows(read_available(master), "X" + source, initially_visible)

        # Discard must preserve both the live settings and row rendering.
        output = edit_setting(master, "show_invisibles", b" ", "discard")
        expect_rendered_rows(output, "X" + source, initially_visible)
        output = edit_setting(master, "show_invisibles", b" ", save)
        visible = not initially_visible
        expect_rendered_rows(output, "X" + source, visible)
        assert f"show_invisibles = {str(visible).lower()}" in (
            case_home / ".tinyeditrc"
        ).read_text()

        os.write(master, b"\x1a")  # Undo after changing the settings
        output = read_available(master)
        expect_rendered_rows(output, source, visible)
        assert b'\x1b[92mt\x1b[m' in output, "Undo lost third-row syntax color"
        os.write(master, b"\x19")  # Redo must also use the live settings
        expect_rendered_rows(read_available(master), "X" + source, visible)

        output = edit_setting(master, "tab_stop", b"\r8\r", save)
        expect_rendered_rows(output, "X" + source, visible, 8)
        os.write(master, b"\x1a")
        expect_rendered_rows(read_available(master), source, visible, 8)
        os.write(master, b"\x19")
        expect_rendered_rows(read_available(master), "X" + source, visible, 8)

        output = edit_setting(master, "show_invisibles", b" ", save)
        expect_rendered_rows(output, "X" + source, initially_visible, 8)
        os.write(master, b"\x1a")
        expect_rendered_rows(read_available(master), source, initially_visible, 8)
        os.write(master, b"\x19")
        expect_rendered_rows(read_available(master), "X" + source, initially_visible, 8)

        for syntax_enabled in (False, True):
            output = edit_setting(master, "syntax_highlight", b" ", save)
            expect_rendered_rows(output, "X" + source, initially_visible, 8)
            assert (b'\x1b[92mt\x1b[m' in output) == syntax_enabled, (
                "syntax setting did not refresh untouched rows"
            )
        os.write(master, b"\x13")
        assert b"bytes written to disk" in read_until(master, b"bytes written to disk")
        assert target.read_text() == "X" + source, "display markers changed the saved text"
    finally:
        finish(process, master)


def test_settings_syntax_color_preview(home):
    """Syntax color rows preview representative text on the chosen background."""
    case_home = pathlib.Path(home) / "settings-syntax-preview"
    case_home.mkdir()
    target = case_home / "preview.c"
    target.write_text("int main(void) { return 0; }\n", encoding="utf-8")
    (case_home / ".tinyeditrc").write_text(
        "color_background = blue-dark\ncolor_syntax_keyword = cyan-light\n",
        encoding="utf-8",
    )
    process, master = spawn_editor([str(target)], case_home)
    try:
        read_available(master)
        os.write(master, b"\x1bOQ" + b"\x1b[B" * setting_index("color_syntax_keyword"))
        output = read_available(master)
        assert b"\x1b[44m\x1b[96mprintf\x1b[m" in output, (
            "keyword preview does not use its foreground and editor background"
        )
        os.write(master, b"\x1b")
    finally:
        finish(process, master)


def test_settings_status_bar_preview(home):
    """Status-bar previews use their separately configured text color."""
    case_home = pathlib.Path(home) / "settings-status-preview"
    case_home.mkdir()
    target = case_home / "preview.c"
    target.write_text("int main(void) { return 0; }\n", encoding="utf-8")
    (case_home / ".tinyeditrc").write_text(
        "color_statusbar = blue-dark\ncolor_statusbar_text = white-light\n",
        encoding="utf-8",
    )
    process, master = spawn_editor([str(target)], case_home)
    try:
        read_available(master)
        os.write(master, b"\x1bOQ" + b"\x1b[B" * setting_index("color_statusbar"))
        output = read_available(master)
        assert b"\x1b[44m\x1b[97m status \x1b[m" in output, (
            "status preview does not use the separate text color"
        )
        os.write(master, b"\x1b")
    finally:
        finish(process, master)


def test_block_indent(home):
    """Tab/Shift+Tab shift the selected lines and keep the selection."""
    case_home = pathlib.Path(home) / "block-indent"
    case_home.mkdir()
    target = case_home / "indent.txt"
    (case_home / ".tinyeditrc").write_text(
        "insert_spaces_for_tab = true\ntab_stop = 4\nbackup_interval = 0\n",
        encoding="utf-8",
    )

    def run(initial, keys):
        target.write_text(initial, encoding="utf-8")
        process, master = spawn_editor([str(target)], case_home)
        try:
            read_available(master)
            for key in keys:
                os.write(master, key)
                read_available(master, 0.2)
            os.write(master, b"\x13")  # Ctrl-S
            read_until(master, b"bytes written to disk")
            return target.read_text(encoding="utf-8")
        finally:
            finish(process, master)

    select_two = [b"\x14", b"\x1b[B", b"\x1b[C"]  # Ctrl-T, Down, Right

    got = run("aaa\nbbb\nccc\n", select_two + [b"\t"])
    assert got == "    aaa\n    bbb\nccc\n", f"block indent: {got!r}"

    # Repeatable: the selection has to survive each press.
    got = run("aaa\nbbb\nccc\n", select_two + [b"\t", b"\t"])
    assert got == "        aaa\n        bbb\nccc\n", f"repeated indent: {got!r}"

    # ...including across a save, which used to clear the selection.
    got = run("aaa\nbbb\nccc\n", select_two + [b"\t", b"\x13", b"\x1b[Z"])
    assert got == "aaa\nbbb\nccc\n", f"outdent after save: {got!r}"

    # A redraw (and therefore a SIGWINCH translated to Ctrl-L) is view-only:
    # it must not clear the selected block before the next edit.
    got = run("aaa\nbbb\nccc\n", select_two + [b"\x0c", b"\t"])
    assert got == "    aaa\n    bbb\nccc\n", f"selection lost on redraw: {got!r}"

    # Ctrl-T initially arms a zero-width anchor. It is selection state for
    # future motion, but not selected text for an editing command.
    got = run("aaa\n", [b"\x14", b"\t"])
    assert got == "    aaa\n", f"collapsed selection treated as a block: {got!r}"

    # Outdent stops at column 0 instead of eating the text.
    got = run("  aaa\n  bbb\n", select_two + [b"\x1b[Z", b"\x1b[Z"])
    assert got == "aaa\nbbb\n", f"outdent floor: {got!r}"

    # A tab counts as one whole level whatever tab_stop says.
    got = run("\taaa\n\tbbb\n", select_two + [b"\x1b[Z"])
    assert got == "aaa\nbbb\n", f"outdent of a literal tab: {got!r}"

    # Ghostty changes Shift+Tab to Kitty CSI-u form when macOS shortcuts
    # are enabled; it must outdent the selected block just like CSI Z.
    got = run("    aaa\n    bbb\n", select_two + [b"\x1b[9;2u"])
    assert got == "aaa\nbbb\n", f"Ghostty Kitty Shift+Tab: {got!r}"

    # With no selection Shift+Tab outdents just the cursor's line.
    got = run("    aaa\n    bbb\n", [b"\x1b[B", b"\x1b[Z"])
    assert got == "    aaa\nbbb\n", f"outdent without selection: {got!r}"


def test_no_save_prompt_when_undone(home):
    """Undoing every edit must leave the file clean, not merely 'dirty'."""
    case_home = pathlib.Path(home) / "undo-clean"
    case_home.mkdir()
    target = case_home / "undo.txt"
    (case_home / ".tinyeditrc").write_text("backup_interval = 0\n", encoding="utf-8")

    def quits_without_prompt(keys):
        target.write_text("aaa\nbbb\n", encoding="utf-8")
        process, master = spawn_editor([str(target)], case_home)
        try:
            read_available(master)
            for key in keys:
                os.write(master, key)
                read_available(master, 0.2)
            os.write(master, b"\x11")  # Ctrl-Q
            return b"Save changes before" not in read_available(master, 0.5)
        finally:
            # Answering the prompt only applies when there was one; when
            # the editor quit on its own the pty is already gone.
            try:
                os.write(master, b"n")
            except OSError:
                pass
            finish(process, master)

    assert quits_without_prompt([]), "clean file asked to save"
    assert quits_without_prompt([b"x", b"\x1a"]), "edit+undo still asked to save"
    assert quits_without_prompt([b"x", b"\x7f"]), "retyped-identical still asked to save"
    assert not quits_without_prompt([b"x"]), "real edit did not ask to save"
    assert not quits_without_prompt([b"x", b"\x1a", b"\x19"]), "redo did not ask to save"


def test_xml_tag_autoclose(home):
    """Opening XML/HTML tags close, while HTML void tags do not."""
    case_home = pathlib.Path(home) / "xml-tag-autoclose"
    case_home.mkdir()

    def type_and_save(name, typed):
        target = case_home / name
        target.write_bytes(b"")
        process, master = spawn_editor([str(target)], case_home)
        try:
            read_available(master)
            os.write(master, typed)
            read_available(master, 0.2)
            os.write(master, b"\x13")
            output = read_until(master, b"bytes written to disk")
            assert b"bytes written to disk" in output, f"{name}: save did not finish: {output!r}"
        finally:
            finish(process, master)
        return target.read_bytes()

    actual = type_and_save("card.xml", b"<card>")
    assert actual == b"<card></card>\n", actual
    actual = type_and_save("line.xml", b"<br>")
    assert actual == b"<br></br>\n", actual
    actual = type_and_save("page.html", b"<section>")
    assert actual == b"<section></section>\n", actual
    actual = type_and_save("image.html", b"<img>")
    assert actual == b"<img>\n", actual
    actual = type_and_save("break.htm", b"<br>")
    assert actual == b"<br>\n", actual
    actual = type_and_save("self-closing.xml", b"<item/>")
    assert actual == b"<item/>\n", actual


def test_malformed_utf8_round_trip(home):
    """Rendering may replace bad bytes; saving and deletion must use the originals."""
    target = home / "malformed.bin"
    original = b"A\xe2\x82B\xc3\xa9\xffC\n"
    target.write_bytes(original)
    process, master = spawn_editor([str(target)], home)
    try:
        output = read_available(master)
        assert b"\xef\xbf\xbd" in output, "malformed byte was not rendered safely"
        os.write(master, b"\x13")
        output = read_until(master, b"bytes written to disk")
        assert b"bytes written to disk" in output, "save did not finish"
        assert target.read_bytes() == original, "save changed malformed bytes"
    finally:
        finish(process, master)

    target.write_bytes(original)
    process, master = spawn_editor([str(target)], home)
    try:
        read_available(master)
        os.write(master, b"\x1b[C\x1b[C\x7f\x13")
        output = read_until(master, b"bytes written to disk")
        assert b"bytes written to disk" in output, "edited malformed file was not saved"
        assert target.read_bytes() == b"A\x82B\xc3\xa9\xffC\n", (
            "backspace did not delete exactly one malformed byte"
        )
    finally:
        finish(process, master)


def test_malformed_utf8_at_file_boundaries(home):
    """A bad sequence beside LF or EOF must not consume the boundary."""
    samples = (
        b"OVERLONG_EOL: A\xc0\x80\nTRUNCATED_EOL: A\xe2\x82\n"
        b"OVERLONG_EOF: A\xf0\x80\x80\xaf",
        b"TRUNCATED_EOF: A\xf0\x9f\x98",
    )
    for index, original in enumerate(samples):
        target = home / f"malformed-boundary-{index}.txt"
        target.write_bytes(original)
        process, master = spawn_editor([str(target)], home)
        try:
            output = read_available(master)
            assert b"\xef\xbf\xbd" in output, "boundary error was not rendered"
            os.write(master, b"\x13")
            output = read_until(master, b"bytes written to disk")
            assert b"bytes written to disk" in output, "boundary file was not saved"
            assert target.read_bytes() == original, "save changed bytes beside LF or EOF"
        finally:
            finish(process, master)



def test_long_prompt_inputs(home):
    """Typed, clipboard and bracketed input retain paths beyond 128 bytes."""
    case_home = home / "long-prompts"
    case_home.mkdir()
    (case_home / ".tinyeditrc").write_text("auto_close_pairs = false\n")
    directory = case_home / ("a" * 80) / ("b" * 80)
    directory.mkdir(parents=True)
    tools = case_home / "tools"
    tools.mkdir()
    for command in ("pbcopy", "pbpaste"):
        script = tools / command
        script.write_text(
            "#!/usr/bin/env python3\nimport os, sys\n"
            "if sys.argv[0].endswith('pbpaste'):\n"
            "    sys.stdout.buffer.write(os.environ['TEST_PASTE_PATH'].encode('utf-8'))\n"
        )
        script.chmod(0o755)
    for method in ("typed", "clipboard", "bracketed"):
        target = directory / f"é-{method}.txt"
        target.write_bytes(b"original\n")
        path = str(target).encode("utf-8")
        assert len(path) > 128
        process, master = spawn_editor([], case_home, {
            "PATH": str(tools) + os.pathsep + os.environ.get("PATH", ""),
            "TEST_PASTE_PATH": str(target),
        })
        try:
            read_available(master)
            os.write(master, b"\x0f")
            assert b"Open file:" in read_until(master, b"Open file:")
            if method == "typed":
                os.write(master, path)
            elif method == "clipboard":
                os.write(master, b"\x16")
            else:
                os.write(master, b"\x1b[200~" + path + b"\x1b[201~")
            read_available(master, 0.3)
            os.write(master, b"\r")
            assert b"original" in read_until(master, b"original"), (
                f"{method}: long Open prompt did not retain its complete path"
            )
            os.write(master, b"Z\x13")
            assert b"bytes written to disk" in read_until(master, b"bytes written to disk")
            assert target.read_bytes() == b"Zoriginal\n"
        finally:
            finish(process, master)


def test_empty_page_navigation(home):
    """Page movement survives startup, close and deletion of all text."""
    case_home = home / "empty-pages"
    case_home.mkdir()
    process, master = spawn_editor([], case_home)
    keys = b"\x1b[5~\x1b[6~\x1b[5;2~\x1b[6;2~"
    try:
        read_available(master)
        for setup in (b"", b"\x17", b"abc\x01\x7f"):
            os.write(master, setup + keys + b"X")
            output = read_available(master)
            assert process.poll() is None, "page navigation crashed on empty text"
            assert b"X" in output, "editing no longer works after empty page movement"
            os.write(master, b"\x01\x7f\x17n")
            read_available(master, 0.2)
    finally:
        finish(process, master)

def test_file_transaction_failures(home):
    """Failed Open/Save as preserve current text, undo and save destination."""
    case_home = home / "file-transactions"
    case_home.mkdir()
    (case_home / ".tinyeditrc").write_text("auto_close_pairs = false\n")
    original = case_home / "original.txt"
    original.write_bytes(b"original\r")
    process, master = spawn_editor([str(original)], case_home)
    try:
        read_available(master)
        os.write(master, b"Z\x1bOS")  # existing SS3 F4 binding
        assert b"Save as:" in read_until(master, b"Save as:")
        os.write(master, os.fsencode(case_home) + b"\r")
        assert b"Can't save!" in read_until(master, b"Can't save!")
        assert original.read_bytes() == b"original\r"
        os.write(master, b"\x13")
        assert b"bytes written to disk" in read_until(master, b"bytes written to disk")
        assert original.read_bytes() == b"Zoriginal\r", "failed Save as changed destination or CR bytes"
        # Separate Y from the earlier insertion under the existing undo timer.
        read_available(master, 2.1)
        os.write(master, b"Y\x0f")
        assert b"Save changes before opening another file?" in read_until(
            master, b"Save changes before opening another file?")
        os.write(master, b"n")
        assert b"Open file:" in read_until(master, b"Open file:")
        os.write(master, os.fsencode(case_home) + b"\r")
        assert b"Can't open file:" in read_until(master, b"Can't open file:")
        os.write(master, b"\x1a")  # undo the Y edit in the retained document
        assert b"Undo" in read_until(master, b"Undo")
        os.write(master, b"\x13")
        assert b"bytes written to disk" in read_until(master, b"bytes written to disk")
        assert original.read_bytes() == b"Zoriginal\r", "failed Open destroyed current text or undo"
    finally:
        finish(process, master)


def test_mouse_burst_preserves_inputs(home):
    """Queued text, navigation, paste and commands survive mouse coalescing."""
    case_home = home / "mouse-burst"
    case_home.mkdir()
    (case_home / ".tinyeditrc").write_text(
        "mouse_enabled = true\nshow_menu = false\nshow_top_bar = false\n"
        "show_line_numbers = false\nauto_close_pairs = false\nbackup_interval = 0\n"
    )
    target = case_home / "burst.txt"
    target.write_bytes(b"abcdef\n")
    process, master = spawn_editor([str(target)], case_home)
    try:
        read_available(master)
        wheel = b"\x1b[<65;1;1M\x1b[<64;1;1M"
        cases = (
            (b"\x1b[<0;2;1M\x1b[<0;2;1mXY", b"aXYbcdef\n"),
            (wheel + b"\x1b[DZ", b"aXZYbcdef\n"),
            (wheel + b"\x1b[200~" + "é界".encode() + b"\x1b[201~",
             "aXZé界Ybcdef\n".encode()),
            (wheel + b"\x1a", b"aXZYbcdef\n"),
        )
        for sequence, expected in cases:
            os.write(master, sequence + b"\x13")
            assert b"bytes written to disk" in read_until(master, b"bytes written to disk"), (
                "command following a mouse burst was lost"
            )
            # An earlier save message can still appear during intermediate
            # mouse redraws. Wait for the actual disk effect of this command.
            deadline = time.monotonic() + 2.0
            while target.read_bytes() != expected and time.monotonic() < deadline:
                read_available(master, 0.1)
            assert target.read_bytes() == expected, (
                f"mouse burst content: expected {expected!r}, got {target.read_bytes()!r}"
            )
            read_available(master, 0.1)
        # The first keyboard event is itself a command, with no text in between.
        previous_inode = target.stat().st_ino
        os.write(master, wheel + b"\x13")
        deadline = time.monotonic() + 2.0
        while target.stat().st_ino == previous_inode and time.monotonic() < deadline:
            read_available(master, 0.1)
        assert target.stat().st_ino != previous_inode, "save command after mouse was not executed"
    finally:
        finish(process, master)


def test_drag_does_not_cross_documents(home):
    """An unfinished press cannot arm a selection in a newly opened document."""
    case_home = home / "mouse-document-reset"
    case_home.mkdir()
    (case_home / ".tinyeditrc").write_text(
        "mouse_enabled = true\nshow_menu = false\nshow_top_bar = false\n"
        "show_line_numbers = false\nauto_close_pairs = false\nbackup_interval = 0\n"
    )
    original, replacement = case_home / "old.txt", case_home / "new.txt"
    original.write_bytes(b"abcdef\n")
    replacement.write_bytes(b"xyz\n")
    process, master = spawn_editor([str(original)], case_home)
    try:
        read_available(master)
        os.write(master, b"\x1b[<0;4;1M")  # press without release
        read_available(master, 0.2)
        os.write(master, b"\x17")
        assert b"File closed." in read_until(master, b"File closed.")
        os.write(master, b"\x0f")
        assert b"Open file:" in read_until(master, b"Open file:")
        os.write(master, os.fsencode(replacement) + b"\r")
        assert b"xyz" in read_until(master, b"xyz"), "replacement document was not loaded"
        os.write(master, b"\x1b[<32;3;1MQ\x13")
        assert b"bytes written to disk" in read_until(master, b"bytes written to disk")
        assert replacement.read_bytes() == b"Qxyz\n", "old drag selected text in the new document"
        assert original.read_bytes() == b"abcdef\n"
    finally:
        finish(process, master)


def test_shared_ascii_autoclose(home):
    """Typing, closer skipping and selection wrapping use the shared policy."""
    for enabled in (False, True):
        case_home = home / f"shared-autoclose-{enabled}"
        case_home.mkdir()
        (case_home / ".tinyeditrc").write_text(
            f"auto_close_pairs = {'true' if enabled else 'false'}\n"
            "auto_close_single_quote = true\nbackup_interval = 0\n"
        )
        target = case_home / "pairs.txt"
        target.write_bytes(b"abc\n")
        process, master = spawn_editor([str(target)], case_home)
        try:
            read_available(master)
            os.write(master, b"\x01(\x13")
            assert b"bytes written to disk" in read_until(master, b"bytes written to disk")
            expected = b"(abc)\n" if enabled else b"(\n"
            deadline = time.monotonic() + 2.0
            while target.read_bytes() != expected and time.monotonic() < deadline:
                read_available(master, 0.1)
            assert target.read_bytes() == expected, "pair policy ignored selection or configuration"
            read_available(master, 0.1)
            os.write(master, b"\x01\x7f")
            read_available(master, 0.1)
            sequence = b"(){}[]\"\"''"
            os.write(master, sequence + b"\x13")
            expected = sequence + b"\n"
            deadline = time.monotonic() + 2.0
            while target.read_bytes() != expected and time.monotonic() < deadline:
                read_available(master, 0.1)
            assert target.read_bytes() == expected, "typing/closer skipping diverged from shared pairs"
        finally:
            finish(process, master)


def test_save_and_open_home_path(home):
    """A prompt path ~/tmp/... uses HOME regardless of the working directory."""
    case_home = home / "home-path"
    (case_home / "tmp").mkdir(parents=True)
    (case_home / ".tinyeditrc").write_text("auto_close_pairs = false\nbackup_interval = 0\n")
    target = case_home / "tmp" / "nomeFile.md"
    process, master = spawn_editor([], case_home)
    try:
        read_available(master)
        os.write(master, b"testo\x13")
        assert b"Save as:" in read_until(master, b"Save as:")
        os.write(master, b"~/tmp/nomeFile.md\r")
        assert b"bytes written to disk" in read_until(master, b"bytes written to disk")
        assert target.read_bytes() == b"testo\n"
        read_available(master, 0.1)
        os.write(master, b"!\x13")
        deadline = time.monotonic() + 2.0
        while target.read_bytes() != b"testo!\n" and time.monotonic() < deadline:
            read_available(master, 0.1)
        assert target.read_bytes() == b"testo!\n", "subsequent save did not use expanded identity"
        os.write(master, b"\x17")
        assert b"File closed." in read_until(master, b"File closed.")
        os.write(master, b"\x0f")
        assert b"Open file:" in read_until(master, b"Open file:")
        os.write(master, b"~/tmp/nomeFile.md\r")
        assert b"testo!" in read_until(master, b"testo!"), "Open did not expand home shorthand"
    finally:
        finish(process, master)


def main():
    with tempfile.TemporaryDirectory(prefix="tinyedit-tests-") as tmp:
        home = pathlib.Path(tmp)
        test_save_and_open_home_path(home)
        test_shared_ascii_autoclose(home)
        test_mouse_burst_preserves_inputs(home)
        test_drag_does_not_cross_documents(home)
        test_file_transaction_failures(home)
        test_long_prompt_inputs(home)
        test_empty_page_navigation(home)
        test_f3(b"\x1bOR", "SS3", home)
        test_path_completion(home)
        test_sidebar(home)
        test_binary_open_rejected(home)
        test_f3(b"\x1b[13~", "CSI", home)
        test_kitty_f1_f2(home)
        test_ghostty_ctrl_i_is_drained(home)
        test_kitty_keyboard_mode_is_restored(home)
        test_alternate_screen_lifecycle(home)
        test_kitty_cmd_z_undoes(home)
        test_kitty_cmd_a_selects_all(home)
        test_shift_click_extends_selection(home)
        test_menu_bar_precedes_first_file_row(home)
        test_menu_restores_editor_background(home)
        test_menu_mouse_navigation(home)
        test_menu_show_invisibles_refreshes_rows(home)
        test_menu_view_toggles_settings(home)
        test_matching_bracket_highlight(home)
        test_very_long_wrapped_line(home)
        test_utf8_word_jumps(home)
        test_malformed_utf8_round_trip(home)
        test_malformed_utf8_at_file_boundaries(home)
        test_regex_replace_all_newline_finishes(home)
        test_ctrl_w_saves_and_closes_only_file(home)
        test_ctrl_o_discards_then_creates_named_file(home)
        test_invisible_colors(home)
        test_selection_across_tab(home)
        test_selection_marks_blank_rows(home)
        test_regex_finds_logical_newline(home)
        test_copy_preserves_selection(home)
        test_bracketed_paste_replaces_selection_atomically(home)
        test_typed_text_replaces_selection(home)
        test_shift_down_selects_last_wrapped_line(home)
        test_up_on_first_line_moves_to_start(home)
        test_ghostty_shift_enter_is_enter(home)
        test_eol_after_trailing_tab(home)
        test_block_indent(home)
        test_no_save_prompt_when_undone(home)
        test_xml_tag_autoclose(home)
        test_settings_syntax_color_preview(home)
        test_settings_status_bar_preview(home)
        for save in ("ctrl-s", "f2", "esc-y"):
            for initially_visible in (0, 1):
                test_settings_refresh_rows(home, save, initially_visible)
    print("pty tests: ok")


if __name__ == "__main__":
    main()
