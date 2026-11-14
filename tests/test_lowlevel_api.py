"""低レベル API の統計・設定・バッチ受信・atomic write の E2E テスト。"""

import pytest
from e2e_support import LowLevelDriver, make_quic_client

import quiche


def _read_until_fin(
    driver: LowLevelDriver,
    stream: quiche.QuicStream,
    timeout_s: float = 5.0,
) -> tuple[bytes, bool]:
    """fin を受け取るまで読み出して連結する。"""
    chunks = bytearray()

    def got_fin() -> bool:
        result = stream.read(65536)
        if result is None:
            return False
        data, fin = result
        chunks.extend(data)
        return fin

    finished = driver.pump(got_fin, timeout_s=timeout_s)
    return bytes(chunks), finished


def test_quic_stats_and_batch_receive(echo_server: int) -> None:
    """
    複数パケット受信 API でハンドシェイクを駆動し、QUIC 統計が取得できることを確認する。
    """
    client = make_quic_client(echo_server)
    driver = LowLevelDriver(client, echo_server)
    try:
        client.start()
        assert driver.pump(client.is_connected, timeout_s=10.0)

        stats = client.stats()
        assert isinstance(stats, quiche.QuicStats)
        assert stats.packets_sent > 0
        assert stats.packets_received > 0
        assert stats.bytes_sent > 0
        assert stats.bytes_received > 0
        assert stats.min_rtt_us >= 0
        assert stats.srtt_us >= 0
        assert stats.packets_dropped >= 0

        # Sans-I/O では Python 側がソケットを所有するため、ソケット溢れ数は無い。
        assert client.socket_packets_dropped() is None
    finally:
        client.close()
        driver.close()


def test_quic_configure_before_handshake(echo_server: int) -> None:
    """
    ハンドシェイク前に設定したトランスポートパラメータで接続できることを確認する。
    """
    client = make_quic_client(echo_server)
    client.configure(
        max_idle_timeout_ms=30000,
        initial_max_data=1 << 20,
        initial_max_stream_data_bidi_local=1 << 18,
        initial_max_stream_data_bidi_remote=1 << 18,
        initial_max_stream_data_uni=1 << 18,
        max_ack_delay_ms=25,
        min_ack_delay_us=1000,
        active_connection_id_limit=4,
        max_datagram_frame_size=1200,
    )
    driver = LowLevelDriver(client, echo_server)
    try:
        client.start()
        assert driver.pump(client.is_connected, timeout_s=10.0)

        # ハンドシェイク開始後の設定変更は受け付けない。
        with pytest.raises(ValueError):
            client.configure(max_idle_timeout_ms=60000)
    finally:
        client.close()
        driver.close()


def test_quic_atomic_write_and_priority(echo_server: int) -> None:
    """
    atomic_write と send group / send order の設定が低レベルストリームで動くことを確認する。
    """
    client = make_quic_client(echo_server)
    driver = LowLevelDriver(client, echo_server)
    try:
        client.start()
        assert driver.pump(client.is_connected, timeout_s=10.0)
        stream = client.open_bidirectional_stream()
        assert stream is not None, "双方向ストリームを開けなかった"
        stream.set_priority(1, 2)

        payload = b"low-level atomic write"
        assert stream.atomic_write(payload, fin=True) is True
        data, finished = _read_until_fin(driver, stream)
        assert finished, "fin を時間内に受け取れなかった"
        assert data == payload
    finally:
        client.close()
        driver.close()


def _submit_http3_request(client: quiche.Http3Client, driver: LowLevelDriver) -> int:
    """HTTP/3 リクエストを送信し、ストリーム ID を返す。"""
    headers = [
        (b":method", b"GET"),
        (b":scheme", b"https"),
        (b":authority", b"localhost"),
        (b":path", b"/"),
    ]
    stream_id = client.submit_request(headers, b"", True)
    if stream_id is not None:
        return stream_id

    def submit() -> bool:
        nonlocal stream_id
        stream_id = client.submit_request(headers, b"", True)
        return stream_id is not None

    assert driver.pump(submit, timeout_s=5.0), "HTTP/3 リクエストを送信できなかった"
    assert stream_id is not None
    return stream_id


def test_http3_stats_and_settings(http3_server: int) -> None:
    """
    HTTP/3 の SETTINGS と受信順、および QUIC 統計が低レベル API から取得できることを確認する。
    """
    client = quiche.Http3Client("127.0.0.1", http3_server, verify_peer=False)
    client.configure(max_idle_timeout_ms=30000)
    driver = LowLevelDriver(client, http3_server)
    try:
        client.start()
        assert driver.pump(client.is_connected, timeout_s=10.0)
        assert driver.pump(client.settings_received, timeout_s=10.0)

        settings = client.settings()
        wire_order = client.settings_wire_order()
        assert settings, "HTTP/3 SETTINGS が空だった"
        assert wire_order, "HTTP/3 SETTINGS の受信順が空だった"
        assert set(wire_order) == set(settings)

        stream_id = _submit_http3_request(client, driver)
        responses: list[quiche.Http3Response] = []

        def response_ready() -> bool:
            response = client.take_response(stream_id)
            if response is None:
                return False
            responses.append(response)
            return True

        assert driver.pump(response_ready, timeout_s=10.0), "HTTP/3 応答が届かなかった"
        response = responses[0]
        assert response.status_code == 200
        assert response.body == b"hello from aioquic h3"

        stats = client.stats()
        assert isinstance(stats, quiche.QuicStats)
        assert stats.packets_sent > 0
        assert stats.packets_received > 0
        assert client.socket_packets_dropped() is None
    finally:
        client.close()
        driver.close()
