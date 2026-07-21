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

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "quiche/common/quiche_mem_slice.h"
#include "quiche/quic/core/quic_config.h"
#include "quiche/quic/core/quic_connection.h"
#include "quiche/quic/core/quic_constants.h"
#include "quiche/quic/core/quic_crypto_client_stream.h"
#include "quiche/quic/core/quic_default_clock.h"
#include "quiche/quic/core/quic_default_connection_helper.h"
#include "quiche/quic/core/quic_generic_session.h"
#include "quiche/quic/core/quic_packet_writer.h"
#include "quiche/quic/core/quic_packets.h"
#include "quiche/quic/core/quic_queue_alarm_factory.h"
#include "quiche/quic/core/quic_server_id.h"
#include "quiche/quic/core/quic_time.h"
#include "quiche/quic/core/quic_types.h"
#include "quiche/quic/core/quic_versions.h"
#include "quiche/quic/platform/api/quic_ip_address.h"
#include "quiche/quic/platform/api/quic_socket_address.h"
#include "quiche/quic/platform/api/quic_default_proof_providers.h"
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

const char* SupportedVersionsSample() {
  // 一度だけ初期化して以降不変。C++11 の magic statics により
  // 初期化はスレッドセーフであり、以降は読み取りのみで競合しない。
  static const std::string storage = [] {
    quic::ParsedQuicVersionVector versions = quic::AllSupportedVersions();
    if (versions.empty()) {
      return std::string("none");
    }
    return quic::ParsedQuicVersionToString(versions[0]);
  }();
  return storage.c_str();
}

class GoogleQuichePySessionVisitor : public webtransport::SessionVisitor {
 public:
  void Reset() {
    session_ready_ = false;
    session_closed_ = false;
    close_error_code_ = 0;
    close_error_message_.clear();
    received_datagrams_.clear();
  }

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
  const std::string& close_error_message() const { return close_error_message_; }

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

class GoogleQuichePyTransportClient : public quic::QuicClientBase {
 public:
  GoogleQuichePyTransportClient(
      quic::QuicSocketAddress server_address, quic::QuicServerId server_id,
      std::string alpn, SendQueue* send_queue,
      std::unique_ptr<quic::ProofVerifier> proof_verifier,
      GoogleQuichePySessionVisitor* session_visitor)
      : quic::QuicClientBase(
            server_id, quic::GetQuicVersionsForGenericSession(),
            quic::QuicConfig(), new quic::QuicDefaultConnectionHelper(),
            new quic::QuicQueueAlarmFactory(),
            std::make_unique<SansIoNetworkHelper>(send_queue),
            std::move(proof_verifier), nullptr),
        session_visitor_(session_visitor),
        alpn_(std::move(alpn)) {
    set_server_address(server_address);
    config()->SetMaxDatagramFrameSizeToSend(quic::kMaxAcceptedDatagramFrameSize);
  }

  quic::QuicGenericClientSession* generic_session() {
    return session() == nullptr
               ? nullptr
               : static_cast<quic::QuicGenericClientSession*>(session());
  }

  const quic::QuicGenericClientSession* generic_session() const {
    return session() == nullptr
               ? nullptr
               : static_cast<const quic::QuicGenericClientSession*>(session());
  }

  // アラームのスケジュール照会と発火に使うキュー式アラームファクトリ。
  quic::QuicQueueAlarmFactory* queue_alarm_factory() {
    return static_cast<quic::QuicQueueAlarmFactory*>(alarm_factory());
  }

  bool EarlyDataAccepted() override { return crypto_stream()->EarlyDataAccepted(); }

  bool ReceivedInchoateReject() override {
    return crypto_stream()->ReceivedInchoateReject();
  }

 protected:
  void InitializeSession() override {
    generic_session()->Initialize();
    generic_session()->CryptoConnect();
  }

  int GetNumSentClientHellosFromSession() override {
    return crypto_stream()->num_sent_client_hellos();
  }

  int GetNumReceivedServerConfigUpdatesFromSession() override {
    return crypto_stream()->num_scup_messages_received();
  }

