# C++ glue 3 ファイルの重複を共通化する

- Created: 2026-08-04
- Completed: {YYYY-MM-DD}
- Branch: feature/refactor-merge-glue-common
- Polished: {YYYY-MM-DD}

## 目的

`quiche_glue.cc` / `quiche_glue_http3.cc` / `quiche_glue_wt.cc` の重複実装を共通化し、挙動分岐の発生を防ぐ。

## 現状

3 ファイルに完全同一またはほぼ同一の実装が重複している:

- `BytesView()`（`quiche_glue.cc` と `quiche_glue_wt.cc`）
- `MapDatagramStatus()`（`quiche_glue.cc` と `quiche_glue_wt.cc`）
- SessionVisitor 実装（`quiche_glue.cc` の `GoogleQuichePySessionVisitor` と `quiche_glue_wt.cc` の `WtSessionVisitor` は `Reset()` の有無以外同一）
- `NextSend()` / `NextTimeoutMs()` / `HandleTimeout()`（3 ファイルでほぼ同一）
- `EnsureClient()` / `SetErrorIfEmpty()` / `SetError()` / `ResetRuntimeError()`（3 ファイルで同一）

コピペのリスクは既に顕在化しており、挙動分岐が実在する:

- エラーメッセージの差異（`"buffer length must be greater than zero"` と `"buffer_len must be greater than zero"`）
- `ReadStream()` の構造差（raw は fin-only ケースを `SkipBytes(0)` で処理、wt は分岐なし。raw のみ `bytes_read == 0 && !fin` のフォールバックを持つ。wt のみ `buffer == nullptr` チェックを持つ）
- `NextSend()` の `ResetRuntimeError()` 有無

`quiche_glue_sans_io.h` という共有基盤が既にあるため、「設計上避けられない重複」とは言えない。

## 設計方針

- `BytesView()` / `MapDatagramStatus()` / SessionVisitor を `quiche_glue_sans_io.h`（または新設の共通ヘッダ）へ移動する
- `NextSend()` / `NextTimeoutMs()` / `HandleTimeout()` を共通化する（SendQueue とアラームファクトリへのアクセサを引数に取る形等）
- `quiche_glue_exports.h` の内容を `quiche_glue.h` へ統合し、ファイルを削除する（全ファイルが既に `quiche_glue.h` を include している）

## 完了条件

- 3 ファイル間の重複が共通ヘッダに集約され、挙動分岐が解消されること
- 既存テストがすべて通ること

## 解決方法

- `quiche_glue_sans_io.h` へ共通実装を集約する
- `ReadStream()` の構造差（fin-only 処理、nullptr チェック、フォールバック）は共通実装に統一する
  - raw 版の `result.bytes_read == 0 && !result.fin` のフォールバックは `PeekNextReadableRegion()` で `has_data()` を確認済みのため到達不能。削除する
- `quiche_glue_exports.h` を `quiche_glue.h` に統合して削除する
- ビルド（Bazel + CMake）とテストで回帰がないことを確認する
