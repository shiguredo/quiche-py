/* quiche_glue.cc / http3 / wt で共有する sans-I/O インフラ。 */
#pragma once

#include <deque>
#include <optional>
#include <string>

#include "quiche/quic/core/quic_constants.h"
#include "quiche/quic/core/quic_packet_writer.h"
#include "quiche/quic/platform/api/quic_ip_address.h"
#include "quiche/quic/platform/api/quic_socket_address.h"
#include "quiche/quic/tools/quic_client_base.h"

namespace quiche_py {

// C++ 側が QUIC 状態機械から吐き出した送信パケットを一時的に保持するキュー。
// クライアントは常に同一サーバー宛のため、宛先は持たずバイト列のみ積む。
struct SendQueue {
  std::deque<std::string> packets;
};

// 送信を I/O せず SendQueue に積むだけの PassThrough な QuicPacketWriter。
// 実際の UDP 送信は Python 側が行う (sans-I/O)。
class BufferingPacketWriter : public quic::QuicPacketWriter {
 public:
  explicit BufferingPacketWriter(SendQueue* queue) : queue_(queue) {}

  quic::WriteResult WritePacket(
      const char* buffer, size_t buf_len, const quic::QuicIpAddress& /*self*/,
      const quic::QuicSocketAddress& /*peer*/,
      quic::PerPacketOptions* /*options*/,
      const quic::QuicPacketWriterParams& /*params*/) override {
    queue_->packets.emplace_back(buffer, buf_len);
    return quic::WriteResult(quic::WRITE_STATUS_OK, static_cast<int>(buf_len));
  }

  bool IsWriteBlocked() const override { return false; }
  void SetWritable() override {}
  std::optional<int> MessageTooBigErrorCode() const override {
    return std::nullopt;
  }
  quic::QuicByteCount GetMaxPacketSize(
      const quic::QuicSocketAddress& /*peer_address*/) const override {
    return quic::kMaxOutgoingPacketSize;
  }
  bool SupportsReleaseTime() const override { return false; }
  bool IsBatchMode() const override { return false; }
  bool SupportsEcn() const override { return false; }
  quic::QuicPacketBuffer GetNextWriteLocation(
      const quic::QuicIpAddress& /*self_address*/,
      const quic::QuicSocketAddress& /*peer_address*/) override {
    return {nullptr, nullptr};
  }
  quic::WriteResult Flush() override {
    return quic::WriteResult(quic::WRITE_STATUS_OK, 0);
  }

 private:
  SendQueue* queue_;
};

// ソケットを一切持たないネットワークヘルパ。送信は BufferingPacketWriter に
// 委ね、受信は Python から ProcessUdpPacket 経由で流し込む。
class SansIoNetworkHelper : public quic::QuicClientBase::NetworkHelper {
 public:
  explicit SansIoNetworkHelper(SendQueue* queue) : queue_(queue) {}

  void RunEventLoop() override {}

  bool CreateUDPSocketAndBind(quic::QuicSocketAddress server_address,
                              quic::QuicIpAddress bind_to_address,
                              int bind_to_port) override {
    // ソケットは作らないが、接続が参照する自アドレスだけは矛盾なく決めておく。
    if (bind_to_address.IsInitialized()) {
      client_address_ = quic::QuicSocketAddress(bind_to_address, bind_to_port);
    } else if (server_address.host().address_family() ==
               quiche::IpAddressFamily::IP_V4) {
      client_address_ =
          quic::QuicSocketAddress(quic::QuicIpAddress::Any4(), bind_to_port);
    } else {
      client_address_ =
          quic::QuicSocketAddress(quic::QuicIpAddress::Any6(), bind_to_port);
    }
    return true;
  }

  void CleanUpAllUDPSockets() override {}

  quic::QuicSocketAddress GetLatestClientAddress() const override {
    return client_address_;
  }

  quic::QuicPacketWriter* CreateQuicPacketWriter() override {
    return new BufferingPacketWriter(queue_);
  }

 private:
  SendQueue* queue_;
  quic::QuicSocketAddress client_address_;
};

}  // namespace quiche_py
