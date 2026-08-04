# WebTransport の receive_datagram が TOO_BIG 時に datagram を破棄する

- Created: 2026-08-04
- Completed: {YYYY-MM-DD}
- Branch: feature/fix-wt-receive-datagram-loss
- Polished: {YYYY-MM-DD}

## 目的

`WebTransportClient.receive_datagram()` でバッファ不足（TOO_BIG）が発生した場合に、先頭 datagram を失わないようにする。

## 現状

`src/bindings/quiche_glue_wt.cc` の `ReceiveDatagram()` は `TakeDatagram()` をサイズ検証より先に呼ぶため、バッファ不足時は datagram がキューから除去された後に `QUICHE_PY_STATUS_TOO_BIG` が返り、**データが黙って失われる**。raw QUIC 側（`quiche_glue.cc` の `ReceiveDatagram()`）はサイズ検証を先に行う正しい実装で、TOO_BIG 時はキューに残る。

現状の `module.cpp` の `receive_datagram()` は peek で正確なサイズを確保してから受信するため単一スレッドでは到達しないが、free-threading（PEP 703）対応を謳う本プロジェクトでは、複数スレッドが peek / receive を挟むと先頭が入れ替わり到達し得る。C API の契約としても「TOO_BIG は再試行を期待するエラー」であり、2 実装の非対称は不正。

## 設計方針

検証と `TakeDatagram()` の順序を入れ替え、raw QUIC 実装に揃える。

## 完了条件

- バッファ不足時に TOO_BIG が返り、datagram がキューに残って再試行で受信できること
- raw QUIC と WebTransport で挙動が一致すること

## 解決方法

- `quiche_glue_wt.cc` の `ReceiveDatagram()` で、サイズ検証を先に行い、超過時は `TakeDatagram()` を呼ばないよう順序を入れ替える
- テスト: バッファ不足時の再試行を検証するテストを追加（関連: `0014-test-add-error-path-and-close`）
