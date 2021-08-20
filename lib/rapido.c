#include "rapido.h"
#include "rapido_internals.h"
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <netinet/in.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <arpa/inet.h>
#include <fcntl.h>

#define WARNING(fmt, ...) fprintf(stderr, fmt, ##__VA_ARGS__)
#ifndef NO_LOG
#define LOG if (1)
#define QLOG(session, ev_type, cat, trigger, data_fmt, ...)                                                                        \
    do {                                                                                                                           \
        if (!(session)->qlog.out)                                                                                                  \
            break;                                                                                                                 \
        fprintf((session)->qlog.out, "[%lu, \"%s:%s:%s\", \"%s\", ", get_time() - (session)->qlog.reference_time,                  \
                (session)->is_server ? "server" : "client", ev_type, cat, trigger);                                                \
        fprintf((session)->qlog.out, data_fmt ? data_fmt : "{}", ##__VA_ARGS__);                                                   \
        fprintf((session)->qlog.out, "],\n");                                                                                      \
    } while (0)
#else
#define QLOG(session, ev_type, cat, trigger, data_fmt, ...)
#define LOG if (0)
#endif

#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))

#define TLS_MAX_RECORD_SIZE 16384
#define TLS_MAX_ENCRYPTED_RECORD_SIZE (TLS_MAX_RECORD_SIZE + 22)
#define TLS_RAPIDO_HELLO_EXT 100

#define DEFAULT_TCPLS_SESSION_ID_AMOUNT 4

#define debug_dump(src, len)                                                                                                       \
    do {                                                                                                                           \
        WARNING("Dumping %zu bytes from %p (%s:%d)\n", (size_t)len, src, __FILE__, __LINE__);                                      \
        for (int i = 0; i < len;) {                                                                                                \
            fprintf(stderr, "%04x:  ", (int)i);                                                                                    \
                                                                                                                                   \
            for (int j = 0; j < 16 && i < len; j++) {                                                                              \
                fprintf(stderr, "%02x ", ((uint8_t *)src)[i + j]);                                                                 \
            }                                                                                                                      \
            fprintf(stderr, "\t");                                                                                                 \
            for (int j = 0; j < 16 && i < len; j++, i++) {                                                                         \
                if (32 <= ((uint8_t *)src)[i] && ((uint8_t *)src)[i] > 127) {                                                      \
                    fprintf(stderr, "%c", ((uint8_t *)src)[i]);                                                                  \
                } else {                                                                                                           \
                    fprintf(stderr, " ");                                                                     \
                }                                                                                                                  \
            }                                                                                                                      \
            fprintf(stderr, "\n");                                                                                                 \
        }                                                                                                                          \
    } while (0);

void tohex(uint8_t *in, size_t len, char *out) {
    uint8_t *max_in = in + len;
    const char *hex = "0123456789abcdef";
    for (; in < max_in; out += 2, in++) {
        out[0] = hex[(*in >> 4) & 0xf];
        out[1] = hex[*in & 0xf];
    }
    *out = 0;
}

int collect_rapido_extensions(ptls_t *tls, struct st_ptls_handshake_properties_t *properties, uint16_t type) {
    return type == TLS_RAPIDO_HELLO_EXT;
}

int collected_rapido_extensions(ptls_t *tls, struct st_ptls_handshake_properties_t *properties, ptls_raw_extension_t *extensions) {
    ptls_raw_extension_t *extension = extensions;
    size_t no_extensions = 0;
    while(extension->type != UINT16_MAX) {
        no_extensions++;
        extension++;
    }
    if (properties->additional_extensions) {
        free(properties->additional_extensions);
    }
    properties->additional_extensions = malloc(sizeof(ptls_raw_extension_t) * (no_extensions + 1));
    memcpy(properties->additional_extensions, extensions, sizeof(ptls_raw_extension_t) * (no_extensions + 1));
    properties->collected_extensions = NULL;
    return 0;
}

void derive_connection_aead_iv(uint8_t *iv, rapido_connection_id_t connection_id) {
    /* XOR the upper 4 bytes with the given connection id */
    uint32_t msb_iv = ntohl(*(uint32_t *)(iv));
    msb_iv ^= connection_id;
    msb_iv = htonl(msb_iv);
    *(uint32_t *)iv = msb_iv;
}

int setup_connection_crypto_context(rapido_t *session, rapido_connection_t *connection)
{
    struct st_ptls_traffic_protection_t *ctx_enc = ptls_get_traffic_protection(session->tls, 0);
    struct st_ptls_traffic_protection_t *ctx_dec = ptls_get_traffic_protection(session->tls, 1);
    uint8_t key[PTLS_MAX_SECRET_SIZE];
    uint8_t iv[PTLS_MAX_IV_SIZE];
    int ret;

    ptls_cipher_suite_t *cipher = ptls_get_cipher(session->tls);

    if ((ret = ptls_hkdf_expand_label(cipher->hash, key, cipher->aead->key_size,
                                      ptls_iovec_init(ctx_enc->secret, cipher->hash->digest_size), "key", ptls_iovec_init(NULL, 0),
                                      session->tls_ctx->hkdf_label_prefix__obsolete)) != 0)
        return -1;
    if ((ret = ptls_hkdf_expand_label(cipher->hash, iv, cipher->aead->iv_size,
                                      ptls_iovec_init(ctx_enc->secret, cipher->hash->digest_size), "iv", ptls_iovec_init(NULL, 0),
                                      session->tls_ctx->hkdf_label_prefix__obsolete)) != 0)
        return -1;

    assert(cipher->aead->iv_size >= 12);
    derive_connection_aead_iv(iv, connection->connection_id);

    connection->encryption_ctx = malloc(sizeof(struct st_ptls_traffic_protection_t));
    assert(connection->encryption_ctx);
    memcpy(connection->encryption_ctx, ctx_enc, sizeof(struct st_ptls_traffic_protection_t));
    connection->encryption_ctx->aead = ptls_aead_new_direct(cipher->aead, 1, key, iv);
    if (connection->connection_id > 0) {
        connection->encryption_ctx->seq = 0;
    } else {
        ptls_aead_free(ctx_enc->aead);
    }

    if ((ret = ptls_hkdf_expand_label(cipher->hash, key, cipher->aead->key_size,
                                      ptls_iovec_init(ctx_dec->secret, cipher->hash->digest_size), "key", ptls_iovec_init(NULL, 0),
                                      session->tls_ctx->hkdf_label_prefix__obsolete)) != 0)
        return -1;
    if ((ret = ptls_hkdf_expand_label(cipher->hash, iv, cipher->aead->iv_size,
                                      ptls_iovec_init(ctx_dec->secret, cipher->hash->digest_size), "iv", ptls_iovec_init(NULL, 0),
                                      session->tls_ctx->hkdf_label_prefix__obsolete)) != 0)
        return -1;

    derive_connection_aead_iv(iv, connection->connection_id);

    connection->decryption_ctx = malloc(sizeof(struct st_ptls_traffic_protection_t));
    assert(connection->decryption_ctx);
    memcpy(connection->decryption_ctx, ctx_dec, sizeof(struct st_ptls_traffic_protection_t));
    connection->decryption_ctx->aead = ptls_aead_new_direct(cipher->aead, 0, key, iv);
    if (connection->connection_id > 0) {
        connection->decryption_ctx->seq = 0;
    } else {
        ptls_aead_free(ctx_dec->aead);
    }
    return 0;
}

uint64_t get_time() {
    struct timespec tv;
    assert(clock_gettime(CLOCK_REALTIME, &tv) == 0);
    return tv.tv_sec * 1000000 + tv.tv_nsec / 1000;
}

void *rapido_array_add(rapido_array_t *array, size_t index) {
    if (index >= array->capacity) {
        size_t new_capacity = max(index * 2, max(array->capacity * 2, 1));
        array->data = reallocarray(array->data, new_capacity, 1 + array->item_size);
        assert(array->data != NULL);
        for (int i = array->capacity; i < new_capacity; i++) {
            array->data[(1 + array->item_size) * i] = false;
        }
        array->capacity = new_capacity;
    }
    assert(array->data[(1 + array->item_size) * index] == false);
    array->data[(1 + array->item_size) * index] = true;
    array->size++;
    return array->data + ((1 + array->item_size) * index) + 1;
}

void *rapido_array_get(rapido_array_t *array, size_t index) {
    if (index >= array->capacity) {
        return NULL;
    }
    size_t offset = (1 + array->item_size) * index;
    return array->data[offset] == true ? array->data + offset + 1 : NULL;
}

int rapido_array_delete(rapido_array_t *array, size_t index) {
    if (index >= array->capacity) {
        return 1;
    }
    size_t offset = (1 + array->item_size) * index;
    if(array->data[offset] == false) {
        return 1;
    }
    array->data[(1 + array->item_size) * index] = false;
    array->size--;
    return 0;
}

void rapido_array_free(rapido_array_t *array) {
    if (array->capacity && array->data) {
        free(array->data);
        memset(array, 0, sizeof(rapido_array_t));
    }
}

#define rapido_array_iter(a, e, bl)                                                                                                \
    do {                                                                                                                           \
        for (int i = 0; i < (a)->capacity; i++) {                                                                                  \
            size_t offset = (1 + (a)->item_size) * i;                                                                               \
            if ((a)->data[offset] == true) {                                                                                       \
                e = (void *)(a)->data + offset + 1;                                                                                \
                bl                                                                                                                 \
            }                                                                                                                      \
        }                                                                                                                          \
    } while (0)

void rapido_queue_init(rapido_queue_t *queue, size_t item_size, size_t capacity) {
    queue->data = malloc(item_size * capacity);
    assert(queue->data != NULL);
    queue->capacity = capacity;
    queue->size = 0;
    queue->front_index = 0;
    queue->back_index = 0;
    queue->item_size = item_size;
}

void *rapido_queue_push(rapido_queue_t *queue) {
    assert(queue->size < queue->capacity);
    size_t item_index = queue->back_index;
    queue->back_index = (queue->back_index + 1) % queue->capacity;
    queue->size++;
    return queue->data + (item_index * queue->item_size);
}

void *rapido_queue_pop(rapido_queue_t *queue) {
    assert(queue->size > 0);
    size_t item_index = queue->front_index;
    queue->front_index = (queue->front_index + 1) % queue->capacity;
    queue->size--;
    return queue->data + (item_index * queue->item_size);
}

void rapido_queue_free(rapido_queue_t *queue) {
    if (queue->capacity && queue->data) {
        free(queue->data);
        memset(queue, 0, sizeof(rapido_queue_t));
    }
}

void rapido_stream_buffer_init(rapido_stream_buffer_t *buffer, size_t capacity) {
    buffer->data = malloc(capacity);
    assert(buffer->data != NULL);
    buffer->capacity = capacity;
    buffer->size = 0;
    buffer->front_index = 0;
    buffer->back_index = 0;
}

void *rapido_stream_buffer_alloc(rapido_stream_buffer_t *buffer, size_t *len) {
    while (buffer->size + *len > buffer->capacity) {  // TODO: Find the right coeff instead
        buffer->data = realloc(buffer->data, buffer->capacity * 2);
        assert(buffer->data);
        buffer->capacity *= 2;
        if (buffer->back_index < buffer->front_index) {
            memcpy(buffer->data + buffer->front_index + buffer->size - buffer->back_index, buffer->data + buffer->back_index, buffer->back_index);
            buffer->back_index = buffer->front_index + buffer->size;
        }
    }
    size_t wrap_offset = buffer->capacity - buffer->back_index;
    *len = min(*len, wrap_offset);
    void *ptr = buffer->data + buffer->back_index;
    buffer->size += *len;
    buffer->back_index = (buffer->back_index + *len) % buffer->capacity;
    return ptr;
}

void *rapido_stream_buffer_trim_end(rapido_stream_buffer_t *buffer, size_t len) {
    buffer->back_index = (buffer->back_index - len) % buffer->capacity;
    buffer->size -= len;
    if (buffer->size == 0) {
        buffer->front_index = 0;
        buffer->back_index = 0;
    }
}

void rapido_stream_buffer_push(rapido_stream_buffer_t *buffer, void *input, size_t len) {
    size_t total_len = len;
    void *ptr = rapido_stream_buffer_alloc(buffer, &len);
    memcpy(ptr, input, len);
    if (len < total_len) {
        ptr = rapido_stream_buffer_alloc(buffer, &len);
        memcpy(ptr, input + len, total_len - len);
    }
}

void *rapido_stream_buffer_peek(rapido_stream_buffer_t *buffer, size_t *len) {
    size_t read_len = min(*len, buffer->size);
    *len = min(read_len, buffer->capacity - buffer->front_index);
    return buffer->data + buffer->front_index;
}

void *rapido_stream_buffer_get(rapido_stream_buffer_t *buffer, size_t *len) {
    void *ptr = rapido_stream_buffer_peek(buffer, len);
    buffer->front_index = (buffer->front_index + *len) % buffer->capacity;
    buffer->size -= *len;
    if (buffer->size == 0) {
        buffer->front_index = 0;
        buffer->back_index = 0;
    }
    return ptr;
}

void rapido_stream_buffer_free(rapido_stream_buffer_t *buffer) {
    if (buffer->capacity && buffer->data) {
        free(buffer->data);
    }
    memset(buffer, 0, sizeof(rapido_stream_buffer_t));
}

int rapido_add_range(rapido_range_list_t *list, uint64_t low, uint64_t high) {
    assert(low < high);
    assert(list->size < RANGES_LEN);
    bool merged = false;
    for (int i = 0; i < list->size && !merged; i++) {
        struct rapido_range_item *r = list->ranges + i;
        if (r->low <= low && low <= r->high) {
            /* Range overlaps, potentially extending the high limit */
            r->high = max(r->high, high);
            merged = true;
        }
        if (r->low <= high && high <= r->high) {
            /* Range overlaps, potentially extending the low limit */
            r->low = min(r->low, low);
            merged = true;
        }
        if (!merged && low < r->low) {
            /* Range is strictly before next range */
            memmove(list->ranges + i + 1, list->ranges + i, (list->size - i) * sizeof(struct rapido_range_item));
            list->size++;
            r->low = low;
            r->high = high;
            return 0;
        }
    }
    for (int i = 0; i + 1 < list->size; i++) {
        struct rapido_range_item *r = list->ranges + i;
        struct rapido_range_item *n = list->ranges + i + 1;
        if (r->high >= n->low) {
            /* Range overlaps next one */
            n->low = r->low;
            memmove(list->ranges + i, list->ranges + i + 1, (list->size - i - 1) * sizeof(struct rapido_range_item));
            list->size--;
            i--;
        }
    }
    if (!merged) {
        /* No range is inferior to this range */
        assert(list->size < RANGES_LEN);
        list->ranges[list->size].low = low;
        list->ranges[list->size].high = high;
        list->size++;
    }
    return 0;
}

void rapido_peek_range(rapido_range_list_t *list, uint64_t *low, uint64_t *high) {
    *low = 0;
    *high = 0;
    if (list->size > 0) {
        *low = list->ranges[0].low;
        *high = list->ranges[0].high;
    }
}

uint64_t rapido_trim_range(rapido_range_list_t *list, uint64_t limit) {
    uint64_t offset = -1;
    for (int i = 0; i < list->size; i++) {
        struct rapido_range_item *r = list->ranges + i;
        if (r->low <= limit && limit < r->high) {
            offset = limit;
            r->low = limit;
        } else if (r->high <= limit) {
            offset = r->high;
            memmove(list->ranges + i, list->ranges + i + 1, (list->size - i - 1) * sizeof(struct rapido_range_item));
            list->size--;
        }
    }
    return offset;
}

void rapido_stream_receive_buffer_init(rapido_stream_receive_buffer_t *receive, size_t capacity) {
    memset(receive, 0, sizeof(rapido_stream_receive_buffer_t));
    receive->buffer.data = malloc(capacity);
    assert(receive->buffer.data != NULL);
    receive->buffer.capacity = capacity;
}

int rapido_stream_receive_buffer_write(rapido_stream_receive_buffer_t *receive, size_t offset, void *input, size_t len) {
    assert(offset >= receive->read_offset);
    size_t write_offset = offset - receive->read_offset;
    if (write_offset + len > receive->buffer.capacity) {
        size_t new_cap = receive->buffer.capacity * 2;
        receive->buffer.data = reallocarray(receive->buffer.data, new_cap, 1);
        assert(receive->buffer.data);
        memcpy(receive->buffer.data + receive->buffer.capacity, receive->buffer.data, receive->buffer.capacity);
        receive->buffer.capacity = new_cap;
    }
    size_t real_offset = (receive->buffer.offset + write_offset) % receive->buffer.capacity;
    size_t wrap_offset = receive->buffer.capacity - write_offset;
    memcpy(receive->buffer.data + real_offset, input, min(len, wrap_offset));
    if (wrap_offset < len) {
        memcpy(receive->buffer.data, input + wrap_offset, len - wrap_offset);
    }

    rapido_add_range(&receive->ranges, offset, offset + len);
    return 0;
}

void *rapido_stream_receive_buffer_get(rapido_stream_receive_buffer_t *receive, size_t *len) {
    size_t limit = max(*len, receive->read_offset + *len);
    size_t read_offset = rapido_trim_range(&receive->ranges, limit);
    void *ptr = NULL;
    if (read_offset != -1) {
        *len = min(*len, read_offset - receive->read_offset);
        size_t wrap_offset = receive->buffer.capacity - receive->buffer.offset;
        *len = min(*len, wrap_offset);
        ptr = receive->buffer.data + receive->buffer.offset;
        receive->read_offset += *len;
        receive->buffer.offset = (receive->buffer.offset + *len) % receive->buffer.capacity;
    } else {
        *len = 0;
    }
    return ptr;
}

void rapido_stream_receive_buffer_free(rapido_stream_receive_buffer_t *receive) {
    if (receive->buffer.capacity && receive->buffer.data != NULL) {
        free(receive->buffer.data);
    }
    memset(receive, 0, sizeof(rapido_stream_receive_buffer_t));
}

typedef uint8_t rapido_frame_type_t;

static const rapido_frame_type_t padding_frame_type = 0x0;
static const rapido_frame_type_t ping_frame_type = 0x1;
static const rapido_frame_type_t stream_frame_type = 0x2;
static const rapido_frame_type_t ack_frame_type = 0x3;
static const rapido_frame_type_t new_session_id_frame_type = 0x4;
static const rapido_frame_type_t new_address_frame_type = 0x5;
static const rapido_frame_type_t connection_failed_frame_type = 0x6;
static const rapido_frame_type_t ebpf_code_frame_type = 0x7;

typedef struct {
    rapido_stream_id_t stream_id;
    uint64_t offset;
    size_t len;
    uint8_t fin;
    uint8_t *data;
} rapido_stream_frame_t;

typedef struct {
    rapido_connection_id_t connection_id;
    uint64_t last_record_acknowledged;
} rapido_ack_frame_t;

typedef struct {
    rapido_connection_id_t sequence;
    uint8_t *tls_session_id;
} rapido_new_session_id_frame_t;

typedef struct {
    rapido_address_id_t address_id;
    uint8_t family;
    uint8_t addr[16];
    uint16_t port;
} rapido_new_address_frame_t;

typedef struct {
    rapido_connection_id_t connection_id;
    uint32_t sequence;
} rapido_connection_failed_frame_t;

typedef struct {
    uint8_t *data;
    size_t len;
    size_t offset;
    rapido_connection_id_t connection_id;
} rapido_ebpf_code_frame_t;

int rapido_frame_is_ack_eliciting(rapido_frame_type_t frame_id) {
    return frame_id != padding_frame_type && frame_id != ack_frame_type;
}

rapido_t *rapido_new(ptls_context_t *tls_ctx, bool is_server, const char *server_name, FILE *qlog_out) {
    rapido_t *session = calloc(1, sizeof(rapido_t));
    assert(session != NULL);
    session->is_server = is_server;

    session->tls_ctx = tls_ctx;
    session->tls = ptls_new(session->tls_ctx, session->is_server);
    ptls_set_server_name(session->tls, server_name, 0);
    session->tls_properties.additional_extensions = malloc(sizeof(ptls_raw_extension_t) * 2);
    session->tls_properties.additional_extensions[0].type = TLS_RAPIDO_HELLO_EXT;
    session->tls_properties.additional_extensions[0].data = ptls_iovec_init(NULL, 0);
    session->tls_properties.additional_extensions[1].type = UINT16_MAX;
    session->tls_properties.collect_extension = collect_rapido_extensions;
    session->tls_properties.collected_extensions = collected_rapido_extensions;

    session->next_stream_id = is_server ? 1 : 0;

    session->connections.item_size = sizeof(rapido_connection_t);
    session->streams.item_size = sizeof(rapido_stream_t);
    session->local_addresses.item_size = sizeof(struct sockaddr_storage);
    session->remote_addresses.item_size = sizeof(struct sockaddr_storage);
    session->tls_session_ids.item_size = TLS_SESSION_ID_LEN;
    rapido_queue_init(&session->pending_notifications, sizeof(rapido_application_notification_t), 256);

    if (session->is_server) {
        session->server.listen_sockets.item_size = sizeof(int);
        session->server.pending_connections.item_size = sizeof(rapido_pending_connection_t);
    }

    session->qlog.out = qlog_out;
    session->qlog.reference_time = get_time();
    QLOG(session, "api", "rapido_new", "", "{\"is_server\": %d, \"server_name\": \"%s\"}", is_server, server_name);
    return session;
}

rapido_address_id_t rapido_add_address(rapido_t *session, struct sockaddr *local_address, socklen_t local_address_len) {
    assert(local_address != NULL);
    assert(local_address_len == sizeof(struct sockaddr_in) || local_address_len == sizeof(struct sockaddr_in6));
    rapido_address_id_t local_address_id = session->next_local_address_id++;
    memcpy(rapido_array_add(&session->local_addresses, local_address_id), local_address, local_address_len);
    // TODO: Send it
    if (session->is_server) {  // TODO: Ipv6 dualstack compat mode ?
        int listen_fd = socket(local_address->sa_family, SOCK_STREAM | SOCK_NONBLOCK, 0);
        assert_perror(listen_fd == -1);
        memcpy(rapido_array_add(&session->server.listen_sockets, local_address_id), &listen_fd, sizeof(listen_fd));
        int yes = 1;
        assert_perror(setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)));
        //TODO Make it a poll struct rather
        assert_perror(bind(listen_fd, local_address, local_address->sa_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6)));
        assert_perror(listen(listen_fd, SOMAXCONN));
    }
    LOG {
        char a[INET6_ADDRSTRLEN];
        QLOG(session, "api", "rapido_add_address", "", "{\"local_address_id\": \"%d\", \"local_address\": \"%s:%d\"}", local_address_id,
             inet_ntop(local_address->sa_family, (local_address->sa_family == AF_INET ? (void *) &((struct sockaddr_in *) local_address)->sin_addr : &((struct sockaddr_in6 *) local_address)->sin6_addr), a, sizeof(a)),
             ntohs(local_address->sa_family == AF_INET ? ((struct sockaddr_in *) local_address)->sin_port : ((struct sockaddr_in6 *) local_address)->sin6_port));
    }
    return local_address_id;
}

