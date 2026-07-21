#include "quiche_glue.h"

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>

#include <cstdint>
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
      .def("next_send", &PyHttp3Client::next_send, nb::lock_self())
      .def("next_timeout_ms", &PyHttp3Client::next_timeout_ms, nb::lock_self())
      .def("handle_timeout", &PyHttp3Client::handle_timeout, nb::lock_self())
      .def("close", &PyHttp3Client::close, nb::lock_self(),
           nb::arg("error_code") = 0, nb::arg("reason") = "")
      .def("is_connected", &PyHttp3Client::is_connected, nb::lock_self())
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

  m.def("version",
        [] { return std::string(Api().supported_versions_sample()); },
        "Sample QUIC version string from QUICHE (first entry of "
        "AllSupportedVersions).");
}
