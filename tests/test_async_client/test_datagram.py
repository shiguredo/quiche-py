"""aioquic との QUIC datagram 相互運用。"""

from e2e_support import SAFE_DATAGRAM_SIZE, connect_async


async def test_datagram_echo(echo_server: int) -> None:
    # DATAGRAM フレームが有効で、送ったペイロードが echo されて戻ることを確認する。
    client = await connect_async(echo_server)
    try:
        assert client.max_datagram_size() > 0

        payload = b"async datagram payload"
        await client.send_datagram(payload, timeout=5.0)
        assert await client.receive_datagram(timeout=5.0) == payload
    finally:
        await client.close()


async def test_datagram_echo_multiple(echo_server: int) -> None:
    # 連続送信した複数 datagram が欠けずに戻ることを確認する。
    client = await connect_async(echo_server)
    try:
        payloads = [f"dgram-{index}".encode() for index in range(5)]
        for payload in payloads:
            await client.send_datagram(payload, timeout=5.0)

        received = [await client.receive_datagram(timeout=5.0) for _ in payloads]
        assert received == payloads
    finally:
        await client.close()


async def test_datagram_echo_near_max_size(echo_server: int) -> None:
    # 実効上限付近のサイズでも datagram が往復することを確認する。
    client = await connect_async(echo_server)
    try:
        payload = b"x" * SAFE_DATAGRAM_SIZE
        await client.send_datagram(payload, timeout=5.0)
        assert await client.receive_datagram(timeout=5.0) == payload
    finally:
        await client.close()
