"""asyncio ベースの非同期 HTTP/3 クライアント。"""

import asyncio
import socket
from collections.abc import Callable, Sequence

from .quiche_ext import Http3Client, Http3Response

__all__ = ["AsyncHttp3Client"]


class _Http3Protocol(asyncio.DatagramProtocol):
    def __init__(self, client: AsyncHttp3Client) -> None:
        self._client = client

    def datagram_received(self, data: bytes, addr: object) -> None:
        self._client._on_receive(data)

    def error_received(self, exc: Exception) -> None:
        pass

    def connection_lost(self, exc: Exception | None) -> None:
        self._client._on_connection_lost(exc)


class AsyncHttp3Client:
    """asyncio で扱える非同期 HTTP/3 クライアント。"""

    def __init__(
        self,
        host: str,
        port: int,
        *,
        server_name: str = "",
        verify_peer: bool = True,
    ) -> None:
        self._host = host
        self._port = port
        self._server_name = server_name
        self._verify_peer = verify_peer

        self._loop: asyncio.AbstractEventLoop | None = None
        self._core: Http3Client | None = None
        self._transport: asyncio.DatagramTransport | None = None
        self._timer: asyncio.TimerHandle | None = None
        self._waiters: list[asyncio.Future[None]] = []
        self._closed = False
        self._close_reason = ""
        self._was_connected = False

    async def connect(self, *, timeout: float | None = 10.0) -> None:
        """UDP ソケットを開き、HTTP/3 ハンドシェイクを確立する。"""
        loop = asyncio.get_running_loop()
        self._loop = loop

        infos = await loop.getaddrinfo(self._host, self._port, type=socket.SOCK_DGRAM)
        if not infos:
            raise OSError(f"failed to resolve {self._host}:{self._port}")
        resolved_ip = str(infos[0][4][0])

        core = Http3Client(
            resolved_ip,
            self._port,
            server_name=self._server_name or self._host,
            verify_peer=self._verify_peer,
        )
        self._core = core

        transport, _ = await loop.create_datagram_endpoint(
            lambda: _Http3Protocol(self),
            remote_addr=(resolved_ip, self._port),
        )
        self._transport = transport

        try:
            core.start()
            self._process()
            await self._wait(lambda: True if core.is_connected() else None, timeout)
        except BaseException:
            self._teardown("connect failed")
            raise

    async def request(
        self,
        headers: Sequence[tuple[bytes, bytes]],
        body: bytes = b"",
        *,
        fin: bool = True,
        timeout: float | None = None,
    ) -> Http3Response:
        """HTTP/3 リクエストを送り、完了レスポンスを返す。"""
        core = self._require_core()

        def submit() -> int | None:
            return core.submit_request(list(headers), body, fin)

        stream_id = await self._wait(submit, timeout)
        self._process()

        def take() -> Http3Response | None:
            return core.take_response(stream_id)

        response = await self._wait(take, timeout)
        self._process()
        return response

    async def get(
        self, path: str, *, authority: str = "", timeout: float | None = None
    ) -> Http3Response:
        """簡易 GET リクエスト。"""
        host = authority or self._server_name or self._host
        headers = [
            (b":method", b"GET"),
            (b":scheme", b"https"),
            (b":authority", host.encode()),
            (b":path", path.encode()),
        ]
        return await self.request(headers, timeout=timeout)

    def is_connected(self) -> bool:
        return self._core is not None and self._core.is_connected()

    @property
    def last_error(self) -> str:
        return "" if self._core is None else self._core.last_error

    async def close(self, error_code: int = 0, reason: str = "") -> None:
        if self._core is not None and not self._closed:
            self._core.close(error_code, reason)
            self._drain_send()
        self._teardown(self._close_reason or "closed by application")

    def _require_core(self) -> Http3Client:
        if self._core is None:
            raise RuntimeError("connect() must be called before using the client")
        return self._core

    def _on_receive(self, data: bytes) -> None:
        if self._core is None or self._closed:
            return
        self._core.receive_packet(data)
        self._process()

    def _on_connection_lost(self, exc: Exception | None) -> None:
        reason = "connection lost" if exc is None else str(exc)
        self._teardown(reason)

    def _drain_send(self) -> None:
        if self._core is None or self._transport is None:
            return
        while True:
            packet = self._core.next_send()
            if packet is None:
                break
            self._transport.sendto(packet)

    def _process(self) -> None:
        if self._core is None or self._closed:
            return
        self._drain_send()
        if self._core.is_connected():
            self._was_connected = True
        elif self._was_connected:
            self._teardown(self._core.last_error or "connection closed")
            return
        else:
            err = self._core.last_error
            if err:
                self._teardown(err)
                return
        self._reschedule_timer()
        self._wake_waiters()

    def _reschedule_timer(self) -> None:
        if self._loop is None or self._core is None:
            return
        if self._timer is not None:
            self._timer.cancel()
            self._timer = None
        timeout_ms = self._core.next_timeout_ms()
        if timeout_ms is None:
            return
        self._timer = self._loop.call_later(timeout_ms / 1000.0, self._on_timeout)

    def _on_timeout(self) -> None:
        if self._core is None or self._closed:
            return
        self._core.handle_timeout()
        self._process()

    def _wake_waiters(self) -> None:
        for fut in list(self._waiters):
            if not fut.done():
                fut.set_result(None)

    def _teardown(self, reason: str) -> None:
        if self._closed:
            return
        self._closed = True
        self._close_reason = reason
        if self._timer is not None:
            self._timer.cancel()
            self._timer = None
        if self._transport is not None:
            self._transport.close()
        self._wake_waiters()

    async def _wait[T](self, attempt: Callable[[], T | None], timeout: float | None) -> T:
        loop = asyncio.get_running_loop()
        deadline = None if timeout is None else loop.time() + timeout
        while True:
            if self._closed:
                raise ConnectionError(self._close_reason or "connection closed")
            result = attempt()
            if result is not None:
                return result
            remaining = None
            if deadline is not None:
                remaining = deadline - loop.time()
                if remaining <= 0:
                    raise TimeoutError("timed out waiting for an HTTP/3 event")
            fut: asyncio.Future[None] = loop.create_future()
            self._waiters.append(fut)
            try:
                await asyncio.wait_for(fut, remaining)
            except TimeoutError:
                raise TimeoutError("timed out waiting for an HTTP/3 event") from None
            finally:
                if fut in self._waiters:
                    self._waiters.remove(fut)
