# moooyooo Linux Explorer

Windowsのエクスプローラーに慣れた人が使える、Linux向けのMDIファイラです。業務ごとに独立した親ウィンドウを起動し、その中で開いたフォルダ群と配置を「プロジェクト」として保存・復元します。

**現在はフォルダ閲覧とプロジェクト保存が動く初期版です。** 複数のMDI子、独立した業務ウィンドウ、フォルダツリー・詳細／アイコン表示、名前検索、MDI／タブ切り替え、配置・表示設定の保存と復元を実装しています。コピー・移動・削除・名前変更・Undo・プレビューは未実装です。

## 起動

このリポジトリのディレクトリで実行します。依存パッケージの導入は通常のターミナルで行います。

```sh
sh tools/install_deps.sh
python3 tools/doctor.py
cmake --preset dev
cmake --build --preset dev --parallel 4
ctest --preset dev
sh tools/run.sh
```

導入済みならインストールは不要です。`run.sh` は差分をビルドして起動します。実行ファイルと生成物はリポジトリ外の `../.build/moooyooo-linux-explorer/dev/` に配置します。

架空の資料・成果物フォルダを3つ開くデモは次で起動します。デモのファイルと状態は一時領域に作り、終了後も再確認用に残します。

```sh
python3 tools/demo.py
```

Ctrl+Nで現在の業務に子を追加、Ctrl+Alt+Nで別の業務ウィンドウを開きます。Ctrl+Sでプロジェクト保存、Ctrl+Oで保存済みのプロジェクトを別ウィンドウに開きます。「ウィンドウ」メニューから左右・上下・格子・重ね合わせ・タブ表示を選べます。保存先はリポジトリ外にしてください。

```sh
sh tools/run.sh --project /tmp/example.mlex-project.json
sh tools/run.sh -- /tmp/folder-a /tmp/folder-b
```

GUI接続を制限された実行環境ではデスクトップに表示できません。その場合はLinuxの通常のターミナルで起動してください。offscreen検証の成功はWayland／X11の実機操作確認を意味しません。

## 決定事項

| 項目 | 採用方針 |
|---|---|
| 開発環境 | C++20、Qt 6.8以降のQt Widgets、CMake、Ninja。IDEはQt Creatorを推奨 |
| 対象 | Linux x86_64、Wayland / X11。初期検証基準はUbuntu 26.04 LTS |
| 複数業務 | 親ウィンドウごとに独立プロセス・独立プロジェクト |
| フォルダ表示 | QMdiArea内に複数のフォルダ子ウィンドウ。自由配置・整列・タブ表示 |
| Ctrl+N | **現在の親の中に子ウィンドウを追加**。利用者の指定によるExplorerとの差分 |
| 親の追加 | メニュー「新しい業務ウィンドウ」またはCtrl+Alt+N |
| 保存 | JSON形式。フォルダの場所・順序・配置・表示設定を保存。ファイル本体は含めない |
| 公開対策 | 実データはリポジトリ外。コミット前はステージ、プッシュ前は履歴も検査 |

Qtには標準のMDI領域と子ウィンドウがあり、この要件に適しています。軽さ・速さは設計目標として測定し、現時点では性能を実証済みとは扱いません。[QtのMDI公式資料](https://doc.qt.io/qt-6.8/qmdiarea.html)

## 仕様

1. [開発環境・選定理由](docs/01-environment.md)
2. [製品要件・画面・プロジェクトの使い方](docs/02-requirements.md)
3. [構成・プロセス・拡張方針](docs/03-architecture.md)
4. [保存形式・競合・復元](docs/04-project-format.md)
5. [ショートカット・操作対象](docs/05-shortcuts.md)
6. [コピー・移動・削除・取り消し](docs/06-file-operations.md)
7. [受け入れ条件・性能目標・開発順序](docs/07-validation-roadmap.md)
8. [Git運用・秘密情報と個人情報の検査](docs/08-publication.md)
9. [初回の検証記録](docs/09-initial-validation.md)
10. [初期実装の検証記録・制限](docs/10-implementation-validation.md)

[プロジェクトのJSON Schema](schemas/project.schema.json)と[架空のプロジェクト例](examples/sample-project.json)を正本の一部として管理します。実装と矛盾する仕様変更は、文書・スキーマ・検証を一緒に更新します。

## 仕様と公開前チェック

Python 3.11以上とGitを使います。Pythonは開発用チェックのみで、アプリの実行依存にはしません。

```sh
python3 tools/check_spec.py
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -v
python3 tools/check_repository.py --worktree
```

新しいチェックアウトでは `python3 tools/setup_git.py` でこのリポジトリだけに作者情報とフックを設定します。設定値は公開済みのGitHub名 `moooyooo` とGitHub noreplyアドレスです。システム全体のGit設定は変更しません。

ビルドの基準と導入手順は[開発環境](docs/01-environment.md)に記載しています。`release` / `asan` プリセットも用意しています。

## 公開について

GitHubへのリポジトリ作成、remote登録、pushは未実施です。公開ライセンスの選択と依存ライブラリの配布条件確認は、公開工程で確定します。Microsoftのロゴ・アイコン・プログラム資産は使用しません。
