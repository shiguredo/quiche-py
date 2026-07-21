"""asyncio ベースの非同期 QUIC クライアント。

sans-I/O コア (quiche_ext.QuicClient) はソケットを持たない非ブロッキングな
状態機械である。本モジュールはその上に UDP ソケットの所有・送受信・タイムアウト
駆動をまとめて面倒を見る薄い asyncio ラッパを提供する。
"""

import asyncio
import socket
from collections.abc import Callable

from .quiche_ext import QuicClient, QuicStream

__all__ = ["AsyncQuicClient", "AsyncQuicStream"]


class _ClientProtocol(asyncio.DatagramProtocol):
    """UDP の受信・切断イベントを AsyncQuicClient へ橋渡しする。"""

    def __init__(self, client: AsyncQuicClient) -> None:
        self._client = client

    def datagram_received(self, data: bytes, addr: object) -> None:
        self._client._on_receive(data)

    def error_received(self, exc: Exception) -> None:
        # 送信先に到達できない等の非致命的なエラー。受信ループは継続する。
        pass

    def connection_lost(self, exc: Exception | None) -> None:
        self._client._on_connection_lost(exc)


class AsyncQuicStream:
    """QUIC ストリームの非同期ラッパ。"""

    def __init__(self, client: AsyncQuicClient, raw: QuicStream) -> None:
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
        """データが届くまで待ち、(data, fin) を返す。"""

        def attempt() -> tuple[bytes, bool] | None:
            return self._raw.read(max_bytes)

        return await self._client._wait(attempt, timeout)

    async def write(self, data: bytes, *, fin: bool = False, timeout: float | None = None) -> None:
        """ストリームへ書き込む。書き込みブロック中は受理されるまで待つ。"""

        def attempt() -> bool | None:
            # write はブロック中に False を返す。受理されたら True。
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


