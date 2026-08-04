# ストリーム制御 API（reset / stop_sending / can_write / is_open）のテストを追加する

- Created: 2026-08-04
- Completed: {YYYY-MM-DD}
- Branch: feature/add-stream-control-tests
- Polished: {YYYY-MM-DD}

## 目的

`QuicStream` / `AsyncQuicStream` の reset / stop_sending / can_write / is_open の挙動をテストで検証する。

## 現状

`src/bindings/module.cpp` の `PyQuicStream` で公開され、`src/quiche/aio.py` の `AsyncQuicStream` にもラップされている以下の API が、テストで一度も呼ばれていない:

- `reset(error_code)` — RESET_STREAM 送信
- `stop_sending(error_code)` — STOP_SENDING 送信
- `can_write()` — 書き込み可能判定
- `is_open()` — ストリーム生存判定

`AsyncQuicStream.reset()` / `stop_sending()` は `_process()` を呼ぶ副作用があるが、その経路も未検証。reset 後の RESET_STREAM 通知、write ブロック時の `can_write()` False、クローズ後の `is_open()` False 化が一切検証されていない。

## 設計方針

aioquic のエコーサーバーを使い、実通信で各 API の挙動を検証する。ピア側での RESET_STREAM 受信確認は aioquic のイベント経由で行う。

## 完了条件

- reset / stop_sending / can_write / is_open の各 API が実通信で検証されること
- `AsyncQuicStream` のラッパ経由でも検証されること

## 解決方法

- `tests/test_sans_io/test_stream.py` に reset / stop_sending / can_write / is_open のテストを追加する
- `tests/test_async_client/test_stream.py` にも同様のテストを追加する
- aioquic サーバー側に RESET_STREAM / STOP_SENDING を受信して応答する機能を conftest に追加する
- 関連: `0008-add-wt-stream-event-api` で追加されるストリームイベント API のテストもこの構成に載せる
