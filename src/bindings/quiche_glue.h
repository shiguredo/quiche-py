/* Bazel ビルドの QUICHE グルーライブラリと nanobind モジュールで共有する。 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  QUICHE_PY_API_VERSION = 3,
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
} quiche_py_api;

const quiche_py_api* quiche_py_get_api(void);

#ifdef __cplusplus
}
#endif
