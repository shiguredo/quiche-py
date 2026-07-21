"""AsyncHttp3Client の aioquic HTTP/3 サーバー相手 E2E。"""

import quiche


async def test_http3_get(http3_server: int) -> None:
    # aioquic H3 サーバーへ GET し、固定ボディが返ることを確認する。
    client = quiche.AsyncHttp3Client("127.0.0.1", http3_server, verify_peer=False)
    await client.connect(timeout=10.0)
    try:
        assert client.is_connected()
        response = await client.get("/", authority="localhost", timeout=10.0)
        assert response.status_code == 200
        assert response.body == b"hello from aioquic h3"
        header_names = {name for name, _value in response.headers}
        assert b":status" in header_names
    finally:
        await client.close()


async def test_http3_request_with_headers(http3_server: int) -> None:
    # 疑似ヘッダを明示した request() でも同様に 200 が返ることを確認する。
    client = quiche.AsyncHttp3Client("127.0.0.1", http3_server, verify_peer=False)
    await client.connect(timeout=10.0)
    try:
        response = await client.request(
            [
                (b":method", b"GET"),
                (b":scheme", b"https"),
                (b":authority", b"localhost"),
                (b":path", b"/explicit"),
                (b"user-agent", b"quiche-py-e2e"),
            ],
            timeout=10.0,
        )
        assert response.status_code == 200
        assert response.body == b"hello from aioquic h3"
    finally:
        await client.close()
