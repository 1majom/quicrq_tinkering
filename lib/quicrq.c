/*
 * QUICR-Q:
 * 
 * Prototyping of quic real-time on top of picoquic.
 * 
 * The prototype will implement several variations of QUICR: stream, rush, and datagrams.
 * These variants use common "glue" code to interface with picoquic:
 *  - feeding media objects for transmission
 *  - providing media objects for rendering
 *  - implementing the picoquic callback
 *  - implementing the socket loop used by picoquic.
 * The socket loop is adapted to wait for media input or end of rendering as well as packet arrival.
 * 
 * The library can be used in three context:
 * - To implement an "origin" server
 * - To implement a relay (e.g., CDN relay)
 * - To implement a client.
 * 
 * The main transaction is the retrieval of a media stream from a server. In the
 * test implementation, this is done by the client setting a connection to the relay
 * (or reusing a suitable connection), and then queuing a "media fragment request", to
 * be sent on the first available client stream. The media request specifies,
 * at a minimum, the identification of the media, possibly the time to start
 * the replay, and the retrieval variant, e.g. stream or datagram.
 * 
 * If the media is available (e.g. at origin), it is sent immediately.
 * If not, the request is queued and the media is requested to an upstream server.
 */

#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "picoquic_utils.h"
#include "quicrq.h"
#include "quicrq_internal.h"
#include "quicrq_relay.h"
#include "quicrq_fragment.h"

/* Allocate space in the message buffer */
int quicrq_msg_buffer_alloc(quicrq_message_buffer_t* msg_buffer, size_t space, size_t bytes_stored)
{
    int ret = 0;

    if (bytes_stored > msg_buffer->buffer_alloc) {
        ret = -1;
    }
    else if (space > msg_buffer->buffer_alloc) {
        uint8_t* x = (uint8_t*)malloc(space);
        if (x == NULL) {
            /* internal error! */
            ret = -1;
        }
        else {
            if (bytes_stored > 0 && bytes_stored <= space) {
                memcpy(x, msg_buffer->buffer, bytes_stored);
            }
            free(msg_buffer->buffer);
            msg_buffer->buffer_alloc = space;
            msg_buffer->buffer = x;
        }
    }
    return ret;
}

/* Accumulate a protocol message from series of read data call backs */
uint8_t * quicrq_msg_buffer_store(uint8_t* bytes, size_t length, quicrq_message_buffer_t* msg_buffer, int* is_finished)
{
    *is_finished = 0;

    while (msg_buffer->nb_bytes_read < 2 && length > 0) {
        msg_buffer->nb_bytes_read++;
        msg_buffer->message_size <<= 8;
        msg_buffer->message_size += bytes[0];
        bytes++;
        length--;
    }

    if (msg_buffer->nb_bytes_read >= 2) {
        size_t bytes_stored = msg_buffer->nb_bytes_read - 2;
        size_t required = msg_buffer->message_size - bytes_stored;

        if (required > 0) {
            if (quicrq_msg_buffer_alloc(msg_buffer, msg_buffer->message_size, bytes_stored) != 0) {
                bytes = NULL;
            } else {
                if (length >= required) {
                    length = required;
                    *is_finished = 1;
                }
                memcpy(msg_buffer->buffer + bytes_stored, bytes, length);
                bytes += length;
                msg_buffer->nb_bytes_read += length;
            }
        }
        else {
            *is_finished = 1;
        }
    }

    return bytes;
}

void quicrq_msg_buffer_reset(quicrq_message_buffer_t* msg_buffer)
{

    msg_buffer->nb_bytes_read = 0;
    msg_buffer->message_size = 0;
}

void quicrq_msg_buffer_release(quicrq_message_buffer_t* msg_buffer)
{
    if (msg_buffer->buffer != NULL) {
        free(msg_buffer->buffer);
    }
    memset(msg_buffer, 0, sizeof(quicrq_message_buffer_t));
}

/* Send a protocol message through series of read data call backs.
 * The repair messages include some data after the header.
 * The "data" and "data_length" must be the same across all calls for the same message.
 * If message is fully sent, the state moves to "ready"
 */
int quicrq_msg_buffer_prepare_to_send_message(quicrq_message_buffer_t* msg_buffer, void* context, size_t space, int more_to_send)
{
    int ret = 0;
    size_t total_size = msg_buffer->message_size;
    size_t total_to_send = 2 + total_size;

    if (msg_buffer->nb_bytes_read < total_to_send) {
        uint8_t* buffer;
        size_t available = total_to_send - msg_buffer->nb_bytes_read;
        if (available > space) {
            more_to_send = 1;
            available = space;
        }

        buffer = picoquic_provide_stream_data_buffer(context, available, 0, more_to_send);
        if (buffer != NULL) {
            /* Feed the message length on two bytes */
            while (msg_buffer->nb_bytes_read < 2 && available > 0) {
                uint8_t b = (msg_buffer->nb_bytes_read == 0) ?
                            (uint8_t)((total_size >> 8) & 255) :
                            (uint8_t)(total_size & 255);
                *buffer = b;
                buffer++;
                available--;
                msg_buffer->nb_bytes_read++;
            }
            /* feed the remaining header content at offset */
            if (available > 0 && msg_buffer->nb_bytes_read < msg_buffer->message_size + 2) {
                size_t offset = msg_buffer->nb_bytes_read - 2;
                memcpy(buffer, msg_buffer->buffer + offset, available);
                msg_buffer->nb_bytes_read += available;
            }
        }
        else {
            ret = -1;
        }

        if (msg_buffer->nb_bytes_read >= total_to_send) {
            msg_buffer->nb_bytes_read = 0;
            msg_buffer->message_size = 0;
        }
    }
    return ret;
}


int quicrq_msg_buffer_prepare_to_send(quicrq_stream_ctx_t* stream_ctx, void* context, size_t space, int more_to_send)
{
    quicrq_message_buffer_t* msg_buffer = &stream_ctx->message_sent;
    int ret = quicrq_msg_buffer_prepare_to_send_message(msg_buffer, context, space, more_to_send);

    if (msg_buffer->message_size == 0) {
        stream_ctx->send_state = quicrq_sending_ready;
    }
    return ret;
}


/* Sending in sequence on a stream. 
 * We do not want to spend too much effort there, so we are going to reuse the "fragment" message
 * to send data fragments of sufficient length. This is a bit of a hack, and it does add some overhead.
 * Need to maintain variables, e.g.:
 * - next object ID to send
 * - current object offset
 * - next 
 */
int quicrq_prepare_to_send_media_to_stream(quicrq_stream_ctx_t* stream_ctx, void* context, size_t space, uint64_t current_time)
{
    /* Find how much data is available on the media stream */
    int is_media_finished = 0;
    int is_new_group = 0;
    int is_still_active = 0;
    int should_skip = 0;
    int has_backlog = 0;
    size_t available = 0;
    size_t data_length = 0;
    uint64_t object_length;
    uint8_t stream_header[QUICRQ_STREAM_HEADER_MAX];
    uint8_t flags = 0;
    size_t h_size;
    uint64_t nb_objects_previous_group = 0;
    int ret = 0;

    /* TODO: maintain a priority threshold per connection. If a stream is congested,
     * check the priority for that stream. Do not commit resource on a low priority
     * stream if another stream is waiting. 
     */

    /* First, create a "mock" buffer based on the available space instead of the actual number of bytes.
     * By design, we are encoding the fragment with the "data" parameter set to NULL. */
    uint8_t* h_byte = quicrq_fragment_msg_encode(stream_header + 2, stream_header + QUICRQ_STREAM_HEADER_MAX, QUICRQ_ACTION_FRAGMENT,
        stream_ctx->next_group_id, stream_ctx->next_object_id, 0, stream_ctx->next_object_offset,
        stream_ctx->next_object_offset + available, flags, space, NULL);

    if (h_byte == NULL) {
        /* That should not happen, unless the stream_header size is way too small */
        ret = -1;
    }
    else {
        h_size = h_byte - stream_header;
        if (h_size > space) {
            /* That should not happen either, picoquic should never provide less than 17 bytes. */
            ret = -1;
        }
        else {
            /* Find how much data is actually available */
            ret = quicrq_fragment_publisher_fn(quicrq_media_source_get_data, stream_ctx->media_ctx, NULL, space - h_size, &available, 
                &flags, &is_new_group, &object_length, &is_media_finished, &is_still_active, &should_skip, current_time);
            if (is_new_group) {
                stream_ctx->next_group_id += 1;
                nb_objects_previous_group = stream_ctx->next_object_id;
                stream_ctx->next_object_id = 0;
                stream_ctx->next_object_offset = 0;
            }
        }
    }

    if (ret == 0) {
        if (should_skip) {
            /* Prepare and a place holder for the object, pretending 0 length, setting the flags to 0xFF
             * Call the publisher APi to signal that the object should be skipped. 
             */
            h_byte = quicrq_fragment_msg_encode(stream_header + 2, stream_header + QUICRQ_STREAM_HEADER_MAX, QUICRQ_ACTION_FRAGMENT,
                stream_ctx->next_group_id, stream_ctx->next_object_id, nb_objects_previous_group, 0, 0, 0xFF, 0, NULL);
            ret = quicrq_fragment_publisher_fn(quicrq_media_source_skip_object, stream_ctx->media_ctx, NULL, 0, &data_length,
                &flags, &is_new_group, &object_length, &is_media_finished, &is_still_active, &has_backlog, current_time);
            if (ret == 0) {
                uint8_t* buffer = (uint8_t*)picoquic_provide_stream_data_buffer(context, h_size, 0, 1);
                if (buffer == NULL) {
                    ret = -1;
                }
                else {
                    /* copy the stream header to the packet */
                    memcpy(buffer, stream_header, h_size);
                    /* Set the message length */
                    size_t message_length = h_size - 2;
                    buffer[0] = (uint8_t)(message_length >> 8);
                    buffer[1] = (uint8_t)(message_length & 0xff);

                    stream_ctx->next_object_id++;
                    stream_ctx->next_object_offset = 0;

                    if (is_media_finished) {
                        stream_ctx->final_group_id = stream_ctx->next_group_id;
                        stream_ctx->final_object_id = stream_ctx->next_object_id;
                        stream_ctx->send_state = quicrq_sending_ready;
                    }
                }
            }
        }
        else if (available == 0 && flags != 0xff) {
            if (is_media_finished) {
                /* Send the fin object immediately, because it would be very hard to get
                 * a new "prepare to send" callback after an empty response.
                 */
                stream_ctx->final_group_id = stream_ctx->next_group_id;
                stream_ctx->final_object_id = stream_ctx->next_object_id;
                h_byte = quicrq_fin_msg_encode(stream_header + 2, stream_header + QUICRQ_STREAM_HEADER_MAX, QUICRQ_ACTION_FIN_DATAGRAM,
                    stream_ctx->final_group_id, stream_ctx->final_object_id);
                if (h_byte == NULL || h_byte > stream_header + space) {
                    ret = -1;
                }
                else {
                    uint8_t* buffer;
                    h_size = h_byte - stream_header;
                    buffer = (uint8_t*)picoquic_provide_stream_data_buffer(context, h_size, 1, 0);
                    stream_ctx->is_local_finished = 1;
                    if (buffer == NULL) {
                        ret = -1;
                    }
                    else {
                        size_t m_size = h_size - 2;
                        picoquic_log_app_message(stream_ctx->cnx_ctx->cnx,
                            "Fin group, object of stream %" PRIu64 " : %" PRIu64 ", %" PRIu64,
                            stream_ctx->stream_id, stream_ctx->final_group_id, stream_ctx->final_object_id);

                        stream_header[0] = (uint8_t)(m_size >> 8);
                        stream_header[1] = (uint8_t)(m_size & 0xff);
                        memcpy(buffer, stream_header, h_size);
                        stream_ctx->is_final_object_id_sent = 1;
                        if (stream_ctx->close_reason == quicrq_media_close_reason_unknown) {
                            stream_ctx->close_reason = quicrq_media_close_finished;
                        }
                    }
                }
            }
            else {
                /* Mark stream as not ready. It will be awakened when data becomes available */
                picoquic_mark_active_stream(stream_ctx->cnx_ctx->cnx, stream_ctx->stream_id, 0, stream_ctx);
            }
        }
        else {
            /* Encode the actual header, instead of a prediction */
            h_byte = quicrq_fragment_msg_encode(stream_header + 2, stream_header + QUICRQ_STREAM_HEADER_MAX, QUICRQ_ACTION_FRAGMENT,
                stream_ctx->next_group_id, stream_ctx->next_object_id, nb_objects_previous_group, stream_ctx->next_object_offset,
                object_length, flags, available, NULL);

            if (h_byte == NULL) {
                /* That should not happen, unless the stream_header size was way too small */
                ret = -1;
            } else if ((size_t)(h_byte - stream_header) != h_size) {
                /* Encoding has changed. May need to change the length, and if that recompute the header */
                h_size = h_byte - stream_header;
                if (h_size + available > space) {
                    /* The encoding changed, the computation of available space was wrong. */
                    available = space - h_size;
                    h_byte = quicrq_fragment_msg_encode(stream_header + 2, stream_header + QUICRQ_STREAM_HEADER_MAX, QUICRQ_ACTION_FRAGMENT,
                        stream_ctx->next_group_id, stream_ctx->next_object_id, nb_objects_previous_group, stream_ctx->next_object_offset, 
                        object_length, flags, available, NULL);
                    /* The header size may have changed again, if the smaller "available" value is coded on fewer bytes. But it can only be decreased. */
                    h_size = h_byte - stream_header;
                }
            }
            if (ret == 0) {
                uint8_t* buffer;
                if (stream_ctx->next_object_offset + available >= object_length) {
                    picoquic_log_app_message(stream_ctx->cnx_ctx->cnx, "Final fragment of object %" PRIu64 ",%" PRIu64 " on stream % " PRIu64,
                        stream_ctx->next_group_id, stream_ctx->next_object_id, stream_ctx->stream_id);
                }

                buffer = (uint8_t*)picoquic_provide_stream_data_buffer(context, h_size + available, 0, 1);
                if (buffer == NULL) {
                    ret = -1;
                }
                else {
                    /* copy the stream header to the packet */
                    memcpy(buffer, stream_header, h_size);
                    ret = quicrq_fragment_publisher_fn(quicrq_media_source_get_data, stream_ctx->media_ctx, buffer + h_size, available, &data_length,
                        &flags, &is_new_group, &object_length, &is_media_finished, &is_still_active, &has_backlog, current_time);
                    if (ret == 0 && available != data_length) {
                        ret = -1;
                    }
                    else
                    {
                        /* Set the message length */
                        size_t message_length = h_size - 2 + available;
                        buffer[0] = (uint8_t)(message_length >> 8);
                        buffer[1] = (uint8_t)(message_length & 0xff);

                        stream_ctx->next_object_offset += available;
                        if (stream_ctx->next_object_offset >= object_length) {
                            stream_ctx->next_object_id++;
                            stream_ctx->next_object_offset = 0;
                        }

                        if (is_media_finished) {
                            stream_ctx->final_group_id = stream_ctx->next_group_id;
                            stream_ctx->final_object_id = stream_ctx->next_object_id;
                            stream_ctx->send_state = quicrq_sending_ready;
                        }
                    }
                }
            }
        }
    }

    return ret;
}

/* Find the stream context associated with a datagram */
quicrq_stream_ctx_t* quicrq_find_stream_ctx_for_datagram(quicrq_cnx_ctx_t* cnx_ctx, uint64_t media_id, int is_sender)
{
    quicrq_stream_ctx_t* stream_ctx = NULL;

    /* Find the stream context by datagram ID */
    stream_ctx = cnx_ctx->first_stream;
    while (stream_ctx != NULL) {
        if ((stream_ctx->is_sender == is_sender) && stream_ctx->transport_mode == quicrq_transport_mode_datagram && stream_ctx->media_id == media_id) {
            break;
        }
        stream_ctx = stream_ctx->next_stream;
    }
    return stream_ctx;
}