  std::unique_ptr<quic::QuicSession> CreateQuicClientSession(
      const quic::ParsedQuicVersionVector& /*supported_versions*/,
      quic::QuicConnection* connection) override {
    return std::make_unique<quic::QuicGenericClientSession>(
        connection, /*owns_connection=*/true, this, *config(),
        server_id().host(), server_id().port(), alpn_, session_visitor_,
        /*owns_visitor=*/false, /*datagram_observer=*/nullptr, crypto_config());
  }

  bool HasActiveRequests() override { return connected(); }

 private:
  const quic::QuicCryptoClientStreamBase* crypto_stream() const {
    return static_cast<const quic::QuicCryptoClientStreamBase*>(
        generic_session()->GetCryptoStream());
  }

  GoogleQuichePySessionVisitor* session_visitor_;
  std::string alpn_;
};

quiche_py_status MapDatagramStatus(
    const webtransport::DatagramStatus& status, std::string* error_message) {
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

struct quiche_py_client {
  quiche_py_client(std::string host, uint16_t port,
                          std::string server_name, std::string alpn,
                          bool verify_peer)
      : host_(std::move(host)),
        port_(port),
        server_name_(server_name.empty() ? host_ : std::move(server_name)),
        alpn_(std::move(alpn)),
        verify_peer_(verify_peer) {
    if (host_.empty()) {
      SetError("host must not be empty");
      return;
    }
    // QUIC に汎用の raw ALPN は無い。相手と合意した識別子を呼び出し側が渡す。
    if (alpn_.empty()) {
      SetError("alpn must not be empty");
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

    client_ = std::make_unique<GoogleQuichePyTransportClient>(
        server_address_, quic::QuicServerId(server_name_, port_), alpn_,
        &send_queue_, std::move(proof_verifier), &session_visitor_);
  }

  // ハンドシェイクを開始する (非ブロッキング)。ClientHello は送信キューに積まれ、
  // 呼び出し側が next_send で取り出して送信する。
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

    session_visitor_.Reset();
    if (!client_->initialized() && !client_->Initialize()) {
      SetError("failed to initialize the QUIC client");
      return QUICHE_PY_STATUS_ERROR;
    }

    client_->StartConnect();
    if (!client_->connected_or_attempting_connect()) {
      PopulateClosedErrorIfNeeded();
      SetErrorIfEmpty("failed to start the QUIC handshake");
      return QUICHE_PY_STATUS_ERROR;
    }
    return QUICHE_PY_STATUS_OK;
  }

  // Python から受け取った UDP データグラムを QUIC 状態機械に流し込む。
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

  // 送信キューから 1 パケット取り出す。空なら WOULD_BLOCK を返す。
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

  // 次にアラームが発火するまでの残り時間 (ミリ秒)。アラーム未設定なら
  // has_timeout=false を返す。
  quiche_py_status NextTimeoutMs(uint64_t* timeout_ms,
                                        bool* has_timeout) {
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
    // ミリ秒未満で切り捨てて 0 になり busy-loop しないよう切り上げる。
    *timeout_ms = static_cast<uint64_t>((remaining_us + 999) / 1000);
    *has_timeout = true;
    return QUICHE_PY_STATUS_OK;
  }

  // 期限切れのアラームを発火する。発火によって新たな送信パケットが
  // 積まれる場合があるため、呼び出し側は next_send で drain すること。
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
    quic::QuicGenericClientSession* session = Session();
    if (session == nullptr) {
      return;
    }
    session->CloseSession(error_code, reason == nullptr ? "" : reason);
  }

  // 接続が確立しセッションが利用可能になっているか。
  bool IsConnected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return client_ != nullptr && client_->connected() &&
           session_visitor_.session_ready();
  }

