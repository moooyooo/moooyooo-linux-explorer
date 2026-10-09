#!/usr/bin/env python3
"""Report public development-tool versions without collecting host details."""
import shutil
import subprocess


def main():
    checks = {
        "C++": ["g++", "-dumpfullversion"],
        "CMake": ["cmake", "--version"],
        "Ninja": ["ninja", "--version"],
        "Qt Widgets": ["pkg-config", "--modversion", "Qt6Widgets"],
        "Qt Test": ["pkg-config", "--modversion", "Qt6Test"],
        "GIO (M3)": ["pkg-config", "--modversion", "gio-2.0"],
    }
    ok = True
    for label, command in checks.items():
        if not shutil.which(command[0]):
            print(f"MISSING {label}")
            ok = False
            continue
        result = subprocess.run(command, text=True, capture_output=True)
        if result.returncode:
            print(f"MISSING {label}")
            ok = False
        else:
            print(f"{label}: {result.stdout.splitlines()[0]}")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
