# QUIC の 0-RTT とセッション再開を追加する

- Created: 2026-08-04
- Completed: {YYYY-MM-DD}
- Branch: feature/add-quic-0rtt-session-resume
- Polished: {YYYY-MM-DD}

## 目的

QUIC クライアントの再接続時に 0-RTT / セッション再開を利用し、ハンドシェイク遅延を削減する。

## 現状

3 クライアントすべて（`quiche_glue.cc` の `GoogleQuichePyTransportClient` / `quiche_glue_http3.cc` の `GoogleQuichePyHttp3Client` / `quiche_glue_wt.cc` の `GoogleQuichePyWtClient`）は session cache を `nullptr` で生成しており、再接続時のハンドシェイクが常に全量（1-RTT）になる。

上流には対応基盤がある:

- `quic_client_base.h` のコンストラクタ引数（session cache）
- `quic_client_session_cache.h` の `QuicClientSessionCache`
- `EarlyDataAccepted()` / `ReceivedInchoateReject()` は `quiche_glue.cc` と `quiche_glue_wt.cc` でオーバーライド済みだが、Python から判定する手段がない

## 設計方針

セッションキャッシュをクライアント生成オプションとして公開し、0-RTT 受容の可否を Python から判定できるようにする。

## 完了条件

- 同じサーバーへの 2 回目以降の接続で 0-RTT データが送信できること（または session resumption が成立すること）
- `EarlyDataAccepted()` の結果を Python から取得できること

## 解決方法

- セッションキャッシュを `quiche_py_client_options` 等に追加し、glue で `QuicClientSessionCache` を生成して渡す
- `EarlyDataAccepted` / `ReceivedInchoateReject` を C API 経由で公開する
- 0-RTT 用のデータ送信タイミング（`connect()` 完了前の `write()`）を aio 層でどう扱うか設計する
- テスト: 再送（reconnect）時の resumption 動作を aioquic で検証する（aioquic が session ticket を発行できるか要調査）
