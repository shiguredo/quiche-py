# WebTransport ストリームの優先度制御 (send group / send order) を追加する

- Created: 2026-08-21
- Completed: {YYYY-MM-DD}
- Branch: feature/add-wt-send-group-order
- Polished: {YYYY-MM-DD}

## 目的

WebTransport ストリームの送信優先度 (send group / send order) を W3C WebTransport API (https://www.w3.org/TR/webtransport/) に準拠して Python から制御できるようにする。

## 現状

上流 `web_transport.h` の `Stream::SetPriority(const StreamPriority&)` (`send_group_id` / `send_order` を持つ `StreamPriority`) がバインディングで公開されていない。`src/bindings/quiche_glue_wt.cc` の `WtSpdyClientSession` は `QuicPriorityType::kWebTransport` で上流の優先度機構を有効化済みだが、Python から使う手段がない。既存 issue 0009 は旧 `WebTransportStream` に `set_priority()` を追加する設計だったが、0035 の設計で W3C の `sendGroup` / `sendOrder` に置き換わる。

W3C では:

- `WebTransportSendStream.sendOrder`: ストリームの送信順序 (数値。大きいほど優先)
- `WebTransportSendStream.sendGroup`: 送信グループ
- `WebTransport.createSendGroup()`: 送信グループを作成
- `WebTransportDatagramsWritable.sendGroup` / `sendOrder`: datagram 側の優先度 (0039 で扱う)

## 設計方針

0035 (公開 API の W3C WebTransport API 準拠再設計) の設計方針に従い、ストリーム側の優先度を実装する (datagram 側は 0039 で扱う):

- `WebTransportSendStream.send_order`: 送信順序を設定・取得できるプロパティ
- `WebTransportSendStream.send_group`: 送信グループを設定・取得できるプロパティ
- `wt.create_send_group()`: `WebTransportSendGroup` を返す
- `create_bidirectional_stream()` / `create_unidirectional_stream()` のオプション (`send_order` / `send_group`) でも設定可能

## 完了条件

- Python からストリームの send group / send order を設定・取得できること
- 設定が QUICHE の優先度処理 (送信順序) に反映されること (0009 の完了条件を引き継ぐ)
- 既存テストが新 API に追従して通ること

## 解決方法

- `src/bindings/quiche_glue_wt.cc` に `Stream::SetPriority()` を呼ぶ C API を追加する
- `src/bindings/module.cpp` で `WebTransportSendStream.send_order` / `send_group` と `WebTransportSendGroup` / `create_send_group()` を公開する
- `src/quiche/aio_wt.py` で `WebTransportSendStream` に `send_order` / `send_group` を公開する
- 既存 issue の引き継ぎ: 0009 (優先度制御)
- テスト: 優先度設定の呼び出しと、優先度に応じた送信順の検証 (可能な範囲で)
