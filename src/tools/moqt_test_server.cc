// MOQT draft-18 の E2E テスト用サーバー。テストから起動して 1 トラックを配信する。
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "quiche/common/quiche_mem_slice.h"
#include "quiche/quic/core/quic_types.h"
#include "quiche/quic/moqt/moqt_fetch_task.h"
#include "quiche/quic/moqt/moqt_key_value_pair.h"
#include "quiche/quic/moqt/moqt_messages.h"
#include "quiche/quic/moqt/moqt_names.h"
#include "quiche/quic/moqt/moqt_object.h"
#include "quiche/quic/moqt/moqt_publisher.h"
#include "quiche/quic/moqt/moqt_session.h"
#include "quiche/quic/moqt/moqt_session_interface.h"
#include "quiche/quic/moqt/tools/moqt_server.h"
#include "quiche/quic/platform/api/quic_ip_address.h"
#include "quiche/quic/platform/api/quic_socket_address.h"
#include "quiche/quic/test_tools/crypto_test_utils.h"

namespace {

// FETCH 応答として 1 オブジェクトだけ返すタスク。
class SingleObjectFetchTask : public moqt::MoqtFetchTask {
 public:
  explicit SingleObjectFetchTask(moqt::PublishedObject object)
      : object_(std::move(object)) {}

  GetNextObjectResult GetNextObject(moqt::PublishedObject& output) override {
    if (sent_) {
      return kEof;
    }
    output = std::move(object_);
    sent_ = true;
    return kSuccess;
  }

  void SetObjectAvailableCallback(
      moqt::ObjectsAvailableCallback callback) override {
    if (!sent_ && callback != nullptr) {
      callback();
    }
  }

  absl::Status GetStatus() override { return absl::OkStatus(); }

 private:
  moqt::PublishedObject object_;
  bool sent_ = false;
};

// 受信した PUBLISH をすべて受け入れる visitor。
class AcceptAllSubscribeVisitor : public moqt::SubscribeVisitor {
 public:
  void OnReply(const moqt::FullTrackName&,
               std::variant<moqt::SubscribeOkData,
                            moqt::MoqtRequestErrorInfo>) override {}
  void OnCanAckObjects(moqt::MoqtObjectAckFunction /*ack_function*/) override {}
  void OnObjectFragment(const moqt::FullTrackName&,
                        const moqt::PublishedObjectMetadata&,
                        absl::string_view, uint64_t) override {}
  void OnPublishDone(moqt::FullTrackName) override {}
  void OnMalformedTrack(const moqt::FullTrackName&) override {}
  void OnStreamFin(const moqt::FullTrackName&,
                   moqt::DataStreamIndex) override {}
  void OnStreamReset(const moqt::FullTrackName&,
                     moqt::DataStreamIndex) override {}
};

// SUBSCRIBE_NAMESPACE に対して 1 個のトラックを返すタスク。
class TestNamespaceTask : public moqt::MoqtNamespaceTask {
 public:
  explicit TestNamespaceTask(moqt::TrackNamespace prefix)
      : prefix_(std::move(prefix)) {}

  void SetObjectsAvailableCallback(
      moqt::ObjectsAvailableCallback callback) override {
    if (callback != nullptr) {
      callback();
    }
  }

  moqt::GetNextResult GetNextSuffix(
      moqt::TrackNamespace& suffix,
      moqt::TransactionType& type) override {
    if (sent_) {
      return moqt::GetNextResult::kEof;
    }
    suffix = moqt::TrackNamespace({"track"});
    type = moqt::TransactionType::kAdd;
    sent_ = true;
    return moqt::GetNextResult::kSuccess;
  }

  std::optional<webtransport::StreamErrorCode> GetStatus() override {
    return std::nullopt;
  }

  void Update(const moqt::MessageParameters&,
              moqt::MoqtResponseCallback response_callback) override {
    if (response_callback != nullptr) {
      std::move(response_callback)(moqt::MessageParameters());
    }
  }

  const moqt::TrackNamespace& prefix() override { return prefix_; }

 private:
  moqt::TrackNamespace prefix_;
  bool sent_ = false;
};

// テスト用に 1 オブジェクトだけを持つトラック。
class StaticTrackPublisher : public moqt::MoqtTrackPublisher {
 public:
  StaticTrackPublisher(std::string payload, std::string track_name,
                       bool notify_objects = true)
      : payload_(std::move(payload)),
        track_name_(ParseTrackName(track_name)),
        notify_objects_(notify_objects) {
    metadata_.location = moqt::Location(0, 0);
    metadata_.subgroup = 0;
    metadata_.publisher_priority = moqt::kDefaultPublisherPriority;
    metadata_.payload_length = payload_.size();
    metadata_.first_object_in_subgroup = true;
  }

  const moqt::FullTrackName& GetTrackName() const override {
    return track_name_;
  }

  std::optional<moqt::PublishedObject> GetCachedObject(
      uint64_t group, std::optional<uint64_t> subgroup, uint64_t min_object,
      uint64_t offset) const override {
    if (group != 0 || min_object > 0 || offset > 0) {
      return std::nullopt;
    }
    if (subgroup.has_value() && *subgroup != 0) {
      return std::nullopt;
    }
    moqt::PublishedObject object;
    object.metadata = metadata_;
    object.payload.push_back(quiche::QuicheMemSlice::Copy(payload_));
    object.fin_after_this = true;
    return object;
  }

