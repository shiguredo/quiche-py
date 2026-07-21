"""aioquic からのサーバー起点ストリーム accept 相互運用。"""

from e2e_support import (
    SERVER_BIDI_PUSH_PAYLOAD,
    SERVER_UNI_PUSH_PAYLOAD,
    connect_async,
    read_until_fin_async,
)


async def test_accept_server_bidirectional_stream(push_server: int) -> None:
    # サーバー起点の双方向ストリームを accept し、push データを読めることを確認する。
    client = await connect_async(push_server)
    try:
        stream = await client.accept_bidirectional_stream(timeout=5.0)
        assert stream.unidirectional is False
        assert stream.stream_id % 4 == 1
        assert await read_until_fin_async(stream) == SERVER_BIDI_PUSH_PAYLOAD
    finally:
        await client.close()


async def test_accept_server_unidirectional_stream(push_server: int) -> None:
    # サーバー起点の単方向ストリームを accept し、push データを読めることを確認する。
    client = await connect_async(push_server)
    try:
        stream = await client.accept_unidirectional_stream(timeout=5.0)
        assert stream.unidirectional is True
        assert stream.stream_id % 4 == 3
        assert await read_until_fin_async(stream) == SERVER_UNI_PUSH_PAYLOAD
    finally:
        await client.close()


async def test_accept_both_server_streams_then_client_echo(push_server: int) -> None:
    # サーバー起点 bidi/uni を両方受けた後も、クライアント起点ストリームで
    # 通常の echo 相互運用が続くことを確認する。
    client = await connect_async(push_server)
    try:
        bidi = await client.accept_bidirectional_stream(timeout=5.0)
        uni = await client.accept_unidirectional_stream(timeout=5.0)
        assert await read_until_fin_async(bidi) == SERVER_BIDI_PUSH_PAYLOAD
        assert await read_until_fin_async(uni) == SERVER_UNI_PUSH_PAYLOAD

        echo = await client.open_bidirectional_stream(timeout=5.0)
        await echo.write(b"after-accept", fin=True, timeout=5.0)
        assert await read_until_fin_async(echo) == b"after-accept"
    finally:
        await client.close()
