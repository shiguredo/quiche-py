# QUIC の SendPing を公開する

- Created: 2026-08-05
- Completed: {YYYY-MM-DD}
- Branch: feature/add-quic-send-ping
- Polished: {YYYY-MM-DD}

## 目的

アプリケーション主導の keepalive を Python から送れるようにする。QUIC の Ping フレームは、コネクションが生きていることを相手に知らせるために使う。

## 現状

上流 `quic_connection.h` の `QuicConnection::SendPing()` / `SendPingAtLevel()` がバインディングで未公開。`quiche_glue.cc` の `quiche_py_client` は Ping 送信 API を持たず、Python から Ping を送る手段がない。

## 設計方針

`quiche_glue.h` の C API に `client_send_ping` を追加し、`src/bindings/module.cpp` の `PyQuicClient` に `send_ping()` メソッドとして公開する。実装は `quic::QuicConnection::SendPing()` を呼ぶ。

## 完了条件

- Python から `send_ping()` を呼ぶと Ping フレームが送信キューに積まれ、`next_send()` で取り出せること

## 解決方法

- `src/bindings/quiche_glue.h`: C API に `client_send_ping` を追加する
- `src/bindings/quiche_glue.cc`: `quiche_py_client::SendPing()` を実装し、API テーブルに配線する
- `src/bindings/module.cpp`: `PyQuicClient::send_ping()` を追加する
- テスト: 接続確立後に `send_ping()` を呼び、送信パケットが出ることと相手に到達することを検証する
