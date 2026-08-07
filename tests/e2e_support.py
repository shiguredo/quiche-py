"""aioquic 相手の E2E テストで共有するヘルパ。

モック・スタブは使わず、実際の UDP と quiche プリミティブ / AsyncQuicClient を駆動する。
"""

import select
import socket
import time
from collections.abc import Callable
from typing import Protocol

import quiche

# エコーサーバー側 (conftest) と揃える ALPN。
ALPN = "hq-interop"

# datagram の実効サイズは max_datagram_size() より小さくなることがあるため、
# 確実に往復できる上限としてこれを使う。
SAFE_DATAGRAM_SIZE = 1000

# push_server がハンドシェイク後に送る固定ペイロード (conftest と共有)。
SERVER_BIDI_PUSH_PAYLOAD = b"server-bidi-push"
SERVER_UNI_PUSH_PAYLOAD = b"server-uni-push"


class PrimitiveDriver:
    """sans-I/O プリミティブを UDP ソケット上で駆動する最小ドライバ。"""

    def __init__(self, client: quiche.QuicClient, port: int) -> None:
        self._client = client
        self._server_addr = ("127.0.0.1", port)
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.setblocking(False)

    @property
    def client(self) -> quiche.QuicClient:
        return self._client

    def close(self) -> None:
        self._sock.close()

    def _flush(self) -> None:
        # 送信キューに積まれたパケットを全て UDP で送り出す。
        while True:
            packet = self._client.next_send()
            if packet is None:
                break
            self._sock.sendto(packet, self._server_addr)

    def pump(self, until: Callable[[], bool], timeout_s: float = 5.0) -> bool:
        """until() が True を返すまでイベントを回す。達成できれば True。"""
        deadline = time.monotonic() + timeout_s
        while True:
            self._flush()
            if until():
                return True
            now = time.monotonic()
            if now >= deadline:
                return False

            timeout_ms = self._client.next_timeout_ms()
            wait = deadline - now
            if timeout_ms is not None:
                wait = min(wait, timeout_ms / 1000.0)
            wait = max(0.0, wait)

            readable, _, _ = select.select([self._sock], [], [], wait)
            if readable:
                data, _ = self._sock.recvfrom(65535)
                self._client.receive_packet(data)
            elif timeout_ms is not None:
                self._client.handle_timeout()


def make_quic_client(port: int, *, alpn: str = ALPN) -> quiche.QuicClient:
    return quiche.QuicClient(
        "127.0.0.1",
        port,
        alpn=alpn,
        verify_peer=False,
    )


def connect_primitive(driver: PrimitiveDriver, timeout_s: float = 10.0) -> None:
    # ハンドシェイクを開始し、接続確立まで駆動する。
    driver.client.start()
    connected = driver.pump(driver.client.is_connected, timeout_s=timeout_s)
    assert connected, "ハンドシェイクが時間内に確立しなかった"


def read_until_fin_primitive(
    driver: PrimitiveDriver,
    stream: quiche.QuicStream,
    timeout_s: float = 5.0,
    *,
    max_bytes: int = 65536,
) -> tuple[bytes, bool]:
    # fin を受け取るまでストリームから読み出して連結する。
    chunks = bytearray()

    def got_fin() -> bool:
        result = stream.read(max_bytes)
        if result is None:
            return False
        data, fin = result
        chunks.extend(data)
        return fin

    finished = driver.pump(got_fin, timeout_s=timeout_s)
    return bytes(chunks), finished


async def connect_async(
    port: int,
    *,
    alpn: str = ALPN,
    server_name: str = "",
) -> quiche.AsyncQuicClient:
    client = quiche.AsyncQuicClient(
        "127.0.0.1",
        port,
        alpn=alpn,
        server_name=server_name,
        verify_peer=False,
    )
    await client.connect(timeout=10.0)
    return client


class AsyncReadableStream(Protocol):
    """fin まで read できる非同期ストリームの共通インターフェース。

    AsyncQuicStream と AsyncWebTransportStream は同一の read シグネチャを
    持つため、両者を同じヘルパで扱えるようにする。
    """

    async def read(
        self, max_bytes: int = 65536, *, timeout: float | None = None
    ) -> tuple[bytes, bool]: ...


async def read_until_fin_async(
    stream: AsyncReadableStream,
    timeout: float = 5.0,
    *,
    max_bytes: int = 65536,
) -> bytes:
    # fin を受け取るまで読み出して連結する (分割到着し得るため)。
    chunks = bytearray()
    while True:
        data, fin = await stream.read(max_bytes=max_bytes, timeout=timeout)
        chunks.extend(data)
        if fin:
            return bytes(chunks)
