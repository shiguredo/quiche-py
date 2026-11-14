#include "quiche_glue.h"

#include <nanobind/nanobind.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>

#include <cstdint>
#include <optional>
#include <string>
#include <stdexcept>
#include <vector>

namespace nb = nanobind;

namespace {

// 1 回の送信で取り出すパケットを収めるバッファサイズ。QUIC の 1 パケットは
// kMaxOutgoingPacketSize 以下に収まるため、十分な余裕を確保しておく。
constexpr size_t kSendBufferSize = 65535;

// Python → C++ へ渡すバイト列 / read バッファの上限。不正な巨大入力で
// メモリを食い潰さないためのガード (QUIC の実効ペイロードより十分大きい)。
constexpr size_t kMaxInputBytes = 16 * 1024 * 1024;

const quiche_py_api& Api() {
  static const quiche_py_api* api = quiche_py_get_api();
  if (api == nullptr) {
    throw std::runtime_error("quiche-py glue API is unavailable");
  }
  if (api->abi_version != QUICHE_PY_API_VERSION) {
    throw std::runtime_error("quiche-py glue API version mismatch");
  }
  return *api;
}

std::string BytesToString(const nb::bytes& value) {
  char* buffer = nullptr;
  Py_ssize_t size = 0;
  if (PyBytes_AsStringAndSize(value.ptr(), &buffer, &size) != 0) {
    throw nb::python_error();
  }
  if (size < 0 || static_cast<size_t>(size) > kMaxInputBytes) {
    throw nb::value_error("input bytes exceed the maximum allowed size");
  }
  return std::string(buffer, static_cast<size_t>(size));
}

nb::bytes BytesFromBuffer(const char* data, size_t size) {
  if (size == 0) {
    return nb::bytes("");
  }
  return nb::bytes(data, size);
}

// None の引数は QUICHE_PY_CONFIG_UNSET として扱い、既存の設定を変更しない。
quiche_py_config MakeConfig(
    const std::optional<uint64_t>& max_idle_timeout_ms,
    const std::optional<uint64_t>& initial_max_data,
    const std::optional<uint64_t>& initial_max_stream_data_bidi_local,
    const std::optional<uint64_t>& initial_max_stream_data_bidi_remote,
    const std::optional<uint64_t>& initial_max_stream_data_uni,
    const std::optional<uint64_t>& max_ack_delay_ms,
    const std::optional<uint64_t>& min_ack_delay_us,
    const std::optional<uint64_t>& active_connection_id_limit,
    const std::optional<uint64_t>& max_datagram_frame_size) {
  quiche_py_config config{};
  config.max_idle_timeout_ms =
      max_idle_timeout_ms.value_or(QUICHE_PY_CONFIG_UNSET);
  config.initial_max_data =
      initial_max_data.value_or(QUICHE_PY_CONFIG_UNSET);
  config.initial_max_stream_data_bidi_local =
      initial_max_stream_data_bidi_local.value_or(QUICHE_PY_CONFIG_UNSET);
  config.initial_max_stream_data_bidi_remote =
      initial_max_stream_data_bidi_remote.value_or(QUICHE_PY_CONFIG_UNSET);
  config.initial_max_stream_data_uni =
      initial_max_stream_data_uni.value_or(QUICHE_PY_CONFIG_UNSET);
  config.max_ack_delay_ms =
      max_ack_delay_ms.value_or(QUICHE_PY_CONFIG_UNSET);
  config.min_ack_delay_us =
      min_ack_delay_us.value_or(QUICHE_PY_CONFIG_UNSET);
  config.active_connection_id_limit =
      active_connection_id_limit.value_or(QUICHE_PY_CONFIG_UNSET);
  config.max_datagram_frame_size =
      max_datagram_frame_size.value_or(QUICHE_PY_CONFIG_UNSET);
  return config;
}

[[noreturn]] void RaisePythonException(PyObject* exception_type,
                                       const std::string& message) {
  PyErr_SetString(exception_type, message.c_str());
  throw nb::python_error();
}

class PyQuicClient;

class PyQuicStream {
 public:
  PyQuicStream(PyQuicClient* client, uint32_t stream_id, bool unidirectional)
      : client_(client), stream_id_(stream_id), unidirectional_(unidirectional) {}

  uint32_t stream_id() const { return stream_id_; }
  bool unidirectional() const { return unidirectional_; }

  nb::object read(size_t max_bytes);
  bool write(const nb::bytes& data, bool fin = false);
  bool atomic_write(const nb::bytes& data, bool fin = false);
  void set_priority(uint32_t send_group_id, uint32_t send_order);
  void reset(uint32_t error_code);
  void stop_sending(uint32_t error_code);
  bool can_write() const;
  bool is_open() const;

 private:
  PyQuicClient* client_;
  uint32_t stream_id_;
  bool unidirectional_;
};

class PyQuicClient {
 public:
  PyQuicClient(const std::string& host, uint16_t port,
               const std::string& server_name, const std::string& alpn,
               bool verify_peer)
      : api_(Api()) {
    quiche_py_client_options options{};
    options.host = host.c_str();
    options.port = port;
    options.server_name = server_name.empty() ? nullptr : server_name.c_str();
    options.alpn = alpn.empty() ? nullptr : alpn.c_str();
    options.verify_peer = verify_peer;
    handle_ = api_.client_create(&options);
    if (handle_ == nullptr) {
      throw std::runtime_error("failed to allocate QUIC client handle");
    }
    std::string err = api_.client_last_error(handle_);
    if (!err.empty()) {
      api_.client_destroy(handle_);
      handle_ = nullptr;
      throw std::runtime_error("failed to create QUIC client: " + err);
    }
  }

  ~PyQuicClient() {
    if (handle_ != nullptr) {
      api_.client_destroy(handle_);
    }
  }

  void start() {
    quiche_py_status status = api_.client_start(handle_);
    ThrowIfNotOk("failed to start the QUIC handshake", status);
  }

  void receive_packet(const nb::bytes& data) {
    std::string payload = BytesToString(data);
    quiche_py_status status = api_.client_receive_packet(
        handle_, reinterpret_cast<const uint8_t*>(payload.data()),
        payload.size());
    // 接続が閉じた場合はエラーにせず、is_connected() で観測させる。
    if (status == QUICHE_PY_STATUS_CLOSED) {
      return;
    }
    ThrowIfNotOk("failed to process an incoming packet", status);
  }

  // ハンドシェイク開始前に接続設定を反映する。
  void configure(const std::optional<uint64_t>& max_idle_timeout_ms = std::nullopt,
                 const std::optional<uint64_t>& initial_max_data = std::nullopt,
                 const std::optional<uint64_t>& initial_max_stream_data_bidi_local = std::nullopt,
                 const std::optional<uint64_t>& initial_max_stream_data_bidi_remote = std::nullopt,
                 const std::optional<uint64_t>& initial_max_stream_data_uni = std::nullopt,
                 const std::optional<uint64_t>& max_ack_delay_ms = std::nullopt,
                 const std::optional<uint64_t>& min_ack_delay_us = std::nullopt,
                 const std::optional<uint64_t>& active_connection_id_limit = std::nullopt,
                 const std::optional<uint64_t>& max_datagram_frame_size = std::nullopt) {
    quiche_py_config config = MakeConfig(
        max_idle_timeout_ms, initial_max_data,
        initial_max_stream_data_bidi_local, initial_max_stream_data_bidi_remote,
        initial_max_stream_data_uni, max_ack_delay_ms, min_ack_delay_us,
        active_connection_id_limit, max_datagram_frame_size);
    ThrowIfNotOk("failed to configure the QUIC client",
                 api_.client_configure(handle_, &config));
  }

  // 受信パケットをまとめて状態機械へ流し込む。
  void receive_packets(const nb::list& packets) {
    std::vector<std::string> payloads;
    payloads.reserve(packets.size());
    for (nb::handle item : packets) {
      nb::bytes bytes = nb::cast<nb::bytes>(item);
      payloads.push_back(BytesToString(bytes));
    }
    std::vector<const uint8_t*> data;
    std::vector<size_t> data_lengths;
    data.reserve(payloads.size());
    data_lengths.reserve(payloads.size());
    for (const std::string& payload : payloads) {
      data.push_back(reinterpret_cast<const uint8_t*>(payload.data()));
      data_lengths.push_back(payload.size());
    }
    quiche_py_status status = api_.client_receive_packets(
        handle_, data.data(), data_lengths.data(), data.size());
    if (status == QUICHE_PY_STATUS_CLOSED) {
      return;
    }
    ThrowIfNotOk("failed to process incoming packets", status);
  }

  nb::object stats() {
    quiche_py_connection_stats stats{};
    if (!api_.client_stats(handle_, &stats)) {
      return nb::none();
    }
    return nb::cast(stats);
  }

  nb::object socket_packets_dropped() const {
    uint64_t packets_dropped = 0;
    if (!api_.client_socket_packets_dropped(handle_, &packets_dropped)) {
      return nb::none();
    }
    return nb::cast(packets_dropped);
  }

  nb::object next_send() {
    std::string buffer(kSendBufferSize, '\0');
    size_t written = 0;
    quiche_py_status status = api_.client_next_send(
        handle_, reinterpret_cast<uint8_t*>(buffer.data()), buffer.size(),
        &written);
    if (status == QUICHE_PY_STATUS_WOULD_BLOCK) {
      return nb::none();
    }
    ThrowIfNotOk("failed to fetch an outgoing packet", status);
    return BytesFromBuffer(buffer.data(), written);
  }

  nb::object next_timeout_ms() {
    uint64_t timeout_ms = 0;
    bool has_timeout = false;
    quiche_py_status status =
        api_.client_next_timeout_ms(handle_, &timeout_ms, &has_timeout);
    ThrowIfNotOk("failed to query the next timeout", status);
    if (!has_timeout) {
      return nb::none();
    }
    return nb::cast(timeout_ms);
  }

  void handle_timeout() {
    quiche_py_status status = api_.client_handle_timeout(handle_);
    if (status == QUICHE_PY_STATUS_CLOSED) {
      return;
    }
    ThrowIfNotOk("failed to handle a timeout", status);
  }

  void close(uint32_t error_code = 0, const std::string& reason = "") {
    api_.client_close(handle_, error_code, reason.c_str());
  }

  bool is_connected() const { return api_.client_is_connected(handle_); }

  std::string last_error() const {
    const char* message = api_.client_last_error(handle_);
    return message == nullptr ? std::string() : std::string(message);
  }

  size_t max_datagram_size() const { return api_.client_max_datagram_size(handle_); }

  PyQuicStream* open_bidirectional_stream() {
    return OpenStream(/*unidirectional=*/false);
  }

  PyQuicStream* open_unidirectional_stream() {
    return OpenStream(/*unidirectional=*/true);
  }

