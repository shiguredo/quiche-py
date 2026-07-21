"""aioquic を使った E2E テスト用の共通フィクスチャ。

sans-I/O 化により C++ 側はブロッキング I/O を持たず GIL を抱え込まないため、
aioquic のエコーサーバーは同一プロセスのスレッドで動かせる (別プロセス不要)。

aioquic は C 拡張で limited API を強制するため free-threaded ビルドではインストール
できない。aioquic (とその依存の cryptography) が利用できない環境では E2E テスト全体を
スキップする。
"""

import pytest

# aioquic がインストール可能か (free-threaded ビルドでは C 拡張がビルドできず不在)。
try:
    import aioquic  # noqa: F401

    AIOQUIC_AVAILABLE = True
except ImportError:
    AIOQUIC_AVAILABLE = False


if AIOQUIC_AVAILABLE:
    import asyncio
    import datetime
    import socket
    import tempfile
    import threading
    from collections.abc import Callable, Iterator
    from pathlib import Path
    from typing import Self

    from aioquic.asyncio import serve
    from aioquic.asyncio.protocol import QuicConnectionProtocol, QuicStreamHandler
    from aioquic.h3.connection import H3_ALPN, H3Connection
    from aioquic.h3.events import DataReceived as H3DataReceived
    from aioquic.h3.events import H3Event
    from aioquic.h3.events import HeadersReceived as H3HeadersReceived
    from aioquic.quic.configuration import QuicConfiguration
    from aioquic.quic.connection import QuicConnection
    from aioquic.quic.events import (
        DatagramFrameReceived,
        HandshakeCompleted,
        ProtocolNegotiated,
        QuicEvent,
        StreamDataReceived,
    )
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.x509.oid import NameOID
    from e2e_support import ALPN, SERVER_BIDI_PUSH_PAYLOAD, SERVER_UNI_PUSH_PAYLOAD

    # datagram フレームの最大サイズ。クライアント側と揃えて datagram を有効化する。
    MAX_DATAGRAM_FRAME_SIZE = 65536

    # HTTP/3 GET の固定レスポンス。
    HTTP3_GET_BODY = b"hello from aioquic h3"

    def _free_udp_port() -> int:
        """空いている UDP ポート番号を一つ返す。"""
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.bind(("127.0.0.1", 0))
            return sock.getsockname()[1]

    def _generate_self_signed_cert(directory: Path) -> tuple[Path, Path]:
        """自己署名証明書と秘密鍵を生成してファイルに書き出す。"""
        key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "localhost")])
        now = datetime.datetime.now(datetime.UTC)
        certificate = (
            x509.CertificateBuilder()
            .subject_name(name)
            .issuer_name(name)
            .public_key(key.public_key())
            .serial_number(x509.random_serial_number())
            .not_valid_before(now - datetime.timedelta(days=1))
            .not_valid_after(now + datetime.timedelta(days=365))
            .add_extension(
                x509.SubjectAlternativeName([x509.DNSName("localhost")]),
                critical=False,
            )
            .sign(key, hashes.SHA256())
        )

        cert_path = directory / "cert.pem"
        key_path = directory / "key.pem"
        cert_path.write_bytes(certificate.public_bytes(serialization.Encoding.PEM))
        key_path.write_bytes(
            key.private_bytes(
                encoding=serialization.Encoding.PEM,
                format=serialization.PrivateFormat.TraditionalOpenSSL,
                encryption_algorithm=serialization.NoEncryption(),
            )
        )
        return cert_path, key_path

    class EchoServerProtocol(QuicConnectionProtocol):
        """aioquic 側の相互運用用エコー実装。

        - 双方向ストリーム: 同一 stream にデータを返す
        - クライアント起点単方向: 同一 stream には書けないため、サーバー起点単方向を
          対応付けて返す (open_unidirectional + accept_unidirectional の往復確認用)
        - datagram: 受け取ったフレームをそのまま返す
        """

        def __init__(
            self,
            quic: QuicConnection,
            stream_handler: QuicStreamHandler | None = None,
        ) -> None:
            super().__init__(quic, stream_handler)
            # クライアント起点 uni (id % 4 == 2) → サーバー起点 uni (id % 4 == 3)
            self._client_uni_echo: dict[int, int] = {}

        def _echo_stream(self, event: StreamDataReceived) -> None:
            if event.stream_id % 4 == 2:
                out_id = self._client_uni_echo.get(event.stream_id)
                if out_id is None:
                    out_id = self._quic.get_next_available_stream_id(is_unidirectional=True)
                    self._client_uni_echo[event.stream_id] = out_id
                self._quic.send_stream_data(out_id, event.data, end_stream=event.end_stream)
            else:
                self._quic.send_stream_data(
                    event.stream_id, event.data, end_stream=event.end_stream
                )
            self.transmit()

        def quic_event_received(self, event: QuicEvent) -> None:
            if isinstance(event, StreamDataReceived):
                self._echo_stream(event)
            elif isinstance(event, DatagramFrameReceived):
                self._quic.send_datagram_frame(event.data)
                self.transmit()

    class PushEchoServerProtocol(EchoServerProtocol):
        """ハンドシェイク完了時にサーバー起点ストリームを push し、以降はエコーする。

        accept_bidirectional_stream / accept_unidirectional_stream の相互運用確認用。
        """

        def __init__(
            self,
            quic: QuicConnection,
            stream_handler: QuicStreamHandler | None = None,
        ) -> None:
            super().__init__(quic, stream_handler)
            self._pushed = False

        def quic_event_received(self, event: QuicEvent) -> None:
            if isinstance(event, HandshakeCompleted) and not self._pushed:
                self._pushed = True
                # サーバー起点の双方向ストリーム (stream_id % 4 == 1)。
                bidi_id = self._quic.get_next_available_stream_id(is_unidirectional=False)
                self._quic.send_stream_data(bidi_id, SERVER_BIDI_PUSH_PAYLOAD, end_stream=True)
                # サーバー起点の単方向ストリーム (stream_id % 4 == 3)。
                uni_id = self._quic.get_next_available_stream_id(is_unidirectional=True)
                self._quic.send_stream_data(uni_id, SERVER_UNI_PUSH_PAYLOAD, end_stream=True)
                self.transmit()

            # 以降は通常のエコー (双方向 / クライアント uni / datagram)。
            super().quic_event_received(event)

    class Http3ServerProtocol(QuicConnectionProtocol):
        """aioquic の HTTP/3 サーバー。GET に固定ボディで応答する。"""

        def __init__(
            self,
            quic: QuicConnection,
            stream_handler: QuicStreamHandler | None = None,
        ) -> None:
            super().__init__(quic, stream_handler)
            self._http: H3Connection | None = None

        def quic_event_received(self, event: QuicEvent) -> None:
            if isinstance(event, ProtocolNegotiated):
                self._http = H3Connection(self._quic)
            if self._http is not None:
                for h3_event in self._http.handle_event(event):
                    self._h3_event_received(h3_event)

        def _h3_event_received(self, event: H3Event) -> None:
            if isinstance(event, H3HeadersReceived):
                assert self._http is not None
                method = b""
                for name, value in event.headers:
                    if name == b":method":
                        method = value
                if method == b"GET":
                    self._http.send_headers(
                        event.stream_id,
                        [
                            (b":status", b"200"),
                            (b"content-type", b"text/plain"),
                        ],
                    )
                    self._http.send_data(event.stream_id, HTTP3_GET_BODY, end_stream=True)
                    self.transmit()
            elif isinstance(event, H3DataReceived):
                # GET 以外はボディを読み捨てる。
                pass

    class AioquicServer:
        """別スレッドの asyncio ループで動く aioquic サーバー。"""

        def __init__(
            self,
            host: str,
            port: int,
            cert_path: Path,
            key_path: Path,
            *,
            create_protocol: Callable[..., QuicConnectionProtocol],
            alpn_protocols: list[str] | None = None,
        ) -> None:
            self._host = host
            self._port = port
            self._cert_path = cert_path
            self._key_path = key_path
            self._create_protocol = create_protocol
            self._alpn_protocols = alpn_protocols if alpn_protocols is not None else [ALPN]
            self._loop = asyncio.new_event_loop()
            self._ready = threading.Event()
            self._startup_error: BaseException | None = None
            self._thread = threading.Thread(target=self._run, daemon=True)

        def __enter__(self) -> Self:
            self._thread.start()
            if not self._ready.wait(timeout=10):
                raise RuntimeError("aioquic server failed to start within timeout")
            if self._startup_error is not None:
                raise self._startup_error
            return self

        def __exit__(self, *_exc: object) -> None:
            self._loop.call_soon_threadsafe(self._loop.stop)
            self._thread.join(timeout=5)
            if not self._loop.is_closed():
                self._loop.close()

        def _run(self) -> None:
            asyncio.set_event_loop(self._loop)
            try:
                self._loop.run_until_complete(self._serve())
            except BaseException as exc:
                # 起動失敗を _startup_error に載せ、__enter__ 側で再送出する。
                # CancelledError 等の BaseException も逃さないため広い except にする。
                self._startup_error = exc
                self._ready.set()
                return
            self._ready.set()
            self._loop.run_forever()

        async def _serve(self) -> None:
            configuration = QuicConfiguration(
                is_client=False,
                alpn_protocols=self._alpn_protocols,
                max_datagram_frame_size=MAX_DATAGRAM_FRAME_SIZE,
            )
            configuration.load_cert_chain(self._cert_path, self._key_path)
            await serve(
                self._host,
                self._port,
                configuration=configuration,
                create_protocol=self._create_protocol,
            )

    @pytest.fixture(scope="session")
    def _tls_cert() -> Iterator[tuple[Path, Path]]:
        """セッション全体で使い回す自己署名証明書。"""
        with tempfile.TemporaryDirectory() as tmp:
            yield _generate_self_signed_cert(Path(tmp))

    def _serve_port(
        _tls_cert: tuple[Path, Path],
        create_protocol: Callable[..., QuicConnectionProtocol],
        *,
        alpn_protocols: list[str] | None = None,
    ) -> Iterator[int]:
        cert_path, key_path = _tls_cert
        port = _free_udp_port()
        with AioquicServer(
            "127.0.0.1",
            port,
            cert_path,
            key_path,
            create_protocol=create_protocol,
            alpn_protocols=alpn_protocols,
        ):
            yield port

    @pytest.fixture
    def echo_server(_tls_cert: tuple[Path, Path]) -> Iterator[int]:
        """aioquic エコーサーバーを起動し、待ち受けポート番号を返す。"""
        yield from _serve_port(_tls_cert, EchoServerProtocol)

    @pytest.fixture
    def push_server(_tls_cert: tuple[Path, Path]) -> Iterator[int]:
        """ハンドシェイク後にサーバー起点ストリームを push するエコーサーバー。"""
        yield from _serve_port(_tls_cert, PushEchoServerProtocol)

    @pytest.fixture
    def http3_server(_tls_cert: tuple[Path, Path]) -> Iterator[int]:
        """aioquic HTTP/3 サーバーを起動し、待ち受けポート番号を返す。"""
        yield from _serve_port(_tls_cert, Http3ServerProtocol, alpn_protocols=list(H3_ALPN))

else:

    @pytest.fixture
    def echo_server() -> None:
        # aioquic がインストールできない free-threaded ビルドでは E2E テストを実行
        # できないため、他のテストの実行を妨げないようここでスキップする。
        pytest.skip("aioquic は free-threaded ビルド非対応のため E2E テストをスキップする")

    @pytest.fixture
    def push_server() -> None:
        pytest.skip("aioquic は free-threaded ビルド非対応のため E2E テストをスキップする")

    @pytest.fixture
    def http3_server() -> None:
        pytest.skip("aioquic は free-threaded ビルド非対応のため E2E テストをスキップする")
