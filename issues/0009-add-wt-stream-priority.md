# WebTransport ストリームの優先度制御 API を追加する

- Created: 2026-08-04
- Completed: {YYYY-MM-DD}
- Branch: feature/add-wt-stream-priority
- Polished: {YYYY-MM-DD}

## 目的

WebTransport ストリームの送信優先度（send group / send order）を Python から制御できるようにする。

## 現状

上流 `web_transport.h` の `Stream::SetPriority(const StreamPriority&)`（`send_group_id` / `send_order` を持つ `StreamPriority`）がバインディングで公開されていない。`quiche_glue_wt.cc` の `WtSpdyClientSession` は `QuicPriorityType::kWebTransport` で上流の優先度機構を有効化済みだが、Python から使う手段がない。

動画・音声・データ混在時にストリーム単位の送信優先制御ができないため、WebTransport を使ったメディア配信の品質制御が実現できない。

## 設計方針

stream_id を引数に `SetPriority()` を呼ぶ C API を追加し、`WebTransportStream`（`AsyncWebTransportStream`）に優先度設定メソッドとして公開する。

## 完了条件

- Python からストリームの send group / send order を設定できること
- 設定が QUICHE の優先度処理（送信順序）に反映されること

## 解決方法

- C API に `wt_client_set_stream_priority(client, stream_id, send_group_id, send_order)` を追加する
- `module.cpp` で `WebTransportStream` に `set_priority()` として公開し、`src/quiche/aio_wt.py` の `AsyncWebTransportStream` にもラップする
- テスト: 優先度設定の呼び出しと、優先度に応じた送信順の検証（可能な範囲で）
