"""WebTransportClient の基本動作テスト。

aioquic 側の WT サーバー相互運用は相手実装依存が大きいため、ここでは
クライアント生成と未接続時の失敗経路を確認する。
"""

import quiche


def test_webtransport_client_create() -> None:
    # ハンドル生成時点では接続していない。
    client = quiche.WebTransportClient("127.0.0.1", 4433, verify_peer=False)
    assert client.is_connected() is False
    assert client.is_session_ready() is False
    assert client.last_error == ""


def test_webtransport_connect_session_before_start_returns_false() -> None:
    # start 前の connect_session はストリーム枠待ち扱いで False を返す。
    client = quiche.WebTransportClient("127.0.0.1", 4433, verify_peer=False)
    assert client.connect_session("/") is False
