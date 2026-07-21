"""aioquic との双方向ストリーム相互運用 (sans-I/O 駆動)。"""

from e2e_support import (
    PrimitiveDriver,
    connect_primitive,
    make_quic_client,
    read_until_fin_primitive,
)


def test_bidirectional_echo(echo_server: int) -> None:
    # クライアント起点の双方向ストリームでデータ + fin が往復することを確認する。
    driver = PrimitiveDriver(make_quic_client(echo_server), echo_server)
    try:
        connect_primitive(driver)
        stream = driver.client.open_bidirectional_stream()
        assert stream is not None, "双方向ストリームを開けなかった"
        assert stream.stream_id % 4 == 0
        assert stream.unidirectional is False

        payload = b"hello over sans-io quic"
        assert stream.write(payload, fin=True) is True

        data, finished = read_until_fin_primitive(driver, stream)
        assert finished, "fin を時間内に受け取れなかった"
        assert data == payload
    finally:
        driver.client.close()
        driver.close()


def test_bidirectional_echo_empty_fin(echo_server: int) -> None:
    # 空データ + fin だけでも STREAM の終端が往復することを確認する。
    driver = PrimitiveDriver(make_quic_client(echo_server), echo_server)
    try:
        connect_primitive(driver)
        stream = driver.client.open_bidirectional_stream()
        assert stream is not None, "双方向ストリームを開けなかった"
        assert stream.write(b"", fin=True) is True

        data, finished = read_until_fin_primitive(driver, stream)
        assert finished, "fin を時間内に受け取れなかった"
        assert data == b""
    finally:
        driver.client.close()
        driver.close()


def test_bidirectional_echo_chunked(echo_server: int) -> None:
    # fin なしの分割書き込み後に fin を付けても、結合されて echo されることを確認する。
    driver = PrimitiveDriver(make_quic_client(echo_server), echo_server)
    try:
        connect_primitive(driver)
        stream = driver.client.open_bidirectional_stream()
        assert stream is not None, "双方向ストリームを開けなかった"
        assert stream.write(b"chunk-a-", fin=False) is True
        assert stream.write(b"chunk-b", fin=True) is True

        data, finished = read_until_fin_primitive(driver, stream)
        assert finished, "fin を時間内に受け取れなかった"
        assert data == b"chunk-a-chunk-b"
    finally:
        driver.client.close()
        driver.close()


def test_bidirectional_echo_large_payload(echo_server: int) -> None:
    # 複数パケットに跨り得るサイズでもストリーム往復できることを確認する。
    driver = PrimitiveDriver(make_quic_client(echo_server), echo_server)
    try:
        connect_primitive(driver)
        stream = driver.client.open_bidirectional_stream()
        assert stream is not None, "双方向ストリームを開けなかった"
        payload = bytes(range(256)) * 32  # 8 KiB
        assert stream.write(payload, fin=True) is True

        data, finished = read_until_fin_primitive(driver, stream)
        assert finished, "fin を時間内に受け取れなかった"
        assert data == payload
    finally:
        driver.client.close()
        driver.close()


def test_multiple_bidirectional_streams(echo_server: int) -> None:
    # 複数の双方向ストリームが独立して往復することを確認する。
    driver = PrimitiveDriver(make_quic_client(echo_server), echo_server)
    try:
        connect_primitive(driver)
        payloads = [f"sans-io-stream-{index}".encode() for index in range(4)]
        streams = []
        for payload in payloads:
            stream = driver.client.open_bidirectional_stream()
            assert stream is not None, "双方向ストリームを開けなかった"
            assert stream.write(payload, fin=True) is True
            streams.append(stream)

        for stream, payload in zip(streams, payloads, strict=True):
            data, finished = read_until_fin_primitive(driver, stream)
            assert finished, "fin を時間内に受け取れなかった"
            assert data == payload
    finally:
        driver.client.close()
        driver.close()
