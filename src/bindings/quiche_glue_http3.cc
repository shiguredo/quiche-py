#include "quiche_glue.h"
#include "quiche_glue_exports.h"
#include "quiche_glue_sans_io.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "quiche/common/http/http_header_block.h"
#include "quiche/quic/core/quic_default_clock.h"
#include "quiche/quic/core/quic_default_connection_helper.h"
#include "quiche/quic/core/quic_packets.h"
#include "quiche/quic/core/quic_queue_alarm_factory.h"
#include "quiche/quic/core/quic_server_id.h"
#include "quiche/quic/core/quic_types.h"
#include "quiche/quic/core/quic_versions.h"
#include "quiche/quic/platform/api/quic_default_proof_providers.h"
#include "quiche/quic/platform/api/quic_socket_address.h"
#include "quiche/quic/tools/fake_proof_verifier.h"
#include "quiche/quic/tools/quic_name_lookup.h"
#include "quiche/quic/tools/quic_spdy_client_base.h"

namespace {

using quiche_py::SansIoNetworkHelper;
using quiche_py::SendQueue;

struct CompletedHttp3Response {
  uint32_t stream_id = 0;
  int status_code = 0;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
};

class ResponseQueue : public quic::QuicSpdyClientBase::ResponseListener {
 public:
  void OnCompleteResponse(quic::QuicStreamId id,
                          const quiche::HttpHeaderBlock& response_headers,
                          absl::string_view response_body) override {
    CompletedHttp3Response completed;
    completed.stream_id = static_cast<uint32_t>(id);
    completed.body = std::string(response_body);
    auto status = response_headers.find(":status");
    if (status != response_headers.end()) {
      int code = 0;
      if (absl::SimpleAtoi(status->second, &code)) {
        completed.status_code = code;
      }
    }
    for (const auto& header : response_headers) {
      completed.headers.emplace_back(std::string(header.first),
                                     std::string(header.second));
    }
    responses_[static_cast<uint32_t>(id)] = std::move(completed);
  }

  std::optional<CompletedHttp3Response> Take(uint32_t stream_id) {
    auto it = responses_.find(stream_id);
    if (it == responses_.end()) {
      return std::nullopt;
    }
    CompletedHttp3Response completed = std::move(it->second);
    responses_.erase(it);
    return completed;
  }

 private:
  std::unordered_map<uint32_t, CompletedHttp3Response> responses_;
};

// sans-I/O 向け: ストリーム枠が無ければ即 nullptr (RunEventLoop で待たない)。
class GoogleQuichePyHttp3Client : public quic::QuicSpdyClientBase {
 public:
  GoogleQuichePyHttp3Client(
      quic::QuicSocketAddress server_address, quic::QuicServerId server_id,
      SendQueue* send_queue,
      std::unique_ptr<quic::ProofVerifier> proof_verifier)
      : quic::QuicSpdyClientBase(
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

  quic::QuicSpdyClientStream* CreateClientStream() override {
    if (!connected()) {
      return nullptr;
    }
    if (!client_session()->CanOpenNextOutgoingBidirectionalStream()) {
      return nullptr;
    }
    auto* stream = static_cast<quic::QuicSpdyClientStream*>(
        client_session()->CreateOutgoingBidirectionalStream());
    if (stream != nullptr) {
      stream->set_visitor(this);
    }
    return stream;
  }
};

}  // namespace

struct quiche_py_http3_response {
  CompletedHttp3Response data;
};

struct quiche_py_http3_client {
  quiche_py_http3_client(std::string host, uint16_t port,
                         std::string server_name, bool verify_peer)
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

    auto response_queue = std::make_unique<ResponseQueue>();
    response_queue_ = response_queue.get();
    client_ = std::make_unique<GoogleQuichePyHttp3Client>(
        server_address_, quic::QuicServerId(server_name_, port_), &send_queue_,
        std::move(proof_verifier));
    client_->set_response_listener(std::move(response_queue));
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
    if (!client_->initialized() && !client_->Initialize()) {
      SetError("failed to initialize the HTTP/3 client");
      return QUICHE_PY_STATUS_ERROR;
    }
    client_->StartConnect();
    if (!client_->connected_or_attempting_connect()) {
      SetErrorIfEmpty("failed to start the HTTP/3 handshake");
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
      SetErrorIfEmpty("connection closed while handling a timeout");
      return QUICHE_PY_STATUS_CLOSED;
    }
    return QUICHE_PY_STATUS_OK;
  }

