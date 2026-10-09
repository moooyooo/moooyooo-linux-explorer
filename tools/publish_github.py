#!/usr/bin/env python3
"""Publish the reviewed main branch and seed backlog issues using GitHub CLI."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
OWNER = "moooyooo"
REPOSITORY = OWNER + "/moooyooo-linux-explorer"
URL = "https://github.com/" + REPOSITORY
REMOTE_URL = URL + ".git"


def run(*args, check=True, visible=False):
    environment = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", GH_HOST="github.com")
    # GitHub CLI handles credentials; never extract them into files or arguments.
    result = subprocess.run(args, cwd=ROOT, env=environment, text=True,
                            stdout=None if visible else subprocess.PIPE,
                            stderr=None if visible else subprocess.PIPE)
    if check and result.returncode:
        raise RuntimeError(f"{args[0]} の処理に失敗しました。認証・接続・直前の検査結果を確認してください。")
    return result


def git(*args):
    return run("git", *args).stdout.strip()


def api(endpoint, missing_ok=False):
    result = run("gh", "api", "--hostname", "github.com", endpoint, check=False)
    try:
        data = json.loads(result.stdout)
    except (ValueError, TypeError):
        raise RuntimeError("GitHubへの接続・認証を確認してください。応答を取得できません。") from None
    if result.returncode:
        if missing_ok and isinstance(data, dict) and str(data.get("status")) == "404":
            return None
        raise RuntimeError("GitHub APIに接続できないか、操作権限がありません。")
    return data


def issue_drafts():
    result = []
    for path in sorted((ROOT / "docs" / "issues").glob("EXP-*.md")):
        content = path.read_text(encoding="utf-8")
        identifier = path.stem
        if not re.fullmatch(r"EXP-\d{3}", identifier):
            raise RuntimeError("Issue原稿のIDが不正です。")
        title = content.splitlines()[0].removeprefix("# ")
        marker = f"<!-- mlex-backlog:{identifier} -->"
        if not title.startswith(f"[{identifier}] ") or marker not in content:
            raise RuntimeError("Issue原稿の見出し・識別子が一致しません。")
        result.append((path, title, marker))
    if not result:
        raise RuntimeError("Issue原稿がありません。")
    return result


def local_checks():
    if Path(git("rev-parse", "--show-toplevel")).resolve() != ROOT:
        raise RuntimeError("このアプリの独立リポジトリで実行してください。")
    if git("branch", "--show-current") != "main":
        raise RuntimeError("公開対象はmainブランチです。")
    if git("status", "--porcelain"):
        raise RuntimeError("未コミットの変更があります。内容を確認してコミットしてから実行してください。")
    if git("config", "--get", "core.hooksPath") != ".githooks":
        raise RuntimeError("python3 tools/setup_git.py で検査フックを有効にしてください。")
    for mode in ("worktree", "staged", "history"):
        run(sys.executable, "tools/check_repository.py", "--" + mode, "--identity", visible=True)
    run(sys.executable, "tools/check_spec.py", visible=True)
    run(sys.executable, "-m", "unittest", "discover", "-s", "tests", "-v", visible=True)
    run("cmake", "--preset", "dev", visible=True)
    run("cmake", "--build", "--preset", "dev", "--parallel", "4", visible=True)
    run("ctest", "--preset", "dev", "--output-on-failure", visible=True)
    issue_drafts()


def validate_repository(repository):
    if (repository.get("full_name") != REPOSITORY or repository.get("private") is not False or
            repository.get("visibility") != "public" or repository.get("fork") or repository.get("archived")):
        raise RuntimeError("公開先の所有者・名前・Public設定を確認してください。既存設定は変更しません。")
    if not repository.get("has_issues"):
        raise RuntimeError("公開先のIssuesを有効にしてください。")


def check_remote():
    if "origin" not in git("remote").splitlines():
        return False
    for options in (("--all",), ("--push", "--all")):
        urls = git("remote", "get-url", *options, "origin").splitlines()
        if urls != [REMOTE_URL]:
            raise RuntimeError("originの送信先が公開対象と一致しません。URLを表示せず停止しました。")
    return True


def seed_issues():
    existing = []
    page = 1
    while True:
        items = api(f"repos/{REPOSITORY}/issues?state=all&per_page=100&page={page}")
        if not isinstance(items, list):
            raise RuntimeError("Issue一覧の応答が不正です。")
        existing.extend(item for item in items if "pull_request" not in item)
        if len(items) < 100:
            break
        page += 1
    for path, title, marker in issue_drafts():
        match = next((item for item in existing if marker in (item.get("body") or "")), None)
        if match:
            print("登録済み: " + match["html_url"], flush=True)
        else:
            run("gh", "issue", "create", "--repo", "github.com/" + REPOSITORY,
                "--title", title, "--body-file", str(path), visible=True)


def publish():
    if not shutil.which("gh"):
        raise RuntimeError("GitHub CLIがありません。通常のターミナルで sudo apt-get install gh を実行してください。")
    if api("user").get("login") != OWNER:
        raise RuntimeError("GitHub CLIをmoooyoooで認証してください。別アカウントでは公開しません。")
    has_remote = check_remote()
    repository = api("repos/" + REPOSITORY, missing_ok=True)
    if repository is None:
        run("gh", "repo", "create", REPOSITORY, "--public", "--disable-wiki",
            "--description", "Windows Explorerの操作感を目指す、Qt Widgets製のLinux向けMDIファイラ", visible=True)
        repository = api("repos/" + REPOSITORY)
    validate_repository(repository)
    if not has_remote:
        run("git", "remote", "add", "origin", REMOTE_URL)
    # Supply gh's credential helper only to this command, without changing global Git settings.
    # Push exactly main, retain all its original commits, and keep the pre-push hook enabled.
    run("git", "-c", "credential.helper=", "-c", "credential.helper=!gh auth git-credential",
        "push", "--set-upstream", "origin", "main:main", visible=True)
    expected = git("rev-parse", "HEAD")
    actual = api(f"repos/{REPOSITORY}/git/ref/heads/main")["object"]["sha"]
    if actual != expected:
        raise RuntimeError("GitHubのmainとローカルHEADが一致しません。公開先を確認してください。")
    seed_issues()
    print(f"公開とIssue登録を確認しました: {URL}\nmain: {expected}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check-only", action="store_true", help="ローカル検査だけを実行し、通信・公開しない")
    args = parser.parse_args()
    local_checks()
    if args.check_only:
        print(f"ローカル検査成功。公開予定: {URL} (Public)、Issue原稿: {len(issue_drafts())}件")
    else:
        publish()
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError, KeyError) as error:
        print(f"停止: {error}", file=sys.stderr)
        raise SystemExit(1)
