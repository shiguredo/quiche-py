/* Bazel ビルドの QUICHE グルーライブラリと nanobind モジュールで共有する。 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  QUICHE_PY_API_VERSION = 4,
};

typedef enum quiche_py_status {
  QUICHE_PY_STATUS_OK = 0,
  QUICHE_PY_STATUS_WOULD_BLOCK = 1,
  QUICHE_PY_STATUS_NOT_FOUND = 2,
  QUICHE_PY_STATUS_CLOSED = 3,
  QUICHE_PY_STATUS_TOO_BIG = 4,
  QUICHE_PY_STATUS_INVALID_ARGUMENT = 5,
  QUICHE_PY_STATUS_ERROR = 6,
} quiche_py_status;

typedef struct quiche_py_client quiche_py_client;
typedef struct quiche_py_http3_client quiche_py_http3_client;
typedef struct quiche_py_http3_response quiche_py_http3_response;
typedef struct quiche_py_wt_client quiche_py_wt_client;

typedef struct quiche_py_client_options {
  const char* host;
  uint16_t port;
  const char* server_name;
  const char* alpn;
  bool verify_peer;
} quiche_py_client_options;

typedef struct quiche_py_http3_client_options {
  const char* host;
  uint16_t port;
  const char* server_name;
  bool verify_peer;
} quiche_py_http3_client_options;

/* HTTP/3 リクエストヘッダ 1 組。name / value は呼び出し側が所有する。 */
typedef struct quiche_py_http3_header {
  const uint8_t* name;
  size_t name_len;
  const uint8_t* value;
  size_t value_len;
} quiche_py_http3_header;

typedef struct quiche_py_wt_client_options {
  const char* host;
  uint16_t port;
  const char* server_name;
  bool verify_peer;
} quiche_py_wt_client_options;

/* 接続設定。QUICHE_PY_CONFIG_UNSET のフィールドは変更しない。 */
#define QUICHE_PY_CONFIG_UNSET UINT64_MAX

typedef struct quiche_py_config {
  uint64_t max_idle_timeout_ms;
  uint64_t initial_max_data;
  uint64_t initial_max_stream_data_bidi_local;
  uint64_t initial_max_stream_data_bidi_remote;
  uint64_t initial_max_stream_data_uni;
  uint64_t max_ack_delay_ms;
  uint64_t min_ack_delay_us;
  uint64_t active_connection_id_limit;
  uint64_t max_datagram_frame_size;
} quiche_py_config;

/* QUIC 接続統計。未接続時は取得関数が false を返す。 */
typedef struct quiche_py_connection_stats {
  uint64_t bytes_sent;
  uint64_t packets_sent;
  uint64_t packets_sent_by_dispatcher;
  uint64_t packets_discarded;
  uint64_t stream_bytes_sent;
  uint64_t bytes_received;
  uint64_t packets_received;
  uint64_t packets_processed;
  uint64_t stream_bytes_received;
  uint64_t bytes_retransmitted;
  uint64_t packets_retransmitted;
  uint64_t bytes_spuriously_retransmitted;
  uint64_t packets_spuriously_retransmitted;
  uint64_t packets_lost;
  uint64_t packets_spuriously_detected_lost;
  uint64_t packets_dropped;
  uint64_t undecryptable_packets_received_before_handshake_complete;
  int64_t min_rtt_us;
  int64_t srtt_us;
  int64_t cwnd_bootstrapping_rtt_us;
  uint64_t egress_mtu;
  uint64_t max_egress_mtu;
  uint64_t ingress_mtu;
  uint64_t estimated_bandwidth_bps;
  uint64_t packets_reordered;
  uint64_t max_sequence_reordering;
  int64_t max_time_reordering_us;
  uint64_t num_coalesced_packets_received;
  uint64_t num_coalesced_packets_processed;
  uint64_t num_ack_aggregation_epochs;
  uint64_t num_failed_authentication_packets_received;
  uint64_t num_tls_server_zero_rtt_packets_received_after_discarding_decrypter;
  uint64_t num_ecn_marks_ect0;
  uint64_t num_ecn_marks_ect1;
  uint64_t num_ecn_marks_ce;
  uint64_t num_ack_frames_sent_with_ecn;
  uint64_t application_data_acked;
  uint64_t application_data_packets_acked;
  uint64_t application_data_packets_with_receive_timestamps_acked;
  double total_loss_detection_response_time;
  uint64_t slowstart_count;
  uint64_t slowstart_num_rtts;
  uint64_t bbr_num_cycles;
  uint64_t bbr_num_short_cycles_for_reno_coexistence;
  uint64_t tcp_loss_events;
  uint64_t max_consecutive_rto_with_forward_progress;
  uint64_t num_amplification_throttling;
  uint64_t first_decrypted_packet;
  uint64_t slowstart_packets_sent;
  uint64_t slowstart_bytes_sent;
  uint64_t slowstart_packets_lost;
  uint64_t slowstart_bytes_lost;
  uint64_t crypto_retransmit_count;
  uint64_t loss_timeout_count;
  uint64_t pto_count;
  uint64_t blocked_frames_received;
  uint64_t blocked_frames_sent;
  uint64_t num_connectivity_probing_received;
  uint64_t num_path_response_received;
  uint64_t key_update_count;
  uint64_t ping_frames_sent;
  uint64_t num_new_connection_id_sent;
  uint64_t num_retire_connection_id_sent;
  uint64_t num_peer_migration_to_proactively_validated_address;
  uint64_t num_reverse_path_validation_upon_migration;
  uint64_t num_validated_peer_migration;
  uint64_t num_invalid_peer_migration;
  uint64_t num_peer_migration_while_validating_default_path;
  int64_t connection_creation_time_us;
  int64_t handshake_completion_time_us;
} quiche_py_connection_stats;

