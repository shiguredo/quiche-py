"""aioquic との QUIC ハンドシェイク相互運用 (sans-I/O 駆動)。"""

from e2e_support import ALPN, PrimitiveDriver, connect_primitive, make_quic_client

import quiche


def test_handshake_establishes_connection(echo_server: int) -> None:
    # ALPN 合意付きでハンドシェイクが完了し、接続状態になることを確認する。
    driver = PrimitiveDriver(make_quic_client(echo_server), echo_server)
    try:
        connect_primitive(driver)
        assert driver.client.is_connected()
        assert driver.client.last_error == ""
    finally:
        driver.client.close()
        driver.close()


def test_handshake_with_server_name(echo_server: int) -> None:
    # SNI (server_name) を明示しても aioquic とハンドシェイクできることを確認する。
    client = quiche.QuicClient(
        "127.0.0.1",
        echo_server,
        server_name="localhost",
        alpn=ALPN,
        verify_peer=False,
    )
    driver = PrimitiveDriver(client, echo_server)
    try:
        connect_primitive(driver)
        assert driver.client.is_connected()
    finally:
        driver.client.close()
        driver.close()


def test_concurrent_clients(echo_server: int) -> None:
    # 同一サーバーに対する独立した 2 接続が同時に確立できることを確認する。
    first = PrimitiveDriver(make_quic_client(echo_server), echo_server)
    second = PrimitiveDriver(make_quic_client(echo_server), echo_server)
    try:
        connect_primitive(first)
        connect_primitive(second)
        assert first.client.is_connected()
        assert second.client.is_connected()
    finally:
        second.client.close()
        first.client.close()
        second.close()
        first.close()


def test_alpn_mismatch_fails_handshake(echo_server: int) -> None:
    # ALPN 不一致では接続が確立しないことを確認する。
    driver = PrimitiveDriver(make_quic_client(echo_server, alpn="not-a-real-alpn"), echo_server)
    try:
        driver.client.start()
        connected = driver.pump(driver.client.is_connected, timeout_s=3.0)
        assert connected is False
        assert driver.client.is_connected() is False
    finally:
        driver.client.close()
        driver.close()
