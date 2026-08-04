# aio 層でコア例外発生時にクライアントがゾンビ化する

- Created: 2026-08-04
- Completed: {YYYY-MM-DD}
- Branch: feature/fix-async-client-zombie
- Polished: {YYYY-MM-DD}

## 目的

`_on_receive()` / `_on_timeout()` でコアの例外が発生した場合に、クライアントを確実に teardown して全 waiters を解放する。

## 現状

`src/quiche/aio.py` の `_on_receive()` と `_on_timeout()`（`aio_http3.py` / `aio_wt.py` も同型）は `receive_packet()` / `handle_timeout()` を裸で呼んでいる。`src/bindings/module.cpp` は `QUICHE_PY_STATUS_CLOSED` 以外のエラーを Python 例外として raise するため、万一 raise されると:

- 例外は `datagram_received` コールバックからイベントループの例外ハンドラへ流れてログされるだけになる
- `_teardown()` も `_wake_waiters()` も呼ばれず、`_closed` は False のまま
- 以後の `_wait()` はすべてタイムアウトまで寝続け、クライアントが永久待ちになる

## 設計方針

コールバック内のコア呼び出しを try / except で包み、例外を `_teardown()` に落とす。teardown は waiters を起こすため、ハングは起きない。

## 完了条件

`_on_receive()` / `_on_timeout()` 内で例外が発生しても、全 `_wait()` が `ConnectionError` で即座に終了すること。

## 解決方法

- `_on_receive()` / `_on_timeout()` で `except Exception` を捕まえて `_teardown(str(exc))` を呼ぶ
- 3 モジュール（`aio.py` / `aio_http3.py` / `aio_wt.py`）すべてに適用する
- テスト: 不正パケットを `receive_packet()` へ送り、例外経路で teardown されることを確認するテストを追加
