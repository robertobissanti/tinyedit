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
    source = (ROOT / "settings.c").read_text(encoding="utf-8")
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
    actual = plain.split(b"\r\n")[:len(expected)]
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

    # Outdent stops at column 0 instead of eating the text.
    got = run("  aaa\n  bbb\n", select_two + [b"\x1b[Z", b"\x1b[Z"])
    assert got == "aaa\nbbb\n", f"outdent floor: {got!r}"

    # A tab counts as one whole level whatever tab_stop says.
    got = run("\taaa\n\tbbb\n", select_two + [b"\x1b[Z"])
    assert got == "aaa\nbbb\n", f"outdent of a literal tab: {got!r}"

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
        test_invisible_colors(home)
        test_selection_across_tab(home)
        test_bracketed_paste_replaces_selection_atomically(home)
        test_eol_after_trailing_tab(home)
        test_block_indent(home)
        test_no_save_prompt_when_undone(home)
        for save in ("ctrl-s", "f2", "esc-y"):
            for initially_visible in (0, 1):
                test_settings_refresh_rows(home, save, initially_visible)
    print("pty tests: ok")


if __name__ == "__main__":
    main()
