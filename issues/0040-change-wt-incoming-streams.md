# WebTransport 受信ストリームを async iterator 化する

- Created: 2026-08-21
- Completed: {YYYY-MM-DD}
- Branch: feature/change-wt-incoming-streams
- Polished: {YYYY-MM-DD}

## 目的

`accept_bidirectional_stream()` / `accept_unidirectional_stream()` の個別待ちを、W3C WebTransport API (https://www.w3.org/TR/webtransport/) の `incomingBidirectionalStreams` / `incomingUnidirectionalStreams` (ReadableStream) に再設計する。

## 現状

`src/quiche/aio_wt.py` の `AsyncWebTransportClient` は `accept_bidirectional_stream()` / `accept_unidirectional_stream()` メソッドを持ち、それぞれ 1 ストリームずつ個別に待つ。W3C では受信ストリームは ReadableStream で表し、`for await` で列挙する:

- `incomingBidirectionalStreams`: ReadableStream\<WebTransportBidirectionalStream\>
- `incomingUnidirectionalStreams`: ReadableStream\<WebTransportReceiveStream\>

## 設計方針

0035 (公開 API の W3C WebTransport API 準拠再設計) の設計方針に従い、受信ストリームを async iterator で列挙できるようにする:

- `wt.incoming_bidirectional_streams`: 受信双方向ストリームの async iterator。`async for stream in wt.incoming_bidirectional_streams:` で回す
- `wt.incoming_unidirectional_streams`: 受信単方向ストリームの async iterator。`async for stream in wt.incoming_unidirectional_streams:` で回す
- 各ストリームは `WebTransportBidirectionalStream` / `WebTransportReceiveStream` (0038 で分離した型)

## 完了条件

- `wt.incoming_bidirectional_streams` / `wt.incoming_unidirectional_streams` を async iterator で列挙できること
- 列挙中もストリームの読み書き (0038 の型) ができること
- 既存テストが新 API に追従して通ること

## 解決方法

- `src/quiche/aio_wt.py` に `incoming_bidirectional_streams` / `incoming_unidirectional_streams` の async iterator を実装する
- 内部の `_accept()` ポーリングを async generator に置き換える
- 既存の `accept_bidirectional_stream()` / `accept_unidirectional_stream()` は async iterator の内部実装として扱う (公開 API からは消す)
- テスト: 受信ストリームを async iterator で列挙するテストを追加・更新
