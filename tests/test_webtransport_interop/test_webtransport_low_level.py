"""WebTransport 低レベル API の相互運用テスト。"""

from e2e_support import LowLevelDriver

import quiche


def test_webtransport_low_level_api(wt_server: int) -> None:
    """
    WebTransport の統計・protocol・atomic write・priority・draining を低レベル API から使えることを確認する。
    """
    client = quiche.WebTransportClient("127.0.0.1", wt_server, verify_peer=False)
    client.configure(max_idle_timeout_ms=30000)
    driver = LowLevelDriver(client, wt_server)
    try:
        client.start()
        assert driver.pump(client.is_connected, timeout_s=10.0)
        assert driver.pump(lambda: client.connect_session("/") is True, timeout_s=10.0)
        assert driver.pump(client.is_session_ready, timeout_s=10.0)

        stats = client.stats()
        assert isinstance(stats, quiche.QuicStats)
        assert stats.packets_sent > 0
        assert stats.packets_received > 0
        assert client.socket_packets_dropped() is None

        session_stats = client.session_stats()
        assert isinstance(session_stats, quiche.WebTransportSessionStats)
        assert session_stats.min_rtt_us >= 0
        assert session_stats.smoothed_rtt_us >= 0
        assert session_stats.rtt_variation_us >= 0
        assert session_stats.application_bytes_acknowledged >= 0
        assert session_stats.estimated_send_rate_bps >= 0

        datagram_stats = client.datagram_stats()
        assert isinstance(datagram_stats, quiche.WebTransportDatagramStats)
        assert datagram_stats.expired_outgoing >= 0
        assert datagram_stats.lost_outgoing >= 0

        assert client.is_draining() is False
        protocol = client.protocol
        assert protocol is None or isinstance(protocol, str)

        stream = client.open_bidirectional_stream()
        assert stream is not None, "双方向ストリームを開けなかった"
        stream.set_priority(1, 2)

        payload = b"low-level webtransport"
        assert stream.atomic_write(payload, fin=True) is True

        chunks = bytearray()

        def got_fin() -> bool:
            result = stream.read(65536)
            if result is None:
                return False
            data, fin = result
            chunks.extend(data)
            return fin

        assert driver.pump(got_fin, timeout_s=10.0), "fin を時間内に受け取れなかった"
        assert bytes(chunks) == payload

        # DRAIN 通知は送信できればよく、このクライアント側の状態は変えない。
        client.notify_draining()
    finally:
        client.close()
        driver.close()
