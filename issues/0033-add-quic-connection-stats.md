# QUIC 接続統計 (QuicConnectionStats) を公開する

- Created: 2026-08-05
- Completed: {YYYY-MM-DD}
- Branch: feature/add-quic-connection-stats
- Polished: {YYYY-MM-DD}

## 目的

接続の健全性を Python から観測できるようにする。RTT や送受信バイト数、パケット喪失数などの接続統計を取得できれば、アプリケーション側で品質監視やデバッグができる。

## 現状

上流 `quic_connection.h` の `QuicConnection::GetStats()` が返す `QuicConnectionStats` (`quic_connection_stats.h`) がバインディングで未公開。`quiche_glue.cc` の `quiche_py_client` は統計取得 API を持たず、Python から RTT 等を取得する手段がない。

`QuicConnectionStats` には以下のようなフィールドがある (一部):

- 送受信: `bytes_sent` / `bytes_received` / `packets_sent` / `packets_received` / `stream_bytes_sent` / `stream_bytes_received`
- 再送・喪失: `bytes_retransmitted` / `packets_retransmitted` / `packets_lost`
- RTT: 推定 RTT / 最小 RTT / スムーズ RTT 等

## 設計方針

`quiche_glue.h` の C API に `client_get_stats` を追加し、`QuicConnectionStats` の主要フィールドを構造体で返す。`src/bindings/module.cpp` の `PyQuicClient` に `stats()` メソッドとして公開する。公開フィールドは利用実態に合わせて絞り込み、将来追加できる形にする。

## 完了条件

- Python から接続統計 (最低限: 送受信バイト数 / パケット数、RTT 系、パケット喪失数) が取得できること

## 解決方法

- `src/bindings/quiche_glue.h`: C API に `client_get_stats` と結果構造体を追加する
- `src/bindings/quiche_glue.cc`: `quiche_py_client::GetStats()` を実装し、API テーブルに配線する
- `src/bindings/module.cpp`: `PyQuicClient::stats()` を追加する
- テスト: 接続確立後に統計が取得でき、値が 0 以上であること (送受信が発生すれば増えること) を検証する