/* Receive data in a datagram */
int quicrq_receive_datagram(quicrq_cnx_ctx_t* cnx_ctx, const uint8_t* bytes, size_t length, uint64_t current_time)
{
    int ret = 0;
    quicrq_stream_ctx_t* stream_ctx = NULL;

    /* Parse the datagram header */
    const uint8_t* bytes_max = bytes + length;
    uint64_t media_id;
    uint64_t group_id;
    uint64_t object_id;
    uint64_t object_offset;
    uint64_t queue_delay;
    uint64_t nb_objects_previous_group;
    uint64_t object_length;
    uint8_t flags;
    const uint8_t* next_bytes;

    next_bytes = quicrq_datagram_header_decode(bytes, bytes_max, &media_id, &group_id, &object_id, &object_offset, &queue_delay, &flags, &nb_objects_previous_group, &object_length);

    if (next_bytes == NULL) {
        DBG_PRINTF("%s", "Error decoding datagram header");
        ret = -1;
    }
    else {
        /* Find the stream context by datagram ID */
        stream_ctx = quicrq_find_stream_ctx_for_datagram(cnx_ctx, media_id, 0);
        if (stream_ctx == NULL) {
            DBG_PRINTF("Unexpected datagram on stream %" PRIu64 ", object id %" PRIu64 "/%" PRIu64 ", max: % " PRIu64, 
                media_id, group_id, object_id, cnx_ctx->next_media_id);
            picoquic_log_app_message(cnx_ctx->cnx, "Unexpected datagram on stream %" PRIu64 ", object id %" PRIu64 "/%" PRIu64 ", max: % " PRIu64,
                media_id, group_id, object_id, cnx_ctx->next_media_id);
            if (media_id >= cnx_ctx->next_media_id) {
                ret = -1;
                picoquic_log_app_message(cnx_ctx->cnx, "Error, unexpected datagram stream %" PRIu64,
                    media_id);
            }
        }
        else {
            /* Compute data length based on remaining bytes */
            size_t data_length = bytes_max - next_bytes;
            /* Verification that there are no unexpected fragments, used in tests */
            if (group_id < stream_ctx->start_group_id ||
                (group_id == stream_ctx->start_group_id && object_id < stream_ctx->start_object_id)) {
                cnx_ctx->qr_ctx->useless_fragments += 1;
            }
            /* Pass data to the media context. */
            if (object_offset + data_length >= object_length) {
                picoquic_log_app_message(cnx_ctx->cnx, "Received final fragment of object %" PRIu64 "/%" PRIu64 " on datagram stream %" PRIu64 ", stream %" PRIu64,
                    group_id, object_id, media_id, stream_ctx->stream_id);
            }
            ret = stream_ctx->consumer_fn(quicrq_media_datagram_ready, stream_ctx->media_ctx, current_time, next_bytes, group_id, object_id, object_offset, 
                queue_delay, flags, nb_objects_previous_group, object_length, bytes_max - next_bytes);
            if (ret == quicrq_consumer_finished) {
                ret = quicrq_cnx_handle_consumer_finished(stream_ctx, 0, 1, ret);
            }
            if (ret != 0) {
                DBG_PRINTF("Error found on dg stream id %" PRIu64 ", object id %" PRIu64 "/%", media_id, group_id, object_id);
            }
        }
    }

    return ret;
}

/* Handle the list of datagrams pending acknowledgement or retransmission.
 * The code maintains an acknowledgement tree of the fragments that were sent.
 * TODO: handle whether we can have overlapping fragments. We will assume that
 * we will not, i.e., that the MTU will remain valid for the duration of
 * the datagram. Thus, we do not use the length field as part of the
 * key.
 */

static void* quicrq_datagram_ack_node_value(picosplay_node_t* datagram_ack_node)
{
    return (datagram_ack_node == NULL) ? NULL : (void*)((char*)datagram_ack_node - offsetof(struct st_quicrq_datagram_ack_state_t, datagram_ack_node));
}

static int64_t quicrq_datagram_ack_node_compare(void* l, void* r)
{
    quicrq_datagram_ack_state_t* da_l = (quicrq_datagram_ack_state_t*)l;
    quicrq_datagram_ack_state_t* da_r = (quicrq_datagram_ack_state_t*)r;
    int64_t ret = (int64_t)(da_l->group_id - da_r->group_id);
    
    if (ret == 0) {
        ret = da_l->object_id - da_r->object_id;
    }
    if (ret == 0) {
        ret = da_l->object_offset - da_r->object_offset;
    }

    return ret;
}

static picosplay_node_t* quicrq_datagram_ack_node_create(void* v_datagram_ack_state)
{
    /* Do not actually create data. Simply return a pointer to the "node"
     * property in the datagram ack state record. This is in line with
     * expected behavior of "picosplay". */
    return &((quicrq_datagram_ack_state_t*)v_datagram_ack_state)->datagram_ack_node;
}

static void quicrq_datagram_ack_extra_dequeue(quicrq_stream_ctx_t* stream_ctx, quicrq_datagram_ack_state_t* das)
{
    if (das->extra_data == NULL) {
        return;
    }
    if (das->extra_previous == NULL) {
        stream_ctx->extra_first = das->extra_next;
    }
    else {
        das->extra_previous->extra_next = das->extra_next;
    }
    if (das->extra_next == NULL) {
        stream_ctx->extra_last = das->extra_previous;
    }
    else {
        das->extra_next->extra_previous = das->extra_previous;
    }

    free(das->extra_data);
    das->extra_data = NULL;
    das->extra_next = NULL;
    das->extra_previous = NULL;
    das->extra_repeat_time = 0;
}

static void quicrq_datagram_ack_extra_queue(quicrq_stream_ctx_t* stream_ctx, quicrq_datagram_ack_state_t* das, const uint8_t * data, uint64_t repeat_time)
{
    if (das->is_extra_queued) {
        return;
    }
    das->is_extra_queued = 1;

    if (das->extra_data != NULL) {
        /* new repeat request replaces the previous one */
        quicrq_datagram_ack_extra_dequeue(stream_ctx, das);
    }
    das->extra_data = (uint8_t *)malloc(das->length);
    if (das->extra_data != NULL) {
        memcpy(das->extra_data, data, das->length);
        if (stream_ctx->extra_last == NULL) {
            stream_ctx->extra_first = das;
            stream_ctx->extra_last = das;
        }
        else {
            stream_ctx->extra_last->extra_next = das;
            das->extra_previous = stream_ctx->extra_last;
            stream_ctx->extra_last = das;
        }
        das->extra_repeat_time = repeat_time;
        stream_ctx->nb_extra_sent++;
    }
}

static void quicrq_datagram_ack_node_delete(void* tree, picosplay_node_t* node)
{
    quicrq_stream_ctx_t* stream_ctx = (quicrq_stream_ctx_t*)
        ((char*)tree - offsetof(struct st_quicrq_stream_ctx_t, datagram_ack_tree));
    quicrq_datagram_ack_state_t* das = (quicrq_datagram_ack_state_t*)
        quicrq_datagram_ack_node_value(node);
    if (das->extra_data != NULL) {
        /* dequeue from extra repeat list */
        quicrq_datagram_ack_extra_dequeue(stream_ctx, das);
    }
    free(quicrq_datagram_ack_node_value(node));
}

static void quicrq_datagram_ack_ctx_init(quicrq_stream_ctx_t* stream_ctx)
{
    stream_ctx->horizon_group_id = UINT64_MAX;
    stream_ctx->horizon_object_id = UINT64_MAX;
    stream_ctx->horizon_offset = UINT64_MAX;
    stream_ctx->horizon_is_last_fragment = 1;
    picosplay_init_tree(&stream_ctx->datagram_ack_tree, quicrq_datagram_ack_node_compare,
        quicrq_datagram_ack_node_create, quicrq_datagram_ack_node_delete, quicrq_datagram_ack_node_value);
}

static void quicrq_datagram_ack_ctx_release(quicrq_stream_ctx_t* stream_ctx)
{
    if (stream_ctx->datagram_ack_tree.size != 0 || stream_ctx->nb_extra_sent > 0 ||
        stream_ctx->nb_horizon_acks > 0 || stream_ctx->nb_horizon_events > 0) {
        picosplay_node_t * next_node = picosplay_first(&stream_ctx->datagram_ack_tree);
        int nb_fragments_acked = 0;
        int nb_fragments_nacked = 0;
        int nb_fragments_alone = 0;
        while (next_node != NULL) {
            quicrq_datagram_ack_state_t* das = (quicrq_datagram_ack_state_t*)quicrq_datagram_ack_node_value(next_node);
            if (das->is_acked) {
                nb_fragments_acked++;
            }
            if (das->nack_received) {
                nb_fragments_nacked++;
            }
            if (!das->is_acked && !das->nack_received) {
                nb_fragments_alone++;
            }

            next_node = picosplay_next(next_node);
        }

        DBG_PRINTF("End of stream  %" PRIu64 ", %d nodes in datagram list, %d acked, %d nacked, alone: %d, extra: %d",
            stream_ctx->stream_id, stream_ctx->datagram_ack_tree.size,
            nb_fragments_acked, nb_fragments_nacked, nb_fragments_alone,
            stream_ctx->nb_extra_sent);
        DBG_PRINTF("Horizon Object ID: %" PRIu64 ", offset: %" PRIu64,
            stream_ctx->horizon_object_id, stream_ctx->horizon_offset);
        DBG_PRINTF("ACKs below horizon: %" PRIu64 ", ACK Init below horizon: %" PRIu64,
            stream_ctx->nb_horizon_acks, stream_ctx->nb_horizon_events);
    }
    picosplay_empty_tree(&stream_ctx->datagram_ack_tree);
}

quicrq_datagram_ack_state_t* quicrq_datagram_ack_find(quicrq_stream_ctx_t* stream_ctx, uint64_t group_id, uint64_t object_id, uint64_t object_offset)
{
    quicrq_datagram_ack_state_t* found = NULL;
    quicrq_datagram_ack_state_t target = { 0 };
    target.group_id = group_id;
    target.object_id = object_id;
    target.object_offset = object_offset;

    picosplay_node_t* node = picosplay_find(&stream_ctx->datagram_ack_tree, (void*)&target);
    if (node != NULL) {
        found = (quicrq_datagram_ack_state_t*)quicrq_datagram_ack_node_value(node);
    }
    return found;
}

int64_t quicrq_datagram_check_horizon(quicrq_stream_ctx_t* stream_ctx, uint64_t group_id, uint64_t object_id, uint64_t object_offset)
{
    int64_t ret = group_id - stream_ctx->horizon_group_id;
    
    if (ret == 0) {
        ret = object_id - stream_ctx->horizon_object_id;
    }
    if (ret == 0) {
        ret = object_offset - stream_ctx->horizon_offset;
    }
    return ret;
}

int quicrq_datagram_ack_init(quicrq_stream_ctx_t* stream_ctx, uint64_t group_id, uint64_t object_id, 
    uint64_t object_offset, uint8_t flags, uint64_t nb_objects_previous_group, const uint8_t * data, size_t length,
    uint64_t queue_delay, uint64_t object_length, void** p_created_state, uint64_t current_time)
{
    int ret = 0;

    /* Check whether the object is below the horizon */
    if (quicrq_datagram_check_horizon(stream_ctx, group_id, object_id, object_offset) < 0) {
        /* at or below horizon, not new. */
        stream_ctx->nb_horizon_events++;
    } else {
        /* Find whether the ack record is there. */
        quicrq_datagram_ack_state_t* found = quicrq_datagram_ack_find(stream_ctx, group_id, object_id, object_offset);

        /* if there, no need to send it. */
        if (found) {
            DBG_PRINTF("ACK Init duplicate, object %" PRIu64 ", offset %" PRIu64,
                object_id, object_offset);
            ret = 1;
        }
        else {
            /* else, create a record. */
            quicrq_datagram_ack_state_t* da_new = (quicrq_datagram_ack_state_t*)malloc(sizeof(quicrq_datagram_ack_state_t));
            if (da_new == NULL) {
                /* memory error */
                ret = -1;
            }
            else {
                memset(da_new, 0, sizeof(quicrq_datagram_ack_state_t));
                da_new->group_id = group_id;
                da_new->object_id = object_id;
                da_new->object_offset = object_offset;
                da_new->flags = flags;
                da_new->nb_objects_previous_group = nb_objects_previous_group;
                da_new->length = length;
                da_new->object_length = object_length;
                da_new->queue_delay = queue_delay;
                da_new->start_time = current_time;
                picosplay_insert(&stream_ctx->datagram_ack_tree, da_new);
                if (p_created_state != NULL) {
                    *p_created_state = da_new;
                }
                /* If this is a delayed fragment, we could schedule an extra repeat 
                 */
                if (stream_ctx->cnx_ctx->qr_ctx->extra_repeat_after_received_delayed &&
                    stream_ctx->cnx_ctx->qr_ctx->extra_repeat_delay > 0 &&
                    queue_delay > 20) {
                    quicrq_datagram_ack_extra_queue(stream_ctx, da_new, data, current_time + stream_ctx->cnx_ctx->qr_ctx->extra_repeat_delay);
                }
            }
        }
    }
    return ret;
}

int quicrq_datagram_handle_ack(quicrq_stream_ctx_t* stream_ctx, uint64_t group_id, uint64_t object_id, uint64_t object_offset, size_t length)
{
    int ret = 0;
    /* handle the case where the acked data overlaps the horizon */
    int is_below_horizon = 0;
    int should_check_horizon = 0;
    int64_t horizon_delta_group = group_id - stream_ctx->horizon_group_id;
    int64_t horizon_delta = object_id - stream_ctx->horizon_object_id;
    int64_t acked_length = length;
    uint64_t acked_offset = object_offset;

    /* If at horizon, check offset */
    if (horizon_delta_group  == 0 && horizon_delta == 0) {
        if (object_offset + length < stream_ctx->horizon_offset) {
            stream_ctx->nb_horizon_acks++;
            is_below_horizon = 1;
        }
        else if (object_offset < stream_ctx->horizon_offset) {
            /* update the ACK to only retain the part above the horizon */
            acked_offset = stream_ctx->horizon_offset;
            acked_length -= (stream_ctx->horizon_offset - object_offset);
            should_check_horizon = 1;
        }
        else if (object_offset == stream_ctx->horizon_offset) {
            should_check_horizon = 1;
        }
    }
    else if (horizon_delta_group < 0 || (horizon_delta_group == 0 && horizon_delta < 0)) {
        is_below_horizon = 1;
        stream_ctx->nb_horizon_acks++;
    }
    else if (horizon_delta_group == 0 && horizon_delta == 1 && stream_ctx->horizon_is_last_fragment && object_offset == 0) {
        should_check_horizon = 1;
    }
    else if (stream_ctx->horizon_group_id == UINT64_MAX) {
        should_check_horizon = 1;
    }

    if (!is_below_horizon) {
        /* Find whether the ack record is there. */
        quicrq_datagram_ack_state_t* found = quicrq_datagram_ack_find(stream_ctx, group_id, object_id, acked_offset);

        /* if there, mark as acknowledged */
        /* in some cases, e.g. spurious repeat, the ack of a previous transmission may have a larger acked length than the current record */
        while (found != NULL && acked_length > 0) {
            found->is_acked = 1;
            acked_length -= found->length;
            acked_offset += found->length;
            if (acked_length > 0) {
                found = (quicrq_datagram_ack_state_t*)quicrq_datagram_ack_node_value(picosplay_next(&found->datagram_ack_node));
                if (found->group_id != group_id || found->object_id != object_id || found->object_offset != acked_offset) {
                    break;
                }
            }
            else {
                break;
            }
        }
    }
    /* Horizon check is complicated because the (group_id, object_id) number
     * space is not strictly monotonous. For example, it would be wrong to
     * jump from (0,17) to (2,0) if there was an object (0,18) still
     * unacknowledged. This is managed using the parameter "number of
     * objects in previous group", which is always present for the first
     * segment of the first object in a given group. We implement this
     * with the following tests:
     * - if next fragment is same group id and same object id:
     *      progress the offset if the next fragment starts at the expected value.
     * - if next fragment is same group id and different object id:
     *      progress the group id if the object id is just one above,
     *      the current object is fully acked, and the next fragment starts
     *      at offset 0.
     * - if next fragment is different group id:
     *      progress group and object id 
     *      if the current fragment  fragment is fully received, 
     *      the next fragment starts is for the next group, the
     *      next object id is zero, the next offset is zero,
     *      and the current object id + 1 matches the number of
     *      objects in previous group.
     */

    if (should_check_horizon) {
        /* Progress the horizon */
        picosplay_node_t* next_node = picosplay_first(&stream_ctx->datagram_ack_tree);
        while (next_node != NULL) {
            int just_after = 0;
            quicrq_datagram_ack_state_t* das = (quicrq_datagram_ack_state_t*)quicrq_datagram_ack_node_value(next_node);
            if (!das->is_acked) {
                break;
            }
            if (das->group_id == stream_ctx->horizon_group_id) {
                if (das->object_id == stream_ctx->horizon_object_id) {
                    just_after = (das->object_offset == stream_ctx->horizon_offset);
                }
                else if (stream_ctx->horizon_is_last_fragment) {
                    just_after = ((das->object_id - stream_ctx->horizon_object_id) == 1) && (das->object_offset == 0);
                }
            }
            else {
                just_after = (stream_ctx->horizon_is_last_fragment &&
                    das->group_id == stream_ctx->horizon_group_id + 1 &&
                    das->object_offset == 0 &&
                    das->nb_objects_previous_group == stream_ctx->horizon_object_id + 1);
            }
            if (!just_after) {
                break;
            }
            else {
                /* collapse the horizon */
                picosplay_node_t* to_be_forgotten = next_node;
                stream_ctx->horizon_group_id = das->group_id;
                stream_ctx->horizon_object_id = das->object_id;
                stream_ctx->horizon_offset = das->object_offset + das->length;
                stream_ctx->horizon_is_last_fragment =
                    stream_ctx->horizon_offset >= das->object_length;
                next_node = picosplay_next(next_node);
                picosplay_delete_hint(&stream_ctx->datagram_ack_tree, to_be_forgotten);
            }
        }
    }
    return ret;
}

 /* If a datagram frame needs to be repeated, a copy of the frame will be
 * queued using the picoquic_queue_datagram_frame() API. That API can 
 * only handle datagram of at most PICOQUIC_DATAGRAM_QUEUE_MAX_LENGTH
 * bytes. If the original datagram is longer, it needs to be split.
 */