rapido_address_id_t rapido_add_remote_address(rapido_t *session, struct sockaddr *remote_address, socklen_t remote_address_len) {
    assert(remote_address != NULL);
    assert(remote_address_len == sizeof(struct sockaddr_in) || remote_address_len == sizeof(struct sockaddr_in6));
    rapido_address_id_t remote_address_id = session->next_remote_address_id++;
    memcpy(rapido_array_add(&session->remote_addresses, remote_address_id), remote_address, remote_address_len);
    LOG {
        char a[INET6_ADDRSTRLEN];
        QLOG(session, "api", "rapido_add_remote_address", "", "{\"local_address_id\": \"%d\", \"local_address\": \"%s:%d\"}", remote_address_id,
             inet_ntop(remote_address->sa_family, remote_address->sa_family == AF_INET ? (void *) &((struct sockaddr_in *) remote_address)->sin_addr : &((struct sockaddr_in6 *) remote_address)->sin6_addr, a, sizeof(a)),
             ntohs(remote_address->sa_family == AF_INET ? ((struct sockaddr_in *) remote_address)->sin_port : ((struct sockaddr_in6 *) remote_address)->sin6_port));
    }
    return remote_address_id;
}

int rapido_remove_address(rapido_t *session, rapido_address_id_t local_address_id) {
    rapido_array_iter(&session->connections, rapido_connection_t *connection,{
        if (connection->local_address_id == local_address_id) {
            WARNING("Local address %d of connection %d is removed\n", local_address_id, connection->connection_id);
            // TODO: Migrate streams and RTX state for this connection
        }
    });
    // TODO: Send it
    if (session->is_server) {
        int *fd = rapido_array_get(&session->server.listen_sockets, local_address_id);
        if (fd != NULL) {
            assert_perror(close(*fd));
            rapido_array_delete(&session->server.listen_sockets, local_address_id);
        }
    }
    QLOG(session, "api", "rapido_remove_address", "", "{\"local_address_id\": \"%d\"}", local_address_id);
    return rapido_array_delete(&session->local_addresses, local_address_id);
}

