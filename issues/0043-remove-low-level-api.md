# 低レベル API を非公開化する

- Created: 2026-08-21
- Completed: {YYYY-MM-DD}
- Branch: feature/remove-low-level-api
- Polished: {YYYY-MM-DD}

## 目的

公開 API から低レベル Sans-I/O クラスを除去し、公開 API を高レベル asyncio API のみにする (CODEBASE.md の「公開 API は asyncio ベースの高レベル API のみを提供すること」への適合)。

## 現状

`src/quiche/__init__.py` は低レベル API (`QuicClient` / `Http3Client` / `WebTransportClient` / `QuicStream` / `WebTransportStream` / `Http3Response`) と高レベル API (`AsyncQuicClient` / `AsyncQuicStream` / `AsyncHttp3Client` / `AsyncWebTransportClient` / `AsyncWebTransportStream`) の両方を公開している。README.md の「特徴」セクションにも低レベル API の記載がある。

低レベル Sans-I/O コア (`quiche_ext` の `receive_packet` / `next_send` / `next_timeout_ms` / `handle_timeout`) は高レベル API の内部実装として必要であり、公開 API から外すだけで残す。

## 設計方針

0035 (公開 API の W3C WebTransport API 準拠再設計) の設計方針に従い、`__init__.py` から低レベルクラスを除去する。`quiche_ext` モジュール自体は残し、内部実装として使用する (直接 import は非推奨だが公開はしない)。

## 完了条件

- `src/quiche/__init__.py` の `__all__` から低レベルクラスが消え、公開 API が高レベル asyncio API のみになること
- README.md の API 記述が新設計に追従すること
- 既存テストが新 API に追従して通ること

## 解決方法

- `src/quiche/__init__.py` の import / `__all__` から低レベルクラスを除去する
- README.md の「特徴」と「使い方」の API 記述を更新する
- `quiche_ext` は内部実装として残す (公開 API から外す)
- テスト: `__init__.py` の公開 API が高レベル API のみであることを確認するテストを追加 (必要な場合)