int quicrq_datagram_handle_repeat(quicrq_stream_ctx_t* stream_ctx,
    quicrq_datagram_ack_state_t* found,
    const uint8_t* data, size_t data_length,
    int prepare_extra,
    uint64_t current_time)
{
    int ret = 0;
    /* Check that the connection is there */
    if (stream_ctx->cnx_ctx == NULL || stream_ctx->cnx_ctx->cnx == NULL) {
        ret = -1;
    }
    else {
        while ((data_length > 0 || found->flags == 0xff) && ret == 0) {
            uint8_t datagram[PICOQUIC_MAX_PACKET_SIZE];
            uint8_t* bytes = datagram;
            uint8_t* bytes_max = datagram + PICOQUIC_MAX_PACKET_SIZE;
            uint64_t queue_delay_delta = 0;
            size_t fragment_length = data_length;
            size_t header_length;
            size_t datagram_length;
            if (current_time > found->start_time) {
                queue_delay_delta = (current_time - found->start_time + 500) / 1000;
            }
            /* Encode the header */
            found->last_sent_time = current_time;
            bytes = quicrq_datagram_header_encode(bytes, bytes_max, stream_ctx->media_id,
                found->group_id, found->object_id, found->object_offset, found->queue_delay + queue_delay_delta, found->flags,
                found->nb_objects_previous_group, found->object_length);
            /* Check how much data should be send in this fragment */
            header_length = bytes - datagram;
            datagram_length = header_length + data_length;
            if (datagram_length > PICOQUIC_DATAGRAM_QUEUE_MAX_LENGTH) {
                fragment_length = PICOQUIC_DATAGRAM_QUEUE_MAX_LENGTH - header_length;
                datagram_length = PICOQUIC_DATAGRAM_QUEUE_MAX_LENGTH;
            }
            /* Copy the data */
            if (bytes + fragment_length > bytes_max) {
                ret = -1;
            }
            else {
                memcpy(bytes, data, fragment_length);
                ret = picoquic_queue_datagram_frame(stream_ctx->cnx_ctx->cnx, datagram_length, datagram);
                if (ret == 0){
                    found->last_sent_time = current_time;
                    if (prepare_extra && stream_ctx->cnx_ctx->qr_ctx->extra_repeat_delay > 0) {
                        quicrq_datagram_ack_extra_queue(stream_ctx, found, data,
                            current_time + stream_ctx->cnx_ctx->qr_ctx->extra_repeat_delay);
                    }
                    if (fragment_length < data_length) {
                        void* p_next_record = NULL;
                        size_t next_offset = found->object_offset + fragment_length;
                        data += fragment_length;
                        data_length -= fragment_length;

                        /* split the fragment, get a new one, update old record, point found to new record. */
                        ret = quicrq_datagram_ack_init(stream_ctx, found->group_id, found->object_id, next_offset,
                            found->flags, found->nb_objects_previous_group, data, data_length,
                            found->queue_delay, found->object_length, &p_next_record, found->start_time);
                        if (ret == 0) {
                            quicrq_datagram_ack_state_t* next_record = (quicrq_datagram_ack_state_t*)p_next_record;
                            next_record->object_length = found->object_length;
                            next_record->nack_received = found->nack_received;
                            found->length = fragment_length;
                            found = next_record;
                        }
                    }
                    else {
                        break;
                    }
                }
                else {
                    DBG_PRINTF("Error queuing datagram: %" PRIu64 "/%" PRIu64 "/%" PRIu64,
                        found->group_id, found->object_id, found->object_offset);
                }
            }
        }
    }
    return ret;
}

int quicrq_datagram_handle_lost(quicrq_stream_ctx_t* stream_ctx, uint64_t group_id, uint64_t object_id, uint64_t object_offset, uint64_t sent_time,
    const uint8_t *bytes, size_t length, uint64_t current_time)
{
    int ret = 0;
    /* Find whether the ack record is there. */
    quicrq_datagram_ack_state_t* found = quicrq_datagram_ack_find(stream_ctx, group_id, object_id, object_offset);

    /* if not there, assume acknowledged and hidden below the horizon */
    /* if found and is acked, do not repeat */
    /* If this is not the last transmission, do not repeat */
    if (found != NULL && !found->is_acked){
        if (!found->is_extra_queued || found->last_sent_time <= sent_time + 1000) {
            found->nack_received = 1;
            stream_ctx->nb_fragment_lost++;
            /* Update the datagram header, and queue as datagram */
            ret = quicrq_datagram_handle_repeat(stream_ctx, found, bytes, length,
                stream_ctx->cnx_ctx->qr_ctx->extra_repeat_on_nack, current_time);
        }
        else {
            DBG_PRINTF("Ignored NACK, object: %" PRIu64 ",%" PRIu64 ", offset: %" PRIu64 ", sent at %" PRIu64 ", last sent %" PRIu64,
                group_id, object_id, object_offset, sent_time, found->last_sent_time);
        }
    }
    return ret;
}

/* Handle the acknowledgements of datagrams */
int quicrq_handle_datagram_ack_nack(quicrq_cnx_ctx_t* cnx_ctx, picoquic_call_back_event_t picoquic_event, 
    uint64_t send_time, const uint8_t* bytes, size_t length, uint64_t current_time)
{
    int ret = 0;
    /* Obtain the datagram ID */
    const uint8_t* bytes_max = bytes + length;
    uint64_t media_id;
    uint64_t group_id;
    uint64_t object_id;
    uint64_t object_offset;
    uint64_t queue_delay;
    uint8_t flags;
    uint64_t nb_objects_previous_group;
    uint64_t object_length;
    const uint8_t* next_bytes;

    if (bytes == NULL) {
        ret = -1;
    }
    else {
        next_bytes = quicrq_datagram_header_decode(bytes, bytes_max, &media_id, &group_id, &object_id, &object_offset, &queue_delay, &flags, &nb_objects_previous_group, &object_length);
        
        /* Retrieve the stream context for the datagram */
        if (next_bytes == NULL) {
            ret = -1;
        }
        else {
            /* Find the stream context by datagram ID.
             * the stream may already be closed, so not finding it is not an error.
             */
            quicrq_stream_ctx_t* stream_ctx = quicrq_find_stream_ctx_for_datagram(cnx_ctx, media_id, 1);
            if (stream_ctx != NULL) {
                size_t data_length = (size_t)(bytes_max - next_bytes);
                switch (picoquic_event) {
                case picoquic_callback_datagram_acked: /* Ack for packet carrying datagram-object received from peer */
                    ret = quicrq_datagram_handle_ack(stream_ctx, group_id, object_id, object_offset, data_length);
                    break;
                case picoquic_callback_datagram_lost: /* Packet carrying datagram-object probably lost */
                    ret = quicrq_datagram_handle_lost(stream_ctx, group_id, object_id, object_offset, send_time,
                        next_bytes, data_length, current_time);
                    break;
                case picoquic_callback_datagram_spurious: /* Packet carrying datagram-object was not really lost */
                    ret = quicrq_datagram_handle_ack(stream_ctx, group_id, object_id, object_offset, data_length);
                    break;
                default:
                    ret = -1;
                }
            }
        }
    }

    return ret;
}

/* control whether an extra copy of the packet can be sent:
* - after the packet is repeated (on nack)
* - if a packet was delayed at a previous hop (after-delayed)
*/

void quicrq_set_extra_repeat(quicrq_ctx_t* qr, int on_nack, int after_delayed)
{
    qr->extra_repeat_on_nack = (on_nack != 0);
    qr->extra_repeat_after_received_delayed = (after_delayed != 0);
}

/* Set the extra repeat delay to a specific value, 
 * or to zero to disable the process.
 */
void quicrq_set_extra_repeat_delay(quicrq_ctx_t* qr, uint64_t delay_in_microseconds)
{
    qr->extra_repeat_delay = delay_in_microseconds;
}

/* Handling of extra repeats in a quicrq_context.
 * Check all the queues and return the next wakeup time, wich will be "now"
 * if there are queued datagrams, or the time at which the next datagram will be
 * queued
 */
uint64_t quicrq_handle_extra_repeat(quicrq_ctx_t* qr, uint64_t current_time)
{
    uint64_t next_time = UINT64_MAX;
    quicrq_cnx_ctx_t* cnx_ctx = qr->first_cnx;

    while (cnx_ctx != NULL) {
        quicrq_stream_ctx_t* stream_ctx = cnx_ctx->first_stream;
        while (stream_ctx != NULL) {
            quicrq_datagram_ack_state_t* das = stream_ctx->extra_first;
            while (das != NULL) {
                if (das->extra_repeat_time <= current_time) {
                    next_time = current_time;
                    int ret = quicrq_datagram_handle_repeat(stream_ctx, das, das->extra_data, das->length, 0, current_time);
                    if (ret != 0) {
                        DBG_PRINTF("Handle repeat error, ret = %d", ret);
                    }
                    quicrq_datagram_ack_extra_dequeue(stream_ctx, das);
                    das = stream_ctx->extra_first;
                }
                else
                {
                    if (das->extra_repeat_time < next_time) {
                        next_time = das->extra_repeat_time;
                    }
                    break;
                }
            }
            stream_ctx = stream_ctx->next_stream;
        }
        cnx_ctx = cnx_ctx->next_cnx;
    }
    return next_time;
}

/* Enable of disablecongestion control*/

void quicrq_enable_congestion_control(quicrq_ctx_t* qr, quicrq_congestion_control_enum congestion_control_mode)
{
    if (congestion_control_mode < 0 || congestion_control_mode >= quicrq_congestion_control_max)
        qr->congestion_control_mode = quicrq_congestion_control_delay;
    else {
        qr->congestion_control_mode = congestion_control_mode;
    }
}

/* Prepare to send a datagram */

int quicrq_prepare_to_send_datagram(quicrq_cnx_ctx_t* cnx_ctx, void* context, size_t space, uint64_t current_time)
{
    /* Find a stream on which datagrams are available */
    int ret = 0;
    int at_least_one_active = 0;
    quicrq_stream_ctx_t* stream_ctx = cnx_ctx->first_stream;

    /* TODO: handle congestion. Check whether one stream is congested. 
     * look at priority levels, etc.
     */

    while (stream_ctx != NULL) {
        if (stream_ctx->transport_mode == quicrq_transport_mode_datagram && stream_ctx->is_sender && stream_ctx->is_active_datagram && stream_ctx->media_id < UINT64_MAX) {
            int media_was_sent = 0;
            ret = quicrq_fragment_datagram_publisher_fn(stream_ctx, context, space, &media_was_sent, &at_least_one_active, current_time);
            if (media_was_sent || ret != 0) {
                break;
            }
            else {
                stream_ctx->is_active_datagram = 0;
            }
        }
        stream_ctx = stream_ctx->next_stream;
    }

    if (ret == 0) {
        picoquic_mark_datagram_ready(cnx_ctx->cnx, at_least_one_active);
    }

    return ret;
}

/* Send the next message on a stream.
 * Messages include:
 * - initial opening message sent by the client, for either receiving or publishing a fragment,
 *   and specifying stream or datagram.
 * - possibly, initial synchronization message sent by the server in response to the client
 *   publishing some media.
 * - if sending the media as stream, stream data until the steam is closed.
 * - if sending the media as datagram and repairs are queued, repair messages.
 * - if repair message in progress, header followed by media.
 * - if all media sent as datagram, final offset message.
 * - possibly, if media received as datagram, repair request messages.
 * 
 * The behavior depends on the state of the stream, and at least two variables: is a message sending in progress,
 * and, is there something else to send after that message.
 * 
 * If no message is being sent, the application looks at what is queued. If sending media, that
 * means checking the repair queue, and also checking whether the final offset needs to be sent.
 * The header for the next message is then formatted, and the application sends that message.
 * 
 * If a protocol message is currently being sent, the application fills the buffer
 * with the next bytes in that message. There is a special case for repair messages,
 * which include a header and then teh original content of the lost datagram.
 * 
 * When the application is done sending the message, it updates it state, e.g., mark the
 * offset as sent and dequeues the repair message.
 * 
 * The sender will close the stream after the receiver has closed it.
 */
int quicrq_prepare_start_point(quicrq_stream_ctx_t* stream_ctx)
{
    int ret = 0;
    quicrq_message_buffer_t* message = &stream_ctx->message_sent;

    quicrq_log_message(stream_ctx->cnx_ctx,
        "Stream %" PRIu64 ", sending start object id: %" PRIu64 "/%" PRIu64,
        stream_ctx->stream_id, stream_ctx->start_group_id, stream_ctx->start_object_id);
    if (quicrq_msg_buffer_alloc(message, quicrq_start_point_msg_reserve(stream_ctx->start_group_id, stream_ctx->start_object_id), 0) != 0) {
        ret = -1;
    }
    else {
        uint8_t* message_next = quicrq_start_point_msg_encode(message->buffer, message->buffer + message->buffer_alloc, QUICRQ_ACTION_START_POINT,
            stream_ctx->start_group_id, stream_ctx->start_object_id);
        if (message_next == NULL) {
            ret = -1;
        }
        else {
            /* Queue the media request message to that stream */
            message->message_size = message_next - message->buffer;
            stream_ctx->send_state = quicrq_sending_start_point;

            picoquic_log_app_message(stream_ctx->cnx_ctx->cnx, "Sending start point on stream %" PRIu64 ", object id %" PRIu64 "/%" PRIu64,
                stream_ctx->stream_id, stream_ctx->start_group_id, stream_ctx->start_object_id);
        }
    }
    return ret;
}

