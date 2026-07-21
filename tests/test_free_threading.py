"""free-threading (PEP 703) 対応の検証テスト。

C++ 拡張 (quiche_ext) は nanobind の FREE_THREADED ビルドでビルドされ、NB_MODULE が
Py_mod_gil_not_used を宣言する。ここでは free-threaded 版 Python (3.14t) で実際に
GIL が無効化されたままであることと、複数スレッドから同時にバインディングを使っても
安全であることを検証する。
"""

import subprocess
import sys
import sysconfig
from concurrent.futures import ThreadPoolExecutor, as_completed

import pytest

import quiche


def _is_free_threaded_build() -> bool:
    # Py_GIL_DISABLED は free-threaded ビルドでのみ 1 に設定される。
    return bool(sysconfig.get_config_var("Py_GIL_DISABLED"))


def test_importing_quiche_does_not_enable_gil() -> None:
    # free-threaded ビルドで、quiche を import しても GIL が無効化されたままである
    # ことを確認する。非対応の C 拡張は import 時に GIL を全局でサイレントに再有効化
    # するため、この検査で退行を検知できる。テストプロセスには他の C 拡張 (aioquic /
    # cryptography 等) が読み込まれていて GIL 状態に干渉し得るため、subprocess で
    # quiche だけを import して隔離して検証する。通常 (GIL あり) ビルドでは検証の意味が
    # ないため skip する。
    if not _is_free_threaded_build():
        pytest.skip("not a free-threaded Python build")

    code = "import sys; import quiche; print(sys._is_gil_enabled())"
    result = subprocess.run(
        [sys.executable, "-c", code],
        capture_output=True,
        text=True,
        timeout=30,
        check=True,
    )
    assert result.stdout.strip() == "False", (
        f"importing quiche re-enabled the GIL: "
        f"stdout={result.stdout.strip()!r} stderr={result.stderr.strip()!r}"
    )


def test_concurrent_shared_client_access() -> None:
    # 1 つの QuicClient を複数スレッドから同時に操作し、nanobind の lock_self
    # (オブジェクト単位クリティカルセクション) が free-threaded 下で正しく排他する
    # ことを確認する。GIL がない環境では排他が壊れているとクラッシュやデータ競合が
    # 起きる。GIL ありビルドでも GIL が排他するため正常に通り、両方で意味を持つ。
    client = quiche.QuicClient("127.0.0.1", 4433, alpn="hq-interop", verify_peer=False)
    client.start()

    thread_count = 8
    iterations = 100

    def worker() -> None:
        for _ in range(iterations):
            # 送信・タイマー・状態参照を繰り返して競合を誘発する。
            client.next_send()
            client.next_timeout_ms()
            _ = client.is_connected()
            _ = client.last_error
            _ = client.max_datagram_size()

    # ThreadPoolExecutor がスレッド内例外を Future に載せるため、広い except は不要。
    with ThreadPoolExecutor(max_workers=thread_count) as executor:
        futures = [executor.submit(worker) for _ in range(thread_count)]
        for future in as_completed(futures, timeout=10):
            future.result()
