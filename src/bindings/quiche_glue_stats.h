/* QUICHE の統計構造体を C API の構造体へ詰め替えるヘルパ。 */
#pragma once

#include "quiche_glue.h"

#include "absl/time/time.h"
#include "quiche/quic/core/quic_connection_stats.h"
#include "quiche/quic/core/quic_packet_number.h"
#include "quiche/quic/core/quic_time.h"
#include "quiche/web_transport/web_transport.h"

namespace quiche_py {

// QuicTime はクロック原点からの経過時間をマイクロ秒で保持する。
inline int64_t TimeToMicroseconds(const quic::QuicTime& time) {
  return time.ToDebuggingValue();
}

// QuicConnectionStats のうち、テストで参照頻度の高いフィールドを転写する。
inline void FillConnectionStats(const quic::QuicConnectionStats& source,
                                quiche_py_connection_stats* destination) {
  destination->bytes_sent = source.bytes_sent;
  destination->packets_sent = source.packets_sent;
  destination->packets_sent_by_dispatcher = source.packets_sent_by_dispatcher;
  destination->packets_discarded = source.packets_discarded;
  destination->stream_bytes_sent = source.stream_bytes_sent;
  destination->bytes_received = source.bytes_received;
  destination->packets_received = source.packets_received;
  destination->packets_processed = source.packets_processed;
  destination->stream_bytes_received = source.stream_bytes_received;
  destination->bytes_retransmitted = source.bytes_retransmitted;
  destination->packets_retransmitted = source.packets_retransmitted;
  destination->bytes_spuriously_retransmitted =
      source.bytes_spuriously_retransmitted;
  destination->packets_spuriously_retransmitted =
      source.packets_spuriously_retransmitted;
  destination->packets_lost = source.packets_lost;
  destination->packets_spuriously_detected_lost =
      source.packet_spuriously_detected_lost;
  destination->packets_dropped = source.packets_dropped;
  destination->undecryptable_packets_received_before_handshake_complete =
      source.undecryptable_packets_received_before_handshake_complete;
  destination->min_rtt_us = source.min_rtt_us;
  destination->srtt_us = source.srtt_us;
  destination->cwnd_bootstrapping_rtt_us = source.cwnd_bootstrapping_rtt_us;
  destination->egress_mtu = source.egress_mtu;
  destination->max_egress_mtu = source.max_egress_mtu;
  destination->ingress_mtu = source.ingress_mtu;
  destination->estimated_bandwidth_bps =
      source.estimated_bandwidth.ToBitsPerSecond();
  destination->packets_reordered = source.packets_reordered;
  destination->max_sequence_reordering = source.max_sequence_reordering;
  destination->max_time_reordering_us = source.max_time_reordering_us;
  destination->num_coalesced_packets_received =
      source.num_coalesced_packets_received;
  destination->num_coalesced_packets_processed =
      source.num_coalesced_packets_processed;
  destination->num_ack_aggregation_epochs = source.num_ack_aggregation_epochs;
  destination->num_failed_authentication_packets_received =
      source.num_failed_authentication_packets_received;
  destination->num_tls_server_zero_rtt_packets_received_after_discarding_decrypter =
      source.num_tls_server_zero_rtt_packets_received_after_discarding_decrypter;
  destination->num_ecn_marks_ect0 = source.num_ecn_marks_received.ect0;
  destination->num_ecn_marks_ect1 = source.num_ecn_marks_received.ect1;
  destination->num_ecn_marks_ce = source.num_ecn_marks_received.ce;
  destination->num_ack_frames_sent_with_ecn =
      source.num_ack_frames_sent_with_ecn;
  destination->application_data_acked = source.application_data_acked;
  destination->application_data_packets_acked =
      source.application_data_packets_acked;
  destination->application_data_packets_with_receive_timestamps_acked =
      source.application_data_packets_with_receive_timestamps_acked;
  destination->total_loss_detection_response_time =
      source.total_loss_detection_response_time;
  destination->slowstart_count = source.slowstart_count;
  destination->slowstart_num_rtts = source.slowstart_num_rtts;
  destination->slowstart_packets_sent = source.slowstart_packets_sent;
  destination->slowstart_bytes_sent = source.slowstart_bytes_sent;
  destination->slowstart_packets_lost = source.slowstart_packets_lost;
  destination->slowstart_bytes_lost = source.slowstart_bytes_lost;
  destination->bbr_num_cycles = source.bbr_num_cycles;
  destination->bbr_num_short_cycles_for_reno_coexistence =
      source.bbr_num_short_cycles_for_reno_coexistence;
  destination->crypto_retransmit_count = source.crypto_retransmit_count;
  destination->loss_timeout_count = source.loss_timeout_count;
  destination->pto_count = source.pto_count;
  destination->tcp_loss_events = source.tcp_loss_events;
  destination->blocked_frames_received = source.blocked_frames_received;
  destination->blocked_frames_sent = source.blocked_frames_sent;
  destination->num_connectivity_probing_received =
      source.num_connectivity_probing_received;
  destination->num_path_response_received = source.num_path_response_received;
  destination->max_consecutive_rto_with_forward_progress =
      source.max_consecutive_rto_with_forward_progress;
  destination->num_amplification_throttling =
      source.num_amplification_throttling;
  destination->key_update_count = source.key_update_count;
  destination->ping_frames_sent = source.ping_frames_sent;
  destination->num_new_connection_id_sent =
      source.num_new_connection_id_sent;
  destination->num_retire_connection_id_sent =
      source.num_retire_connection_id_sent;
  destination->num_peer_migration_to_proactively_validated_address =
      source.num_peer_migration_to_proactively_validated_address;
  destination->num_reverse_path_validation_upon_migration =
      source.num_reverse_path_validtion_upon_migration;
  destination->num_validated_peer_migration =
      source.num_validated_peer_migration;
  destination->num_invalid_peer_migration =
      source.num_invalid_peer_migration;
  destination->num_peer_migration_while_validating_default_path =
      source.num_peer_migration_while_validating_default_path;
  destination->first_decrypted_packet =
      source.first_decrypted_packet.IsInitialized()
          ? source.first_decrypted_packet.ToUint64()
          : 0;
  destination->connection_creation_time_us =
      TimeToMicroseconds(source.connection_creation_time);
  destination->handshake_completion_time_us =
      TimeToMicroseconds(source.handshake_completion_time);
}

// W3C WebTransport Stats 相当のセッション統計を転写する。
inline void FillSessionStats(const webtransport::SessionStats& source,
                             quiche_py_wt_session_stats* destination) {
  destination->min_rtt_us =
      static_cast<uint64_t>(absl::ToInt64Microseconds(source.min_rtt));
  destination->smoothed_rtt_us =
      static_cast<uint64_t>(absl::ToInt64Microseconds(source.smoothed_rtt));
  destination->rtt_variation_us =
      static_cast<uint64_t>(absl::ToInt64Microseconds(source.rtt_variation));
  destination->application_bytes_acknowledged =
      source.application_bytes_acknowledged;
  destination->estimated_send_rate_bps = source.estimated_send_rate_bps;
}

// WebTransport の datagram 統計を転写する。
inline void FillDatagramStats(const webtransport::DatagramStats& source,
                              quiche_py_wt_datagram_stats* destination) {
  destination->expired_outgoing = source.expired_outgoing;
  destination->lost_outgoing = source.lost_outgoing;
}

}  // namespace quiche_py