  quiche_py_status OpenStream(bool unidirectional, uint32_t* stream_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    ResetRuntimeError();
    if (stream_id == nullptr) {
      SetError("stream_id must not be null");
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }

    quiche_py_status status = EnsureConnected();
    if (status != QUICHE_PY_STATUS_OK) {
      return status;
    }

    Stream* stream = nullptr;
    quic::QuicGenericClientSession* session = Session();
    if (unidirectional) {
      if (!session->CanOpenNextOutgoingUnidirectionalStream()) {
        return QUICHE_PY_STATUS_WOULD_BLOCK;
      }
      stream = session->OpenOutgoingUnidirectionalStream();
    } else {
      if (!session->CanOpenNextOutgoingBidirectionalStream()) {
        return QUICHE_PY_STATUS_WOULD_BLOCK;
      }
      stream = session->OpenOutgoingBidirectionalStream();
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

    quiche_py_status status = EnsureConnected();
    if (status != QUICHE_PY_STATUS_OK) {
      return status;
    }

    quic::QuicGenericClientSession* session = Session();
    Stream* stream = unidirectional
                         ? session->AcceptIncomingUnidirectionalStream()
                         : session->AcceptIncomingBidirectionalStream();
    if (stream == nullptr) {
      return QUICHE_PY_STATUS_WOULD_BLOCK;
    }

    *stream_id = stream->GetStreamId();
    return QUICHE_PY_STATUS_OK;
  }

  quiche_py_status ReadStream(uint32_t stream_id, uint8_t* buffer,
                                     size_t buffer_len, size_t* bytes_read,
                                     bool* fin) {
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
    if (peek.peeked_data.empty() && peek.fin_next) {
      *fin = stream->SkipBytes(0);
      return QUICHE_PY_STATUS_OK;
    }
    if (buffer_len == 0) {
      SetError("buffer length must be greater than zero");
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }

    Stream::ReadResult result =
        stream->Read(absl::Span<char>(reinterpret_cast<char*>(buffer), buffer_len));
    *bytes_read = result.bytes_read;
    *fin = result.fin;
    if (result.bytes_read == 0 && !result.fin) {
      return QUICHE_PY_STATUS_WOULD_BLOCK;
    }
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
      SetError("writing empty data requires fin=true");
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
    quiche_py_status status = EnsureConnected();
    if (status != QUICHE_PY_STATUS_OK) {
      return status;
    }

    webtransport::DatagramStatus datagram_status =
        Session()->SendOrQueueDatagram(BytesView(data, data_len));
    quiche_py_status mapped =
        MapDatagramStatus(datagram_status, &last_error_);
    if (mapped == QUICHE_PY_STATUS_OK ||
        mapped == QUICHE_PY_STATUS_WOULD_BLOCK) {
      ResetRuntimeError();
    }
    return mapped;
  }

  quiche_py_status PeekDatagramSize(size_t* datagram_size) {
    std::lock_guard<std::mutex> lock(mutex_);
    ResetRuntimeError();
    if (datagram_size == nullptr) {
      SetError("datagram_size must not be null");
      return QUICHE_PY_STATUS_INVALID_ARGUMENT;
    }
    if (!session_visitor_.HasDatagram()) {
      return QUICHE_PY_STATUS_WOULD_BLOCK;
    }
    *datagram_size = session_visitor_.NextDatagramSize();
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
    if (!session_visitor_.HasDatagram()) {
      return QUICHE_PY_STATUS_WOULD_BLOCK;
    }

    size_t datagram_size = session_visitor_.NextDatagramSize();
    if (buffer_len < datagram_size) {
      SetError(absl::StrCat("buffer too small for datagram: need ",
                            datagram_size, " bytes"));
      return QUICHE_PY_STATUS_TOO_BIG;
    }

    std::string datagram = session_visitor_.TakeDatagram();
    if (!datagram.empty()) {
      std::memcpy(buffer, datagram.data(), datagram.size());
    }
    *bytes_read = datagram.size();
    return QUICHE_PY_STATUS_OK;
  }

  size_t MaxDatagramSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const quic::QuicGenericClientSession* session = Session();
    return session == nullptr ? 0
                              : static_cast<size_t>(session->GetMaxDatagramSize());
  }

