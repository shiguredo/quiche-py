# WebTransport ストリームを送受信ストリームに分離する

- Created: 2026-08-21
- Completed: {YYYY-MM-DD}
- Branch: feature/change-wt-stream-separation
- Polished: {YYYY-MM-DD}

## 目的

`AsyncWebTransportStream` を W3C WebTransport API (https://www.w3.org/TR/webtransport/) の `WebTransportSendStream` / `WebTransportReceiveStream` / `WebTransportBidirectionalStream` に分離する。

## 現状

`src/quiche/aio_wt.py` の `AsyncWebTransportStream` は read / write の両方を持つ単一ストリームで、`stream_id` / `unidirectional` 属性を持つ。W3C ではストリームは方向に応じて読み書きが分かれる:

- `WebTransportSendStream` (WritableStream): `write()` / `drain()` / `sendOrder` / `sendGroup` / `getStats()` / `getWriter()`
- `WebTransportReceiveStream` (ReadableStream): `read()` / async iterator / `getStats()`
- `WebTransportBidirectionalStream`: `readable` (ReceiveStream) と `writable` (SendStream) の両方を持つ

## 設計方針

0035 (公開 API の W3C WebTransport API 準拠再設計) の設計方針に従い、ストリームを送受信に分離する:

- `WebTransportSendStream`: 送信専用。`write(data)` / `await drain()` / `send_order` プロパティ。`reset()` は W3C の WritableStream の abort に相当する形で扱う
- `WebTransportReceiveStream`: 受信専用。`async for data in stream` または `await read()`。ピアのリセット / STOP_SENDING は `WebTransportError` (`source` `"stream"` / `stream_error_code`) で伝える
- `WebTransportBidirectionalStream`: `readable` と `writable` を持つデータクラス

`create_bidirectional_stream()` / `create_unidirectional_stream()` は `WebTransportBidirectionalStream` / `WebTransportSendStream` を返す。

## 完了条件

- 双方向ストリームが `readable` / `writable` を持つこと
- 単方向ストリームが送受信の方向に応じて `WebTransportSendStream` / `WebTransportReceiveStream` に分かれること
- ピアのリセット / STOP_SENDING が `WebTransportError` 経由でエラーコード付きで伝わること (0008 の完了条件を引き継ぐ)
- 既存テストが新 API に追従して通ること

## 解決方法

- `src/bindings/quiche_glue_wt.cc` にストリームイベント取得を追加する (0008 の `StreamVisitor` 実装を引き継ぐ)
- `src/bindings/module.cpp` で `WebTransportSendStream` / `WebTransportReceiveStream` / `WebTransportBidirectionalStream` を公開する
- `src/quiche/aio_wt.py` で単一 `AsyncWebTransportStream` を 3 クラスに分割する
- 既存 issue の引き継ぎ: 0008 (リセット / STOP_SENDING 通知) / 0030 (ストリーム補助 API のうち `ReadableBytes` 相当と枠解放通知)
- テスト: 双方向 / 単方向 / リセット / STOP_SENDING のテストを追加・更新