rapido_connection_id_t rapido_create_connection(rapido_t *session, uint8_t local_address_id, uint8_t remote_address_id) {
    assert(!session->is_server);
    struct sockaddr* local_address = (struct sockaddr *)rapido_array_get(&session->local_addresses, local_address_id);
    struct sockaddr* remote_address = (struct sockaddr *)rapido_array_get(&session->remote_addresses, remote_address_id);
    assert(local_address != NULL || local_address_id == session->next_local_address_id);
    assert(remote_address != NULL);
    assert(local_address == NULL || local_address->sa_family == remote_address->sa_family);

    rapido_connection_id_t connection_id = session->next_connection_id++;
    uint8_t *tls_session_id = rapido_array_get(&session->tls_session_ids, connection_id);
    assert(connection_id == 0 || tls_session_id != NULL);

    rapido_connection_t *connection = rapido_array_add(&session->connections, connection_id);
    memset(connection, 0, sizeof(rapido_connection_t));
    connection->connection_id = connection_id;
    connection->local_address_id = local_address_id;
    connection->remote_address_id = remote_address_id;
    rapido_stream_buffer_init(&connection->receive_buffer, 32 * TLS_MAX_RECORD_SIZE);
    rapido_stream_buffer_init(&connection->send_buffer, 32 * TLS_MAX_RECORD_SIZE);

    connection->socket = socket(remote_address->sa_family, SOCK_STREAM | SOCK_NONBLOCK, 0);
    assert_perror(connection->socket == -1);
    int yes = 1;
    assert_perror(setsockopt(connection->socket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)));
    if (local_address != NULL) {
        assert_perror(bind(connection->socket, local_address, local_address->sa_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6)));
    }
    int ret = connect(connection->socket, remote_address, remote_address->sa_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6));
    if (ret && errno != EINPROGRESS) {
        assert_perror(ret);
    }

    if (local_address == NULL) {
        local_address = rapido_array_add(&session->local_addresses, local_address_id);
        session->next_local_address_id++;
        socklen_t local_address_len = sizeof(struct sockaddr_storage);
        assert_perror(getsockname(connection->socket, local_address, &local_address_len));
        LOG {
            char a[INET6_ADDRSTRLEN];
            QLOG(session, "network", "new_local_address", "", "{\"local_address_id\": \"%d\", \"local_address\": \"%s:%d\"}", local_address_id,
                 inet_ntop(local_address->sa_family, (local_address->sa_family == AF_INET ? (void *) &((struct sockaddr_in *) local_address)->sin_addr : &((struct sockaddr_in6 *) local_address)->sin6_addr), a, sizeof(a)),
                 ntohs(local_address->sa_family == AF_INET ? ((struct sockaddr_in *) local_address)->sin_port : ((struct sockaddr_in6 *) local_address)->sin6_port));
        }
    }

    ptls_buffer_t handshake_buffer = { 0 };
    ptls_buffer_init(&handshake_buffer, "", 0);
    if (connection_id == 0) {
        connection->tls = session->tls;
        session->tls_ctx->random_bytes(rapido_array_add(&session->tls_session_ids, 0), TLS_SESSION_ID_LEN);
        session->tls_properties.client.tls_session_id = ptls_iovec_init(rapido_array_get(&session->tls_session_ids, 0), TLS_SESSION_ID_LEN);
        ret = ptls_handshake(session->tls, &handshake_buffer, NULL, 0, &session->tls_properties);
    } else {
        connection->tls = ptls_new(session->tls_ctx, session->is_server);
        ptls_set_server_name(connection->tls, ptls_get_server_name(session->tls), 0);
        ptls_handshake_properties_t tls_properties = { 0 };
        tls_properties.client.tls_session_id = ptls_iovec_init(rapido_array_get(&session->tls_session_ids, connection_id), TLS_SESSION_ID_LEN);
        ret = ptls_handshake(connection->tls, &handshake_buffer, NULL, 0, &tls_properties);
    }

    assert(ret == PTLS_ERROR_IN_PROGRESS);
    assert(send(connection->socket, handshake_buffer.base, handshake_buffer.off, 0) == handshake_buffer.off);
    ptls_buffer_dispose(&handshake_buffer);

    QLOG(session, "api", "rapido_create_connection", "", "{\"connection_id\": \"%d\", \"local_address_id\": \"%d\", \"remote_address_id\": \"%d\"}", connection_id, local_address_id, remote_address_id);
    return connection_id;
}

