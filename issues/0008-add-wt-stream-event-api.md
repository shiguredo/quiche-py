# WebTransport ストリームのリセット / STOP_SENDING 通知 API を追加する

- Created: 2026-08-04
- Completed: {YYYY-MM-DD}
- Branch: feature/add-wt-stream-event-api
- Polished: {YYYY-MM-DD}

## 目的

ピアが WebTransport ストリームをリセット / STOP_SENDING したときに、その理由コードを Python 側から観測できるようにする。

## 現状

`src/bindings/quiche_glue_wt.cc` の `OpenStream()` / `AcceptStream()` はストリームに `StreamVisitor` を一切設定しない。上流は転送実装済みである:

- `web_transport.h` の `StreamVisitor::OnResetStreamReceived()` / `OnStopSendingReceived()` / `OnWriteSideInDataRecvdState()`
- 上流実装 `quic_spdy_stream.cc` の RESET_STREAM / STOP_SENDING / DataRecvd の visitor への転送

現在はピアがストリームをリセットしても `is_open()` が False に変わるか read が NOT_FOUND になるだけで、**エラーコードが永遠に観測できない**。draft-ietf-webtrans-http3 §4.3 は RESET_STREAM / STOP_SENDING の信号をアプリケーションへ伝播することを前提としており、理由コードが分からないとアプリのエラー処理（再送・代替ストリーム選択）ができない。

## 設計方針

glue に StreamVisitor 実装を追加し、ストリームイベント（リセット / STOP_SENDING / 書き込み側クローズ）をキューに積んで C API 経由で Python に公開する。ストリーム寿命（visitor はストリーム所有）に注意する。

## 完了条件

- ピアの RESET_STREAM / STOP_SENDING をエラーコード付きで Python から受信できること
- `WebTransportStream`（および可能なら `QuicStream`）にイベント取得 API が追加され、`AsyncWebTransportStream` 側でも利用できること

## 解決方法

- `StreamVisitor` 実装（イベントキュー）を追加し、`OpenStream()` / `AcceptStream()` で設定する
- C API にストリームイベント取得関数（例: `wt_client_take_stream_event`）を追加し、`module.cpp` で Python に公開する
- テスト: ピア側のリセットを観測するテストを追加（関連: `0013-test-add-stream-control-tests`）