int quicrq_prepare_to_send_on_stream(quicrq_stream_ctx_t* stream_ctx, void* context, size_t space, uint64_t current_time)
{
    int ret = 0;
    int more_to_send = 0;

    if (stream_ctx->send_state == quicrq_sending_ready) {
        quicrq_message_buffer_t* message = &stream_ctx->message_sent;
        /* Ready to send next message */
        if (stream_ctx->is_sender) {
            if ((stream_ctx->start_group_id > 0 || stream_ctx->start_object_id > 0) && !stream_ctx->is_start_object_id_sent) {
                ret = quicrq_prepare_start_point(stream_ctx);
            }
            else if ((stream_ctx->final_group_id > 0 || stream_ctx->final_object_id > 0) &&
                !stream_ctx->is_final_object_id_sent) {
                quicrq_log_message(stream_ctx->cnx_ctx, 
                    "Stream %" PRIu64 ", sending final group id: %" PRIu64 ", object id : % " PRIu64,
                    stream_ctx->stream_id, stream_ctx->final_group_id, stream_ctx->final_object_id);
                /* TODO: encode the final offset message in the protocol buffer */
                if (quicrq_msg_buffer_alloc(message, quicrq_fin_msg_reserve(stream_ctx->final_group_id, stream_ctx->final_object_id), 0) != 0) {
                    ret = -1;
                }
                else {
                    uint8_t* message_next = quicrq_fin_msg_encode(message->buffer, message->buffer + message->buffer_alloc, QUICRQ_ACTION_FIN_DATAGRAM,
                        stream_ctx->final_group_id, stream_ctx->final_object_id);
                    if (message_next == NULL) {
                        ret = -1;
                    }
                    else {
                        /* Queue the media request message to that stream */
                        message->message_size = message_next - message->buffer;
                        stream_ctx->send_state = quicrq_sending_final_point;
                    }
                }
            }
            else if (stream_ctx->is_cache_real_time && !stream_ctx->is_cache_policy_sent) {
                quicrq_log_message(stream_ctx->cnx_ctx,
                    "Stream %" PRIu64 ", sending cache policy: %" PRIu64 "/%d",
                    stream_ctx->stream_id, stream_ctx->is_cache_real_time);
                if (quicrq_msg_buffer_alloc(message, quicrq_cache_policy_msg_reserve(), 0) != 0) {
                    ret = -1;
                }
                else {
                    uint8_t* message_next = quicrq_cache_policy_msg_encode(message->buffer, message->buffer + message->buffer_alloc, QUICRQ_ACTION_CACHE_POLICY, 1);
                    if (message_next == NULL) {
                        ret = -1;
                    }
                    else {
                        /* Queue the media request message to that stream */
                        message->message_size = message_next - message->buffer;
                        stream_ctx->send_state = quicrq_sending_cache_policy;
                    }
                }
            }
            else if (stream_ctx->transport_mode == quicrq_transport_mode_single_stream && quicrq_fragment_is_ready_to_send(stream_ctx->media_ctx, space, current_time)) {
                stream_ctx->send_state = quicrq_sending_single_stream;
            }
            else {
                /* This is a bug. If there is nothing to send, we should not be sending any stream data */
                quicrq_log_message(stream_ctx->cnx_ctx, "Nothing to send on stream %" PRIu64 ", state: %d, final: %" PRIu64,
                    stream_ctx->stream_id, stream_ctx->send_state, stream_ctx->final_object_id);
                DBG_PRINTF("Nothing to send on stream %" PRIu64 ", state: %d, final: %" PRIu64 ",%" PRIu64,
                    stream_ctx->stream_id, stream_ctx->send_state,
                    stream_ctx->final_group_id, stream_ctx->final_object_id);
                picoquic_mark_active_stream(stream_ctx->cnx_ctx->cnx, stream_ctx->stream_id, 0, stream_ctx);
            }
        }
        else {
            /* TODO: consider receiver messages */
            quicrq_log_message(stream_ctx->cnx_ctx,
                "Consider receiver messages on stream %" PRIu64 ", final: %" PRIu64 ", %" PRIu64,
                stream_ctx->stream_id, stream_ctx->final_group_id, stream_ctx->final_object_id);
            DBG_PRINTF(
                "Consider receiver messages on stream %" PRIu64 ", final: %" PRIu64 ", %" PRIu64,
                stream_ctx->stream_id, stream_ctx->final_group_id, stream_ctx->final_object_id);
        }
    }
    else if (stream_ctx->send_state == quicrq_notify_ready) {
        if (stream_ctx->first_notify_url != NULL) {
            quicrq_notify_url_t* notified = stream_ctx->first_notify_url;
            quicrq_message_buffer_t* message = &stream_ctx->message_sent;
            if (quicrq_msg_buffer_alloc(message, quicrq_notify_msg_reserve(notified->url_len), 0) != 0) {
                ret = -1;
            }
            else {
                uint8_t* message_next = quicrq_notify_msg_encode(message->buffer, message->buffer + message->buffer_alloc,
                    QUICRQ_ACTION_NOTIFY, notified->url_len, notified->url);
                if (message_next == NULL) {
                    ret = -1;
                }
                else {
                    /* Queue the media request message to that stream */
                    char buffer[256];

                    message->message_size = message_next - message->buffer;
                    stream_ctx->send_state = quicrq_sending_notify;

                    quicrq_log_message(stream_ctx->cnx_ctx, "On stream %" PRIu64 ", notify URL:%s",
                        stream_ctx->stream_id,
                        quicrq_uint8_t_to_text(notified->url, notified->url_len, buffer, 256));

                    stream_ctx->first_notify_url = notified->next_notify_url;
                    /* This free assumes the url bytes were allocated with the notified struct */
                    free(notified);
                }
            }
        }
    }

    if (ret == 0){
        switch (stream_ctx->send_state) {
        case quicrq_sending_ready:
            /* Nothing to send. Mark the stream as not active. */
            picoquic_mark_active_stream(stream_ctx->cnx_ctx->cnx, stream_ctx->stream_id, 0, stream_ctx);
            break;
        case quicrq_sending_single_stream:
            /* Send available stream data. Check whether the FIN is reached. */
            ret = quicrq_prepare_to_send_media_to_stream(stream_ctx, context, space, current_time);
            break;
        case quicrq_sending_initial:
            /* Send available buffer data. Mark state ready after sent. */
            more_to_send = (stream_ctx->final_group_id > 0 || stream_ctx->final_object_id > 0) && !stream_ctx->is_final_object_id_sent;
            ret = quicrq_msg_buffer_prepare_to_send(stream_ctx, context, space, more_to_send);
            break;
        case quicrq_sending_repair:
            /* Send available buffer data and repair data. Dequeue repair and mark state ready after sent. */
            more_to_send = (stream_ctx->final_group_id > 0 || stream_ctx->final_object_id > 0) && !stream_ctx->is_final_object_id_sent;
            ret = quicrq_msg_buffer_prepare_to_send(stream_ctx, context, space, more_to_send);
            break;
        case quicrq_sending_final_point:
            /* Send available buffer data and repair data. Mark offset sent and mark state ready after sent. */
            ret = quicrq_msg_buffer_prepare_to_send(stream_ctx, context, space, more_to_send);
            if (stream_ctx->send_state == quicrq_sending_ready){
                stream_ctx->is_final_object_id_sent = 1;
                if (stream_ctx->close_reason == quicrq_media_close_reason_unknown) {
                    stream_ctx->close_reason = quicrq_media_close_finished;
                }
            }
            break;
        case quicrq_sending_start_point:
            more_to_send |= (!stream_ctx->is_cache_policy_sent && stream_ctx->is_cache_real_time);
            ret = quicrq_msg_buffer_prepare_to_send(stream_ctx, context, space, more_to_send);
            stream_ctx->is_start_object_id_sent = 1;
            stream_ctx->send_state = quicrq_sending_ready;
            break;
        case quicrq_sending_cache_policy:
            more_to_send |= (!stream_ctx->is_start_object_id_sent && (stream_ctx->start_group_id > 0 || stream_ctx->start_object_id > 0));
            ret = quicrq_msg_buffer_prepare_to_send(stream_ctx, context, space, more_to_send);
            stream_ctx->is_cache_policy_sent = 1;
            stream_ctx->send_state = quicrq_sending_ready;
            break;
        case quicrq_sending_subscribe:
            ret = quicrq_msg_buffer_prepare_to_send(stream_ctx, context, space, 0);
            if (stream_ctx->send_state == quicrq_sending_ready) {
                stream_ctx->send_state = quicrq_waiting_notify;
            }
            break;
        case quicrq_sending_notify:
            more_to_send = (stream_ctx->first_notify_url != NULL);
            ret = quicrq_msg_buffer_prepare_to_send(stream_ctx, context, space, more_to_send);
            if (stream_ctx->send_state == quicrq_sending_ready) {
                stream_ctx->send_state = quicrq_notify_ready;
            }
            break;
        case quicrq_waiting_notify:
        case quicrq_notify_ready:
            /* Nothing to send in that state -- make sure the stream is not active */
            DBG_PRINTF("Unexpected state %s on stream %" PRIu64, stream_ctx->send_state, stream_ctx->stream_id);
            (void)picoquic_provide_stream_data_buffer(context, 0, 0, 0);
            break;
        case quicrq_sending_fin:
            /* TODO; send the fin bit on the stream (uni), clean up stream_ctx for that uni stream */
            (void) picoquic_provide_stream_data_buffer(context, 0, 1, 0);
            stream_ctx->send_state = quicrq_sending_no_more;
            stream_ctx->is_local_finished = 1;
            if (stream_ctx->is_peer_finished) {
                if (stream_ctx->close_reason == quicrq_media_close_reason_unknown) {
                    stream_ctx->close_reason = quicrq_media_close_remote_application;
                }
                quicrq_delete_stream_ctx(stream_ctx->cnx_ctx, stream_ctx);
            }
            break;
        default:
            /* Someone forgot to upgrade this code... */
            quicrq_log_message(stream_ctx->cnx_ctx, "Unexpected state %s on stream %" PRIu64, stream_ctx->send_state, stream_ctx->stream_id);
            DBG_PRINTF("Unexpected state %s on stream %" PRIu64, stream_ctx->send_state, stream_ctx->stream_id);
            ret = -1;
            break;
        }
    }

    return ret;
}
/* Prepare the warp header */
int quicrq_prepare_warp_header(quicrq_uni_stream_ctx_t* uni_stream_ctx) 
{
    int ret = 0;

    quicrq_message_buffer_t* message = &uni_stream_ctx->message_buffer;
    if (quicrq_msg_buffer_alloc(message, quicrq_warp_header_msg_reserve(uni_stream_ctx->control_stream_ctx->media_id, uni_stream_ctx->current_group_id), 0) != 0) {
        ret = -1;
    }
    else {
        uint8_t* message_next = quicrq_warp_header_msg_encode(message->buffer,
            message->buffer + message->buffer_alloc,
            QUICRQ_ACTION_WARP_HEADER,
            uni_stream_ctx->control_stream_ctx->media_id,
            uni_stream_ctx->current_group_id);
        if (message_next == NULL) {
            ret = -1;
        }
        message->message_size = message_next - message->buffer;
        uni_stream_ctx->send_state = quicrq_sending_warp_header_sent;
    }

    return ret;
}

/* Prepare the warp object header */
int quicrq_prepare_warp_object_header(quicrq_uni_stream_ctx_t* uni_stream_ctx, uint64_t current_time) 
{
    int ret = 0;

    /* This handles both RUSH mode and WARP mode. RUSH sends only one object per uni stream,
    * as specified in the uni-stream context. This means there is no need to check "last object
    * id" -- it is alsways, "the specified object plus 1." We rely on the uni stream creation
    * setting the last object ID to zero for WARP, and to "object+1" for RUSH.
    */
    quicrq_fragment_publisher_context_t* media_ctx = uni_stream_ctx->control_stream_ctx->media_ctx;
    quicrq_fragment_cache_t* cache_ctx = media_ctx->cache_ctx;
    /* Check whether the fin object for the group is known */
    if (uni_stream_ctx->last_object_id == 0) {
        /* see if we have media stream has reported its final group already
        * (in Rush mode, this is known as we only send on object per uni stream.)
        */
        if ((uni_stream_ctx->control_stream_ctx->final_group_id > 0 ||
            uni_stream_ctx->control_stream_ctx->final_object_id > 0) &&
            uni_stream_ctx->control_stream_ctx->final_group_id == uni_stream_ctx->current_group_id) {
            uni_stream_ctx->last_object_id = uni_stream_ctx->control_stream_ctx->final_object_id;
        }
        else {
            /* check to see if we know object_count from this group */
            uni_stream_ctx->last_object_id = quicrq_fragment_get_object_count(cache_ctx, uni_stream_ctx->current_group_id);
        }
    }

    if (uni_stream_ctx->last_object_id > 0 && uni_stream_ctx->current_object_id >= uni_stream_ctx->last_object_id) {
        /* we have sent all the objects from the current group */
        uni_stream_ctx->send_state = quicrq_sending_warp_all_sent;
    }
    else {
        /* Check whether the next fragment is available */
        uint8_t flags = 0;
        uint64_t nb_objects_previous_group = 0;
        /* Todo: we may send this immediately, as soon as the object length is known. */
        if (quicrq_fragment_get_object_properties(cache_ctx, uni_stream_ctx->current_group_id, uni_stream_ctx->current_object_id,
            &uni_stream_ctx->current_object_length, &uni_stream_ctx->nb_objects_previous_group, 
            &uni_stream_ctx->current_object_flags) == 0){
            int should_skip = 0;
            quicrq_message_buffer_t* message = &uni_stream_ctx->message_buffer;
            uint8_t* message_next = NULL;

            should_skip = quicrq_evaluate_warp_congestion(uni_stream_ctx, media_ctx, uni_stream_ctx->current_object_length, flags, current_time);

            if (should_skip) {
                uni_stream_ctx->current_object_length = 0;
                uni_stream_ctx->current_object_flags = 0xff;
            }
            /* Encode object header */
            if (quicrq_msg_buffer_alloc(message, quicrq_object_header_msg_reserve(uni_stream_ctx->current_object_id, 
                nb_objects_previous_group, uni_stream_ctx->current_object_length), 0) != 0) {
                ret = -1;
            }
            else {
                message_next = quicrq_object_header_msg_encode(message->buffer,
                    message->buffer + message->buffer_alloc,
                    QUICRQ_ACTION_OBJECT_HEADER, uni_stream_ctx->current_object_id,
                    uni_stream_ctx->nb_objects_previous_group,
                    uni_stream_ctx->current_object_flags, uni_stream_ctx->current_object_length);

                if (message_next == NULL) {
                    ret = -1;
                }
                else {
                    message->message_size = message_next - message->buffer;
                }
                if (uni_stream_ctx->current_object_length == 0) {
                    /* No need to wait for transmission of the object data! */
                    uni_stream_ctx->current_object_id++;
                    uni_stream_ctx->send_state = quicrq_sending_warp_header_sent;
                }
                else {
                    uni_stream_ctx->send_state = quicrq_sending_object_data;
                }
            }
        }
        else {
            /* Not available. Could it be because the final object ID has been reached? */
            quicrq_fragment_notify_final_to_control(cache_ctx, uni_stream_ctx->control_stream_ctx);
        }
    }
    return ret;
}
/* Sending data on unidirectional stream, for warp mode */
int quicrq_prepare_to_send_on_unistream(quicrq_cnx_ctx_t * cnx_ctx, quicrq_uni_stream_ctx_t * uni_stream_ctx, void* context, size_t space, uint64_t current_time)
{
    int ret = 0;
    /* prepare the message that needs to be sent */
    if (uni_stream_ctx->send_state == quicrq_sending_object_data) {
        /* Todo: if content is available, send it */
        /* When done: back to quicrq_sending_warp_header_sent */
        quicrq_fragment_publisher_context_t* media_ctx = uni_stream_ctx->control_stream_ctx->media_ctx;
        quicrq_fragment_cache_t* cache_ctx = media_ctx->cache_ctx;
        size_t fragment_length = quicrq_fragment_object_copy_available_data(cache_ctx,
            uni_stream_ctx->current_group_id, uni_stream_ctx->current_object_id,
            uni_stream_ctx->current_object_offset, space, NULL);

        if (fragment_length > 0) {
            uint8_t* buffer = (uint8_t*)picoquic_provide_stream_data_buffer(context, fragment_length, 0, 1);
            if (buffer == NULL) {
                ret = -1;
            }
            else {
                size_t copied_length = quicrq_fragment_object_copy_available_data(cache_ctx,
                    uni_stream_ctx->current_group_id, uni_stream_ctx->current_object_id,
                    uni_stream_ctx->current_object_offset, fragment_length, buffer);
                if (copied_length != fragment_length) {
                    ret = -1;
                }
                else {
                    uni_stream_ctx->current_object_offset += copied_length;
                    if (uni_stream_ctx->current_object_offset == uni_stream_ctx->current_object_length) {
                        /* this object is sent, back to state quicrq_sending_warp_header_sent */
                        uni_stream_ctx->current_object_id++;
                        uni_stream_ctx->current_object_offset = 0;
                        uni_stream_ctx->send_state = quicrq_sending_warp_header_sent;
                    } else if (uni_stream_ctx->current_object_offset > uni_stream_ctx->current_object_length) {
                        ret = -1;
                    }
                }
            }
        }
        else {
            /* Nothing to send yet. */
            ret = picoquic_mark_active_stream(cnx_ctx->cnx, uni_stream_ctx->stream_id, 0, uni_stream_ctx);
        }
    }
    else {
        if (uni_stream_ctx->message_buffer.message_size == 0) {
            if (uni_stream_ctx->send_state == quicrq_sending_open) {
                /* prepare warp_header, since its the first time we are dealing with this stream */
                ret = quicrq_prepare_warp_header(uni_stream_ctx);
            }
            else if (uni_stream_ctx->send_state == quicrq_sending_warp_header_sent) {
                ret = quicrq_prepare_warp_object_header(uni_stream_ctx, current_time);
            }
        }

        if (uni_stream_ctx->message_buffer.message_size > 0) {
            /* Send the message that was just prepared */
            int more_to_send = uni_stream_ctx->send_state != quicrq_sending_warp_should_close;
            quicrq_log_message(cnx_ctx, "Send:UniStream %" PRIu64 ",  message buffer size = %" PRIu8,
                uni_stream_ctx->stream_id, uni_stream_ctx->message_buffer.message_size);
            ret = quicrq_msg_buffer_prepare_to_send_message(&uni_stream_ctx->message_buffer, context, space, more_to_send);
        } else {
            /* Nothing to send: maybe end of stream, maybe need to wait */
            if (uni_stream_ctx->send_state == quicrq_sending_warp_all_sent) {
                /* Send the fin bit on the stream (uni), clean up stream_ctx for that uni stream */
                (void)picoquic_provide_stream_data_buffer(context, 0, 1, 0);
                uni_stream_ctx->send_state = quicrq_sending_warp_should_close;
                /* Dispose of uni stream context. */
                quicrq_delete_uni_stream_ctx(cnx_ctx, uni_stream_ctx);
            }
            else {
                /* Nothing to send */
                ret = picoquic_mark_active_stream(cnx_ctx->cnx, uni_stream_ctx->stream_id, 0, uni_stream_ctx);
            }
        }
    }

    return ret;
}

