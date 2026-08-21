# 公開 API を W3C WebTransport API 準拠の高レベル asyncio のみに再設計する

- Created: 2026-08-21
- Completed: {YYYY-MM-DD}
- Branch: feature/change-w3c-webtransport-api
- Polished: 2026-08-21

## 目的

quiche-py の公開 API を、W3C WebTransport API (https://www.w3.org/TR/webtransport/) を意識した高レベル asyncio API に再設計する。低レベル API や Sans I/O は公開しない。W3C 準拠の対象は WebTransport クライアントであり、QUIC / HTTP/3 クライアントは W3C を意識した高レベル asyncio API として整理・維持する。この issue は設計方針の確定を目的とし、実装はサブ issue に分割する。

## 現状

- `src/quiche/__init__.py` が低レベル API (`QuicClient` / `Http3Client` / `WebTransportClient` / `QuicStream` / `WebTransportStream` / `Http3Response`) と高レベル API (`AsyncQuicClient` / `AsyncQuicStream` / `AsyncHttp3Client` / `AsyncWebTransportClient` / `AsyncWebTransportStream`) の両方を公開している。
- 現行の高レベル API は低レベル Sans-I/O コア (`quiche_ext` の `receive_packet` / `next_send` / `next_timeout_ms` / `handle_timeout`) の上に薄い asyncio ラッパ (`aio.py` / `aio_http3.py` / `aio_wt.py`) を重ねた構造で、`_wait` / `_process` / `_teardown` が 3 モジュールでほぼ重複している。
- 現行 `AsyncWebTransportClient` は W3C API と形状が大きく異なる:
  - 接続確立は `connect()` + `is_session_ready()` ポーリング (W3C の `ready` Promise に相当する awaitable が無い)
  - クローズは `close(error_code, reason)` + `last_error: str` (W3C の `closed` Promise / `WebTransportCloseInfo` / `WebTransportError` に相当する型が無い)
  - ストリームは単一の `AsyncWebTransportStream` が read / write の両方を持つ (W3C の `WebTransportSendStream` / `WebTransportReceiveStream` / `WebTransportBidirectionalStream` への分離が無い)
  - datagram は `send_datagram()` / `receive_datagram()` メソッド (W3C の `datagrams` duplex stream / `WebTransportDatagramsWritable` が無い)
  - 受信ストリームは `accept_bidirectional_stream()` / `accept_unidirectional_stream()` の個別待ち (W3C の `incomingBidirectionalStreams` / `incomingUnidirectionalStreams` ストリームが無い)
  - エラーは `last_error: str` のみ (W3C の `WebTransportError(source, streamErrorCode)` が無い)
  - 優先度制御 API が無い (issue 0009 が send group / send order を要求済みだが、W3C の `sendGroup` / `sendOrder` に相当する設計になっていない)

## 設計方針

公開 API を W3C WebTransport API の IDL (Candidate Recommendation Snapshot 2026-07-30) に準拠した asyncio 版として再設計する。準拠の強さは「W3C IDL のメンバー・セマンティクスを維持し、Promise を asyncio の awaitable に、ReadableStream / WritableStream を async iterator / write() + drain() に写像する」とする。IDL とセマンティクスは変えず、asyncio 適応のみ行う。

### 対象とする W3C IDL と Python 対応

W3C `WebTransport` の主なメンバーを Python (asyncio) に写像する:

| W3C (IDL) | Python (asyncio) |
|---|---|
| `constructor(USVString url, optional WebTransportOptions options = {})` | `WebTransport(url, *, server_name=..., verify_peer=...)` 等の URL + オプションコンストラクタ |
| `readonly attribute Promise<undefined> ready` | `await wt.ready` できる awaitable (asyncio.Future) |
| `readonly attribute Promise<WebTransportCloseInfo> closed` | `closed` awaitable。graceful クローズで closeCode / reason を持つデータクラスに解決、abrupt クローズや確立失敗は `WebTransportError` で例外解決 |
| `readonly attribute Promise<undefined> draining` | `draining` awaitable |
| `undefined close(optional WebTransportCloseInfo closeInfo = {})` | `await wt.close(close_code=0, reason="")` または `close_code` / `reason` を持つデータクラス引数 |
| `readonly attribute WebTransportDatagramDuplexStream datagrams` | `wt.datagrams` プロパティ。`readable` (受信 async iterator) / `create_writable()` / `max_datagram_size` を持つオブジェクト。`incomingMaxAge` / `outgoingMaxAge` / `incomingMaxBufferedDatagrams` / `outgoingMaxBufferedDatagrams` にも対応 (datagram 滞留時間・バッファ上限の制御) |
| `Promise<WebTransportBidirectionalStream> createBidirectionalStream(optional WebTransportSendStreamOptions options = {})` | `await wt.create_bidirectional_stream(...)` → `WebTransportBidirectionalStream` |
| `Promise<WebTransportSendStream> createUnidirectionalStream(optional WebTransportSendStreamOptions options = {})` | `await wt.create_unidirectional_stream(...)` → `WebTransportSendStream` |
| `readonly attribute ReadableStream incomingBidirectionalStreams` | `wt.incoming_bidirectional_streams` を async iterator で回す |
| `readonly attribute ReadableStream incomingUnidirectionalStreams` | `wt.incoming_unidirectional_streams` を async iterator で回す |
| `WebTransportSendGroup createSendGroup()` | `wt.create_send_group()` → `WebTransportSendGroup` |
| `Promise<WebTransportConnectionStats> getStats()` | `await wt.get_stats()` → `WebTransportConnectionStats` 相当の統計データクラス (`WebTransportDatagramStats` を含む) |
| `readonly attribute DOMString protocol` | `wt.protocol` プロパティ (サブプロトコル)。`WebTransportOptions.protocols` に対応する `protocols` オプションも提供 |

W3C ストリームインターフェースの Python 対応:

| W3C (IDL) | Python (asyncio) |
|---|---|
| `WebTransportBidirectionalStream` (`readable` / `writable` 属性) | `readable` (ReceiveStream) と `writable` (SendStream) を持つデータクラス |
| `WebTransportSendStream : WritableStream` (`sendGroup` / `sendOrder` / `getStats()` / `getWriter()`) | `writable.write(data)` / `await writable.drain()` / `send_order` プロパティ。優先度を `send_order` / `send_group` で設定可能 |
| `WebTransportReceiveStream : ReadableStream` (`getStats()`) | `async for data in readable` または `await readable.read()` |
| `WebTransportWriter.atomicWrite()` / `commit()` | フロー制御デッドロック回避のための `atomic_write()` 相当 (デッドロック回避の設計が必要になった場合のみ導入し、実装のサブ issue で判断する) |
| `WebTransportDatagramsWritable` (`sendGroup` / `sendOrder`) | datagram 送信用の writable。`max_datagram_size` を超える datagram は無視 |

エラー・クローズ関連:

| W3C (IDL) | Python (asyncio) |
|---|---|
| `WebTransportError(source, streamErrorCode)` | `WebTransportError` 例外。`source` (`"stream"` / `"session"`) と `stream_error_code` 属性を持つ |
| `WebTransportCloseInfo` (`closeCode` / `reason`) | closeCode / reason を持つデータクラス (または関数引数) |
| `WebTransportReliabilityMode` (`"pending"` / `"reliable-only"` / `"supports-unreliable"`) | enum。本ライブラリは HTTP/3 のみ対象のため、確立後は常に `"supports-unreliable"` になる前提 |
| `WebTransportCongestionControl` (`"default"` / `"throughput"` / `"low-latency"`) | enum。実装できる範囲で対応し、対応可否は実装のサブ issue で判断する |

### QUIC / HTTP/3 / MOQT の位置付け

- W3C API は WebTransport のみを定義する。QUIC / HTTP/3 / MOQT は W3C に存在しないため、`AsyncQuicClient` / `AsyncHttp3Client` は「W3C を意識した高レベル asyncio API」として整理・維持する (低レベル API は非公開化)。整理の範囲 (URL 化、close() の await 化 など) は実装のサブ issue で判断する。
- `quiche_ext` の低レベルクラスは公開 API から外す。内部実装 (Sans-IO コア) としては残す。
- MOQT は本 issue のスコープ外とする (draft-ietf-moq-transport 準拠の設計・実装は別途)。

### asyncio 流儀の取り込み

- W3C の `ReadableStream` / `WritableStream` は Python では async iterator / `write()` + `drain()` に写像する。
- タイムアウトは呼び出し側の `asyncio.wait_for` に任せる。現行の内部 `_wait(attempt, timeout)` によるポーリングは、`ready` / `closed` / `draining` を `asyncio.Future` で公開することで置き換える。
- `receive_packet` / `next_send` / `next_timeout_ms` / `handle_timeout` の駆動は内部実装に隠す。

## 完了条件

この issue は「設計方針の確定」を完了とする:

- WebTransport クライアントの新公開 API 設計 (本 issue の「設計方針」セクション) が確定していること
- 実装のサブ issue が分割され、それぞれに変更対象ファイル・完了条件が定義されていること (サブ issue の一覧は「解決方法」セクション)
- 既存 open issue のうち、本 issue が再設計で置き換えるもの (0003 / 0005 / 0007 / 0008 / 0009 / 0018 / 0021 / 0022 / 0030、および旧 API 前提の test issue 0012 / 0013 / 0014) の扱いが決まっていること
- README.md の API 記述を新設計に追従する作業がサブ issue に含まれていること

実装そのもの (公開 API が高レベルだけになる、テストが新 API に追従する等) はサブ issue の完了条件であり、本 issue の完了条件ではない。

## 解決方法

- 設計方針をこの issue で確定し、実装はサブ issue に分割する (1 issue 1 目的の原則)。
- サブ issue は change / add / refactor / remove / test のカテゴリで起票する。Branch prefix は `feature/change-` / `feature/add-` / `feature/refactor-` / `feature/remove-` から選び、test カテゴリは既存の test issue 同様 `feature/add-` を使う。
- サブ issue の実装順は、変更対象が重なるものに配慮して決める (例: asyncio クライアント共通化 → WebTransport クライアント再設計 → 低レベル API 非公開化 の順)。具体的な順序はサブ issue 起票時に確定する。
- 想定されるサブ issue:
  - WebTransport クライアントの再設計 (`ready` / `closed` / `draining` / `close` / `WebTransportError` / `WebTransportCloseInfo` / `get_stats`)
  - ストリームの分離 (`WebTransportSendStream` / `WebTransportReceiveStream` / `WebTransportBidirectionalStream`)
  - datagram duplex stream (`datagrams` / `WebTransportDatagramsWritable` / datagram 滞留時間・バッファ上限、および datagram 側の `send_group` / `send_order`)
  - 受信ストリームの async iterator 化 (`incomingBidirectionalStreams` / `incomingUnidirectionalStreams`)
  - send group / send order (優先度制御、ストリーム側)
  - QUIC / HTTP/3 クライアントの公開 API 整理
  - 低レベル API の非公開化 (`__init__.py` から低レベルクラスを除去し、`README.md` の API 記述を更新)
  - asyncio クライアント 3 モジュールの共通化
  - W3C API 対応のテスト追加

### 既存 open issue の扱い

本 issue が再設計で置き換える既存 open issue は、サブ issue が実装を引き継ぐ形とし、本 issue の設計確定後に既存 issue を closed にする:

- `0003-bug-fix-wt-session-rejection-detection` (CONNECT 拒否 / セッションクローズ検知): W3C の `ready` の reject (`WebTransportError`) と `closed` / `draining` awaitable で実現される。拒否理由 (HTTP ステータスコード) は `WebTransportError` に直接の属性が無いため、`message` に含めるか、サブ issue「WebTransport クライアントの再設計」で別途設計する (ステータスコードが失われないことを、そのサブ issue の完了条件に含める)
- `0005-bug-fix-unbounded-datagram-queue` (受信 datagram キュー無制限): W3C の `datagrams.incomingMaxBufferedDatagrams` で上限を設定する。サブ issue「datagram duplex stream」が引き継ぐ
- `0007-bug-fix-wt-connect-timeout-stages` (connect() の 3 段階タイムアウト): `ready` awaitable の導入で置き換わる。タイムアウトは呼び出し側の `asyncio.wait_for` に委ねる。中間段階で失敗した場合に CONNECTION_CLOSE が送信されること (orphan 化防止) も、サブ issue「WebTransport クライアントの再設計」の完了条件に含める
- `0008-add-wt-stream-event-api` (ストリームのリセット / STOP_SENDING 通知): `WebTransportError` (`source` `"stream"` / `streamErrorCode`) とストリーム再設計で吸収される。サブ issue「ストリームの分離」が引き継ぐ
- `0009-add-wt-stream-priority` (優先度制御): `set_priority()` 設計は W3C の `sendGroup` / `sendOrder` に置き換わる。サブ issue「send group / send order」が引き継ぎ、C API (`module.cpp` / `quiche_glue_wt.cc`) の実装と、設定が QUICHE の優先度処理 (送信順序) に反映されることを完了条件に含める
- `0018-refactor-merge-async-client-common` (asyncio 共通化): サブ issue「asyncio クライアント 3 モジュールの共通化」が引き継ぐ
- `0021-change-wt-is-connected-session-state` (セッション状態): 本 issue で廃止予定の `is_connected()` / `is_session_ready()` を対象とする。サブ issue「WebTransport クライアントの再設計」が引き継ぐ
- `0022-add-wt-session-stats-and-controls` (セッション統計・制御): 下記の写像でサブ issue が引き継ぐ (担当: 統計と `draining` は「WebTransport クライアントの再設計」、datagram 滞留時間は「datagram duplex stream」、`protocol` プロパティは「WebTransport クライアントの再設計」)
  - `GetSessionStats()` / `GetDatagramStats()` → `WebTransportConnectionStats.getStats()` / `WebTransportDatagramStats`
  - `SetOnDraining()` / `NotifySessionDraining()` → `draining` awaitable
  - `GetNegotiatedSubprotocol()` → `WebTransport.protocol` プロパティ / `protocols` オプション
  - `SetDatagramMaxTimeInQueue()` → datagrams の `incomingMaxAge` / `outgoingMaxAge`
  - `GetPerspective()` / `GetUnderlyingProtocol()` → W3C に写像先が無いため、必要なら QUIC / HTTP/3 側の高レベル API で提供 (サブ issue「QUIC / HTTP/3 クライアントの公開 API 整理」で判断)
- `0030-add-wt-stream-aux-apis` (ストリーム補助 API): 枠解放通知は 0008 と統合予定。`ReadableBytes()` / `ResetDueToInternalError()` などの独立 API は、ストリーム再設計後の役割をサブ issue「ストリームの分離」で判断する
- 既存 test issue (`0012` / `0013` / `0014`): 旧 API を前提とするため、新 API への追従 (書き換え) をサブ issue「W3C API 対応のテスト追加」で行う