  PyQuicStream* accept_bidirectional_stream() {
    return AcceptStream(/*unidirectional=*/false);
  }

  PyQuicStream* accept_unidirectional_stream() {
    return AcceptStream(/*unidirectional=*/true);
  }

  bool send_datagram(const nb::bytes& data) {
    std::string payload = BytesToString(data);
    quiche_py_status status = api_.client_send_datagram(
        handle_, reinterpret_cast<const uint8_t*>(payload.data()),
        payload.size());
    if (status == QUICHE_PY_STATUS_WOULD_BLOCK) {
      return false;
    }
    ThrowIfNotOk("failed to send datagram", status);
    return true;
  }

  nb::object receive_datagram() {
    size_t datagram_size = 0;
    quiche_py_status status =
        api_.client_peek_datagram_size(handle_, &datagram_size);
    if (status == QUICHE_PY_STATUS_WOULD_BLOCK) {
      return nb::none();
    }
    ThrowIfNotOk("failed to receive datagram", status);

    std::string buffer(datagram_size, '\0');
    size_t bytes_read = 0;
    status = api_.client_receive_datagram(
        handle_, buffer.empty() ? nullptr
                                : reinterpret_cast<uint8_t*>(buffer.data()),
        buffer.size(), &bytes_read);
    ThrowIfNotOk("failed to receive datagram", status);
    return BytesFromBuffer(buffer.data(), bytes_read);
  }

  quiche_py_client* handle() const { return handle_; }
  const quiche_py_api& api() const { return api_; }

  void ThrowIfNotOk(const std::string& context,
                    quiche_py_status status) const {
    if (status != QUICHE_PY_STATUS_OK) {
      ThrowStatus(context, status);
    }
  }

  [[noreturn]] void ThrowStatus(const std::string& context,
                                quiche_py_status status) const {
    std::string message = context;
    std::string detail = last_error();
    if (!detail.empty()) {
      message += ": " + detail;
    }

    switch (status) {
      case QUICHE_PY_STATUS_INVALID_ARGUMENT:
      case QUICHE_PY_STATUS_TOO_BIG:
        RaisePythonException(PyExc_ValueError, message);
      case QUICHE_PY_STATUS_NOT_FOUND:
        RaisePythonException(PyExc_KeyError, message);
      case QUICHE_PY_STATUS_CLOSED:
        RaisePythonException(PyExc_ConnectionError, message);
      case QUICHE_PY_STATUS_ERROR:
        RaisePythonException(PyExc_RuntimeError, message);
      case QUICHE_PY_STATUS_WOULD_BLOCK:
      case QUICHE_PY_STATUS_OK:
        break;
    }
    throw std::runtime_error(message);
  }

 private:
  PyQuicStream* OpenStream(bool unidirectional) {
    uint32_t stream_id = 0;
    quiche_py_status status =
        api_.client_open_stream(handle_, unidirectional, &stream_id);
    if (status == QUICHE_PY_STATUS_WOULD_BLOCK) {
      return nullptr;
    }
    ThrowIfNotOk("failed to open a QUIC stream", status);
    return new PyQuicStream(this, stream_id, unidirectional);
  }

  PyQuicStream* AcceptStream(bool unidirectional) {
    uint32_t stream_id = 0;
    quiche_py_status status =
        api_.client_accept_stream(handle_, unidirectional, &stream_id);
    if (status == QUICHE_PY_STATUS_WOULD_BLOCK) {
      return nullptr;
    }
    ThrowIfNotOk("failed to accept a QUIC stream", status);
    return new PyQuicStream(this, stream_id, unidirectional);
  }

  const quiche_py_api& api_;
  quiche_py_client* handle_ = nullptr;
};

nb::object PyQuicStream::read(size_t max_bytes) {
  if (max_bytes == 0) {
    throw nb::value_error("max_bytes must be greater than zero");
  }
  if (max_bytes > kMaxInputBytes) {
    throw nb::value_error("max_bytes exceed the maximum allowed size");
  }

  std::string buffer(max_bytes, '\0');
  size_t bytes_read = 0;
  bool fin = false;
  quiche_py_status status = client_->api().client_read_stream(
      client_->handle(), stream_id_, reinterpret_cast<uint8_t*>(buffer.data()),
      buffer.size(), &bytes_read, &fin);
  if (status == QUICHE_PY_STATUS_WOULD_BLOCK) {
    return nb::none();
  }
  client_->ThrowIfNotOk("failed to read from a QUIC stream", status);

  return nb::make_tuple(BytesFromBuffer(buffer.data(), bytes_read), fin);
}

bool PyQuicStream::write(const nb::bytes& data, bool fin) {
  std::string payload = BytesToString(data);
  quiche_py_status status = client_->api().client_write_stream(
      client_->handle(), stream_id_,
      reinterpret_cast<const uint8_t*>(payload.data()), payload.size(), fin);
  if (status == QUICHE_PY_STATUS_WOULD_BLOCK) {
    return false;
  }
  client_->ThrowIfNotOk("failed to write to a QUIC stream", status);
  return true;
}

bool PyQuicStream::atomic_write(const nb::bytes& data, bool fin) {
  std::string payload = BytesToString(data);
  quiche_py_status status = client_->api().client_write_stream_atomic(
      client_->handle(), stream_id_,
      reinterpret_cast<const uint8_t*>(payload.data()), payload.size(), fin);
  if (status == QUICHE_PY_STATUS_WOULD_BLOCK) {
    return false;
  }
  client_->ThrowIfNotOk("failed to atomically write to a QUIC stream", status);
  return true;
}

void PyQuicStream::set_priority(uint32_t send_group_id, uint32_t send_order) {
  quiche_py_status status = client_->api().client_stream_set_priority(
      client_->handle(), stream_id_, send_group_id, send_order);
  client_->ThrowIfNotOk("failed to set the QUIC stream priority", status);
}

void PyQuicStream::reset(uint32_t error_code) {
  quiche_py_status status =
      client_->api().client_reset_stream(client_->handle(), stream_id_,
                                         error_code);
  client_->ThrowIfNotOk("failed to reset a QUIC stream", status);
}

void PyQuicStream::stop_sending(uint32_t error_code) {
  quiche_py_status status =
      client_->api().client_stop_sending(client_->handle(), stream_id_,
                                         error_code);
  client_->ThrowIfNotOk("failed to stop sending on a QUIC stream", status);
}

bool PyQuicStream::can_write() const {
  return client_->api().client_stream_can_write(client_->handle(), stream_id_);
}

bool PyQuicStream::is_open() const {
  return client_->api().client_stream_exists(client_->handle(), stream_id_);
}

class PyHttp3Response {
 public:
  explicit PyHttp3Response(quiche_py_http3_response* handle) : handle_(handle) {}

  ~PyHttp3Response() {
    if (handle_ != nullptr) {
      Api().h3_response_destroy(handle_);
    }
  }

  PyHttp3Response(const PyHttp3Response&) = delete;
  PyHttp3Response& operator=(const PyHttp3Response&) = delete;

  uint32_t stream_id() const {
    return Api().h3_response_stream_id(handle_);
  }

  int status_code() const { return Api().h3_response_status_code(handle_); }

  nb::list headers() const {
    nb::list result;
    size_t count = Api().h3_response_header_count(handle_);
    for (size_t i = 0; i < count; ++i) {
      const uint8_t* name = nullptr;
      size_t name_len = 0;
      const uint8_t* value = nullptr;
      size_t value_len = 0;
      quiche_py_status status = Api().h3_response_header_at(
          handle_, i, &name, &name_len, &value, &value_len);
      if (status != QUICHE_PY_STATUS_OK) {
        RaisePythonException(PyExc_RuntimeError,
                             "failed to read an HTTP/3 response header");
      }
      result.append(nb::make_tuple(BytesFromBuffer(reinterpret_cast<const char*>(name), name_len),
                                   BytesFromBuffer(reinterpret_cast<const char*>(value), value_len)));
    }
    return result;
  }

  nb::bytes body() const {
    size_t body_len = 0;
    const uint8_t* body = Api().h3_response_body(handle_, &body_len);
    return BytesFromBuffer(reinterpret_cast<const char*>(body), body_len);
  }

 private:
  quiche_py_http3_response* handle_ = nullptr;
};

class PyHttp3Client {
 public:
  PyHttp3Client(const std::string& host, uint16_t port,
                const std::string& server_name, bool verify_peer)
      : api_(Api()) {
    quiche_py_http3_client_options options{};
    options.host = host.c_str();
    options.port = port;
    options.server_name = server_name.empty() ? nullptr : server_name.c_str();
    options.verify_peer = verify_peer;
    handle_ = api_.h3_client_create(&options);
    if (handle_ == nullptr) {
      throw std::runtime_error("failed to allocate HTTP/3 client handle");
    }
    std::string err = api_.h3_client_last_error(handle_);
    if (!err.empty()) {
      api_.h3_client_destroy(handle_);
      handle_ = nullptr;
      throw std::runtime_error("failed to create HTTP/3 client: " + err);
    }
  }

  ~PyHttp3Client() {
    if (handle_ != nullptr) {
      api_.h3_client_destroy(handle_);
    }
  }

  void start() {
    ThrowIfNotOk("failed to start the HTTP/3 handshake",
                 api_.h3_client_start(handle_));
  }

  void receive_packet(const nb::bytes& data) {
    std::string payload = BytesToString(data);
    quiche_py_status status = api_.h3_client_receive_packet(
        handle_, reinterpret_cast<const uint8_t*>(payload.data()),
        payload.size());
    if (status == QUICHE_PY_STATUS_CLOSED) {
      return;
    }
    ThrowIfNotOk("failed to process an incoming packet", status);
  }

  void configure(const std::optional<uint64_t>& max_idle_timeout_ms = std::nullopt,
                 const std::optional<uint64_t>& initial_max_data = std::nullopt,
                 const std::optional<uint64_t>& initial_max_stream_data_bidi_local = std::nullopt,
                 const std::optional<uint64_t>& initial_max_stream_data_bidi_remote = std::nullopt,
                 const std::optional<uint64_t>& initial_max_stream_data_uni = std::nullopt,
                 const std::optional<uint64_t>& max_ack_delay_ms = std::nullopt,
                 const std::optional<uint64_t>& min_ack_delay_us = std::nullopt,
                 const std::optional<uint64_t>& active_connection_id_limit = std::nullopt,
                 const std::optional<uint64_t>& max_datagram_frame_size = std::nullopt) {
    quiche_py_config config = MakeConfig(
        max_idle_timeout_ms, initial_max_data,
        initial_max_stream_data_bidi_local, initial_max_stream_data_bidi_remote,
        initial_max_stream_data_uni, max_ack_delay_ms, min_ack_delay_us,
        active_connection_id_limit, max_datagram_frame_size);
    ThrowIfNotOk("failed to configure the HTTP/3 client",
                 api_.h3_client_configure(handle_, &config));
  }

