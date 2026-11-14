/* MOQT draft-18 クライアント (WebTransport over HTTP/3) の C API 実装。
 * 仕様: draft-ietf-moq-transport-18 */
#include "quiche_glue.h"
#include "quiche_glue_exports.h"
#include "quiche_glue_sans_io.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "absl/base/casts.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "quiche/common/http/http_header_block.h"
#include "quiche/common/quiche_mem_slice.h"
#include "quiche/quic/core/crypto/quic_crypto_client_config.h"
#include "quiche/quic/core/http/quic_connection_migration_manager.h"
#include "quiche/quic/core/http/quic_spdy_client_session.h"
#include "quiche/quic/core/http/quic_spdy_client_stream.h"
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
#include "quiche/quic/moqt/moqt_fetch_task.h"
#include "quiche/quic/moqt/moqt_key_value_pair.h"
#include "quiche/quic/moqt/moqt_names.h"
#include "quiche/quic/moqt/moqt_publisher.h"
#include "quiche/quic/moqt/moqt_session.h"
#include "quiche/quic/moqt/moqt_session_callbacks.h"
#include "quiche/quic/moqt/moqt_session_interface.h"
#include "quiche/quic/moqt/moqt_types.h"
#include "quiche/quic/platform/api/quic_default_proof_providers.h"
#include "quiche/quic/platform/api/quic_socket_address.h"
#include "quiche/quic/tools/fake_proof_verifier.h"
#include "quiche/quic/tools/quic_client_base.h"
#include "quiche/quic/tools/quic_name_lookup.h"
#include "quiche/web_transport/web_transport.h"
#include "quiche/web_transport/web_transport_headers.h"

namespace {

using quiche_py::SansIoNetworkHelper;
using quiche_py::SendQueue;

struct PyMoqtEventData {
  int kind = 0;
  uint64_t request_id = 0;
  std::string track_name;
  uint64_t group_id = 0;
  uint64_t object_id = 0;
  uint64_t subgroup_id = 0;
  bool has_subgroup = false;
  uint64_t offset = 0;
  bool fin = false;
  std::string payload;
  uint64_t publisher_priority = 0;
  int status = 0;
  uint32_t error_code = 0;
  std::string reason;
  std::string new_session_uri;
  uint64_t timeout_ms = 0;
};

// MOQT の C++ コールバックから Python 側のイベントキューへ橋渡しする。
class MoqtEventSink {
 public:
  virtual ~MoqtEventSink() = default;
  virtual void PushEvent(PyMoqtEventData event) = 0;
};

// SUBSCRIBE の応答と OBJECT / 終了イベントをイベントキューへ積む。
class PyMoqtSubscribeVisitor : public moqt::SubscribeVisitor {
 public:
  explicit PyMoqtSubscribeVisitor(MoqtEventSink* sink) : sink_(sink) {}

  void OnReply(const moqt::FullTrackName& full_track_name,
               std::variant<moqt::SubscribeOkData, moqt::MoqtRequestErrorInfo>
                   response) override {
    PyMoqtEventData event;
    event.track_name = full_track_name.ToString();
    if (std::holds_alternative<moqt::SubscribeOkData>(response)) {
      event.kind = QUICHE_PY_MOQT_EVENT_SUBSCRIBE_OK;
    } else {
      const moqt::MoqtRequestErrorInfo& error =
          std::get<moqt::MoqtRequestErrorInfo>(response);
      event.kind = QUICHE_PY_MOQT_EVENT_SUBSCRIBE_ERROR;
      event.error_code = static_cast<uint32_t>(error.error_code);
      event.reason = error.reason_phrase;
    }
    sink_->PushEvent(std::move(event));
  }

  void OnCanAckObjects(moqt::MoqtObjectAckFunction ack_function) override {
    ack_function_ = std::move(ack_function);
  }

  void OnObjectFragment(const moqt::FullTrackName& full_track_name,
                        const moqt::PublishedObjectMetadata& metadata,
                        absl::string_view object, uint64_t offset) override {
    PyMoqtEventData event;
    event.kind = QUICHE_PY_MOQT_EVENT_OBJECT;
    event.track_name = full_track_name.ToString();
    event.group_id = metadata.location.group;
    event.object_id = metadata.location.object;
    event.has_subgroup = metadata.subgroup.has_value();
    event.subgroup_id = metadata.subgroup.value_or(0);
    event.offset = offset;
    event.payload.assign(object.data(), object.size());
    event.publisher_priority = metadata.publisher_priority;
    event.status = static_cast<int>(metadata.status);
    sink_->PushEvent(std::move(event));
  }

  void OnPublishDone(moqt::FullTrackName full_track_name) override {
    PyMoqtEventData event;
    event.kind = QUICHE_PY_MOQT_EVENT_PUBLISH_DONE;
    event.track_name = full_track_name.ToString();
    sink_->PushEvent(std::move(event));
  }

  void OnMalformedTrack(
      const moqt::FullTrackName& full_track_name) override {
    PyMoqtEventData event;
    event.kind = QUICHE_PY_MOQT_EVENT_MALFORMED_TRACK;
    event.track_name = full_track_name.ToString();
    sink_->PushEvent(std::move(event));
  }

  void OnStreamFin(const moqt::FullTrackName& full_track_name,
                   moqt::DataStreamIndex stream) override {
    PyMoqtEventData event;
    event.kind = QUICHE_PY_MOQT_EVENT_STREAM_FIN;
    event.track_name = full_track_name.ToString();
    event.group_id = stream.group;
    event.subgroup_id = stream.subgroup;
    event.has_subgroup = true;
    event.fin = true;
    sink_->PushEvent(std::move(event));
  }

  void OnStreamReset(const moqt::FullTrackName& full_track_name,
                     moqt::DataStreamIndex stream) override {
    PyMoqtEventData event;
    event.kind = QUICHE_PY_MOQT_EVENT_STREAM_RESET;
    event.track_name = full_track_name.ToString();
    event.group_id = stream.group;
    event.subgroup_id = stream.subgroup;
    event.has_subgroup = true;
    sink_->PushEvent(std::move(event));
  }

  bool CanAckObjects() const { return ack_function_.has_value(); }

  void AckObject(uint64_t group_id, uint64_t object_id) {
    if (ack_function_.has_value()) {
      (*ack_function_)(group_id, object_id, quic::QuicTimeDelta::Zero());
    }
  }