/* Processing of subscribe and notify messages */

int quicrq_notify_url_to_stream(quicrq_stream_ctx_t* stream_ctx, const uint8_t* url, size_t url_length)
{
    int ret = 0;
    /* Store the subscribe parameters */
    if (url_length >= stream_ctx->subscribe_prefix_length &&
        memcmp(url, stream_ctx->subscribe_prefix, stream_ctx->subscribe_prefix_length) == 0) {
        quicrq_notify_url_t* notified = (quicrq_notify_url_t*)
            malloc(sizeof(quicrq_notify_url_t) + url_length);
        if (notified == NULL) {
            ret = -1;
        }
        else {
            memset(notified, 0, sizeof(quicrq_notify_url_t));
            notified->next_notify_url = stream_ctx->first_notify_url;
            notified->url_len = url_length;
            notified->url_len = url_length;
            notified->url = ((uint8_t*)notified) + sizeof(quicrq_notify_url_t);
            memcpy(notified->url, url, url_length);
            stream_ctx->first_notify_url = notified;
            quicrq_wakeup_media_stream(stream_ctx);
            ret = 1;
        }
    }
    return ret;
}

int quicrq_notify_url_to_all(quicrq_ctx_t* qr_ctx, const uint8_t* url, size_t url_length)
{
    int ret = 0;
    quicrq_cnx_ctx_t* cnx_ctx = qr_ctx->first_cnx;

    while (cnx_ctx != NULL && ret == 0) {
        quicrq_stream_ctx_t* stream_ctx = cnx_ctx->first_stream;

        while (stream_ctx != NULL) {
            if (stream_ctx->send_state == quicrq_notify_ready) {
                if ((ret = quicrq_notify_url_to_stream(stream_ctx, url, url_length)) > 0) {
                    ret = 0;
                    break;
                }
            }
            stream_ctx = stream_ctx->next_stream;
        }
        cnx_ctx = cnx_ctx->next_cnx;
    }

    return ret;
}

int quicrq_process_incoming_subscribe(quicrq_stream_ctx_t* stream_ctx, size_t url_length, const uint8_t* url)
{
    int ret = 0;
    quicrq_ctx_t* qr_ctx = stream_ctx->cnx_ctx->qr_ctx;
    /* Store the subscribe parameters */
    stream_ctx->subscribe_prefix = malloc(url_length + 1);
    if (stream_ctx->subscribe_prefix == NULL) {
        ret = -1;
    }
    else {
        stream_ctx->subscribe_prefix_length = url_length;
        memcpy(stream_ctx->subscribe_prefix, url, url_length);
        stream_ctx->receive_state = quicrq_receive_done;
        stream_ctx->send_state = quicrq_notify_ready;
    }
    if (ret == 0) {
        /* Check all the known media source, see whether they match */
        quicrq_media_source_ctx_t* srce_ctx = qr_ctx->first_source;

        while (srce_ctx != NULL) {
            if (quicrq_notify_url_to_stream(stream_ctx, srce_ctx->media_url, srce_ctx->media_url_length) < 0) {
                ret = -1;
                break;
            }
            else {
                srce_ctx = srce_ctx->next_source;
            }
        }
    }

    /* TODO: if this is a relay, perform necessary actions */
    return ret;
}

/* Receive and process media control messages.
 * This is governed by the receive state variable, with the following values:
 * - not yet ready: the state of a client stream, before sending the initial message.
 * - receive initial: the state of a server stream, when it was just created.
 * - receive confirmation: the state of a client after sending a post
 * - receive stream: receiver state if expecting media on stream.
 * - receive repair: while receiving datagrams, receive repairs, or the final offset
 * - receive done: waiting for end of data by the peer.
 * The media receiver closes the stream when the fragment is completely received,
 * or when the receiver stopped listening, or if the sender closed its own stream.
 * The media sender closes the stream if the receiver closes it, or if the sender
 * has to abandon the stream. 
 */

int quicrq_receive_stream_data(quicrq_stream_ctx_t* stream_ctx, uint8_t* bytes, size_t length, int is_fin)
{
    int ret = 0;

    while (ret == 0 && length > 0) {
        /* There may be a set of messages back to back, and all have to be received. */
        if (stream_ctx->receive_state == quicrq_receive_done) {
            /* Protocol violation, was not expecting any message */
            ret = -1;
            break;
        }
        else {
            /* Receive the next message on the stream, if any */
            int is_finished = 0;
            uint8_t* next_bytes = quicrq_msg_buffer_store(bytes, length, &stream_ctx->message_receive, &is_finished);
            if (next_bytes == NULL) {
                /* Something went wrong */
                ret = -1;
            }
            else
            {
                length = (bytes + length) - next_bytes;
                bytes = next_bytes;
                if (is_finished) {
                    /* Decode the incoming message */
                    quicrq_message_t incoming = { 0 };
                    const uint8_t* r_bytes = quicrq_msg_decode(stream_ctx->message_receive.buffer, stream_ctx->message_receive.buffer + stream_ctx->message_receive.message_size, &incoming);

                    if (r_bytes == NULL) {
                        /* Message was incorrect */
                        ret = -1;
                    }
                    else switch (incoming.message_type) {
                    case QUICRQ_ACTION_REQUEST:
                        if (stream_ctx->receive_state != quicrq_receive_initial) {
                            quicrq_log_message(stream_ctx->cnx_ctx, "Stream %" PRIu64 ", unexpected subscribe message is stream receive state %d",
                                stream_ctx->stream_id, stream_ctx->receive_state);
                            ret = -1;
                        }
                        else {
                            char url_text[256];
                            uint64_t intent_group = 0;
                            uint64_t intent_object = 0;

                            /* Process initial request */
                            stream_ctx->media_id = incoming.media_id;
                            stream_ctx->transport_mode = incoming.transport_mode;
                            /* Open the media -- TODO, variants with different actions. */
                            quicrq_log_message(stream_ctx->cnx_ctx, "Stream %" PRIu64 ", received a subscribe request for url %s, mode = %s, id= %" PRIu64,
                                stream_ctx->stream_id, quicrq_uint8_t_to_text(incoming.url, incoming.url_length, url_text, 256),
                                quicrq_transport_mode_to_string(stream_ctx->transport_mode), incoming.media_id);
                            ret = quicrq_subscribe_local_media(stream_ctx, incoming.url, incoming.url_length);
                            if (ret == 0) {
                                quicrq_wakeup_media_stream(stream_ctx);
                            }
                            if (ret == 0) {
                                /* Apply the preferences based on intent */
                                stream_ctx->is_sender = 1;
                                switch (incoming.subscribe_intent) {
                                case quicrq_subscribe_intent_current_group:
                                    intent_group = stream_ctx->media_ctx->cache_ctx->next_group_id;
                                    intent_object = 0;
                                    break;
                                case quicrq_subscribe_intent_next_group:
                                    intent_group = stream_ctx->media_ctx->cache_ctx->next_group_id + 1;
                                    intent_object = 0;
                                    break;
                                case quicrq_subscribe_intent_start_point:
                                    intent_group = incoming.group_id;
                                    intent_object = incoming.object_id;
                                    break;
                                default:
                                    break;
                                }
                                /* Override the intent if impossible to meet */
                                if (stream_ctx->start_group_id > 0 || stream_ctx->start_object_id > 0) {
                                    if (intent_group < stream_ctx->next_group_id ||
                                        (intent_group == stream_ctx->next_group_id &&
                                            intent_object < stream_ctx->next_object_id)) {
                                        intent_group = stream_ctx->start_group_id;
                                        intent_object = stream_ctx->start_object_id;
                                    }
                                }
                            }
                            if (intent_group > 0 || intent_object > 0) {
                                /* apply the intent, prepare a start point message */
                                stream_ctx->start_group_id = intent_group;
                                stream_ctx->start_object_id = intent_object;
                                stream_ctx->next_group_id = intent_group;
                                stream_ctx->next_object_id = intent_object;
                                stream_ctx->media_ctx->current_group_id = intent_group;
                                stream_ctx->media_ctx->current_object_id = intent_object;
                                stream_ctx->media_ctx->current_offset = 0;
                                ret = quicrq_prepare_start_point(stream_ctx);
                                stream_ctx->receive_state = quicrq_receive_done;
                                picoquic_mark_active_stream(stream_ctx->cnx_ctx->cnx, stream_ctx->stream_id, 1, stream_ctx);
                            }
                            else if (incoming.transport_mode == quicrq_transport_mode_single_stream) {
                                /* Start sending stream without endpoint message */
                                stream_ctx->send_state = quicrq_sending_single_stream;
                                stream_ctx->receive_state = quicrq_receive_done;
                                picoquic_mark_active_stream(stream_ctx->cnx_ctx->cnx, stream_ctx->stream_id, 1, stream_ctx);
                            }
                            else if (incoming.transport_mode == quicrq_transport_mode_datagram
                                || incoming.transport_mode == quicrq_transport_mode_warp
                                || incoming.transport_mode == quicrq_transport_mode_rush) {
                                /* Start sending data without endpoint message */
                                stream_ctx->send_state = quicrq_sending_ready;
                                stream_ctx->receive_state = quicrq_receive_done;
                            }
                            else {
                                /* Not supported yet */
                                ret = -1;
                            }
                        }
                        break;
                    case QUICRQ_ACTION_POST:
                        if (stream_ctx->receive_state != quicrq_receive_initial) {
                            quicrq_log_message(stream_ctx->cnx_ctx, "Stream %" PRIu64 ", unexpected publish message is stream receive state %d",
                                stream_ctx->stream_id, stream_ctx->receive_state);
                            /* TODO:
                               Post should indicate status, as well as first object?
                             */
                            ret = -1;
                        }
                        else {
                            char url_text[256];
                            quicrq_log_message(stream_ctx->cnx_ctx, "Stream %" PRIu64 ", received a publish request for url %s, mode = %s",
                                stream_ctx->stream_id, quicrq_uint8_t_to_text(incoming.url, incoming.url_length, url_text, 256),
                                quicrq_transport_mode_to_string(incoming.transport_mode));
                            /* Decide whether to receive the data as stream or as datagrams */
                            /* Prepare a consumer for the data. */
                            ret = quicrq_cnx_accept_media(stream_ctx, incoming.url, incoming.url_length, incoming.transport_mode,
                                incoming.cache_policy, incoming.group_id, incoming.object_id);
                        }
                        break;
                    case QUICRQ_ACTION_ACCEPT:
                        /* Verify that the client just started a "post" -- if (stream_ctx->receive_state != quicrq_receive_initial) { */
                        /* Open the media provider */
                        /* Depending on mode, set media ready or datagram ready */
                        quicrq_log_message(stream_ctx->cnx_ctx, "Stream %" PRIu64 ", publish request accepted, mode = %s",
                            stream_ctx->stream_id, quicrq_transport_mode_to_string(incoming.transport_mode));
                        ret = quicrq_cnx_post_accepted(stream_ctx, incoming.transport_mode, incoming.media_id);
                        break;
                    case QUICRQ_ACTION_START_POINT:
                        if (stream_ctx->receive_state != quicrq_receive_fragment || stream_ctx->start_group_id != 0 || stream_ctx->start_object_id != 0) {
                            /* Protocol error */
                            ret = -1;
                        }
                        else {
                            /* Pass the start point to the media consumer. */
                            quicrq_log_message(stream_ctx->cnx_ctx,
                                "Stream %" PRIu64 ", start point notified: %" PRIu64 "/%" PRIu64,
                                stream_ctx->stream_id, incoming.group_id, incoming.object_id);
                            stream_ctx->start_group_id = incoming.group_id;
                            stream_ctx->start_object_id = incoming.object_id;
                            ret = stream_ctx->consumer_fn(quicrq_media_start_point, stream_ctx->media_ctx, picoquic_get_quic_time(stream_ctx->cnx_ctx->qr_ctx->quic),
                                NULL, incoming.group_id, incoming.object_id, 0, 0, incoming.flags, 0, 0, 0);

                            ret = quicrq_cnx_handle_consumer_finished(stream_ctx, 0, 0, ret);
                        }
                        break;
                    case QUICRQ_ACTION_FIN_DATAGRAM:
                        if (stream_ctx->receive_state != quicrq_receive_fragment ||
                            (stream_ctx->final_object_id != 0 || stream_ctx->final_object_id != 0)) {
                            /* Protocol error */
                            ret = -1;
                        }
                        else {
                            quicrq_log_message(stream_ctx->cnx_ctx,
                                "Stream %" PRIu64 ", final point notified: %" PRIu64 "/%" PRIu64,
                                stream_ctx->stream_id, incoming.group_id, incoming.object_id);
                            /* Pass the final offset to the media consumer. */
                            ret = stream_ctx->consumer_fn(quicrq_media_final_object_id, stream_ctx->media_ctx, picoquic_get_quic_time(stream_ctx->cnx_ctx->qr_ctx->quic), NULL,
                                incoming.group_id, incoming.object_id, 0, 0, 0, 0, 0, 0);
                            ret = quicrq_cnx_handle_consumer_finished(stream_ctx, 1, 0, ret);
                        }
                        break;
                    case QUICRQ_ACTION_FRAGMENT:
                        if (stream_ctx->receive_state != quicrq_receive_fragment) {
                            /* Protocol error */
                            ret = -1;
                        }
                        else {
                            /* Verification that there are no unexpected fragments, used in tests */
                            if (incoming.group_id < stream_ctx->start_group_id ||
                                (incoming.group_id == stream_ctx->start_group_id &&
                                    incoming.object_id < stream_ctx->start_object_id)) {
                                stream_ctx->cnx_ctx->qr_ctx->useless_fragments++;
                            }
                            /* Pass the fragment data to the media consumer. */
                            ret = stream_ctx->consumer_fn(quicrq_media_datagram_ready, stream_ctx->media_ctx, picoquic_get_quic_time(stream_ctx->cnx_ctx->qr_ctx->quic),
                                incoming.data, incoming.group_id, incoming.object_id,
                                incoming.fragment_offset, 0, incoming.flags, incoming.nb_objects_previous_group,
                                incoming.object_length, incoming.fragment_length);
                            ret = quicrq_cnx_handle_consumer_finished(stream_ctx, 0, 0, ret);
                        }
                        break;
                    case QUICRQ_ACTION_SUBSCRIBE:
                        if (stream_ctx->receive_state != quicrq_receive_initial) {
                            quicrq_log_message(stream_ctx->cnx_ctx, "Stream %" PRIu64 ", unexpected subscribe pattern message is stream receive state %d",
                                stream_ctx->stream_id, stream_ctx->receive_state);
                            ret = -1;
                        }
                        else {
                            char url_text[256];
                            /* Process initial request */
                            quicrq_log_message(stream_ctx->cnx_ctx, "Stream %" PRIu64 ", received subscribe pattern request for url %s",
                                stream_ctx->stream_id, quicrq_uint8_t_to_text(incoming.url, incoming.url_length, url_text, 256));
                            /* Create the subscription state */
                            ret = quicrq_process_incoming_subscribe(stream_ctx, incoming.url_length, incoming.url);
                            /* If relay, create source and forward the request */
                            if (stream_ctx->cnx_ctx->qr_ctx->manage_relay_subscribe_fn != NULL) {
                                stream_ctx->cnx_ctx->qr_ctx->manage_relay_subscribe_fn(stream_ctx->cnx_ctx->qr_ctx,
                                    quicrq_subscribe_action_subscribe, incoming.url, incoming.url_length);
                            }
                        }
                        break;
                    case QUICRQ_ACTION_NOTIFY:
                        if (stream_ctx->receive_state != quicrq_receive_notify) {
                            quicrq_log_message(stream_ctx->cnx_ctx, "Stream %" PRIu64 ", unexpected subscribe pattern message is stream receive state %d",
                                stream_ctx->stream_id, stream_ctx->receive_state);
                            ret = -1;
                        }
                        if (stream_ctx->media_notify_fn != NULL) {
                            stream_ctx->media_notify_fn(stream_ctx->notify_ctx, incoming.url, incoming.url_length);
                        }
                        break;
                    case QUICRQ_ACTION_CACHE_POLICY:
                        if (stream_ctx->receive_state != quicrq_receive_fragment || stream_ctx->is_cache_real_time) {
                            /* Protocol error */
                            ret = -1;
                        }
                        else {
                            /* Pass the start point to the media consumer. */
                            quicrq_log_message(stream_ctx->cnx_ctx,
                                "Stream %" PRIu64 ", cache policy: %d",
                                stream_ctx->stream_id, incoming.cache_policy);
                            stream_ctx->is_cache_real_time = (incoming.cache_policy == 0) ? 0 : 1;
                            ret = stream_ctx->consumer_fn(quicrq_media_real_time_cache, stream_ctx->media_ctx, picoquic_get_quic_time(stream_ctx->cnx_ctx->qr_ctx->quic),
                                NULL, 0, 0, 0, 0, 0, 0, 0, 0);

                            ret = quicrq_cnx_handle_consumer_finished(stream_ctx, 0, 0, ret);
                        }
                    break;
                        break;
                    default:
                        /* Some unknown message, maybe not implemented yet */
                        ret = -1;
                        break;
                    }
                    /* As the message was processed, reset the message buffer. */
                    quicrq_msg_buffer_reset(&stream_ctx->message_receive);
                }
            }
        }
    }

    if (is_fin) {
        /* The peer is finished. */
        stream_ctx->is_peer_finished = 1;
        if (stream_ctx->is_local_finished) {
            quicrq_cnx_ctx_t* cnx_ctx = stream_ctx->cnx_ctx;

            if (stream_ctx->close_reason == quicrq_media_close_reason_unknown) {
                stream_ctx->close_reason = quicrq_media_close_remote_application;
            }
            quicrq_delete_stream_ctx(cnx_ctx, stream_ctx);
        }
        else {
            stream_ctx->send_state = quicrq_sending_fin;
            picoquic_mark_active_stream(stream_ctx->cnx_ctx->cnx, stream_ctx->stream_id, 1, stream_ctx);
        }
    }

    return ret;
}