rapido_stream_id_t rapido_open_stream(rapido_t *session) {
    rapido_stream_id_t next_stream_id = session->next_stream_id;
    session->next_stream_id += 2;
    rapido_stream_t *stream = rapido_array_add(&session->streams, next_stream_id);
    memset(stream, 0, sizeof(rapido_stream_t));
    stream->stream_id = next_stream_id;
    rapido_stream_receive_buffer_init(&stream->read_buffer, 2 * TLS_MAX_RECORD_SIZE);
    rapido_stream_buffer_init(&stream->send_buffer, 2 * TLS_MAX_RECORD_SIZE);
    QLOG(session, "api", "rapido_open_stream", "", "{\"stream_id\": \"%d\"}", next_stream_id);
    return next_stream_id;
}
int rapido_attach_stream(rapido_t *session, rapido_stream_id_t stream_id, rapido_connection_id_t connection_id) {
    assert(stream_id < SET_LEN && connection_id < SET_LEN);
    rapido_stream_t *stream = rapido_array_get(&session->streams, stream_id);
    assert(stream != NULL);
    rapido_connection_t *connection = rapido_array_get(&session->connections, connection_id);
    assert(connection != NULL);

    SET_ADD(stream->connections, connection_id);
    SET_ADD(connection->attached_streams, stream_id);
    QLOG(session, "api", "rapido_attach_stream", "", "{\"stream_id\": \"%d\", \"connection_id\": \"%d\"}", stream_id, connection_id);
    return 0;
}
int rapido_remove_stream(rapido_t *session, rapido_stream_id_t stream_id, rapido_connection_id_t connection_id) {
    assert(stream_id < SET_LEN && connection_id < SET_LEN);
    rapido_stream_t *stream = rapido_array_get(&session->streams, stream_id);
    assert(stream != NULL);
    rapido_connection_t *connection = rapido_array_get(&session->connections, connection_id);
    assert(connection != NULL);

    SET_REMOVE(stream->connections, connection_id);
    SET_REMOVE(connection->attached_streams, stream_id);
    QLOG(session, "api", "rapido_remove_stream", "", "{\"stream_id\": \"%d\", \"connection_id\": \"%d\"}", stream_id, connection_id);
    return 0;
}
int rapido_add_to_stream(rapido_t *session, rapido_stream_id_t stream_id, void *data, size_t len) {
    rapido_stream_t *stream = rapido_array_get(&session->streams, stream_id);
    assert(stream != NULL);
    rapido_stream_buffer_push(&stream->send_buffer, data, len);
    QLOG(session, "api", "rapido_add_to_stream", "", "{\"stream_id\": \"%d\", \"len\": \"%zu\"}", stream_id, len);
    return 0;
}
int rapido_set_stream_producer(rapido_t *session, rapido_stream_id_t stream_id, rapido_stream_producer_t producer, void *producer_ctx) {
    rapido_stream_t *stream = rapido_array_get(&session->streams, stream_id);
    assert(stream != NULL);
    stream->producer = producer;
    stream->producer_ctx = producer_ctx;
    QLOG(session, "api", "rapido_set_stream_producer", "", "{\"stream_id\": \"%d\"}", stream_id);
    return 0;
}
void *rapido_read_stream(rapido_t *session, rapido_stream_id_t stream_id, size_t *len) {
    rapido_stream_t *stream = rapido_array_get(&session->streams, stream_id);
    assert(stream != NULL);
    QLOG(session, "api", "rapido_read_stream", "", "{\"stream_id\": \"%d\", \"len\": \"%zu\"}", stream_id, *len);
    return rapido_stream_receive_buffer_get(&stream->read_buffer, len);
}
int rapido_close_stream(rapido_t *session, rapido_stream_id_t stream_id) {
    rapido_stream_t *stream = rapido_array_get(&session->streams, stream_id);
    assert(stream != NULL);
    assert(!stream->fin_set);
    stream->fin_set = true;
    stream->write_fin = stream->write_offset + stream->send_buffer.size;
    QLOG(session, "api", "rapido_close_stream", "", "{\"stream_id\": \"%d\", \"fin_offset\": \"%zu\"}", stream_id, stream->write_fin);
    return 0;
}
int rapido_prepare_stream_frame(rapido_t *session, rapido_stream_t *stream, uint8_t *buf, size_t *len) {
    // TODO: Handle ACK/RTX buffers
    size_t stream_header_len = sizeof(rapido_frame_type_t) + sizeof(rapido_stream_id_t) + (2 * sizeof(uint64_t));
    assert(*len > 1 + stream_header_len);
    size_t consumed = 0;
    size_t payload_len = min(*len, TLS_MAX_RECORD_SIZE) - 1 - stream_header_len;
    // TODO: Handle when the buffer returns a smaller pointer due to buffer cycling
    void *stream_data = rapido_stream_buffer_get(&stream->send_buffer, &payload_len);
    bool fin = stream->fin_set && stream->write_offset + payload_len == stream->write_fin;
    if (payload_len == 0 && !fin) {
        *len = 0;
        return 0;
    }

    *(uint8_t *)(buf + consumed) = stream_frame_type;
    consumed += sizeof(rapido_frame_type_t);
    *(rapido_stream_id_t *)(buf + consumed) = htobe32(stream->stream_id);
    consumed += sizeof(rapido_stream_id_t);
    *(uint64_t *)(buf + consumed) = htobe64(stream->write_offset);
    consumed += sizeof(uint64_t);
    *(uint64_t *)(buf + consumed) = htobe64(payload_len);
    consumed += sizeof(uint64_t);
    *(uint8_t *)(buf + consumed) = fin;
    consumed += sizeof(uint8_t);
    memcpy(buf + consumed, stream_data, payload_len);
    consumed += payload_len;

    QLOG(session, "frames", "prepare_stream_frame", "", "{\"stream_id\": \"%d\", \"offset\": \"%lu\", \"len\": \"%lu\", \"fin\": %d}", stream->stream_id, stream->write_offset, payload_len, fin);

    if (fin) {
        stream->fin_sent = true;
    }
    stream->write_offset += payload_len;
    *len = consumed;
    return 0;
}