  void receive_packets(const nb::list& packets) {
    std::vector<std::string> payloads;
    payloads.reserve(packets.size());
    for (nb::handle item : packets) {
      nb::bytes bytes = nb::cast<nb::bytes>(item);
      payloads.push_back(BytesToString(bytes));
    }
    std::vector<const uint8_t*> data;
    std::vector<size_t> data_lengths;
    data.reserve(payloads.size());
    data_lengths.reserve(payloads.size());
    for (const std::string& payload : payloads) {
      data.push_back(reinterpret_cast<const uint8_t*>(payload.data()));
      data_lengths.push_back(payload.size());
    }
    quiche_py_status status = api_.h3_client_receive_packets(
        handle_, data.data(), data_lengths.data(), data.size());
    if (status == QUICHE_PY_STATUS_CLOSED) {
      return;
    }
    ThrowIfNotOk("failed to process incoming packets", status);
  }

  nb::object stats() {
    quiche_py_connection_stats stats{};
    if (!api_.h3_client_stats(handle_, &stats)) {
      return nb::none();
    }
    return nb::cast(stats);
  }

  nb::object socket_packets_dropped() const {
    uint64_t packets_dropped = 0;
    if (!api_.h3_client_socket_packets_dropped(handle_, &packets_dropped)) {
      return nb::none();
    }
    return nb::cast(packets_dropped);
  }

  bool settings_received() const {
    return api_.h3_client_settings_received(handle_);
  }

  nb::dict settings() const {
    nb::dict values;
    size_t count = api_.h3_client_setting_count(handle_);
    for (size_t index = 0; index < count; ++index) {
      uint64_t identifier = 0;
      uint64_t value = 0;
      if (api_.h3_client_setting_at(handle_, index, &identifier, &value)) {
        values[nb::cast(identifier)] = nb::cast(value);
      }
    }
    return values;
  }

  nb::list settings_wire_order() const {
    nb::list order;
    size_t count = api_.h3_client_settings_wire_order_count(handle_);
    for (size_t index = 0; index < count; ++index) {
      uint64_t identifier = 0;
      if (api_.h3_client_settings_wire_order_at(handle_, index, &identifier)) {
        order.append(nb::cast(identifier));
      }
    }
    return order;
  }

  nb::object next_send() {
    std::string buffer(kSendBufferSize, '\0');
    size_t written = 0;
    quiche_py_status status = api_.h3_client_next_send(
        handle_, reinterpret_cast<uint8_t*>(buffer.data()), buffer.size(),
        &written);
    if (status == QUICHE_PY_STATUS_WOULD_BLOCK) {
      return nb::none();
    }
    ThrowIfNotOk("failed to fetch an outgoing packet", status);
    return BytesFromBuffer(buffer.data(), written);
  }

  nb::object next_timeout_ms() {
    uint64_t timeout_ms = 0;
    bool has_timeout = false;
    ThrowIfNotOk("failed to query the next timeout",
                 api_.h3_client_next_timeout_ms(handle_, &timeout_ms,
                                                &has_timeout));
    if (!has_timeout) {
      return nb::none();
    }
    return nb::cast(timeout_ms);
  }

  void handle_timeout() {
    quiche_py_status status = api_.h3_client_handle_timeout(handle_);
    if (status == QUICHE_PY_STATUS_CLOSED) {
      return;
    }
    ThrowIfNotOk("failed to handle a timeout", status);
  }

  void close(uint32_t error_code = 0, const std::string& reason = "") {
    api_.h3_client_close(handle_, error_code, reason.c_str());
  }

  bool is_connected() const { return api_.h3_client_is_connected(handle_); }

  std::string last_error() const {
    const char* message = api_.h3_client_last_error(handle_);
    return message == nullptr ? std::string() : std::string(message);
  }

  nb::object submit_request(const nb::object& headers, const nb::bytes& body,
                            bool fin) {
    std::vector<quiche_py_http3_header> native_headers;
    std::vector<std::string> owned_names;
    std::vector<std::string> owned_values;
    nb::list header_list(headers);
    for (size_t i = 0; i < nb::len(header_list); ++i) {
      nb::tuple pair = nb::cast<nb::tuple>(header_list[i]);
      if (nb::len(pair) != 2) {
        throw nb::value_error("each header must be a (name, value) pair");
      }
      owned_names.push_back(BytesToString(nb::cast<nb::bytes>(pair[0])));
      owned_values.push_back(BytesToString(nb::cast<nb::bytes>(pair[1])));
    }
    native_headers.reserve(owned_names.size());
    for (size_t i = 0; i < owned_names.size(); ++i) {
      quiche_py_http3_header header{};
      header.name =
          reinterpret_cast<const uint8_t*>(owned_names[i].data());
      header.name_len = owned_names[i].size();
      header.value =
          reinterpret_cast<const uint8_t*>(owned_values[i].data());
      header.value_len = owned_values[i].size();
      native_headers.push_back(header);
    }

    std::string body_payload = BytesToString(body);
    uint32_t stream_id = 0;
    quiche_py_status status = api_.h3_client_submit_request(
        handle_, native_headers.empty() ? nullptr : native_headers.data(),
        native_headers.size(),
        reinterpret_cast<const uint8_t*>(body_payload.data()),
        body_payload.size(), fin, &stream_id);
    if (status == QUICHE_PY_STATUS_WOULD_BLOCK) {
      return nb::none();
    }
    ThrowIfNotOk("failed to submit an HTTP/3 request", status);
    return nb::cast(stream_id);
  }

  nb::object take_response(uint32_t stream_id) {
    quiche_py_http3_response* response = nullptr;
    quiche_py_status status =
        api_.h3_client_take_response(handle_, stream_id, &response);
    if (status == QUICHE_PY_STATUS_WOULD_BLOCK) {
      return nb::none();
    }
    ThrowIfNotOk("failed to take an HTTP/3 response", status);
    return nb::cast(new PyHttp3Response(response),
                    nb::rv_policy::take_ownership);
  }

  void ThrowIfNotOk(const std::string& context,
                    quiche_py_status status) const {
    if (status != QUICHE_PY_STATUS_OK) {
      ThrowStatus(context, status);
    }
  }

  [[noreturn]] void ThrowStatus(const std::string& context,
                                quiche_py_status status) const {
    std::string message = context;
    std::string detail = last_error();
    if (!detail.empty()) {
      message += ": " + detail;
    }
    switch (status) {
      case QUICHE_PY_STATUS_INVALID_ARGUMENT:
      case QUICHE_PY_STATUS_TOO_BIG:
        RaisePythonException(PyExc_ValueError, message);
      case QUICHE_PY_STATUS_NOT_FOUND:
        RaisePythonException(PyExc_KeyError, message);
      case QUICHE_PY_STATUS_CLOSED:
        RaisePythonException(PyExc_ConnectionError, message);
      case QUICHE_PY_STATUS_ERROR:
        RaisePythonException(PyExc_RuntimeError, message);
      case QUICHE_PY_STATUS_WOULD_BLOCK:
      case QUICHE_PY_STATUS_OK:
        break;
    }
    throw std::runtime_error(message);
  }

 private:
  const quiche_py_api& api_;
  quiche_py_http3_client* handle_ = nullptr;
};

class PyWebTransportClient;

class PyWebTransportStream {
 public:
  PyWebTransportStream(PyWebTransportClient* client, uint32_t stream_id,
                       bool unidirectional)
      : client_(client),
        stream_id_(stream_id),
        unidirectional_(unidirectional) {}

  uint32_t stream_id() const { return stream_id_; }
  bool unidirectional() const { return unidirectional_; }

  nb::object read(size_t max_bytes);
  bool write(const nb::bytes& data, bool fin = false);
  bool atomic_write(const nb::bytes& data, bool fin = false);
  void set_priority(uint32_t send_group_id, uint32_t send_order);
  void reset(uint32_t error_code);
  void stop_sending(uint32_t error_code);
  bool can_write() const;
  bool is_open() const;

 private:
  PyWebTransportClient* client_;
  uint32_t stream_id_;
  bool unidirectional_;
};

class PyWebTransportClient {
 public:
  PyWebTransportClient(const std::string& host, uint16_t port,
                       const std::string& server_name, bool verify_peer)
      : api_(Api()) {
    quiche_py_wt_client_options options{};
    options.host = host.c_str();
    options.port = port;
    options.server_name = server_name.empty() ? nullptr : server_name.c_str();
    options.verify_peer = verify_peer;
    handle_ = api_.wt_client_create(&options);
    if (handle_ == nullptr) {
      throw std::runtime_error("failed to allocate WebTransport client handle");
    }
    std::string err = api_.wt_client_last_error(handle_);
    if (!err.empty()) {
      api_.wt_client_destroy(handle_);
      handle_ = nullptr;
      throw std::runtime_error("failed to create WebTransport client: " + err);
    }
  }

  ~PyWebTransportClient() {
    if (handle_ != nullptr) {
      api_.wt_client_destroy(handle_);
    }
  }

  void start() {
    ThrowIfNotOk("failed to start the WebTransport handshake",
                 api_.wt_client_start(handle_));
  }

  void receive_packet(const nb::bytes& data) {
    std::string payload = BytesToString(data);
    quiche_py_status status = api_.wt_client_receive_packet(
        handle_, reinterpret_cast<const uint8_t*>(payload.data()),
        payload.size());
    if (status == QUICHE_PY_STATUS_CLOSED) {
      return;
    }
    ThrowIfNotOk("failed to process an incoming packet", status);
  }

  void configure(const std::optional<uint64_t>& max_idle_timeout_ms = std::nullopt,
                 const std::optional<uint64_t>& initial_max_data = std::nullopt,
                 const std::optional<uint64_t>& initial_max_stream_data_bidi_local = std::nullopt,
                 const std::optional<uint64_t>& initial_max_stream_data_bidi_remote = std::nullopt,
                 const std::optional<uint64_t>& initial_max_stream_data_uni = std::nullopt,
                 const std::optional<uint64_t>& max_ack_delay_ms = std::nullopt,
                 const std::optional<uint64_t>& min_ack_delay_us = std::nullopt,
                 const std::optional<uint64_t>& active_connection_id_limit = std::nullopt,
                 const std::optional<uint64_t>& max_datagram_frame_size = std::nullopt) {
    quiche_py_config config = MakeConfig(
        max_idle_timeout_ms, initial_max_data,
        initial_max_stream_data_bidi_local, initial_max_stream_data_bidi_remote,
        initial_max_stream_data_uni, max_ack_delay_ms, min_ack_delay_us,
        active_connection_id_limit, max_datagram_frame_size);
    ThrowIfNotOk("failed to configure the WebTransport client",
                 api_.wt_client_configure(handle_, &config));
  }

