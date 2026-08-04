# QuicClient コンストラクタの引数順序の罠を解消する

- Created: 2026-08-04
- Completed: {YYYY-MM-DD}
- Branch: feature/change-quic-client-init-alpn-order
- Polished: {YYYY-MM-DD}

## 目的

`QuicClient` コンストラクタの位置引数呼び出しで `alpn` と `server_name` を誤解釈する罠を解消する。

## 現状

`src/bindings/module.cpp` の `PyQuicClient` の初期化は次の引数順序である:

```
QuicClient(host, port, server_name="", alpn, verify_peer=true)
```

既定値を持つ `server_name` の後に必須の `alpn` が続く。`QuicClient(host, port, False)` のような位置引数呼び出しは `server_name=False` と解釈され TypeError になる。Python の通常の関数定義では書けないシグネチャであり、pyi（`quiche_ext.pyi`）も同じ順序で表現されるため型チェッカでも検出できない。

## 設計方針

`alpn` を `server_name` より前に置く（`host, port, alpn, server_name="", verify_peer=true`）。破壊的変更のため、利用者への影響を確認する。

## 完了条件

- 位置引数呼び出しで `alpn` と `server_name` を誤解釈しないシグネチャになること
- `AsyncQuicClient` 側のキーワード引数 API（`alpn` 必須、`server_name` 既定）と整合すること

## 解決方法

- `module.cpp` の `PyQuicClient` の引数順序を `alpn` を前にして変更し、pyi も更新する
- `src/quiche/aio.py` の `AsyncQuicClient` はキーワード引数で影響がないことを確認する
- テスト: 位置引数呼び出しとキーワード引数呼び出しの両方を検証するテストを追加する
