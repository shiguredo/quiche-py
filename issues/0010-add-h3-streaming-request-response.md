# HTTP/3 のストリーミング送受信と trailers を追加する

- Created: 2026-08-04
- Completed: {YYYY-MM-DD}
- Branch: feature/add-h3-streaming-request-response
- Polished: {YYYY-MM-DD}

## 目的

HTTP/3 クライアントで、リクエストボディの分割送信（ストリーミング）、レスポンスの部分受信、trailers の送受信を可能にする。

## 現状

`src/bindings/quiche_glue_http3.cc` の `SubmitRequest()` はヘッダと body を一括で `QuicSpdyClientStream::SendRequest()` に渡し、以降そのストリームに書き込む手段がない。`fin=False` を渡すと送信しっぱなしで永遠に閉じられない宙吊りストリームになる。

- 上流 `quic_spdy_stream.h` の `WriteOrBufferBody()` / `WriteTrailers()` / `OnBodyAvailable()` が未公開
- `ResponseQueue::OnCompleteResponse()` はレスポンス完了まで全ボディをバッファするため、巨大レスポンスでメモリが無制限に膨らみ、TTFB（最初のバイトまでの時間）も観測できない
- `CompletedHttp3Response` に trailers フィールドがなく、`received_trailers()` も非公開

大容量アップロード（動画・ファイル）や trailers でのメタデータ送信（gRPC のステータス等）が実現できない。

## 設計方針

ストリームハンドルを返す API に変更し、`WriteOrBufferBody()` / `WriteTrailers()` を公開する。レスポンスは完了時だけでなくボディ到着の都度取り出せるようにする（部分ボディのキュー配信）。

## 完了条件

- リクエストボディを分割して送信し、途中で `fin` を付けられること
- レスポンスボディを完了前に部分受信できること
- trailers の送受信ができること

## 解決方法

- `SubmitRequest()` の戻り値を stream_id に加えてストリーム書き込み手段（`h3_client_write_request_body` 等）を追加する
- レスポンスを `ResponseListener` 方式から部分配信方式（ボディチャンクキュー）へ変更する
- trailers を `CompletedHttp3Response`（または部分レスポンス構造）に追加する
- `module.cpp` / `src/quiche/aio_http3.py` へ公開し、`request()` の大ボディ・分割送信テストを追加する
