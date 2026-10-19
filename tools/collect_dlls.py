"""Collect missing MinGW runtime DLLs beside built executables (Windows)."""
import argparse
from collections import deque
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


def collect(build_dir, extra_dirs=(), objdump=None):
    build_dir = Path(build_dir).resolve()
    executables = sorted(build_dir.glob("*.exe"))
    if not executables:
        raise RuntimeError(f"No executables found in {build_dir}; build first")

    project = Path(__file__).resolve().parent.parent
    cache_path = build_dir / "CMakeCache.txt"
    cache = cache_path.read_text(encoding="utf-8", errors="replace") if cache_path.exists() else ""
    source = (project / "CMakeLists.txt").read_text(encoding="utf-8")
    directories = [Path(path).resolve() for path in extra_dirs]
    prefix = re.search(r"^MSYS2_PREFIX:[^=]+=(.+)$", cache, re.MULTILINE)
    if not prefix:
        prefix = re.search(r'set\(MSYS2_PREFIX\s+"([^"]+)"', source)
    if prefix:
        directories.append(Path(prefix.group(1).strip()) / "bin")
    compiler = re.search(r"^CMAKE_CXX_COMPILER:[^=]+=(.+)$", cache, re.MULTILINE)
    if compiler:
        directories.append(Path(compiler.group(1).strip()).parent)
    directories.extend(Path(path) for path in os.get_exec_path() if path)
    directories = list(dict.fromkeys(path.resolve() for path in directories if path.is_dir()))

    inspector = objdump or shutil.which("objdump")
    if not inspector:
        inspector = next((str(path / "objdump.exe") for path in directories
                          if (path / "objdump.exe").is_file()), None)
    if not inspector:
        raise RuntimeError("objdump not found; add MinGW bin to PATH or pass --objdump")

    system_dir = Path(os.environ["SystemRoot"]) / "System32"
    pending = deque(executables)
    visited = set()
    missing = set()
    copies = {}
    while pending:
        binary = pending.popleft()
        key = binary.name.lower()
        if key in visited:
            continue
        visited.add(key)
        result = subprocess.run([str(inspector), "-p", str(binary)], capture_output=True,
                                text=True, errors="replace", check=True)
        for name in re.findall(r"DLL Name:\s*(\S+)", result.stdout):
            # Windows API sets are virtual system dependencies, not distributable files.
            if name.lower().startswith(("api-ms-", "ext-ms-")):
                continue
            local = build_dir / name
            if local.is_file():
                pending.append(local)
                continue
            if (system_dir / name).is_file():
                continue
            found = next((directory / name for directory in directories
                          if (directory / name).is_file()), None)
            if found is None:
                missing.add(name)
            else:
                copies[name] = found
                pending.append(found)
    if missing:
        raise RuntimeError("Unresolved DLLs: " + ", ".join(sorted(missing))
                           + "; pass --search-dir with the matching runtime directory")
    for name, source_path in sorted(copies.items()):
        destination = build_dir / name
        if not destination.exists():
            shutil.copy2(source_path, destination)
            print(f"Copied: {name} <- {source_path}")
    print(f"DLL collection complete: {len(copies)} missing DLL(s) collected into {build_dir}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path,
                        default=Path(__file__).resolve().parent.parent / "build")
    parser.add_argument("--search-dir", action="append", default=[],
                        help="Additional DLL source directory (repeatable, searched first)")
    parser.add_argument("--objdump", help="Path to MinGW objdump executable")
    args = parser.parse_args()
    try:
        collect(args.build_dir, args.search_dir, args.objdump)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"DLL collection failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
