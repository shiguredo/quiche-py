/* HTTP/3 / WebTransport C API 実装の宣言 (API テーブル配線用)。 */
#pragma once

#include "quiche_glue.h"

#ifdef __cplusplus
extern "C" {
#endif

quiche_py_http3_client* quiche_py_h3_client_create(
    const quiche_py_http3_client_options* options);
void quiche_py_h3_client_destroy(quiche_py_http3_client* client);
quiche_py_status quiche_py_h3_client_start(quiche_py_http3_client* client);
quiche_py_status quiche_py_h3_client_receive_packet(
    quiche_py_http3_client* client, const uint8_t* data, size_t data_len);
quiche_py_status quiche_py_h3_client_next_send(quiche_py_http3_client* client,
                                               uint8_t* buffer,
                                               size_t buffer_len,
                                               size_t* written);
quiche_py_status quiche_py_h3_client_next_timeout_ms(
    quiche_py_http3_client* client, uint64_t* timeout_ms, bool* has_timeout);
quiche_py_status quiche_py_h3_client_handle_timeout(
    quiche_py_http3_client* client);
void quiche_py_h3_client_close(quiche_py_http3_client* client,
                               uint32_t error_code, const char* reason);
bool quiche_py_h3_client_is_connected(const quiche_py_http3_client* client);
quiche_py_status quiche_py_h3_client_submit_request(
    quiche_py_http3_client* client, const quiche_py_http3_header* headers,
    size_t header_count, const uint8_t* body, size_t body_len, bool fin,
    uint32_t* stream_id);
quiche_py_status quiche_py_h3_client_take_response(
    quiche_py_http3_client* client, uint32_t stream_id,
    quiche_py_http3_response** response);
void quiche_py_h3_response_destroy(quiche_py_http3_response* response);
uint32_t quiche_py_h3_response_stream_id(
    const quiche_py_http3_response* response);
int quiche_py_h3_response_status_code(
    const quiche_py_http3_response* response);
size_t quiche_py_h3_response_header_count(
    const quiche_py_http3_response* response);
quiche_py_status quiche_py_h3_response_header_at(
    const quiche_py_http3_response* response, size_t index, const uint8_t** name,
    size_t* name_len, const uint8_t** value, size_t* value_len);
const uint8_t* quiche_py_h3_response_body(
    const quiche_py_http3_response* response, size_t* body_len);
const char* quiche_py_h3_client_last_error(
    const quiche_py_http3_client* client);

quiche_py_wt_client* quiche_py_wt_client_create(
    const quiche_py_wt_client_options* options);
void quiche_py_wt_client_destroy(quiche_py_wt_client* client);
quiche_py_status quiche_py_wt_client_start(quiche_py_wt_client* client);
quiche_py_status quiche_py_wt_client_receive_packet(
    quiche_py_wt_client* client, const uint8_t* data, size_t data_len);
quiche_py_status quiche_py_wt_client_next_send(quiche_py_wt_client* client,
                                               uint8_t* buffer,
                                               size_t buffer_len,
                                               size_t* written);
quiche_py_status quiche_py_wt_client_next_timeout_ms(
    quiche_py_wt_client* client, uint64_t* timeout_ms, bool* has_timeout);
quiche_py_status quiche_py_wt_client_handle_timeout(
    quiche_py_wt_client* client);
void quiche_py_wt_client_close(quiche_py_wt_client* client,
                               uint32_t error_code, const char* reason);
bool quiche_py_wt_client_is_connected(const quiche_py_wt_client* client);
quiche_py_status quiche_py_wt_client_connect_session(
    quiche_py_wt_client* client, const char* path,
    const quiche_py_http3_header* extra_headers, size_t header_count);
bool quiche_py_wt_client_is_session_ready(const quiche_py_wt_client* client);
quiche_py_status quiche_py_wt_client_open_stream(quiche_py_wt_client* client,
                                                 bool unidirectional,
                                                 uint32_t* stream_id);
quiche_py_status quiche_py_wt_client_accept_stream(quiche_py_wt_client* client,
                                                   bool unidirectional,
                                                   uint32_t* stream_id);
quiche_py_status quiche_py_wt_client_read_stream(
    quiche_py_wt_client* client, uint32_t stream_id, uint8_t* buffer,
    size_t buffer_len, size_t* bytes_read, bool* fin);
quiche_py_status quiche_py_wt_client_write_stream(
    quiche_py_wt_client* client, uint32_t stream_id, const uint8_t* data,
    size_t data_len, bool fin);
quiche_py_status quiche_py_wt_client_reset_stream(quiche_py_wt_client* client,
                                                  uint32_t stream_id,
                                                  uint32_t error_code);
quiche_py_status quiche_py_wt_client_stop_sending(quiche_py_wt_client* client,
                                                  uint32_t stream_id,
                                                  uint32_t error_code);
bool quiche_py_wt_client_stream_can_write(quiche_py_wt_client* client,
                                          uint32_t stream_id);
bool quiche_py_wt_client_stream_exists(quiche_py_wt_client* client,
                                       uint32_t stream_id);
quiche_py_status quiche_py_wt_client_send_datagram(quiche_py_wt_client* client,
                                                   const uint8_t* data,
                                                   size_t data_len);
quiche_py_status quiche_py_wt_client_peek_datagram_size(
    quiche_py_wt_client* client, size_t* datagram_size);
quiche_py_status quiche_py_wt_client_receive_datagram(
    quiche_py_wt_client* client, uint8_t* buffer, size_t buffer_len,
    size_t* bytes_read);
size_t quiche_py_wt_client_max_datagram_size(quiche_py_wt_client* client);
const char* quiche_py_wt_client_last_error(const quiche_py_wt_client* client);

#ifdef __cplusplus
}
#endif
