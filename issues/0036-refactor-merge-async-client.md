# asyncio クライアント 3 モジュールの重複を共通化する

- Created: 2026-08-21
- Completed: {YYYY-MM-DD}
- Branch: feature/refactor-merge-async-client
- Polished: {YYYY-MM-DD}

## 目的

`src/quiche/aio.py` / `aio_http3.py` / `aio_wt.py` のほぼ同一実装を共通化し、挙動分岐の発生を防ぐ。

## 現状

`aio.py` / `aio_http3.py` / `aio_wt.py` の約 200 行 × 3 がほぼ同一である (`_process` / `_wait` / `_teardown` / `_reschedule_timer` / `_on_timeout` / `_wake_waiters` / `_drain_send` / `_on_receive` / `_on_connection_lost` / `_require_core` / プロトコルクラス / connect の骨格)。

コピペのリスクは既に顕在化しており、挙動分岐が実在する:

- `_reschedule_timer()` は `aio.py` のみ `_loop is None` / `_core is None` で `RuntimeError` を raise し、他 2 つは黙って return する
- `_drain_send()` も `aio.py` のみ `RuntimeError` を raise する
- `_ClientProtocol.error_received` のコメントは `aio.py` にのみ存在する
- `AsyncQuicStream.read()` にのみ docstring があり、`AsyncWebTransportStream.read()` には無い

「バグ修正 1 箇所 × 3 ファイル + 修正漏れリスク」が確定している。本 issue は 0035 (公開 API の W3C WebTransport API 準拠再設計) のサブ issue として、再設計の前に共通基盤を整える。

## 設計方針

transport / timer / waiters / teardown を管理する共通基底クラス (例: `_AsyncClientBase`) を新設し、各モジュールはコア型 (`QuicClient` / `Http3Client` / `WebTransportClient`) の差だけを持つ構成にする。

## 完了条件

- 3 モジュールの重複が共通基底クラスに集約され、挙動分岐が解消されること
- 既存テストがすべて通ること
- 既存の公開 API 形状 (`AsyncQuicClient` / `AsyncHttp3Client` / `AsyncWebTransportClient`) が変わらないこと

## 解決方法

- `src/quiche/_async_base.py` を新設し、共通ロジック (transport / timer / waiters / teardown / `_wait` / `_process` / `_drain_send` / プロトコルクラス) を移す
- `aio.py` / `aio_http3.py` / `aio_wt.py` は基底クラスを継承し、コア型とプロトコルクラスのみを持つ
- 既存の挙動分岐 (raise 有無) は基底クラスに統一する
- テストで回帰がないことを確認する (既存テストの全実行)
