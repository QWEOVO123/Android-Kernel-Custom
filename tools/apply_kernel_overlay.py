#!/usr/bin/env python3
"""Apply the Meow source snapshot to its pinned Android Common kernel."""
import argparse
from pathlib import Path
import shutil
import subprocess

repo = Path(__file__).resolve().parents[1]
overlay = repo / "bootmenu-source"
patches = repo / "kernel-patches"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("kernel", type=Path, help="clean Android Common kernel checkout")
parser.add_argument("--check", action="store_true", help="check without writing files")
options = parser.parse_args()
kernel = options.kernel.resolve()

def git(*args):
    return subprocess.check_output(["git", "-C", str(kernel), *args], text=True).strip()

try:
    if Path(git("rev-parse", "--show-toplevel")).resolve() != kernel:
        parser.error("kernel must be the root of a Git checkout")
    if git("rev-parse", "HEAD") != (patches / "BASE").read_text().strip():
        parser.error("kernel checkout is not at the recorded base commit")
    if git("status", "--porcelain", "--untracked-files=no"):
        parser.error("kernel checkout has tracked changes")
except subprocess.CalledProcessError as exc:
    raise SystemExit(exc.returncode) from exc

links = {"drivers/kernelsu": "../KernelSU/kernel",
         "KernelSU/kernel/include/uapi": "../../uapi"}
files = sorted(path.relative_to(overlay) for path in overlay.rglob("*") if path.is_file())
for relative in [*files, *map(Path, links)]:
    target = kernel / relative
    if target.exists() or target.is_symlink():
        parser.error(f"overlay destination already exists: {relative}")
    if any(parent.is_symlink() for parent in target.parents if parent != kernel):
        parser.error(f"overlay destination has a symlink ancestor: {relative}")
patch = str(patches / "0001-android12-5.10.233-meow.patch")
git("apply", "--check", patch)
if not options.check:
    git("apply", patch)
    for relative in files:
        target = kernel / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(overlay / relative, target)
        if relative.suffix == ".sh" or relative.name.endswith("-control.py") or relative.name == "ip_forward_ttl.py":
            target.chmod(target.stat().st_mode | 0o111)
    for relative, link in links.items():
        (kernel / relative).symlink_to(link)
print(f"Checked {len(files)} overlay files and {len(links)} symlinks"
      if options.check else f"Applied patch and installed {len(files)} overlay files")