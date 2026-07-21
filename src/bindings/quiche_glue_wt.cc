#include "quiche_glue.h"
#include "quiche_glue_exports.h"
#include "quiche_glue_sans_io.h"

#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/base/casts.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "quiche/common/http/http_header_block.h"
#include "quiche/common/quiche_mem_slice.h"
#include "quiche/quic/core/crypto/quic_crypto_client_config.h"
#include "quiche/quic/core/http/quic_connection_migration_manager.h"
#include "quiche/quic/core/http/quic_spdy_client_session.h"
#include "quiche/quic/core/http/quic_spdy_client_stream.h"
#include "quiche/quic/core/http/quic_spdy_session.h"
#include "quiche/quic/core/http/web_transport_http3.h"
#include "quiche/quic/core/quic_config.h"
#include "quiche/quic/core/quic_connection.h"
#include "quiche/quic/core/quic_default_clock.h"
#include "quiche/quic/core/quic_default_connection_helper.h"
#include "quiche/quic/core/quic_packets.h"
#include "quiche/quic/core/quic_queue_alarm_factory.h"
#include "quiche/quic/core/quic_server_id.h"
#include "quiche/quic/core/quic_session.h"
#include "quiche/quic/core/quic_types.h"
#include "quiche/quic/core/quic_versions.h"
#include "quiche/quic/platform/api/quic_default_proof_providers.h"
#include "quiche/quic/platform/api/quic_socket_address.h"
#include "quiche/quic/tools/fake_proof_verifier.h"
#include "quiche/quic/tools/quic_client_base.h"
#include "quiche/quic/tools/quic_name_lookup.h"
#include "quiche/web_transport/web_transport.h"

namespace {

using quiche_py::SansIoNetworkHelper;
using quiche_py::SendQueue;
using webtransport::DatagramStatusCode;
using webtransport::SessionErrorCode;
using webtransport::Stream;

absl::string_view BytesView(const uint8_t* data, size_t data_len) {
  if (data == nullptr || data_len == 0) {
    return {};
  }
  return absl::string_view(reinterpret_cast<const char*>(data), data_len);
}

class WtSessionVisitor : public webtransport::SessionVisitor {
 public:
  void OnSessionReady() override { session_ready_ = true; }

  void OnSessionClosed(SessionErrorCode error_code,
                       const std::string& error_message) override {
    session_ready_ = false;
    session_closed_ = true;
    close_error_code_ = error_code;
    close_error_message_ = error_message;
  }

  void OnIncomingBidirectionalStreamAvailable() override {}
  void OnIncomingUnidirectionalStreamAvailable() override {}

  void OnDatagramReceived(absl::string_view datagram) override {
    received_datagrams_.emplace_back(datagram);
  }

  void OnCanCreateNewOutgoingBidirectionalStream() override {}
  void OnCanCreateNewOutgoingUnidirectionalStream() override {}

  bool session_ready() const { return session_ready_; }
  bool session_closed() const { return session_closed_; }
  SessionErrorCode close_error_code() const { return close_error_code_; }
  const std::string& close_error_message() const {
    return close_error_message_;
  }

  bool HasDatagram() const { return !received_datagrams_.empty(); }
  size_t NextDatagramSize() const {
    return received_datagrams_.empty() ? 0 : received_datagrams_.front().size();
  }
  std::string TakeDatagram() {
    std::string datagram = std::move(received_datagrams_.front());
    received_datagrams_.pop_front();
    return datagram;
  }

 private:
  bool session_ready_ = false;
  bool session_closed_ = false;
  SessionErrorCode close_error_code_ = 0;
  std::string close_error_message_;
  std::deque<std::string> received_datagrams_;
};

class WtSpdyClientSession : public quic::QuicSpdyClientSession {
 public:
  WtSpdyClientSession(const quic::QuicConfig& config,
                      const quic::ParsedQuicVersionVector& supported_versions,
                      quic::QuicConnection* connection,
                      const quic::QuicServerId& server_id,
                      quic::QuicCryptoClientConfig* crypto_config)
      : quic::QuicSpdyClientSession(
            config, supported_versions, connection,
            /*visitor=*/nullptr, /*writer=*/nullptr,
            /*migration_helper=*/nullptr,
            quic::QuicConnectionMigrationConfig{
                .allow_server_preferred_address = false},
            server_id, crypto_config, quic::QuicPriorityType::kWebTransport) {}