int rapido_decode_stream_frame(rapido_t *session, uint8_t *buf, size_t *len, rapido_stream_frame_t *frame) {
    size_t stream_header_len = sizeof(rapido_frame_type_t) + sizeof(rapido_stream_id_t) + (2 * sizeof(uint64_t));
    assert(buf[0] == stream_frame_type);
    assert(*len > stream_header_len);
    size_t consumed = 1;
    frame->stream_id = be32toh(*(rapido_stream_id_t *)(buf + consumed));
    consumed += sizeof(rapido_stream_id_t);
    frame->offset = be64toh(*(uint64_t *)(buf + consumed));
    consumed += sizeof(uint64_t);
    frame->len = be64toh(*(uint64_t *)(buf + consumed));
    consumed += sizeof(uint64_t);
    frame->fin = buf[consumed];
    consumed += sizeof(uint8_t);
    frame->data = buf + consumed;
    consumed += frame->len;
    *len = consumed;
    QLOG(session, "frames", "decode_stream_frame", "", "{\"stream_id\": \"%d\", \"offset\": \"%lu\", \"len\": \"%lu\", \"fin\": %d}", frame->stream_id, frame->offset, frame->len, frame->fin);
    return 0;
}

int rapido_process_stream_frame(rapido_t *session, rapido_stream_frame_t *frame) {
    rapido_stream_t *stream = rapido_array_get(&session->streams, frame->stream_id);
    if (stream == NULL) {
        assert(STREAM_IS_CLIENT(frame->stream_id) == session->is_server);
        stream = rapido_array_add(&session->streams, frame->stream_id);
        memset(stream, 0, sizeof(rapido_stream_t));
        stream->stream_id = frame->stream_id;
        rapido_stream_receive_buffer_init(&stream->read_buffer, 2 * TLS_MAX_RECORD_SIZE);
        rapido_stream_buffer_init(&stream->send_buffer, 2 * TLS_MAX_RECORD_SIZE);
        rapido_application_notification_t *notification = rapido_queue_push(&session->pending_notifications);
        notification->notification_type = rapido_new_stream;
        notification->stream_id = frame->stream_id;
    }
    assert(!stream->fin_received || (frame->offset + frame->len <= stream->read_fin));
    assert(!frame->fin || !stream->fin_received);
    assert(frame->len > 0 || frame->fin);
    if (frame->len) {
        rapido_stream_receive_buffer_write(&stream->read_buffer, frame->offset, frame->data, frame->len);
        rapido_application_notification_t *notification = rapido_queue_push(&session->pending_notifications);
        notification->notification_type = rapido_stream_has_data;
        notification->stream_id = frame->stream_id;
    }
    if (frame->fin) {
        stream->fin_received = frame->fin;
        stream->read_fin = frame->offset + frame->len;
        rapido_application_notification_t *notification = rapido_queue_push(&session->pending_notifications);
        notification->notification_type = rapido_stream_closed;
        notification->stream_id = frame->stream_id;
    }
    return 0;
}

int rapido_prepare_new_session_id_frame(rapido_t *session, uint8_t *tls_session_id, rapido_connection_id_t sequence, uint8_t *buf, size_t *len) {
    size_t new_session_id_len = sizeof(rapido_frame_type_t) + sizeof(rapido_connection_id_t) + 32;
    assert(*len > 1 + new_session_id_len);
    size_t consumed = 0;

    *(uint8_t *)(buf + consumed) = new_session_id_frame_type;
    consumed += sizeof(rapido_frame_type_t);
    *(uint32_t *)(buf + consumed) = htobe32(sequence);
    consumed += sizeof(uint32_t);
    memcpy(buf + consumed, tls_session_id, TLS_SESSION_ID_LEN);
    consumed += TLS_SESSION_ID_LEN;

    QLOG(session, "frames", "prepare_new_session_id_frame", "", "{\"sequence\": \"%d\"}", sequence);

    *len = consumed;
    return 0;
}