quicrq_stream_ctx_t*  quicrq_get_control_stream_for_media_id(quicrq_cnx_ctx_t* cnx, uint64_t  media_id) {
    quicrq_stream_ctx_t* ctrl_stream_ctx = cnx->first_stream;
    while (ctrl_stream_ctx != NULL) {
        if (ctrl_stream_ctx->media_id == media_id) {
            return ctrl_stream_ctx;
        }
        ctrl_stream_ctx = ctrl_stream_ctx->next_stream;
    }
    return NULL;
}
/*
 * todo: add documentation
 */
int quicrq_receive_warp_or_rush_stream_data(quicrq_cnx_ctx_t* cnx_ctx, quicrq_uni_stream_ctx_t* uni_stream_ctx, uint8_t* bytes, size_t length, int is_fin) {
    // warp-header:grp-id [obj1 , obj2 ]
    int ret = 0;

    while (ret == 0 && length > 0) {
        /* Receive the next message on the stream, if any */
        /* TODO: add state for receiving data, take data out of header envelope */
        if (uni_stream_ctx->receive_state == quicrq_receive_object_data) {
            /* In data state, receive "object_length" bytes. Store them as 
             * fragment. If enough to complete the object, consider it finished,
             * cycle to state "quicrq_receive_object_header" so remaining bytes
             * can be processed.
             */
            quicrq_stream_ctx_t* ctrl_stream_ctx = uni_stream_ctx->control_stream_ctx;
            size_t copied = length;
            if (uni_stream_ctx->current_object_offset + copied > uni_stream_ctx->current_object_length) {
                copied = uni_stream_ctx->current_object_length - uni_stream_ctx->current_object_offset;
            }
            ret = ctrl_stream_ctx->consumer_fn(quicrq_media_datagram_ready, ctrl_stream_ctx->media_ctx, picoquic_get_quic_time(ctrl_stream_ctx->cnx_ctx->qr_ctx->quic),
                bytes, uni_stream_ctx->current_group_id, uni_stream_ctx->current_object_id,
                uni_stream_ctx->current_object_offset, 0, uni_stream_ctx->current_object_flags,
                uni_stream_ctx->nb_objects_previous_group,  uni_stream_ctx->current_object_length, copied);
            uni_stream_ctx->current_object_offset += copied;
            length -= copied;
            if (uni_stream_ctx->current_object_offset >= uni_stream_ctx->current_object_length) {
                uni_stream_ctx->receive_state = quicrq_receive_object_header;
                /* Increment predicted object ID to enable checks */
                uni_stream_ctx->current_object_id += 1;
#if 1
                uni_stream_ctx->current_object_offset = 0;
#endif
            }
            if (ret == quicrq_consumer_finished) {
                ret = quicrq_cnx_handle_consumer_finished(ctrl_stream_ctx, 0, 1, ret);
            }
        }
        else {
            int is_finished = 0;
            uint8_t* next_bytes = quicrq_msg_buffer_store(bytes, length, &uni_stream_ctx->message_buffer, &is_finished);
            if (next_bytes == NULL) {
                /* Something went wrong */
                ret = -1;
            }
            else
            {
                length = (bytes + length) - next_bytes;
                bytes = next_bytes;
                if (is_finished) {
                    /* Decode the incoming message */
                    quicrq_message_t incoming = { 0 };
                    const uint8_t* r_bytes = quicrq_msg_decode(uni_stream_ctx->message_buffer.buffer, uni_stream_ctx->message_buffer.buffer + uni_stream_ctx->message_buffer.message_size, &incoming);

                    if (r_bytes == NULL) {
                        /* Message was incorrect */
                        ret = -1;
                    }
                    else {
                        quicrq_log_message(cnx_ctx, "UniStream %" PRIu64 ", received message type=%" PRIu8,
                            uni_stream_ctx->stream_id, incoming.message_type);

                        switch (incoming.message_type) {
                        case QUICRQ_ACTION_WARP_HEADER:
                            if (uni_stream_ctx->receive_state != quicrq_receive_open) {
                                /* Protocol error */
                                ret = -1;
                            }
                            else {
                                uni_stream_ctx->current_group_id = incoming.group_id;
                                uni_stream_ctx->receive_state = quicrq_receive_warp_header;
                                quicrq_stream_ctx_t* ctrl_stream_ctx = quicrq_get_control_stream_for_media_id(cnx_ctx, incoming.media_id);
                                if (ctrl_stream_ctx == NULL) {
                                    /* Protocol error: unknown media ID. */
                                    quicrq_log_message(cnx_ctx, "UniStream %" PRIu64 ", unknown media id=%" PRIu64,
                                        uni_stream_ctx->stream_id, incoming.media_id);
                                    ret = -1;
                                }
                                else {
                                    uni_stream_ctx->receive_state = quicrq_receive_warp_header;
                                    if (uni_stream_ctx->control_stream_ctx == NULL) {
                                        quicrq_chain_uni_stream_to_control_stream(uni_stream_ctx, ctrl_stream_ctx);
                                    }
                                    quicrq_log_message(cnx_ctx, "UniStream %" PRIu64 ", received warp header message uni_id= %" PRIu64,
                                        ", ControlStream id= %" PRIu64 " media id=%" PRIu64,
                                        uni_stream_ctx->stream_id, uni_stream_ctx->control_stream_ctx->stream_id, incoming.media_id);
                                }
                            }
                            break;
                        case QUICRQ_ACTION_OBJECT_HEADER:
                            if (uni_stream_ctx->receive_state != quicrq_receive_warp_header &&
                                uni_stream_ctx->receive_state != quicrq_receive_object_header) {
                                /* Protocol error */
                                ret = -1;
                            }
                            else if (uni_stream_ctx->control_stream_ctx == NULL) {
                                /* Protocol error -- the control stream MUST be initialized in the authorized states */
                                ret = -1;
                            }
                            else if (
                                (uni_stream_ctx->control_stream_ctx->transport_mode == quicrq_transport_mode_rush &&
                                    uni_stream_ctx->current_object_id != 0) ||
                                (uni_stream_ctx->control_stream_ctx->transport_mode == quicrq_transport_mode_warp &&
                                uni_stream_ctx->current_object_id != incoming.object_id )) {
                                /* The peer is misbehaving */
                                DBG_PRINTF("Object not expected: %" PRIu64, incoming.object_id);
                                ret = -1;
                            }
                            else if (incoming.object_length > 0) {
                                uni_stream_ctx->receive_state = quicrq_receive_object_data;
                                uni_stream_ctx->current_object_id = incoming.object_id;
                                uni_stream_ctx->current_object_length = incoming.object_length;
                                uni_stream_ctx->current_object_flags = incoming.flags;
                                uni_stream_ctx->nb_objects_previous_group = incoming.nb_objects_previous_group;
                                uni_stream_ctx->current_object_offset = 0;
                            }
                            else {
                                /* Special case of zero length objects */
                                quicrq_stream_ctx_t* ctrl_stream_ctx = uni_stream_ctx->control_stream_ctx;

                                uni_stream_ctx->receive_state = quicrq_receive_object_header;
                                /* Pass the empty data to the media consumer. */
                                ret = ctrl_stream_ctx->consumer_fn(quicrq_media_datagram_ready, ctrl_stream_ctx->media_ctx, picoquic_get_quic_time(ctrl_stream_ctx->cnx_ctx->qr_ctx->quic),
                                    incoming.data, uni_stream_ctx->current_group_id, incoming.object_id,
                                    0, 0, incoming.flags, incoming.nb_objects_previous_group, 0, 0);
                                /* Increment predicted object ID to enable checks */
                                uni_stream_ctx->current_object_id += 1;
                                if (ret == quicrq_consumer_finished) {
                                    ret = quicrq_cnx_handle_consumer_finished(ctrl_stream_ctx, 0, 1, ret);
                                }
                            }
                            break;
                        default:
                            /* Some unknown message, maybe not implemented yet */
                            ret = -1;
                            break;
                        }
                    }
                    /* As the message was processed, reset the message buffer. */
                    quicrq_msg_buffer_reset(&uni_stream_ctx->message_buffer);
                }
            }
        }
    }

    if (is_fin) {
        /* TODO: What needs to be done here:
         * Update control stream context, this GOP is received. (Maybe not, the information is in the cache)
         * Delete the uni stream context.
         * Mark FIN when all GOP are received -- not so obvious in relay + error scenarios.
         */
        quicrq_delete_uni_stream_ctx(uni_stream_ctx->control_stream_ctx->cnx_ctx, uni_stream_ctx);
    }

    return ret;
}
/* Callback from Quic
 */