  quic::WebTransportHttp3VersionSet LocallySupportedWebTransportVersions()
      const override {
    return quic::kDefaultSupportedWebTransportVersions;
  }

  quic::HttpDatagramSupport LocalHttpDatagramSupport() override {
    return quic::HttpDatagramSupport::kRfcAndDraft04;
  }
};

class GoogleQuichePyWtClient : public quic::QuicClientBase {
 public:
  GoogleQuichePyWtClient(quic::QuicSocketAddress server_address,
                         quic::QuicServerId server_id, SendQueue* send_queue,
                         std::unique_ptr<quic::ProofVerifier> proof_verifier)
      : quic::QuicClientBase(
            server_id, quic::CurrentSupportedHttp3Versions(), quic::QuicConfig(),
            new quic::QuicDefaultConnectionHelper(),
            new quic::QuicQueueAlarmFactory(),
            std::make_unique<SansIoNetworkHelper>(send_queue),
            std::move(proof_verifier), nullptr) {
    set_server_address(server_address);
  }

  quic::QuicQueueAlarmFactory* queue_alarm_factory() {
    return static_cast<quic::QuicQueueAlarmFactory*>(alarm_factory());
  }

  WtSpdyClientSession* client_session() {
    return absl::down_cast<WtSpdyClientSession*>(QuicClientBase::session());
  }

  bool EarlyDataAccepted() override {
    return client_session()->EarlyDataAccepted();
  }
  bool ReceivedInchoateReject() override {
    return client_session()->ReceivedInchoateReject();
  }

 protected:
  void InitializeSession() override {
    client_session()->Initialize();
    client_session()->CryptoConnect();
  }

  int GetNumSentClientHellosFromSession() override {
    return client_session()->GetNumSentClientHellos();
  }
  int GetNumReceivedServerConfigUpdatesFromSession() override {
    return client_session()->GetNumReceivedServerConfigUpdates();
  }
  bool HasActiveRequests() override {
    return client_session()->HasActiveRequestStreams();
  }

  std::unique_ptr<quic::QuicSession> CreateQuicClientSession(
      const quic::ParsedQuicVersionVector& supported_versions,
      quic::QuicConnection* connection) override {
    return std::make_unique<WtSpdyClientSession>(
        *config(), supported_versions, connection, server_id(), crypto_config());
  }
};

quiche_py_status MapDatagramStatus(const webtransport::DatagramStatus& status,
                                   std::string* error_message) {
  switch (status.code) {
    case DatagramStatusCode::kSuccess:
      return QUICHE_PY_STATUS_OK;
    case DatagramStatusCode::kBlocked:
      return QUICHE_PY_STATUS_WOULD_BLOCK;
    case DatagramStatusCode::kTooBig:
      if (error_message != nullptr) {
        *error_message = status.error_message;
      }
      return QUICHE_PY_STATUS_TOO_BIG;
    case DatagramStatusCode::kInternalError:
      if (error_message != nullptr) {
        *error_message = status.error_message;
      }
      return QUICHE_PY_STATUS_ERROR;
  }
  if (error_message != nullptr) {
    *error_message = "unknown datagram status";
  }
  return QUICHE_PY_STATUS_ERROR;
}

}  // namespace

struct quiche_py_wt_client {
  quiche_py_wt_client(std::string host, uint16_t port, std::string server_name,
                      bool verify_peer)
      : host_(std::move(host)),
        port_(port),
        server_name_(server_name.empty() ? host_ : std::move(server_name)),
        verify_peer_(verify_peer) {
    if (host_.empty()) {
      SetError("host must not be empty");
      return;
    }
    server_address_ = quic::tools::LookupAddress(host_, absl::StrCat(port_));
    if (!server_address_.IsInitialized()) {
      SetError(absl::StrCat("failed to resolve ", host_, ":", port_));
      return;
    }
    std::unique_ptr<quic::ProofVerifier> proof_verifier;
    if (verify_peer_) {
      proof_verifier = quic::CreateDefaultProofVerifier(server_name_);
    } else {
      proof_verifier = std::make_unique<quic::FakeProofVerifier>();
    }
    if (!proof_verifier) {
      SetError("failed to create QUIC proof verifier");
      return;
    }
    client_ = std::make_unique<GoogleQuichePyWtClient>(
        server_address_, quic::QuicServerId(server_name_, port_), &send_queue_,
        std::move(proof_verifier));
  }

