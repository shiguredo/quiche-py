# connect() 失敗後の再試行で UDP transport がリークする

- Created: 2026-08-04
- Completed: {YYYY-MM-DD}
- Branch: feature/fix-connect-retry-transport-leak
- Polished: {YYYY-MM-DD}

## 目的

connect() が一度失敗した後、同じクライアントで connect() を呼び直したときに UDP ソケット（FD）をリークさせない。

## 現状

`src/quiche/aio.py` の `AsyncQuicClient.connect()`（`aio_http3.py` / `aio_wt.py` も同型）は失敗時に `_teardown("connect failed")` を呼び、`_closed = True` になる。同じクライアントで connect() を呼び直すと:

1. getaddrinfo → core 生成 → `create_datagram_endpoint()` まで成功し、新しい `DatagramTransport` が生成される
2. `_wait()` の先頭で `_closed` を検知して即座に `ConnectionError` になる
3. `except BaseException` の `_teardown()` は `_closed` ガードで no-op のため、**新しい transport が close されずソケットがリークする**

また、例外は「connect failed」固定で、最初の失敗理由（タイムアウト / サーバーの close 理由）がユーザーに伝わらない。

## 設計方針

connect() 後の再利用可否を明確にする。再利用不可とするなら connect() 冒頭で明示的なエラーを返す。再利用可能とするなら失敗パスで新 transport を確実に閉じる。

## 完了条件

- connect() 失敗後に再 connect() した場合、ソケットのリークが発生しないこと
- 再利用不可の場合、理由が明確な例外（例: RuntimeError）が投げられること

## 解決方法

- connect() 冒頭で `if self._closed: raise RuntimeError("client is closed; create a new client")` とする
- または connect() 成功時に `_closed` / `_was_connected` をリセットし、失敗パスで必ず新 transport を閉じる
- テスト: connect 失敗 → 再 connect のシーケンスで FD リークを検証するテストを追加
