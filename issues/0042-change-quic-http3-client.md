# QUIC / HTTP/3 クライアントの公開 API を整理する

- Created: 2026-08-21
- Completed: {YYYY-MM-DD}
- Branch: feature/change-quic-http3-client
- Polished: {YYYY-MM-DD}

## 目的

`AsyncQuicClient` / `AsyncHttp3Client` を「W3C を意識した高レベル asyncio API」として整理し、WebTransport クライアント (0037) と一貫した形状にする。

## 現状

`src/quiche/aio.py` の `AsyncQuicClient` と `src/quiche/aio_http3.py` の `AsyncHttp3Client` は、`connect()` / `close(error_code, reason)` / `is_connected()` / `last_error` という構造を持つ。W3C 準拠に再設計される `AsyncWebTransportClient` (0037) と API 形状が揃わない:

- `close(error_code, reason)` のセマンティクス (クローズ情報の型がない)
- `is_connected()` / `last_error` による状態管理
- `aio_http3.py` の `request()` は submit_request 待ちと take_response 待ちの二重タイムアウト構造を持つ (0007 で指摘)

## 設計方針

0035 (公開 API の W3C WebTransport API 準拠再設計) の設計方針に従い、W3C には無い QUIC / HTTP/3 の API は「W3C を意識した高レベル asyncio API」として整理する。整理の範囲は実装時に判断する (下記は候補):

- `close()` のシグネチャをクローズ情報 (close_code / reason) を持つ形に揃える
- `is_connected()` / `last_error` の状態管理を整理する
- `aio_http3.py` の `request()` の二重タイムアウトを 1 つの deadline に整理する (0007 の引き継ぎ)
- 低レベル API の非公開化 (0043) との整合を取る

## 完了条件

- `AsyncQuicClient` / `AsyncHttp3Client` が WebTransport クライアント (0037) と一貫した高レベル asyncio API を持つこと
- 既存テストが新 API に追従して通ること

## 解決方法

- `src/quiche/aio.py` / `aio_http3.py` の API 形状を整理する
- `src/quiche/aio_http3.py` の `request()` のタイムアウトを 1 つの deadline に整理する
- 既存 issue の引き継ぎ: 0007 (request() の二重タイムアウト)
- テスト: QUIC / HTTP/3 クライアントのテストを更新
