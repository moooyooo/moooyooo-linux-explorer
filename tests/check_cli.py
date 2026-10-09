#!/usr/bin/env python3
"""Integration checks for the real executable, using isolated synthetic state."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    executable = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="mlex-process-", dir="/tmp") as folder:
        root = Path(folder)
        environment = os.environ.copy()
        environment["QT_QPA_PLATFORM"] = "offscreen"
        for kind in ("STATE", "DATA", "CONFIG", "CACHE", "RUNTIME"):
            path = root / kind.lower()
            path.mkdir(mode=0o700)
            environment[f"XDG_{kind}_HOME" if kind != "RUNTIME" else "XDG_RUNTIME_DIR"] = str(path)
        commands = [(["--unknown"], 2), (["--project", "missing", "folder"], 2), (["--project", "missing"], 1)]
        for arguments, expected in commands:
            result = subprocess.run([executable, *arguments], cwd=root, env=environment, capture_output=True, timeout=5)
            check(result.returncode == expected, "CLI exit status is incorrect")
        invalid = root / "invalid.json"
        invalid.write_text(json.dumps({"schema_version": 999}), encoding="utf-8")
        result = subprocess.run([executable, "--project", str(invalid)], cwd=root, env=environment, capture_output=True, timeout=5)
        check(result.returncode == 1, "Invalid project was accepted")
        strange = root / "-folder with 'quotes' & symbols"
        strange.mkdir()
        processes = []
        try:
            for _ in range(2):
                processes.append(subprocess.Popen([executable, "--", strange.name], cwd=root, env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
            deadline = time.monotonic() + 5
            locks = root / "state" / "moooyooo-linux-explorer" / "recovery"
            while time.monotonic() < deadline:
                if len(list(locks.glob("*.lock"))) == 2:
                    break
                check(all(p.poll() is None for p in processes), "A GUI process exited during startup")
                time.sleep(0.05)
            check(len(list(locks.glob("*.lock"))) == 2, "Independent workspaces did not initialize")
            processes[0].terminate()
            processes[0].wait(timeout=5)
            check(processes[1].poll() is None, "Closing one process terminated the other")
        finally:
            for process in processes:
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=5)
    print("PASS CLI errors, literal paths, and two independent GUI processes")


if __name__ == "__main__":
    main()
