/* quiche_py_config を QuicConfig へ反映するヘルパ。 */
#pragma once

#include "quiche_glue.h"

#include "quiche/quic/core/quic_config.h"
#include "quiche/quic/core/quic_time.h"

namespace quiche_py {

// QUICHE_PY_CONFIG_UNSET のフィールドは変更しない。
inline void ApplyConfig(quic::QuicConfig* config,
                        const quiche_py_config& source) {
  if (source.max_idle_timeout_ms != QUICHE_PY_CONFIG_UNSET) {
    config->SetIdleNetworkTimeout(quic::QuicTime::Delta::FromMilliseconds(
        static_cast<int64_t>(source.max_idle_timeout_ms)));
  }
  if (source.initial_max_data != QUICHE_PY_CONFIG_UNSET) {
    config->SetInitialSessionFlowControlWindowToSend(source.initial_max_data);
  }
  if (source.initial_max_stream_data_bidi_local != QUICHE_PY_CONFIG_UNSET) {
    config->SetInitialMaxStreamDataBytesOutgoingBidirectionalToSend(
        source.initial_max_stream_data_bidi_local);
  }
  if (source.initial_max_stream_data_bidi_remote != QUICHE_PY_CONFIG_UNSET) {
    config->SetInitialMaxStreamDataBytesIncomingBidirectionalToSend(
        source.initial_max_stream_data_bidi_remote);
  }
  if (source.initial_max_stream_data_uni != QUICHE_PY_CONFIG_UNSET) {
    config->SetInitialMaxStreamDataBytesUnidirectionalToSend(
        source.initial_max_stream_data_uni);
  }
  if (source.max_ack_delay_ms != QUICHE_PY_CONFIG_UNSET) {
    config->SetMaxAckDelayToSendMs(
        static_cast<uint32_t>(source.max_ack_delay_ms));
  }
  if (source.min_ack_delay_us != QUICHE_PY_CONFIG_UNSET) {
    config->SetMinAckDelayDraft14Us(source.min_ack_delay_us);
  }
  if (source.active_connection_id_limit != QUICHE_PY_CONFIG_UNSET) {
    config->SetActiveConnectionIdLimitToSend(
        source.active_connection_id_limit);
  }
  if (source.max_datagram_frame_size != QUICHE_PY_CONFIG_UNSET) {
    config->SetMaxDatagramFrameSizeToSend(source.max_datagram_frame_size);
  }
}

}  // namespace quiche_py