int rapido_decode_new_session_id_frame(rapido_t *session, uint8_t *buf, size_t *len, rapido_new_session_id_frame_t *frame) {
    size_t new_session_id_len = sizeof(rapido_frame_type_t) + sizeof(rapido_connection_id_t) + 32;
    assert(*len >= new_session_id_len);
    size_t consumed = 1;
    frame->sequence = be32toh(*(rapido_connection_id_t *)(buf + consumed));
    consumed += sizeof(rapido_connection_id_t);
    frame->tls_session_id = buf + consumed;
    consumed += TLS_SESSION_ID_LEN;

    LOG {
        char tls_session_id_str[TLS_SESSION_ID_LEN * 2 + 1];
        tohex(frame->tls_session_id, TLS_SESSION_ID_LEN, tls_session_id_str);
        QLOG(session, "frames", "decode_new_session_id_frame", "", "{\"sequence\": \"%d\", \"session_id\": \"%s\"}",
             frame->sequence, tls_session_id_str);
    }

    *len = consumed;
    return 0;
}

int rapido_process_new_session_id_frame(rapido_t *session, rapido_new_session_id_frame_t *frame) {
    assert(!session->is_server);
    memcpy(rapido_array_add(&session->tls_session_ids, frame->sequence), frame->tls_session_id, TLS_SESSION_ID_LEN);
    return 0;
}

int rapido_connection_wants_to_send(rapido_t *session, rapido_connection_t *connection) {
    if (!ptls_handshake_is_complete(connection->tls)) {
        return 0;
    }
    int wants_to_send = 0;

    wants_to_send |= connection->send_buffer.size > 0;

    size_t streams_to_write = SET_SIZE(connection->attached_streams);
    for (int i = 0; !wants_to_send && streams_to_write && i < SET_LEN; i++) {
        if (SET_HAS(connection->attached_streams, i)) {
            rapido_stream_t *stream = rapido_array_get(&session->streams, i);
            if (!stream->fin_set && stream->send_buffer.size < TLS_MAX_RECORD_SIZE && stream->producer) {
                stream->producer(session, stream->stream_id, stream->producer_ctx);
            }
            wants_to_send |= stream->send_buffer.size || (stream->fin_set && !stream->fin_sent);
        }
    }

    if (!wants_to_send && session->is_server) {
        wants_to_send |= session->tls_session_ids.size - session->server.tls_session_ids_sent > 0;
    }
    return wants_to_send;
}

int rapido_prepare_record(rapido_t *session, rapido_connection_t *connection, uint8_t *cleartext, size_t *len) {
    assert(*len >= TLS_MAX_RECORD_SIZE);
    size_t consumed = 0;

    if (session->is_server) {
        for (int i = session->server.tls_session_ids_sent; consumed < TLS_MAX_RECORD_SIZE && i < session->tls_session_ids.size; i++) {
            size_t frame_len = *len - consumed;
            rapido_prepare_new_session_id_frame(session, rapido_array_get(&session->tls_session_ids, i), i, cleartext + consumed, &frame_len);
            if (frame_len > 0) {
                consumed += frame_len;
                session->server.tls_session_ids_sent++;
            }
        }
    }

    size_t streams_to_write = SET_SIZE(connection->attached_streams);
    for (int i = 0; consumed < TLS_MAX_RECORD_SIZE && i < SET_LEN && streams_to_write; i++) {
        if (SET_HAS(connection->attached_streams, i)) {
            rapido_stream_t *stream = rapido_array_get(&session->streams, i);
            if (stream->send_buffer.size || (stream->fin_set && !stream->fin_sent)) {
                size_t frame_len = *len - consumed;
                assert(rapido_prepare_stream_frame(session, stream, cleartext + consumed, &frame_len) == 0);
                consumed += frame_len;
            } else {
                streams_to_write--;
            }
        }
    }

    assert(consumed <= TLS_MAX_RECORD_SIZE);
    *len = consumed;
    return 0;
}

