# HTTP/3 のセッション情報（GOAWAY / 1xx / ALPS）を公開する

- Created: 2026-08-04
- Completed: {YYYY-MM-DD}
- Branch: feature/add-h3-session-information
- Polished: {YYYY-MM-DD}

## 目的

HTTP/3 サーバーからの GOAWAY 受信、1xx 中間レスポンス、ALPS の情報を Python から取得できるようにする。

## 現状

上流に実装がある以下のセッション情報がバインディングで未公開:

- `goaway_received()`（`quic_spdy_client_base.h`）— サーバーからの GOAWAY 受信の検知
- `preliminary_headers()`（同）— 1xx 中間レスポンス
- ALPS（Application-Layer Protocol Settings、`quic_spdy_session.h`）— アプリケーション層プロトコル設定

サーバー側の運用（GOAWAY による接続の段階的終了、ロードバランサの設定伝達等）をクライアントから観測できない。

## 設計方針

上流 API を C API で公開し、`Http3Client`（`AsyncHttp3Client`）に追加する。各情報の公開形式を決定する。

## 完了条件

- GOAWAY 受信を検知して Python から取得できること
- 1xx 中間レスポンス / ALPS の情報が取得できること

## 解決方法

- C API に `h3_client_goaway_received` / `h3_client_preliminary_headers` / ALPS 取得関数を追加する
- `module.cpp` / `src/quiche/aio_http3.py` に公開する
- テスト: aioquic サーバーに GOAWAY / 1xx を送らせて検証する（aioquic の対応状況を調査してから決定）
