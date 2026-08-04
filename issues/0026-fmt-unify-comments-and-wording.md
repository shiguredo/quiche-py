# 文言・コメント・型の細部を統一する

- Created: 2026-08-04
- Completed: {YYYY-MM-DD}
- Branch: feature/fix-unify-comments-and-wording
- Polished: {YYYY-MM-DD}

## 目的

レビューで指摘された軽微な文言・コメント・型の不統一を解消する。

## 現状

- `src/bindings/module.cpp` のエラーメッセージ `"max_bytes exceed the maximum allowed size"` は文法ミス（`exceeds` が正しい）
- `src/bindings/quiche_glue.h` のセクション見出しコメント `/* --- raw QUIC (既存) --- */` / `/* --- HTTP/3 --- */` / `/* --- WebTransport over HTTP/3 --- */` が英語で、「既存」は stale（コメントは日本語の規約）
- `module.cpp` の nanobind ドキュメント文字列（`m.doc()` / 各 `.def()` の説明）が英語のまま。Python 側（`src/quiche/*.py`）の docstring は日本語で統一されており、公開 API のドキュメント言語が揃っていない
- `tests/test_free_threading.py` の skip メッセージと assert メッセージが英語（テストのログメッセージは日本語の規約。`conftest.py` の skip は日本語で統一済み）
- `tests/prop_async_client.py` のコメント「同一エコーサーバーを例間で使い回すのは意図的」が誤り（`echo_server` は関数スコープフィクスチャで、hypothesis の例ごとに再起動される）
- `src/quiche/quiche_ext.pyi` の `connect_session` の `headers` 型が `object | None` のまま（`submit_request` は `list[tuple[bytes, bytes]]` に型付け済み。stubgen_patterns.txt にパターンが無いため）
- `src/bindings/quiche_glue.cc` の `NextSend()` だけ `ResetRuntimeError()` を呼ばず、`last_error` に古いエラーが残留する（3 実装共通）
- `quiche_glue_wt.cc` の `SendDatagram()` は成功時に `last_error` をクリアしない非対称
- `src/bindings/quiche_glue.cc` の `ReadStream()` は `buffer == nullptr` を検証しない（wt 版にはある）
- `CMakeLists.txt` の `QUICHE_ROOT` の説明文が 3 回重複
- `verify_peer=False` 時に使う上流の `FakeProofVerifier`（3 クライアント共通）について、採用理由（QUICHE 標準の検証スキップ機構でありモックではないこと）のコメントがコードに無い
- `src/bindings/module.cpp` の `ThrowStatus()` 末尾の `throw std::runtime_error` は到達不能（`[[noreturn]]` の契約上 `NB_UNREACHABLE` にするのが安全）
- `aio.py` にのみ存在する `_reschedule_timer` / `_drain_send` の到達不能 `RuntimeError`（`_process()` / `close()` が先にガードするため）と、`error_received` のコメント、`AsyncQuicStream.read` の docstring

## 設計方針

挙動を変えない範囲で、文言・コメント・型・エラー処理の細部を統一する。1 件ずつ小さな変更の積み重ね。

## 完了条件

- 上記の不統一がすべて解消されること
- 既存テストがすべて通ること

## 解決方法

- エラーメッセージの文法修正（`exceeds`）
- C++ コメントの日本語化（セクション見出しの「既存」除去を含む）
- nanobind docstring の言語方針を決定して統一（日本語化または英語のままとする旨を README 等に明記）
- `test_free_threading.py` のメッセージ日本語化
- `prop_async_client.py` のコメント修正（またはセッションスコープ化の検討）
- stubgen_patterns.txt に `connect_session` のパターンを追加して pyi の型を精密化
- `NextSend()` への `ResetRuntimeError()` 追加と `SendDatagram()` の成功時クリア統一
- `ReadStream()` の `buffer == nullptr` 検証追加（wt 版に統一）
- `FakeProofVerifier` の採用理由コメントを追加
- `CMakeLists.txt` の重複 doc 削除、`ThrowStatus()` の `NB_UNREACHABLE` 化、aio 層の到達不能コード削除