 private:
  MoqtEventSink* sink_;
  std::optional<moqt::MoqtObjectAckFunction> ack_function_;
};

// クライアントから PUBLISH する 1 トラック分のオブジェクトキャッシュ。
struct PyMoqtObject {
  uint64_t group_id = 0;
  uint64_t object_id = 0;
  std::string payload;
  bool fin = false;
};

class PyMoqtTrackPublisher : public moqt::MoqtTrackPublisher {
 public:
  explicit PyMoqtTrackPublisher(moqt::FullTrackName track_name)
      : track_name_(std::move(track_name)) {}

  const moqt::FullTrackName& GetTrackName() const override {
    return track_name_;
  }

  void AddObject(uint64_t group_id, uint64_t object_id, std::string payload,
                 bool fin) {
    PyMoqtObject object;
    object.group_id = group_id;
    object.object_id = object_id;
    object.payload = std::move(payload);
    object.fin = fin;
    objects_.push_back(std::move(object));
    std::sort(objects_.begin(), objects_.end(),
              [](const PyMoqtObject& left, const PyMoqtObject& right) {
                return std::tie(left.group_id, left.object_id) <
                       std::tie(right.group_id, right.object_id);
              });
    for (moqt::MoqtObjectListener* listener : listeners_) {
      listener->OnNewObjectAvailable(moqt::Location(group_id, object_id),
                                     uint64_t{0},
                                     moqt::kDefaultPublisherPriority);
    }
  }

  std::optional<moqt::PublishedObject> GetCachedObject(
      uint64_t group, std::optional<uint64_t> subgroup, uint64_t min_object,
      uint64_t offset) const override {
    if (offset > 0 || (subgroup.has_value() && *subgroup != 0)) {
      return std::nullopt;
    }
    for (const PyMoqtObject& object : objects_) {
      if (object.group_id < group) {
        continue;
      }
      if (object.group_id == group && object.object_id < min_object) {
        continue;
      }
      moqt::PublishedObject published;
      published.metadata.location =
          moqt::Location(object.group_id, object.object_id);
      published.metadata.subgroup = uint64_t{0};
      published.metadata.payload_length = object.payload.size();
      published.metadata.publisher_priority = moqt::kDefaultPublisherPriority;
      published.metadata.first_object_in_subgroup = true;
      published.metadata.arrival_time = quic::QuicDefaultClock::Get()->Now();
      published.payload.push_back(
          quiche::QuicheMemSlice::Copy(object.payload));
      published.fin_after_this = object.fin;
      return published;
    }
    return std::nullopt;
  }

  void AddObjectListener(
      moqt::MoqtObjectListener* listener,
      const moqt::MessageParameters& /*parameters*/) override {
    listeners_.push_back(listener);
    listener->OnSubscribeAccepted();
    for (const PyMoqtObject& object : objects_) {
      listener->OnNewObjectAvailable(
          moqt::Location(object.group_id, object.object_id), uint64_t{0},
          moqt::kDefaultPublisherPriority);
    }
  }

  void RemoveObjectListener(moqt::MoqtObjectListener* listener) override {
    listeners_.erase(std::remove(listeners_.begin(), listeners_.end(), listener),
                     listeners_.end());
  }

  absl::Status UpdateObjectListener(
      moqt::MoqtObjectListener* listener,
      const moqt::MessageParameters& /*parameters*/) override {
    if (std::find(listeners_.begin(), listeners_.end(), listener) ==
        listeners_.end()) {
      return absl::NotFoundError("listener not found");
    }
    return absl::OkStatus();
  }

  std::optional<moqt::Location> largest_location() const override {
    if (objects_.empty()) {
      return std::nullopt;
    }
    return moqt::Location(objects_.back().group_id, objects_.back().object_id);
  }

  const moqt::TrackProperties& properties() const override {
    return properties_;
  }

  std::optional<quic::QuicTimeDelta> expiration() const override {
    return std::nullopt;
  }

  std::unique_ptr<moqt::MoqtFetchTask> StandaloneFetch(
      moqt::Location /*start*/, moqt::Location /*end*/,
      moqt::MoqtGroupOrder /*order*/,
      moqt::FetchResponseCallback /*callback*/) override {
    return nullptr;
  }

  std::unique_ptr<moqt::MoqtFetchTask> RelativeFetch(
      uint64_t /*group_diff*/, moqt::MoqtGroupOrder /*order*/,
      moqt::FetchResponseCallback /*callback*/) override {
    return nullptr;
  }

  std::unique_ptr<moqt::MoqtFetchTask> AbsoluteFetch(
      uint64_t /*group*/, moqt::MoqtGroupOrder /*order*/,
      moqt::FetchResponseCallback /*callback*/) override {
    return nullptr;
  }

 private:
  moqt::FullTrackName track_name_;
  std::vector<PyMoqtObject> objects_;
  std::vector<moqt::MoqtObjectListener*> listeners_;
  moqt::TrackProperties properties_;
};

class PyMoqtPublisher : public moqt::MoqtPublisher {
 public:
  std::shared_ptr<PyMoqtTrackPublisher> GetOrCreate(
      const moqt::FullTrackName& track_name) {
    auto it = tracks_.find(track_name);
    if (it != tracks_.end()) {
      return it->second;
    }
    auto track = std::make_shared<PyMoqtTrackPublisher>(track_name);
    tracks_.emplace(track_name, track);
    return track;
  }

  std::shared_ptr<PyMoqtTrackPublisher> Find(
      const moqt::FullTrackName& track_name) const {
    auto it = tracks_.find(track_name);
    return it == tracks_.end() ? nullptr : it->second;
  }

  std::shared_ptr<moqt::MoqtTrackPublisher> GetTrack(
      const moqt::FullTrackName& track_name) override {
    return Find(track_name);
  }

