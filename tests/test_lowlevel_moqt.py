"""MOQT draft-18 クライアントの低レベル E2E テスト。"""

from e2e_support import LowLevelDriver

import quiche


def _connect(
    port: int,
) -> tuple[quiche.MoqtClient, LowLevelDriver]:
    """MOQT サーバーへ接続し、MOQT セッションの確立まで駆動する。"""
    client = quiche.MoqtClient("127.0.0.1", port, verify_peer=False)
    driver = LowLevelDriver(client, port)
    client.start()
    assert driver.pump(client.is_connected, timeout_s=10.0)

    def establish_session() -> bool:
        return client.connect() and client.is_session_ready()

    assert driver.pump(establish_session, timeout_s=10.0)
    return client, driver


def _poll_events_until(
    client: quiche.MoqtClient,
    driver: LowLevelDriver,
    kinds: set[str],
    timeout_s: float = 10.0,
) -> list[dict]:
    """指定した種別のイベントが届くまで駆動し、届いたイベントを返す。"""
    events: list[dict] = []

    def received() -> bool:
        while True:
            event = client.poll_event()
            if event is None:
                return False
            events.append(event)
            if event["kind"] in kinds:
                return True

    assert driver.pump(received, timeout_s=timeout_s), (
        f"MOQT イベントが届かなかった: {kinds}, events={events}"
    )
    return events


def test_moqt_subscribe_and_receive_object(moqt_server: int) -> None:
    """
    MOQT サーバーへ接続し、SUBSCRIBE して OBJECT を受信できることを確認する。
    """
    client, driver = _connect(moqt_server)
    try:
        assert client.is_terminated() is False

        assert client.subscribe("test--track")
        events = _poll_events_until(client, driver, {"subscribe_ok", "subscribe_error"})
        reply = events[-1]
        assert reply["kind"] == "subscribe_ok", reply
        assert reply["track_name"] == "test--track"

        events = _poll_events_until(client, driver, {"object"})
        object_event = events[-1]
        assert object_event["kind"] == "object"
        assert object_event["track_name"] == "test--track"
        assert object_event["group_id"] == 0
        assert object_event["object_id"] == 0
        assert object_event["payload"] == b"moqt-object-payload"
        assert object_event["offset"] == 0
        assert client.is_terminated() is False
    finally:
        client.close()
        driver.close()


def test_moqt_publish_and_add_object(moqt_server: int) -> None:
    """
    PUBLISH を送信し、サーバーに受理された後にオブジェクトを追加できることを確認する。
    """
    client, driver = _connect(moqt_server)
    try:
        assert client.publish("publish--track")
        events = _poll_events_until(client, driver, {"publish_ok", "publish_error"})
        reply = events[-1]
        assert reply["kind"] == "publish_ok", reply
        assert reply["track_name"] == "publish--track"

        # PUBLISH 済みのトラックへオブジェクトを追加してもエラーにならないこと。
        client.add_object("publish--track", 0, 0, b"published-payload", fin=True)
        assert client.is_terminated() is False
    finally:
        client.close()
        driver.close()


def test_moqt_fetch(moqt_server: int) -> None:
    """
    FETCH を送信し、FETCH_OK、OBJECT、FETCH_DONE を順に受信できることを確認する。
    """
    client, driver = _connect(moqt_server)
    try:
        request_id = client.fetch("test--track", 0, 0, 0, 0)
        assert request_id > 0

        events = _poll_events_until(client, driver, {"fetch_ok", "fetch_error"})
        reply = events[-1]
        assert reply["kind"] == "fetch_ok", reply
        assert reply["request_id"] == request_id
        assert reply["track_name"] == "test--track"

        events = _poll_events_until(client, driver, {"object", "fetch_error"})
        object_event = events[-1]
        assert object_event["kind"] == "object", object_event
        assert object_event["request_id"] == request_id
        assert object_event["payload"] == b"moqt-object-payload"

        events = _poll_events_until(client, driver, {"fetch_done", "fetch_error"})
        done_event = events[-1]
        assert done_event["kind"] == "fetch_done", done_event
        assert done_event["request_id"] == request_id
    finally:
        client.close()
        driver.close()


def test_moqt_namespace_and_track_status(moqt_server: int) -> None:
    """
    PUBLISH_NAMESPACE、SUBSCRIBE_NAMESPACE、TRACK_STATUS が利用できることを確認する。
    """
    client, driver = _connect(moqt_server)
    try:
        assert client.publish_namespace("namespace")
        events = _poll_events_until(
            client, driver, {"publish_namespace_ok", "publish_namespace_error"}
        )
        reply = events[-1]
        assert reply["kind"] == "publish_namespace_ok", reply
        assert reply["track_name"] == "namespace"

        assert client.subscribe_namespace("namespace")
        events = _poll_events_until(
            client, driver, {"subscribe_namespace_ok", "subscribe_namespace_error"}
        )
        reply = events[-1]
        assert reply["kind"] == "subscribe_namespace_ok", reply
        assert reply["track_name"] == "namespace"

        events = _poll_events_until(
            client,
            driver,
            {"namespace_added", "namespace_done", "subscribe_namespace_error"},
        )
        namespace_event = events[-1]
        assert namespace_event["kind"] == "namespace_added", namespace_event
        assert namespace_event["track_name"] == "namespace-track"

        client.unsubscribe_namespace("namespace")
        client.publish_namespace_done("namespace")

        assert client.track_status("test--status")
        events = _poll_events_until(client, driver, {"track_status_ok", "track_status_error"})
        status_event = events[-1]
        assert status_event["kind"] == "track_status_ok", status_event
        assert status_event["track_name"] == "test--status"
    finally:
        client.close()
        driver.close()


def test_moqt_object_ack(moqt_server: int) -> None:
    """
    SUBSCRIBE 後に OBJECT_ACK を送信できることを確認する。
    """
    client, driver = _connect(moqt_server)
    try:
        assert client.subscribe("test--track")
        events = _poll_events_until(client, driver, {"subscribe_ok", "subscribe_error"})
        assert events[-1]["kind"] == "subscribe_ok", events[-1]

        events = _poll_events_until(client, driver, {"object"})
        assert events[-1]["kind"] == "object", events[-1]

        # OBJECT_ACK に対応したサーバーでは ack_object() が成功する。
        client.ack_object("test--track", 0, 0)
        assert client.is_terminated() is False
    finally:
        client.close()
        driver.close()
