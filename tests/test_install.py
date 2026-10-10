"""Exercise shared installation and resource loading with an isolated HOME."""
import os
from pathlib import Path
import subprocess
import tempfile
import test_pty as terminal

ROOT = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix="tinyedit install ") as temporary:
        base = Path(temporary)
        prefix = base / "prefix space"
        stage = base / "stage"
        home = base / "home"
        home.mkdir()
        env = dict(os.environ, HOME=str(home))
        def make(*args):
            subprocess.run(["make", f"PREFIX={prefix}", *args], cwd=ROOT, env=env, check=True)
        try:
            make("install-binary", f"DESTDIR={stage}")
            staged = stage / str(prefix).lstrip("/")
            assert (staged / "bin/tinyedit").is_file()
            assert not (staged / "share").exists()
            assert not list(home.iterdir())
            make("install", f"DESTDIR={stage}")
            for source, destination in (("colorschemes", "color-scheme"), ("syntax-configs", "syntax")):
                for resource in (ROOT / source).glob("*.conf"):
                    assert (staged / "share/tinyedit" / destination / resource.name).read_bytes() == resource.read_bytes()
            assert (staged / "share/tinyedit/docs/README.md").is_file()
            assert not list(home.iterdir()), "installation must never write to HOME"
            # Move the staged tree to its runtime prefix; DESTDIR must not be embedded.
            staged.rename(prefix)
            terminal.BINARY = prefix / "bin/tinyedit"
            (home / ".tinyeditrc").write_text("color_mode = rgb\nrgb_syntax_keyword = #010203\n")
            target = home / "sample.vim"
            target.write_text("set number\n")
            for personal in (False, True):
                if personal:
                    directory = home / ".tinyedit/syntax"
                    directory.mkdir(parents=True)
                    (directory / "mine.conf").write_text("extensions = vim\nkeywords = number\nfiletype = Personal\n")
                process, master = terminal.spawn_editor([str(target)], home)
                try:
                    output = terminal.read_available(master)
                    assert b"Vim" in output
                    assert (b"\x1b[38;2;1;2;3mn" if personal else b"\x1b[38;2;1;2;3ms") in output
                finally:
                    terminal.finish(process, master)
            terminal.test_color_scheme_selection(home)
            for personal in (False, True):
                if personal:
                    directory = home / ".tinyedit/color-scheme"
                    directory.mkdir(parents=True)
                    (directory / "catppuccin-mocha.conf").write_text(
                        "color_mode = rgb\nrgb_background = #010203\n")
                process, master = terminal.spawn_editor([], home)
                try:
                    terminal.read_available(master)
                    navigation, _ = terminal.setting_navigation("color_mode")
                    os.write(master, b"\x1bOQ" + navigation + b"\x1b[B\r")
                    output = terminal.read_available(master)
                    assert b"Choose Color Scheme (use < > to change) catppuccin-mocha" in output
                    if personal:
                        assert b"010203" in output, repr(output)
                    os.write(master, b"\x1b[C")
                    assert b"Choose Color Scheme (use < > to change) one-dark" in terminal.read_available(master)
                finally:
                    terminal.finish(process, master)
            (prefix / "share/tinyedit/docs/README.md").write_text("shared guide marker\n")
            for personal in (False, True):
                if personal:
                    directory = home / ".tinyedit/docs"
                    directory.mkdir(parents=True)
                    (directory / "README.md").write_text("personal guide marker\n")
                process, master = terminal.spawn_editor([], home)
                try:
                    terminal.read_available(master)
                    os.write(master, b"\x1b[21~")
                    terminal.read_available(master, 0.2)
                    os.write(master, b"\x1b[C" * 4 + b"\x1b[B\r")
                    output = terminal.read_available(master, 0.8)
                    assert b"README.md" in output
                    assert (b"personal guide marker" if personal else b"shared guide marker") in output
                finally:
                    terminal.finish(process, master)
        finally:
            # Restore the normal development binary even when a regression fails.
            subprocess.run(["make"], cwd=ROOT, check=True)
    print("shared installation tests: ok")


if __name__ == "__main__":
    main()
