# aio_wt.connect() の 3 段階独立タイムアウトと接続 orphan 化

- Created: 2026-08-04
- Completed: {YYYY-MM-DD}
- Branch: feature/fix-wt-connect-timeout-stages
- Polished: {YYYY-MM-DD}

## 目的

`AsyncWebTransportClient.connect()` のタイムアウト意味論を整理し、中間段階で失敗した場合にサーバー側の QUIC 接続 / WebTransport セッションを orphan 化させない。

## 現状

`src/quiche/aio_wt.py` の `connect()` は次の 3 段階で、各段階に同じ `timeout`（既定 10 秒）を最初から適用する:

1. `_wait(is_connected, timeout)` — QUIC ハンドシェイク
2. `_wait(open_session, timeout)` — CONNECT 送信
3. `_wait(is_session_ready, timeout)` — セッション ready

- 既定で最大 30 秒かかる。`timeout=10` を渡したユーザーの想定（全体 10 秒）と異なる
- 2 段階目以降で timeout すると、QUIC 接続は確立済みなのに `_teardown()` され、`_teardown()` は `transport.close()` しか行わないため CONNECTION_CLOSE / CLOSE_WEBTRANSPORT_SESSION が一切送られず、サーバー側の接続とセッションが idle timeout まで資源を保持する

`src/quiche/aio_http3.py` の `request()` も submit_request 待ちと take_response 待ちで同じ二重タイムアウト構造を持つ。

## 設計方針

接続 / リクエスト全体で 1 つの deadline に収める、または残り時間を次段へ引き継ぐ。失敗時は `core.close()` と送信キュー drain を行ってから teardown する。

## 完了条件

- `connect(timeout=N)` が全体で N 秒以内に完了または失敗すること
- 中間段階で失敗した場合、サーバーへ CONNECTION_CLOSE（または CLOSE_WEBTRANSPORT_SESSION）が送信されること

## 解決方法

- 3 段階の `_wait` を 1 つの deadline（残り時間を引き継ぐ）で駆動する
- 失敗パスで `core.close()` + `_drain_send()` を呼んでから `_teardown()` する
- `aio_http3.py` の `request()` も同様に 1 つの deadline へ整理する
- テスト: 中間段階でタイムアウトするシナリオのテストを追加
