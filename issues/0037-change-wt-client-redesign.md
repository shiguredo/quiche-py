# WebTransport クライアントを W3C WebTransport API 準拠に再設計する

- Created: 2026-08-21
- Completed: {YYYY-MM-DD}
- Branch: feature/change-wt-client-redesign
- Polished: {YYYY-MM-DD}

## 目的

`AsyncWebTransportClient` の接続・クローズ・状態管理を、W3C WebTransport API (https://www.w3.org/TR/webtransport/) の `WebTransport` インターフェース (Candidate Recommendation Snapshot 2026-07-30) に準拠した形状へ再設計する。

## 現状

`src/quiche/aio_wt.py` の `AsyncWebTransportClient` は以下の構造を持つ:

- 接続確立は `connect(path, headers, timeout)` + `is_session_ready()` ポーリング
- クローズは `close(error_code, reason)` + `last_error: str`
- 状態は `is_connected()` / `is_session_ready()` のメソッド + `last_error` 文字列
- CONNECT 拒否やセッションクローズは 0003 の通り検知不能 (拒否理由が失われる)
- connect() の 3 段階独立タイムアウトは 0007 の通り orphan 化の原因

W3C の `WebTransport` は `ready` / `closed` / `draining` の Promise、`close(closeInfo)`、`getStats()`、`WebTransportError(source, streamErrorCode)`、`WebTransportCloseInfo(closeCode, reason)` を持つ。現行 API はこれらに相当する型・awaitable を持たない。

## 設計方針

0035 (公開 API の W3C WebTransport API 準拠再設計) の設計方針に従い、`AsyncWebTransportClient` を W3C `WebTransport` 相当に再設計する:

- `ready`: セッション確立時に解決する asyncio.Future。確立失敗 (CONNECT 拒否等) は `WebTransportError` で例外解決する
- `closed`: graceful クローズで `WebTransportCloseInfo` (closeCode / reason) に解決、abrupt クローズや確立失敗は `WebTransportError` で例外解決する
- `draining`: サーバーが drain を要求した時に解決する asyncio.Future
- `close(close_code=0, reason="")`: クローズ情報を送信して接続を閉じる
- `get_stats()`: `WebTransportConnectionStats` 相当の統計データクラスを返す (`WebTransportDatagramStats` を含む)
- `WebTransportError`: `source` (`"stream"` / `"session"`) と `stream_error_code` 属性を持つ例外
- `WebTransportCloseInfo`: `close_code` / `reason` を持つデータクラス
- `protocol`: 交渉されたサブプロトコルを返すプロパティ。`protocols` オプションも提供

内部のポーリング (`_wait(is_session_ready)` 等) は `ready` / `closed` / `draining` の Future で置き換える。タイムアウトは呼び出し側の `asyncio.wait_for` に委ねる。

## 完了条件

- `ready` / `closed` / `draining` を awaitable として公開し、セッション確立・クローズ・drain を観測できること
- CONNECT 拒否 (HTTP ステータスコード) が `WebTransportError` 経由で失われずに伝わること (0003 の完了条件を引き継ぐ)
- 中間段階で失敗した場合に CONNECTION_CLOSE が送信されること (0007 の orphan 化防止を引き継ぐ)
- `close(close_code, reason)` でクローズ情報を送信できること
- `get_stats()` で統計情報を取得できること
- `protocol` プロパティでサブプロトコルを取得できること
- 既存テストが新 API に追従して通ること

## 解決方法

- `src/bindings/quiche_glue_wt.cc` に以下を追加・公開する:
  - セッションクローズ理由 (HTTP ステータスコード / エラーメッセージ) の取得
  - セッション統計 (`GetSessionStats()` / `GetDatagramStats()` 相当)
  - draining 通知の受信 (`SetOnDraining()` / `NotifySessionDraining()` 相当)
  - サブプロトコルの取得 (`GetNegotiatedSubprotocol()` 相当)
- `src/bindings/module.cpp` で `WebTransportError` / `WebTransportCloseInfo` / 統計データクラスを公開する
- `src/quiche/aio_wt.py` の `AsyncWebTransportClient` を再設計する (`ready` / `closed` / `draining` / `close` / `get_stats` / `protocol`)
- 既存 issue の引き継ぎ: 0003 (CONNECT 拒否検知) / 0007 (connect タイムアウト) / 0021 (is_connected のセッション状態反映) / 0022 (統計・draining・subprotocol のうち本 issue 対象分)
- テスト: ready / closed / draining / close / 拒否理由 / 統計 / サブプロトコルのテストを追加・更新
