# HTTP/3 クライアント close() の error_code / reason が無視される

- Created: 2026-08-04
- Completed: {YYYY-MM-DD}
- Branch: feature/fix-h3-close-error-code
- Polished: {YYYY-MM-DD}

## 目的

`Http3Client.close(error_code, reason)` の引数がサーバーへ伝達されるようにする。現在は引数が黙殺され、固定のエラーコードで切断される。

## 現状

`src/bindings/quiche_glue_http3.cc` の `quiche_py_http3_client::Close()` は引数（error_code / reason）をコメント付きで捨て、`QuicClientBase::Disconnect()` を呼ぶ。上流の `Disconnect()` は `QUIC_PEER_GOING_AWAY` / "Client disconnecting" で固定の CONNECTION_CLOSE を送る。

- raw QUIC（`quiche_glue.cc` の `quiche_py_client::Close()`）は `QuicGenericSessionBase::CloseSession(error_code, reason)` で引数を伝える
- WebTransport（`quiche_glue_wt.cc` の `quiche_py_wt_client::Close()`）は `WebTransportHttp3::CloseSession(error_code, reason)` で引数を伝える

HTTP/3 だけが API 契約違反になっている。`src/quiche/aio_http3.py` の `close(error_code, reason)` の引数も同様に無意味になる。

## 設計方針

raw QUIC と同じパターンで CONNECTION_CLOSE のアプリケーションエラーフィールドに error_code / reason を載せる。HTTP/3 のエラーコード空間（RFC 9114 §8.1）の扱いを決定する。

## 完了条件

`close(error_code, reason)` を呼んだときに、その error_code / reason が CONNECTION_CLOSE フレームに載って送信されること。E2E でサーバー側が受信した close 理由を確認できること。

## 解決方法

- `quiche_py_http3_client::Close()` で `session()->connection()->CloseConnection(QUIC_NO_ERROR, error_code, reason, SEND_CONNECTION_CLOSE_PACKET)` を呼ぶ
- aioquic のエコーサーバー側で close 理由を受信し、テストで検証する
