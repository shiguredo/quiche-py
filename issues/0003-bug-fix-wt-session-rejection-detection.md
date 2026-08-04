# WebTransport の CONNECT 拒否 / セッションクローズが検知不能

- Created: 2026-08-04
- Completed: {YYYY-MM-DD}
- Branch: feature/fix-wt-session-rejection-detection
- Polished: {YYYY-MM-DD}

## 目的

サーバーが WebTransport CONNECT を拒否した場合（403 / 404 / 429 等）やセッションをクローズした場合に、クライアントが即座に検知し、拒否理由をユーザーへ伝える。

## 現状

`src/bindings/quiche_glue_wt.cc` の `WtSessionVisitor::OnSessionClosed()` で `session_closed_` フラグと close 情報が記録されるが:

- `ReceivePacket()` / `HandleTimeout()` は QUIC 接続が生きている限り `QUICHE_PY_STATUS_CLOSED` を返さない
- `PopulateClosedErrorIfNeeded()` は CLOSED 返却時のみ呼ばれるため、接続が生きている間は close 理由が `last_error` に載らない
- `src/quiche/aio_wt.py` の `_process()` は `is_connected()` しか監視せず、セッション状態（`is_session_ready()` / セッションクローズ）を見ない

結果、CONNECT 拒否時は `_wait(is_session_ready())`（`aio_wt.py` の `connect()`）が理由不明のままタイムアウト（既定 10 秒）まで待ち、`TimeoutError` になり拒否理由は完全に失われる。この間のポーリングは `0001-bug-fix-wt-session-use-after-free` の UAF 経路にもなる。

## 設計方針

セッションクローズをイベントとして観測できる仕組みを追加し、aio 層で即 teardown して close 理由を `ConnectionError` として伝える。あわせて上流 `WebTransportHttp3::rejection_reason()` の公開を検討する。

## 完了条件

- サーバーが CONNECT を 4xx で拒否した場合、ユーザーに拒否理由（ステータスコード / エラーメッセージ）付きの `ConnectionError` が短時間で届くこと
- セッション ready 後にサーバーがセッションをクローズした場合、クライアントが検知して teardown すること

## 解決方法

- `quiche_py_wt_client` にセッションクローズ状態の取得 API（またはセッションイベント取得 API）を追加し、close 理由を公開する
- `src/quiche/aio_wt.py` の `_process()` でセッションクローズを監視し、検知時に `_teardown(last_error)` する
- `_wait` ループで `last_error` を監視し、接続確立後にエラーが記録されたら `ConnectionError` で打ち切る
- 回帰テスト: CONNECT 拒否とセッションクローズのシナリオをテスト可能な検証用サーバーが必要（関連: `0012-test-add-webtransport-interop`）