int rapido_run_network(rapido_t *session) {
    // TODO: Read and writes until it blocks
    QLOG(session, "api", "rapido_run_network", "", NULL);
    int no_fds = 0;
do {
        size_t nfds = session->connections.size;
        if (session->is_server) {
            nfds += session->server.listen_sockets.size + session->server.pending_connections.size;
        }
        struct pollfd fds[nfds];
        size_t connections_index[nfds];
        enum {
            fd_listen_socket,
            fd_pending_connection,
            fd_connection
        } fd_types[nfds];
        nfds = 0;
        if (session->is_server) {
            rapido_array_iter(&session->server.listen_sockets, int *socket, {
                fds[nfds].fd = *socket;
                fds[nfds].events = POLLIN;
                connections_index[nfds] = -1;
                fd_types[nfds] = fd_listen_socket;
                nfds++;
            });
            rapido_array_iter(&session->server.pending_connections, rapido_pending_connection_t * connection, {
                fds[nfds].fd = connection->socket;
                fds[nfds].events = POLLIN;
                connections_index[nfds] = i;
                fd_types[nfds] = fd_pending_connection;
                nfds++;
            });
        }
        rapido_array_iter(&session->connections, rapido_connection_t * connection, {
            fds[nfds].fd = connection->socket;
            fds[nfds].events = POLLIN;
            if (rapido_connection_wants_to_send(session, connection)) {
                fds[nfds].events |= POLLOUT;
            }
            connections_index[nfds] = i;
            fd_types[nfds] = fd_connection;
            nfds++;
        });

        int polled_fds = poll(fds, nfds, no_fds > 0 ? 1000 : 0);
        assert(polled_fds >= 0 || errno == EINTR);
        if (polled_fds == 0) {
            no_fds++;
        } else {
            no_fds = 0;
        }

        size_t fd_offset = 0;
        /* Accept new TCP connections and prepare the TLS handshake */
        while (fd_offset < nfds && fd_types[fd_offset] == fd_listen_socket) {
            if (fds[fd_offset].revents & POLLIN) {
                struct sockaddr_storage remote_address;
                socklen_t remote_address_len = sizeof(remote_address_len);
                int conn_fd = accept(fds[fd_offset].fd, (struct sockaddr *)&remote_address, &remote_address_len);
                assert_perror(conn_fd == -1);
                assert_perror(fcntl(conn_fd, F_SETFL, O_NONBLOCK));
                rapido_pending_connection_t *pending_connection =
                    rapido_array_add(&session->server.pending_connections, session->server.next_pending_connection++);
                memset(pending_connection, 0, sizeof(rapido_pending_connection_t));
                pending_connection->socket = conn_fd;
                pending_connection->tls_ctx = session->tls_ctx;
                if (!ptls_handshake_is_complete(session->tls)) {
                    pending_connection->tls = session->tls;
                } else {
                    pending_connection->tls = ptls_new(session->tls_ctx, session->is_server);
                    ptls_set_server_name(pending_connection->tls, ptls_get_server_name(session->tls), 0);
                }
                polled_fds--;
            }
            fd_offset++;
        }

        /* Do the TLS handshake on pending connections */
        while (fd_offset < nfds && fd_types[fd_offset] == fd_pending_connection) {
            if (fds[fd_offset].revents & POLLIN) {
                rapido_pending_connection_t *connection =
                    rapido_array_get(&session->server.pending_connections, connections_index[fd_offset]);
                uint8_t recvbuf[TLS_MAX_ENCRYPTED_RECORD_SIZE];
                assert(fds[fd_offset].fd > -1);
                size_t recvd = recv(fds[fd_offset].fd, recvbuf, sizeof(recvbuf), 0);
                assert_perror(recvd == -1 && (errno == EWOULDBLOCK || errno == EAGAIN));
                ptls_buffer_t handshake_buffer = {0};
                ptls_buffer_init(&handshake_buffer, "", 0);
                uint8_t tls_session_id_buf[TLS_SESSION_ID_LEN];
                session->tls_properties.server.tls_session_id = ptls_iovec_init(tls_session_id_buf, sizeof(tls_session_id_buf));
                size_t consumed = recvd;
                int ret = ptls_handshake(connection->tls, &handshake_buffer, recvbuf, &consumed, &session->tls_properties);
                if (consumed < recvd) {
                    assert(!"More data after handshake");
                }
                if (ret == PTLS_ERROR_IN_PROGRESS) {
                    assert(!"Fragmented handshake"); // TODO: Handle fragmented handshake
                } else {
                    if (ret == 0) {
                        /* ClientHello */
                        if (!ptls_handshake_is_complete(session->tls)) {
                            assert(session->tls_properties.server.tls_session_id.len > 0);
                            assert(session->tls_properties.collected_extensions == NULL);
                            bool has_rapido_hello = false;
                            for (ptls_raw_extension_t *extension = session->tls_properties.additional_extensions;
                                 extension->type != UINT16_MAX && !has_rapido_hello; extension++) {
                                if (extension->type == TLS_RAPIDO_HELLO_EXT) {
                                    has_rapido_hello = true;
                                }
                            }
                            assert(has_rapido_hello);
                            session->tls_properties.collected_extensions = collected_rapido_extensions;
                            free(session->tls_properties.additional_extensions);
                            session->tls_properties.additional_extensions = malloc(sizeof(ptls_raw_extension_t) * 2);
                            session->tls_properties.additional_extensions[0].type = TLS_RAPIDO_HELLO_EXT;
                            session->tls_properties.additional_extensions[0].data = ptls_iovec_init(NULL, 0);
                            session->tls_properties.additional_extensions[1].type = UINT16_MAX;
                        }
                        if (!ptls_handshake_is_complete(connection->tls)) {
                            memcpy(connection->tls_session_id, session->tls_properties.server.tls_session_id.base,
                                   session->tls_properties.server.tls_session_id.len);
                        }
                        assert(send(connection->socket, handshake_buffer.base, handshake_buffer.off, 0) ==
                               handshake_buffer.off);
                        ptls_buffer_dispose(&handshake_buffer);
                        /* ClientFinished */
                        if (ptls_handshake_is_complete(connection->tls)) {
                            int tls_session_id_sequence = -1;
                            rapido_array_iter(&session->tls_session_ids, uint8_t * tls_session_id, {
                                if (memcmp(tls_session_id, connection->tls_session_id, TLS_SESSION_ID_LEN) == 0) {
                                    tls_session_id_sequence = i;
                                    break;
                                }
                            });
                            if (tls_session_id_sequence == -1) {
                                assert(session->tls_session_ids.size == 0);
                                memcpy(rapido_array_add(&session->tls_session_ids, 0), connection->tls_session_id,
                                       TLS_SESSION_ID_LEN);
                                session->server.tls_session_ids_sent = 1;
                                tls_session_id_sequence = 0;

                                for (int sid = 1; sid < DEFAULT_TCPLS_SESSION_ID_AMOUNT; sid++) {
                                    session->tls_ctx->random_bytes(rapido_array_add(&session->tls_session_ids, sid),
                                                                   TLS_SESSION_ID_LEN);
                                }
                            }
                            rapido_connection_t *new_connection = rapido_array_add(&session->connections, tls_session_id_sequence);
                            memset(new_connection, 0, sizeof(rapido_connection_t));
                            new_connection->socket = connection->socket;
                            new_connection->connection_id = tls_session_id_sequence;
                            new_connection->tls = session->tls;
                            rapido_stream_buffer_init(&new_connection->receive_buffer, 32 * TLS_MAX_RECORD_SIZE);
                            rapido_stream_buffer_init(&new_connection->send_buffer, 32 * TLS_MAX_RECORD_SIZE);
                            assert(setup_connection_crypto_context(session, new_connection) == 0);
                            // TODO: Find the addresses it uses
                            rapido_application_notification_t *notification = rapido_queue_push(&session->pending_notifications);
                            notification->notification_type = rapido_new_connection;
                            notification->connection_id = new_connection->connection_id;
                            if (connection->tls != session->tls) {
                                ptls_free(connection->tls);
                            }
                            rapido_array_delete(&session->server.pending_connections, connections_index[fd_offset]);
                        }
                    } else {
                        WARNING("Pending connection %zu returned pTLS error code %d during handshake\n",
                                connections_index[fd_offset], ret);
                        close(connection->socket);
                        rapido_array_delete(&session->server.pending_connections, connections_index[fd_offset]);
                    }
                }
                polled_fds--;
            }
            fd_offset++;
        }

        int wants_to_read;
        do {
            wants_to_read = 0;
            for (int i = fd_offset; i < nfds; i++) {
                rapido_connection_t *connection = rapido_array_get(&session->connections, connections_index[i]);
                /* Read incoming TLS records */
                if (fds[i].revents & POLLIN) {
                    size_t recvbuf_max = 32 * TLS_MAX_ENCRYPTED_RECORD_SIZE;
                    size_t recvd = recv(fds[i].fd, rapido_stream_buffer_alloc(&connection->receive_buffer, &recvbuf_max), recvbuf_max, 0);
                    if (recvd == -1 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
                        fds[i].revents &= ~(POLLIN);
                        rapido_stream_buffer_trim_end(&connection->receive_buffer, recvbuf_max);
                        continue;
                    } else if (recvd == 0 || errno == EPIPE) {
                        fds[i].revents &= ~(POLLIN);
                        close(connection->socket);
                        connection->socket = -1;
                        rapido_application_notification_t *notification = rapido_queue_push(&session->pending_notifications);
                        notification->notification_type = rapido_connection_closed;
                        notification->connection_id = connection->connection_id;
                        // TODO: Properly close the connection
                        continue;
                    }
                    rapido_stream_buffer_trim_end(&connection->receive_buffer, recvbuf_max - recvd);
                    size_t consumed = 0;
                    recvd = UINT64_MAX;
                    uint8_t *recvbuf = rapido_stream_buffer_peek(&connection->receive_buffer, &recvd);
                    wants_to_read |= 1;
                    if (!session->is_server && !ptls_handshake_is_complete(connection->tls)) {
                        ptls_buffer_t handshake_buffer = {0};
                        ptls_buffer_init(&handshake_buffer, "", 0);
                        consumed = recvd;
                        int ret = ptls_handshake(connection->tls, &handshake_buffer, recvbuf, &consumed, &session->tls_properties);
                        if (ret == PTLS_ERROR_IN_PROGRESS || ret == PTLS_ALERT_DECODE_ERROR) {
                            assert(!"Fragmented handshake"); // TODO: Handle fragmented handshake
                        }
                        assert(ret == 0);
                        assert(session->tls_properties.collected_extensions == NULL);
                        bool has_rapido_hello = false;
                        for (ptls_raw_extension_t *extension = session->tls_properties.additional_extensions;
                             extension->type != UINT16_MAX && !has_rapido_hello; extension++) {
                            if (extension->type == TLS_RAPIDO_HELLO_EXT) {
                                has_rapido_hello = true;
                            }
                        }
                        assert(has_rapido_hello || ptls_handshake_is_complete(session->tls));
                        assert(send(fds[i].fd, handshake_buffer.base, handshake_buffer.off, 0) ==
                               handshake_buffer.off);
                        ptls_buffer_dispose(&handshake_buffer);
                        session->tls_properties.collected_extensions = collected_rapido_extensions;
                        free(session->tls_properties.additional_extensions);
                        session->tls_properties.additional_extensions = NULL;
                        if (connection->tls != session->tls) {
                            ptls_free(connection->tls);
                            connection->tls = session->tls;
                        }
                        assert(setup_connection_crypto_context(session, connection) == 0);
                    }
                    if (ptls_handshake_is_complete(connection->tls) && consumed < recvd) {
                        ptls_set_traffic_protection(session->tls, connection->decryption_ctx, 1);
                        size_t recvd_offset = consumed;
                        while (recvd_offset < recvd) {
                            uint8_t plaintext_buf[TLS_MAX_ENCRYPTED_RECORD_SIZE];
                            ptls_buffer_t plaintext = {0};
                            ptls_buffer_init(&plaintext, plaintext_buf, sizeof(plaintext_buf));
                            consumed = recvd - recvd_offset;
                            int ret = ptls_receive(session->tls, &plaintext, recvbuf + recvd_offset, &consumed);
                            recvd_offset += consumed;
                            if (ret == PTLS_ERROR_IN_PROGRESS) {
                                assert(!"Fragmented data"); // TODO: Handle fragmented data
                            }
                            assert(ret == 0);
                            if (plaintext.off > 0) {
                                QLOG(session, "transport", "receive_record", "",
                                     "{\"connection_id\": \"%zu\", \"record_sequence\": \"%lu\", \"ciphertext_len\": \"%zu\"}",
                                     connections_index[i], ptls_get_traffic_protection(session->tls, 1)->seq - 1, consumed);
                            }
                            for (size_t offset = 0; offset < plaintext.off;) {
                                rapido_frame_type_t frame_type = plaintext.base[offset];
                                size_t len = plaintext.off - offset;
                                switch (frame_type) {
                                case stream_frame_type: {
                                    rapido_stream_frame_t frame;
                                    assert(rapido_decode_stream_frame(session, plaintext.base + offset, &len, &frame) == 0);
                                    assert(rapido_process_stream_frame(session, &frame) == 0);
                                } break;
                                case new_session_id_frame_type: {
                                    rapido_new_session_id_frame_t frame;
                                    assert(rapido_decode_new_session_id_frame(session, plaintext.base + offset, &len, &frame) == 0);
                                    assert(rapido_process_new_session_id_frame(session, &frame) == 0);
                                } break;
                                default:
                                    WARNING("Unsupported frame type: %d\n", frame_type);
                                    assert(!"Unsupported frame type");
                                    offset = plaintext.off;
                                    break;
                                }
                                offset += len;
                            }
                            consumed = recvd_offset;
                        }
                        connection->decryption_ctx->seq = ptls_get_traffic_protection(session->tls, 1)->seq;
                        connection->stats.bytes_received += recvd;
                    }
                    size_t len = consumed;
                    rapido_stream_buffer_get(&connection->receive_buffer, &len);
                    if (len < consumed) {
                        len = consumed - len;
                        rapido_stream_buffer_get(&connection->receive_buffer, &len);
                    }
                }
            }
        } while (polled_fds && wants_to_read && session->pending_notifications.size < session->pending_notifications.capacity / 2);

        /* Write outgoing TLS records */
        int wants_to_write = 0;
        rapido_array_iter(&session->connections, rapido_connection_t * connection, {
            if (rapido_connection_wants_to_send(session, connection)) {
                wants_to_write = 1;
                break;
            }
        });
        while (polled_fds && wants_to_write && session->pending_notifications.size < session->pending_notifications.capacity / 2) {
            wants_to_write = 0;
            for (int i = fd_offset; i < nfds; i++) {
                if (fds[i].revents & POLLOUT) {
                    rapido_connection_t *connection = rapido_array_get(&session->connections, connections_index[i]);
                    if (connection->send_buffer.size == 0) {
                        uint8_t cleartext[TLS_MAX_RECORD_SIZE];
                        size_t cleartext_len = sizeof(cleartext);
                        rapido_prepare_record(session, connection, cleartext, &cleartext_len);
                        if (cleartext_len > 0) {
                            ptls_set_traffic_protection(session->tls, connection->encryption_ctx, 0);
                            uint8_t ciphertext[TLS_MAX_ENCRYPTED_RECORD_SIZE];
                            ptls_buffer_t sendbuf = {0};
                            ptls_buffer_init(&sendbuf, ciphertext, sizeof(ciphertext));
                            assert(ptls_send(session->tls, &sendbuf, cleartext, cleartext_len) == 0);
                            QLOG(session, "transport", "sent_record", "",
                                 "{\"connection_id\": \"%zu\", \"record_sequence\": \"%lu\", \"ciphertext_len\": \"%zu\"}",
                                 connections_index[i], ptls_get_traffic_protection(session->tls, 0)->seq - 1, sendbuf.off);
                            ssize_t sent_len = send(connection->socket, sendbuf.base, sendbuf.off, 0);
                            if (sent_len == -1 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EPIPE)) {
                                fds[i].revents &= ~(POLLOUT);
                                sent_len = 0;
                                if (errno == EPIPE) {
                                    close(connection->socket);
                                    connection->socket = -1;
                                    rapido_application_notification_t *notification = rapido_queue_push(&session->pending_notifications);
                                    notification->notification_type = rapido_connection_closed;
                                    notification->connection_id = connection->connection_id;
                                    // TODO: Properly close the connection
                                }
                            }
                            if (sent_len < sendbuf.off) {
                                rapido_stream_buffer_push(&connection->send_buffer, sendbuf.base + sent_len, sendbuf.off - sent_len);
                            }
                            connection->stats.bytes_sent += sent_len;
                            connection->encryption_ctx->seq = ptls_get_traffic_protection(session->tls, 0)->seq;
                        }
                    } else {
                        size_t fragmented_record_len = TLS_MAX_ENCRYPTED_RECORD_SIZE;
                        void *fragmented_record = rapido_stream_buffer_peek(&connection->send_buffer, &fragmented_record_len);
                        assert(fragmented_record_len > 0);
                        ssize_t sent_len = send(connection->socket, fragmented_record, fragmented_record_len, 0);
                        if (sent_len == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                            fds[i].revents &= ~(POLLOUT);
                            sent_len = 0;
                        }
                        size_t flush_len = sent_len;
                        rapido_stream_buffer_get(&connection->send_buffer, &flush_len);
                        if (flush_len < sent_len) {
                            flush_len = sent_len - flush_len;
                            rapido_stream_buffer_get(&connection->receive_buffer, &flush_len);
                        }
                    }
                    if (rapido_connection_wants_to_send(session, connection) == 0) {
                        fds[i].revents &= ~(POLLOUT);
                    } else {
                        wants_to_write = 1;
                    }
                }
            }
        }
} while (no_fds < 2 && session->pending_notifications.size < session->pending_notifications.capacity / 2);
    return 0;
}