  // 直近のエラー文字列を返す。返却ポインタは thread_local バッファを指し、
  // 同一スレッドでの次回呼び出しまで有効。呼び出し側で保持する場合はコピーすること。
  const char* last_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    static thread_local std::string buffer;
    buffer = last_error_;
    return buffer.c_str();
  }

 private:
  quic::QuicGenericClientSession* Session() {
    return client_ == nullptr ? nullptr : client_->generic_session();
  }

  const quic::QuicGenericClientSession* Session() const {
    return client_ == nullptr ? nullptr : client_->generic_session();
  }

  Stream* StreamById(uint32_t stream_id) {
    quic::QuicGenericClientSession* session = Session();
    return session == nullptr ? nullptr : session->GetStreamById(stream_id);
  }

  quiche_py_status EnsureClient() {
    if (HasClient()) {
      return QUICHE_PY_STATUS_OK;
    }
    SetErrorIfEmpty("client is not initialized");
    return QUICHE_PY_STATUS_ERROR;
  }

  // is_connected() と同じ条件にする。TLS 完了前 (session_ready 前) の
  // ストリーム / datagram 操作を拒否する。
  quiche_py_status EnsureConnected() {
    quiche_py_status status = EnsureClient();
    if (status != QUICHE_PY_STATUS_OK) {
      return status;
    }
    if (client_->connected() && session_visitor_.session_ready()) {
      return QUICHE_PY_STATUS_OK;
    }

    PopulateClosedErrorIfNeeded();
    SetErrorIfEmpty("connection is not open");
    return QUICHE_PY_STATUS_CLOSED;
  }

  quiche_py_status RequireStream(uint32_t stream_id, Stream** stream) {
    quiche_py_status status = EnsureConnected();
    if (status != QUICHE_PY_STATUS_OK) {
      return status;
    }

    *stream = StreamById(stream_id);
    if (*stream != nullptr) {
      return QUICHE_PY_STATUS_OK;
    }
    return StreamMissingStatus(stream_id);
  }

  quiche_py_status StreamMissingStatus(uint32_t stream_id) {
    PopulateClosedErrorIfNeeded();
    if (Session() == nullptr || !client_->connected()) {
      SetErrorIfEmpty("connection is not open");
      return HasClient() ? QUICHE_PY_STATUS_CLOSED
                         : QUICHE_PY_STATUS_ERROR;
    }
    SetError(absl::StrCat("stream ", stream_id, " is not available"));
    return QUICHE_PY_STATUS_NOT_FOUND;
  }

  void PopulateClosedErrorIfNeeded() {
    if (!last_error_.empty()) {
      return;
    }
    if (session_visitor_.session_closed()) {
      if (!session_visitor_.close_error_message().empty()) {
        last_error_ = session_visitor_.close_error_message();
      } else {
        last_error_ =
            absl::StrCat("session closed (error code ",
                         session_visitor_.close_error_code(), ")");
      }
    }
  }

  bool HasClient() const { return client_ != nullptr; }

  void SetErrorIfEmpty(absl::string_view message) {
    if (last_error_.empty()) {
      last_error_ = std::string(message);
    }
  }

  void SetError(std::string message) { last_error_ = std::move(message); }
  void ResetRuntimeError() {
    if (HasClient()) {
      last_error_.clear();
    }
  }

  std::string host_;
  uint16_t port_;
  std::string server_name_;
  std::string alpn_;
  bool verify_peer_;
  std::string last_error_;
  quic::QuicSocketAddress server_address_;
  // send_queue_ と session_visitor_ は client_ から参照されるため、
  // client_ より前に宣言して client_ より後に破棄されるようにする。
  SendQueue send_queue_;
  GoogleQuichePySessionVisitor session_visitor_;
  std::unique_ptr<GoogleQuichePyTransportClient> client_;
  mutable std::mutex mutex_;
};

