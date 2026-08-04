# HTTP/3 リクエスト / レスポンスの trailers をサポートする

- Created: 2026-08-05
- Completed: {YYYY-MM-DD}
- Branch: feature/add-h3-trailers
- Polished: {YYYY-MM-DD}

## 目的

HTTP/3 の trailers (trailer section) を Python から利用できるようにする。現状は trailers が完全に無視されており、サーバーが送る trailers (例: リクエスト処理完了後に確定するメタデータ) を受信できない。

## 現状

上流に実装がある trailers 関連 API がバインディングで未公開:

- 受信: `quic_spdy_stream.h` の `received_trailers()` / `OnTrailingHeadersComplete()`。`src/bindings/quiche_glue_http3.cc` の `ResponseQueue::OnCompleteResponse()` はレスポンス headers と body のみを `CompletedHttp3Response` に格納し、trailers は破棄される
- 送信: `quic_spdy_stream.h` の `WriteTrailers()`。`submit_request()` (`src/bindings/quiche_glue_http3.cc` の `SubmitRequest`) は `SendRequest()` しか呼ばず、trailers 送信手段がない

上流の `quic_spdy_client_base.h` は `latest_response_trailers_` を保持しているが、これは `store_response_` 時のみの内部実装であり、`ResponseListener::OnCompleteResponse()` には trailers が渡されない。

## 設計方針

- 受信: `CompletedHttp3Response` に trailers フィールドを追加し、レスポンスに含める。上流の `ResponseListener` に trailers を渡す経路がないため、`quiche_glue_http3.cc` 側でレスポンス完了時に `QuicSpdyClientStream::received_trailers()` を取得して格納する
- 送信: `submit_request()` に trailers 引数 (任意) を追加し、`WriteTrailers()` を呼ぶ。trailers 送信前に body の FIN を送らない順序制約 (`quic_spdy_stream.cc` の `WriteTrailers()` のコメント: trailers は FIN の前に送ること) に注意する

## 完了条件

- レスポンスの trailers が Python から取得できること
- リクエストに trailers を付与して送信できること

## 解決方法

- `src/bindings/quiche_glue_http3.cc`: `CompletedHttp3Response` に trailers を追加し、C API (`quiche_py_h3_response_*`) に trailers アクセサを追加する
- `src/bindings/module.cpp`: `PyHttp3Response` に trailers プロパティを追加し、`submit_request()` に trailers 引数を追加する
- テスト: テスト用 HTTP/3 サーバーで trailers 付きレスポンス / リクエストを検証する
