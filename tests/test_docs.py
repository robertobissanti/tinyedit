"""Verify the installed guide bundle without touching the user's home."""
import re
import subprocess
import tempfile
from pathlib import Path
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix="tinyedit docs ") as temporary:
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
            for match in re.finditer(r"!?\[[^\]\n]*\]\(([^)\n]+)\)", text):
                target = match.group(1)
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
        assert not (destination / ".git").exists()
    print("documentation installation tests: ok")


if __name__ == "__main__":
    main()
