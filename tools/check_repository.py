#!/usr/bin/env python3
"""Fail closed on obvious publication hazards; never print matched secrets.

This is a local baseline, not proof that arbitrary personal information is absent.
Staged mode reads Git blobs, and history mode also checks removed historical data.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
PUBLIC_NAME = "moooyooo"
PUBLIC_EMAIL = "11898118+moooyooo@users.noreply.github.com"
MAX_BYTES = 2 * 1024 * 1024
TEXT_SUFFIXES = {".md", ".json", ".py", ".sh", ".toml", ".yml", ".yaml", ".txt", ".svg", ".ui", ".qrc", ".cpp", ".cc", ".h", ".hpp", ".cmake", ".desktop"}
TEXT_NAMES = {".gitignore", ".gitattributes", ".editorconfig", "CMakeLists.txt", "LICENSE", "NOTICE", "pre-commit", "commit-msg", "pre-push"}
FORBIDDEN_PARTS = {".ssh", ".aws", ".gnupg", ".kube", ".codex", ".agents", "secrets", "credentials", "runtime", "local-data", "captures", "build", "out", "dist", "__pycache__"}
FORBIDDEN_SUFFIXES = {".pem", ".key", ".p12", ".pfx", ".kdbx", ".db", ".sqlite", ".sqlite3", ".log", ".eml", ".mbox", ".zip", ".tar", ".gz", ".7z"}
PATTERNS = [
    ("private key", re.compile(r"-----BEGIN (?:[A-Z0-9 ]+ )?PRIVATE KEY-----")),
    ("GitHub token", re.compile(r"\b(?:gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,})\b")),
    ("cloud access key", re.compile(r"\b(?:AKIA|ASIA)[A-Z0-9]{16}\b")),
    ("API token", re.compile(r"\bsk-(?:proj-|svcacct-)?[A-Za-z0-9_-]{20,}\b")),
    ("service token", re.compile(r"\bxox[baprs]-[A-Za-z0-9-]{15,}\b")),
    ("JWT", re.compile(r"\beyJ[A-Za-z0-9_-]{12,}\.[A-Za-z0-9_-]{12,}\.[A-Za-z0-9_-]{12,}\b")),
    ("credential in URL", re.compile(r"[a-z][a-z0-9+.-]*://[^/\s:@]+:[^/\s@]+@", re.I)),
    ("personal home path", re.compile(r"/(?:home|Users)/[A-Za-z0-9_.-]+(?:/|\b)")),
    ("private network address", re.compile(r"\b(?:10(?:\.\d{1,3}){3}|192\.168(?:\.\d{1,3}){2}|172\.(?:1[6-9]|2\d|3[01])(?:\.\d{1,3}){2})\b")),
    ("phone-like number", re.compile(r"(?<!\d)0[789]0[- ]?\d{4}[- ]?\d{4}(?!\d)")),
    ("assigned credential", re.compile(r'''(?i)["']?(?:api[_-]?key|access[_-]?token|auth[_-]?token|client[_-]?secret|password|passwd|secret)["']?\s*[:=]\s*["']([^"'\s]{8,})["']''')),
]
EMAIL = re.compile(r"[A-Za-z0-9.!#$%&'*+/=?^_`{|}~-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}")


def git(repo, *args):
    result = subprocess.run(["git", "-C", str(repo), *args], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        raise RuntimeError(f"Git command failed: {args[0]}")
    return result.stdout


def repository_root(repo):
    actual = Path(os.fsdecode(git(repo, "rev-parse", "--show-toplevel").strip())).resolve()
    if actual != repo.resolve():
        raise RuntimeError("refusing to operate on a different repository")


def text_findings(text):
    findings = []
    for label, pattern in PATTERNS:
        for match in pattern.finditer(text):
            findings.append((text.count("\n", 0, match.start()) + 1, label))
    for match in EMAIL.finditer(text):
        email = match.group(0).strip("'`")
        if email == PUBLIC_EMAIL or email.rsplit("@", 1)[1].lower().endswith(".test"):
            continue
        findings.append((text.count("\n", 0, match.start()) + 1, "non-public email address"))
    return findings


def blob_findings(name, data, mode="100644"):
    path = PurePosixPath(name)
    findings = []
    if mode not in {"100644", "100755"}:
        findings.append((0, "symlink, submodule, or unsupported file mode"))
    if any(part.lower() in FORBIDDEN_PARTS for part in path.parts):
        findings.append((0, "private or generated directory"))
    if path.name.lower().startswith(".env") or path.name.lower().endswith(".mlex-project.json") or path.suffix.lower() in FORBIDDEN_SUFFIXES:
        findings.append((0, "private or runtime file type"))
    if path.name.lower() in {"credentials.json", "secrets.json", "id_rsa", "id_ed25519", "cmakeuserpresets.json", "compile_commands.json"}:
        findings.append((0, "machine-specific or credential filename"))
    if path.suffix.lower() not in TEXT_SUFFIXES and path.name not in TEXT_NAMES:
        findings.append((0, "unreviewed file type"))
    if data is None or len(data) > MAX_BYTES:
        return findings + [(0, "file exceeds review size limit")]
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        return findings + [(0, "binary or non-UTF-8 content needs review")]
    if "\0" in text:
        findings.append((0, "binary content needs review"))
    findings.extend(text_findings(text))
    return findings


def scan_worktree(repo):
    findings, count = [], 0
    for base, dirs, files in os.walk(repo, followlinks=False):
        dirs[:] = [d for d in dirs if d != ".git"]
        for directory in list(dirs):
            path = Path(base) / directory
            if path.is_symlink():
                findings.append((str(path.relative_to(repo)), 0, "symlink directory"))
                dirs.remove(directory)
        for filename in files:
            path = Path(base) / filename
            name = str(path.relative_to(repo))
            if path.is_symlink():
                data = os.fsencode(os.readlink(path))
            else:
                with path.open("rb") as stream:
                    data = stream.read(MAX_BYTES + 1)
            mode = "120000" if path.is_symlink() else "100644"
            findings.extend((name, line, reason) for line, reason in blob_findings(name, data, mode))
            count += 1
    return findings, count


def bounded_blob(repo, oid):
    if int(git(repo, "cat-file", "-s", oid)) > MAX_BYTES:
        return None
    return git(repo, "cat-file", "blob", oid)


def scan_staged(repo):
    findings, count = [], 0
    # All index entries, not only changed working-tree paths: catches forced adds.
    for entry in git(repo, "ls-files", "--stage", "-z").split(b"\0"):
        if not entry:
            continue
        metadata, raw_name = entry.split(b"\t", 1)
        mode, oid, stage = metadata.decode("ascii").split()
        name = os.fsdecode(raw_name)
        if stage != "0":
            findings.append((name, 0, "unmerged index entry"))
            continue
        data = b"" if mode == "160000" else bounded_blob(repo, oid)
        findings.extend((name, line, reason) for line, reason in blob_findings(name, data, mode))
        count += 1
    return findings, count


def check_identity(repo):
    findings = []
    for kind in ("AUTHOR", "COMMITTER"):
        value = git(repo, "var", f"GIT_{kind}_IDENT").decode("utf-8").strip()
        if not value.startswith(f"{PUBLIC_NAME} <{PUBLIC_EMAIL}> "):
            findings.append(("Git identity", 0, f"unexpected {kind.lower()} name or email"))
    return findings


def scan_history(repo, extra_tips=()):
    findings, seen, count = [], set(), 0
    commits = git(repo, "rev-list", "--all", *extra_tips).decode("ascii").splitlines()
    for commit in commits:
        metadata = git(repo, "show", "-s", "--format=%an%x00%ae%x00%cn%x00%ce%x00%B", commit).decode("utf-8")
        author, author_email, committer, committer_email, message = metadata.split("\0", 4)
        if (author, author_email, committer, committer_email) != (PUBLIC_NAME, PUBLIC_EMAIL, PUBLIC_NAME, PUBLIC_EMAIL):
            findings.append((f"commit {commit[:12]}", 0, "unexpected public author or committer identity"))
        findings.extend((f"commit {commit[:12]} message", line, reason) for line, reason in text_findings(message))
        for entry in git(repo, "ls-tree", "-r", "-z", commit).split(b"\0"):
            if not entry:
                continue
            metadata, raw_name = entry.split(b"\t", 1)
            mode, kind, oid = metadata.decode("ascii").split()
            name = os.fsdecode(raw_name)
            key = (mode, oid, name)
            if key in seen:
                continue
            seen.add(key)
            data = bounded_blob(repo, oid) if kind == "blob" else b""
            findings.extend((f"{commit[:12]}:{name}", line, reason) for line, reason in blob_findings(name, data, mode))
            count += 1
    # Annotated tag messages and tagger metadata can also carry private data.
    tag_targets = list(extra_tips)
    for ref in git(repo, "for-each-ref", "--format=%(refname) %(objecttype)").decode("utf-8").splitlines():
        name, kind = ref.rsplit(" ", 1)
        findings.extend(("Git reference", line, reason) for line, reason in text_findings(name))
        if kind not in {"commit", "tag"}:
            findings.append(("Git reference", 0, "non-commit reference needs explicit review"))
        if kind == "tag":
            tag_targets.append(name)
    seen_tags = set()
    for target in tag_targets:
        oid = git(repo, "rev-parse", target).decode("ascii").strip()
        while git(repo, "cat-file", "-t", oid).strip() == b"tag":
            if oid in seen_tags:
                break
            seen_tags.add(oid)
            if int(git(repo, "cat-file", "-s", oid)) > MAX_BYTES:
                raise ValueError("tag exceeds inspection limit")
            content = git(repo, "cat-file", "tag", oid).decode("utf-8")
            findings.extend(("annotated tag", line, reason) for line, reason in text_findings(content))
            oid = content.splitlines()[0].removeprefix("object ")
        if git(repo, "cat-file", "-t", target + "^{}").strip() != b"commit":
            findings.append(("push object", 0, "push target must ultimately refer to a commit"))
    return findings, count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group(required=True)
    for name in ("worktree", "staged", "history", "push"):
        modes.add_argument(f"--{name}", action="store_true")
    modes.add_argument("--message", type=Path)
    parser.add_argument("--identity", action="store_true")
    args = parser.parse_args()
    try:
        if not args.worktree or args.identity:
            repository_root(ROOT)
        if args.worktree:
            findings, count = scan_worktree(ROOT)
        elif args.staged:
            findings, count = scan_staged(ROOT)
        elif args.message:
            with args.message.open("rb") as stream:
                data = stream.read(MAX_BYTES + 1)
            if len(data) > MAX_BYTES:
                raise ValueError("commit message exceeds size limit")
            findings = [("commit message", line, reason) for line, reason in text_findings(data.decode("utf-8"))]
            count = 1
        else:
            tips = []
            if args.push:
                for line in sys.stdin:
                    fields = line.split()
                    if len(fields) != 4 or not re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", fields[1]):
                        raise ValueError("invalid pre-push input")
                    if set(fields[1]) != {"0"}:
                        tips.append(fields[1])
            findings, count = scan_history(ROOT, tips)
        if args.identity:
            findings.extend(check_identity(ROOT))
        for name, line, reason in findings:
            print(f"BLOCK {name}:{line}: {reason}")
        print(f"Reviewed {count} files or historical file versions; {len(findings)} findings.")
        return 1 if findings else 0
    except (OSError, RuntimeError, ValueError, UnicodeError) as error:
        # Do not fall back to allowing the commit/push when inspection fails.
        print(f"BLOCK: repository inspection failed ({type(error).__name__}).")
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
