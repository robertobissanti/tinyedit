"""Build identity across archives, commits, local edits and packaging overrides."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

with tempfile.TemporaryDirectory(prefix="tinyedit-build-") as directory:
    root = Path(directory)
    for name in ("src", "inc", "scripts"):
        shutil.copytree(ROOT / name, root / name)
    shutil.copy2(ROOT / "Makefile", root / "Makefile")
    env = dict(os.environ)
    env.pop("BUILD_ID", None)
    env.update(GIT_AUTHOR_NAME="Test", GIT_AUTHOR_EMAIL="test@example.invalid",
               GIT_COMMITTER_NAME="Test", GIT_COMMITTER_EMAIL="test@example.invalid")

    def run(*args):
        return subprocess.check_output(args, cwd=root, env=env, stderr=subprocess.STDOUT).decode()

    def identity():
        run("sh", "scripts/build-info.sh")
        return (root / "bin/build_info.h").read_text()

    archive = identity()
    assert '"source-s' in archive
    before = (root / "bin/build_info.h").stat().st_mtime_ns
    assert identity() == archive
    assert (root / "bin/build_info.h").stat().st_mtime_ns == before
    run("git", "init", "-q")
    run("git", "add", "src", "inc", "scripts", "Makefile")
    run("git", "commit", "-qm", "Initial sources")
    clean = identity()
    run("make", "bin/tinyedit")
    original_version = run("bin/tinyedit", "--version")
    assert '"g' in clean and "dirty" not in clean
    with (root / "src/tinyedit.c").open("a") as stream:
        stream.write("\n/* changed source */\n")
    dirty = identity()
    assert "-dirty-s" in dirty and dirty != clean
    with (root / "src/tinyedit.c").open("a") as stream:
        stream.write("/* another source change */\n")
    assert identity() != dirty
    run("git", "add", "src/tinyedit.c")
    run("git", "commit", "-qm", "Change sources")
    assert identity() != clean and "dirty" not in identity()
    run("make", "bin/tinyedit")
    assert run("bin/tinyedit", "--version") != original_version, "make did not rebuild after commit"
    # An archive nested in another repository must not inherit its parent commit.
    nested = root / "nested-archive"
    nested.mkdir()
    for name in ("src", "inc", "scripts"):
        shutil.copytree(root / name, nested / name)
    shutil.copy2(root / "Makefile", nested / "Makefile")
    subprocess.check_call(["sh", "scripts/build-info.sh"], cwd=nested, env=env)
    assert '"source-s' in (nested / "bin/build_info.h").read_text()
    env["BUILD_ID"] = "package-r2"
    assert '"package-r2"' in identity()
    env["BUILD_ID"] = 'bad"identifier'
    try:
        identity()
    except subprocess.CalledProcessError:
        pass
    else:
        raise AssertionError("invalid override accepted")

version = subprocess.check_output([str(ROOT / "bin/tinyedit"), "--version"], stdin=subprocess.DEVNULL)
assert version.startswith(b"tinyedit 0.3.8 Build ") and b"\x1b" not in version
print("build identity tests passed")
