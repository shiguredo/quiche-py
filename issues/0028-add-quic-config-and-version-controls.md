# QUIC のバージョン制御と設定チューニング API を追加する

- Created: 2026-08-04
- Completed: {YYYY-MM-DD}
- Branch: feature/add-quic-config-and-version-controls
- Polished: {YYYY-MM-DD}

## 目的

QUIC クライアントのバージョンネゴシエーション制御、接続設定のチューニング、preferred address の挙動を Python から制御できるようにする。

## 現状

- バージョン制御: raw QUIC は `GetQuicVersionsForGenericSession()` でバージョンが固定される（上流 `quic_client_base.h` の `SetSupportedVersions()` が未公開）。HTTP/3 / WebTransport は `CurrentSupportedHttp3Versions()` 固定
- 設定チューニング: `set_initial_max_packet_length` 等の QuicConfig 調整手段が未公開。keepalive は上流の generic session で常時有効（`quic_generic_session.h`）だが、その挙動の制御手段がない
- preferred address: WebTransport は `allow_server_preferred_address = false` を明示（`quiche_glue_wt.cc` の `WtSpdyClientSession`）だが、raw QUIC は上流デフォルトの移行ロジック（`quic_client_base.cc` の `OnServerPreferredAddressAvailable()`）が走る。sans-I/O ヘルパ（`quiche_glue_sans_io.h` の `SansIoNetworkHelper`）が疑似ソケット成功を返すため、preferred address 宛のプローブが元アドレスへ送られる誤経路になり得る

## 設計方針

クライアント生成オプションにバージョン指定・設定項目を追加する。preferred address の挙動は raw と WT で統一する（無効化または対応）。

## 完了条件

- 接続で使う QUIC バージョンを Python から指定できること（または意図的な固定であることが明文化されること）
- preferred address の挙動が raw と WebTransport で一致すること

## 解決方法

- `quiche_py_client_options` にバージョン指定（または固定バージョン選択）を追加する
- `QuicClientBase::SetSupportedVersions()` を C API 経由で公開する
- raw QUIC クライアントの接続設定（`QuicConfig`）を `allow_server_preferred_address = false` にするか、preferred address 移行を sans-I/O で正しく扱う
- テスト: バージョン指定と preferred address の挙動を検証する
