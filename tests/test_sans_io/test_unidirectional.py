"""aioquic との単方向ストリーム相互運用 (sans-I/O 駆動)。

クライアント起点単方向は同一 stream に返信できないため、エコーサーバーは
サーバー起点単方向へ載せ替えて返す。
"""

from e2e_support import (
    PrimitiveDriver,
    connect_primitive,
    make_quic_client,
    read_until_fin_primitive,
)

import quiche


def test_unidirectional_echo_via_server_stream(echo_server: int) -> None:
    # クライアント起点 uni へ書いたデータが、サーバー起点 uni として戻ることを確認する。
    driver = PrimitiveDriver(make_quic_client(echo_server), echo_server)
    try:
        connect_primitive(driver)

        outbound = driver.client.open_unidirectional_stream()
        assert outbound is not None, "単方向ストリームを開けなかった"
        assert outbound.unidirectional is True
        assert outbound.stream_id % 4 == 2
        assert outbound.write(b"uni payload over quic", fin=True) is True

        accepted: list[quiche.QuicStream] = []

        def got_inbound() -> bool:
            stream = driver.client.accept_unidirectional_stream()
            if stream is None:
                return False
            accepted.append(stream)
            return True

        found = driver.pump(got_inbound, timeout_s=5.0)
        assert found, "サーバー起点の単方向ストリームを時間内に accept できなかった"
        inbound = accepted[0]
        assert inbound.unidirectional is True
        assert inbound.stream_id % 4 == 3

        data, finished = read_until_fin_primitive(driver, inbound)
        assert finished, "fin を時間内に受け取れなかった"
        assert data == b"uni payload over quic"
    finally:
        driver.client.close()
        driver.close()
