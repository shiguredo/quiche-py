# エラーパス・境界値・相手側クローズのテストを追加する

- Created: 2026-08-04
- Completed: {YYYY-MM-DD}
- Branch: feature/add-error-path-tests
- Polished: {YYYY-MM-DD}

## 目的

ステータス → 例外マッピング、境界値、タイムアウト、接続確立後の相手側クローズといったエラーパスのテストを追加する。

## 現状

以下の実装済みエラーパスに対応するテストがない:

- `module.cpp` のステータス → 例外マッピングのうち `INVALID_ARGUMENT` / `TOO_BIG` / `NOT_FOUND`（KeyError / ValueError）系統が未テスト
- `read(max_bytes=0)` の ValueError、`max_bytes` 上限超過の ValueError（`module.cpp` の `PyQuicStream::read` / `PyWebTransportStream::read`）
- 存在しないストリーム ID への操作（`quiche_glue.cc` の `StreamMissingStatus()` → NOT_FOUND → KeyError）
- `_wait()` の TimeoutError 送出（`src/quiche/aio.py` / `aio_http3.py` / `aio_wt.py` の 3 実装すべて未検証）、`timeout=0`、接続拒否（応答なし）時のタイムアウト
- 接続確立後に相手側から CONNECTION_CLOSE を受けた場合の `_was_connected` 分岐（`aio.py` の `_process()`）が未検証
- 16 MiB 超入力ガード（`module.cpp` の `BytesToString()`）が `send_datagram` のみで、write / receive_packet / submit_request の body では未検証
- datagram の実効上限超過時の TOO_BIG → ValueError

## 設計方針

既存の aioquic E2E 基盤（`tests/conftest.py`）を拡張し、エラーパスを実通信で検証する。相手側クローズは aioquic サーバーに CONNECTION_CLOSE を送らせる機能を追加する。

## 完了条件

- 上記のエラーパス・境界値がすべてテストで検証されること

## 解決方法

- `tests/test_basic.py` に read / write / receive_packet / submit_request の入力上限テストを追加する
- 存在しないストリーム ID への操作テストを追加する
- 接続拒否・タイムアウトのテストを `tests/test_async_client/` に追加する
- conftest に CONNECTION_CLOSE を送るサーバープロトコルを追加し、確立後の相手側クローズを検証する
- datagram の TOO_BIG 経路のテストを追加する
