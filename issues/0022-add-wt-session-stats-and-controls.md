# WebTransport セッションの統計・制御 API を追加する

- Created: 2026-08-04
- Completed: {YYYY-MM-DD}
- Branch: feature/add-wt-session-stats-and-controls
- Polished: {YYYY-MM-DD}

## 目的

WebTransport セッションの統計情報取得とセッション制御（datagram 滞留時間・draining・subprotocol）を Python から利用できるようにする。

## 現状

上流 `web_transport.h` の `Session` インターフェースが公開している以下の API がバインディングで未公開:

- `GetDatagramStats()` / `GetSessionStats()` — RTT / 送信レート / datagram 喪失数等
- `SetDatagramMaxTimeInQueue()` — 輻輳時 datagram キュー滞留時間の制御
- `NotifySessionDraining()` / `SetOnDraining()` — draft-ietf-webtrans-http3 §4.6 の DRAIN_WEBTRANSPORT_SESSION capsule 送受信
- `GetNegotiatedSubprotocol()` — 交渉された subprotocol の取得
- `GetPerspective()` / `GetUnderlyingProtocol()`

サーバーからの GOAWAY / drain 通知も観測不能（上流 `web_transport_http3.cc` の `OnGoAwayReceived` 相当が未公開）。`SetDatagramMaxTimeInQueue` は `quic_generic_session.h` にも対応実装がある。

## 設計方針

上流 API を C API で公開し、`WebTransportClient`（`AsyncWebTransportClient`）に追加する。各 API の公開可否と Python 側のシグネチャを決定する。

## 完了条件

- 統計情報（RTT / 送信レート / datagram 統計）が Python から取得できること
- datagram 滞留時間の上限設定、draining 通知の受信、subprotocol の取得ができること

## 解決方法

- C API に `wt_client_get_session_stats` / `wt_client_get_datagram_stats` / `wt_client_set_datagram_max_time_in_queue` / `wt_client_get_negotiated_subprotocol` 等を追加する
- draining 通知は SessionVisitor のコールバックをキューに積んで公開する
- `module.cpp` / `src/quiche/aio_wt.py` に公開する
- テスト: セッション ready 後の統計取得・設定の呼び出しテストを追加