  quiche_py_status Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    ResetRuntimeError();
    quiche_py_status status = EnsureClient();
    if (status != QUICHE_PY_STATUS_OK) {
      return status;
    }
    if (client_->connected()) {
      return QUICHE_PY_STATUS_OK;
    }
    web_transport_ = nullptr;
    session_visitor_ = nullptr;
    connect_requested_ = false;
    if (!client_->initialized() && !client_->Initialize()) {
      SetError("failed to initialize the WebTransport client");
      return QUICHE_PY_STATUS_ERROR;
    }
    client_->StartConnect();
    if (!client_->connected_or_attempting_connect()) {
      SetErrorIfEmpty("failed to start the WebTransport handshake");
      return QUICHE_PY_STATUS_ERROR;
    }
    return QUICHE_PY_STATUS_OK;
  }

  quiche_py_status ReceivePacket(const uint8_t* data, size_t data_len) {
    std::lock_guard<std::mutex> lock(mutex_);
    ResetRuntimeError();
    if (data == nullptr && data_len > 0) {
      SetError("data must not be null when data_len > 0");
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    if (data_len == 0) {
      return QUICHE_PY_STATUS_OK;
    }
    quiche_py_status status = EnsureClient();
    if (status != QUICHE_PY_STATUS_OK) {
      return status;
    }
    quic::QuicSession* session = client_->session();
    if (session == nullptr) {
      SetError("handshake has not been started");
      return QUICHE_PY_STATUS_ERROR;
    }
    quic::QuicReceivedPacket packet(reinterpret_cast<const char*>(data),
                                    data_len,
                                    quic::QuicDefaultClock::Get()->Now());
    session->ProcessUdpPacket(client_->network_helper()->GetLatestClientAddress(),
                              server_address_, packet);
    if (!client_->connected()) {
      PopulateClosedErrorIfNeeded();
      SetErrorIfEmpty("connection closed while processing a packet");
      return QUICHE_PY_STATUS_CLOSED;
    }
    return QUICHE_PY_STATUS_OK;
  }

  quiche_py_status NextSend(uint8_t* buffer, size_t buffer_len,
                            size_t* written) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (written == nullptr) {
      SetError("written must not be null");
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    *written = 0;
    if (send_queue_.packets.empty()) {
      return QUICHE_PY_STATUS_WOULD_BLOCK;
    }
    const std::string& front = send_queue_.packets.front();
    if (buffer_len < front.size()) {
      SetError(absl::StrCat("buffer too small for packet: need ", front.size(),
                            " bytes"));
      return QUICHE_PY_STATUS_TOO_BIG;
    }
    std::memcpy(buffer, front.data(), front.size());
    *written = front.size();
    send_queue_.packets.pop_front();
    return QUICHE_PY_STATUS_OK;
  }

  quiche_py_status NextTimeoutMs(uint64_t* timeout_ms, bool* has_timeout) {
    std::lock_guard<std::mutex> lock(mutex_);
    ResetRuntimeError();
    if (timeout_ms == nullptr || has_timeout == nullptr) {
      SetError("timeout_ms and has_timeout must not be null");
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    *timeout_ms = 0;
    *has_timeout = false;
    quiche_py_status status = EnsureClient();
    if (status != QUICHE_PY_STATUS_OK) {
      return status;
    }
    std::optional<quic::QuicTime> next =
        client_->queue_alarm_factory()->GetNextUpcomingAlarm();
    if (!next.has_value()) {
      return QUICHE_PY_STATUS_OK;
    }
    quic::QuicTime now = quic::QuicDefaultClock::Get()->Now();
    int64_t remaining_us = (*next - now).ToMicroseconds();
    if (remaining_us < 0) {
      remaining_us = 0;
    }
    *timeout_ms = static_cast<uint64_t>((remaining_us + 999) / 1000);
    *has_timeout = true;
    return QUICHE_PY_STATUS_OK;
  }

  quiche_py_status HandleTimeout() {
    std::lock_guard<std::mutex> lock(mutex_);
    ResetRuntimeError();
    quiche_py_status status = EnsureClient();
    if (status != QUICHE_PY_STATUS_OK) {
      return status;
    }
    client_->queue_alarm_factory()->ProcessAlarmsUpTo(
        quic::QuicDefaultClock::Get()->Now());
    if (!client_->connected()) {
      PopulateClosedErrorIfNeeded();
      SetErrorIfEmpty("connection closed while handling a timeout");
      return QUICHE_PY_STATUS_CLOSED;
    }
    return QUICHE_PY_STATUS_OK;
  }

  void Close(uint32_t error_code, const char* reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    ResetRuntimeError();
    if (web_transport_ != nullptr) {
      web_transport_->CloseSession(error_code, reason == nullptr ? "" : reason);
      return;
    }
    if (client_ != nullptr) {
      client_->Disconnect();
    }
  }

  bool IsConnected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return client_ != nullptr && client_->connected();
  }

  bool IsSessionReady() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return session_visitor_ != nullptr && session_visitor_->session_ready();
  }

  quiche_py_status ConnectSession(const char* path,
                                  const quiche_py_http3_header* extra_headers,
                                  size_t header_count) {
    std::lock_guard<std::mutex> lock(mutex_);
    ResetRuntimeError();
    if (path == nullptr || path[0] == '\0') {
      SetError("path must not be empty");
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    if (extra_headers == nullptr && header_count > 0) {
      SetError("extra_headers must not be null when header_count > 0");
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    quiche_py_status status = EnsureClient();
    if (status != QUICHE_PY_STATUS_OK) {
      return status;
    }
    if (connect_requested_ && web_transport_ != nullptr) {
      return QUICHE_PY_STATUS_OK;
    }
    if (!client_->connected()) {
      return QUICHE_PY_STATUS_WOULD_BLOCK;
    }
    WtSpdyClientSession* session = client_->client_session();
    if (session == nullptr) {
      SetError("HTTP/3 session is unavailable");
      return QUICHE_PY_STATUS_ERROR;
    }
    if (!session->settings_received()) {
      return QUICHE_PY_STATUS_WOULD_BLOCK;
    }
    if (!session->SupportsWebTransport()) {
      SetError("QUIC server does not support WebTransport");
      return QUICHE_PY_STATUS_ERROR;
    }
    if (!session->CanOpenNextOutgoingBidirectionalStream()) {
      return QUICHE_PY_STATUS_WOULD_BLOCK;
    }

    auto* stream = session->CreateOutgoingBidirectionalStream();
    if (stream == nullptr) {
      return QUICHE_PY_STATUS_WOULD_BLOCK;
    }

    quiche::HttpHeaderBlock headers;
    for (size_t i = 0; i < header_count; ++i) {
      const quiche_py_http3_header& header = extra_headers[i];
      if (header.name == nullptr || header.name_len == 0) {
        SetError("header name must not be empty");
        return QUICHE_PY_STATUS_INVALID_ARGUMENT;
      }
      absl::string_view name(reinterpret_cast<const char*>(header.name),
                             header.name_len);
      absl::string_view value(
          header.value == nullptr
              ? ""
              : reinterpret_cast<const char*>(header.value),
          header.value_len);
      headers[name] = value;
    }
    headers[":scheme"] = "https";
    headers[":authority"] = server_name_;
    headers[":path"] = path;
    headers[":method"] = "CONNECT";
    headers[":protocol"] = "webtransport";
    stream->SendRequest(std::move(headers), "", false);

    web_transport_ = stream->web_transport();
    if (web_transport_ == nullptr) {
      SetError("failed to associate a WebTransport session with the request");
      return QUICHE_PY_STATUS_ERROR;
    }
    auto visitor = std::make_unique<WtSessionVisitor>();
    session_visitor_ = visitor.get();
    web_transport_->SetVisitor(std::move(visitor));
    connect_requested_ = true;
    return QUICHE_PY_STATUS_OK;
  }

  quiche_py_status OpenStream(bool unidirectional, uint32_t* stream_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    ResetRuntimeError();
    if (stream_id == nullptr) {
      SetError("stream_id must not be null");
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    quiche_py_status status = EnsureSessionReady();
    if (status != QUICHE_PY_STATUS_OK) {
      return status;
    }
    Stream* stream = nullptr;
    if (unidirectional) {
      if (!web_transport_->CanOpenNextOutgoingUnidirectionalStream()) {
        return QUICHE_PY_STATUS_WOULD_BLOCK;
      }
      stream = web_transport_->OpenOutgoingUnidirectionalStream();
    } else {
      if (!web_transport_->CanOpenNextOutgoingBidirectionalStream()) {
        return QUICHE_PY_STATUS_WOULD_BLOCK;
      }
      stream = web_transport_->OpenOutgoingBidirectionalStream();
    }
    if (stream == nullptr) {
      return QUICHE_PY_STATUS_WOULD_BLOCK;
    }
    *stream_id = stream->GetStreamId();
    return QUICHE_PY_STATUS_OK;
  }

  quiche_py_status AcceptStream(bool unidirectional, uint32_t* stream_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    ResetRuntimeError();
    if (stream_id == nullptr) {
      SetError("stream_id must not be null");
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    quiche_py_status status = EnsureSessionReady();
    if (status != QUICHE_PY_STATUS_OK) {
      return status;
    }
    Stream* stream = unidirectional
                         ? web_transport_->AcceptIncomingUnidirectionalStream()
                         : web_transport_->AcceptIncomingBidirectionalStream();
    if (stream == nullptr) {
      return QUICHE_PY_STATUS_WOULD_BLOCK;
    }
    *stream_id = stream->GetStreamId();
    return QUICHE_PY_STATUS_OK;
  }

  quiche_py_status ReadStream(uint32_t stream_id, uint8_t* buffer,
                              size_t buffer_len, size_t* bytes_read, bool* fin) {
    std::lock_guard<std::mutex> lock(mutex_);
    ResetRuntimeError();
    if (bytes_read == nullptr || fin == nullptr) {
      SetError("bytes_read and fin must not be null");
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    *bytes_read = 0;
    *fin = false;
    Stream* stream = nullptr;
    quiche_py_status status = RequireStream(stream_id, &stream);
    if (status != QUICHE_PY_STATUS_OK) {
      return status;
    }
    Stream::PeekResult peek = stream->PeekNextReadableRegion();
    if (!peek.has_data()) {
      return QUICHE_PY_STATUS_WOULD_BLOCK;
    }
    if (buffer_len == 0) {
      SetError("buffer_len must be greater than zero");
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    if (buffer == nullptr) {
      SetError("buffer must not be null");
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    Stream::ReadResult read =
        stream->Read(absl::Span<char>(reinterpret_cast<char*>(buffer), buffer_len));
    *bytes_read = read.bytes_read;
    *fin = read.fin;
    return QUICHE_PY_STATUS_OK;
  }

  quiche_py_status WriteStream(uint32_t stream_id, const uint8_t* data,
                               size_t data_len, bool fin) {
    std::lock_guard<std::mutex> lock(mutex_);
    ResetRuntimeError();
    if (data == nullptr && data_len > 0) {
      SetError("data must not be null when data_len > 0");
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    if (data_len == 0 && !fin) {
      SetError("empty write requires fin=true");
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    Stream* stream = nullptr;
    quiche_py_status status = RequireStream(stream_id, &stream);
    if (status != QUICHE_PY_STATUS_OK) {
      return status;
    }
    if (!stream->CanWrite()) {
      return QUICHE_PY_STATUS_WOULD_BLOCK;
    }
    std::vector<quiche::QuicheMemSlice> slices;
    if (data_len > 0) {
      slices.push_back(quiche::QuicheMemSlice::Copy(BytesView(data, data_len)));
    }
    webtransport::StreamWriteOptions options;
    options.set_send_fin(fin);
    absl::Status write_status = stream->Writev(absl::MakeSpan(slices), options);
    if (!write_status.ok()) {
      if (write_status.code() == absl::StatusCode::kUnavailable) {
        return QUICHE_PY_STATUS_WOULD_BLOCK;
      }
      SetError(write_status.ToString());
      return QUICHE_PY_STATUS_ERROR;
    }
    return QUICHE_PY_STATUS_OK;
  }

  quiche_py_status ResetStream(uint32_t stream_id, uint32_t error_code) {
    std::lock_guard<std::mutex> lock(mutex_);
    ResetRuntimeError();
    Stream* stream = nullptr;
    quiche_py_status status = RequireStream(stream_id, &stream);
    if (status != QUICHE_PY_STATUS_OK) {
      return status;
    }
    stream->ResetWithUserCode(error_code);
    return QUICHE_PY_STATUS_OK;
  }

  quiche_py_status StopSending(uint32_t stream_id, uint32_t error_code) {
    std::lock_guard<std::mutex> lock(mutex_);
    ResetRuntimeError();
    Stream* stream = nullptr;
    quiche_py_status status = RequireStream(stream_id, &stream);
    if (status != QUICHE_PY_STATUS_OK) {
      return status;
    }
    stream->SendStopSending(error_code);
    return QUICHE_PY_STATUS_OK;
  }

  bool StreamCanWrite(uint32_t stream_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    Stream* stream = StreamById(stream_id);
    return stream != nullptr && stream->CanWrite();
  }

  bool StreamExists(uint32_t stream_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return StreamById(stream_id) != nullptr;
  }

  quiche_py_status SendDatagram(const uint8_t* data, size_t data_len) {
    std::lock_guard<std::mutex> lock(mutex_);
    ResetRuntimeError();
    if (data == nullptr && data_len > 0) {
      SetError("data must not be null when data_len > 0");
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    quiche_py_status status = EnsureSessionReady();
    if (status != QUICHE_PY_STATUS_OK) {
      return status;
    }
    return MapDatagramStatus(
        web_transport_->SendOrQueueDatagram(BytesView(data, data_len)),
        &last_error_);
  }

  quiche_py_status PeekDatagramSize(size_t* datagram_size) {
    std::lock_guard<std::mutex> lock(mutex_);
    ResetRuntimeError();
    if (datagram_size == nullptr) {
      SetError("datagram_size must not be null");
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    *datagram_size = 0;
    quiche_py_status status = EnsureSessionReady();
    if (status != QUICHE_PY_STATUS_OK) {
      return status;
    }
    if (session_visitor_ == nullptr || !session_visitor_->HasDatagram()) {
      return QUICHE_PY_STATUS_WOULD_BLOCK;
    }
    *datagram_size = session_visitor_->NextDatagramSize();
    return QUICHE_PY_STATUS_OK;
  }

  quiche_py_status ReceiveDatagram(uint8_t* buffer, size_t buffer_len,
                                   size_t* bytes_read) {
    std::lock_guard<std::mutex> lock(mutex_);
    ResetRuntimeError();
    if (bytes_read == nullptr) {
      SetError("bytes_read must not be null");
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    *bytes_read = 0;
    quiche_py_status status = EnsureSessionReady();
    if (status != QUICHE_PY_STATUS_OK) {
      return status;
    }
    if (session_visitor_ == nullptr || !session_visitor_->HasDatagram()) {
      return QUICHE_PY_STATUS_WOULD_BLOCK;
    }
    std::string datagram = session_visitor_->TakeDatagram();
    if (buffer_len < datagram.size()) {
      SetError(absl::StrCat("buffer too small for datagram: need ",
                            datagram.size(), " bytes"));
      return QUICHE_PY_STATUS_TOO_BIG;
    }
    if (!datagram.empty()) {
      if (buffer == nullptr) {
        SetError("buffer must not be null");
        return QUICHE_PY_STATUS_INVALID_ARGUMENT;
      }
      std::memcpy(buffer, datagram.data(), datagram.size());
    }
    *bytes_read = datagram.size();
    return QUICHE_PY_STATUS_OK;
  }

  size_t MaxDatagramSize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (web_transport_ == nullptr) {
      return 0;
    }
    return static_cast<size_t>(web_transport_->GetMaxDatagramSize());
  }

  const char* last_error() const { return last_error_.c_str(); }

 private:
  quiche_py_status EnsureClient() {
    if (client_ == nullptr) {
      SetErrorIfEmpty("WebTransport client is not available");
      return QUICHE_PY_STATUS_ERROR;
    }
    return QUICHE_PY_STATUS_OK;
  }

  quiche_py_status EnsureSessionReady() {
    quiche_py_status status = EnsureClient();
    if (status != QUICHE_PY_STATUS_OK) {
      return status;
    }
    if (web_transport_ == nullptr || session_visitor_ == nullptr ||
        !session_visitor_->session_ready()) {
      SetError("WebTransport session is not ready");
      return QUICHE_PY_STATUS_ERROR;
    }
    return QUICHE_PY_STATUS_OK;
  }

  Stream* StreamById(uint32_t stream_id) {
    if (web_transport_ == nullptr) {
      return nullptr;
    }
    return web_transport_->GetStreamById(stream_id);
  }

  quiche_py_status RequireStream(uint32_t stream_id, Stream** stream) {
    quiche_py_status status = EnsureSessionReady();
    if (status != QUICHE_PY_STATUS_OK) {
      return status;
    }
    *stream = StreamById(stream_id);
    if (*stream == nullptr) {
      SetError(absl::StrCat("stream ", stream_id, " is not available"));
      return QUICHE_PY_STATUS_NOT_FOUND;
    }
    return QUICHE_PY_STATUS_OK;
  }

  void PopulateClosedErrorIfNeeded() {
    if (!last_error_.empty()) {
      return;
    }
    if (session_visitor_ != nullptr && session_visitor_->session_closed()) {
      if (!session_visitor_->close_error_message().empty()) {
        last_error_ = session_visitor_->close_error_message();
      } else {
        last_error_ = absl::StrCat("session closed (error code ",
                                   session_visitor_->close_error_code(), ")");
      }
    }
  }

  void SetErrorIfEmpty(absl::string_view message) {
    if (last_error_.empty()) {
      last_error_ = std::string(message);
    }
  }
  void SetError(std::string message) { last_error_ = std::move(message); }
  void ResetRuntimeError() {
    if (client_ != nullptr) {
      last_error_.clear();
    }
  }

  std::string host_;
  uint16_t port_;
  std::string server_name_;
  bool verify_peer_;
  std::string last_error_;
  quic::QuicSocketAddress server_address_;
  SendQueue send_queue_;
  std::unique_ptr<GoogleQuichePyWtClient> client_;
  quic::WebTransportHttp3* web_transport_ = nullptr;
  WtSessionVisitor* session_visitor_ = nullptr;
  bool connect_requested_ = false;
  mutable std::mutex mutex_;
};

extern "C" {

quiche_py_wt_client* quiche_py_wt_client_create(
    const quiche_py_wt_client_options* options) {
  if (options == nullptr) {
    return nullptr;
  }
  return new quiche_py_wt_client(
      options->host == nullptr ? "" : options->host, options->port,
      options->server_name == nullptr ? "" : options->server_name,
      options->verify_peer);
}

void quiche_py_wt_client_destroy(quiche_py_wt_client* client) { delete client; }

quiche_py_status quiche_py_wt_client_start(quiche_py_wt_client* client) {
  return client == nullptr ? QUICHE_PY_STATUS_INVALID_ARGUMENT : client->Start();
}

quiche_py_status quiche_py_wt_client_receive_packet(
    quiche_py_wt_client* client, const uint8_t* data, size_t data_len) {
  return client == nullptr ? QUICHE_PY_STATUS_INVALID_ARGUMENT
                           : client->ReceivePacket(data, data_len);
}

quiche_py_status quiche_py_wt_client_next_send(quiche_py_wt_client* client,
                                               uint8_t* buffer,
                                               size_t buffer_len,
                                               size_t* written) {
  return client == nullptr
             ? QUICHE_PY_STATUS_INVALID_ARGUMENT
             : client->NextSend(buffer, buffer_len, written);
}

quiche_py_status quiche_py_wt_client_next_timeout_ms(
    quiche_py_wt_client* client, uint64_t* timeout_ms, bool* has_timeout) {
  return client == nullptr
             ? QUICHE_PY_STATUS_INVALID_ARGUMENT
             : client->NextTimeoutMs(timeout_ms, has_timeout);
}

quiche_py_status quiche_py_wt_client_handle_timeout(
    quiche_py_wt_client* client) {
  return client == nullptr ? QUICHE_PY_STATUS_INVALID_ARGUMENT
                           : client->HandleTimeout();
}

void quiche_py_wt_client_close(quiche_py_wt_client* client,
                               uint32_t error_code, const char* reason) {
  if (client != nullptr) {
    client->Close(error_code, reason);
  }
}

bool quiche_py_wt_client_is_connected(const quiche_py_wt_client* client) {
  return client != nullptr && client->IsConnected();
}

quiche_py_status quiche_py_wt_client_connect_session(
    quiche_py_wt_client* client, const char* path,
    const quiche_py_http3_header* extra_headers, size_t header_count) {
  return client == nullptr
             ? QUICHE_PY_STATUS_INVALID_ARGUMENT
             : client->ConnectSession(path, extra_headers, header_count);
}

bool quiche_py_wt_client_is_session_ready(const quiche_py_wt_client* client) {
  return client != nullptr && client->IsSessionReady();
}

quiche_py_status quiche_py_wt_client_open_stream(quiche_py_wt_client* client,
                                                 bool unidirectional,
                                                 uint32_t* stream_id) {
  return client == nullptr
             ? QUICHE_PY_STATUS_INVALID_ARGUMENT
             : client->OpenStream(unidirectional, stream_id);
}

quiche_py_status quiche_py_wt_client_accept_stream(quiche_py_wt_client* client,
                                                   bool unidirectional,
                                                   uint32_t* stream_id) {
  return client == nullptr
             ? QUICHE_PY_STATUS_INVALID_ARGUMENT
             : client->AcceptStream(unidirectional, stream_id);
}

quiche_py_status quiche_py_wt_client_read_stream(
    quiche_py_wt_client* client, uint32_t stream_id, uint8_t* buffer,
    size_t buffer_len, size_t* bytes_read, bool* fin) {
  return client == nullptr ? QUICHE_PY_STATUS_INVALID_ARGUMENT
                           : client->ReadStream(stream_id, buffer, buffer_len,
                                                bytes_read, fin);
}

quiche_py_status quiche_py_wt_client_write_stream(
    quiche_py_wt_client* client, uint32_t stream_id, const uint8_t* data,
    size_t data_len, bool fin) {
  return client == nullptr ? QUICHE_PY_STATUS_INVALID_ARGUMENT
                           : client->WriteStream(stream_id, data, data_len, fin);
}

quiche_py_status quiche_py_wt_client_reset_stream(quiche_py_wt_client* client,
                                                  uint32_t stream_id,
                                                  uint32_t error_code) {
  return client == nullptr
             ? QUICHE_PY_STATUS_INVALID_ARGUMENT
             : client->ResetStream(stream_id, error_code);
}

quiche_py_status quiche_py_wt_client_stop_sending(quiche_py_wt_client* client,
                                                  uint32_t stream_id,
                                                  uint32_t error_code) {
  return client == nullptr
             ? QUICHE_PY_STATUS_INVALID_ARGUMENT
             : client->StopSending(stream_id, error_code);
}

bool quiche_py_wt_client_stream_can_write(quiche_py_wt_client* client,
                                          uint32_t stream_id) {
  return client != nullptr && client->StreamCanWrite(stream_id);
}

bool quiche_py_wt_client_stream_exists(quiche_py_wt_client* client,
                                       uint32_t stream_id) {
  return client != nullptr && client->StreamExists(stream_id);
}

quiche_py_status quiche_py_wt_client_send_datagram(quiche_py_wt_client* client,
                                                   const uint8_t* data,
                                                   size_t data_len) {
  return client == nullptr ? QUICHE_PY_STATUS_INVALID_ARGUMENT
                           : client->SendDatagram(data, data_len);
}

quiche_py_status quiche_py_wt_client_peek_datagram_size(
    quiche_py_wt_client* client, size_t* datagram_size) {
  return client == nullptr ? QUICHE_PY_STATUS_INVALID_ARGUMENT
                           : client->PeekDatagramSize(datagram_size);
}

quiche_py_status quiche_py_wt_client_receive_datagram(
    quiche_py_wt_client* client, uint8_t* buffer, size_t buffer_len,
    size_t* bytes_read) {
  return client == nullptr ? QUICHE_PY_STATUS_INVALID_ARGUMENT
                           : client->ReceiveDatagram(buffer, buffer_len,
                                                     bytes_read);
}

size_t quiche_py_wt_client_max_datagram_size(quiche_py_wt_client* client) {
  return client == nullptr ? 0 : client->MaxDatagramSize();
}

const char* quiche_py_wt_client_last_error(const quiche_py_wt_client* client) {
  return client == nullptr ? "" : client->last_error();
}

}  // extern "C"
