# QUIC クライアントの生成オプション (UserAgentID / TLS 署名アルゴリズム) を公開する

- Created: 2026-08-05
- Completed: {YYYY-MM-DD}
- Branch: feature/add-quic-client-settings
- Polished: {YYYY-MM-DD}

## 目的

QUIC クライアントの接続設定を Python から指定できるようにする。UserAgentID はサーバー側のログ解析・統計に使われ、TLS 署名アルゴリズムはサーバー証明書が特定の署名アルゴリズム (例: RSA-PSS のみ) しか受け付けない環境で必要になる。

## 現状

上流 `quic_client_base.h` の `QuicClientBase::SetUserAgentID()` / `SetTlsSignatureAlgorithms()` がバインディングで未公開。`quiche_glue.h` の `quiche_py_client_options` には該当フィールドがなく、`src/bindings/module.cpp` の `PyQuicClient` のコンストラクタ引数でも指定できない。

## 設計方針

`quiche_py_client_options` に `user_agent_id` / `tls_signature_algorithms` フィールドを追加し、`GoogleQuichePyTransportClient` 生成時に `SetUserAgentID()` / `SetTlsSignatureAlgorithms()` を呼ぶ。`PyQuicClient` のコンストラクタに任意引数として公開する。

## 完了条件

- Python から UserAgentID / TLS 署名アルゴリズムを指定して接続できること

## 解決方法

- `src/bindings/quiche_glue.h`: `quiche_py_client_options` にフィールドを追加する
- `src/bindings/quiche_glue.cc`: `quiche_py_client` のコンストラクタで受け取り、`QuicClientBase` のセッターを呼ぶ
- `src/bindings/module.cpp`: `PyQuicClient` のコンストラクタに任意引数を追加する
- テスト: 指定した値が TLS ハンドシェイクに反映されることを検証する (UserAgentID は `client_hello` の UA フィールド、TLS 署名アルゴリズムは証明書検証時の署名スキーム)
