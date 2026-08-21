# WebTransport datagram を duplex stream 化する

- Created: 2026-08-21
- Completed: {YYYY-MM-DD}
- Branch: feature/change-wt-datagram-duplex
- Polished: {YYYY-MM-DD}

## 目的

`send_datagram()` / `receive_datagram()` メソッドを、W3C WebTransport API (https://www.w3.org/TR/webtransport/) の `WebTransportDatagramDuplexStream` に再設計する。

## 現状

`src/quiche/aio_wt.py` の `AsyncWebTransportClient` は `send_datagram(data)` / `receive_datagram()` / `max_datagram_size()` メソッドを持つ。W3C では datagram は duplex stream で表す:

- `WebTransportDatagramDuplexStream`: `readable` (受信 ReadableStream) / `createWritable()` (送信) / `maxDatagramSize` / `incomingMaxAge` / `outgoingMaxAge` / `incomingMaxBufferedDatagrams` / `outgoingMaxBufferedDatagrams`
- `WebTransportDatagramsWritable` (WritableStream): `sendGroup` / `sendOrder`
- 受信キューは `incomingMaxBufferedDatagrams` を超えると古いものから破棄される (0005 の無制限キュー問題に相当)

## 設計方針

0035 (公開 API の W3C WebTransport API 準拠再設計) の設計方針に従い、`wt.datagrams` プロパティを duplex stream として提供する:

- `wt.datagrams.readable`: 受信 datagram を async iterator で読む
- `wt.datagrams.create_writable()`: 送信 datagram の writable を返す。`max_datagram_size` を超える datagram は無視する
- `wt.datagrams.max_datagram_size`: 現在の最大 datagram サイズ
- `wt.datagrams.incoming_max_age` / `outgoing_max_age`: datagram 滞留時間の上限 (秒)
- `wt.datagrams.incoming_max_buffered_datagrams` / `outgoing_max_buffered_datagrams`: バッファ上限
- datagram 側の `send_group` / `send_order` (優先度) を `create_writable()` のオプションで設定可能

受信キューは `incoming_max_buffered_datagrams` を超えると古いものから破棄する (0005 の完了条件を引き継ぐ)。

## 完了条件

- `wt.datagrams` が duplex stream として利用できること (受信 async iterator / 送信 writable / `max_datagram_size`)
- datagram 滞留時間・バッファ上限を設定できること
- 受信キューが上限を超えてもメモリ使用量が一定に保たれ、datagram の受信処理が継続すること (0005 の完了条件を引き継ぐ)
- datagram 側の優先度 (`send_group` / `send_order`) を設定できること
- 既存テストが新 API に追従して通ること

## 解決方法

- `src/bindings/quiche_glue_wt.cc` に受信キュー上限と破棄ポリシーを実装する (0005 を引き継ぐ)
- `src/bindings/module.cpp` で `WebTransportDatagramDuplexStream` / `WebTransportDatagramsWritable` を公開する
- `src/quiche/aio_wt.py` で `send_datagram` / `receive_datagram` / `max_datagram_size` を `wt.datagrams` に置き換える
- datagram 滞留時間は上流 `SetDatagramMaxTimeInQueue()` に写像する (0022 の引き継ぎ)
- 既存 issue の引き継ぎ: 0005 (受信キュー無制限) / 0022 (datagram 滞留時間)
- テスト: datagram の送受信・バッファ上限・滞留時間・優先度のテストを追加・更新