  void receive_packets(const nb::list& packets) {
    std::vector<std::string> payloads;
    payloads.reserve(packets.size());
    for (nb::handle item : packets) {
      nb::bytes bytes = nb::cast<nb::bytes>(item);
      payloads.push_back(BytesToString(bytes));
    }
    std::vector<const uint8_t*> data;
    std::vector<size_t> data_lengths;
    data.reserve(payloads.size());
    data_lengths.reserve(payloads.size());
    for (const std::string& payload : payloads) {
      data.push_back(reinterpret_cast<const uint8_t*>(payload.data()));
      data_lengths.push_back(payload.size());
    }
    quiche_py_status status = api_.wt_client_receive_packets(
        handle_, data.data(), data_lengths.data(), data.size());
    if (status == QUICHE_PY_STATUS_CLOSED) {
      return;
    }
    ThrowIfNotOk("failed to process incoming packets", status);
  }

  nb::object stats() {
    quiche_py_connection_stats stats{};
    if (!api_.wt_client_stats(handle_, &stats)) {
      return nb::none();
    }
    return nb::cast(stats);
  }

  nb::object socket_packets_dropped() const {
    uint64_t packets_dropped = 0;
    if (!api_.wt_client_socket_packets_dropped(handle_, &packets_dropped)) {
      return nb::none();
    }
    return nb::cast(packets_dropped);
  }

  nb::object session_stats() {
    quiche_py_wt_session_stats stats{};
    if (!api_.wt_client_session_stats(handle_, &stats)) {
      return nb::none();
    }
    return nb::cast(stats);
  }

  nb::object datagram_stats() {
    quiche_py_wt_datagram_stats stats{};
    if (!api_.wt_client_datagram_stats(handle_, &stats)) {
      return nb::none();
    }
    return nb::cast(stats);
  }

  bool is_draining() const { return api_.wt_client_is_draining(handle_); }

  void notify_draining() { api_.wt_client_notify_draining(handle_); }

  nb::object protocol() const {
    const char* protocol = api_.wt_client_protocol(handle_);
    if (protocol == nullptr) {
      return nb::none();
    }
    return nb::cast(std::string(protocol));
  }

  nb::object next_send() {
    std::string buffer(kSendBufferSize, '\0');
    size_t written = 0;
    quiche_py_status status = api_.wt_client_next_send(
        handle_, reinterpret_cast<uint8_t*>(buffer.data()), buffer.size(),
        &written);
    if (status == QUICHE_PY_STATUS_WOULD_BLOCK) {
      return nb::none();
    }
    ThrowIfNotOk("failed to fetch an outgoing packet", status);
    return BytesFromBuffer(buffer.data(), written);
  }

  nb::object next_timeout_ms() {
    uint64_t timeout_ms = 0;
    bool has_timeout = false;
    ThrowIfNotOk(
        "failed to query the next timeout",
        api_.wt_client_next_timeout_ms(handle_, &timeout_ms, &has_timeout));
    if (!has_timeout) {
      return nb::none();
    }
    return nb::cast(timeout_ms);
  }

  void handle_timeout() {
    quiche_py_status status = api_.wt_client_handle_timeout(handle_);
    if (status == QUICHE_PY_STATUS_CLOSED) {
      return;
    }
    ThrowIfNotOk("failed to handle a timeout", status);
  }

  void close(uint32_t error_code = 0, const std::string& reason = "") {
    api_.wt_client_close(handle_, error_code, reason.c_str());
  }

  bool is_connected() const { return api_.wt_client_is_connected(handle_); }
  bool is_session_ready() const {
    return api_.wt_client_is_session_ready(handle_);
  }

  std::string last_error() const {
    const char* message = api_.wt_client_last_error(handle_);
    return message == nullptr ? std::string() : std::string(message);
  }

  bool connect_session(const std::string& path, const nb::object& headers) {
    std::vector<quiche_py_http3_header> native_headers;
    std::vector<std::string> owned_names;
    std::vector<std::string> owned_values;
    if (!headers.is_none()) {
      nb::list header_list(headers);
      for (size_t i = 0; i < nb::len(header_list); ++i) {
        nb::tuple pair = nb::cast<nb::tuple>(header_list[i]);
        if (nb::len(pair) != 2) {
          throw nb::value_error("each header must be a (name, value) pair");
        }
        owned_names.push_back(BytesToString(nb::cast<nb::bytes>(pair[0])));
        owned_values.push_back(BytesToString(nb::cast<nb::bytes>(pair[1])));
      }
      native_headers.reserve(owned_names.size());
      for (size_t i = 0; i < owned_names.size(); ++i) {
        quiche_py_http3_header header{};
        header.name =
            reinterpret_cast<const uint8_t*>(owned_names[i].data());
        header.name_len = owned_names[i].size();
        header.value =
            reinterpret_cast<const uint8_t*>(owned_values[i].data());
        header.value_len = owned_values[i].size();
        native_headers.push_back(header);
      }
    }
    quiche_py_status status = api_.wt_client_connect_session(
        handle_, path.c_str(),
        native_headers.empty() ? nullptr : native_headers.data(),
        native_headers.size());
    if (status == QUICHE_PY_STATUS_WOULD_BLOCK) {
      return false;
    }
    ThrowIfNotOk("failed to open a WebTransport session", status);
    return true;
  }

  PyWebTransportStream* open_bidirectional_stream() {
    return OpenStream(false);
  }
  PyWebTransportStream* open_unidirectional_stream() {
    return OpenStream(true);
  }
  PyWebTransportStream* accept_bidirectional_stream() {
    return AcceptStream(false);
  }
  PyWebTransportStream* accept_unidirectional_stream() {
    return AcceptStream(true);
  }

  bool send_datagram(const nb::bytes& data) {
    std::string payload = BytesToString(data);
    quiche_py_status status = api_.wt_client_send_datagram(
        handle_, reinterpret_cast<const uint8_t*>(payload.data()),
        payload.size());
    if (status == QUICHE_PY_STATUS_WOULD_BLOCK) {
      return false;
    }
    ThrowIfNotOk("failed to send datagram", status);
    return true;
  }

  nb::object receive_datagram() {
    size_t datagram_size = 0;
    quiche_py_status status =
        api_.wt_client_peek_datagram_size(handle_, &datagram_size);
    if (status == QUICHE_PY_STATUS_WOULD_BLOCK) {
      return nb::none();
    }
    ThrowIfNotOk("failed to receive datagram", status);
    std::string buffer(datagram_size, '\0');
    size_t bytes_read = 0;
    status = api_.wt_client_receive_datagram(
        handle_,
        buffer.empty() ? nullptr : reinterpret_cast<uint8_t*>(buffer.data()),
        buffer.size(), &bytes_read);
    ThrowIfNotOk("failed to receive datagram", status);
    return BytesFromBuffer(buffer.data(), bytes_read);
  }

  size_t max_datagram_size() const {
    return api_.wt_client_max_datagram_size(handle_);
  }

  quiche_py_wt_client* handle() const { return handle_; }
  const quiche_py_api& api() const { return api_; }

  void ThrowIfNotOk(const std::string& context,
                    quiche_py_status status) const {
    if (status != QUICHE_PY_STATUS_OK) {
      ThrowStatus(context, status);
    }
  }

  [[noreturn]] void ThrowStatus(const std::string& context,
                                quiche_py_status status) const {
    std::string message = context;
    std::string detail = last_error();
    if (!detail.empty()) {
      message += ": " + detail;
    }
    switch (status) {
      case QUICHE_PY_STATUS_INVALID_ARGUMENT:
      case QUICHE_PY_STATUS_TOO_BIG:
        RaisePythonException(PyExc_ValueError, message);
      case QUICHE_PY_STATUS_NOT_FOUND:
        RaisePythonException(PyExc_KeyError, message);
      case QUICHE_PY_STATUS_CLOSED:
        RaisePythonException(PyExc_ConnectionError, message);
      case QUICHE_PY_STATUS_ERROR:
        RaisePythonException(PyExc_RuntimeError, message);
      case QUICHE_PY_STATUS_WOULD_BLOCK:
      case QUICHE_PY_STATUS_OK:
        break;
    }
    throw std::runtime_error(message);
  }

 private:
  PyWebTransportStream* OpenStream(bool unidirectional) {
    uint32_t stream_id = 0;
    quiche_py_status status =
        api_.wt_client_open_stream(handle_, unidirectional, &stream_id);
    if (status == QUICHE_PY_STATUS_WOULD_BLOCK) {
      return nullptr;
    }
    ThrowIfNotOk("failed to open a WebTransport stream", status);
    return new PyWebTransportStream(this, stream_id, unidirectional);
  }

  PyWebTransportStream* AcceptStream(bool unidirectional) {
    uint32_t stream_id = 0;
    quiche_py_status status =
        api_.wt_client_accept_stream(handle_, unidirectional, &stream_id);
    if (status == QUICHE_PY_STATUS_WOULD_BLOCK) {
      return nullptr;
    }
    ThrowIfNotOk("failed to accept a WebTransport stream", status);
    return new PyWebTransportStream(this, stream_id, unidirectional);
  }

