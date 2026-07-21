"""AsyncQuicClient のストリームエコーに対する PBT。

固定文字列の単体 E2E では拾いにくい任意ペイロードのラウンドトリップを検証する。
モック・スタブは使わない。
"""

from e2e_support import connect_async, read_until_fin_async
from hypothesis import HealthCheck, given, settings
from hypothesis import strategies as st


@given(payload=st.binary(min_size=1, max_size=4096))
@settings(
    max_examples=20,
    deadline=None,
    # 同一エコーサーバーを例間で使い回すのは意図的 (起動コストが高いため)。
    suppress_health_check=[HealthCheck.function_scoped_fixture],
)
async def test_bidirectional_echo_roundtrip(echo_server: int, payload: bytes) -> None:
    # 任意バイト列を双方向ストリームで echo できることを確認する。
    client = await connect_async(echo_server)
    try:
        stream = await client.open_bidirectional_stream(timeout=5.0)
        await stream.write(payload, fin=True, timeout=5.0)
        assert await read_until_fin_async(stream) == payload
    finally:
        await client.close()
