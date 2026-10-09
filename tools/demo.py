#!/usr/bin/env python3
"""Open a reproducible GUI demo using only synthetic files outside repositories."""
import argparse
import copy
import json
import os
from pathlib import Path
import subprocess
import tempfile
import uuid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prepare-only", action="store_true", help="Print the generated project location without launching")
    args = parser.parse_args()
    os.umask(0o077)
    source = Path(__file__).resolve().parents[1]
    directory = Path(tempfile.mkdtemp(prefix="mlex-demo-", dir="/tmp"))
    names = ["業務サンプル/資料", "業務サンプル/成果物", "別業務/受信資料"]
    for name in names:
        folder = directory / name
        folder.mkdir(parents=True)
        (folder / "アーカイブ").mkdir()
        for filename in ["01_作業メモ.txt", "02_予定表.txt", "10_確認事項.txt"]:
            (folder / filename).write_text("画面確認用の架空データです。実際の業務情報は含みません。\n", encoding="utf-8")
    project = json.loads((source / "examples/sample-project.json").read_text(encoding="utf-8"))
    template = project["panes"][0]
    project["project_id"] = str(uuid.uuid4())
    project["name"] = "デモ業務 — 資料と成果物"
    project["window"] = {"width": 1440, "height": 900, "maximized": False}
    project["layout"] = {"mode": "mdi", "arrangement": "tile_grid"}
    project["panes"] = []
    for index, name in enumerate(names):
        pane = copy.deepcopy(template)
        pane["pane_id"] = str(uuid.uuid4())
        pane["location"] = {"kind": "absolute", "path": str(directory / name)}
        pane["rect"] = (
            {"x": 0, "y": 0, "width": 0.5, "height": 1}
            if index == 0
            else {"x": 0.5, "y": (index - 1) * 0.5, "width": 0.5, "height": 0.5}
        )
        project["panes"].append(pane)
    project["active_pane_id"] = project["panes"][0]["pane_id"]
    path = directory / "demo.mlex-project.json"
    path.write_text(json.dumps(project, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    if args.prepare_only:
        print(path)
        return 0
    # Keep the sample directory after exit so saved demo projects remain usable.
    environment = os.environ.copy()
    for kind in ("STATE", "DATA", "CONFIG", "CACHE"):
        environment[f"XDG_{kind}_HOME"] = str(directory / kind.lower())
    return subprocess.call(["sh", str(source / "tools/run.sh"), "--project", str(path)], env=environment)


if __name__ == "__main__":
    raise SystemExit(main())
