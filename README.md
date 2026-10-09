# moooyooo Linux Explorer

Windowsのエクスプローラーに慣れた人が使える、Linux向けのMDIファイラです。業務ごとに独立した親ウィンドウを起動し、その中で開いたフォルダ群と配置を「プロジェクト」として保存・復元します。

**現在は仕様策定段階です。アプリ本体・GUI・ファイル操作は未実装です。** この初回リポジトリには設計、保存形式、架空のサンプル、仕様検証と公開前チェックの仕組みを収録します。

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

[プロジェクトのJSON Schema](schemas/project.schema.json)と[架空のプロジェクト例](examples/sample-project.json)を正本の一部として管理します。実装と矛盾する仕様変更は、文書・スキーマ・検証を一緒に更新します。

## この段階で実行できる確認

Python 3.11以上とGitを使います。Pythonは開発用チェックのみで、アプリの実行依存にはしません。

```sh
python3 tools/check_spec.py
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -v
python3 tools/check_repository.py --worktree
```

新しいチェックアウトでは `python3 tools/setup_git.py` でこのリポジトリだけに作者情報とフックを設定します。設定値は公開済みのGitHub名 `moooyooo` とGitHub noreplyアドレスです。システム全体のGit設定は変更しません。

GUIビルド用の依存関係と導入手順は[開発環境](docs/01-environment.md)に記載しています。現段階にビルド対象やアプリ起動コマンドはありません。

## 公開について

GitHubへのリポジトリ作成、remote登録、pushは未実施です。公開ライセンスの選択と依存ライブラリの配布条件確認は、公開工程で確定します。Microsoftのロゴ・アイコン・プログラム資産は使用しません。