  const quiche_py_api& api_;
  quiche_py_wt_client* handle_ = nullptr;
};

nb::object PyWebTransportStream::read(size_t max_bytes) {
  if (max_bytes == 0) {
    throw nb::value_error("max_bytes must be greater than zero");
  }
  if (max_bytes > kMaxInputBytes) {
    throw nb::value_error("max_bytes exceed the maximum allowed size");
  }
  std::string buffer(max_bytes, '\0');
  size_t bytes_read = 0;
  bool fin = false;
  quiche_py_status status = client_->api().wt_client_read_stream(
      client_->handle(), stream_id_, reinterpret_cast<uint8_t*>(buffer.data()),
      buffer.size(), &bytes_read, &fin);
  if (status == QUICHE_PY_STATUS_WOULD_BLOCK) {
    return nb::none();
  }
  client_->ThrowIfNotOk("failed to read from a WebTransport stream", status);
  return nb::make_tuple(BytesFromBuffer(buffer.data(), bytes_read), fin);
}

bool PyWebTransportStream::write(const nb::bytes& data, bool fin) {
  std::string payload = BytesToString(data);
  quiche_py_status status = client_->api().wt_client_write_stream(
      client_->handle(), stream_id_,
      reinterpret_cast<const uint8_t*>(payload.data()), payload.size(), fin);
  if (status == QUICHE_PY_STATUS_WOULD_BLOCK) {
    return false;
  }
  client_->ThrowIfNotOk("failed to write to a WebTransport stream", status);
  return true;
}

bool PyWebTransportStream::atomic_write(const nb::bytes& data, bool fin) {
  std::string payload = BytesToString(data);
  quiche_py_status status = client_->api().wt_client_write_stream_atomic(
      client_->handle(), stream_id_,
      reinterpret_cast<const uint8_t*>(payload.data()), payload.size(), fin);
  if (status == QUICHE_PY_STATUS_WOULD_BLOCK) {
    return false;
  }
  client_->ThrowIfNotOk("failed to atomically write to a WebTransport stream",
                        status);
  return true;
}

void PyWebTransportStream::set_priority(uint32_t send_group_id,
                                        uint32_t send_order) {
  quiche_py_status status = client_->api().wt_client_stream_set_priority(
      client_->handle(), stream_id_, send_group_id, send_order);
  client_->ThrowIfNotOk("failed to set the WebTransport stream priority",
                        status);
}

void PyWebTransportStream::reset(uint32_t error_code) {
  client_->ThrowIfNotOk(
      "failed to reset a WebTransport stream",
      client_->api().wt_client_reset_stream(client_->handle(), stream_id_,
                                            error_code));
}

void PyWebTransportStream::stop_sending(uint32_t error_code) {
  client_->ThrowIfNotOk(
      "failed to stop sending on a WebTransport stream",
      client_->api().wt_client_stop_sending(client_->handle(), stream_id_,
                                            error_code));
}

bool PyWebTransportStream::can_write() const {
  return client_->api().wt_client_stream_can_write(client_->handle(),
                                                   stream_id_);
}

bool PyWebTransportStream::is_open() const {
  return client_->api().wt_client_stream_exists(client_->handle(), stream_id_);
}

}  // namespace


const char* MoqtEventKindName(int kind) {
  switch (kind) {
    case QUICHE_PY_MOQT_EVENT_SUBSCRIBE_OK:
      return "subscribe_ok";
    case QUICHE_PY_MOQT_EVENT_SUBSCRIBE_ERROR:
      return "subscribe_error";
    case QUICHE_PY_MOQT_EVENT_OBJECT:
      return "object";
    case QUICHE_PY_MOQT_EVENT_PUBLISH_DONE:
      return "publish_done";
    case QUICHE_PY_MOQT_EVENT_MALFORMED_TRACK:
      return "malformed_track";
    case QUICHE_PY_MOQT_EVENT_STREAM_FIN:
      return "stream_fin";
    case QUICHE_PY_MOQT_EVENT_STREAM_RESET:
      return "stream_reset";
    case QUICHE_PY_MOQT_EVENT_SESSION_TERMINATED:
      return "session_terminated";
    case QUICHE_PY_MOQT_EVENT_GOAWAY:
      return "goaway";
    case QUICHE_PY_MOQT_EVENT_PUBLISH_OK:
      return "publish_ok";
    case QUICHE_PY_MOQT_EVENT_PUBLISH_ERROR:
      return "publish_error";
    case QUICHE_PY_MOQT_EVENT_FETCH_OK:
      return "fetch_ok";
    case QUICHE_PY_MOQT_EVENT_FETCH_ERROR:
      return "fetch_error";
    case QUICHE_PY_MOQT_EVENT_FETCH_DONE:
      return "fetch_done";
    case QUICHE_PY_MOQT_EVENT_PUBLISH_NAMESPACE_OK:
      return "publish_namespace_ok";
    case QUICHE_PY_MOQT_EVENT_PUBLISH_NAMESPACE_ERROR:
      return "publish_namespace_error";
    case QUICHE_PY_MOQT_EVENT_PUBLISH_NAMESPACE_CANCELLED:
      return "publish_namespace_cancelled";
    case QUICHE_PY_MOQT_EVENT_SUBSCRIBE_NAMESPACE_OK:
      return "subscribe_namespace_ok";
    case QUICHE_PY_MOQT_EVENT_SUBSCRIBE_NAMESPACE_ERROR:
      return "subscribe_namespace_error";
    case QUICHE_PY_MOQT_EVENT_NAMESPACE_ADDED:
      return "namespace_added";
    case QUICHE_PY_MOQT_EVENT_NAMESPACE_DONE:
      return "namespace_done";
    case QUICHE_PY_MOQT_EVENT_TRACK_STATUS_OK:
      return "track_status_ok";
    case QUICHE_PY_MOQT_EVENT_TRACK_STATUS_ERROR:
      return "track_status_error";
    default:
      return "unknown";
  }
}

class PyMoqtClient {
 public:
  PyMoqtClient(const std::string& host, uint16_t port,
               const std::string& server_name, const std::string& path,
               bool verify_peer)
      : api_(Api()) {
    quiche_py_moqt_client_options options{};
    options.host = host.c_str();
    options.port = port;
    options.server_name = server_name.empty() ? nullptr : server_name.c_str();
    options.path = path.empty() ? nullptr : path.c_str();
    options.verify_peer = verify_peer;
    handle_ = api_.moqt_client_create(&options);
    if (handle_ == nullptr) {
      throw std::runtime_error("failed to allocate MOQT client handle");
    }
    std::string err = last_error();
    if (!err.empty()) {
      api_.moqt_client_destroy(handle_);
      handle_ = nullptr;
      throw std::runtime_error("failed to create MOQT client: " + err);
    }
  }

  ~PyMoqtClient() {
    if (handle_ != nullptr) {
      api_.moqt_client_destroy(handle_);
    }
  }

  void start() {
    ThrowIfNotOk("failed to start the MOQT handshake",
                 api_.moqt_client_start(handle_));
  }

  bool connect() {
    quiche_py_status status = api_.moqt_client_connect(handle_);
    if (status == QUICHE_PY_STATUS_WOULD_BLOCK) {
      return false;
    }
    ThrowIfNotOk("failed to establish the MOQT session", status);
    return true;
  }

  void receive_packet(const nb::bytes& data) {
    std::string payload = BytesToString(data);
    quiche_py_status status = api_.moqt_client_receive_packet(
        handle_, reinterpret_cast<const uint8_t*>(payload.data()),
        payload.size());
    if (status == QUICHE_PY_STATUS_CLOSED) {
      return;
    }
    ThrowIfNotOk("failed to process an incoming packet", status);
  }

  void receive_packets(const nb::list& packets) {
    std::vector<std::string> payloads;
    payloads.reserve(packets.size());
    for (nb::handle item : packets) {
      nb::bytes bytes = nb::cast<nb::bytes>(item);
      payloads.push_back(BytesToString(bytes));
    }
    std::vector<const uint8_t*> data;
    std::vector<size_t> data_lengths;
    data.reserve(payloads.size());
    data_lengths.reserve(payloads.size());
    for (const std::string& payload : payloads) {
      data.push_back(reinterpret_cast<const uint8_t*>(payload.data()));
      data_lengths.push_back(payload.size());
    }
    quiche_py_status status = api_.moqt_client_receive_packets(
        handle_, data.data(), data_lengths.data(), data.size());
    if (status == QUICHE_PY_STATUS_CLOSED) {
      return;
    }
    ThrowIfNotOk("failed to process incoming packets", status);
  }

  nb::object next_send() {
    std::string buffer(kSendBufferSize, '\0');
    size_t written = 0;
    quiche_py_status status = api_.moqt_client_next_send(
        handle_, reinterpret_cast<uint8_t*>(buffer.data()), buffer.size(),
        &written);
    if (status == QUICHE_PY_STATUS_WOULD_BLOCK) {
      return nb::none();
    }
    ThrowIfNotOk("failed to fetch an outgoing MOQT packet", status);
    return BytesFromBuffer(buffer.data(), written);
  }

  nb::object next_timeout_ms() {
    uint64_t timeout_ms = 0;
    bool has_timeout = false;
    ThrowIfNotOk("failed to query the next MOQT timeout",
                 api_.moqt_client_next_timeout_ms(handle_, &timeout_ms,
                                                   &has_timeout));
    if (!has_timeout) {
      return nb::none();
    }
    return nb::cast(timeout_ms);
  }

  void handle_timeout() {
    quiche_py_status status = api_.moqt_client_handle_timeout(handle_);
    if (status == QUICHE_PY_STATUS_CLOSED) {
      return;
    }
    ThrowIfNotOk("failed to handle a MOQT timeout", status);
  }

  void close(uint32_t error_code = 0, const std::string& reason = "") {
    api_.moqt_client_close(handle_, error_code, reason.c_str());
  }

  bool is_connected() const {
    return api_.moqt_client_is_connected(handle_);
  }

  bool is_session_ready() const {
    return api_.moqt_client_is_session_ready(handle_);
  }

  bool is_terminated() const {
    return api_.moqt_client_is_terminated(handle_);
  }

  bool subscribe(const std::string& track_name) {
    quiche_py_status status =
        api_.moqt_client_subscribe(handle_, track_name.c_str());
    if (status == QUICHE_PY_STATUS_WOULD_BLOCK) {
      return false;
    }
    ThrowIfNotOk("failed to subscribe to a MOQT track", status);
    return true;
  }

  void unsubscribe(const std::string& track_name) {
    ThrowIfNotOk("failed to unsubscribe from a MOQT track",
                 api_.moqt_client_unsubscribe(handle_, track_name.c_str()));
  }

  bool publish(const std::string& track_name) {
    quiche_py_status status =
        api_.moqt_client_publish(handle_, track_name.c_str());
    if (status == QUICHE_PY_STATUS_WOULD_BLOCK) {
      return false;
    }
    ThrowIfNotOk("failed to publish a MOQT track", status);
    return true;
  }

  void add_object(const std::string& track_name, uint64_t group_id,
                  uint64_t object_id, const nb::bytes& payload,
                  bool fin = true) {
    std::string data = BytesToString(payload);
    ThrowIfNotOk(
        "failed to add a MOQT object",
        api_.moqt_client_add_object(
            handle_, track_name.c_str(), group_id, object_id,
            reinterpret_cast<const uint8_t*>(data.data()), data.size(), fin));
  }

  void ack_object(const std::string& track_name, uint64_t group_id,
                  uint64_t object_id) {
    ThrowIfNotOk("failed to acknowledge a MOQT object",
                 api_.moqt_client_ack_object(handle_, track_name.c_str(),
                                             group_id, object_id));
  }

  uint64_t fetch(const std::string& track_name, uint64_t start_group,
                 uint64_t start_object, uint64_t end_group,
                 const nb::object& end_object) {
    uint64_t request_id = 0;
    bool has_end_object = !end_object.is_none();
    uint64_t end_object_value =
        has_end_object ? nb::cast<uint64_t>(end_object) : 0;
    ThrowIfNotOk(
        "failed to send a MOQT FETCH",
        api_.moqt_client_fetch(handle_, track_name.c_str(), start_group,
                               start_object, end_group, has_end_object,
                               end_object_value, &request_id));
    return request_id;
  }