  void AddObjectListener(moqt::MoqtObjectListener* listener,
                         const moqt::MessageParameters& /*parameters*/) override {
    listener_ = listener;
    listener_->OnSubscribeAccepted();
    if (notify_objects_) {
      listener_->OnNewObjectAvailable(metadata_.location, metadata_.subgroup,
                                      metadata_.publisher_priority);
    }
  }

  void RemoveObjectListener(moqt::MoqtObjectListener* listener) override {
    if (listener_ == listener) {
      listener_ = nullptr;
    }
  }

  absl::Status UpdateObjectListener(
      moqt::MoqtObjectListener* listener,
      const moqt::MessageParameters& /*parameters*/) override {
    if (listener_ != listener) {
      return absl::NotFoundError("listener not found");
    }
    return absl::OkStatus();
  }

  std::optional<moqt::Location> largest_location() const override {
    return metadata_.location;
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
      moqt::FetchResponseCallback callback) override {
    moqt::FetchOkData response;
    response.end_of_track = true;
    response.end_location = metadata_.location;
    response.properties = properties_;
    std::move(callback)(std::move(response));

    moqt::PublishedObject object;
    object.metadata = metadata_;
    object.payload.push_back(quiche::QuicheMemSlice::Copy(payload_));
    object.fin_after_this = true;
    return std::make_unique<SingleObjectFetchTask>(std::move(object));
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
  static moqt::FullTrackName ParseTrackName(const std::string& name) {
    absl::StatusOr<moqt::FullTrackName> parsed = moqt::FullTrackName::Parse(name);
    if (!parsed.ok()) {
      std::cerr << "invalid track name: " << name << std::endl;
      std::exit(1);
    }
    return *std::move(parsed);
  }

  std::string payload_;
  moqt::FullTrackName track_name_;
  moqt::PublishedObjectMetadata metadata_;
  moqt::TrackProperties properties_;
  moqt::MoqtObjectListener* listener_ = nullptr;
  bool notify_objects_ = true;
};

class StaticPublisher : public moqt::MoqtPublisher {
 public:
  StaticPublisher(std::shared_ptr<StaticTrackPublisher> track,
                  std::shared_ptr<StaticTrackPublisher> status_track)
      : track_(std::move(track)), status_track_(std::move(status_track)) {}

  std::shared_ptr<moqt::MoqtTrackPublisher> GetTrack(
      const moqt::FullTrackName& track_name) override {
    if (track_name == track_->GetTrackName()) {
      return track_;
    }
    if (track_name == status_track_->GetTrackName()) {
      return status_track_;
    }
    return nullptr;
  }

 private:
  std::shared_ptr<StaticTrackPublisher> track_;
  std::shared_ptr<StaticTrackPublisher> status_track_;
};

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: moqt_test_server <port>" << std::endl;
    return 1;
  }
  const int port = std::atoi(argv[1]);
  if (port <= 0 || port > 65535) {
    std::cerr << "invalid port: " << argv[1] << std::endl;
    return 1;
  }

  auto track = std::make_shared<StaticTrackPublisher>("moqt-object-payload",
                                                      "test--track");
  auto status_track = std::make_shared<StaticTrackPublisher>(
      "", "test--status", /*notify_objects=*/false);
  auto publisher = std::make_shared<StaticPublisher>(track, status_track);

  moqt::MoqtSessionParameters parameters(quic::Perspective::IS_SERVER);
  parameters.version = std::string(moqt::kDraft18);
  parameters.using_webtrans = true;
  parameters.support_object_acks = true;

  moqt::MoqtServer server(
      quic::test::crypto_test_utils::ProofSourceForTesting(),
      [publisher](absl::string_view /*path*/)
          -> absl::StatusOr<moqt::MoqtConfigureSessionCallback> {
        return moqt::MoqtConfigureSessionCallback(
            [publisher](moqt::MoqtSession* session) {
              session->set_publisher(publisher.get());
              session->callbacks().incoming_publish_callback =
                  [](const moqt::FullTrackName&,
                     const moqt::MessageParameters&,
                     const moqt::TrackProperties&,
                     moqt::MoqtResponseCallback callback)
                  -> moqt::SubscribeVisitor* {
                if (callback != nullptr) {
                  std::move(callback)(moqt::MessageParameters());
                }
                return new AcceptAllSubscribeVisitor();
              };
              session->callbacks().incoming_publish_namespace_callback =
                  [](const moqt::TrackNamespace&,
                     const moqt::MessageParameters*,
                     moqt::MoqtResponseCallback callback) {
                    if (callback != nullptr) {
                      std::move(callback)(moqt::MessageParameters());
                    }
                  };
              session->callbacks().incoming_subscribe_namespace_callback =
                  [](const moqt::TrackNamespace& prefix,
                     const moqt::MessageParameters&,
                     moqt::MoqtResponseCallback callback)
                  -> std::unique_ptr<moqt::MoqtNamespaceTask> {
                if (callback != nullptr) {
                  std::move(callback)(moqt::MessageParameters());
                }
                return std::make_unique<TestNamespaceTask>(prefix);
              };
            });
      },
      parameters);

  absl::Status status = server.CreateUDPSocketAndListen(
      quic::QuicSocketAddress(quic::QuicIpAddress::Loopback4(), port));
  if (!status.ok()) {
    std::cerr << "failed to listen: " << status << std::endl;
    return 1;
  }
  std::cout << "READY " << port << std::endl;
  std::cout.flush();
  server.HandleEventsForever();
  return 0;
}
