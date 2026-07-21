"""asyncio ベースの非同期 WebTransport over HTTP/3 クライアント。"""

import asyncio
import socket
from collections.abc import Callable, Sequence

from .quiche_ext import WebTransportClient, WebTransportStream

__all__ = ["AsyncWebTransportClient", "AsyncWebTransportStream"]


class _WtProtocol(asyncio.DatagramProtocol):
    def __init__(self, client: AsyncWebTransportClient) -> None:
        self._client = client

    def datagram_received(self, data: bytes, addr: object) -> None:
        self._client._on_receive(data)

    def error_received(self, exc: Exception) -> None:
        pass

    def connection_lost(self, exc: Exception | None) -> None:
        self._client._on_connection_lost(exc)


class AsyncWebTransportStream:
    """WebTransport ストリームの非同期ラッパ。"""

    def __init__(self, client: AsyncWebTransportClient, raw: WebTransportStream) -> None:
        self._client = client
        self._raw = raw

    @property
    def stream_id(self) -> int:
        return self._raw.stream_id

    @property
    def unidirectional(self) -> bool:
        return self._raw.unidirectional

    async def read(
        self, max_bytes: int = 65536, *, timeout: float | None = None
    ) -> tuple[bytes, bool]:
        def attempt() -> tuple[bytes, bool] | None:
            return self._raw.read(max_bytes)

        return await self._client._wait(attempt, timeout)

    async def write(self, data: bytes, *, fin: bool = False, timeout: float | None = None) -> None:
        def attempt() -> bool | None:
            return True if self._raw.write(data, fin) else None

        await self._client._wait(attempt, timeout)
        self._client._process()

    def reset(self, error_code: int = 0) -> None:
        self._raw.reset(error_code)
        self._client._process()

    def stop_sending(self, error_code: int = 0) -> None:
        self._raw.stop_sending(error_code)
        self._client._process()

    def can_write(self) -> bool:
        return self._raw.can_write()

    def is_open(self) -> bool:
        return self._raw.is_open()


class AsyncWebTransportClient:
    """asyncio で扱える WebTransport over HTTP/3 クライアント。"""

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
        self._core: WebTransportClient | None = None
        self._transport: asyncio.DatagramTransport | None = None
        self._timer: asyncio.TimerHandle | None = None
        self._waiters: list[asyncio.Future[None]] = []
        self._closed = False
        self._close_reason = ""
        self._was_connected = False

    async def connect(
        self,
        path: str = "/",
        *,
        headers: Sequence[tuple[bytes, bytes]] | None = None,
        timeout: float | None = 10.0,
    ) -> None:
        """QUIC ハンドシェイク後に WebTransport CONNECT を完了する。"""
        loop = asyncio.get_running_loop()
        self._loop = loop

        infos = await loop.getaddrinfo(self._host, self._port, type=socket.SOCK_DGRAM)
        if not infos:
            raise OSError(f"failed to resolve {self._host}:{self._port}")
        resolved_ip = str(infos[0][4][0])

        core = WebTransportClient(
            resolved_ip,
            self._port,
            server_name=self._server_name or self._host,
            verify_peer=self._verify_peer,
        )
        self._core = core

        transport, _ = await loop.create_datagram_endpoint(
            lambda: _WtProtocol(self),
            remote_addr=(resolved_ip, self._port),
        )
        self._transport = transport

        try:
            core.start()
            self._process()
            await self._wait(lambda: True if core.is_connected() else None, timeout)

            extra = list(headers) if headers is not None else None

            def open_session() -> bool | None:
                return True if core.connect_session(path, extra) else None

            await self._wait(open_session, timeout)
            self._process()
            await self._wait(lambda: True if core.is_session_ready() else None, timeout)
        except BaseException:
            self._teardown("connect failed")
            raise

    async def open_bidirectional_stream(
        self, *, timeout: float | None = None
    ) -> AsyncWebTransportStream:
        return await self._open(unidirectional=False, timeout=timeout)

    async def open_unidirectional_stream(
        self, *, timeout: float | None = None
    ) -> AsyncWebTransportStream:
        return await self._open(unidirectional=True, timeout=timeout)

    async def accept_bidirectional_stream(
        self, *, timeout: float | None = None
    ) -> AsyncWebTransportStream:
        return await self._accept(unidirectional=False, timeout=timeout)

    async def accept_unidirectional_stream(
        self, *, timeout: float | None = None
    ) -> AsyncWebTransportStream:
        return await self._accept(unidirectional=True, timeout=timeout)

    async def send_datagram(self, data: bytes, *, timeout: float | None = None) -> None:
        core = self._require_core()

        def attempt() -> bool | None:
            return True if core.send_datagram(data) else None

        await self._wait(attempt, timeout)
        self._process()

    async def receive_datagram(self, *, timeout: float | None = None) -> bytes:
        core = self._require_core()

        def attempt() -> bytes | None:
            return core.receive_datagram()

        return await self._wait(attempt, timeout)

    def max_datagram_size(self) -> int:
        return self._require_core().max_datagram_size()

    def is_connected(self) -> bool:
        return self._core is not None and self._core.is_connected()

    def is_session_ready(self) -> bool:
        return self._core is not None and self._core.is_session_ready()

    @property
    def last_error(self) -> str:
        return "" if self._core is None else self._core.last_error

    async def close(self, error_code: int = 0, reason: str = "") -> None:
        if self._core is not None and not self._closed:
            self._core.close(error_code, reason)
            self._drain_send()
        self._teardown(self._close_reason or "closed by application")

    async def _open(
        self, *, unidirectional: bool, timeout: float | None
    ) -> AsyncWebTransportStream:
        core = self._require_core()

        def attempt() -> AsyncWebTransportStream | None:
            raw = (
                core.open_unidirectional_stream()
                if unidirectional
                else core.open_bidirectional_stream()
            )
            return AsyncWebTransportStream(self, raw) if raw is not None else None

        return await self._wait(attempt, timeout)

    async def _accept(
        self, *, unidirectional: bool, timeout: float | None
    ) -> AsyncWebTransportStream:
        core = self._require_core()

        def attempt() -> AsyncWebTransportStream | None:
            raw = (
                core.accept_unidirectional_stream()
                if unidirectional
                else core.accept_bidirectional_stream()
            )
            return AsyncWebTransportStream(self, raw) if raw is not None else None

        return await self._wait(attempt, timeout)

    def _require_core(self) -> WebTransportClient:
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
                    raise TimeoutError("timed out waiting for a WebTransport event")
            fut: asyncio.Future[None] = loop.create_future()
            self._waiters.append(fut)
            try:
                await asyncio.wait_for(fut, remaining)
            except TimeoutError:
                raise TimeoutError("timed out waiting for a WebTransport event") from None
            finally:
                if fut in self._waiters:
                    self._waiters.remove(fut)