namespace {

quiche_py_client* CreateClient(
    const quiche_py_client_options* options) {
  if (options == nullptr) {
    return nullptr;
  }
  return new quiche_py_client(
      options->host == nullptr ? "" : options->host, options->port,
      options->server_name == nullptr ? "" : options->server_name,
      options->alpn == nullptr ? "" : options->alpn, options->verify_peer);
}

void DestroyClient(quiche_py_client* client) { delete client; }

quiche_py_status StartClient(quiche_py_client* client) {
  return client == nullptr ? QUICHE_PY_STATUS_INVALID_ARGUMENT
                           : client->Start();
}

quiche_py_status ReceiveClientPacket(quiche_py_client* client,
                                            const uint8_t* data,
                                            size_t data_len) {
  return client == nullptr ? QUICHE_PY_STATUS_INVALID_ARGUMENT
                           : client->ReceivePacket(data, data_len);
}

quiche_py_status NextClientSend(quiche_py_client* client,
                                       uint8_t* buffer, size_t buffer_len,
                                       size_t* written) {
  return client == nullptr ? QUICHE_PY_STATUS_INVALID_ARGUMENT
                           : client->NextSend(buffer, buffer_len, written);
}

quiche_py_status NextClientTimeoutMs(quiche_py_client* client,
                                            uint64_t* timeout_ms,
                                            bool* has_timeout) {
  return client == nullptr ? QUICHE_PY_STATUS_INVALID_ARGUMENT
                           : client->NextTimeoutMs(timeout_ms, has_timeout);
}

quiche_py_status HandleClientTimeout(quiche_py_client* client) {
  return client == nullptr ? QUICHE_PY_STATUS_INVALID_ARGUMENT
                           : client->HandleTimeout();
}

void CloseClient(quiche_py_client* client, uint32_t error_code,
                 const char* reason) {
  if (client != nullptr) {
    client->Close(error_code, reason);
  }
}

bool ClientIsConnected(const quiche_py_client* client) {
  return client != nullptr && client->IsConnected();
}

quiche_py_status OpenClientStream(quiche_py_client* client,
                                         bool unidirectional,
                                         uint32_t* stream_id) {
  return client == nullptr ? QUICHE_PY_STATUS_INVALID_ARGUMENT
                           : client->OpenStream(unidirectional, stream_id);
}

quiche_py_status AcceptClientStream(quiche_py_client* client,
                                           bool unidirectional,
                                           uint32_t* stream_id) {
  return client == nullptr ? QUICHE_PY_STATUS_INVALID_ARGUMENT
                           : client->AcceptStream(unidirectional, stream_id);
}

quiche_py_status ReadClientStream(quiche_py_client* client,
                                         uint32_t stream_id, uint8_t* buffer,
                                         size_t buffer_len, size_t* bytes_read,
                                         bool* fin) {
  return client == nullptr ? QUICHE_PY_STATUS_INVALID_ARGUMENT
                           : client->ReadStream(stream_id, buffer, buffer_len,
                                                bytes_read, fin);
}

quiche_py_status WriteClientStream(quiche_py_client* client,
                                          uint32_t stream_id,
                                          const uint8_t* data, size_t data_len,
                                          bool fin) {
  return client == nullptr ? QUICHE_PY_STATUS_INVALID_ARGUMENT
                           : client->WriteStream(stream_id, data, data_len, fin);
}

quiche_py_status ResetClientStream(quiche_py_client* client,
                                          uint32_t stream_id,
                                          uint32_t error_code) {
  return client == nullptr ? QUICHE_PY_STATUS_INVALID_ARGUMENT
                           : client->ResetStream(stream_id, error_code);
}

quiche_py_status StopSendingOnStream(quiche_py_client* client,
                                            uint32_t stream_id,
                                            uint32_t error_code) {
  return client == nullptr ? QUICHE_PY_STATUS_INVALID_ARGUMENT
                           : client->StopSending(stream_id, error_code);
}

bool ClientStreamCanWrite(quiche_py_client* client, uint32_t stream_id) {
  return client != nullptr && client->StreamCanWrite(stream_id);
}

bool ClientStreamExists(quiche_py_client* client, uint32_t stream_id) {
  return client != nullptr && client->StreamExists(stream_id);
}

quiche_py_status SendClientDatagram(quiche_py_client* client,
                                           const uint8_t* data,
                                           size_t data_len) {
  return client == nullptr ? QUICHE_PY_STATUS_INVALID_ARGUMENT
                           : client->SendDatagram(data, data_len);
}

quiche_py_status PeekClientDatagramSize(quiche_py_client* client,
                                               size_t* datagram_size) {
  return client == nullptr ? QUICHE_PY_STATUS_INVALID_ARGUMENT
                           : client->PeekDatagramSize(datagram_size);
}

quiche_py_status ReceiveClientDatagram(quiche_py_client* client,
                                              uint8_t* buffer,
                                              size_t buffer_len,
                                              size_t* bytes_read) {
  return client == nullptr ? QUICHE_PY_STATUS_INVALID_ARGUMENT
                           : client->ReceiveDatagram(buffer, buffer_len,
                                                     bytes_read);
}

size_t ClientMaxDatagramSize(quiche_py_client* client) {
  return client == nullptr ? 0 : client->MaxDatagramSize();
}

const char* ClientLastError(const quiche_py_client* client) {
  return client == nullptr ? "" : client->last_error();
}

const quiche_py_api kQuichePyApi = {
    QUICHE_PY_API_VERSION,
    &SupportedVersionsSample,
    &CreateClient,
    &DestroyClient,
    &StartClient,
    &ReceiveClientPacket,
    &NextClientSend,
    &NextClientTimeoutMs,
    &HandleClientTimeout,
    &CloseClient,
    &ClientIsConnected,
    &OpenClientStream,
    &AcceptClientStream,
    &ReadClientStream,
    &WriteClientStream,
    &ResetClientStream,
    &StopSendingOnStream,
    &ClientStreamCanWrite,
    &ClientStreamExists,
    &SendClientDatagram,
    &PeekClientDatagramSize,
    &ReceiveClientDatagram,
    &ClientMaxDatagramSize,
    &ClientLastError,
    &quiche_py_h3_client_create,
    &quiche_py_h3_client_destroy,
    &quiche_py_h3_client_start,
    &quiche_py_h3_client_receive_packet,
    &quiche_py_h3_client_next_send,
    &quiche_py_h3_client_next_timeout_ms,
    &quiche_py_h3_client_handle_timeout,
    &quiche_py_h3_client_close,
    &quiche_py_h3_client_is_connected,
    &quiche_py_h3_client_submit_request,
    &quiche_py_h3_client_take_response,
    &quiche_py_h3_response_destroy,
    &quiche_py_h3_response_stream_id,
    &quiche_py_h3_response_status_code,
    &quiche_py_h3_response_header_count,
    &quiche_py_h3_response_header_at,
    &quiche_py_h3_response_body,
    &quiche_py_h3_client_last_error,
    &quiche_py_wt_client_create,
    &quiche_py_wt_client_destroy,
    &quiche_py_wt_client_start,
    &quiche_py_wt_client_receive_packet,
    &quiche_py_wt_client_next_send,
    &quiche_py_wt_client_next_timeout_ms,
    &quiche_py_wt_client_handle_timeout,
    &quiche_py_wt_client_close,
    &quiche_py_wt_client_is_connected,
    &quiche_py_wt_client_connect_session,
    &quiche_py_wt_client_is_session_ready,
    &quiche_py_wt_client_open_stream,
    &quiche_py_wt_client_accept_stream,
    &quiche_py_wt_client_read_stream,
    &quiche_py_wt_client_write_stream,
    &quiche_py_wt_client_reset_stream,
    &quiche_py_wt_client_stop_sending,
    &quiche_py_wt_client_stream_can_write,
    &quiche_py_wt_client_stream_exists,
    &quiche_py_wt_client_send_datagram,
    &quiche_py_wt_client_peek_datagram_size,
    &quiche_py_wt_client_receive_datagram,
    &quiche_py_wt_client_max_datagram_size,
    &quiche_py_wt_client_last_error,
};

}  // namespace

extern "C" const quiche_py_api* quiche_py_get_api() {
  return &kQuichePyApi;
}
