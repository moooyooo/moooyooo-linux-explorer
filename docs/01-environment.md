# 開発環境

2026-10-09の初期決定。ここでの最低バージョンは本アプリの設計上の基準であり、各製品の最新版を意味しません。

## 採用する構成

| 項目 | 基準 | 目的 |
|---|---|---|
| 言語 | C++20 | ネイティブ実行、RAIIによる資源管理、Qtとの直接統合 |
| GUI | Qt 6.8以上・7未満、Qt Widgets | MDI、ツリー、詳細一覧、メニュー、キーボード操作 |
| Qtモジュール | Core / Gui / Widgets、テスト時のみTest | Webエンジンを含めず、依存を限定 |
| Linux連携 | GLib / GIO 2.80以上 | ごみ箱、コピーの基礎処理、キャンセル、既定アプリとの連携 |
| ビルド | CMake 3.24以上、Ninja 1.11以上、pkg-config | 再現できるコマンドラインビルド |
| コンパイラ | GCC 13以上、追加検証はClang 18以上 | C++20の共通範囲に限定 |
| IDE | Qt Creatorを推奨。エディターは自由 | Designer、デバッガー、CMake統合 |
| 品質確認 | Qt Test / CTest、clang-format、clang-tidy、ASan / UBSan | GUI操作、モデル、メモリ、不正動作 |
| 開発用スクリプト | Python 3.11以上、標準ライブラリ | Git検査と設計検証。アプリからは呼び出さない |

Qt自体の必須言語水準はC++17ですが、本アプリではC++20を選びます。Qt 6.8のAPIを基準に、利用するディストリビューションで保守されるQt 6.xのパッチ版を使います。採用時の実バージョンはビルド記録に残し、バージョン番号だけで保守状況を推定しません。[QtのC++20対応](https://doc.qt.io/qt-6.8/cpp20-overview.html)

## 選定理由

| 候補 | このアプリでの評価 | 決定 |
|---|---|---|
| C++ / Qt Widgets | 標準のQMdiArea / QMdiSubWindowを利用でき、一覧操作とC++の処理を一体で設計できる | 採用 |
| Python / PySide6 | 画面の試作は速いが、今回は実行時依存と処理の見通しを優先する | 本体には不採用 |
| GTK系 | Linux連携は魅力。今回必要なMDIの構成はQtの既存部品を利用する方が実装範囲を抑えられると判断 | GUIには不採用。GIOは利用 |
| WebベースGUI | HTMLの表現力より、デスクトップの一覧操作とMDIを優先する | 不採用 |

比較は本要件に対する設計判断です。言語・GUIごとのベンチマークを実行した結果ではありません。Qtがすべての条件で最軽量・最速とは主張しません。[Qt Widgets概要](https://doc.qt.io/qt-6.8/qtwidgets-index.html)、[MDI](https://doc.qt.io/qt-6.8/qmdiarea.html)

## 対象環境と導入

最初の検証対象はUbuntu 26.04 LTS / x86_64です。Waylandを主対象、X11も受け入れ対象とします。GNOME / KDEでフォーカス、IME、クリップボード、ごみ箱、既定アプリを検証します。他ディストリビューションとARM64は、ビルド・操作確認が済むまで対応済みと表記しません。Windows / macOSの製品対応は初版の範囲外です。

ディストリビューションのパッケージで上の基準を満たす開発環境を導入します。2026-10-09に利用者が通常のターミナルで導入スクリプトを実行し、その後に各ツールの存在とバージョンを確認しました。エージェント環境内のsudoは権限昇格禁止により実行できませんでした。

```sh
sh tools/install_deps.sh
python3 tools/doctor.py
```

導入対象はbuild-essential、cmake、ninja-build、pkg-config、qt6-base-dev、qt6-base-dev-tools、qt6-wayland、libglib2.0-dev、xvfbです。GIOは操作ワーカーのごみ箱・キャンセル処理にリンクします。GUI実行ファイルと `mlex-file-worker` は同じbinディレクトリに配置します。Qtが基準未満の場合は適合する開発イメージか公式Qt SDKを選び、手元のシステムQtを上書きしません。IDEは任意です。

CMake Presetsに `dev` / `release` / `asan` を定義しています。生成先はソースの隣の `.build/moooyooo-linux-explorer/<preset>/` です。開発ビルドは警告を有効にし、依存ライブラリの警告へ一律の `-Werror` は適用しません。依存取得をCMake構成時に自動実行せず、OSパッケージと明示したSDKを用います。リリースは動的リンクを基本とし、まず通常のLinuxパッケージ、後からAppImage等を評価します。Flatpakはアクセス権と業務フォルダ操作の整合を確認してから扱います。

## 拡張性と配布条件

UI、プロジェクト、コマンド、ファイル操作、Linux連携を別のCMakeターゲットへ分離します。最初から外部プラグインABIは公開せず、内部のインターフェースに実装を追加できる形にします。

Qtはライセンスがモジュールごとに異なります。採用するCore / Gui / Widgets / TestとGIOについて実際の配布物に含まれる条件・著作権表示を記録し、ライセンス全文と必要な再リンク等の条件を公開前に確認します。動的リンクだけで義務がすべて完了するとは扱いません。本プロジェクト自身の公開ライセンスは公開工程で選定します。[Qt公式のライセンス説明](https://doc.qt.io/qt-6.8/licensing.html)

## この作業環境の現状

導入後の確認値はGCC 15.2.0、CMake 4.2.3、Ninja 1.13.2、Qt Widgets / Test 6.10.2、GIO 2.88.0、Python 3.14.4です。C++ビルドとoffscreenでのGUI操作は検証対象になりました。デスクトップ接続はエージェント環境で拒否されるため、Wayland／X11での表示・操作は利用者の通常セッションで別途確認します。詳細は[実装の検証記録](10-implementation-validation.md)を参照してください。
