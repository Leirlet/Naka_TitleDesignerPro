# Phase 3（3D）引継ぎ資料（2026-11-01）

Phase 0〜2 はすべて Premiere 実機でユーザー確認済み。要件・決定事項・経緯の正本は `docs/requirements.md`。この資料は、別セッションで Phase 3 を始めるための要点だけをまとめたもの。

## Phase 3 の範囲（requirements.md より）
- 3D: 押し出し、ベベル、光沢（PBR）
- 方式・UI・既存機能（装飾スタック、文字単位アニメーション、縦書き・縦中横・ルビ）との組み合わせ方は**未決定**。着手前にユーザーと決める（選択肢を出して推奨を付けて聞く、が今までのやり方）

### 決めることの候補
- 描画方式: 文字アウトライン（`ID2D1Geometry`）を `Tessellate` で三角形化 → D3D11 で押し出しメッシュ＋ベベルを生成してシェーディング、など。今の描画は D3D11 + D2D1.1 + DirectWrite で、外部ライブラリは使っていない（使うならユーザーに確認）
- 3D の見た目の要素: 押し出しの深さ・色（側面のグラデーション）、ベベルの形と幅、ライト（向き・強さ）、材質（金属・光沢・粗さ）、カメラ（パース・回転）
- 2D の装飾スタック（フチ・影・座布団）と 3D をどう重ねるか（3D を 1 つの層として扱う、正面に 2D の塗り・フチを貼る、など）
- アニメーションとの関係（文字ごとの回転を 3D でも効かせるか）
- 縦書き・ルビ・縦中横は Run 単位のジオメトリなので、3D でもそのまま使える見込み（未確認）

## コードの構成
| ファイル | 内容 |
|---|---|
| `src/common/tdp_doc.h/.cpp` | 文書モデル（`.tdp` = JSON v2）。TextObject、Span、Layer（装飾スタック）、AnimSpec、`EvalAnim`、`TcyRanges` |
| `src/common/tdp_render.cpp/.h` | 描画エンジン（インポーターとデザイナーで共有）。`BuildShape` → 文字（クラスタ）ごとの `Run`（アウトライン・セル・アニメーション行列・不透明度・`paintBox`）→ 装飾スタックを前から `DESTINATION_OVER` で合成。不透明度ごとにグループ分けして重ねる |
| `src/importer/tdp_loader.cpp` | MediaCore に置く薄いローダー（`.prm`）。実装 DLL を `C:\ProgramData\NAKA\TitleDesignerPro\` から読む |
| `src/importer/tdp_importer.cpp` | 実装 DLL 本体（カスタムインポーター） |
| `src/designer/designer.cpp` + `src/designer/ui/` | デザイナー exe（WebView2 + GPU 描画キャンバス）。UI は `app.js` / `index.html` / `style.css` |
| `tools/render_test.cpp` | `tdp_render_test.exe <tdp> <out.bmp> <frame> <width>`（単体描画） |
| `tools/init_test.cpp` | 配置済み実装 DLL の imInit を呼ぶテスト |
| `tools/install_loader.ps1` | ローダーを MediaCore に入れる（管理者権限が必要） |
| `tools/dev/sheet.py` | `python -I tools/dev/sheet.py <tdp> <out.png> <frames,カンマ区切り> [width]` で複数フレームを並べた画像を作る（`<out.png>` と同じフォルダに一時 bmp を書く） |
| `tools/dev/cdp.mjs` | デザイナーを CDP で操作・撮影する Node クライアント（`--eval "<js>" --wait <ms> --shot out.png`、ポート 9333） |
| `tests/samples/*.tdp` | 確認用サンプル（アニメ、縦書き、縦中横、ルビ、行ごとのグラデーションなど） |

## ビルド・配置・確認
- ビルド: `"C:\Program Files\Microsoft Visual Studio\18\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build E:\_Title_Designer_Pro\build --config Release [--target X]`、出力は `build\bin\Release`
- 配置: Premiere が終了していることを確かめてから、`NAKAfx_TitleDesignerPro_impl.dll`・`NAKA_TitleDesignerPro.exe`・`ui\*` を `C:\ProgramData\NAKA\TitleDesignerPro\` にコピーし、ハッシュで一致を確かめる（管理者権限は不要）
- ローダー（MediaCore の `.prm`）を変えるときだけ UAC が要る。実行前にユーザーの了承を取る
- デザイナー単体: 環境変数 `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--remote-debugging-port=9333` を付けて `NAKA_TitleDesignerPro.exe --file <tdp>` を起動し、`tools/dev/cdp.mjs` で操作・撮影
- 実機確認: **Premiere はユーザーにスタートメニューから起動してもらう**（下の落とし穴を参照）。ユーザーは結果を「問題なかった」などで返してくれる

## 落とし穴（必読）
- **Claude デスクトップアプリはパッケージアプリ**。Claude から AppData（Local / LocalLow / Roaming）や HKCU に書いたものは `%LOCALAPPDATA%\Packages\Claude_pzs8sxrjxfjjc\LocalCache\` に振り替えられ、ユーザーが起動した Premiere からは見えない。そのため配置先を ProgramData にした。Premiere に見せたいファイル・レジストリを AppData / HKCU に書かない。Claude から起動した Premiere での確認は当てにならない
- プラグインキャッシュ: imInit は `malNoError`（キャッシュしない）を返す。`imIsCacheable` に戻すと「ファイル > 新規」が消える
- 新規メニューが消えたら、まず `%TEMP%\NAKA_TDP_importer.log` の `loader:` 行を見る（DLL が見えているか、どの依存 DLL が読めないかを記録する）
- 描画の確認は `tdp_render_test` → 画像を見る。数値だけで「直った」と書かない
- `getenv` などのデバッグ出力を入れたら必ず消す（C4996 警告が出る）

## ユーザーの決まり（全体の CLAUDE.md より抜粋）
- 返答は日本語
- 本名を使わない（NAKA / `NAKAfx_` / カテゴリ `NAKAfx`）
- フォント・素材・ツールのダウンロード、サードパーティ製ライブラリの使用、UAC が要る操作は事前に確認
- ユーザーがアプリで手作業中は、そのアプリを操作しない
- 未確認のことを「できる」「直った」と書かない
- 既存ファイルを Python のテキストモードで書き換えない（CRLF が崩れる）
