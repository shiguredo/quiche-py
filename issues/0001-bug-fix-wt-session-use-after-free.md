# WebTransport クライアントのセッション破棄後 use-after-free

- Created: 2026-08-04
- Completed: {YYYY-MM-DD}
- Branch: feature/fix-wt-session-use-after-free
- Polished: {YYYY-MM-DD}

## 目的

WebTransport over HTTP/3 クライアントが、サーバー側からセッションがクローズされた後に破棄済みメモリへアクセスしてクラッシュまたはメモリ破壊を起こす use-after-free を修正する。

## 現状

`src/bindings/quiche_glue_wt.cc` の `quiche_py_wt_client` は `WebTransportHttp3*` と `WtSessionVisitor*` を raw ポインタで保持している。両者は CONNECT ストリーム（QuicSpdyStream）が所有するオブジェクトで、ストリームが閉じられると破棄される。

- 上流 `web_transport_http3.h` の「The session is owned by QuicSpdyStream object for the CONNECT stream that established it」
- ストリームクローズ時に `closed_streams_clean_up_alarm_` が現在時刻でセットされ、次の `ProcessAlarmsUpTo`（Python 側の `handle_timeout()`）でストリームごと `WebTransportHttp3` と visitor が delete される（上流 `quic_session.cc` の `CleanUpClosedStreams()`）

破棄後も以下の経路が dangling 参照に到達する:

- `IsSessionReady()` が `session_visitor_->session_ready()` を読む
- `Close()` が `web_transport_->CloseSession()` を呼ぶ
- `EnsureSessionReady()` 経由の `SendDatagram()` / `OpenStream()` / `AcceptStream()` / `PeekDatagramSize()` / `ReceiveDatagram()` / `MaxDatagramSize()` が `session_visitor_` または `web_transport_` を deref する

QUIC 接続自体は `connected()` のままなので、Python 側（`src/quiche/aio_wt.py` の `_wait(is_session_ready())` ポーリングと `close()`）は破棄済みメモリへの到達を続ける。raw QUIC クライアント（`quiche_glue.cc` の `quiche_py_client`）は visitor を構造体メンバとして所有しており（上流 `owns_visitor=false`）、この問題は WebTransport クライアントのみに存在する。

発生シナリオ: サーバーが CLOSE_WEBTRANSPORT_SESSION capsule を送る、または CONNECT を拒否して FIN でストリームを閉じる → 次の `handle_timeout()` でセッション破棄 → 以降の操作で UAF。

## 設計方針

visitor の所有権を `quiche_py_wt_client` 側へ移す、またはセッション終了（`OnSessionClosed`）を検知した時点で `web_transport_` / `session_visitor_` を確実に無効化する仕組みを導入し、破棄後アクセスが構造的に起きない設計にする。

## 完了条件

サーバーが CLOSE_WEBTRANSPORT_SESSION capsule または CONNECT 拒否（FIN）でセッションを閉じた後、`is_session_ready()` / `close()` / `send_datagram()` / `open_*_stream()` / `receive_datagram()` を呼んでもクラッシュ・メモリ破壊が発生しないこと。ASAN ビルドで検証できること。

## 解決方法

- `WtSessionVisitor` を `quiche_py_wt_client` が `std::unique_ptr` で所有し、破棄タイミングをクライアント側で制御する
- または `OnSessionClosed` 通知を契機にセッション終了フラグを立て、以降の操作を `QUICHE_PY_STATUS_CLOSED` で拒否する（フラグ更新とポインタ無効化を同一 mutex 内で行う）
- 併せて `src/quiche/aio_wt.py` 側でセッションクローズを検知して即 teardown する（関連: `0003-bug-fix-wt-session-rejection-detection`）
- 回帰テスト: セッションクローズ後の全操作を呼ぶテストを追加（aioquic には WebTransport 実装が無いため、検証用サーバーの同梱が必要。関連: `0012-test-add-webtransport-interop`）