  bool publish_namespace(const std::string& track_namespace) {
    quiche_py_status status = api_.moqt_client_publish_namespace(
        handle_, track_namespace.c_str());
    if (status == QUICHE_PY_STATUS_WOULD_BLOCK) {
      return false;
    }
    ThrowIfNotOk("failed to publish a MOQT namespace", status);
    return true;
  }

  void publish_namespace_done(const std::string& track_namespace) {
    ThrowIfNotOk("failed to send PUBLISH_NAMESPACE_DONE",
                 api_.moqt_client_publish_namespace_done(
                     handle_, track_namespace.c_str()));
  }

  bool subscribe_namespace(const std::string& prefix) {
    quiche_py_status status = api_.moqt_client_subscribe_namespace(
        handle_, prefix.c_str());
    if (status == QUICHE_PY_STATUS_WOULD_BLOCK) {
      return false;
    }
    ThrowIfNotOk("failed to subscribe to a MOQT namespace", status);
    return true;
  }

  void unsubscribe_namespace(const std::string& prefix) {
    ThrowIfNotOk("failed to unsubscribe from a MOQT namespace",
                 api_.moqt_client_unsubscribe_namespace(handle_,
                                                        prefix.c_str()));
  }

  bool track_status(const std::string& track_name) {
    quiche_py_status status =
        api_.moqt_client_track_status(handle_, track_name.c_str());
    if (status == QUICHE_PY_STATUS_WOULD_BLOCK) {
      return false;
    }
    ThrowIfNotOk("failed to request MOQT track status", status);
    return true;
  }

  nb::object poll_event() {
    quiche_py_moqt_event event{};
    if (!api_.moqt_client_poll_event(handle_, &event)) {
      return nb::none();
    }
    nb::dict result;
    result["kind"] = MoqtEventKindName(event.kind);
    result["request_id"] = event.request_id;
    result["track_name"] =
        std::string(event.track_name, event.track_name_len);
    result["group_id"] = event.group_id;
    result["object_id"] = event.object_id;
    result["subgroup_id"] =
        event.has_subgroup ? nb::cast(event.subgroup_id) : nb::none();
    result["offset"] = event.offset;
    result["fin"] = event.fin;
    result["payload"] =
        event.payload == nullptr
            ? nb::bytes("")
            : BytesFromBuffer(reinterpret_cast<const char*>(event.payload),
                              event.payload_len);
    result["publisher_priority"] = event.publisher_priority;
    result["status"] = event.status;
    result["error_code"] = event.error_code;
    result["reason"] = std::string(event.reason, event.reason_len);
    result["new_session_uri"] =
        std::string(event.new_session_uri, event.new_session_uri_len);
    result["timeout_ms"] = event.timeout_ms;
    return result;
  }

  std::string last_error() const {
    const char* message = api_.moqt_client_last_error(handle_);
    return message == nullptr ? std::string() : std::string(message);
  }

  quiche_py_moqt_client* handle() const { return handle_; }
  const quiche_py_api& api() const { return api_; }

  void ThrowIfNotOk(const std::string& context,
                    quiche_py_status status) const {
    if (status != QUICHE_PY_STATUS_OK) {
      ThrowStatus(context, status);
    }
  }

  [[noreturn]] void ThrowStatus(const std::string& context,
                                quiche_py_status status) const {
    std::string message = context;
    std::string detail = last_error();
    if (!detail.empty()) {
      message += ": " + detail;
    }
    switch (status) {
      case QUICHE_PY_STATUS_INVALID_ARGUMENT:
      case QUICHE_PY_STATUS_TOO_BIG:
        RaisePythonException(PyExc_ValueError, message);
      case QUICHE_PY_STATUS_NOT_FOUND:
        RaisePythonException(PyExc_KeyError, message);
      case QUICHE_PY_STATUS_CLOSED:
        RaisePythonException(PyExc_ConnectionError, message);
      case QUICHE_PY_STATUS_ERROR:
        RaisePythonException(PyExc_RuntimeError, message);
      case QUICHE_PY_STATUS_WOULD_BLOCK:
      case QUICHE_PY_STATUS_OK:
        break;
    }
    throw std::runtime_error(message);
  }

 private:
  const quiche_py_api& api_;
  quiche_py_moqt_client* handle_ = nullptr;
};

