# WebTransport の相互運用テストを追加する

- Created: 2026-08-04
- Completed: {YYYY-MM-DD}
- Branch: feature/add-wt-interop-tests
- Polished: {YYYY-MM-DD}

## 目的

`AsyncWebTransportClient` / `WebTransportClient` の実動作（QUIC ハンドシェイク → CONNECT → セッション ready → ストリーム / datagram の往復）を検証するテストを追加する。

## 現状

`tests/test_webtransport_client.py` はクライアント生成（2 件）と start 前の `connect_session()` の失敗経路のみで、公開 API の大部分が未検証:

- `AsyncWebTransportClient.connect()` の 3 段階フロー（QUIC ハンドシェイク → CONNECT → session ready）は一度も実行されていない
- `WebTransportStream` / `AsyncWebTransportStream` の read / write / datagram は未テスト
- CONNECT 拒否（4xx）やセッションクローズのシナリオも未テスト（関連: `0003-bug-fix-wt-session-rejection-detection`、`0001-bug-fix-wt-session-use-after-free` の回帰テスト）

制約: 現行の E2E 相手である aioquic には WebTransport 実装が無いため、aioquic では相互運用テストを書けない。

## 設計方針

WebTransport サーバーとして何を使うかを決定する。候補:

- バインディング内（QUICHE の `WebTransportOnlyDispatcher` 等）に最小の検証用サーバーを同梱する
- 外部の WebTransport サーバー（公開サービス等）を E2E 相手にする（外部依存のため CI では不安定になり得る）

## 完了条件

- 実際の WebTransport セッションでストリーム（双方向 / 単方向）と datagram の往復が検証できること
- CONNECT 拒否とセッションクローズのシナリオがテストできること

## 解決方法

- 検証用 WebTransport サーバー（QUICHE のツールを流用した最小実装）を追加し、`tests/conftest.py` のフィクスチャとして起動する
- `tests/test_webtransport_client.py` を拡張し、セッション確立・ストリーム往復・datagram 往復・CONNECT 拒否のテストを追加する
- サーバー実装の追加が大規模になる場合は、分割して別 issue にする