int quicrq_callback(picoquic_cnx_t* cnx,
    uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t fin_or_event, void* callback_ctx, void* v_stream_ctx)
{

    int ret = 0;
    quicrq_cnx_ctx_t* cnx_ctx = (quicrq_cnx_ctx_t*)callback_ctx;
    quicrq_stream_ctx_t* stream_ctx = NULL;
    quicrq_uni_stream_ctx_t* uni_stream_ctx = NULL;
    if((stream_id & 2) == 0) {
        stream_ctx = (quicrq_stream_ctx_t*)v_stream_ctx;
    } else {
        uni_stream_ctx = (quicrq_uni_stream_ctx_t*)v_stream_ctx;
    }


    /* If this is the first reference to the connection, the application context is set
     * to the default value defined for the server. This default value contains the pointer
     * to the global context in which streams and roles are defined.
     */
    if (callback_ctx == NULL || callback_ctx == picoquic_get_default_callback_context(picoquic_get_quic_ctx(cnx))) {
        if (fin_or_event == picoquic_callback_close) {
            picoquic_set_callback(cnx, NULL, NULL);
            return 0;
        }
        else {
            cnx_ctx = quicrq_create_cnx_context((quicrq_ctx_t*)callback_ctx, cnx);
            if (cnx_ctx == NULL) {
                /* cannot handle the connection */
                picoquic_close(cnx, PICOQUIC_ERROR_MEMORY);
                return -1;
            }
            else {
                /* Mark as a server connection, since this was automatically created */
                cnx_ctx->is_server = 1;
                picoquic_set_callback(cnx, quicrq_callback, cnx_ctx);
            }
        }
    }

    if (ret == 0) {
        switch (fin_or_event) {
        case picoquic_callback_stream_data:
        case picoquic_callback_stream_fin:
            /* Data arrival on stream #x, maybe with fin mark */

            if ((stream_id & 2) == 0) {
                if (stream_ctx == NULL) {
                    /* Retrieve, or create and initialize stream context for control channel */
                    stream_ctx = quicrq_find_or_create_stream(stream_id, cnx_ctx, 1);
                    if (stream_ctx == NULL) {
                        /* Internal error */
                        (void)picoquic_reset_stream(cnx, stream_id, QUICRQ_ERROR_INTERNAL);
                        return(-1);
                    }
                }

                /* we consider unidir streams as warp/rush streams. may be we need something more explicit */
                ret = quicrq_receive_stream_data(stream_ctx, bytes, length, (fin_or_event == picoquic_callback_stream_fin));
            }
            else {
                if (uni_stream_ctx == NULL) {
                    uni_stream_ctx = quicrq_find_or_create_uni_stream(stream_id, cnx_ctx, NULL, 1);
                    if (uni_stream_ctx == NULL) {
                        /* Internal error */
                        (void)picoquic_reset_stream(cnx, stream_id, QUICRQ_ERROR_INTERNAL);
                        return (-1);
                    }
                    (void)picoquic_set_app_stream_ctx(cnx, stream_id, uni_stream_ctx);
                }

                /* we consider unidirectional streams as warp/rush streams. maybe we need something more explicit */
                ret = quicrq_receive_warp_or_rush_stream_data(cnx_ctx, uni_stream_ctx, bytes, length,
                    (fin_or_event == picoquic_callback_stream_fin));
            }

            break;
        case picoquic_callback_prepare_to_send:
            /* Active sending API, called after wake up and picoquic conditions (congestion/rate control are ready to be sent */
            if ((stream_id & 2) == 0) {
                if (stream_ctx == NULL) {
                    /* This should never happen */
                    picoquic_log_app_message(cnx, "QUICRQ callback returns %d, event %d", ret, fin_or_event);
                    DBG_PRINTF("Prepare to send on NULL context, steam: %" PRIu64, stream_id);
                    ret = -1;
                }
                else {
                    ret = quicrq_prepare_to_send_on_stream(stream_ctx, bytes, length, picoquic_get_quic_time(cnx_ctx->qr_ctx->quic));
                }

            }  else {
                if (uni_stream_ctx == NULL) {
                    /* This should never happen */
                    picoquic_log_app_message(cnx, "QUICRQ callback returns %d, event %d", ret, fin_or_event);
                    DBG_PRINTF("Prepare to send on NULL Unistream context, steam: %" PRIu64, stream_id);
                    ret = -1;
                }
                else {
                    ret = quicrq_prepare_to_send_on_unistream(cnx_ctx, uni_stream_ctx, bytes, length, picoquic_get_quic_time(cnx_ctx->qr_ctx->quic));
                }
            }
            break;
        case picoquic_callback_datagram:
            /* Receive data in a datagram */
            ret = quicrq_receive_datagram(cnx_ctx, bytes, length, picoquic_get_quic_time(cnx_ctx->qr_ctx->quic));
            break;
        case picoquic_callback_prepare_datagram:
            /* Prepare to send a datagram */ {
            uint64_t current_time = picoquic_get_quic_time(cnx_ctx->qr_ctx->quic);

            ret = quicrq_prepare_to_send_datagram(cnx_ctx, bytes, length, current_time);
            break;
        }
        case picoquic_callback_stream_reset: /* Client reset stream #x */
        case picoquic_callback_stop_sending: /* Client asks server to reset stream #x */
            /* TODO: react to abandon stream, etc. */
            break;
        case picoquic_callback_stateless_reset: /* Received an error message */
        case picoquic_callback_close: /* Received connection close */
        case picoquic_callback_application_close: /* Received application close */
        {
            /* Document the reason code, so it could be passed to the media producers */
            quicrq_media_close_reason_enum close_reason = quicrq_media_close_quic_connection;
            uint64_t close_error_number = 0;
            if (fin_or_event == picoquic_callback_close) {
                if (cnx != NULL) {
                    close_error_number = picoquic_get_remote_error(cnx);
                }
            }
            else if (fin_or_event == picoquic_callback_application_close) {
                close_reason = quicrq_media_close_remote_application;
                if (cnx != NULL) {
                    close_error_number = picoquic_get_application_error(cnx);
                }
            }

            /* Remove the connection from the context, and then delete it */
            cnx_ctx->cnx = NULL;
            quicrq_delete_cnx_context(cnx_ctx, close_reason, close_error_number);
            picoquic_set_callback(cnx, NULL, NULL);
            break;
        }
        case picoquic_callback_version_negotiation:
            /* The server should never receive a version negotiation response */
            break;
        case picoquic_callback_stream_gap:
            /* This callback is never used. */
            break;
        case picoquic_callback_almost_ready:
        case picoquic_callback_ready:
            /* Check that the transport parameters are what the sample expects */
            break;
        case picoquic_callback_datagram_acked:
            /* Ack for packet carrying datagram-object received from peer */
        case picoquic_callback_datagram_lost:
            /* Packet carrying datagram-object probably lost */
        case picoquic_callback_datagram_spurious:
            /* Packet carrying datagram-object was not really lost */
            ret = quicrq_handle_datagram_ack_nack(cnx_ctx, fin_or_event, stream_id /* encodes the send time!*/,
                bytes, length, picoquic_get_quic_time(cnx_ctx->qr_ctx->quic));
            break;
        case picoquic_callback_pacing_changed:
            /* Notification of rate change from congestion controller */
            break;
        default:
            /* unexpected */
            break;
        }
    }

    if (ret != 0) {
        picoquic_log_app_message(cnx, "QUICRQ callback returns %d, event %d", ret, fin_or_event);
        DBG_PRINTF("QUICRQ callback returns %d, event %d", ret, fin_or_event);
    }

    return ret;
}

quicrq_stream_ctx_t* quicrq_cnx_subscribe_pattern(quicrq_cnx_ctx_t* cnx_ctx, const uint8_t* url, size_t url_length, quicrq_media_notify_fn media_notify_fn, void* notify_ctx)
{
    /* Create a stream for the subscribe pattern */
    uint64_t stream_id = picoquic_get_next_local_stream_id(cnx_ctx->cnx, 0);
    quicrq_stream_ctx_t* stream_ctx = quicrq_create_stream_context(cnx_ctx, stream_id);
    quicrq_message_buffer_t* message = &stream_ctx->message_sent;

    if (stream_ctx != NULL) {
        if (quicrq_msg_buffer_alloc(message, quicrq_subscribe_msg_reserve(url_length), 0) == 0) {
            /* Format the media request */
            uint8_t* message_next = quicrq_subscribe_msg_encode(message->buffer, message->buffer + message->buffer_alloc,
                QUICRQ_ACTION_SUBSCRIBE, url_length, url);
            if (message_next == NULL) {
                cnx_ctx->first_stream->close_reason = quicrq_media_close_internal_error;
                quicrq_delete_stream_ctx(cnx_ctx, stream_ctx);
                stream_ctx = NULL;
            }
            else {
                char buffer[256];
                /* Set the call back functions */
                stream_ctx->media_notify_fn = media_notify_fn;
                stream_ctx->notify_ctx = notify_ctx;
                /* Queue the media request message to that stream */
                message->message_size = message_next - message->buffer;
                stream_ctx->send_state = quicrq_sending_subscribe;
                stream_ctx->receive_state = quicrq_receive_notify;

                picoquic_mark_active_stream(cnx_ctx->cnx, stream_id, 1, stream_ctx);
                quicrq_log_message(cnx_ctx, "Posting subscribe to URL pattern: %s* on stream %" PRIu64,
                    quicrq_uint8_t_to_text(url, url_length, buffer, 256), stream_ctx->stream_id);
            }
        }
    }
    return stream_ctx;
}

int quicrq_cnx_subscribe_pattern_close(quicrq_cnx_ctx_t* cnx_ctx, quicrq_stream_ctx_t* stream_ctx)
{
    int ret = 0;
    if (stream_ctx->send_state == quicrq_waiting_notify) {
        stream_ctx->send_state = quicrq_sending_fin;

        picoquic_mark_active_stream(cnx_ctx->cnx, stream_ctx->stream_id, 1, stream_ctx);
    }
    else if (stream_ctx->send_state != quicrq_sending_fin) {
        ret = 1;
    }
    return ret;
}

void quicrq_init_transport_parameters(picoquic_tp_t* tp, int client_mode)
{
    memset(tp, 0, sizeof(picoquic_tp_t));
    tp->initial_max_stream_data_bidi_local = 0x200000;
    tp->initial_max_stream_data_bidi_remote = 65635;
    tp->initial_max_stream_data_uni = 65535;
    tp->initial_max_data = 0x100000;
    if (client_mode) {
        tp->initial_max_stream_id_bidir = 2049;
        tp->initial_max_stream_id_unidir = 2051;
    }
    else {
        tp->initial_max_stream_id_bidir = 2048;
        tp->initial_max_stream_id_unidir = 2050;
    }
    tp->idle_timeout = 30000;
    tp->max_packet_size = PICOQUIC_MAX_PACKET_SIZE;
    tp->ack_delay_exponent = 3;
    tp->active_connection_id_limit = 4;
    tp->max_ack_delay = 10000ull;
    tp->enable_loss_bit = 2;
    tp->min_ack_delay = 1000ull;
    tp->enable_time_stamp = 0;
    tp->max_datagram_frame_size = PICOQUIC_MAX_PACKET_SIZE;
}

void quicrq_set_cache_duration(quicrq_ctx_t* qr_ctx, uint64_t cache_duration_max)
{
    qr_ctx->cache_duration_max = cache_duration_max;
}

uint64_t quicrq_time_check(quicrq_ctx_t* qr_ctx, uint64_t current_time)
{
    uint64_t next_time = UINT64_MAX;
    uint64_t extra_repeat_time = quicrq_handle_extra_repeat(qr_ctx, current_time);
    uint64_t quic_time = picoquic_get_next_wake_time(qr_ctx->quic, current_time);

    if (extra_repeat_time < quic_time) {
        quic_time = extra_repeat_time;
    }
    if (quic_time < next_time) {
        next_time = quic_time;
    }

    if (qr_ctx->manage_relay_cache_fn != NULL) {
        int should_manage = qr_ctx->is_cache_closing_needed;
        if (qr_ctx->cache_duration_max > 0) {
            if (current_time >= qr_ctx->cache_check_next_time) {
                should_manage = 1;
                qr_ctx->cache_check_next_time = current_time + qr_ctx->cache_duration_max / 2;
            }
            if (qr_ctx->cache_check_next_time < next_time) {
                next_time = qr_ctx->cache_check_next_time;
            }
        }
        if (should_manage) {
            uint64_t manage_time = qr_ctx->manage_relay_cache_fn(qr_ctx, current_time);
            if (manage_time < next_time) {
                next_time = manage_time;
            }
        }
    }

    return next_time;
}

/* get the quic context from quicqr context */
picoquic_quic_t* quicrq_get_quic_ctx(quicrq_ctx_t* qr_ctx)
{
    return (qr_ctx==NULL)?NULL:qr_ctx->quic;
}

/* Delete a QUICR configuration */
void quicrq_delete(quicrq_ctx_t* qr_ctx)
{
    struct st_quicrq_cnx_ctx_t* cnx_ctx = qr_ctx->first_cnx;
    struct st_quicrq_cnx_ctx_t* next = NULL;
    struct st_quicrq_media_source_ctx_t* srce_ctx = NULL;
    struct st_quicrq_media_source_ctx_t* srce_next = NULL;
    struct st_quicrq_media_object_source_ctx_t* object_source_ctx = qr_ctx->first_object_source;
    struct st_quicrq_media_object_source_ctx_t* object_source_next = NULL;

    while (cnx_ctx != NULL) {
        next = cnx_ctx->next_cnx;
        quicrq_delete_cnx_context(cnx_ctx, quicrq_media_close_delete_context, 0);
        cnx_ctx = next;
    }

    /* Media object sources are deleted first, because this will
     * trigger closure of old-style media sources.
     */

    while (object_source_ctx != NULL) {
        object_source_next = object_source_ctx->next_in_qr_ctx;
        quicrq_delete_object_source(object_source_ctx);
        object_source_ctx = object_source_next;
    }
    srce_ctx = qr_ctx->first_source;
    while (srce_ctx != NULL) {
        srce_next = srce_ctx->next_source;
        quicrq_delete_source(srce_ctx, qr_ctx);
        srce_ctx = srce_next;
    }

    if (qr_ctx->quic != NULL) {
        picoquic_free(qr_ctx->quic);
    }

    quicrq_disable_relay(qr_ctx);

    free(qr_ctx);
}

/* Create a QUICRQ context
 * TODO: consider passing a picoquic configuration object 
 */
quicrq_ctx_t* quicrq_create_empty()
{
    quicrq_ctx_t* qr_ctx = (quicrq_ctx_t*)malloc(sizeof(quicrq_ctx_t));

    if (qr_ctx != NULL) {
        memset(qr_ctx, 0, sizeof(quicrq_ctx_t));
    }
    return qr_ctx;
}

void quicrq_set_quic(quicrq_ctx_t* qr_ctx, picoquic_quic_t* quic)
{
    qr_ctx->quic = quic;
}

quicrq_ctx_t* quicrq_create(char const* alpn,
    char const* cert_file_name, char const* key_file_name, char const* cert_root_file_name,
    char const* ticket_store_file_name, char const* token_store_file_name,
    const uint8_t* ticket_encryption_key, size_t ticket_encryption_key_length,
    uint64_t* p_simulated_time)
{
    quicrq_ctx_t* qr_ctx = quicrq_create_empty();
    uint64_t current_time = (p_simulated_time == NULL) ? picoquic_current_time() : *p_simulated_time;

    if (qr_ctx != NULL) {
        qr_ctx->quic = picoquic_create(QUICRQ_MAX_CONNECTIONS, cert_file_name, key_file_name, cert_root_file_name, alpn,
            quicrq_callback, qr_ctx, NULL, NULL, NULL, current_time, p_simulated_time,
            ticket_store_file_name, ticket_encryption_key, ticket_encryption_key_length);

        if (qr_ctx->quic == NULL ||
            (token_store_file_name != NULL && picoquic_load_retry_tokens(qr_ctx->quic, token_store_file_name) != 0)) {
            quicrq_delete(qr_ctx);
            qr_ctx = NULL;
        }
        else {
            picoquic_set_default_congestion_algorithm(qr_ctx->quic, picoquic_bbr_algorithm);
            picoquic_set_default_priority(qr_ctx->quic, 4);
            qr_ctx->cache_duration_max = QUICRQ_CACHE_DURATION_DEFAULT;
        }
    }
    return qr_ctx;
}

/* Delete a connection context */
void quicrq_delete_cnx_context(quicrq_cnx_ctx_t* cnx_ctx, quicrq_media_close_reason_enum close_reason, uint64_t close_error_code)
{
    /* Delete the stream contexts */
    while (cnx_ctx->first_stream != NULL) {
        if (cnx_ctx->first_stream->close_reason == quicrq_media_close_reason_unknown) {
            cnx_ctx->first_stream->close_reason = close_reason;
            cnx_ctx->first_stream->close_error_code = close_error_code;
        }
        quicrq_delete_stream_ctx(cnx_ctx, cnx_ctx->first_stream);
    }

    /* Delete any uni stream context that was not chained to a stream context */
    while (cnx_ctx->first_uni_stream != NULL) {
        quicrq_delete_uni_stream_ctx(cnx_ctx, cnx_ctx->first_uni_stream);
    }

    /* Delete the quic connection */
    if (cnx_ctx->cnx != NULL) {
        picoquic_set_callback(cnx_ctx->cnx, NULL, NULL);
        picoquic_delete_cnx(cnx_ctx->cnx);
        cnx_ctx->cnx = NULL;
    }
    /* Remove the connection from the double linked list */
    if (cnx_ctx->qr_ctx != NULL) {
        if (cnx_ctx->next_cnx == NULL) {
            cnx_ctx->qr_ctx->last_cnx = cnx_ctx->previous_cnx;
        }
        else {
            cnx_ctx->next_cnx->previous_cnx = cnx_ctx->previous_cnx;
        }
        if (cnx_ctx->previous_cnx == NULL) {
            cnx_ctx->qr_ctx->first_cnx = cnx_ctx->next_cnx;
        }
        else {
            cnx_ctx->previous_cnx->next_cnx = cnx_ctx->next_cnx;
        }
    }
    /* Free the context */
    free(cnx_ctx);
}

/* Create a connection context. 
 * The QUIC connection has to be created before the QUICRQ connection. */
quicrq_cnx_ctx_t* quicrq_create_cnx_context(quicrq_ctx_t* qr_ctx, picoquic_cnx_t * cnx)
{
    quicrq_cnx_ctx_t* cnx_ctx = (quicrq_cnx_ctx_t*)malloc(sizeof(quicrq_cnx_ctx_t));

    if (cnx_ctx != NULL) {
        memset(cnx_ctx, 0, sizeof(quicrq_cnx_ctx_t));
        /* document quic connection */
        cnx_ctx->cnx = cnx;
        /* Add the connection in the double linked list */
        if (qr_ctx->last_cnx == NULL) {
            qr_ctx->first_cnx = cnx_ctx;
        }
        else {
            qr_ctx->last_cnx->next_cnx = cnx_ctx;
        }
        cnx_ctx->previous_cnx = qr_ctx->last_cnx;
        qr_ctx->last_cnx = cnx_ctx;
        cnx_ctx->qr_ctx = qr_ctx;
        picoquic_set_callback(cnx, quicrq_callback, cnx_ctx);
    }
    return cnx_ctx;
}

/* Create a client connection.
 */
