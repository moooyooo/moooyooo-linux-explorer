#!/usr/bin/env python3
"""Configure only this independent repository's public identity and hooks."""

import sys

sys.dont_write_bytecode = True
from check_repository import ROOT, PUBLIC_EMAIL, PUBLIC_NAME, git, repository_root


def main():
    repository_root(ROOT)
    for key, value in {
        "user.name": PUBLIC_NAME,
        "user.email": PUBLIC_EMAIL,
        "user.useConfigOnly": "true",
        "core.hooksPath": ".githooks",
    }.items():
        git(ROOT, "config", "--local", key, value)
    for name in ("pre-commit", "commit-msg", "pre-push"):
        path = ROOT / ".githooks" / name
        path.chmod(path.stat().st_mode | 0o111)
    print(f"Configured repository-local identity: {PUBLIC_NAME} <{PUBLIC_EMAIL}>")
    print("Enabled .githooks for staged content, commit messages, and pushed history.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError):
        print("Setup failed: initialize this directory as an independent Git repository first.")
        raise SystemExit(1)
