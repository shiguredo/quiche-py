"""webtransport-py の h3 サーバーに対する WebTransport over HTTP/3 相互運用テスト。

quiche-py はクライアント実装しか提供しないため、相手サーバーとして
webtransport-py の h3.Server を利用し、実際の UDP 通信で相互運用を検証する。
モック・スタブは使わない。
"""

from e2e_support import read_until_fin_async

import quiche

# サーバー側 (conftest) と共有する、セッション確立時に push される固定ペイロード。
SERVER_UNI_PUSH_PAYLOAD = b"server-uni-from-webtransport-py"


async def _connect(port: int) -> quiche.AsyncWebTransportClient:
    """webtransport-py サーバーへ接続して WebTransport セッションを確立する。"""
    client = quiche.AsyncWebTransportClient(
        "127.0.0.1",
        port,
        verify_peer=False,
    )
    await client.connect(path="/webtransport", timeout=10.0)
    return client


async def test_session_ready(wt_server: int) -> None:
    """webtransport-py サーバーとの間で WebTransport セッションが確立できることを確認する。"""
    client = await _connect(wt_server)
    try:
        assert client.is_connected() is True
        assert client.is_session_ready() is True
    finally:
        await client.close()


async def test_bidirectional_stream_echo(wt_server: int) -> None:
    """双方向ストリームでデータ + fin が往復できることを確認する。

    quiche-py が書き込んだデータを webtransport-py サーバーが受信し、
    同一ストリームにエコーして返すことで相互運用を検証する。
    """
    client = await _connect(wt_server)
    try:
        stream = await client.open_bidirectional_stream(timeout=5.0)
        assert stream.unidirectional is False

        payload = b"hello via bidi stream"
        await stream.write(payload, fin=True, timeout=5.0)
        assert await read_until_fin_async(stream) == payload
    finally:
        await client.close()


async def test_multiple_bidirectional_streams(wt_server: int) -> None:
    """複数の双方向ストリームが独立して往復することを確認する。"""
    client = await _connect(wt_server)
    try:
        payloads = [f"interop-stream-{index}".encode() for index in range(4)]
        streams = []
        for payload in payloads:
            stream = await client.open_bidirectional_stream(timeout=5.0)
            await stream.write(payload, fin=True, timeout=5.0)
            streams.append(stream)

        for stream, payload in zip(streams, payloads, strict=True):
            assert await read_until_fin_async(stream) == payload
    finally:
        await client.close()


async def test_unidirectional_stream_echo(wt_server: int) -> None:
    """クライアント起点の単方向ストリームのエコーで相互運用を確認する。

    同一ストリームには逆方向に書けないため、webtransport-py サーバーが
    サーバー起点の単方向ストリームを開いて返し、quiche-py 側が
    accept_unidirectional_stream で受け取る。
    """
    client = await _connect(wt_server)
    try:
        stream = await client.open_unidirectional_stream(timeout=5.0)
        assert stream.unidirectional is True

        payload = b"hello via uni stream"
        await stream.write(payload, fin=True, timeout=5.0)

        echo = await client.accept_unidirectional_stream(timeout=5.0)
        assert await read_until_fin_async(echo) == payload
    finally:
        await client.close()


async def test_server_initiated_unidirectional_stream(wt_push_server: int) -> None:
    """サーバー起点の単方向ストリームをクライアントが受信できることを確認する。

    webtransport-py サーバーがセッション確立時に単方向ストリームを push し、
    quiche-py が accept_unidirectional_stream で受け取る。
    """
    client = await _connect(wt_push_server)
    try:
        stream = await client.accept_unidirectional_stream(timeout=5.0)
        assert await read_until_fin_async(stream) == SERVER_UNI_PUSH_PAYLOAD
    finally:
        await client.close()


async def test_datagram_echo(wt_server: int) -> None:
    """データグラムが往復できることを確認する。

    quiche-py が送ったデータグラムを webtransport-py サーバーが受信し、
    そのまま返すことで相互運用を検証する。
    """
    client = await _connect(wt_server)
    try:
        payload = b"hello datagram"
        await client.send_datagram(payload, timeout=5.0)
        assert await client.receive_datagram(timeout=5.0) == payload
    finally:
        await client.close()
