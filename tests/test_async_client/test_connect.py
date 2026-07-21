"""aioquic との QUIC ハンドシェイク相互運用。"""

import pytest
from e2e_support import connect_async

import quiche


async def test_handshake_establishes_connection(echo_server: int) -> None:
    # ALPN 合意付きでハンドシェイクが完了し、接続状態になることを確認する。
    client = await connect_async(echo_server)
    try:
        assert client.is_connected()
        assert client.last_error == ""
    finally:
        await client.close()


async def test_handshake_with_server_name(echo_server: int) -> None:
    # SNI (server_name) を明示しても aioquic とハンドシェイクできることを確認する。
    client = await connect_async(echo_server, server_name="localhost")
    try:
        assert client.is_connected()
    finally:
        await client.close()


async def test_concurrent_clients(echo_server: int) -> None:
    # 同一サーバーに対する独立した 2 接続が同時に確立できることを確認する。
    first = await connect_async(echo_server)
    second = await connect_async(echo_server)
    try:
        assert first.is_connected()
        assert second.is_connected()
    finally:
        await second.close()
        await first.close()


async def test_alpn_mismatch_fails_handshake(echo_server: int) -> None:
    # ALPN 不一致時は timeout ではなく ConnectionError で失敗することを確認する。
    client = quiche.AsyncQuicClient(
        "127.0.0.1",
        echo_server,
        alpn="not-a-real-alpn",
        verify_peer=False,
    )
    with pytest.raises(ConnectionError):
        await client.connect(timeout=10.0)
    assert client.is_connected() is False
