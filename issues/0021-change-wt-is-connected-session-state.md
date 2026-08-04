# WebTransport クライアントの is_connected() がセッション状態を反映しない

- Created: 2026-08-04
- Completed: {YYYY-MM-DD}
- Branch: feature/change-wt-is-connected-session-state
- Polished: {YYYY-MM-DD}

## 目的

`WebTransportClient.is_connected()` の意味論を raw QUIC クライアントと揃え、WebTransport セッションの状態を反映する。

## 現状

`src/bindings/quiche_glue_wt.cc` の `quiche_py_wt_client::IsConnected()` は QUIC 接続レベルしか見ない:

```cpp
return client_ != nullptr && client_->connected();
```

WebTransport セッションがクローズしても `is_connected()` は true を返し続け、ユーザーは `is_session_ready()` との併用を強いられる。raw QUIC 版（`quiche_glue.cc` の `quiche_py_client::IsConnected()`）は `session_visitor_.session_ready()` も条件に含めており、API として不整合。

## 設計方針

`IsConnected()` にセッション状態（`session_ready()`）を条件として含める。あわせてセッションクローズ時の挙動を整理する（関連: `0003-bug-fix-wt-session-rejection-detection` との整合）。

## 完了条件

- セッションクローズ後に `is_connected()` が false を返すこと
- ハンドシェイク完了直後（セッション ready 前）に `is_connected()` が false のままであること（現行の raw QUIC と同等）

## 解決方法

- `quiche_py_wt_client::IsConnected()` の条件に `session_visitor_ != nullptr && session_visitor_->session_ready()` を追加する
- この変更が `src/quiche/aio_wt.py` の `connect()` フロー（`_wait(is_connected)` → `_wait(is_session_ready)`）に与える影響を確認し、必要ならフローも整理する
- テストで回帰がないことを確認する
