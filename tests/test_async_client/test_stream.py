"""aioquic との双方向ストリーム相互運用。"""

from e2e_support import connect_async, read_until_fin_async


async def test_bidirectional_echo(echo_server: int) -> None:
    # クライアント起点の双方向ストリームでデータ + fin が往復することを確認する。
    client = await connect_async(echo_server)
    try:
        stream = await client.open_bidirectional_stream(timeout=5.0)
        assert stream.stream_id % 4 == 0
        assert stream.unidirectional is False

        payload = b"hello async quic"
        await stream.write(payload, fin=True, timeout=5.0)
        assert await read_until_fin_async(stream) == payload
    finally:
        await client.close()


async def test_bidirectional_echo_empty_fin(echo_server: int) -> None:
    # 空データ + fin だけでも STREAM の終端が往復することを確認する。
    client = await connect_async(echo_server)
    try:
        stream = await client.open_bidirectional_stream(timeout=5.0)
        await stream.write(b"", fin=True, timeout=5.0)
        assert await read_until_fin_async(stream) == b""
    finally:
        await client.close()


async def test_bidirectional_echo_chunked(echo_server: int) -> None:
    # fin なしの分割書き込み後に fin を付けても、結合されて echo されることを確認する。
    client = await connect_async(echo_server)
    try:
        stream = await client.open_bidirectional_stream(timeout=5.0)
        await stream.write(b"chunk-a-", fin=False, timeout=5.0)
        await stream.write(b"chunk-b", fin=True, timeout=5.0)
        assert await read_until_fin_async(stream) == b"chunk-a-chunk-b"
    finally:
        await client.close()


async def test_bidirectional_echo_large_payload(echo_server: int) -> None:
    # 複数パケットに跨り得るサイズでもストリーム往復できることを確認する。
    client = await connect_async(echo_server)
    try:
        stream = await client.open_bidirectional_stream(timeout=5.0)
        payload = bytes(range(256)) * 32  # 8 KiB
        await stream.write(payload, fin=True, timeout=5.0)
        assert await read_until_fin_async(stream) == payload
    finally:
        await client.close()


async def test_multiple_bidirectional_streams(echo_server: int) -> None:
    # 複数の双方向ストリームが独立して往復することを確認する。
    client = await connect_async(echo_server)
    try:
        payloads = [f"async-stream-{index}".encode() for index in range(4)]
        streams = []
        for payload in payloads:
            stream = await client.open_bidirectional_stream(timeout=5.0)
            await stream.write(payload, fin=True, timeout=5.0)
            streams.append(stream)

        for stream, payload in zip(streams, payloads, strict=True):
            assert await read_until_fin_async(stream) == payload
    finally:
        await client.close()
