"""aioquic との QUIC datagram 相互運用 (sans-I/O 駆動)。"""

from e2e_support import (
    SAFE_DATAGRAM_SIZE,
    PrimitiveDriver,
    connect_primitive,
    make_quic_client,
)


def test_datagram_echo(echo_server: int) -> None:
    # DATAGRAM フレームが有効で、送ったペイロードが echo されて戻ることを確認する。
    driver = PrimitiveDriver(make_quic_client(echo_server), echo_server)
    try:
        connect_primitive(driver)
        assert driver.client.max_datagram_size() > 0

        payload = b"sans-io datagram payload"
        assert driver.client.send_datagram(payload) is True

        received: list[bytes] = []

        def got_datagram() -> bool:
            datagram = driver.client.receive_datagram()
            if datagram is None:
                return False
            received.append(datagram)
            return True

        echoed = driver.pump(got_datagram, timeout_s=5.0)
        assert echoed, "datagram の echo を時間内に受け取れなかった"
        assert received[0] == payload
    finally:
        driver.client.close()
        driver.close()


def test_datagram_echo_multiple(echo_server: int) -> None:
    # 連続送信した複数 datagram が欠けずに戻ることを確認する。
    driver = PrimitiveDriver(make_quic_client(echo_server), echo_server)
    try:
        connect_primitive(driver)
        payloads = [f"dgram-{index}".encode() for index in range(5)]
        for payload in payloads:
            assert driver.client.send_datagram(payload) is True

        received: list[bytes] = []

        def got_all() -> bool:
            while True:
                datagram = driver.client.receive_datagram()
                if datagram is None:
                    break
                received.append(datagram)
            return len(received) >= len(payloads)

        echoed = driver.pump(got_all, timeout_s=5.0)
        assert echoed, "datagram の echo を時間内に受け取れなかった"
        assert received == payloads
    finally:
        driver.client.close()
        driver.close()


def test_datagram_echo_near_max_size(echo_server: int) -> None:
    # 実効上限付近のサイズでも datagram が往復することを確認する。
    driver = PrimitiveDriver(make_quic_client(echo_server), echo_server)
    try:
        connect_primitive(driver)
        payload = b"x" * SAFE_DATAGRAM_SIZE
        assert driver.client.send_datagram(payload) is True

        received: list[bytes] = []

        def got_datagram() -> bool:
            datagram = driver.client.receive_datagram()
            if datagram is None:
                return False
            received.append(datagram)
            return True

        echoed = driver.pump(got_datagram, timeout_s=5.0)
        assert echoed, "datagram の echo を時間内に受け取れなかった"
        assert received[0] == payload
    finally:
        driver.client.close()
        driver.close()
