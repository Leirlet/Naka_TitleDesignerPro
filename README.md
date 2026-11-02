# NAKA Title Designer Pro

Premiere Pro 2026 以降（Windows）用のテロップデザイナー。Premiere のカスタムインポーターとして `.tdp` を読み込み、フレームを GPU（D3D11 + Direct2D + DirectWrite）で描画する。編集は別プロセスのデザイナー（WebView2 の UI + GPU 描画キャンバス）で行う。

- 装飾スタック（塗り・フチ・影・内側の影・ロングシャドウ・座布団）、グラデーション・模様、文字ごとの設定、カーニング
- 文字単位のアニメーション（登場・退場のプリセット＋調整）
- 縦書き・縦中横・ルビ
- 3D（押し出し・ベベル・ライトと材質・3D 回転）

仕様と経緯は [docs/requirements.md](docs/requirements.md)。

## ビルドに必要なもの
- Visual Studio 2026（C++ デスクトップ開発、同梱の CMake 3.24 以上）
- Windows SDK（シェーダーのコンパイルに `fxc.exe` を使う）
- Adobe Premiere Pro 26.0 C++ SDK（リポジトリには含まない）。既定の場所は `C:/tools/Premiere Pro 26.0 C++ SDK Windows/Premiere Pro 26.0 C++ SDK`。別の場所なら CMake の `PR_SDK` で指定する
- WebView2 SDK（リポジトリには含まない）: NuGet パッケージ `Microsoft.Web.WebView2` 1.0.4314.35 を取得し、`.nupkg` を zip として `third_party/webview2/pkg/` に展開する（`third_party/webview2/pkg/build/native/include` と `.../x64/WebView2LoaderStatic.lib` があればよい）

## ビルド
```
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```
出力は `build/bin/Release`。

| 出力 | 内容 |
|---|---|
| `NAKAfx_TitleDesignerPro.prm` | MediaCore に置くローダー |
| `NAKAfx_TitleDesignerPro_impl.dll` | インポーター本体 |
| `NAKA_TitleDesignerPro.exe` + `ui/` | デザイナー |
| `tdp_render_test.exe` | `.tdp` を 1 フレーム描いて bmp に書く確認用ツール |

## 配置
1. ローダーを `C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\` に置く（管理者権限が必要。`tools/install_loader.ps1`）。最初の一回と、ローダーを変えたときだけ
2. `NAKAfx_TitleDesignerPro_impl.dll`・`NAKA_TitleDesignerPro.exe`・`ui/` を `C:\ProgramData\NAKA\TitleDesignerPro\` に置く（Premiere を終了してから）
3. Premiere を起動すると「ファイル > 新規」にメニューが出る

ログは `%TEMP%\NAKA_TDP_importer.log`。

## 確認用
- `tests/samples/*.tdp`: 機能ごとの確認用サンプル
- `tools/dev/sheet.py`: 複数フレームを並べた画像を作る（`tdp_render_test` を使う）
- `tools/dev/cdp.mjs`: デザイナーを Chrome DevTools Protocol で操作・撮影する（デザイナーを環境変数 `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--remote-debugging-port=9333` 付きで起動しておく）