  void Close(uint32_t /*error_code*/, const char* /*reason*/) {
    std::lock_guard<std::mutex> lock(mutex_);
    ResetRuntimeError();
    if (client_ != nullptr) {
      client_->Disconnect();
    }
  }

  bool IsConnected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return client_ != nullptr && client_->connected();
  }

  quiche_py_status SubmitRequest(const quiche_py_http3_header* headers,
                                 size_t header_count, const uint8_t* body,
                                 size_t body_len, bool fin,
                                 uint32_t* stream_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    ResetRuntimeError();
    if (stream_id == nullptr) {
      SetError("stream_id must not be null");
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    *stream_id = 0;
    if (headers == nullptr && header_count > 0) {
      SetError("headers must not be null when header_count > 0");
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    if (body == nullptr && body_len > 0) {
      SetError("body must not be null when body_len > 0");
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    quiche_py_status status = EnsureClient();
    if (status != QUICHE_PY_STATUS_OK) {
      return status;
    }
    if (!client_->connected()) {
      SetError("connection is not established");
      return QUICHE_PY_STATUS_ERROR;
    }

    quic::QuicSpdyClientStream* stream = client_->CreateClientStream();
    if (stream == nullptr) {
      return QUICHE_PY_STATUS_WOULD_BLOCK;
    }

    quiche::HttpHeaderBlock block;
    for (size_t i = 0; i < header_count; ++i) {
      const quiche_py_http3_header& header = headers[i];
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
      block[name] = value;
    }

    absl::string_view body_view(
        body == nullptr ? "" : reinterpret_cast<const char*>(body), body_len);
    stream->SendRequest(std::move(block), body_view, fin);
    *stream_id = static_cast<uint32_t>(stream->id());
    return QUICHE_PY_STATUS_OK;
  }

  quiche_py_status TakeResponse(uint32_t stream_id,
                                quiche_py_http3_response** response) {
    std::lock_guard<std::mutex> lock(mutex_);
    ResetRuntimeError();
    if (response == nullptr) {
      SetError("response must not be null");
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    *response = nullptr;
    if (response_queue_ == nullptr) {
      SetError("response queue is unavailable");
      return QUICHE_PY_STATUS_ERROR;
    }
    std::optional<CompletedHttp3Response> completed =
        response_queue_->Take(stream_id);
    if (!completed.has_value()) {
      return QUICHE_PY_STATUS_WOULD_BLOCK;
    }
    auto* owned = new quiche_py_http3_response;
    owned->data = std::move(*completed);
    *response = owned;
    return QUICHE_PY_STATUS_OK;
  }

  const char* last_error() const { return last_error_.c_str(); }

 private:
  quiche_py_status EnsureClient() {
    if (client_ == nullptr) {
      SetErrorIfEmpty("HTTP/3 client is not available");
      return QUICHE_PY_STATUS_ERROR;
    }
    return QUICHE_PY_STATUS_OK;
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
  ResponseQueue* response_queue_ = nullptr;
  std::unique_ptr<GoogleQuichePyHttp3Client> client_;
  mutable std::mutex mutex_;
};

extern "C" {

quiche_py_http3_client* quiche_py_h3_client_create(
    const quiche_py_http3_client_options* options) {
  if (options == nullptr) {
    return nullptr;
  }
  return new quiche_py_http3_client(
      options->host == nullptr ? "" : options->host, options->port,
      options->server_name == nullptr ? "" : options->server_name,
      options->verify_peer);
}

void quiche_py_h3_client_destroy(quiche_py_http3_client* client) {
  delete client;
}

quiche_py_status quiche_py_h3_client_start(quiche_py_http3_client* client) {
  return client == nullptr ? QUICHE_PY_STATUS_INVALID_ARGUMENT : client->Start();
}

quiche_py_status quiche_py_h3_client_receive_packet(
    quiche_py_http3_client* client, const uint8_t* data, size_t data_len) {
  return client == nullptr ? QUICHE_PY_STATUS_INVALID_ARGUMENT
                           : client->ReceivePacket(data, data_len);
}

quiche_py_status quiche_py_h3_client_next_send(quiche_py_http3_client* client,
                                               uint8_t* buffer,
                                               size_t buffer_len,
                                               size_t* written) {
  return client == nullptr
             ? QUICHE_PY_STATUS_INVALID_ARGUMENT
             : client->NextSend(buffer, buffer_len, written);
}

quiche_py_status quiche_py_h3_client_next_timeout_ms(
    quiche_py_http3_client* client, uint64_t* timeout_ms, bool* has_timeout) {
  return client == nullptr
             ? QUICHE_PY_STATUS_INVALID_ARGUMENT
             : client->NextTimeoutMs(timeout_ms, has_timeout);
}

quiche_py_status quiche_py_h3_client_handle_timeout(
    quiche_py_http3_client* client) {
  return client == nullptr ? QUICHE_PY_STATUS_INVALID_ARGUMENT
                           : client->HandleTimeout();
}

void quiche_py_h3_client_close(quiche_py_http3_client* client,
                               uint32_t error_code, const char* reason) {
  if (client != nullptr) {
    client->Close(error_code, reason);
  }
}

bool quiche_py_h3_client_is_connected(const quiche_py_http3_client* client) {
  return client != nullptr && client->IsConnected();
}

quiche_py_status quiche_py_h3_client_submit_request(
    quiche_py_http3_client* client, const quiche_py_http3_header* headers,
    size_t header_count, const uint8_t* body, size_t body_len, bool fin,
    uint32_t* stream_id) {
  return client == nullptr ? QUICHE_PY_STATUS_INVALID_ARGUMENT
                           : client->SubmitRequest(headers, header_count, body,
                                                   body_len, fin, stream_id);
}

quiche_py_status quiche_py_h3_client_take_response(
    quiche_py_http3_client* client, uint32_t stream_id,
    quiche_py_http3_response** response) {
  return client == nullptr ? QUICHE_PY_STATUS_INVALID_ARGUMENT
                           : client->TakeResponse(stream_id, response);
}

void quiche_py_h3_response_destroy(quiche_py_http3_response* response) {
  delete response;
}

uint32_t quiche_py_h3_response_stream_id(
    const quiche_py_http3_response* response) {
  return response == nullptr ? 0 : response->data.stream_id;
}

int quiche_py_h3_response_status_code(
    const quiche_py_http3_response* response) {
  return response == nullptr ? 0 : response->data.status_code;
}

size_t quiche_py_h3_response_header_count(
    const quiche_py_http3_response* response) {
  return response == nullptr ? 0 : response->data.headers.size();
}

quiche_py_status quiche_py_h3_response_header_at(
    const quiche_py_http3_response* response, size_t index, const uint8_t** name,
    size_t* name_len, const uint8_t** value, size_t* value_len) {
  if (response == nullptr || name == nullptr || name_len == nullptr ||
      value == nullptr || value_len == nullptr) {
    return QUICHE_PY_STATUS_INVALID_ARGUMENT;
  }
  if (index >= response->data.headers.size()) {
    return QUICHE_PY_STATUS_NOT_FOUND;
  }
  const auto& header = response->data.headers[index];
  *name = reinterpret_cast<const uint8_t*>(header.first.data());
  *name_len = header.first.size();
  *value = reinterpret_cast<const uint8_t*>(header.second.data());
  *value_len = header.second.size();
  return QUICHE_PY_STATUS_OK;
}

const uint8_t* quiche_py_h3_response_body(
    const quiche_py_http3_response* response, size_t* body_len) {
  if (response == nullptr || body_len == nullptr) {
    return nullptr;
  }
  *body_len = response->data.body.size();
  return reinterpret_cast<const uint8_t*>(response->data.body.data());
}

const char* quiche_py_h3_client_last_error(
    const quiche_py_http3_client* client) {
  return client == nullptr ? "" : client->last_error();
}

}  // extern "C"