 private:
  std::map<moqt::FullTrackName, std::shared_ptr<PyMoqtTrackPublisher>> tracks_;
};

struct PyMoqtFetchEntry {
  uint64_t request_id = 0;
  std::string track_name;
  std::unique_ptr<moqt::MoqtFetchTask> task;
  bool done = false;
};

struct PyMoqtNamespaceEntry {
  std::string prefix;
  std::unique_ptr<moqt::MoqtNamespaceTask> task;
};

// WebTransport 上で MOQT を扱うための HTTP/3 クライアントセッション。
class MoqtSpdyClientSession : public quic::QuicSpdyClientSession {
 public:
  MoqtSpdyClientSession(
      const quic::QuicConfig& config,
      const quic::ParsedQuicVersionVector& supported_versions,
      quic::QuicConnection* connection, const quic::QuicServerId& server_id,
      quic::QuicCryptoClientConfig* crypto_config)
      : quic::QuicSpdyClientSession(
            config, supported_versions, connection, /*visitor=*/nullptr,
            /*writer=*/nullptr, /*migration_helper=*/nullptr,
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

class GoogleQuichePyMoqtClient : public quic::QuicClientBase {
 public:
  GoogleQuichePyMoqtClient(quic::QuicSocketAddress server_address,
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

  MoqtSpdyClientSession* client_session() {
    quic::QuicSession* session = quic::QuicClientBase::session();
    return session == nullptr
               ? nullptr
               : absl::down_cast<MoqtSpdyClientSession*>(session);
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
    return std::make_unique<MoqtSpdyClientSession>(
        *config(), supported_versions, connection, server_id(), crypto_config());
  }
};

}  // namespace

struct quiche_py_moqt_client : public MoqtEventSink {
  quiche_py_moqt_client(std::string host, uint16_t port,
                        std::string server_name, std::string path,
                        bool verify_peer)
      : host_(std::move(host)),
        port_(port),
        server_name_(server_name.empty() ? host_ : std::move(server_name)),
        path_(path.empty() ? "/" : std::move(path)),
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
    client_ = std::make_unique<GoogleQuichePyMoqtClient>(
        server_address_, quic::QuicServerId(server_name_, port_), &send_queue_,
        std::move(proof_verifier));
  }

  ~quiche_py_moqt_client() override {
    // FETCH と NAMESPACE のタスクはセッションより先に破棄する。購読 visitor は
    // セッション破棄時のコールバックで参照されるため、client_ の後に破棄する。
    fetches_.clear();
    namespaces_.clear();
    client_.reset();
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
    moqt_session_ = nullptr;
    moqt_alarm_factory_raw_ = nullptr;
    session_ready_ = false;
    terminated_ = false;
    connect_requested_ = false;
    {
      std::lock_guard<std::mutex> event_lock(event_mutex_);
      events_.clear();
    }
    if (!client_->initialized() && !client_->Initialize()) {
      SetError("failed to initialize the MOQT client");
      return QUICHE_PY_STATUS_ERROR;
    }
    client_->StartConnect();
    if (!client_->connected_or_attempting_connect()) {
      SetErrorIfEmpty("failed to start the MOQT handshake");
      return QUICHE_PY_STATUS_ERROR;
    }
    return QUICHE_PY_STATUS_OK;
  }

  quiche_py_status ReceivePacket(const uint8_t* data, size_t data_len) {
    std::lock_guard<std::mutex> lock(mutex_);
    ResetRuntimeError();
    return ReceivePacketLocked(data, data_len);
  }

  quiche_py_status ReceivePackets(const uint8_t* const* data,
                                  const size_t* data_lengths,
                                  size_t packet_count) {
    std::lock_guard<std::mutex> lock(mutex_);
    ResetRuntimeError();
    if (packet_count > 0 && (data == nullptr || data_lengths == nullptr)) {
      SetError("data and data_lengths must not be null when packet_count > 0");
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    for (size_t index = 0; index < packet_count; ++index) {
      quiche_py_status status =
          ReceivePacketLocked(data[index], data_lengths[index]);
      if (status != QUICHE_PY_STATUS_OK) {
        return status;
      }
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
    if (buffer == nullptr && buffer_len > 0) {
      SetError("buffer must not be null when buffer_len > 0");
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
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
    if (moqt_alarm_factory_raw_ != nullptr) {
      std::optional<quic::QuicTime> moqt_next =
          moqt_alarm_factory_raw_->GetNextUpcomingAlarm();
      if (moqt_next.has_value() && (!next.has_value() || *moqt_next < *next)) {
        next = moqt_next;
      }
    }
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
    quic::QuicTime now = quic::QuicDefaultClock::Get()->Now();
    client_->queue_alarm_factory()->ProcessAlarmsUpTo(now);
    if (moqt_alarm_factory_raw_ != nullptr) {
      moqt_alarm_factory_raw_->ProcessAlarmsUpTo(now);
    }
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
    if (client_ != nullptr) {
      client_->Disconnect();
    }
    if (reason != nullptr && reason[0] != '\0') {
      SetError(reason);
    } else {
      SetError(absl::StrCat("closed with error code ", error_code));
    }
  }

  bool IsConnected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return client_ != nullptr && client_->connected();
  }

  bool IsSessionReady() const { return session_ready_.load(); }

  bool IsTerminated() const { return terminated_.load(); }

  quiche_py_status Connect() {
    std::lock_guard<std::mutex> lock(mutex_);
    ResetRuntimeError();
    if (connect_requested_ && moqt_session_ != nullptr) {
      return QUICHE_PY_STATUS_OK;
    }
    quiche_py_status status = EnsureClient();
    if (status != QUICHE_PY_STATUS_OK) {
      return status;
    }
    MoqtSpdyClientSession* session = client_->client_session();
    if (session == nullptr) {
      SetError("HTTP/3 session is unavailable");
      return QUICHE_PY_STATUS_ERROR;
    }
    if (!client_->connected()) {
      return QUICHE_PY_STATUS_WOULD_BLOCK;
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
    headers[":scheme"] = "https";
    headers[":authority"] = server_name_;
    headers[":path"] = path_;
    headers[":method"] = "CONNECT";
    headers[":protocol"] = "webtransport";
    std::vector<std::string> subprotocols = {std::string(moqt::kDraft18)};
    absl::StatusOr<std::string> protocols =
        webtransport::SerializeSubprotocolRequestHeader(subprotocols);
    if (!protocols.ok()) {
      SetError(protocols.status().ToString());
      return QUICHE_PY_STATUS_ERROR;
    }
    headers["wt-available-protocols"] = *protocols;
    stream->SendRequest(std::move(headers), "", false);

    web_transport_ = stream->web_transport();
    if (web_transport_ == nullptr) {
      SetError("failed to associate a WebTransport session with the request");
      return QUICHE_PY_STATUS_ERROR;
    }

    moqt::MoqtSessionParameters parameters(quic::Perspective::IS_CLIENT);
    parameters.version = std::string(moqt::kDraft18);
    parameters.using_webtrans = true;
    parameters.path = path_;
    parameters.authority = server_name_;
    parameters.support_object_acks = true;

    moqt::MoqtSessionCallbacks callbacks;
    callbacks.session_established_callback = [this] { session_ready_ = true; };
    callbacks.session_terminated_callback = [this](absl::string_view error) {
      terminated_ = true;
      PyMoqtEventData event;
      event.kind = QUICHE_PY_MOQT_EVENT_SESSION_TERMINATED;
      event.reason = std::string(error);
      PushEvent(std::move(event));
    };
    callbacks.goaway_received_callback =
        [this](absl::string_view new_session_uri,
               quic::QuicTimeDelta timeout) {
          PyMoqtEventData event;
          event.kind = QUICHE_PY_MOQT_EVENT_GOAWAY;
          event.new_session_uri = std::string(new_session_uri);
          event.timeout_ms =
              static_cast<uint64_t>(timeout.ToMilliseconds());
          PushEvent(std::move(event));
        };
    callbacks.session_deleted_callback = [this] { moqt_session_ = nullptr; };

    moqt_alarm_factory_ = std::make_unique<quic::QuicQueueAlarmFactory>();
    moqt_alarm_factory_raw_ = moqt_alarm_factory_.get();
    auto moqt_session = std::make_unique<moqt::MoqtSession>(
        web_transport_, parameters, std::move(moqt_alarm_factory_),
        std::move(callbacks));
    moqt_session_ = moqt_session.get();
    moqt_session_->set_publisher(&publisher_);
    web_transport_->SetVisitor(std::move(moqt_session));
    connect_requested_ = true;
    return QUICHE_PY_STATUS_OK;
  }

  quiche_py_status Subscribe(const char* track_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    ResetRuntimeError();
    if (track_name == nullptr || track_name[0] == '\0') {
      SetError("track_name must not be empty");
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    quiche_py_status status = EnsureSessionReady();
    if (status != QUICHE_PY_STATUS_OK) {
      return status;
    }
    absl::StatusOr<moqt::FullTrackName> name =
        moqt::FullTrackName::Parse(track_name);
    if (!name.ok()) {
      SetError(name.status().ToString());
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    auto visitor = std::make_unique<PyMoqtSubscribeVisitor>(this);
    if (!moqt_session_->Subscribe(*name, visitor.get(),
                                  moqt::MessageParameters())) {
      SetError(absl::StrCat("failed to send SUBSCRIBE for ", track_name));
      return QUICHE_PY_STATUS_ERROR;
    }
    subscribers_[name->ToString()] = visitor.get();
    visitors_.push_back(std::move(visitor));
    return QUICHE_PY_STATUS_OK;
  }

  quiche_py_status Unsubscribe(const char* track_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    ResetRuntimeError();
    if (track_name == nullptr || track_name[0] == '\0') {
      SetError("track_name must not be empty");
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    quiche_py_status status = EnsureSessionReady();
    if (status != QUICHE_PY_STATUS_OK) {
      return status;
    }
    absl::StatusOr<moqt::FullTrackName> name =
        moqt::FullTrackName::Parse(track_name);
    if (!name.ok()) {
      SetError(name.status().ToString());
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    moqt_session_->Unsubscribe(*name);
    return QUICHE_PY_STATUS_OK;
  }

  quiche_py_status Publish(const char* track_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    ResetRuntimeError();
    if (track_name == nullptr || track_name[0] == '\0') {
      SetError("track_name must not be empty");
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    quiche_py_status status = EnsureSessionReady();
    if (status != QUICHE_PY_STATUS_OK) {
      return status;
    }
    absl::StatusOr<moqt::FullTrackName> name =
        moqt::FullTrackName::Parse(track_name);
    if (!name.ok()) {
      SetError(name.status().ToString());
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    const std::string name_string = name->ToString();
    std::shared_ptr<PyMoqtTrackPublisher> track =
        publisher_.GetOrCreate(*name);
    bool sent = moqt_session_->Publish(
        track, moqt::MessageParameters(), moqt::TrackProperties(),
        [this, name_string](
            std::variant<moqt::MessageParameters, moqt::MoqtRequestErrorInfo>
                response) {
          PyMoqtEventData event;
          event.track_name = name_string;
          if (std::holds_alternative<moqt::MessageParameters>(response)) {
            event.kind = QUICHE_PY_MOQT_EVENT_PUBLISH_OK;
          } else {
            const moqt::MoqtRequestErrorInfo& error =
                std::get<moqt::MoqtRequestErrorInfo>(response);
            event.kind = QUICHE_PY_MOQT_EVENT_PUBLISH_ERROR;
            event.error_code = static_cast<uint32_t>(error.error_code);
            event.reason = error.reason_phrase;
          }
          PushEvent(std::move(event));
        });
    if (!sent) {
      SetError(absl::StrCat("failed to send PUBLISH for ", track_name));
      return QUICHE_PY_STATUS_WOULD_BLOCK;
    }
    return QUICHE_PY_STATUS_OK;
  }

  quiche_py_status AddObject(const char* track_name, uint64_t group_id,
                             uint64_t object_id, const uint8_t* payload,
                             size_t payload_len, bool fin) {
    std::lock_guard<std::mutex> lock(mutex_);
    ResetRuntimeError();
    if (track_name == nullptr || track_name[0] == '\0') {
      SetError("track_name must not be empty");
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    if (payload == nullptr && payload_len > 0) {
      SetError("payload must not be null when payload_len > 0");
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    // 1 オブジェクトの上限を 64 MiB に制限する。
    if (payload_len > 64 * 1024 * 1024) {
      SetError(absl::StrCat("payload too large: ", payload_len, " bytes"));
      return QUICHE_PY_STATUS_TOO_BIG;
    }
    quiche_py_status status = EnsureSessionReady();
    if (status != QUICHE_PY_STATUS_OK) {
      return status;
    }
    absl::StatusOr<moqt::FullTrackName> name =
        moqt::FullTrackName::Parse(track_name);
    if (!name.ok()) {
      SetError(name.status().ToString());
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    std::shared_ptr<PyMoqtTrackPublisher> track = publisher_.Find(*name);
    if (track == nullptr) {
      SetError(absl::StrCat("track is not published: ", track_name));
      return QUICHE_PY_STATUS_NOT_FOUND;
    }
    std::string data;
    if (payload != nullptr) {
      data.assign(reinterpret_cast<const char*>(payload), payload_len);
    }
    track->AddObject(group_id, object_id, std::move(data), fin);
    return QUICHE_PY_STATUS_OK;
  }

  quiche_py_status AckObject(const char* track_name, uint64_t group_id,
                             uint64_t object_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    ResetRuntimeError();
    if (track_name == nullptr || track_name[0] == '\0') {
      SetError("track_name must not be empty");
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    auto it = subscribers_.find(track_name);
    if (it == subscribers_.end()) {
      SetError(absl::StrCat("subscription not found: ", track_name));
      return QUICHE_PY_STATUS_NOT_FOUND;
    }
    if (!it->second->CanAckObjects()) {
      SetError("OBJECT_ACK is not available for this subscription");
      return QUICHE_PY_STATUS_ERROR;
    }
    it->second->AckObject(group_id, object_id);
    return QUICHE_PY_STATUS_OK;
  }

  quiche_py_status Fetch(const char* track_name, uint64_t start_group,
                         uint64_t start_object, uint64_t end_group,
                         bool has_end_object, uint64_t end_object,
                         uint64_t* request_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    ResetRuntimeError();
    if (request_id == nullptr) {
      SetError("request_id must not be null");
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    if (track_name == nullptr || track_name[0] == '\0') {
      SetError("track_name must not be empty");
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    quiche_py_status status = EnsureSessionReady();
    if (status != QUICHE_PY_STATUS_OK) {
      return status;
    }
    absl::StatusOr<moqt::FullTrackName> name =
        moqt::FullTrackName::Parse(track_name);
    if (!name.ok()) {
      SetError(name.status().ToString());
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    const uint64_t id = next_request_id_++;
    const std::string name_string = name->ToString();
    std::unique_ptr<moqt::MoqtFetchTask> task = moqt_session_->Fetch(
        *name,
        [this, id, name_string](
            std::variant<moqt::FetchOkData, moqt::MoqtRequestErrorInfo>
                response) {
          PyMoqtEventData event;
          event.request_id = id;
          event.track_name = name_string;
          if (std::holds_alternative<moqt::FetchOkData>(response)) {
            event.kind = QUICHE_PY_MOQT_EVENT_FETCH_OK;
          } else {
            const moqt::MoqtRequestErrorInfo& error =
                std::get<moqt::MoqtRequestErrorInfo>(response);
            event.kind = QUICHE_PY_MOQT_EVENT_FETCH_ERROR;
            event.error_code = static_cast<uint32_t>(error.error_code);
            event.reason = error.reason_phrase;
          }
          PushEvent(std::move(event));
        },
        moqt::Location(start_group, start_object), end_group,
        has_end_object ? std::optional<uint64_t>(end_object) : std::nullopt,
        moqt::MessageParameters());
    if (task == nullptr) {
      SetError(absl::StrCat("failed to send FETCH for ", track_name));
      return QUICHE_PY_STATUS_WOULD_BLOCK;
    }
    fetches_.push_back(
        PyMoqtFetchEntry{id, name_string, std::move(task), false});
    *request_id = id;
    return QUICHE_PY_STATUS_OK;
  }

  quiche_py_status PublishNamespace(const char* track_namespace) {
    std::lock_guard<std::mutex> lock(mutex_);
    ResetRuntimeError();
    if (track_namespace == nullptr || track_namespace[0] == '\0') {
      SetError("track_namespace must not be empty");
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    quiche_py_status status = EnsureSessionReady();
    if (status != QUICHE_PY_STATUS_OK) {
      return status;
    }
    absl::StatusOr<moqt::TrackNamespace> ns =
        moqt::TrackNamespace::Parse(track_namespace);
    if (!ns.ok()) {
      SetError(ns.status().ToString());
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    const std::string ns_string = ns->ToString();
    bool sent = moqt_session_->PublishNamespace(
        *ns, moqt::MessageParameters(),
        [this, ns_string](
            std::variant<moqt::MessageParameters, moqt::MoqtRequestErrorInfo>
                response) {
          PyMoqtEventData event;
          event.track_name = ns_string;
          if (std::holds_alternative<moqt::MessageParameters>(response)) {
            event.kind = QUICHE_PY_MOQT_EVENT_PUBLISH_NAMESPACE_OK;
          } else {
            const moqt::MoqtRequestErrorInfo& error =
                std::get<moqt::MoqtRequestErrorInfo>(response);
            event.kind = QUICHE_PY_MOQT_EVENT_PUBLISH_NAMESPACE_ERROR;
            event.error_code = static_cast<uint32_t>(error.error_code);
            event.reason = error.reason_phrase;
          }
          PushEvent(std::move(event));
        },
        [this, ns_string]() {
          PyMoqtEventData event;
          event.kind = QUICHE_PY_MOQT_EVENT_PUBLISH_NAMESPACE_CANCELLED;
          event.track_name = ns_string;
          PushEvent(std::move(event));
        });
    if (!sent) {
      SetError(
          absl::StrCat("failed to send PUBLISH_NAMESPACE for ",
                       track_namespace));
      return QUICHE_PY_STATUS_WOULD_BLOCK;
    }
    return QUICHE_PY_STATUS_OK;
  }

  quiche_py_status PublishNamespaceDone(const char* track_namespace) {
    std::lock_guard<std::mutex> lock(mutex_);
    ResetRuntimeError();
    if (track_namespace == nullptr || track_namespace[0] == '\0') {
      SetError("track_namespace must not be empty");
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    quiche_py_status status = EnsureSessionReady();
    if (status != QUICHE_PY_STATUS_OK) {
      return status;
    }
    absl::StatusOr<moqt::TrackNamespace> ns =
        moqt::TrackNamespace::Parse(track_namespace);
    if (!ns.ok()) {
      SetError(ns.status().ToString());
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    if (!moqt_session_->PublishNamespaceDone(*ns)) {
      SetError(absl::StrCat("failed to send PUBLISH_NAMESPACE_DONE for ",
                            track_namespace));
      return QUICHE_PY_STATUS_NOT_FOUND;
    }
    return QUICHE_PY_STATUS_OK;
  }

  quiche_py_status SubscribeNamespace(const char* prefix) {
    std::lock_guard<std::mutex> lock(mutex_);
    ResetRuntimeError();
    if (prefix == nullptr || prefix[0] == '\0') {
      SetError("prefix must not be empty");
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    quiche_py_status status = EnsureSessionReady();
    if (status != QUICHE_PY_STATUS_OK) {
      return status;
    }
    absl::StatusOr<moqt::TrackNamespace> ns =
        moqt::TrackNamespace::Parse(prefix);
    if (!ns.ok()) {
      SetError(ns.status().ToString());
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    const std::string prefix_string = ns->ToString();
    std::unique_ptr<moqt::MoqtNamespaceTask> task =
        moqt_session_->SubscribeNamespace(
            *ns, moqt::MessageParameters(),
            [this, prefix_string](
                std::variant<moqt::MessageParameters,
                             moqt::MoqtRequestErrorInfo>
                    response) {
              PyMoqtEventData event;
              event.track_name = prefix_string;
              if (std::holds_alternative<moqt::MessageParameters>(response)) {
                event.kind = QUICHE_PY_MOQT_EVENT_SUBSCRIBE_NAMESPACE_OK;
              } else {
                const moqt::MoqtRequestErrorInfo& error =
                    std::get<moqt::MoqtRequestErrorInfo>(response);
                event.kind = QUICHE_PY_MOQT_EVENT_SUBSCRIBE_NAMESPACE_ERROR;
                event.error_code = static_cast<uint32_t>(error.error_code);
                event.reason = error.reason_phrase;
              }
              PushEvent(std::move(event));
            });
    if (task == nullptr) {
      SetError(absl::StrCat("failed to send SUBSCRIBE_NAMESPACE for ", prefix));
      return QUICHE_PY_STATUS_WOULD_BLOCK;
    }
    namespaces_.push_back(
        PyMoqtNamespaceEntry{prefix_string, std::move(task)});
    return QUICHE_PY_STATUS_OK;
  }

  quiche_py_status UnsubscribeNamespace(const char* prefix) {
    std::lock_guard<std::mutex> lock(mutex_);
    ResetRuntimeError();
    if (prefix == nullptr || prefix[0] == '\0') {
      SetError("prefix must not be empty");
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    absl::StatusOr<moqt::TrackNamespace> ns =
        moqt::TrackNamespace::Parse(prefix);
    if (!ns.ok()) {
      SetError(ns.status().ToString());
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    const std::string prefix_string = ns->ToString();
    auto it = std::find_if(
        namespaces_.begin(), namespaces_.end(),
        [&prefix_string](const PyMoqtNamespaceEntry& entry) {
          return entry.prefix == prefix_string;
        });
    if (it == namespaces_.end()) {
      SetError(absl::StrCat("namespace subscription not found: ", prefix));
      return QUICHE_PY_STATUS_NOT_FOUND;
    }
    namespaces_.erase(it);
    return QUICHE_PY_STATUS_OK;
  }

  quiche_py_status TrackStatus(const char* track_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    ResetRuntimeError();
    if (track_name == nullptr || track_name[0] == '\0') {
      SetError("track_name must not be empty");
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    quiche_py_status status = EnsureSessionReady();
    if (status != QUICHE_PY_STATUS_OK) {
      return status;
    }
    absl::StatusOr<moqt::FullTrackName> name =
        moqt::FullTrackName::Parse(track_name);
    if (!name.ok()) {
      SetError(name.status().ToString());
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    const std::string name_string = name->ToString();
    bool sent = moqt_session_->TrackStatus(
        *name, moqt::MessageParameters(),
        [this, name_string](
            std::variant<moqt::TrackStatusOkData, moqt::MoqtRequestErrorInfo>
                response) {
          PyMoqtEventData event;
          event.track_name = name_string;
          if (std::holds_alternative<moqt::TrackStatusOkData>(response)) {
            event.kind = QUICHE_PY_MOQT_EVENT_TRACK_STATUS_OK;
          } else {
            const moqt::MoqtRequestErrorInfo& error =
                std::get<moqt::MoqtRequestErrorInfo>(response);
            event.kind = QUICHE_PY_MOQT_EVENT_TRACK_STATUS_ERROR;
            event.error_code = static_cast<uint32_t>(error.error_code);
            event.reason = error.reason_phrase;
          }
          PushEvent(std::move(event));
        });
    if (!sent) {
      SetError(absl::StrCat("failed to send TRACK_STATUS for ", track_name));
      return QUICHE_PY_STATUS_WOULD_BLOCK;
    }
    return QUICHE_PY_STATUS_OK;
  }

  bool PollEvent(quiche_py_moqt_event* event) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      DrainAsyncTasksLocked();
    }
    std::lock_guard<std::mutex> lock(event_mutex_);
    if (event == nullptr || events_.empty()) {
      return false;
    }
    last_event_ = std::move(events_.front());
    events_.pop_front();
    FillEvent(last_event_, event);
    return true;
  }

  void PushEvent(PyMoqtEventData event) override {
    std::lock_guard<std::mutex> lock(event_mutex_);
    events_.push_back(std::move(event));
  }

  const char* last_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    static thread_local std::string buffer;
    buffer = last_error_;
    return buffer.c_str();
  }

 private:
  void DrainAsyncTasksLocked() {
    DrainFetchTasksLocked();
    DrainNamespaceTasksLocked();
  }

  void DrainFetchTasksLocked() {
    for (auto it = fetches_.begin(); it != fetches_.end();) {
      if (it->done || it->task == nullptr) {
        it = fetches_.erase(it);
        continue;
      }
      bool keep = true;
      for (int iteration = 0; iteration < 1024; ++iteration) {
        moqt::PublishedObject object;
        moqt::MoqtFetchTask::GetNextObjectResult result =
            it->task->GetNextObject(object);
        if (result == moqt::MoqtFetchTask::kSuccess) {
          PyMoqtEventData event;
          event.kind = QUICHE_PY_MOQT_EVENT_OBJECT;
          event.request_id = it->request_id;
          event.track_name = it->track_name;
          event.group_id = object.metadata.location.group;
          event.object_id = object.metadata.location.object;
          event.has_subgroup = object.metadata.subgroup.has_value();
          event.subgroup_id = object.metadata.subgroup.value_or(0);
          event.fin = object.fin_after_this;
          event.publisher_priority = object.metadata.publisher_priority;
          event.status = static_cast<int>(object.metadata.status);
          for (const quiche::QuicheMemSlice& slice : object.payload) {
            event.payload.append(slice.data(), slice.length());
          }
          PushEvent(std::move(event));
          continue;
        }
        if (result == moqt::MoqtFetchTask::kPending) {
          break;
        }
        if (result == moqt::MoqtFetchTask::kEof) {
          PyMoqtEventData event;
          event.kind = QUICHE_PY_MOQT_EVENT_FETCH_DONE;
          event.request_id = it->request_id;
          event.track_name = it->track_name;
          PushEvent(std::move(event));
          it->done = true;
          keep = false;
          break;
        }
        // kError
        PyMoqtEventData event;
        event.kind = QUICHE_PY_MOQT_EVENT_FETCH_ERROR;
        event.request_id = it->request_id;
        event.track_name = it->track_name;
        event.reason = it->task->GetStatus().ToString();
        PushEvent(std::move(event));
        it->done = true;
        keep = false;
        break;
      }
      if (keep) {
        ++it;
      } else {
        it = fetches_.erase(it);
      }
    }
  }

  void DrainNamespaceTasksLocked() {
    for (auto it = namespaces_.begin(); it != namespaces_.end();) {
      if (it->task == nullptr) {
        it = namespaces_.erase(it);
        continue;
      }
      bool keep = true;
      for (int iteration = 0; iteration < 1024; ++iteration) {
        moqt::TrackNamespace suffix;
        moqt::TransactionType transaction = moqt::TransactionType::kAdd;
        moqt::GetNextResult result =
            it->task->GetNextSuffix(suffix, transaction);
        if (result == moqt::GetNextResult::kSuccess) {
          absl::StatusOr<moqt::TrackNamespace> full =
              it->task->prefix().AddSuffix(suffix);
          if (!full.ok()) {
            SetError(full.status().ToString());
            keep = false;
            break;
          }
          PyMoqtEventData event;
          event.kind = transaction == moqt::TransactionType::kAdd
                           ? QUICHE_PY_MOQT_EVENT_NAMESPACE_ADDED
                           : QUICHE_PY_MOQT_EVENT_NAMESPACE_DONE;
          event.track_name = full->ToString();
          PushEvent(std::move(event));
          continue;
        }
        if (result == moqt::GetNextResult::kPending) {
          break;
        }
        if (result == moqt::GetNextResult::kEof) {
          keep = false;
          break;
        }
        // kError
        PyMoqtEventData event;
        event.kind = QUICHE_PY_MOQT_EVENT_SUBSCRIBE_NAMESPACE_ERROR;
        event.track_name = it->prefix;
        std::optional<webtransport::StreamErrorCode> status =
            it->task->GetStatus();
        event.error_code = status.value_or(0);
        event.reason = "namespace subscription failed";
        PushEvent(std::move(event));
        keep = false;
        break;
      }
      if (keep) {
        ++it;
      } else {
        it = namespaces_.erase(it);
      }
    }
  }

  quiche_py_status EnsureClient() {
    if (client_ == nullptr) {
      SetErrorIfEmpty("MOQT client is not available");
      return QUICHE_PY_STATUS_ERROR;
    }
    return QUICHE_PY_STATUS_OK;
  }

  quiche_py_status EnsureSessionReady() {
    quiche_py_status status = EnsureClient();
    if (status != QUICHE_PY_STATUS_OK) {
      return status;
    }
    if (!session_ready_.load() || moqt_session_ == nullptr) {
      SetError("MOQT session is not ready");
      return QUICHE_PY_STATUS_WOULD_BLOCK;
    }
    return QUICHE_PY_STATUS_OK;
  }

  quiche_py_status ReceivePacketLocked(const uint8_t* data, size_t data_len) {
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

  void PopulateClosedErrorIfNeeded() {
    if (!last_error_.empty()) {
      return;
    }
    if (terminated_.load()) {
      SetErrorIfEmpty("MOQT session terminated");
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

  static void FillEvent(const PyMoqtEventData& source,
                        quiche_py_moqt_event* destination) {
    *destination = {};
    destination->kind = source.kind;
    destination->request_id = source.request_id;
    destination->track_name = source.track_name.c_str();
    destination->track_name_len = source.track_name.size();
    destination->group_id = source.group_id;
    destination->object_id = source.object_id;
    destination->subgroup_id = source.subgroup_id;
    destination->has_subgroup = source.has_subgroup;
    destination->offset = source.offset;
    destination->fin = source.fin;
    destination->payload = source.payload.empty()
                               ? nullptr
                               : reinterpret_cast<const uint8_t*>(
                                     source.payload.data());
    destination->payload_len = source.payload.size();
    destination->publisher_priority = source.publisher_priority;
    destination->status = source.status;
    destination->error_code = source.error_code;
    destination->reason = source.reason.c_str();
    destination->reason_len = source.reason.size();
    destination->new_session_uri = source.new_session_uri.c_str();
    destination->new_session_uri_len = source.new_session_uri.size();
    destination->timeout_ms = source.timeout_ms;
  }

  std::string host_;
  uint16_t port_;
  std::string server_name_;
  std::string path_;
  bool verify_peer_;
  std::string last_error_;
  quic::QuicSocketAddress server_address_;
  SendQueue send_queue_;
  PyMoqtPublisher publisher_;
  std::unique_ptr<GoogleQuichePyMoqtClient> client_;
  quic::WebTransportHttp3* web_transport_ = nullptr;
  moqt::MoqtSession* moqt_session_ = nullptr;
  std::unique_ptr<quic::QuicQueueAlarmFactory> moqt_alarm_factory_;
  quic::QuicQueueAlarmFactory* moqt_alarm_factory_raw_ = nullptr;
  std::vector<std::unique_ptr<PyMoqtSubscribeVisitor>> visitors_;
  std::map<std::string, PyMoqtSubscribeVisitor*> subscribers_;
  std::vector<PyMoqtFetchEntry> fetches_;
  std::vector<PyMoqtNamespaceEntry> namespaces_;
  uint64_t next_request_id_ = 1;
  std::atomic<bool> session_ready_{false};
  std::atomic<bool> terminated_{false};
  bool connect_requested_ = false;
  std::deque<PyMoqtEventData> events_;
  PyMoqtEventData last_event_;
  mutable std::mutex mutex_;
  mutable std::mutex event_mutex_;
};

extern "C" {

quiche_py_moqt_client* quiche_py_moqt_client_create(
    const quiche_py_moqt_client_options* options) {
  if (options == nullptr) {
    return nullptr;
  }
  return new quiche_py_moqt_client(
      options->host == nullptr ? "" : options->host, options->port,
      options->server_name == nullptr ? "" : options->server_name,
      options->path == nullptr ? "" : options->path, options->verify_peer);
}

void quiche_py_moqt_client_destroy(quiche_py_moqt_client* client) {
  delete client;
}

quiche_py_status quiche_py_moqt_client_start(quiche_py_moqt_client* client) {
  return client == nullptr ? QUICHE_PY_STATUS_INVALID_ARGUMENT
                           : client->Start();
}

quiche_py_status quiche_py_moqt_client_receive_packet(
    quiche_py_moqt_client* client, const uint8_t* data, size_t data_len) {
  return client == nullptr ? QUICHE_PY_STATUS_INVALID_ARGUMENT
                           : client->ReceivePacket(data, data_len);
}

quiche_py_status quiche_py_moqt_client_receive_packets(
    quiche_py_moqt_client* client, const uint8_t* const* data,
    const size_t* data_lengths, size_t packet_count) {
  return client == nullptr
             ? QUICHE_PY_STATUS_INVALID_ARGUMENT
             : client->ReceivePackets(data, data_lengths, packet_count);
}

quiche_py_status quiche_py_moqt_client_next_send(
    quiche_py_moqt_client* client, uint8_t* buffer, size_t buffer_len,
    size_t* written) {
  return client == nullptr
             ? QUICHE_PY_STATUS_INVALID_ARGUMENT
             : client->NextSend(buffer, buffer_len, written);
}

quiche_py_status quiche_py_moqt_client_next_timeout_ms(
    quiche_py_moqt_client* client, uint64_t* timeout_ms, bool* has_timeout) {
  return client == nullptr
             ? QUICHE_PY_STATUS_INVALID_ARGUMENT
             : client->NextTimeoutMs(timeout_ms, has_timeout);
}

quiche_py_status quiche_py_moqt_client_handle_timeout(
    quiche_py_moqt_client* client) {
  return client == nullptr ? QUICHE_PY_STATUS_INVALID_ARGUMENT
                           : client->HandleTimeout();
}

void quiche_py_moqt_client_close(quiche_py_moqt_client* client,
                                 uint32_t error_code, const char* reason) {
  if (client != nullptr) {
    client->Close(error_code, reason);
  }
}

bool quiche_py_moqt_client_is_connected(
    const quiche_py_moqt_client* client) {
  return client != nullptr && client->IsConnected();
}

bool quiche_py_moqt_client_is_session_ready(
    const quiche_py_moqt_client* client) {
  return client != nullptr && client->IsSessionReady();
}

bool quiche_py_moqt_client_is_terminated(
    const quiche_py_moqt_client* client) {
  return client != nullptr && client->IsTerminated();
}

quiche_py_status quiche_py_moqt_client_connect(
    quiche_py_moqt_client* client) {
  return client == nullptr ? QUICHE_PY_STATUS_INVALID_ARGUMENT
                           : client->Connect();
}

quiche_py_status quiche_py_moqt_client_subscribe(
    quiche_py_moqt_client* client, const char* track_name) {
  return client == nullptr ? QUICHE_PY_STATUS_INVALID_ARGUMENT
                           : client->Subscribe(track_name);
}

quiche_py_status quiche_py_moqt_client_unsubscribe(
    quiche_py_moqt_client* client, const char* track_name) {
  return client == nullptr ? QUICHE_PY_STATUS_INVALID_ARGUMENT
                           : client->Unsubscribe(track_name);
}

bool quiche_py_moqt_client_poll_event(quiche_py_moqt_client* client,
                                      quiche_py_moqt_event* event) {
  return client != nullptr && client->PollEvent(event);
}

const char* quiche_py_moqt_client_last_error(
    const quiche_py_moqt_client* client) {
  return client == nullptr ? "" : client->last_error();
}

}  // extern "C"

quiche_py_status quiche_py_moqt_client_publish(quiche_py_moqt_client* client,
                                                const char* track_name) {
  return client == nullptr ? QUICHE_PY_STATUS_INVALID_ARGUMENT
                           : client->Publish(track_name);
}

quiche_py_status quiche_py_moqt_client_add_object(
    quiche_py_moqt_client* client, const char* track_name, uint64_t group_id,
    uint64_t object_id, const uint8_t* payload, size_t payload_len, bool fin) {
  return client == nullptr
             ? QUICHE_PY_STATUS_INVALID_ARGUMENT
             : client->AddObject(track_name, group_id, object_id, payload,
                                 payload_len, fin);
}

quiche_py_status quiche_py_moqt_client_ack_object(
    quiche_py_moqt_client* client, const char* track_name, uint64_t group_id,
    uint64_t object_id) {
  return client == nullptr
             ? QUICHE_PY_STATUS_INVALID_ARGUMENT
             : client->AckObject(track_name, group_id, object_id);
}

quiche_py_status quiche_py_moqt_client_fetch(
    quiche_py_moqt_client* client, const char* track_name,
    uint64_t start_group, uint64_t start_object, uint64_t end_group,
    bool has_end_object, uint64_t end_object, uint64_t* request_id) {
  return client == nullptr
             ? QUICHE_PY_STATUS_INVALID_ARGUMENT
             : client->Fetch(track_name, start_group, start_object, end_group,
                             has_end_object, end_object, request_id);
}

quiche_py_status quiche_py_moqt_client_publish_namespace(
    quiche_py_moqt_client* client, const char* track_namespace) {
  return client == nullptr ? QUICHE_PY_STATUS_INVALID_ARGUMENT
                           : client->PublishNamespace(track_namespace);
}

quiche_py_status quiche_py_moqt_client_publish_namespace_done(
    quiche_py_moqt_client* client, const char* track_namespace) {
  return client == nullptr ? QUICHE_PY_STATUS_INVALID_ARGUMENT
                           : client->PublishNamespaceDone(track_namespace);
}

quiche_py_status quiche_py_moqt_client_subscribe_namespace(
    quiche_py_moqt_client* client, const char* prefix) {
  return client == nullptr ? QUICHE_PY_STATUS_INVALID_ARGUMENT
                           : client->SubscribeNamespace(prefix);
}

quiche_py_status quiche_py_moqt_client_unsubscribe_namespace(
    quiche_py_moqt_client* client, const char* prefix) {
  return client == nullptr ? QUICHE_PY_STATUS_INVALID_ARGUMENT
                           : client->UnsubscribeNamespace(prefix);
}

quiche_py_status quiche_py_moqt_client_track_status(
    quiche_py_moqt_client* client, const char* track_name) {
  return client == nullptr ? QUICHE_PY_STATUS_INVALID_ARGUMENT
                           : client->TrackStatus(track_name);
}
