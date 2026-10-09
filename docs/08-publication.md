# Gitと公開前の確認

このアプリだけの独立リポジトリをmainブランチで作ります。作者・コミッターは `moooyooo`、メールはGitHubの公開用noreply `11898118+moooyooo@users.noreply.github.com` です。個人のメールやグローバル設定を継承しません。remoteとpushは公開の指示があるまで設定・実行しません。

## 初期設定と日常の手順

初期化済みリポジトリ／クローンで次を実行すると、このリポジトリだけのGit設定とフックを有効にします。

```sh
python3 tools/setup_git.py
python3 tools/check_spec.py
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -v
python3 tools/check_repository.py --worktree
git add -- README.md AGENTS.md docs schemas examples tools tests .gitignore .gitattributes .githooks
git diff --cached --check
git diff --cached --stat
git diff --cached
python3 tools/check_repository.py --staged --identity
git commit -m "Describe the actual change"
```

上記のadd対象は初回構成の例です。通常は変更した必要なファイルだけを指定します。生成物・検証ログ・実データをまとめて追加しません。コミット前に差分を読み、目的に関係ない変更がないか確認します。

| フック | 検査対象 |
|---|---|
| pre-commit | 作業ファイルではなくGit index内の全ファイル、author / committerの実効値 |
| commit-msg | 今回のコミットメッセージと作者情報 |
| pre-push | 全ローカル参照とpush対象からたどる履歴、過去に削除したファイル、コミット文面・作者情報、注釈付きタグ |

Gitフックはクローンしただけでは有効になりません。各開発環境でsetupを行います。ローカルフックは無効化できるためサーバー側の強制境界ではありません。公開工程でCIとリモート側の保護も追加します。失敗時に `--no-verify` やhooksPathの差し替えで回避しません。

## 検出と制限

初期チェッカーは次を検出します。

- 代表的なAPI／サービスのトークン、秘密鍵ヘッダー、引用符付きの資格情報代入、URLへの認証情報埋め込み。
- 許可したnoreplyと `.test` 以外のメール、具体的なホームパス、プライベートIP、携帯電話番号に似た文字列。
- `.env`、鍵、認証情報名、実行時プロジェクト拡張子、DB、ログ、アーカイブ、生成物、個人設定ディレクトリ。
- 未確認のファイル種別、バイナリ、非UTF-8、2MiB超のファイル、シンボリックリンク、submodule。これらは自動的に安全とみなさず停止する。
- 履歴内の作者・コミッター情報と削除済みのファイル。検査に失敗した場合もcommit / pushを通さない。

検出した値そのものは出力しません。場所・行・分類だけを報告します。実際の秘密情報をテストへ埋め込まず、検証は人工的に組み立てたダミー値を一時ディレクトリに置いて行います。

この検査だけで、自然文の氏名・住所・会社情報、未知形式の秘密、難読化したデータの不在を保証しません。文書の目視確認は毎回必要です。画像や配布物は現在のゲートでは通りません。導入する工程で内容・EXIF等のメタデータ・必要性を確認し、レビュー済みの種別と検査方法を追加します。検査対象を一括で除外して通す変更にはしません。

`.gitignore`は誤追加の予防であり、機密情報を保存してよい場所を作るものではありません。個人のデータや実プロジェクトは、無視されるパスにも置きません。開発用の一時データはリポジトリ外で生成します。

## 公開前

```sh
python3 tools/check_repository.py --history --identity
```

利用者から `moooyooo/moooyooo-linux-explorer` のPublic公開とIssue管理の指示を受けています。ライセンス選択とバイナリ配布の確認は残作業として管理し、今回の公開対象はコミットされたソース・仕様・検証コードです。将来の外部コントリビューター受け入れでは、作者をmoooyoooに偽装せず、作者情報の公開ルールとチェッカーを明示的に更新します。

もし本物の認証情報を発見した場合は、公開を止め、値をログへ再表示せず、資格情報の失効・更新と履歴の是正を行います。単に最新ファイルから削除しただけで解決した扱いにはしません。

## Public公開とIssueの初回登録

通常のLinuxターミナルで、このリポジトリのディレクトリから実行します。GitHub CLIが未導入の場合だけ導入し、ブラウザーでmoooyoooのアカウントを使って認証します。パスワードやトークンをソース、コマンド引数、チャットへ貼り付けません。

```sh
sudo apt-get install gh
gh auth login --hostname github.com --git-protocol https --web --scopes workflow
python3 tools/publish_github.py
```

GitHub CLIの認証情報はCLI／OSの資格情報管理に委ねます。`GH_CONFIG_DIR`等の設定先をリポジトリ内にしないでください。すでに認証済みでワークフロー更新権限だけが不足する場合は、通常のターミナルで `gh auth refresh --hostname github.com --scopes workflow` を使用します。[認証](https://cli.github.com/manual/gh_auth_login)、[リポジトリ作成](https://cli.github.com/manual/gh_repo_create)、[Issue作成](https://cli.github.com/manual/gh_issue_create)

公開スクリプトは、未コミット変更、作者情報、検査フック、全作業ファイル・index・過去の履歴、仕様、Pythonテスト、ビルド、CTestを確認してから通信します。認証ユーザーと公開先を固定し、既存のPrivateリポジトリや別のoriginは変更しません。mainだけを通常のpushで送り、履歴の書き換え・強制push・フック無効化は行いません。Gitの資格情報ヘルパーはpushコマンドにだけ指定します。

Issueは [原稿](../TODO.md) の識別子で全状態を検索し、登録済みなら再作成せず、本文・ラベル・完了状態を維持します。接続エラーで途中終了した場合は、表示された公開状況を確認してから同じコマンドを再実行できます。成功時にリポジトリURL、Issue URL、GitHubと一致したmainのコミットを表示します。実行前や途中失敗時は公開完了と記録しません。

GitHub Actionsの `Publication checks` は読み取り権限だけで公開内容とPythonの検証を実行します。checkoutは公式v4のコミット `11d5960a326750d5838078e36cf38b85af677262` に固定し、資格情報をGit設定へ残しません。初回push後にGitHub側で成功を確認します。C++向けCI、mainの保護、外部作者の受け入れはEXP-007で追跡します。
