# module.cpp の 3 クライアントクラスの重複を共通化する

- Created: 2026-08-04
- Completed: {YYYY-MM-DD}
- Branch: feature/refactor-merge-module-common
- Polished: {YYYY-MM-DD}

## 目的

`src/bindings/module.cpp`（1,064 行）の 3 クライアントクラス間の重複を共通化し、可読性と保守性を向上させる。

## 現状

`module.cpp` に以下の重複がある:

- `ThrowIfNotOk()` / `ThrowStatus()` が `PyQuicClient` / `PyHttp3Client` / `PyWebTransportClient` の 3 クラスで完全同一
- `PyQuicStream` と `PyWebTransportStream` の実装が完全同一
- `OpenStream()` / `AcceptStream()` が `PyQuicClient` / `PyWebTransportClient` で同一
- `receive_datagram()` が `PyQuicClient` / `PyWebTransportClient` で同一
- `submit_request()` と `connect_session()` のヘッダ変換ループ（`owned_names` / `owned_values` へのコピー）が同一

ファイルは 1,064 行に肥大化しており、クラスごとの差分（どの C API 関数を呼ぶか）だけが本質である。

## 設計方針

- `ThrowStatus` は last_error 取得をコールバック化したフリー関数に共通化する
- ストリーム・ヘッダ変換はテンプレートで共通化する

## 完了条件

- 3 クラス間の重複が共通コードに集約され、ファイルサイズが削減されること
- 既存テストがすべて通ること

## 解決方法

- 共通ヘルパ（フリー関数 / テンプレート）を module.cpp 内に整理する
- ストリームクラスをテンプレート化（C API 関数テーブルを引数に取る）する
- 挙動を変えずにリファクタリングし、テストで回帰がないことを確認する
