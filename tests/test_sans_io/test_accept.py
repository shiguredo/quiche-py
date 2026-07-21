"""aioquic からのサーバー起点ストリーム accept 相互運用 (sans-I/O 駆動)。"""

from e2e_support import (
    SERVER_BIDI_PUSH_PAYLOAD,
    SERVER_UNI_PUSH_PAYLOAD,
    PrimitiveDriver,
    connect_primitive,
    make_quic_client,
    read_until_fin_primitive,
)

import quiche


def test_accept_server_bidirectional_stream(push_server: int) -> None:
    # サーバー起点の双方向ストリームを accept し、push データを読めることを確認する。
    driver = PrimitiveDriver(make_quic_client(push_server), push_server)
    try:
        connect_primitive(driver)
        accepted: list[quiche.QuicStream] = []

        def got_stream() -> bool:
            stream = driver.client.accept_bidirectional_stream()
            if stream is None:
                return False
            accepted.append(stream)
            return True

        found = driver.pump(got_stream, timeout_s=5.0)
        assert found, "サーバー起点の双方向ストリームを時間内に accept できなかった"
        stream = accepted[0]
        assert stream.unidirectional is False
        assert stream.stream_id % 4 == 1

        data, finished = read_until_fin_primitive(driver, stream)
        assert finished, "fin を時間内に受け取れなかった"
        assert data == SERVER_BIDI_PUSH_PAYLOAD
    finally:
        driver.client.close()
        driver.close()


def test_accept_server_unidirectional_stream(push_server: int) -> None:
    # サーバー起点の単方向ストリームを accept し、push データを読めることを確認する。
    driver = PrimitiveDriver(make_quic_client(push_server), push_server)
    try:
        connect_primitive(driver)
        accepted: list[quiche.QuicStream] = []

        def got_stream() -> bool:
            stream = driver.client.accept_unidirectional_stream()
            if stream is None:
                return False
            accepted.append(stream)
            return True

        found = driver.pump(got_stream, timeout_s=5.0)
        assert found, "サーバー起点の単方向ストリームを時間内に accept できなかった"
        stream = accepted[0]
        assert stream.unidirectional is True
        assert stream.stream_id % 4 == 3

        data, finished = read_until_fin_primitive(driver, stream)
        assert finished, "fin を時間内に受け取れなかった"
        assert data == SERVER_UNI_PUSH_PAYLOAD
    finally:
        driver.client.close()
        driver.close()


def test_accept_both_server_streams_then_client_echo(push_server: int) -> None:
    # サーバー起点 bidi/uni を両方受けた後も、クライアント起点ストリームで
    # 通常の echo 相互運用が続くことを確認する。
    driver = PrimitiveDriver(make_quic_client(push_server), push_server)
    try:
        connect_primitive(driver)
        bidi_holder: list[quiche.QuicStream] = []
        uni_holder: list[quiche.QuicStream] = []

        def got_both() -> bool:
            if not bidi_holder:
                stream = driver.client.accept_bidirectional_stream()
                if stream is not None:
                    bidi_holder.append(stream)
            if not uni_holder:
                stream = driver.client.accept_unidirectional_stream()
                if stream is not None:
                    uni_holder.append(stream)
            return bool(bidi_holder) and bool(uni_holder)

        found = driver.pump(got_both, timeout_s=5.0)
        assert found, "サーバー起点ストリームを時間内に accept できなかった"

        bidi_data, bidi_fin = read_until_fin_primitive(driver, bidi_holder[0])
        uni_data, uni_fin = read_until_fin_primitive(driver, uni_holder[0])
        assert bidi_fin and uni_fin
        assert bidi_data == SERVER_BIDI_PUSH_PAYLOAD
        assert uni_data == SERVER_UNI_PUSH_PAYLOAD

        echo = driver.client.open_bidirectional_stream()
        assert echo is not None, "双方向ストリームを開けなかった"
        assert echo.write(b"after-accept", fin=True) is True
        data, finished = read_until_fin_primitive(driver, echo)
        assert finished, "fin を時間内に受け取れなかった"
        assert data == b"after-accept"
    finally:
        driver.client.close()
        driver.close()
