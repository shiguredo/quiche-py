"""aioquic との単方向ストリーム相互運用。

クライアント起点単方向は同一 stream に返信できないため、エコーサーバーは
サーバー起点単方向へ載せ替えて返す。これにより open → accept の往復を検証する。
"""

from e2e_support import connect_async, read_until_fin_async


async def test_unidirectional_echo_via_server_stream(echo_server: int) -> None:
    # クライアント起点 uni へ書いたデータが、サーバー起点 uni として戻ることを確認する。
    client = await connect_async(echo_server)
    try:
        outbound = await client.open_unidirectional_stream(timeout=5.0)
        assert outbound.unidirectional is True
        assert outbound.stream_id % 4 == 2

        payload = b"uni payload over quic"
        await outbound.write(payload, fin=True, timeout=5.0)

        inbound = await client.accept_unidirectional_stream(timeout=5.0)
        assert inbound.unidirectional is True
        assert inbound.stream_id % 4 == 3
        assert await read_until_fin_async(inbound) == payload
    finally:
        await client.close()


async def test_unidirectional_echo_chunked(echo_server: int) -> None:
    # 単方向でも分割書き込みが結合されてサーバー起点 uni に返ることを確認する。
    client = await connect_async(echo_server)
    try:
        outbound = await client.open_unidirectional_stream(timeout=5.0)
        await outbound.write(b"uni-a-", fin=False, timeout=5.0)
        await outbound.write(b"uni-b", fin=True, timeout=5.0)

        inbound = await client.accept_unidirectional_stream(timeout=5.0)
        assert await read_until_fin_async(inbound) == b"uni-a-uni-b"
    finally:
        await client.close()