int rapido_free(rapido_t *session) {
    rapido_array_iter(&session->connections, rapido_connection_t *connection,{
        if (connection->socket > -1) {
            close(connection->socket);
        }
        rapido_stream_buffer_free(&connection->receive_buffer);
        rapido_stream_buffer_free(&connection->send_buffer);
        ptls_aead_free(connection->encryption_ctx->aead);
        free(connection->encryption_ctx);
        ptls_aead_free(connection->decryption_ctx->aead);
        free(connection->decryption_ctx);
        if (connection->tls != session->tls) {
            ptls_free(connection->tls);
        }
    });
    rapido_array_iter(&session->streams, rapido_stream_t *stream,{
        rapido_stream_receive_buffer_free(&stream->read_buffer);
        rapido_stream_buffer_free(&stream->send_buffer);
    });
    free(session->tls_properties.additional_extensions);
    ptls_get_traffic_protection(session->tls, 0)->aead = NULL;
    ptls_get_traffic_protection(session->tls, 1)->aead = NULL;
    ptls_free(session->tls);
    rapido_array_free(&session->connections);
    rapido_array_free(&session->streams);
    rapido_array_free(&session->local_addresses);
    rapido_array_free(&session->remote_addresses);
    rapido_array_free(&session->tls_session_ids);
    rapido_queue_free(&session->pending_notifications);
    if (session->is_server) {
        rapido_array_iter(&session->server.listen_sockets, int *socket,{
            if (*socket > -1) {
                close(*socket);
            }
        });
        rapido_array_free(&session->server.listen_sockets);
        rapido_array_iter(&session->server.pending_connections, rapido_pending_connection_t *connection,{
            if (connection->socket > -1) {
                close(connection->socket);
            }
        });
        rapido_array_free(&session->server.pending_connections);
    }
}