quicrq_cnx_ctx_t* quicrq_create_client_cnx(quicrq_ctx_t* qr_ctx,
    const char* sni, struct sockaddr* addr)
{
    quicrq_cnx_ctx_t* cnx_ctx = NULL;
    picoquic_tp_t client_parameters;
    picoquic_cnx_t * cnx = picoquic_create_cnx(qr_ctx->quic,
        picoquic_null_connection_id, picoquic_null_connection_id,
        addr, picoquic_get_quic_time(qr_ctx->quic), 0, sni, QUICRQ_ALPN, 1);
    /* Set parameters */
    if (cnx != NULL) {
        quicrq_init_transport_parameters(&client_parameters, 1);
        picoquic_set_transport_parameters(cnx, &client_parameters);
        /* Enable keep alive with period= 10 second to avoid closing connections. */
        picoquic_enable_keep_alive(cnx, 10000000);

        if (picoquic_start_client_cnx(cnx) != 0) {
            picoquic_delete_cnx(cnx);
            cnx = NULL;
        }
        if (cnx != NULL) {
            cnx_ctx = quicrq_create_cnx_context(qr_ctx, cnx);
            cnx_ctx->is_client = 1;
            if (cnx_ctx == NULL) {
                picoquic_delete_cnx(cnx);
            }
        }
    }
    return cnx_ctx;
}

/* Access the server address behind a quicrq connection context
 */
void quicrq_get_peer_address(quicrq_cnx_ctx_t* cnx_ctx, struct sockaddr_storage* stored_addr)
{
    struct sockaddr* peer_addr;

    picoquic_get_peer_addr(cnx_ctx->cnx, &peer_addr);
    picoquic_store_addr(stored_addr, peer_addr);
}

quicrq_cnx_ctx_t* quicrq_first_connection(quicrq_ctx_t* qr_ctx)
{
    return qr_ctx->first_cnx;
}

void quicrq_delete_uni_stream_ctx(quicrq_cnx_ctx_t* cnx_ctx, quicrq_uni_stream_ctx_t* uni_stream_ctx)
{
    quicrq_stream_ctx_t* ctrl_stream = uni_stream_ctx->control_stream_ctx;

    /* update chain in connection context */
    if (uni_stream_ctx->next_uni_stream_for_cnx == NULL) {
        cnx_ctx->last_uni_stream = uni_stream_ctx->previous_uni_stream_for_cnx;
    }
    else {
        uni_stream_ctx->next_uni_stream_for_cnx->previous_uni_stream_for_cnx = uni_stream_ctx->previous_uni_stream_for_cnx;
    }
    if (uni_stream_ctx->previous_uni_stream_for_cnx == NULL) {
        cnx_ctx->first_uni_stream = uni_stream_ctx->next_uni_stream_for_cnx;
    }
    else {
        uni_stream_ctx->previous_uni_stream_for_cnx->next_uni_stream_for_cnx = uni_stream_ctx->next_uni_stream_for_cnx;
    }
    /* Update chain in control stream context */
    if (ctrl_stream != NULL) {
        if (uni_stream_ctx->next_uni_stream_for_control_stream == NULL) {
            ctrl_stream->last_uni_stream = uni_stream_ctx->previous_uni_stream_for_control_stream;
        }
        else {
            uni_stream_ctx->next_uni_stream_for_control_stream->previous_uni_stream_for_control_stream = 
                uni_stream_ctx->previous_uni_stream_for_control_stream;
        }
        if (uni_stream_ctx->previous_uni_stream_for_control_stream == NULL) {
            ctrl_stream->first_uni_stream = uni_stream_ctx->next_uni_stream_for_control_stream;
        }
        else {
            uni_stream_ctx->previous_uni_stream_for_control_stream->next_uni_stream_for_control_stream = 
                uni_stream_ctx->next_uni_stream_for_control_stream;
        }
        uni_stream_ctx->control_stream_ctx = NULL;
    }
    /* Unlink the unistream context from the picoquic stream context */
    if (cnx_ctx->cnx != NULL) {
        if (ctrl_stream != NULL && ctrl_stream->is_sender) {
            (void)picoquic_mark_active_stream(cnx_ctx->cnx, uni_stream_ctx->stream_id, 0, NULL);
            if (uni_stream_ctx->send_state < quicrq_sending_warp_should_close) {
                /* This is an error case: reset the stream before closing it will force an abandon. */
                (void)picoquic_reset_stream(cnx_ctx->cnx, uni_stream_ctx->stream_id, 0);
            }
        }
        else {
            picoquic_unlink_app_stream_ctx(cnx_ctx->cnx, uni_stream_ctx->stream_id);
        }
    }
    /* Release memory*/
    quicrq_msg_buffer_release(&uni_stream_ctx->message_buffer);
    free(uni_stream_ctx);
}

void quicrq_delete_stream_ctx(quicrq_cnx_ctx_t* cnx_ctx, quicrq_stream_ctx_t* stream_ctx)
{
    quicrq_datagram_ack_ctx_release(stream_ctx);

    while (stream_ctx->first_notify_url != NULL) {
        quicrq_notify_url_t* next = stream_ctx->first_notify_url->next_notify_url;
        free(stream_ctx->first_notify_url);
        stream_ctx->first_notify_url = next;
    }

    if (stream_ctx->subscribe_prefix != NULL) {
        free(stream_ctx->subscribe_prefix);
        stream_ctx->subscribe_prefix = NULL;
    }
    /* Delete the uni streams controlled by this context */
    while (stream_ctx->first_uni_stream != NULL) {
        quicrq_delete_uni_stream_ctx(stream_ctx->cnx_ctx, stream_ctx->first_uni_stream);
    }

    /* Remove stream context from connection context */
    if (stream_ctx->next_stream == NULL) {
        cnx_ctx->last_stream = stream_ctx->previous_stream;
    }
    else {
        stream_ctx->next_stream->previous_stream = stream_ctx->previous_stream;
    }
    if (stream_ctx->previous_stream == NULL) {
        cnx_ctx->first_stream = stream_ctx->next_stream;
    }
    else {
        stream_ctx->previous_stream->next_stream = stream_ctx->next_stream;
    }

    quicrq_unsubscribe_local_media(stream_ctx);

    if (cnx_ctx->cnx != NULL) {
        (void)picoquic_mark_active_stream(cnx_ctx->cnx, stream_ctx->stream_id, 0, NULL);
        (void)picoquic_add_to_stream(cnx_ctx->cnx, stream_ctx->stream_id, NULL, 0, 1);
    }
    if (stream_ctx->media_ctx != NULL) {
        uint64_t current_time = picoquic_get_quic_time(cnx_ctx->qr_ctx->quic);
        if (stream_ctx->is_sender) {
            (void)quicrq_fragment_publisher_fn(quicrq_media_source_close, stream_ctx->media_ctx, NULL, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL, current_time);
        }
        else {
            if (stream_ctx->consumer_fn != NULL) {
                stream_ctx->consumer_fn(quicrq_media_close, stream_ctx->media_ctx, current_time, NULL, 0, 0, 0, 0, 0, 0, stream_ctx->close_reason, stream_ctx->close_error_code);
            }
        }
    }

    quicrq_msg_buffer_release(&stream_ctx->message_receive);
    quicrq_msg_buffer_release(&stream_ctx->message_sent);

    free(stream_ctx);
}

quicrq_stream_ctx_t* quicrq_create_stream_context(quicrq_cnx_ctx_t* cnx_ctx, uint64_t stream_id)
{
    quicrq_stream_ctx_t* stream_ctx = (quicrq_stream_ctx_t*)malloc(sizeof(quicrq_stream_ctx_t));
    if (stream_ctx != NULL) {
        memset(stream_ctx, 0, sizeof(quicrq_stream_ctx_t));
        stream_ctx->cnx_ctx = cnx_ctx;
        stream_ctx->stream_id = stream_id;
        if (cnx_ctx->last_stream == NULL) {
            cnx_ctx->first_stream = stream_ctx;
        }
        else {
            cnx_ctx->last_stream->next_stream = stream_ctx;
        }
        stream_ctx->previous_stream = cnx_ctx->last_stream;
        cnx_ctx->last_stream = stream_ctx;
        quicrq_datagram_ack_ctx_init(stream_ctx);
    }

    return stream_ctx;
}

void quicrq_chain_uni_stream_to_control_stream(quicrq_uni_stream_ctx_t* uni_stream_ctx, quicrq_stream_ctx_t* stream_ctx)
{
    uni_stream_ctx->control_stream_ctx = stream_ctx;
    if (stream_ctx->first_uni_stream == NULL) {
        stream_ctx->first_uni_stream = uni_stream_ctx;
    }
    else {
        stream_ctx->last_uni_stream->next_uni_stream_for_control_stream = uni_stream_ctx;
    }
    uni_stream_ctx->previous_uni_stream_for_control_stream = stream_ctx->last_uni_stream;
    stream_ctx->last_uni_stream = uni_stream_ctx; 
}

quicrq_uni_stream_ctx_t* quicrq_create_uni_stream_context(
    quicrq_cnx_ctx_t* cnx_ctx, quicrq_stream_ctx_t * stream_ctx, uint64_t stream_id)
{
    quicrq_uni_stream_ctx_t* uni_stream_ctx = (quicrq_uni_stream_ctx_t*)malloc(sizeof(quicrq_uni_stream_ctx_t));
    if (uni_stream_ctx != NULL) {
        /* Chain to connection */
        memset(uni_stream_ctx, 0, sizeof(quicrq_uni_stream_ctx_t));
        uni_stream_ctx->stream_id = stream_id;
        if (cnx_ctx->last_uni_stream == NULL) {
            cnx_ctx->first_uni_stream = uni_stream_ctx;
        }
        else {
            cnx_ctx->last_uni_stream->next_uni_stream_for_cnx = uni_stream_ctx;
        }
        uni_stream_ctx->previous_uni_stream_for_cnx = cnx_ctx->last_uni_stream;
        cnx_ctx->last_uni_stream = uni_stream_ctx;
        /* Chain to control stream */
        if (stream_ctx != NULL) {
            quicrq_chain_uni_stream_to_control_stream(uni_stream_ctx, stream_ctx);
        }
    }
    return uni_stream_ctx;
}

quicrq_stream_ctx_t* quicrq_find_or_create_stream(
    uint64_t stream_id,
    quicrq_cnx_ctx_t* cnx_ctx,
    int should_create)
{
    quicrq_stream_ctx_t* stream_ctx = cnx_ctx->first_stream;

    while (stream_ctx != NULL) {
        if (stream_ctx->stream_id == stream_id) {
            break;
        }
        stream_ctx = stream_ctx->next_stream;
    }
    if (stream_ctx == NULL && should_create) {
        stream_ctx = quicrq_create_stream_context(cnx_ctx, stream_id);
    }

    return stream_ctx;
}

quicrq_uni_stream_ctx_t* quicrq_find_or_create_uni_stream(
    uint64_t stream_id,
    quicrq_cnx_ctx_t* cnx_ctx,
    quicrq_stream_ctx_t* stream_ctx,
    int should_create)
{
    quicrq_uni_stream_ctx_t* uni_stream_ctx = cnx_ctx->first_uni_stream;

    while (uni_stream_ctx != NULL) {
        if (uni_stream_ctx->stream_id == stream_id) {
            break;
        }
        uni_stream_ctx = uni_stream_ctx->next_uni_stream_for_cnx;
    }

    if (uni_stream_ctx == NULL && should_create) {
        uni_stream_ctx = quicrq_create_uni_stream_context(cnx_ctx, stream_ctx, stream_id);
    }

    return uni_stream_ctx;
}

int quicrq_cnx_has_stream(quicrq_cnx_ctx_t* cnx_ctx)
{
    return (cnx_ctx->first_stream != NULL);
}

int quicrq_close_cnx(quicrq_cnx_ctx_t* cnx_ctx)
{
    int ret = 0;

    if (cnx_ctx->cnx != NULL && picoquic_get_cnx_state(cnx_ctx->cnx) < picoquic_state_disconnecting) {
        ret = picoquic_close(cnx_ctx->cnx, 0);
    }

    return ret;
}

int quicrq_is_cnx_disconnected(quicrq_cnx_ctx_t* cnx_ctx)
{
    return  (cnx_ctx->cnx == NULL || picoquic_get_cnx_state(cnx_ctx->cnx) == picoquic_state_disconnected);
}


/* Media publisher API.
 * Simplified API for now:
 * - cnx_ctx: context of the QUICR connection
 * - media_url: URL of the media fragment
 * - media_publisher_fn: callback function for processing media arrival
 * - media_ctx: media context managed by the publisher
 */


/* Utility function, encode or decode a object header.
 */
const uint8_t* quicr_decode_object_header(const uint8_t* fh, const uint8_t* fh_max, quicrq_media_object_header_t* hdr)
{
    /* decode the object header */
    if ((fh = picoquic_frames_uint64_decode(fh, fh_max, &hdr->timestamp)) != NULL &&
        (fh = picoquic_frames_uint64_decode(fh, fh_max, &hdr->number)) != NULL){
        uint32_t length = 0;
        fh = picoquic_frames_uint32_decode(fh, fh_max, &length);
        hdr->length = length;
    }
    return fh;
}

uint8_t* quicr_encode_object_header(uint8_t* fh, const uint8_t* fh_max, const quicrq_media_object_header_t* hdr)
{
    /* decode the object header */
    if ((fh = picoquic_frames_uint64_encode(fh, fh_max, hdr->timestamp)) != NULL &&
        (fh = picoquic_frames_uint64_encode(fh, fh_max, hdr->number)) != NULL) {
        fh = picoquic_frames_uint32_encode(fh, fh_max, (uint32_t)hdr->length);
    }

    return fh;
}

/* Utility function, write an URL as a string. */
const char* quicrq_uint8_t_to_text(const uint8_t* u, size_t length, char* buffer, size_t buffer_length)
{
    if (buffer_length < 16) {
        return "???";
    }
    else {
        size_t available = buffer_length - 8;
        size_t i = 0;
        size_t l = 0;
        for (; l < available && i < length; i++) {
            int c = u[i];
            if (c == '\\') {
                buffer[l++] = '\\';
                buffer[l++] = '\\';
            }
            else if (c >= 32 && c <= 126 && c != '\\') {
                buffer[l++] = (char)c;
            }
            else {
                int d;

                buffer[l++] = '\\';
                d = c / 100;
                buffer[l++] = '0' + d;
                c -= 100 * d;
                d = c / 10;
                buffer[l++] = '0' + d;
                c -= 10 * d;
                buffer[l++] = '0' + c;
            }
        }
        if (i < length) {
            available = buffer_length - 1;
            for (int j = 0; j < 3 && l < available; j++) {
                buffer[l++] = '.';
            }
        }
        buffer[l++] = 0;
        return buffer;
    }
}

/* Logging function
 */
void quicrq_log_message(quicrq_cnx_ctx_t* cnx_ctx, const char* fmt, ...)
{
    if (cnx_ctx != NULL && cnx_ctx->cnx != NULL){
        va_list args;
        va_start(args, fmt);
        picoquic_log_app_message_v(cnx_ctx->cnx, fmt, args);
        va_end(args);
    }
}

/* Utility function to get names or letters for transport mode */
char quicrq_transport_mode_to_letter(quicrq_transport_mode_enum transport_mode)
{
    char ret = 'u';

    switch (transport_mode) {
    case quicrq_transport_mode_single_stream:
        ret = 's';
        break;
    case quicrq_transport_mode_warp:
        ret = 'w'; /* w for warp */
        break;
    case quicrq_transport_mode_rush:
        ret = 'r'; /* r for rush */
        break;
    case quicrq_transport_mode_datagram:
        ret = 'd';
        break;
    default:
        break;
    }
    return ret;
}

const char * quicrq_transport_mode_to_string(quicrq_transport_mode_enum transport_mode)
{
    const char * ret = "undefined";

    switch (transport_mode) {
    case quicrq_transport_mode_single_stream:
        ret = "stream";
        break;
    case quicrq_transport_mode_warp:
        ret = "warp";
        break;
    case quicrq_transport_mode_rush:
        ret = "rush";
        break;
    case quicrq_transport_mode_datagram:
        ret = "datagram";
        break;
    default:
        break;
    }
    return ret;
}