class AsyncQuicClient:
    """asyncio で扱える非同期 QUIC クライアント。

    UDP ソケットを Python 側で所有し、受信・送信・タイムアウトを駆動する。
    """

    def __init__(
        self,
        host: str,
        port: int,
        *,
        server_name: str = "",
        alpn: str,
        verify_peer: bool = True,
    ) -> None:
        # QUIC に汎用の raw ALPN は無い。相手サーバーと合意した識別子を必須にする。
        if not alpn:
            raise ValueError("alpn must not be empty")
        self._host = host
        self._port = port
        self._server_name = server_name
        self._alpn = alpn
        self._verify_peer = verify_peer

        self._loop: asyncio.AbstractEventLoop | None = None
        self._core: QuicClient | None = None
        self._transport: asyncio.DatagramTransport | None = None
        self._timer: asyncio.TimerHandle | None = None
        self._waiters: list[asyncio.Future[None]] = []
        self._closed = False
        self._close_reason = ""
        self._was_connected = False

    async def connect(self, *, timeout: float | None = 10.0) -> None:
        """UDP ソケットを開き、QUIC ハンドシェイクを確立する。"""
        loop = asyncio.get_running_loop()
        self._loop = loop

        # コアとソケットで宛先がずれないよう、アドレス解決を一度だけ行う。
        infos = await loop.getaddrinfo(self._host, self._port, type=socket.SOCK_DGRAM)
        if not infos:
            raise OSError(f"failed to resolve {self._host}:{self._port}")
        resolved_ip = str(infos[0][4][0])

        core = QuicClient(
            resolved_ip,
            self._port,
            server_name=self._server_name or self._host,
            alpn=self._alpn,
            verify_peer=self._verify_peer,
        )
        self._core = core

        transport, _ = await loop.create_datagram_endpoint(
            lambda: _ClientProtocol(self),
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

    async def open_bidirectional_stream(self, *, timeout: float | None = None) -> AsyncQuicStream:
        return await self._open(unidirectional=False, timeout=timeout)

    async def open_unidirectional_stream(self, *, timeout: float | None = None) -> AsyncQuicStream:
        return await self._open(unidirectional=True, timeout=timeout)

    async def accept_bidirectional_stream(self, *, timeout: float | None = None) -> AsyncQuicStream:
        return await self._accept(unidirectional=False, timeout=timeout)

    async def accept_unidirectional_stream(
        self, *, timeout: float | None = None
    ) -> AsyncQuicStream:
        return await self._accept(unidirectional=True, timeout=timeout)

    async def send_datagram(self, data: bytes, *, timeout: float | None = None) -> None:
        """QUIC datagram を送る。送信ブロック中は受理されるまで待つ。"""
        core = self._require_core()

        def attempt() -> bool | None:
            return True if core.send_datagram(data) else None

        await self._wait(attempt, timeout)
        self._process()

    async def receive_datagram(self, *, timeout: float | None = None) -> bytes:
        """次の QUIC datagram を受け取る。"""
        core = self._require_core()

        def attempt() -> bytes | None:
            return core.receive_datagram()

        return await self._wait(attempt, timeout)

    def max_datagram_size(self) -> int:
        return self._require_core().max_datagram_size()

    def is_connected(self) -> bool:
        return self._core is not None and self._core.is_connected()

    @property
    def last_error(self) -> str:
        return "" if self._core is None else self._core.last_error

    async def close(self, error_code: int = 0, reason: str = "") -> None:
        """QUIC セッションを閉じ、ソケットを解放する。"""
        if self._core is not None and not self._closed:
            self._core.close(error_code, reason)
            # CONNECTION_CLOSE を送出する。
            self._drain_send()
        self._teardown(self._close_reason or "closed by application")

    # 以降は内部実装。

    async def _open(self, *, unidirectional: bool, timeout: float | None) -> AsyncQuicStream:
        core = self._require_core()

        def attempt() -> AsyncQuicStream | None:
            raw = (
                core.open_unidirectional_stream()
                if unidirectional
                else core.open_bidirectional_stream()
            )
            return AsyncQuicStream(self, raw) if raw is not None else None

        return await self._wait(attempt, timeout)

    async def _accept(self, *, unidirectional: bool, timeout: float | None) -> AsyncQuicStream:
        core = self._require_core()

        def attempt() -> AsyncQuicStream | None:
            raw = (
                core.accept_unidirectional_stream()
                if unidirectional
                else core.accept_bidirectional_stream()
            )
            return AsyncQuicStream(self, raw) if raw is not None else None

        return await self._wait(attempt, timeout)

    def _require_core(self) -> QuicClient:
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
        # 送信キューに積まれたパケットを全て UDP で送り出す。
        if self._core is None:
            raise RuntimeError("QUIC core is not initialized")
        if self._transport is None:
            return
        while True:
            packet = self._core.next_send()
            if packet is None:
                break
            self._transport.sendto(packet)

    def _process(self) -> None:
        """送信 drain・切断検出・タイマー再設定・待機解除をまとめて行う。"""
        if self._core is None or self._closed:
            return

        self._drain_send()

        if self._core.is_connected():
            self._was_connected = True
        elif self._was_connected:
            # 一度接続確立後に切断された場合、last_error が空でも teardown する。
            self._teardown(self._core.last_error or "connection closed")
            return
        else:
            # ハンドシェイク未完了でもコアがエラーを記録していれば即失敗させる。
            # next_timeout_ms 等が last_error を消す前にここで拾う。
            err = self._core.last_error
            if err:
                self._teardown(err)
                return

        self._reschedule_timer()
        self._wake_waiters()

    def _reschedule_timer(self) -> None:
        if self._loop is None:
            raise RuntimeError("event loop is not initialized")
        if self._core is None:
            raise RuntimeError("QUIC core is not initialized")
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
        """attempt() が None 以外を返すまで待ち、その値を返す。

        attempt() は「結果」または「まだ得られないことを示す None」を返す。
        接続が閉じたら ConnectionError、時間切れなら TimeoutError を送出する。
        """
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
                    raise TimeoutError("timed out waiting for a QUIC event")

            fut: asyncio.Future[None] = loop.create_future()
            self._waiters.append(fut)
            try:
                await asyncio.wait_for(fut, remaining)
            except TimeoutError:
                raise TimeoutError("timed out waiting for a QUIC event") from None
            finally:
                if fut in self._waiters:
                    self._waiters.remove(fut)