/* WebTransport のセッション統計 (W3C WebTransport Stats 相当)。 */
typedef struct quiche_py_wt_session_stats {
  uint64_t min_rtt_us;
  uint64_t smoothed_rtt_us;
  uint64_t rtt_variation_us;
  uint64_t application_bytes_acknowledged;
  uint64_t estimated_send_rate_bps;
} quiche_py_wt_session_stats;

/* WebTransport の datagram 統計。 */
typedef struct quiche_py_wt_datagram_stats {
  uint64_t expired_outgoing;
  uint64_t lost_outgoing;
} quiche_py_wt_datagram_stats;

typedef struct quiche_py_api {
  uint32_t abi_version;

  const char* (*supported_versions_sample)(void);

  /* --- raw QUIC (既存) --- */
  quiche_py_client* (*client_create)(
      const quiche_py_client_options* options);
  void (*client_destroy)(quiche_py_client* client);

  quiche_py_status (*client_start)(quiche_py_client* client);
  quiche_py_status (*client_receive_packet)(
      quiche_py_client* client, const uint8_t* data, size_t data_len);
  quiche_py_status (*client_next_send)(quiche_py_client* client,
                                              uint8_t* buffer, size_t buffer_len,
                                              size_t* written);
  quiche_py_status (*client_next_timeout_ms)(
      quiche_py_client* client, uint64_t* timeout_ms, bool* has_timeout);
  quiche_py_status (*client_handle_timeout)(
      quiche_py_client* client);

  void (*client_close)(quiche_py_client* client, uint32_t error_code,
                       const char* reason);
  bool (*client_is_connected)(const quiche_py_client* client);

  quiche_py_status (*client_open_stream)(quiche_py_client* client,
                                                bool unidirectional,
                                                uint32_t* stream_id);
  quiche_py_status (*client_accept_stream)(
      quiche_py_client* client, bool unidirectional, uint32_t* stream_id);
  quiche_py_status (*client_read_stream)(
      quiche_py_client* client, uint32_t stream_id, uint8_t* buffer,
      size_t buffer_len, size_t* bytes_read, bool* fin);
  quiche_py_status (*client_write_stream)(
      quiche_py_client* client, uint32_t stream_id, const uint8_t* data,
      size_t data_len, bool fin);
  quiche_py_status (*client_reset_stream)(
      quiche_py_client* client, uint32_t stream_id, uint32_t error_code);
  quiche_py_status (*client_stop_sending)(
      quiche_py_client* client, uint32_t stream_id, uint32_t error_code);
  bool (*client_stream_can_write)(quiche_py_client* client,
                                  uint32_t stream_id);
  bool (*client_stream_exists)(quiche_py_client* client,
                               uint32_t stream_id);

  quiche_py_status (*client_send_datagram)(quiche_py_client* client,
                                                  const uint8_t* data,
                                                  size_t data_len);
  quiche_py_status (*client_peek_datagram_size)(
      quiche_py_client* client, size_t* datagram_size);
  quiche_py_status (*client_receive_datagram)(
      quiche_py_client* client, uint8_t* buffer, size_t buffer_len,
      size_t* bytes_read);
  size_t (*client_max_datagram_size)(quiche_py_client* client);

  const char* (*client_last_error)(const quiche_py_client* client);

  /* --- HTTP/3 --- */
  quiche_py_http3_client* (*h3_client_create)(
      const quiche_py_http3_client_options* options);
  void (*h3_client_destroy)(quiche_py_http3_client* client);
  quiche_py_status (*h3_client_start)(quiche_py_http3_client* client);
  quiche_py_status (*h3_client_receive_packet)(
      quiche_py_http3_client* client, const uint8_t* data, size_t data_len);
  quiche_py_status (*h3_client_next_send)(quiche_py_http3_client* client,
                                          uint8_t* buffer, size_t buffer_len,
                                          size_t* written);
  quiche_py_status (*h3_client_next_timeout_ms)(
      quiche_py_http3_client* client, uint64_t* timeout_ms, bool* has_timeout);
  quiche_py_status (*h3_client_handle_timeout)(
      quiche_py_http3_client* client);
  void (*h3_client_close)(quiche_py_http3_client* client, uint32_t error_code,
                          const char* reason);
  bool (*h3_client_is_connected)(const quiche_py_http3_client* client);
  quiche_py_status (*h3_client_submit_request)(
      quiche_py_http3_client* client, const quiche_py_http3_header* headers,
      size_t header_count, const uint8_t* body, size_t body_len, bool fin,
      uint32_t* stream_id);
  /* 完了していれば所有権付きレスポンスを返す。未完了なら WOULD_BLOCK。 */
  quiche_py_status (*h3_client_take_response)(
      quiche_py_http3_client* client, uint32_t stream_id,
      quiche_py_http3_response** response);
  void (*h3_response_destroy)(quiche_py_http3_response* response);
  uint32_t (*h3_response_stream_id)(const quiche_py_http3_response* response);
  int (*h3_response_status_code)(const quiche_py_http3_response* response);
  size_t (*h3_response_header_count)(const quiche_py_http3_response* response);
  quiche_py_status (*h3_response_header_at)(
      const quiche_py_http3_response* response, size_t index,
      const uint8_t** name, size_t* name_len, const uint8_t** value,
      size_t* value_len);
  const uint8_t* (*h3_response_body)(const quiche_py_http3_response* response,
                                     size_t* body_len);
  const char* (*h3_client_last_error)(const quiche_py_http3_client* client);

  /* --- WebTransport over HTTP/3 --- */
  quiche_py_wt_client* (*wt_client_create)(
      const quiche_py_wt_client_options* options);
  void (*wt_client_destroy)(quiche_py_wt_client* client);
  quiche_py_status (*wt_client_start)(quiche_py_wt_client* client);
  quiche_py_status (*wt_client_receive_packet)(
      quiche_py_wt_client* client, const uint8_t* data, size_t data_len);
  quiche_py_status (*wt_client_next_send)(quiche_py_wt_client* client,
                                          uint8_t* buffer, size_t buffer_len,
                                          size_t* written);
  quiche_py_status (*wt_client_next_timeout_ms)(
      quiche_py_wt_client* client, uint64_t* timeout_ms, bool* has_timeout);
  quiche_py_status (*wt_client_handle_timeout)(quiche_py_wt_client* client);
  void (*wt_client_close)(quiche_py_wt_client* client, uint32_t error_code,
                          const char* reason);
  bool (*wt_client_is_connected)(const quiche_py_wt_client* client);
  /* SETTINGS 受信後に CONNECT を送る。枠不足なら WOULD_BLOCK。 */
  quiche_py_status (*wt_client_connect_session)(
      quiche_py_wt_client* client, const char* path,
      const quiche_py_http3_header* extra_headers, size_t header_count);
  bool (*wt_client_is_session_ready)(const quiche_py_wt_client* client);
  quiche_py_status (*wt_client_open_stream)(quiche_py_wt_client* client,
                                            bool unidirectional,
                                            uint32_t* stream_id);
  quiche_py_status (*wt_client_accept_stream)(quiche_py_wt_client* client,
                                              bool unidirectional,
                                              uint32_t* stream_id);
  quiche_py_status (*wt_client_read_stream)(
      quiche_py_wt_client* client, uint32_t stream_id, uint8_t* buffer,
      size_t buffer_len, size_t* bytes_read, bool* fin);
  quiche_py_status (*wt_client_write_stream)(
      quiche_py_wt_client* client, uint32_t stream_id, const uint8_t* data,
      size_t data_len, bool fin);
  quiche_py_status (*wt_client_reset_stream)(quiche_py_wt_client* client,
                                             uint32_t stream_id,
                                             uint32_t error_code);
  quiche_py_status (*wt_client_stop_sending)(quiche_py_wt_client* client,
                                             uint32_t stream_id,
                                             uint32_t error_code);
  bool (*wt_client_stream_can_write)(quiche_py_wt_client* client,
                                     uint32_t stream_id);
  bool (*wt_client_stream_exists)(quiche_py_wt_client* client,
                                  uint32_t stream_id);
  quiche_py_status (*wt_client_send_datagram)(quiche_py_wt_client* client,
                                              const uint8_t* data,
                                              size_t data_len);
  quiche_py_status (*wt_client_peek_datagram_size)(
      quiche_py_wt_client* client, size_t* datagram_size);
  quiche_py_status (*wt_client_receive_datagram)(
      quiche_py_wt_client* client, uint8_t* buffer, size_t buffer_len,
      size_t* bytes_read);
  size_t (*wt_client_max_datagram_size)(quiche_py_wt_client* client);
  const char* (*wt_client_last_error)(const quiche_py_wt_client* client);

  /* --- 低レベルテスト用の拡張 API --- */
  quiche_py_status (*client_configure)(quiche_py_client* client,
                                       const quiche_py_config* config);
  bool (*client_stats)(const quiche_py_client* client,
                       quiche_py_connection_stats* stats);
  bool (*client_socket_packets_dropped)(const quiche_py_client* client,
                                        uint64_t* packets_dropped);
  quiche_py_status (*client_receive_packets)(
      quiche_py_client* client, const uint8_t* const* data,
      const size_t* data_lengths, size_t packet_count);
  quiche_py_status (*client_write_stream_atomic)(
      quiche_py_client* client, uint32_t stream_id, const uint8_t* data,
      size_t data_len, bool fin);
  quiche_py_status (*client_stream_set_priority)(quiche_py_client* client,
                                                 uint32_t stream_id,
                                                 uint32_t send_group_id,
                                                 uint32_t send_order);

  quiche_py_status (*h3_client_configure)(
      quiche_py_http3_client* client, const quiche_py_config* config);
  bool (*h3_client_stats)(const quiche_py_http3_client* client,
                          quiche_py_connection_stats* stats);
  bool (*h3_client_socket_packets_dropped)(
      const quiche_py_http3_client* client, uint64_t* packets_dropped);
  quiche_py_status (*h3_client_receive_packets)(
      quiche_py_http3_client* client, const uint8_t* const* data,
      const size_t* data_lengths, size_t packet_count);
  bool (*h3_client_settings_received)(const quiche_py_http3_client* client);
  size_t (*h3_client_setting_count)(const quiche_py_http3_client* client);
  bool (*h3_client_setting_at)(const quiche_py_http3_client* client,
                               size_t index, uint64_t* identifier,
                               uint64_t* value);
  size_t (*h3_client_settings_wire_order_count)(
      const quiche_py_http3_client* client);
  bool (*h3_client_settings_wire_order_at)(
      const quiche_py_http3_client* client, size_t index, uint64_t* identifier);

  quiche_py_status (*wt_client_configure)(quiche_py_wt_client* client,
                                          const quiche_py_config* config);
  bool (*wt_client_stats)(const quiche_py_wt_client* client,
                          quiche_py_connection_stats* stats);
  bool (*wt_client_socket_packets_dropped)(const quiche_py_wt_client* client,
                                           uint64_t* packets_dropped);
  quiche_py_status (*wt_client_receive_packets)(
      quiche_py_wt_client* client, const uint8_t* const* data,
      const size_t* data_lengths, size_t packet_count);
  bool (*wt_client_session_stats)(const quiche_py_wt_client* client,
                                  quiche_py_wt_session_stats* stats);
  bool (*wt_client_datagram_stats)(const quiche_py_wt_client* client,
                                   quiche_py_wt_datagram_stats* stats);
  bool (*wt_client_is_draining)(const quiche_py_wt_client* client);
  void (*wt_client_notify_draining)(quiche_py_wt_client* client);
  const char* (*wt_client_protocol)(const quiche_py_wt_client* client);
  quiche_py_status (*wt_client_write_stream_atomic)(
      quiche_py_wt_client* client, uint32_t stream_id, const uint8_t* data,
      size_t data_len, bool fin);
  quiche_py_status (*wt_client_stream_set_priority)(quiche_py_wt_client* client,
                                                    uint32_t stream_id,
                                                    uint32_t send_group_id,
                                                    uint32_t send_order);
} quiche_py_api;

const quiche_py_api* quiche_py_get_api(void);

#ifdef __cplusplus
}
#endif