NB_MODULE(quiche_ext, m) {
  (void)Api();
  m.doc() =
      "Google QUICHE sans-I/O bindings for QUIC, HTTP/3, and WebTransport.";

  nb::class_<PyQuicStream>(m, "QuicStream")
      .def_prop_ro("stream_id", &PyQuicStream::stream_id,
                   "QUIC stream ID.", nb::lock_self())
      .def_prop_ro("unidirectional", &PyQuicStream::unidirectional,
                   "Whether the stream is unidirectional.", nb::lock_self())
      .def("read", &PyQuicStream::read, nb::lock_self(),
           nb::arg("max_bytes") = 65536,
           "Read from the stream. Returns None when no data is available, "
           "otherwise returns (data, fin).")
      .def("write", &PyQuicStream::write, nb::lock_self(), nb::arg("data"),
           nb::arg("fin") = false,
           "Write bytes to the stream. Returns False when the stream is "
           "currently write-blocked.")
      .def("atomic_write", &PyQuicStream::atomic_write, nb::lock_self(),
           nb::arg("data"), nb::arg("fin") = false,
           "Atomically write bytes to the stream. Returns False when flow "
           "control credit is insufficient.")
      .def("set_priority", &PyQuicStream::set_priority, nb::lock_self(),
           nb::arg("send_group_id"), nb::arg("send_order"),
           "Set the WebTransport send group and send order of the stream.")
      .def("reset", &PyQuicStream::reset, nb::lock_self(),
           nb::arg("error_code") = 0,
           "Reset the stream with an application-defined error code.")
      .def("stop_sending", &PyQuicStream::stop_sending, nb::lock_self(),
           nb::arg("error_code") = 0,
           "Request that the peer stop sending on the stream.")
      .def("can_write", &PyQuicStream::can_write, nb::lock_self(),
           "Whether the stream can currently accept writes.")
      .def("is_open", &PyQuicStream::is_open, nb::lock_self(),
           "Whether the stream is still known to the session.");

  nb::class_<PyQuicClient>(m, "QuicClient")
      .def(nb::init<const std::string&, uint16_t, const std::string&,
                    const std::string&, bool>(),
           nb::arg("host"), nb::arg("port"), nb::arg("server_name") = "",
           nb::arg("alpn"), nb::arg("verify_peer") = true,
           "Create a sans-I/O client-side QUIC connection handle. "
           "alpn is required; there is no generic raw-QUIC ALPN.")
      .def("start", &PyQuicClient::start, nb::lock_self(),
           "Start the QUIC handshake. Outgoing packets are produced via "
           "next_send().")
      .def("receive_packet", &PyQuicClient::receive_packet, nb::lock_self(),
           nb::arg("data"),
           "Feed an inbound UDP datagram into the QUIC state machine.")
      .def("receive_packets", &PyQuicClient::receive_packets, nb::lock_self(),
           nb::arg("packets"),
           "Feed a batch of inbound UDP datagrams into the QUIC state machine.")
      .def("configure", &PyQuicClient::configure, nb::lock_self(),
           nb::kw_only(),
           nb::arg("max_idle_timeout_ms") = nb::none(),
           nb::arg("initial_max_data") = nb::none(),
           nb::arg("initial_max_stream_data_bidi_local") = nb::none(),
           nb::arg("initial_max_stream_data_bidi_remote") = nb::none(),
           nb::arg("initial_max_stream_data_uni") = nb::none(),
           nb::arg("max_ack_delay_ms") = nb::none(),
           nb::arg("min_ack_delay_us") = nb::none(),
           nb::arg("active_connection_id_limit") = nb::none(),
           nb::arg("max_datagram_frame_size") = nb::none(),
           "Set QUIC transport parameters before starting the handshake.")
      .def("next_send", &PyQuicClient::next_send, nb::lock_self(),
           "Return the next outgoing UDP datagram, or None when the send queue "
           "is empty.")
      .def("next_timeout_ms", &PyQuicClient::next_timeout_ms, nb::lock_self(),
           "Return milliseconds until the next timer fires, or None when no "
           "timer is scheduled.")
      .def("handle_timeout", &PyQuicClient::handle_timeout, nb::lock_self(),
           "Fire any expired timers. New outgoing packets may be queued.")
      .def("close", &PyQuicClient::close, nb::lock_self(),
           nb::arg("error_code") = 0, nb::arg("reason") = "",
           "Close the QUIC session with an application-defined error code.")
      .def("open_bidirectional_stream",
           &PyQuicClient::open_bidirectional_stream, nb::lock_self(),
           nb::rv_policy::take_ownership, nb::keep_alive<0, 1>(),
           "Open an outgoing bidirectional stream. Returns None when no slot "
           "is currently available.")
      .def("open_unidirectional_stream",
           &PyQuicClient::open_unidirectional_stream, nb::lock_self(),
           nb::rv_policy::take_ownership, nb::keep_alive<0, 1>(),
           "Open an outgoing unidirectional stream. Returns None when no slot "
           "is currently available.")
      .def("accept_bidirectional_stream",
           &PyQuicClient::accept_bidirectional_stream, nb::lock_self(),
           nb::rv_policy::take_ownership, nb::keep_alive<0, 1>(),
           "Accept an incoming bidirectional stream. Returns None when none is "
           "available.")
      .def("accept_unidirectional_stream",
           &PyQuicClient::accept_unidirectional_stream, nb::lock_self(),
           nb::rv_policy::take_ownership, nb::keep_alive<0, 1>(),
           "Accept an incoming unidirectional stream. Returns None when none "
           "is available.")
      .def("send_datagram", &PyQuicClient::send_datagram, nb::lock_self(),
           nb::arg("data"),
           "Send a QUIC datagram. Returns False when datagram sending is "
           "currently blocked.")
      .def("receive_datagram", &PyQuicClient::receive_datagram,
           nb::lock_self(),
           "Receive the next QUIC datagram. Returns None when no datagram is "
           "available.")
      .def("is_connected", &PyQuicClient::is_connected, nb::lock_self(),
           "Whether the QUIC connection is established and ready to use.")
      .def("stats", &PyQuicClient::stats, nb::lock_self(),
           "Return QUIC connection statistics, or None before the session is "
           "created.")
      .def("socket_packets_dropped", &PyQuicClient::socket_packets_dropped,
           nb::lock_self(),
           "Return socket buffer overflow drops, or None when unavailable.")
      .def("max_datagram_size", &PyQuicClient::max_datagram_size,
           nb::lock_self(),
           "Return the currently negotiated maximum QUIC datagram payload size.")
      .def_prop_ro("last_error", &PyQuicClient::last_error,
                   "The most recent transport error string, if any.",
                   nb::lock_self());

  nb::class_<PyHttp3Response>(m, "Http3Response")
      .def_prop_ro("stream_id", &PyHttp3Response::stream_id, nb::lock_self())
      .def_prop_ro("status_code", &PyHttp3Response::status_code, nb::lock_self())
      .def_prop_ro("headers", &PyHttp3Response::headers, nb::lock_self())
      .def_prop_ro("body", &PyHttp3Response::body, nb::lock_self());

  nb::class_<PyHttp3Client>(m, "Http3Client")
      .def(nb::init<const std::string&, uint16_t, const std::string&, bool>(),
           nb::arg("host"), nb::arg("port"), nb::arg("server_name") = "",
           nb::arg("verify_peer") = true,
           "Create a sans-I/O HTTP/3 client (ALPN h3).")
      .def("start", &PyHttp3Client::start, nb::lock_self())
      .def("receive_packet", &PyHttp3Client::receive_packet, nb::lock_self(),
           nb::arg("data"))
      .def("receive_packets", &PyHttp3Client::receive_packets, nb::lock_self(),
           nb::arg("packets"))
      .def("configure", &PyHttp3Client::configure, nb::lock_self(),
           nb::kw_only(),
           nb::arg("max_idle_timeout_ms") = nb::none(),
           nb::arg("initial_max_data") = nb::none(),
           nb::arg("initial_max_stream_data_bidi_local") = nb::none(),
           nb::arg("initial_max_stream_data_bidi_remote") = nb::none(),
           nb::arg("initial_max_stream_data_uni") = nb::none(),
           nb::arg("max_ack_delay_ms") = nb::none(),
           nb::arg("min_ack_delay_us") = nb::none(),
           nb::arg("active_connection_id_limit") = nb::none(),
           nb::arg("max_datagram_frame_size") = nb::none(),
           "Set QUIC transport parameters before starting the handshake.")
      .def("next_send", &PyHttp3Client::next_send, nb::lock_self())
      .def("next_timeout_ms", &PyHttp3Client::next_timeout_ms, nb::lock_self())
      .def("handle_timeout", &PyHttp3Client::handle_timeout, nb::lock_self())
      .def("close", &PyHttp3Client::close, nb::lock_self(),
           nb::arg("error_code") = 0, nb::arg("reason") = "")
      .def("is_connected", &PyHttp3Client::is_connected, nb::lock_self())
      .def("stats", &PyHttp3Client::stats, nb::lock_self())
      .def("socket_packets_dropped", &PyHttp3Client::socket_packets_dropped,
           nb::lock_self())
      .def("settings_received", &PyHttp3Client::settings_received,
           nb::lock_self())
      .def("settings", &PyHttp3Client::settings, nb::lock_self())
      .def("settings_wire_order", &PyHttp3Client::settings_wire_order,
           nb::lock_self())
      .def("submit_request", &PyHttp3Client::submit_request, nb::lock_self(),
           nb::arg("headers"), nb::arg("body") = nb::bytes(""),
           nb::arg("fin") = true,
           "Submit an HTTP/3 request. Returns stream_id or None when blocked.")
      .def("take_response", &PyHttp3Client::take_response, nb::lock_self(),
           nb::arg("stream_id"),
           "Take a completed response. Returns None when not ready.")
      .def_prop_ro("last_error", &PyHttp3Client::last_error, nb::lock_self());

  nb::class_<PyWebTransportStream>(m, "WebTransportStream")
      .def_prop_ro("stream_id", &PyWebTransportStream::stream_id, nb::lock_self())
      .def_prop_ro("unidirectional", &PyWebTransportStream::unidirectional,
                   nb::lock_self())
      .def("read", &PyWebTransportStream::read, nb::lock_self(),
           nb::arg("max_bytes") = 65536)
      .def("write", &PyWebTransportStream::write, nb::lock_self(),
           nb::arg("data"), nb::arg("fin") = false)
      .def("atomic_write", &PyWebTransportStream::atomic_write,
           nb::lock_self(), nb::arg("data"), nb::arg("fin") = false)
      .def("set_priority", &PyWebTransportStream::set_priority,
           nb::lock_self(), nb::arg("send_group_id"), nb::arg("send_order"))
      .def("reset", &PyWebTransportStream::reset, nb::lock_self(),
           nb::arg("error_code") = 0)
      .def("stop_sending", &PyWebTransportStream::stop_sending, nb::lock_self(),
           nb::arg("error_code") = 0)
      .def("can_write", &PyWebTransportStream::can_write, nb::lock_self())
      .def("is_open", &PyWebTransportStream::is_open, nb::lock_self());

  nb::class_<PyWebTransportClient>(m, "WebTransportClient")
      .def(nb::init<const std::string&, uint16_t, const std::string&, bool>(),
           nb::arg("host"), nb::arg("port"), nb::arg("server_name") = "",
           nb::arg("verify_peer") = true,
           "Create a sans-I/O WebTransport-over-HTTP/3 client.")
      .def("start", &PyWebTransportClient::start, nb::lock_self())
      .def("receive_packet", &PyWebTransportClient::receive_packet,
           nb::lock_self(), nb::arg("data"))
      .def("receive_packets", &PyWebTransportClient::receive_packets,
           nb::lock_self(), nb::arg("packets"))
      .def("configure", &PyWebTransportClient::configure, nb::lock_self(),
           nb::kw_only(),
           nb::arg("max_idle_timeout_ms") = nb::none(),
           nb::arg("initial_max_data") = nb::none(),
           nb::arg("initial_max_stream_data_bidi_local") = nb::none(),
           nb::arg("initial_max_stream_data_bidi_remote") = nb::none(),
           nb::arg("initial_max_stream_data_uni") = nb::none(),
           nb::arg("max_ack_delay_ms") = nb::none(),
           nb::arg("min_ack_delay_us") = nb::none(),
           nb::arg("active_connection_id_limit") = nb::none(),
           nb::arg("max_datagram_frame_size") = nb::none(),
           "Set QUIC transport parameters before starting the handshake.")
      .def("next_send", &PyWebTransportClient::next_send, nb::lock_self())
      .def("next_timeout_ms", &PyWebTransportClient::next_timeout_ms,
           nb::lock_self())
      .def("handle_timeout", &PyWebTransportClient::handle_timeout,
           nb::lock_self())
      .def("close", &PyWebTransportClient::close, nb::lock_self(),
           nb::arg("error_code") = 0, nb::arg("reason") = "")
      .def("is_connected", &PyWebTransportClient::is_connected, nb::lock_self())
      .def("is_session_ready", &PyWebTransportClient::is_session_ready,
           nb::lock_self())
      .def("stats", &PyWebTransportClient::stats, nb::lock_self())
      .def("socket_packets_dropped",
           &PyWebTransportClient::socket_packets_dropped, nb::lock_self())
      .def("session_stats", &PyWebTransportClient::session_stats,
           nb::lock_self())
      .def("datagram_stats", &PyWebTransportClient::datagram_stats,
           nb::lock_self())
      .def("is_draining", &PyWebTransportClient::is_draining, nb::lock_self())
      .def("notify_draining", &PyWebTransportClient::notify_draining,
           nb::lock_self())
      .def_prop_ro("protocol", &PyWebTransportClient::protocol,
                   nb::lock_self())
      .def("connect_session", &PyWebTransportClient::connect_session,
           nb::lock_self(), nb::arg("path"), nb::arg("headers") = nb::none(),
           "Send CONNECT webtransport. Returns False when blocked.")
      .def("open_bidirectional_stream",
           &PyWebTransportClient::open_bidirectional_stream, nb::lock_self(),
           nb::rv_policy::take_ownership, nb::keep_alive<0, 1>())
      .def("open_unidirectional_stream",
           &PyWebTransportClient::open_unidirectional_stream, nb::lock_self(),
           nb::rv_policy::take_ownership, nb::keep_alive<0, 1>())
      .def("accept_bidirectional_stream",
           &PyWebTransportClient::accept_bidirectional_stream, nb::lock_self(),
           nb::rv_policy::take_ownership, nb::keep_alive<0, 1>())
      .def("accept_unidirectional_stream",
           &PyWebTransportClient::accept_unidirectional_stream, nb::lock_self(),
           nb::rv_policy::take_ownership, nb::keep_alive<0, 1>())
      .def("send_datagram", &PyWebTransportClient::send_datagram,
           nb::lock_self(), nb::arg("data"))
      .def("receive_datagram", &PyWebTransportClient::receive_datagram,
           nb::lock_self())
      .def("max_datagram_size", &PyWebTransportClient::max_datagram_size,
           nb::lock_self())
      .def_prop_ro("last_error", &PyWebTransportClient::last_error,
                   nb::lock_self());

  nb::class_<quiche_py_connection_stats>(m, "QuicStats")
      .def_ro("bytes_sent", &quiche_py_connection_stats::bytes_sent)
      .def_ro("packets_sent", &quiche_py_connection_stats::packets_sent)
      .def_ro("packets_sent_by_dispatcher",
              &quiche_py_connection_stats::packets_sent_by_dispatcher)
      .def_ro("packets_discarded",
              &quiche_py_connection_stats::packets_discarded)
      .def_ro("stream_bytes_sent",
              &quiche_py_connection_stats::stream_bytes_sent)
      .def_ro("bytes_received", &quiche_py_connection_stats::bytes_received)
      .def_ro("packets_received",
              &quiche_py_connection_stats::packets_received)
      .def_ro("packets_processed",
              &quiche_py_connection_stats::packets_processed)
      .def_ro("stream_bytes_received",
              &quiche_py_connection_stats::stream_bytes_received)
      .def_ro("bytes_retransmitted",
              &quiche_py_connection_stats::bytes_retransmitted)
      .def_ro("packets_retransmitted",
              &quiche_py_connection_stats::packets_retransmitted)
      .def_ro("bytes_spuriously_retransmitted",
              &quiche_py_connection_stats::bytes_spuriously_retransmitted)
      .def_ro("packets_spuriously_retransmitted",
              &quiche_py_connection_stats::packets_spuriously_retransmitted)
      .def_ro("packets_lost", &quiche_py_connection_stats::packets_lost)
      .def_ro("packets_spuriously_detected_lost",
              &quiche_py_connection_stats::packets_spuriously_detected_lost)
      .def_ro("packets_dropped",
              &quiche_py_connection_stats::packets_dropped)
      .def_ro("min_rtt_us", &quiche_py_connection_stats::min_rtt_us)
      .def_ro("srtt_us", &quiche_py_connection_stats::srtt_us)
      .def_ro("cwnd_bootstrapping_rtt_us",
              &quiche_py_connection_stats::cwnd_bootstrapping_rtt_us)
      .def_ro("egress_mtu", &quiche_py_connection_stats::egress_mtu)
      .def_ro("max_egress_mtu", &quiche_py_connection_stats::max_egress_mtu)
      .def_ro("ingress_mtu", &quiche_py_connection_stats::ingress_mtu)
      .def_ro("estimated_bandwidth_bps",
              &quiche_py_connection_stats::estimated_bandwidth_bps)
      .def_ro("packets_reordered",
              &quiche_py_connection_stats::packets_reordered)
      .def_ro("max_sequence_reordering",
              &quiche_py_connection_stats::max_sequence_reordering)
      .def_ro("max_time_reordering_us",
              &quiche_py_connection_stats::max_time_reordering_us)
      .def_ro("num_coalesced_packets_received",
              &quiche_py_connection_stats::num_coalesced_packets_received)
      .def_ro("num_coalesced_packets_processed",
              &quiche_py_connection_stats::num_coalesced_packets_processed)
      .def_ro("num_ack_aggregation_epochs",
              &quiche_py_connection_stats::num_ack_aggregation_epochs)
      .def_ro("num_failed_authentication_packets_received",
              &quiche_py_connection_stats::num_failed_authentication_packets_received)
      .def_ro("num_tls_server_zero_rtt_packets_received_after_discarding_decrypter",
              &quiche_py_connection_stats::num_tls_server_zero_rtt_packets_received_after_discarding_decrypter)
      .def_ro("num_ecn_marks_ect0",
              &quiche_py_connection_stats::num_ecn_marks_ect0)
      .def_ro("num_ecn_marks_ect1",
              &quiche_py_connection_stats::num_ecn_marks_ect1)
      .def_ro("num_ecn_marks_ce",
              &quiche_py_connection_stats::num_ecn_marks_ce)
      .def_ro("num_ack_frames_sent_with_ecn",
              &quiche_py_connection_stats::num_ack_frames_sent_with_ecn)
      .def_ro("application_data_acked",
              &quiche_py_connection_stats::application_data_acked)
      .def_ro("application_data_packets_acked",
              &quiche_py_connection_stats::application_data_packets_acked)
      .def_ro("application_data_packets_with_receive_timestamps_acked",
              &quiche_py_connection_stats::application_data_packets_with_receive_timestamps_acked)
      .def_ro("total_loss_detection_response_time",
              &quiche_py_connection_stats::total_loss_detection_response_time)
      .def_ro("slowstart_count",
              &quiche_py_connection_stats::slowstart_count)
      .def_ro("slowstart_num_rtts",
              &quiche_py_connection_stats::slowstart_num_rtts)
      .def_ro("slowstart_packets_sent",
              &quiche_py_connection_stats::slowstart_packets_sent)
      .def_ro("slowstart_bytes_sent",
              &quiche_py_connection_stats::slowstart_bytes_sent)
      .def_ro("slowstart_packets_lost",
              &quiche_py_connection_stats::slowstart_packets_lost)
      .def_ro("slowstart_bytes_lost",
              &quiche_py_connection_stats::slowstart_bytes_lost)
      .def_ro("bbr_num_cycles",
              &quiche_py_connection_stats::bbr_num_cycles)
      .def_ro("bbr_num_short_cycles_for_reno_coexistence",
              &quiche_py_connection_stats::bbr_num_short_cycles_for_reno_coexistence)
      .def_ro("crypto_retransmit_count",
              &quiche_py_connection_stats::crypto_retransmit_count)
      .def_ro("loss_timeout_count",
              &quiche_py_connection_stats::loss_timeout_count)
      .def_ro("pto_count", &quiche_py_connection_stats::pto_count)
      .def_ro("tcp_loss_events",
              &quiche_py_connection_stats::tcp_loss_events)
      .def_ro("blocked_frames_received",
              &quiche_py_connection_stats::blocked_frames_received)
      .def_ro("blocked_frames_sent",
              &quiche_py_connection_stats::blocked_frames_sent)
      .def_ro("num_connectivity_probing_received",
              &quiche_py_connection_stats::num_connectivity_probing_received)
      .def_ro("num_path_response_received",
              &quiche_py_connection_stats::num_path_response_received)
      .def_ro("max_consecutive_rto_with_forward_progress",
              &quiche_py_connection_stats::max_consecutive_rto_with_forward_progress)
      .def_ro("num_amplification_throttling",
              &quiche_py_connection_stats::num_amplification_throttling)
      .def_ro("key_update_count",
              &quiche_py_connection_stats::key_update_count)
      .def_ro("ping_frames_sent",
              &quiche_py_connection_stats::ping_frames_sent)
      .def_ro("num_new_connection_id_sent",
              &quiche_py_connection_stats::num_new_connection_id_sent)
      .def_ro("num_retire_connection_id_sent",
              &quiche_py_connection_stats::num_retire_connection_id_sent)
      .def_ro("num_peer_migration_to_proactively_validated_address",
              &quiche_py_connection_stats::num_peer_migration_to_proactively_validated_address)
      .def_ro("num_reverse_path_validation_upon_migration",
              &quiche_py_connection_stats::num_reverse_path_validation_upon_migration)
      .def_ro("num_validated_peer_migration",
              &quiche_py_connection_stats::num_validated_peer_migration)
      .def_ro("num_invalid_peer_migration",
              &quiche_py_connection_stats::num_invalid_peer_migration)
      .def_ro("num_peer_migration_while_validating_default_path",
              &quiche_py_connection_stats::num_peer_migration_while_validating_default_path)
      .def_ro("first_decrypted_packet",
              &quiche_py_connection_stats::first_decrypted_packet)
      .def_ro("connection_creation_time_us",
              &quiche_py_connection_stats::connection_creation_time_us)
      .def_ro("handshake_completion_time_us",
              &quiche_py_connection_stats::handshake_completion_time_us);

  nb::class_<quiche_py_wt_session_stats>(m, "WebTransportSessionStats")
      .def_ro("min_rtt_us", &quiche_py_wt_session_stats::min_rtt_us)
      .def_ro("smoothed_rtt_us",
              &quiche_py_wt_session_stats::smoothed_rtt_us)
      .def_ro("rtt_variation_us",
              &quiche_py_wt_session_stats::rtt_variation_us)
      .def_ro("application_bytes_acknowledged",
              &quiche_py_wt_session_stats::application_bytes_acknowledged)
      .def_ro("estimated_send_rate_bps",
              &quiche_py_wt_session_stats::estimated_send_rate_bps);

  nb::class_<quiche_py_wt_datagram_stats>(m, "WebTransportDatagramStats")
      .def_ro("expired_outgoing",
              &quiche_py_wt_datagram_stats::expired_outgoing)
      .def_ro("lost_outgoing",
              &quiche_py_wt_datagram_stats::lost_outgoing);

  nb::class_<PyMoqtClient>(m, "MoqtClient")
      .def(nb::init<const std::string&, uint16_t, const std::string&,
                    const std::string&, bool>(),
           nb::arg("host"), nb::arg("port"), nb::arg("server_name") = "",
           nb::arg("path") = "/", nb::arg("verify_peer") = true,
           "MOQT draft-18 のクライアントを WebTransport/HTTP3 上に生成する。")
      .def("start", &PyMoqtClient::start, nb::lock_self())
      .def("connect", &PyMoqtClient::connect, nb::lock_self(),
           "Send the MOQT CONNECT. Returns False when blocked.")
      .def("receive_packet", &PyMoqtClient::receive_packet, nb::lock_self(),
           nb::arg("data"))
      .def("receive_packets", &PyMoqtClient::receive_packets, nb::lock_self(),
           nb::arg("packets"))
      .def("next_send", &PyMoqtClient::next_send, nb::lock_self())
      .def("next_timeout_ms", &PyMoqtClient::next_timeout_ms, nb::lock_self())
      .def("handle_timeout", &PyMoqtClient::handle_timeout, nb::lock_self())
      .def("close", &PyMoqtClient::close, nb::lock_self(),
           nb::arg("error_code") = 0, nb::arg("reason") = "")
      .def("is_connected", &PyMoqtClient::is_connected, nb::lock_self())
      .def("is_session_ready", &PyMoqtClient::is_session_ready,
           nb::lock_self())
      .def("is_terminated", &PyMoqtClient::is_terminated, nb::lock_self())
      .def("subscribe", &PyMoqtClient::subscribe, nb::lock_self(),
           nb::arg("track_name"),
           "Send SUBSCRIBE. Returns False when the session is not ready.")
      .def("unsubscribe", &PyMoqtClient::unsubscribe, nb::lock_self(),
           nb::arg("track_name"))
      .def("publish", &PyMoqtClient::publish, nb::lock_self(),
           nb::arg("track_name"),
           "Send PUBLISH for a track. Returns False when blocked.")
      .def("add_object", &PyMoqtClient::add_object, nb::lock_self(),
           nb::arg("track_name"), nb::arg("group_id"), nb::arg("object_id"),
           nb::arg("payload"), nb::arg("fin") = true,
           "Add an object to a published track.")
      .def("ack_object", &PyMoqtClient::ack_object, nb::lock_self(),
           nb::arg("track_name"), nb::arg("group_id"), nb::arg("object_id"))
      .def("fetch", &PyMoqtClient::fetch, nb::lock_self(),
           nb::arg("track_name"), nb::arg("start_group"),
           nb::arg("start_object"), nb::arg("end_group"),
           nb::arg("end_object") = nb::none(),
           "Send FETCH and return a request ID.")
      .def("publish_namespace", &PyMoqtClient::publish_namespace,
           nb::lock_self(), nb::arg("track_namespace"))
      .def("publish_namespace_done", &PyMoqtClient::publish_namespace_done,
           nb::lock_self(), nb::arg("track_namespace"))
      .def("subscribe_namespace", &PyMoqtClient::subscribe_namespace,
           nb::lock_self(), nb::arg("prefix"))
      .def("unsubscribe_namespace", &PyMoqtClient::unsubscribe_namespace,
           nb::lock_self(), nb::arg("prefix"))
      .def("track_status", &PyMoqtClient::track_status, nb::lock_self(),
           nb::arg("track_name"))
      .def("poll_event", &PyMoqtClient::poll_event, nb::lock_self(),
           "次の MOQT イベントを取り出す。イベントが無い場合は None を返す。")
      .def_prop_ro("last_error", &PyMoqtClient::last_error,
                   nb::lock_self());

  m.def("version",
        [] { return std::string(Api().supported_versions_sample()); },
        "Sample QUIC version string from QUICHE (first entry of "
        "AllSupportedVersions).");
}
