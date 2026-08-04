# WebTransport ストリームの補助 API（ReadableBytes / 内部リセット / 枠解放通知）を追加する

- Created: 2026-08-04
- Completed: {YYYY-MM-DD}
- Branch: feature/add-wt-stream-aux-apis
- Polished: {YYYY-MM-DD}

## 目的

WebTransport ストリームの読み取り量把握、内部エラー時のリセット、ストリーム枠の解放通知を Python から利用できるようにする。

## 現状

上流 `web_transport.h` の `Stream` インターフェースが公開している以下の API が未公開:

- `ReadableBytes()` — 読み取り可能な総バイト数。現在は常に最大バッファで読むため、必要バッファ量が分からない
- `ResetDueToInternalError()` — アプリ側のエラーコードが使えない場合の汎用リセット
- `MaybeResetDueToStreamObjectGone()` — ストリームオブジェクト破棄時のリセット

また `SessionVisitor::OnCanCreateNewOutgoingBidirectionalStream()` / `OnCanCreateNewOutgoingUnidirectionalStream()`（`web_transport.h`）はバインディング側で no-op のまま。現在は aio 層のポーリング（`src/quiche/aio.py` の `_open()` / `_accept()` のループ）で代替されており、ストリーム枠が解放されたタイミングをイベントとして受ける手段がない。

## 設計方針

ストリーム補助 API を C API で公開し、枠解放通知は `0008-add-wt-stream-event-api` のストリームイベント API と統合する形で設計する。

## 完了条件

- `ReadableBytes()` が Python から取得できること
- 内部エラー時のリセット手段が提供されること
- ストリーム枠解放がイベントとして観測できること（またはポーリングのままとする判断が明文化されること）

## 解決方法

- C API に `wt_client_stream_readable_bytes` / `wt_client_reset_stream_internal` 等を追加する
- 枠解放通知は `0008-add-wt-stream-event-api` の設計に含めて検討する（統合の判断は実装時に行う）
- `module.cpp` / `src/quiche/aio_wt.py` に公開する
- テスト: 各 API の呼び出しテストを追加
