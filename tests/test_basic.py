"""version() と QuicClient 生成・引数バリデーションの基本テスト。

E2E サーバーを必要としない単体テスト群。
"""

import pytest

import quiche


def test_version_returns_string() -> None:
    # version() が文字列を返すことを確認する。
    assert isinstance(quiche.version(), str)


def test_quic_client_creation() -> None:
    # 正常な引数で QuicClient を生成できることを確認する。
    client = quiche.QuicClient(
        "127.0.0.1",
        4433,
        server_name="",
        alpn="hq-interop",
        verify_peer=False,
    )
    assert not client.is_connected()
    assert client.last_error == ""


def test_quic_client_empty_host_raises() -> None:
    # 空ホストで RuntimeError が送出されることを確認する。
    with pytest.raises(RuntimeError, match="host must not be empty"):
        quiche.QuicClient(
            "",
            4433,
            server_name="",
            alpn="hq-interop",
            verify_peer=False,
        )


def test_quic_client_empty_alpn_raises() -> None:
    # 空 ALPN は受け付けず RuntimeError になることを確認する。
    # QUIC に汎用の raw ALPN は無く、相手と合意した識別子を明示する必要がある。
    with pytest.raises(RuntimeError, match="alpn must not be empty"):
        quiche.QuicClient(
            "127.0.0.1",
            4433,
            alpn="",
            verify_peer=False,
        )


def test_async_client_empty_alpn_raises() -> None:
    # AsyncQuicClient でも空 ALPN は ValueError になることを確認する。
    with pytest.raises(ValueError, match="alpn must not be empty"):
        quiche.AsyncQuicClient(
            "127.0.0.1",
            4433,
            alpn="",
            verify_peer=False,
        )


def test_async_client_requires_connect() -> None:
    # connect() 前の操作は RuntimeError になることを確認する。
    client = quiche.AsyncQuicClient(
        "127.0.0.1",
        4433,
        alpn="hq-interop",
        verify_peer=False,
    )
    with pytest.raises(RuntimeError, match="connect\\(\\) must be called"):
        client.max_datagram_size()


def test_send_datagram_rejects_oversized_input() -> None:
    # 入力サイズ上限を超える datagram は ValueError になることを確認する。
    client = quiche.QuicClient(
        "127.0.0.1",
        4433,
        alpn="hq-interop",
        verify_peer=False,
    )
    with pytest.raises(ValueError, match="maximum allowed size"):
        client.send_datagram(b"x" * (16 * 1024 * 1024 + 1))
