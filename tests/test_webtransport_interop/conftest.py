"""webtransport-py を使った WebTransport over HTTP/3 相互運用テスト用の共通フィクスチャ。

quiche-py はクライアント実装しか提供しないため、相互運用の相手サーバーとして
webtransport-py の h3.Server を利用する。sans-I/O 化により C++ 側はブロッキング
I/O を持たず GIL を抱え込まないため、webtransport-py のサーバーは同一プロセスの
別スレッドの asyncio ループで動かせる (別プロセス不要)。

webtransport-py は C++ 拡張 (ngtcp2 / nghttp3 / nghttp2) を含む。aioquic と同一の
e2e グループで導入されるため、グループ未導入の free-threaded ビルドでは
インポートできない。その場合はテスト全体をスキップする。
"""

import pytest

# webtransport-py がインポート可能か (e2e グループ導入の有無)。
try:
    import webtransport  # noqa: F401

    WEBTRANSPORT_PY_AVAILABLE = True
except ImportError:
    WEBTRANSPORT_PY_AVAILABLE = False


if WEBTRANSPORT_PY_AVAILABLE:
    import asyncio
    import datetime
    import tempfile
    import threading
    from collections.abc import Iterator
    from pathlib import Path
    from typing import Self

    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.x509.oid import NameOID
    from webtransport import h3

    # サーバーがセッション確立時に単方向ストリームで push する固定ペイロード。
    # テスト側 (test_webtransport_h3.py) と共有する。
    SERVER_UNI_PUSH_PAYLOAD = b"server-uni-from-webtransport-py"

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

    class EchoServer:
        """webtransport-py の h3 サーバーにエコー動作を設定するラッパ。

        - セッション確立: push_payload が指定されていればサーバー起点の単方向
          ストリームを開いて送る (accept_unidirectional_stream の相互運用確認用)
        - 双方向ストリーム: 同一ストリームにデータを返す
        - クライアント起点単方向: 同一ストリームには書けないため、サーバー起点の
          単方向ストリームを対応付けて返す (open_unidirectional +
          accept_unidirectional の往復確認用)
        - datagram: 受け取ったデータグラムをそのまま返す
        """

        def __init__(self, server: h3.Server, push_payload: bytes | None = None) -> None:
            self._server = server
            self._push_payload = push_payload
            # クライアント起点 uni (stream_id % 4 == 2) → サーバー起点 uni (% 4 == 3)
            self._uni_map: dict[int, int] = {}

            server.on_session_ready(self._on_session_ready)
            server.on_stream_data(self._on_stream_data)
            server.on_datagram(self._on_datagram)

        async def _on_session_ready(self, session_id: int, addr: tuple[str, int]) -> None:
            # push_payload が指定された場合のみ、セッション確立を契機に
            # サーバー起点の単方向ストリームを開いて送る。
            if self._push_payload is None:
                return
            stream_id = await self._server.open_stream(addr, session_id)
            if stream_id >= 0:
                await self._server.send_stream_data(addr, stream_id, self._push_payload, fin=True)

        async def _on_stream_data(
            self,
            session_id: int,
            stream_id: int,
            data: bytes,
            addr: tuple[str, int],
        ) -> None:
            if stream_id % 4 == 2:
                # クライアント起点の単方向ストリーム。逆方向に書けないため、
                # サーバー起点の単方向ストリームを対応付けて返す。
                out_id = self._uni_map.get(stream_id)
                if out_id is None:
                    out_id = await self._server.open_stream(addr, session_id)
                    if out_id < 0:
                        return
                    self._uni_map[stream_id] = out_id
                await self._server.send_stream_data(addr, out_id, data, fin=True)
            else:
                # 双方向ストリームは同一ストリームに返す。
                await self._server.send_stream_data(addr, stream_id, data, fin=True)

        async def _on_datagram(self, session_id: int, data: bytes, addr: tuple[str, int]) -> None:
            await self._server.send_datagram(addr, session_id, data)

    class WebtransportPyServer:
        """別スレッドの asyncio ループで動く webtransport-py の h3 エコーサーバー。"""

        def __init__(
            self,
            cert_path: Path,
            key_path: Path,
            *,
            push_payload: bytes | None = None,
        ) -> None:
            self._cert_path = cert_path
            self._key_path = key_path
            self._push_payload = push_payload
            self._server: h3.Server | None = None
            self._loop = asyncio.new_event_loop()
            self._ready = threading.Event()
            self._startup_error: BaseException | None = None
            self._server_task: asyncio.Task[None] | None = None
            self._thread = threading.Thread(target=self._run, daemon=True)

        def __enter__(self) -> Self:
            self._thread.start()
            if not self._ready.wait(timeout=10):
                raise RuntimeError("webtransport-py server failed to start within timeout")
            if self._startup_error is not None:
                raise self._startup_error
            return self

        def __exit__(self, *_exc: object) -> None:
            if self._server_task is not None:
                self._server_task.cancel()
            self._loop.call_soon_threadsafe(self._loop.stop)
            self._thread.join(timeout=5)
            if not self._loop.is_closed():
                self._loop.close()

        @property
        def port(self) -> int:
            if self._server is None:
                raise RuntimeError("webtransport-py server is not started")
            return self._server.actual_port

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
            if self._server is None:
                raise RuntimeError("webtransport-py server is not started")
            # サーバーのメインループを別タスクで回し、イベントループを走らせ続ける。
            self._server_task = self._loop.create_task(self._server.run())
            self._ready.set()
            self._loop.run_forever()

        async def _serve(self) -> None:
            self._server = h3.Server(
                host="127.0.0.1",
                port=0,
                certfile=str(self._cert_path),
                keyfile=str(self._key_path),
            )
            EchoServer(self._server, self._push_payload)
            await self._server.start()

    @pytest.fixture(scope="session")
    def _tls_cert() -> Iterator[tuple[Path, Path]]:
        """セッション全体で使い回す自己署名証明書。"""
        with tempfile.TemporaryDirectory() as tmp:
            yield _generate_self_signed_cert(Path(tmp))

    @pytest.fixture
    def wt_server(_tls_cert: tuple[Path, Path]) -> Iterator[int]:
        """webtransport-py の h3 エコーサーバーを起動し、待ち受けポート番号を返す。

        ストリーム / データグラムをエコーするが、セッション確立時の push は行わない。
        """
        cert_path, key_path = _tls_cert
        with WebtransportPyServer(cert_path, key_path) as server:
            yield server.port

    @pytest.fixture
    def wt_push_server(_tls_cert: tuple[Path, Path]) -> Iterator[int]:
        """webtransport-py の h3 エコーサーバーを起動し、待ち受けポート番号を返す。

        セッション確立時にサーバー起点単方向ストリームを 1 本 push し、
        以降はストリーム / データグラムをエコーする。
        """
        cert_path, key_path = _tls_cert
        with WebtransportPyServer(
            cert_path, key_path, push_payload=SERVER_UNI_PUSH_PAYLOAD
        ) as server:
            yield server.port

else:

    @pytest.fixture
    def wt_server() -> None:
        # webtransport-py がインストールできない環境 (e2e グループ未導入) では
        # 相互運用テストを実行できないため、他のテストの実行を妨げないよう
        # ここでスキップする。
        pytest.skip("webtransport-py が未インストールのため相互運用テストをスキップする")

    @pytest.fixture
    def wt_push_server() -> None:
        pytest.skip("webtransport-py が未インストールのため相互運用テストをスキップする")
