# TODOとIssue管理

現在の実装は0.2.0です。残作業は下表で管理し、詳細な受け入れ条件は各原稿に記載します。GitHubへの初回登録には `python3 tools/publish_github.py` を使用します。公開スクリプトの実行前は、原稿の存在をIssue登録済みとは扱いません。

| ID | 優先度 | 状態 | 作業・Issue原稿 | GitHub検索 |
|---|---|---|---|---|
| EXP-001 | P1 | 未着手 | [実デスクトップでの受け入れ確認](docs/issues/EXP-001.md) | [Issue](https://github.com/moooyooo/moooyooo-linux-explorer/issues?q=is%3Aissue+EXP-001+in%3Atitle) |
| EXP-002 | P1 | 未着手 | [ファイル操作の障害・競合検証](docs/issues/EXP-002.md) | [Issue](https://github.com/moooyooo/moooyooo-linux-explorer/issues?q=is%3Aissue+EXP-002+in%3Atitle) |
| EXP-003 | P2 | 未着手 | [最近使ったプロジェクトと場所の移動](docs/issues/EXP-003.md) | [Issue](https://github.com/moooyooo/moooyooo-linux-explorer/issues?q=is%3Aissue+EXP-003+in%3Atitle) |
| EXP-004 | P2 | 未着手 | [限定プレビューとドラッグ転送](docs/issues/EXP-004.md) | [Issue](https://github.com/moooyooo/moooyooo-linux-explorer/issues?q=is%3Aissue+EXP-004+in%3Atitle) |
| EXP-005 | P1 | 未着手 | [ライセンスと配布パッケージ](docs/issues/EXP-005.md) | [Issue](https://github.com/moooyooo/moooyooo-linux-explorer/issues?q=is%3Aissue+EXP-005+in%3Atitle) |
| EXP-006 | P2 | 未着手 | [大量ファイル・多数の内部ファイラの性能測定](docs/issues/EXP-006.md) | [Issue](https://github.com/moooyooo/moooyooo-linux-explorer/issues?q=is%3Aissue+EXP-006+in%3Atitle) |
| EXP-007 | P1 | 未着手 | [C++ビルドCIとブランチ保護](docs/issues/EXP-007.md) | [Issue](https://github.com/moooyooo/moooyooo-linux-explorer/issues?q=is%3Aissue+EXP-007+in%3Atitle) |

P1は日常利用版・配布前の優先作業、P2は続く改善です。着手・完了時はこの表と対応するGitHub Issueの状態を更新します。完了には関連コミットと検証結果を記録します。Issueの詳細は登録後のGitHub側を正本とし、登録用原稿で利用者の追記や完了状態を上書きしません。

新規の不具合・機能要望はGitHubのIssueテンプレートを使います。公開する再現例、ログ、画像には架空データを用い、ホームの実パス、業務名、認証情報を含めません。
