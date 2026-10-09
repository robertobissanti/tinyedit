"""Verify the installed guide bundle without touching the user's home."""
import os
import re
import subprocess
import tempfile
from pathlib import Path
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]


def test_colorscheme_copy_failure(temporary):
    """A later successful copy must not hide an earlier installation error."""
    tools = Path(temporary) / "fake-tools"
    tools.mkdir()
    copier = tools / "cp"
    copier.write_text(
        '#!/bin/sh\n'
        'if [ "$1" = "$TE_FAIL_SOURCE" ]; then exit 23; fi\n'
        'exec /bin/cp "$@"\n'
    )
    copier.chmod(0o755)
    sources = sorted((ROOT / "colorschemes").glob("*.conf"))
    assert len(sources) > 1, "fixture needs another copy after the injected failure"
    destination = Path(temporary) / "failed-install"
    env = dict(os.environ, PATH=str(tools) + ":" + os.environ["PATH"],
               TE_FAIL_SOURCE="colorschemes/" + sources[0].name)
    result = subprocess.run(
        ["make", "install-colorschemes", f"COLORSCHEME_DIR={destination}"],
        cwd=ROOT, env=env, capture_output=True, text=True,
    )
    assert not (destination / sources[0].name).exists(), "copy fault was injected"
    assert result.returncode != 0, "install-colorschemes must report a failed copy"


def main():
    with tempfile.TemporaryDirectory(prefix="tinyedit docs ") as temporary:
        test_colorscheme_copy_failure(temporary)
        destination = Path(temporary) / ".tinyedit"
        active = destination / "syntax" / "personal.conf"
        active.parent.mkdir(parents=True)
        active.write_text("personal syntax\n")
        subprocess.run(["make", "install-docs", f"DOCS_DIR={destination}"], cwd=ROOT, check=True)
        assert (destination / "docs/README.md").read_bytes() == (ROOT / "docs/README.md").read_bytes()
        for source in (ROOT / "docs").glob("*.md"):
            installed = destination / "docs" / source.name
            assert installed.read_bytes() == source.read_bytes(), source
            text = re.sub(r"```.*?```", "", installed.read_text(), flags=re.S)
            text = re.sub(r"`[^`]*`", "", text)
            targets = [m.group(1) for m in re.finditer(r"!?\[[^\]\n]*\]\(([^)\n]+)\)", text)]
            targets += re.findall(r'<img\s[^>]*src="([^"]+)"', text)  # HTML images too
            for target in targets:
                parts = urlsplit(target)
                if parts.scheme or not parts.path:
                    continue
                path = installed.parent / unquote(parts.path)
                assert path.exists(), (installed.name, target)
                if path.is_dir():
                    assert (path / "README.md").is_file(), target
        subprocess.run(["make", "install-docs", f"DOCS_DIR={destination}"], cwd=ROOT, check=True)
        assert active.read_text() == "personal syntax\n"
        assert not (destination / "local").exists()
        schemes = Path(temporary) / "color-scheme"
        subprocess.run(["make", "install-colorschemes", f"COLORSCHEME_DIR={schemes}"], cwd=ROOT, check=True)
        shipped = sorted(p.name for p in (ROOT / "colorschemes").glob("*.conf"))
        assert shipped and sorted(p.name for p in schemes.glob("*.conf")) == shipped
        (schemes / shipped[0]).write_text("mine\n")
        subprocess.run(["make", "install-colorschemes", f"COLORSCHEME_DIR={schemes}"], cwd=ROOT, check=True)
        assert (schemes / shipped[0]).read_text() == "mine\n", "install-colorschemes never overwrites"
        subprocess.run(["make", "install-colorschemes-force", f"COLORSCHEME_DIR={schemes}"], cwd=ROOT, check=True)
        assert (schemes / shipped[0]).read_bytes() == (ROOT / "colorschemes" / shipped[0]).read_bytes()
        assert not (destination / ".git").exists()
    print("documentation installation tests: ok")


if __name__ == "__main__":
    main()
