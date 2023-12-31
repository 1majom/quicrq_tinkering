
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "quicrq.h"
#include "quicrq_reassembly.h"
#include "quicrq_internal.h"
#include "quicrq_tests.h"
#include "quicrq_test_internal.h"
#include "picoquic_utils.h"
#include "picosplay.h"
/* Unit test of test_media and media api
 */


/* In generation mode, data is created during the test. 
 * In regular mode, data is read from a file.
 * We need to simulate two modes of reading the data, either "streaming"
 * in which the data is sent as fast as the path permits, or "real time"
 * in which the data is only sent if the current time is larger than
 * the creation time.
 */

/* Definition of test publisher. 
 */

void test_media_publisher_close(void* media_ctx)
{
    test_media_publisher_context_t* pub_ctx = (test_media_publisher_context_t*)media_ctx;
    /* Close source file */
    if (pub_ctx->F != NULL) {
        picoquic_file_close(pub_ctx->F);
    }
    if (pub_ctx->generation_context != NULL) {
        free(pub_ctx->generation_context);
    }
    if (pub_ctx->media_object != NULL) {
        free(pub_ctx->media_object);
    }
    free(pub_ctx);
}

void* test_media_publisher_init(char const* media_source_path, const generation_parameters_t * generation_model,
    int is_real_time, uint64_t start_time)
{
    test_media_publisher_context_t* media_ctx = (test_media_publisher_context_t*)
        malloc(sizeof(test_media_publisher_context_t));
    if (media_ctx != NULL) {
        memset(media_ctx, 0, sizeof(test_media_publisher_context_t));
        media_ctx->start_time = start_time;
        media_ctx->is_real_time = (is_real_time != 0);
        media_ctx->F = picoquic_file_open(media_source_path, "rb");
        media_ctx->is_audio = test_media_is_audio((const uint8_t*)media_source_path, strlen(media_source_path));

        if (media_ctx->F == NULL) {
            if (generation_model != NULL) {
                media_ctx->generation_context = (generation_parameters_t*)malloc(sizeof(generation_parameters_t));
                if (media_ctx->generation_context != NULL) {
                    memcpy(media_ctx->generation_context, generation_model, sizeof(generation_parameters_t));
                }
            }
            if (media_ctx->generation_context == NULL) {
                free(media_ctx);
                media_ctx = NULL;
            }
        }
    }
    return media_ctx;
}

void* test_media_publisher_subscribe(void* v_srce_ctx, quicrq_stream_ctx_t* stream_ctx)
{
    test_media_source_context_t* srce_ctx = (test_media_source_context_t*)v_srce_ctx;
    test_media_publisher_context_t* media_ctx = 
        test_media_publisher_init(srce_ctx->file_path, srce_ctx->generation_context, srce_ctx->is_real_time, 
            srce_ctx->start_time);
#ifdef _WINDOWS
    UNREFERENCED_PARAMETER(stream_ctx);
#endif

    if (media_ctx != NULL) {
        media_ctx->is_audio = test_media_is_audio((const uint8_t*)srce_ctx->file_path, strlen(srce_ctx->file_path));
        media_ctx->p_next_time = srce_ctx->p_next_time;
        media_ctx->min_packet_size = srce_ctx->min_packet_size;
        if (stream_ctx != NULL) {
            /* Just to appease the Wpedantic warnings. */
            media_ctx->stream_id = stream_ctx->stream_id;
        }
    }

    return media_ctx;
}


/* Media publisher callback for stream mode.
 * In stream mode, the object data is directly copied to the output.
 */

int test_media_allocate_object(test_media_publisher_context_t* pub_ctx, size_t target_size)
{
    int ret = 0;

    if (pub_ctx->media_object_size > target_size) {
        ret = -1;
    } else if (pub_ctx->media_object_alloc < target_size) {
        uint8_t* new_memory = (uint8_t*)malloc(target_size);
        if (new_memory == NULL){
            ret = -1;
        }
        else {
            if (pub_ctx->media_object_size > 0 && pub_ctx->media_object_size <= target_size) {
                memcpy(new_memory, pub_ctx->media_object, pub_ctx->media_object_size);
            }
            if (pub_ctx->media_object_alloc > 0) {
                free(pub_ctx->media_object);
            }
            pub_ctx->media_object_alloc = target_size;
            pub_ctx->media_object = new_memory;
        }
    }
    return ret;
}

int test_media_read_object_from_file(test_media_publisher_context_t* pub_ctx)
{
    /* If there is no memory, allocate default size. */
    size_t nb_read;
    pub_ctx->media_object_size = 0;
    int ret = test_media_allocate_object(pub_ctx, QUIRRQ_MEDIA_TEST_HEADER_SIZE);
    if (ret == 0) {
        /* Read the object header */
        nb_read = fread(pub_ctx->media_object, 1, QUIRRQ_MEDIA_TEST_HEADER_SIZE, pub_ctx->F);
        if (nb_read != QUIRRQ_MEDIA_TEST_HEADER_SIZE) {
            /* Assume this is the end of file. */
            pub_ctx->is_finished = 1;
        }
        else {
            /* decode the object header */
            const uint8_t* fh_max = pub_ctx->media_object + QUIRRQ_MEDIA_TEST_HEADER_SIZE;
            const uint8_t* fh = quicr_decode_object_header(pub_ctx->media_object, fh_max, &pub_ctx->current_header);

            if (fh != NULL){
                /* If there is not enough memory, allocate data for a full object */
                size_t target_size = (fh - pub_ctx->media_object) + pub_ctx->current_header.length;
                pub_ctx->media_object_size = QUIRRQ_MEDIA_TEST_HEADER_SIZE;
                ret = test_media_allocate_object(pub_ctx, target_size);
                if (ret == 0) {
                    if (pub_ctx->current_header.length > 0) {
                        /* Read the object content */
                        size_t required = target_size - pub_ctx->media_object_size;
                        nb_read = fread(pub_ctx->media_object + pub_ctx->media_object_size, 1, required, pub_ctx->F);
                        if (nb_read != required) {
                            ret = -1;
                            DBG_PRINTF("Reading %zu object bytes, required %zu, ret=%d", nb_read, required, ret);
                        }
                        else {
                            pub_ctx->media_object_size = target_size;
                        }
                    }
                }
            }
            else {
                /* malformed header ! */
                ret = -1;
                DBG_PRINTF("Reading malformed object header, ret=%d", ret);
            }       
        }
    }
    return ret;
}

size_t test_media_generate_object_size(generation_parameters_t * gen_ctx)
{
    size_t l = 0;
    size_t size_min = gen_ctx->target_p_min;
    size_t size_max = gen_ctx->target_p_max;
    size_t delta = 0;
    size_t multiply = 1;
    size_t reminder = 0;
    size_t r_delta = 0;
    /* Is this an I object? If yes, size_min and size_max are bigger */
    if (gen_ctx->objects_in_epoch > 0 && (gen_ctx->nb_objects_sent% gen_ctx->objects_in_epoch) == 0) {
        size_min *= gen_ctx->nb_p_in_i;
        size_max *= gen_ctx->nb_p_in_i;
    }
    /* Do a random allocation */
    l = size_min;
    delta = size_max - size_min;
    if (delta > 0) {
        if (delta > RAND_MAX) {
            multiply = (delta + RAND_MAX - 1) / RAND_MAX;
        }
        reminder = (multiply * RAND_MAX) % delta;
        while (r_delta < reminder) {
            r_delta = multiply * rand();
        }
        l += (r_delta - reminder) % delta;
    }
    return l;
}

int test_media_generate_object(test_media_publisher_context_t* pub_ctx)
{
    int ret = 0;

    /* Compute the time stamp.
        * At this point, we have a conflict between the way time in microseconds
        * per QUIC API, and specifications like "30 fps" that do not result in
        * an integer number of microseconds. We deal with that by computing a
        * virtual time as "number of elapsed objects * 1,000,000 / fps ".
        * 
        */
    pub_ctx->current_header.number = pub_ctx->generation_context->nb_objects_sent;
    pub_ctx->current_header.timestamp = 
        (pub_ctx->generation_context->nb_objects_elapsed * 1000000ull) /
        pub_ctx->generation_context->objects_per_second;
    if (pub_ctx->current_header.timestamp >= pub_ctx->generation_context->target_duration) {
        /* No object to generate, same as end of file */
        pub_ctx->is_finished = 1;
    }
    else {
        size_t object_size_max;

        /* Compute the content size */
        pub_ctx->current_header.length = test_media_generate_object_size(pub_ctx->generation_context);
        object_size_max = pub_ctx->current_header.length + QUIRRQ_MEDIA_TEST_HEADER_SIZE;
        ret = test_media_allocate_object(pub_ctx, object_size_max);
        if (ret == 0) {
            const uint8_t* fh;
            /* Generate the object header */
            fh = quicr_encode_object_header(pub_ctx->media_object, pub_ctx->media_object + QUIRRQ_MEDIA_TEST_HEADER_SIZE, &pub_ctx->current_header);
            if (fh == NULL) {
                ret = -1;
            }
            else {
                /* Generate the object content */
                uint8_t* content = pub_ctx->media_object + (fh - pub_ctx->media_object);

                for (size_t i = 0; i < pub_ctx->current_header.length; i++) {
                    content[i] = (uint8_t)(rand() & 0xff);
                }
                pub_ctx->media_object_size = (fh - pub_ctx->media_object) + pub_ctx->current_header.length;
                /* Update the generation context */
                pub_ctx->generation_context->nb_objects_elapsed += 1;
                pub_ctx->generation_context->nb_objects_sent += 1;
            }
        }
    }
    return ret;
}

static int test_media_publisher_check_object(test_media_publisher_context_t* pub_ctx)
{
    int ret = 0;

    if (pub_ctx->media_object_size <= pub_ctx->media_object_read) {
        /* No more object data available. */
        pub_ctx->media_object_size = 0;
        pub_ctx->media_object_read = 0;
        if (pub_ctx->F != NULL) {
            /* Read the next object from the file */
            ret = test_media_read_object_from_file(pub_ctx);
        }
        else {
            /* Generate a object */
            ret = test_media_generate_object(pub_ctx);
        }
    }

    return ret;
}

uint8_t test_media_set_flags(int is_real_time, int is_audio, size_t media_object_size)
{
    uint8_t flags = 0;

    if (is_real_time) {
        if (is_audio) {
            flags = 0x80;
        }
        else {
            if (media_object_size > 10000) {
                flags = 0x81;
            }
            else {
                flags = 0x82;
            }
        }
    }

    return flags;
}

int test_media_object_publisher_fn(
    quicrq_media_source_action_enum action,
    void* media_ctx,
    uint8_t* data,
    size_t data_max_size,
    size_t* data_length,
    uint8_t * flags,
    int * is_new_group,
    uint64_t * object_length,
    int* is_media_finished,
    int * is_still_active,
    int* has_backlog,
    uint64_t current_time)
{
    int ret = 0;
    test_media_publisher_context_t* pub_ctx = (test_media_publisher_context_t*)media_ctx;

    *is_new_group = 0;
    *has_backlog = 0;

    if (action == quicrq_media_source_get_data) {
        if (data == NULL && pub_ctx->min_packet_size > 0 && data_max_size < pub_ctx->min_packet_size) {
            /* Simulate behavior of a data source that only transmit packets if
             * enough space is available in datagram */
            *data_length = 0;
            *is_still_active = 1;
        }
        else {
            *is_media_finished = 0;
            *data_length = 0;
            *flags = test_media_set_flags(pub_ctx->is_real_time, pub_ctx->is_audio, pub_ctx->media_object_size);
            ret = test_media_publisher_check_object(pub_ctx);
            *object_length = pub_ctx->media_object_size;
            if (ret == 0) {
                *is_still_active = 1;
                if (pub_ctx->is_finished) {
                    *is_media_finished = 1;
                    *is_still_active = 0;
                }
                else if (pub_ctx->media_object_size > pub_ctx->media_object_read) {
                    if (!pub_ctx->is_real_time ||
                        current_time >= pub_ctx->current_header.timestamp + pub_ctx->start_time) {
                        /* Copy data from object in memory */
                        size_t available = pub_ctx->media_object_size - pub_ctx->media_object_read;
                        size_t copied = data_max_size;
                        /* Simulate begining of group on either video I frame (l > 10000) 
                         * or any audio frame (l < 200) */
                        if (pub_ctx->media_object_read == 0 && (
                            pub_ctx->media_object_size > 10000 || pub_ctx->media_object_size < 200)) {
                            *is_new_group = 1;
                        }

                        if (data_max_size >= available) {
                            copied = available;
                        }
                        *data_length = copied;
                        if (data != NULL) {
                            /* If data is set to NULL, return the available size but do not copy anything */
                            memcpy(data, pub_ctx->media_object + pub_ctx->media_object_read, copied);
                            pub_ctx->media_object_read += copied;
                        }
                        *pub_ctx->p_next_time = UINT64_MAX;
                    }
                    else {
                        *pub_ctx->p_next_time = pub_ctx->current_header.timestamp + pub_ctx->start_time;
                        *data_length = 0;
                        *is_still_active = 0;
                    }
                }
                else
                {
                    *data_length = 0;
                    *is_still_active = 0;
                }
            }
        }
    }
    else if (action == quicrq_media_source_skip_object)
    {
        /* Do nothing. There is no point in supporting that call in tests. */
    }
    else if (action == quicrq_media_source_close) {
        /* close the context */
        test_media_publisher_close(media_ctx);
    }
    return ret;
}

uint64_t test_media_publisher_next_time(void * media_ctx, uint64_t current_time)
{
    test_media_publisher_context_t* pub_ctx = (test_media_publisher_context_t*)media_ctx;
    uint64_t next_time = current_time;
    int ret = test_media_publisher_check_object(pub_ctx);

    if (ret == 0 && pub_ctx->current_header.timestamp + pub_ctx->start_time > next_time) {
        next_time = pub_ctx->current_header.timestamp + pub_ctx->start_time;
    }

    return next_time;
}

/* Provide an API for "declaring" a test media to the local quicrq context  */
static test_media_source_context_t* test_media_create_source(char const* media_source_path, const generation_parameters_t* generation_model,
    int is_real_time, uint64_t* p_next_time, uint64_t start_time)
{
    test_media_source_context_t* srce_ctx = (test_media_source_context_t*)malloc(sizeof(test_media_source_context_t));

    if (srce_ctx != NULL) {
        memset(srce_ctx, 0, sizeof(test_media_source_context_t));
        srce_ctx->file_path = media_source_path;
        srce_ctx->generation_context = generation_model;
        srce_ctx->is_real_time = is_real_time;
        srce_ctx->p_next_time = p_next_time;
        *srce_ctx->p_next_time = UINT64_MAX;
        srce_ctx->start_time = start_time;
    }
    return srce_ctx;
}


void test_media_delete(void * v_pub_source_ctx)
{
    free(v_pub_source_ctx);
}

/* Publish a test media as an object source:
 * - Create a test source context, then call quicrq_publish_object_source.
 * - Document the time for the next object
 * - If the time has come, then read the object and publish it.
 * - If reaching the end, call object fin.
 * - Call delete object in the end.
 */

static int test_media_object_source_check(test_media_object_source_context_t* object_pub_ctx)
{
    int ret = 0;
    test_media_publisher_context_t* pub_ctx = object_pub_ctx->pub_ctx;
    /* if this is the first call, or if the previous call was published,
     * read the next data from file. */
    if (!pub_ctx->is_finished) {
        if (!object_pub_ctx->object_is_ready || object_pub_ctx->object_is_published) {
            if (pub_ctx->F != NULL) {
                /* Read the next object from the file */
                ret = test_media_read_object_from_file(pub_ctx);
            }
            else {
                /* Generate a object */
                ret = test_media_generate_object(pub_ctx);
            }
            if (ret == 0) {
                object_pub_ctx->object_is_ready = 1;
                object_pub_ctx->object_is_published = 0;
                if (pub_ctx->is_finished) {
                    object_pub_ctx->source_is_finished = 1;
                }
            }
        }
    }
    return ret;
}

int test_media_is_new_group(size_t media_object_size)
{
    /* For test purpose, we consider objects larger than 10000 bytes as starting a new group */
    /* Special case of audio: small packets, group by itself. */
    return (media_object_size > 10000 || media_object_size < 200);
}

int test_media_object_source_iterate(
    test_media_object_source_context_t* object_pub_ctx,
    uint64_t current_time, int * is_active)
{
    int ret = 0;
    test_media_publisher_context_t* pub_ctx = object_pub_ctx->pub_ctx;
    uint64_t published_group_id = object_pub_ctx->object_source_ctx->next_group_id;
    uint64_t published_object_id = object_pub_ctx->object_source_ctx->next_object_id;

    ret = test_media_object_source_check(object_pub_ctx);

    if (ret == 0) {
        if (object_pub_ctx->object_is_ready && !object_pub_ctx->object_is_published) {
            if (object_pub_ctx->source_is_finished) {
                /* if this the file is finished but the fin is not, publish the fin
                 */
                quicrq_publish_object_fin(object_pub_ctx->object_source_ctx);
                object_pub_ctx->object_is_published = 1;
                *is_active |= 1;
            }
            else if (!pub_ctx->is_real_time ||
                current_time >= pub_ctx->start_time + pub_ctx->current_header.timestamp) {
                /* else if the data is not published, publish it */
                int is_new_group = test_media_is_new_group(pub_ctx->media_object_size);
                quicrq_media_object_properties_t properties = { 0 };
                if (is_new_group && published_object_id > 0) {
                    published_group_id++;
                    published_object_id = 0;
                }
                properties.flags = test_media_set_flags(pub_ctx->is_real_time, pub_ctx->is_audio, pub_ctx->media_object_size);
                ret = quicrq_publish_object(object_pub_ctx->object_source_ctx, pub_ctx->media_object, pub_ctx->media_object_size, 
                    &properties, published_group_id, published_object_id);
                object_pub_ctx->object_is_published = 1;
                *is_active |= 1;
            }
        }
    }
    return ret;
}

uint64_t test_media_object_source_next_time(
    test_media_object_source_context_t* object_pub_ctx,
    uint64_t current_time)
{
    int ret = 0;
    uint64_t next_time = UINT64_MAX;
    test_media_publisher_context_t* pub_ctx = object_pub_ctx->pub_ctx;

    ret = test_media_object_source_check(object_pub_ctx);

    if (ret == 0) {
        if (object_pub_ctx->object_is_ready && !object_pub_ctx->object_is_published) {
            if (object_pub_ctx->source_is_finished) {
                /* if this the file is finished but the fin is not, publish the fin now
                 */
                next_time = current_time;
            }
            else if (pub_ctx->is_real_time) {
                next_time = pub_ctx->start_time + pub_ctx->current_header.timestamp;
            }
            else {
                next_time = current_time;
            }
        }
    }
    else {
        next_time = current_time;
    }
    return next_time;
}

void test_media_object_source_delete(test_media_object_source_context_t* object_pub_ctx)
{
    if (object_pub_ctx != NULL) {
        if (object_pub_ctx->object_source_ctx != NULL) {
            quicrq_delete_object_source(object_pub_ctx->object_source_ctx);
        }
        if (object_pub_ctx->pub_ctx != NULL) {
            test_media_publisher_close(object_pub_ctx->pub_ctx);
        }
        free(object_pub_ctx);
    }
}

int test_media_is_audio(const uint8_t* url, size_t url_length)
{
    /* Mark stream as "audio" if the url says so */
    int is_audio = 0;
    for (size_t i = 0; i + 4 < url_length; i++) {
        if (url[i] == (uint8_t)'a' &&
            url[i + 1] == (uint8_t)'u' &&
            url[i + 2] == (uint8_t)'d' &&
            url[i + 3] == (uint8_t)'i' &&
            url[i + 4] == (uint8_t)'o') {
            is_audio = 1;
            break;
        }
    }
    return is_audio;
}

/* In order to test the "start point" function, we start the reference source at a specific start point.
*/
static int test_media_object_source_set_start(test_media_object_source_context_t* object_pub_ctx, uint64_t start_group_id, uint64_t start_object_id)
{
    int ret = 0;

    if (object_pub_ctx->pub_ctx != NULL && object_pub_ctx->pub_ctx->F != NULL) {
        uint64_t group_id = 0;
        uint64_t object_id = 0;

        /* Read the media file until the context matches */
        while (ret == 0 && (group_id < start_group_id || (group_id == start_group_id && object_id < start_object_id))) {
            ret = test_media_read_object_from_file(object_pub_ctx->pub_ctx);
            if (ret == 0 && object_id > 0 && test_media_is_new_group(object_pub_ctx->pub_ctx->current_header.length)) {
                group_id++;
                object_id = 0;
            }
            else {
                object_id++;
            }
        }
        if (ret == 0) {
            /* mark the segment as available */
            object_pub_ctx->object_is_ready = 1;
        }
    }
    else {
        ret = -1;
    }
    return ret;
}

test_media_object_source_context_t* test_media_object_source_publish_ex(quicrq_ctx_t* qr_ctx, uint8_t* url, size_t url_length,
    char const* media_source_path, const generation_parameters_t* generation_model, int is_real_time,
    uint64_t start_time, quicrq_media_object_source_properties_t * properties)
{
    int ret = 0;
    test_media_object_source_context_t* object_pub_ctx = (test_media_object_source_context_t*)malloc(
        sizeof(test_media_object_source_context_t));
    if (object_pub_ctx != NULL) {
        memset(object_pub_ctx, 0, sizeof(test_media_object_source_context_t));
        object_pub_ctx->pub_ctx =
            test_media_publisher_init(media_source_path, generation_model, is_real_time, start_time);
        if (object_pub_ctx->pub_ctx == NULL) {
            ret = -1;
        }
        else if (properties != NULL && (properties->start_group_id != 0 || properties->start_object_id != 0)) {
            ret = test_media_object_source_set_start(object_pub_ctx, properties->start_group_id, properties->start_object_id);
        }

        if (ret == 0) {
            object_pub_ctx->object_source_ctx = quicrq_publish_object_source(qr_ctx, url, url_length, properties);
            if (object_pub_ctx->pub_ctx == NULL) {
                ret = -1;
            }
        }

        if (ret != 0) {
            test_media_object_source_delete(object_pub_ctx);
            object_pub_ctx = NULL;
        }
        else {
            /* Mark stream as "audio" if the url says so */
            test_media_publisher_context_t* media_ctx = (test_media_publisher_context_t*)object_pub_ctx->pub_ctx;
            media_ctx->is_audio = test_media_is_audio(url, url_length);
        }
    }
    return object_pub_ctx;
}

test_media_object_source_context_t* test_media_object_source_publish(quicrq_ctx_t* qr_ctx, uint8_t* url, size_t url_length,
    char const* media_source_path, const generation_parameters_t* generation_model, int is_real_time,
    uint64_t start_time)
{
    return test_media_object_source_publish_ex(qr_ctx, url, url_length, media_source_path, generation_model, is_real_time, start_time, NULL);
}

/* Media receiver definitions.
 * Manage a list of objects being reassembled. The list is organized as a splay,
 * indexed by the object id and object offset. When a new fragment is received
 * the code will check whether the object is already present, and then whether the
 * fragment for that object has already arrived.
 */

typedef struct st_test_media_consumer_context_t {
    FILE* Res;
    FILE* Log;
    uint8_t header_bytes[QUIRRQ_MEDIA_TEST_HEADER_SIZE];
    quicrq_media_object_header_t current_header;

    quicrq_reassembly_context_t reassembly_ctx;

} test_media_consumer_context_t;

int test_media_derive_file_names(const uint8_t* url, size_t url_length, quicrq_transport_mode_enum transport_mode, int is_real_time, int is_post,
    char * result_file_name, char * result_log_name, size_t result_name_size)
{
    int ret = 0;
    size_t last_sep = 0;
    size_t last_dot = (int)url_length;
    size_t name_length = 0;
    for (size_t i = 0; i < url_length; i++) {
        if (url[i] == (uint8_t)'\\' || url[i] == (uint8_t)'/') {
            last_sep = (int)(i + 1);
            last_dot = (int)url_length;
        }
        else if (url[i] == (uint8_t)'.') {
            last_dot = (int)i;
        }
    }
    name_length = last_dot - last_sep;
    if (name_length <= 0 || name_length + 10 >= result_name_size) {
        ret = -1;
    }
    else {
        /* Derive file names from URL */
        memcpy(result_file_name, url + last_sep, name_length);
        result_file_name[name_length + 0] = '_';
        result_file_name[name_length + 1] = (is_post) ? 'P' : 'G';
        result_file_name[name_length + 2] = '_';
        result_file_name[name_length + 3] = (is_real_time) ? 'r' : 'n';
        result_file_name[name_length + 4] = '_';
        result_file_name[name_length + 5] = quicrq_transport_mode_to_letter(transport_mode);
        result_file_name[name_length + 6] = '.';
        result_file_name[name_length + 7] = 'b';
        result_file_name[name_length + 8] = 'i';
        result_file_name[name_length + 9] = 'n';
        result_file_name[name_length + 10] = 0;
        memcpy(result_log_name, url + last_sep, name_length);
        result_log_name[name_length + 0] = '_';
        result_log_name[name_length + 1] = (is_post) ? 'P' : 'G';
        result_log_name[name_length + 2] = '_';
        result_log_name[name_length + 3] = (is_real_time) ? 'r' : 's';
        result_log_name[name_length + 4] = '_';
        result_log_name[name_length + 5] = quicrq_transport_mode_to_letter(transport_mode);
        result_log_name[name_length + 6] = '.';
        result_log_name[name_length + 7] = 'c';
        result_log_name[name_length + 8] = 's';
        result_log_name[name_length + 9] = 'v';
        result_log_name[name_length + 10] = 0;
    }
    return ret;
}

int test_media_consumer_init_callback(quicrq_stream_ctx_t* stream_ctx, const uint8_t* url, size_t url_length)
{
    int ret = 0;
    void* media_ctx;
    char result_file_name[512];
    char result_log_name[512];

    ret = test_media_derive_file_names(url, url_length, stream_ctx->transport_mode,
        1, 1, result_file_name, result_log_name, sizeof(result_file_name));

    if (ret == 0) {
        /* Init the local media consumer */
        media_ctx = test_media_consumer_init(result_file_name, result_log_name);

        if (media_ctx == NULL) {
            ret = -1;
        }
        else
        {
            /* set the parameter in the stream context. */
            ret = quicrq_set_media_stream_ctx(stream_ctx, test_media_object_consumer_cb, media_ctx);
        }
    }

    return ret;
}

int test_media_consumer_close(void* media_ctx)
{
    /* Close result file and log file */
    int ret = 0;
    test_media_consumer_context_t* cons_ctx = (test_media_consumer_context_t*)media_ctx;

    if (cons_ctx->Res != NULL) {
        picoquic_file_close(cons_ctx->Res);
    }
    if (cons_ctx->Log != NULL) {
        picoquic_file_close(cons_ctx->Log);
    }

    quicrq_reassembly_release(&cons_ctx->reassembly_ctx);

    free(media_ctx);

    return ret;
}

void* test_media_consumer_init(char const* media_result_file, char const* media_result_log)
{
    /* Open and initialize result file and log file */
    test_media_consumer_context_t* cons_ctx = (test_media_consumer_context_t*)malloc(sizeof(test_media_consumer_context_t));
    if (cons_ctx != NULL) {
        int last_err;
        memset(cons_ctx, 0, sizeof(test_media_consumer_context_t));

        quicrq_reassembly_init(&cons_ctx->reassembly_ctx);

        if ((cons_ctx->Res = picoquic_file_open_ex(media_result_file, "wb", &last_err)) == NULL) {
            DBG_PRINTF("Cannot open %s, error: %d (0x%x)", media_result_file, last_err, last_err);
        }
        if ((cons_ctx->Log = picoquic_file_open_ex(media_result_log, "w", &last_err)) == NULL) {
            DBG_PRINTF("Cannot open %s, error: %d (0x%x)", media_result_log, last_err, last_err);
        }
        if (cons_ctx->Res == NULL || cons_ctx->Log == NULL) {
            (void)test_media_consumer_close(cons_ctx);
            cons_ctx = NULL;
        }
    }
    return cons_ctx;
}

int test_media_consumer_object_ready(
    void* media_ctx,
    uint64_t current_time,
    uint64_t group_id,
    uint64_t object_id,
    uint8_t flags,
    const uint8_t* data,
    size_t data_length,
    quicrq_reassembly_object_mode_enum object_mode)
{
    int ret = 0;
    test_media_consumer_context_t* cons_ctx = (test_media_consumer_context_t*)media_ctx;

    /* Find the object header */
    if (data_length < QUIRRQ_MEDIA_TEST_HEADER_SIZE) {
        /* Malformed object */
        ret = -1;
    }
    else {
        quicrq_media_object_header_t current_header;
        const uint8_t* fh = quicr_decode_object_header(data,
            data + QUIRRQ_MEDIA_TEST_HEADER_SIZE, &current_header);
        if (fh == NULL) {
            ret = -1;
        }
        if (ret == 0) {
            /* if first time seen, document the delivery in the log */
            if (object_mode != quicrq_reassembly_object_repair) {
                if (fprintf(cons_ctx->Log, "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%zu,%d\n",
                    group_id, object_id, current_time,
                    current_header.timestamp, current_header.number, current_header.length, flags) <= 0) {
                    ret = -1;
                }
            }
        }
        if (ret == 0) {
            /* if in sequence, write the data to the file. */
            if (object_mode != quicrq_reassembly_object_peek) {
                if (fwrite(data, 1, data_length, cons_ctx->Res) != data_length) {
                    ret = -1;
                }
            }
        }
    }
    return ret;
}

int test_media_object_consumer_cb(
    quicrq_media_consumer_enum action,
    void* media_ctx,
    uint64_t current_time,
    const uint8_t* data,
    uint64_t group_id,
    uint64_t object_id,
    uint64_t offset,
    uint64_t queue_delay,
    uint8_t flags,
    uint64_t nb_objects_previous_group,
    uint64_t object_length,
    size_t data_length)
{
    int ret = 0;
    test_media_consumer_context_t* cons_ctx = (test_media_consumer_context_t*)media_ctx;

    switch (action) {
    case quicrq_media_datagram_ready:
        ret = quicrq_reassembly_input(&cons_ctx->reassembly_ctx, current_time, data, group_id, object_id, offset,
            queue_delay, flags, nb_objects_previous_group, object_length, data_length,
            test_media_consumer_object_ready, cons_ctx);
        if (ret == 0 && cons_ctx->reassembly_ctx.is_finished) {
            ret = quicrq_consumer_finished;
        }
        break;
    case quicrq_media_final_object_id:
        ret = quicrq_reassembly_learn_final_object_id(&cons_ctx->reassembly_ctx, group_id, object_id);
        if (ret == 0 && cons_ctx->reassembly_ctx.is_finished) {
            ret = quicrq_consumer_finished;
        }
        break;
    case quicrq_media_start_point:
        ret = quicrq_reassembly_learn_start_point(&cons_ctx->reassembly_ctx, group_id, object_id, current_time,
            test_media_consumer_object_ready, cons_ctx);
        if (ret == 0 && cons_ctx->reassembly_ctx.is_finished) {
            ret = quicrq_consumer_finished;
        }
        break;
    case quicrq_media_close:
        ret = test_media_consumer_close(media_ctx);
        break;
    case quicrq_media_real_time_cache:
        /* Ignore that for now */
        break;
    default:
        ret = -1;
        break;
    }
    return ret;
}

/* Object stream consumer
 */

void test_object_stream_consumer_release(test_object_stream_ctx_t* cons_ctx)
{
    /* Close result file and log file */
    cons_ctx->is_closed = 1;
    if (cons_ctx->Res != NULL) {
        picoquic_file_close(cons_ctx->Res);
        cons_ctx->Res = NULL;
    }
    if (cons_ctx->Log != NULL) {
        picoquic_file_close(cons_ctx->Log);
        cons_ctx->Log = NULL;
    }
}

void test_object_stream_consumer_close(void* v_cons_ctx)
{
    /* Close result file and log file */
    test_object_stream_ctx_t* cons_ctx = (test_object_stream_ctx_t*)v_cons_ctx;

    test_object_stream_consumer_release(cons_ctx);

    free(cons_ctx);
}

int test_object_stream_consumer_cb(
    quicrq_media_consumer_enum action,
    void* object_consumer_ctx,
    uint64_t current_time,
    uint64_t group_id,
    uint64_t object_id,
    const uint8_t* data,
    size_t data_length,
    quicrq_object_stream_consumer_properties_t* properties,
    quicrq_media_close_reason_enum close_reason,
    uint64_t close_error_number)
{
    int ret = 0;
    test_object_stream_ctx_t* cons_ctx = (test_object_stream_ctx_t*)object_consumer_ctx;

    switch (action) {
    case quicrq_media_datagram_ready:
        /* Special case for zero length objects */
        if (data_length == 0) {
            /* Create a fake header */
            quicrq_media_object_header_t current_header = { 0 };
            uint8_t flags = 0xff;
            /* Create log entry */
            if (fprintf(cons_ctx->Log, "%" PRIu64 ",%" PRIu64 ",%" PRIu64  ",%" PRIu64 ",%" PRIu64 ",%zu,%d\n",
                group_id, object_id, current_time, current_header.timestamp, current_header.number, current_header.length, flags) <= 0) {
                ret = -1;
            }
            if (ret == 0) {
                uint8_t header_buf[256];
                uint8_t* fh = quicr_encode_object_header(header_buf, header_buf + sizeof(header_buf), &current_header);
                if (fh == NULL) {
                    ret = -1;
                }
                else {
                    size_t header_length = fh - header_buf;
                    if (fwrite(header_buf, 1, header_length, cons_ctx->Res) != header_length) {
                        ret = -1;
                    }
                }
            }
        }
        else if (data_length < QUIRRQ_MEDIA_TEST_HEADER_SIZE) {
            /* Malformed object */
            ret = -1;
        }
        else {
            /* Parse the object header */
            quicrq_media_object_header_t current_header;
            const uint8_t* fh = quicr_decode_object_header(data,
                data + QUIRRQ_MEDIA_TEST_HEADER_SIZE, &current_header);
            if (fh == NULL) {
                ret = -1;
            }
            if (ret == 0) {
                /* in sequence, document the delivery in the log */
                uint8_t flags = (properties == NULL) ? 0 : properties->flags;
                if (fprintf(cons_ctx->Log, "%" PRIu64 ",%" PRIu64 ",%" PRIu64  ",%" PRIu64 ",%" PRIu64 ",%zu,%d\n",
                    group_id, object_id, current_time, current_header.timestamp, current_header.number, current_header.length, flags) <= 0) {
                    ret = -1;
                }
            }
            if (ret == 0) {
                /* in sequence, write the data to the file. */
                if (fwrite(data, 1, data_length, cons_ctx->Res) != data_length) {
                    ret = -1;
                }
            }
        }
        break;
    case quicrq_media_close:
        DBG_PRINTF("Closing media stream, reason code %d, error_number %" PRIu64 "\n", close_reason, close_error_number);
        /* Remove the reference to the media context, as the caller will free it. */
        cons_ctx->media_ctx = NULL;
        /* Close streams and other resource */
        test_object_stream_consumer_release(cons_ctx);
        break;
    default:
        ret = -1;
        break;
    }
    return ret;
}

test_object_stream_ctx_t* test_object_stream_subscribe_ex(quicrq_cnx_ctx_t* cnx_ctx, const uint8_t* url, size_t url_length,
    quicrq_transport_mode_enum transport_mode, quicrq_subscribe_order_enum order_required, quicrq_subscribe_intent_t * intent,
    char const* media_result_file, char const* media_result_log)
{
    int ret = 0;
    /* Open and initialize result file and log file */
    test_object_stream_ctx_t* cons_ctx = (test_object_stream_ctx_t*)malloc(sizeof(test_object_stream_ctx_t));

    if (cons_ctx != NULL) {
        int last_err;
        memset(cons_ctx, 0, sizeof(test_object_stream_ctx_t));

        if ((cons_ctx->Res = picoquic_file_open_ex(media_result_file, "wb", &last_err)) == NULL) {
            DBG_PRINTF("Cannot open %s, error: %d (0x%x)", media_result_file, last_err, last_err);
        }
        if ((cons_ctx->Log = picoquic_file_open_ex(media_result_log, "w", &last_err)) == NULL) {
            DBG_PRINTF("Cannot open %s, error: %d (0x%x)", media_result_log, last_err, last_err);
        }
        if (cons_ctx->Res == NULL || cons_ctx->Log == NULL) {
            ret = -1;
        }
        else {
            cons_ctx->media_ctx = quicrq_subscribe_object_stream(cnx_ctx, url, url_length, transport_mode, 
                order_required, intent, test_object_stream_consumer_cb, cons_ctx);
            if (cons_ctx->media_ctx == NULL) {
                ret = -1;
            }
        }
        if (ret != 0){
            test_object_stream_consumer_close(cons_ctx);
            cons_ctx = NULL;
        }
    }

    return cons_ctx;
}

test_object_stream_ctx_t* test_object_stream_subscribe(quicrq_cnx_ctx_t* cnx_ctx, const uint8_t* url, size_t url_length,
    quicrq_transport_mode_enum transport_mode, char const* media_result_file, char const* media_result_log)
{
    return test_object_stream_subscribe_ex(cnx_ctx, url, url_length, transport_mode, quicrq_subscribe_in_order,
        NULL, media_result_file, media_result_log);
}

void test_object_stream_unsubscribe(test_object_stream_ctx_t* cons_ctx)
{
    if (cons_ctx != NULL && cons_ctx->media_ctx != NULL) {
        quicrq_unsubscribe_object_stream(cons_ctx->media_ctx);
        cons_ctx->media_ctx = NULL;
    }
}

/* Compare media file.
 * These are binary files composed of sequences of objects.
 */
int quicrq_compare_media_file_ex(char const* media_result_file, char const* media_reference_file,
    int * nb_losses, uint8_t * loss_flag, uint64_t start_group_id, uint64_t start_object_id)
{
    int ret = 0;
    /* Open contexts for each file */
    test_media_publisher_context_t* result_ctx = (test_media_publisher_context_t*)
        test_media_publisher_init(media_result_file, NULL, 0, 0);
    test_media_publisher_context_t* ref_ctx = (test_media_publisher_context_t*)
        test_media_publisher_init(media_reference_file, NULL, 0, 0);

    if (loss_flag != NULL) {
        *loss_flag = 0xff;
    }

    if (nb_losses != NULL) {
        *nb_losses = 0;
    }

    if (result_ctx == NULL || ref_ctx == NULL) {
        ret = -1;
        DBG_PRINTF("Could not create result(0x%x) or reference(0x%x) publisher contexts, ret=%d", result_ctx, ref_ctx, ret);
    }
    else {
        int is_audio = test_media_is_audio((const uint8_t*)media_reference_file, strlen(media_reference_file));
        int nb_object = 0;
        int nb_ref_object = 0;
        uint64_t ref_group_id = 0;
        uint64_t ref_object_id = 0;

        /* Read the objects on both. They should match, or both should come to an end */
        while (ret == 0 && !result_ctx->is_finished && !ref_ctx->is_finished) {
            ret = test_media_read_object_from_file(result_ctx);
            nb_object++;
            if (ret != 0) {
                DBG_PRINTF("Could not read object from results, ret=%d", ret);
            } else {
                /* Get the next object, skipping to the start point if necessary */
                do {
                    ret = test_media_read_object_from_file(ref_ctx);
                    if (ret == 0) {
                        if (nb_ref_object > 0) {
                            /* Mimic here the generation of group id and object id in the test publisher,
                             * see test_media_object_source_iterate
                             */
                            if (test_media_is_new_group(ref_ctx->current_header.length)) {
                                ref_group_id++;
                                ref_object_id = 0;
                            }
                            else {
                                ref_object_id++;
                            }
                        }
                        nb_ref_object++;
                    }
                } while (ret == 0 && (ref_group_id < start_group_id ||
                    (ref_group_id == start_group_id && ref_object_id < start_object_id)));
                /* Compare values */
                if (ret == 0) {
                    /* Compare the media objects */
                    if (result_ctx->is_finished) {
                        if (!ref_ctx->is_finished) {
                            ret = -1;
                            DBG_PRINTF("Result file finished before reference: ret=%d", ret);
                        }
                    }
                    else if (ref_ctx->is_finished) {
                        if (!result_ctx->is_finished) {
                            ret = -1;
                            DBG_PRINTF("Result file not finished with reference: ret=%d", ret);
                        }
                    }
                    else if (result_ctx->media_object_size == 0 || result_ctx->current_header.length == 0) {
                        /* Indicates that this object was dropped in transmission */
                        if (nb_losses == NULL) {
                            DBG_PRINTF("Numbers %llu was lost", (unsigned long long)ref_ctx->current_header.number);
                            ret = -1;
                        }
                        else {
                            *nb_losses += 1;
                        }
                        if (loss_flag != NULL) {
                            uint8_t flag = test_media_set_flags(1, is_audio, ref_ctx->media_object_size);
                            if (flag < *loss_flag) {
                                *loss_flag = flag;
                            }
                        }
                    }
                    else if (ref_ctx->current_header.timestamp != result_ctx->current_header.timestamp) {
                        ret = -1;
                        DBG_PRINTF("Time stamps differ, %llx vs %llx: ret=%d", (unsigned long long)ref_ctx->current_header.timestamp, 
                            (unsigned long long)result_ctx->current_header.timestamp, ret);
                    }
                    else if (ref_ctx->current_header.number != result_ctx->current_header.number) {
                        ret = -1;
                        DBG_PRINTF("Numbers differ, %llu vs %llu: ret=%d", (unsigned long long)ref_ctx->current_header.number,
                            (unsigned long long)result_ctx->current_header.number, ret);
                    }
                    else if (ref_ctx->current_header.length != result_ctx->current_header.length) {
                        ret = -1;
                        DBG_PRINTF("Lengths differ, %lu vs %lu: ret=%d", (unsigned long)ref_ctx->current_header.number,
                            (unsigned long)result_ctx->current_header.number, ret);
                    }
                    else if (ref_ctx->media_object_size != result_ctx->media_object_size){
                        ret = -1;
                        DBG_PRINTF("object sizes differ, %zu vs %zu: ret=%d",  ref_ctx->media_object_size,
                            result_ctx->media_object_size, ret);
                    }
                    else if (memcmp(ref_ctx->media_object, result_ctx->media_object, ref_ctx->media_object_size) != 0) {
                        ret = -1;
                        DBG_PRINTF("Contents object #%d differ: ret=%d", nb_object, ret);
                    }
                }
            }
        }
    }
    if (result_ctx != NULL) {
        test_media_publisher_close(result_ctx);
    }
    if (ref_ctx != NULL) {
        test_media_publisher_close(ref_ctx);
    }

    return ret;
}

int quicrq_compare_media_file(char const* media_result_file, char const* media_reference_file)
{
    return quicrq_compare_media_file_ex(media_result_file, media_reference_file, NULL, NULL, 0, 0);
}

/* Compare log file to reference log file  
 */
int quicrq_compare_log_file(char const* media_result_log, char const* media_reference_log)
{
    int ret = 0;
    int last_err1=0;
    int last_err2 = 0;
    FILE* F = picoquic_file_open_ex(media_result_log, "r", &last_err1);
    FILE* G = picoquic_file_open_ex(media_reference_log, "r", &last_err2);

    if (F == NULL || G == NULL) {
        ret = -1;
    }
    else {
    char result_line[512];
    char ref_line[512];
    while (ret == 0) {
        char* result_read = fgets(result_line, sizeof(result_line), F);
        char* ref_read = fgets(ref_line, sizeof(ref_line), G);
        if (result_read == NULL) {
            if (ref_read != NULL) {
                ret = -1;
            }
            break;
        }
        else if (ref_read == NULL) {
            if (result_read != NULL) {
                ret = -1;
            }
            break;
        }
        else if (strcmp(ref_read, result_read) != 0) {
            ret = -1;
        }
    }
    }
    if (F != NULL) {
        picoquic_file_close(F);
    }
    if (G != NULL) {
        picoquic_file_close(G);
    }
    return ret;
}

/* Compare log file to reference log file
 */
const char* quicrq_get_log_number(const char* s, const char* s_max, int* v)
{
    *v = 0;

    while (s != NULL && s < s_max) {
        char c = *s;
        s++;
        if (c == ' ') {
            continue;
        }
        if (c == ',') {
            break;
        }
        if (c >= '0' && c <= '9') {
            *v *= 10;
            *v += (c - '0');
        }
        else {
            s = NULL;
        }
    }
    return s;
}

int quicrq_log_file_statistics(char const* media_result_log, int* nb_frames, int* nb_losses,
    uint64_t* delay_average, uint64_t* delay_min, uint64_t* delay_max)
{
    int ret = 0;
    int last_err1 = 0;
    *nb_frames = 0;
    *nb_losses = 0;
    *delay_average = 0;
    *delay_min = 0;
    *delay_max = 0;


    FILE* F = picoquic_file_open_ex(media_result_log, "r", &last_err1);

    if (F == NULL) {
        ret = -1;
    }
    else {
        char result_line[512];
        uint64_t d_sum = 0;
        uint64_t d_min = UINT64_MAX;
        uint64_t d_max = 0;
        while (ret == 0) {
            char* result_read = fgets(result_line, sizeof(result_line), F);
            if (result_read == NULL) {
                break;
            }
            else {
                int g_id = 0;
                int o_id = 0;
                int a_time = 0;
                int o_time = 0;
                int f_num = 0;
                int len = 0;
                const char* s = result_line;
                const char* s_max = result_line + strlen(result_line);

                /* Parse the log line */
                if (NULL != (s = quicrq_get_log_number(s, s_max, &g_id)) &&
                    NULL != (s = quicrq_get_log_number(s, s_max, &o_id)) &&
                    NULL != (s = quicrq_get_log_number(s, s_max, &a_time)) &&
                    NULL != (s = quicrq_get_log_number(s, s_max, &o_time)) &&
                    NULL != (s = quicrq_get_log_number(s, s_max, &f_num))) {
                    s = quicrq_get_log_number(s, s_max, &len);
                }
                if (s == NULL) {
                    ret = -1;
                    break;
                }
                else {
                    *nb_frames += 1;
                    if (len <= 0) {
                        *nb_losses += 1;
                    }
                    else {
                        uint64_t delay = (a_time > o_time) ? a_time - o_time : 0;
                        d_sum += delay;
                        if (d_min > delay) {
                            d_min = delay;
                        }
                        if (d_max < delay) {
                            d_max = delay;
                        }
                    }
                }
            }
        }
        if (ret == 0 && *nb_frames > 0) {
            *delay_average = d_sum / *nb_frames;
            *delay_min = d_min;
            *delay_max = d_max;
        }
    }
    if (F != NULL) {
        picoquic_file_close(F);
    }

    return ret;
}

/* The media test provides two results:
 * - a media result file, which shuld be identical to the media source file
 * - a media result log, which provides for each received object the receive time, compared to the media time
 */

int quicrq_media_api_test_one(char const *media_source_name, char const* media_log_reference, 
    char const * media_result_file, char const* media_result_log, const generation_parameters_t* generation_model, int is_real_time)
{
    int ret = 0;
    char media_source_path[512];
    char media_log_ref_path[512];
    uint8_t media_buffer[1024];
    uint64_t current_time = 0;
    uint64_t next_time;
    size_t data_length;
    void* srce_ctx = NULL;
    void* pub_ctx = NULL;
    void* cons_ctx = NULL;
    uint64_t group_id = 0;
    uint64_t object_id = 0;
    uint64_t object_offset = 0;
    uint8_t flags = 0;
    int is_new_group = 0;
    uint64_t object_length = 0;
    int is_media_finished = 0;
    int is_still_active = 0;
    int has_backlog = 0;
    int inactive = 0;

    /* Locate the source and reference file */
    if (picoquic_get_input_path(media_source_path, sizeof(media_source_path),
        quicrq_test_solution_dir, media_source_name) != 0 ||
        picoquic_get_input_path(media_log_ref_path, sizeof(media_log_ref_path),
            quicrq_test_solution_dir, media_log_reference) != 0){
        ret = -1;
    }

    /* Init the publisher and consumer */
    if (ret == 0) {
        srce_ctx = test_media_create_source(media_source_path, generation_model, is_real_time, &next_time, 0);
        if (srce_ctx != NULL) {
            pub_ctx = test_media_publisher_subscribe(srce_ctx, NULL);
        }
        cons_ctx = test_media_consumer_init(media_result_file, media_result_log);
        if (pub_ctx == NULL || cons_ctx == NULL){
            ret = -1;
        }
    }

    /* Loop through publish and consume until finished */
    while (ret == 0 && !is_media_finished && inactive < 32) {
        ret = test_media_object_publisher_fn(quicrq_media_source_get_data,
            pub_ctx, media_buffer, sizeof(media_buffer),
            &data_length, &flags, &is_new_group, &object_length, &is_media_finished, &is_still_active, &has_backlog, current_time);
        if (ret != 0) {
            DBG_PRINTF("Publisher, ret=%d", ret);
        }
        else if (!is_media_finished && data_length == 0) {
            /* Update the current time to reflect media time */
            current_time = test_media_publisher_next_time(pub_ctx, current_time);
            inactive++;
        } else if (is_media_finished && data_length == 0){
            break;
        } else {
            uint64_t nb_objects_previous_group = 0;
            inactive = 0;
            if (is_new_group) {
                if (object_id > 0) {
                    nb_objects_previous_group = object_id;
                    group_id += 1;
                    object_id = 0;
                    object_offset = 0;
                }
            }
            ret = test_media_object_consumer_cb(quicrq_media_datagram_ready, cons_ctx, current_time, media_buffer,
                group_id, object_id, object_offset, 0, flags, nb_objects_previous_group, object_length, data_length);
            if (ret != 0) {
                DBG_PRINTF("Consumer, ret=%d", ret);
            }
            else
            {
                object_offset += data_length;

                if (object_offset >= object_length) {
                    object_id++;
                    object_offset = 0;
                }
            }
        }
    }

    /* Close publisher and consumer */
    if (pub_ctx != NULL) {
        test_media_publisher_close(pub_ctx);
    }

    if (ret == 0) {
        ret = test_media_object_consumer_cb(quicrq_media_final_object_id, cons_ctx, current_time, NULL,
            group_id, object_id, 0, 0, 0, 0, 0, 0);
        if (ret == quicrq_consumer_finished) {
            ret = 0;
        }
        else {
            DBG_PRINTF("Consumer not finished after final offset! ret = %d", ret);
            ret = -1;
        }
    }

    if (cons_ctx != NULL) {
        int close_ret = test_media_consumer_close(cons_ctx);
        if (ret == 0) {
            ret = close_ret;
        }
    }

    /* Compare media result to media source */
    if (ret == 0) {
        ret = quicrq_compare_log_file(media_result_log, media_log_ref_path);
    }

    if (ret == 0) {
        ret = quicrq_compare_media_file(media_result_file, media_source_path);
    }

    return ret;
}

#ifdef _WINDOWS
#define QUICRQ_TEST_VIDEO1_SOURCE "tests\\video1_source.bin"
#define QUICRQ_TEST_VIDEO1_LOGREF "tests\\video1_logref.csv"
#define QUICRQ_TEST_VIDEO1_RT_LOGREF "tests\\video1_rt_logref.csv"
#else
#define QUICRQ_TEST_VIDEO1_SOURCE "tests/video1_source.bin"
#define QUICRQ_TEST_VIDEO1_LOGREF "tests/video1_logref.csv"
#define QUICRQ_TEST_VIDEO1_RT_LOGREF "tests/video1_rt_logref.csv"
#endif
#define QUICRQ_TEST_VIDEO1_RESULT "video1_result.bin"
#define QUICRQ_TEST_VIDEO1_LOG    "video1_log.csv"
#define QUICRQ_TEST_VIDEO1_RT_RESULT "video1_rt_result.bin"
#define QUICRQ_TEST_VIDEO1_RT_LOG    "video1_rt_log.csv"
#define QUICRQ_TEST_VIDEO1_LOSS_RESULT "video1_loss_result.bin"
#define QUICRQ_TEST_VIDEO1_LOSS_LOG    "video1_loss_log.csv"
#define QUICRQ_TEST_MEDIA_object_RESULT "media_object_result.bin"
#define QUICRQ_TEST_MEDIA_object_LOG    "media_object_log.csv"
#define QUICRQ_TEST_MEDIA_object_LOSS_RESULT "media_object_loss_result.bin"
#define QUICRQ_TEST_MEDIA_object_LOSS_LOG    "media_object_loss_log.csv"



const generation_parameters_t video_1mps = {
    10000000, 30, 10, 60, 4000, 5000, 0, 0 };

int quicrq_media_video1_test()
{
    int ret = quicrq_media_api_test_one(QUICRQ_TEST_VIDEO1_SOURCE, QUICRQ_TEST_VIDEO1_LOGREF, QUICRQ_TEST_VIDEO1_RESULT, QUICRQ_TEST_VIDEO1_LOG,
        &video_1mps, 0);

    return ret;
}

int quicrq_media_video1_rt_test()
{
    int ret = quicrq_media_api_test_one(QUICRQ_TEST_VIDEO1_SOURCE, QUICRQ_TEST_VIDEO1_RT_LOGREF, 
        QUICRQ_TEST_VIDEO1_RT_RESULT, QUICRQ_TEST_VIDEO1_RT_LOG,
        &video_1mps, 1);

    return ret;
}

#ifdef _WINDOWS
#define QUICRQ_TEST_AUDIO1_SOURCE "tests\\audio1_source.bin"
#define QUICRQ_TEST_AUDIO1_LOGREF "tests\\audio1_logref.csv"
#define QUICRQ_TEST_AUDIO1_RT_LOGREF "tests\\audio1_rt_logref.csv"
#else
#define QUICRQ_TEST_AUDIO1_SOURCE "tests/audio1_source.bin"
#define QUICRQ_TEST_AUDIO1_LOGREF "tests/audio1_logref.csv"
#define QUICRQ_TEST_AUDIO1_RT_LOGREF "tests/audio1_rt_logref.csv"
#endif
#define QUICRQ_TEST_AUDIO1_RESULT "audio1_result.bin"
#define QUICRQ_TEST_AUDIO1_LOG    "audio1_log.csv"
#define QUICRQ_TEST_AUDIO1_RT_RESULT "audio1_rt_result.bin"
#define QUICRQ_TEST_AUDIO1_RT_LOG    "audio1_rt_log.csv"
#define QUICRQ_TEST_AUDIO1_LOSS_RESULT "audio1_loss_result.bin"
#define QUICRQ_TEST_AUDIO1_LOSS_LOG    "audio1_loss_log.csv"

int target_duration;
int objects_per_second;
int nb_p_in_i;
int objects_in_epoch;
size_t target_p_min;
size_t target_p_max;
int nb_objects_elapsed;
int nb_objects_sent;

const generation_parameters_t audio_18kbps = {
    10000000, 100, 1, 1, 22, 22, 0, 0 };

int quicrq_media_audio1_test()
{
    int ret = quicrq_media_api_test_one(QUICRQ_TEST_AUDIO1_SOURCE, QUICRQ_TEST_AUDIO1_RT_LOGREF,
        QUICRQ_TEST_AUDIO1_RT_RESULT, QUICRQ_TEST_AUDIO1_RT_LOG,
        &audio_18kbps, 1);

    return ret;
}

/* Media datagram test. Check the datagram API.
 */

typedef struct st_media_disorder_hole_t {
    struct st_media_disorder_hole_t* next_loss;
    uint64_t group_id;
    uint64_t object_id;
    uint64_t offset;
    uint64_t object_length;
    int is_last_fragment;
    size_t length;
    uint8_t media_buffer[1024];
} media_disorder_hole_t;

int quicrq_media_datagram_test_one(char const* media_source_name, char const* media_result_file, char const* media_result_log, size_t nb_losses, uint64_t* loss_object, int * loss_mode, size_t nb_dup)
{
    int ret = 0;
    char media_source_path[512];
    uint8_t media_buffer[1024];
    uint64_t current_time = 0;
    uint64_t next_srce_time = 0;
    size_t data_length;
    void* srce_ctx = NULL;
    void* pub_ctx = NULL;
    void* cons_ctx = NULL;
    size_t actual_losses = 0;
    int consumer_properly_finished = 0;
    uint64_t group_id = 0;
    uint64_t object_id = 0;
    uint64_t object_offset = 0;
    uint64_t object_length = 0;
    uint8_t flags = 0;
    int is_new_group = 0;
    int is_media_finished = 0;
    int is_still_active = 0;
    int has_backlog = 0;
    media_disorder_hole_t* first_loss = NULL;
    media_disorder_hole_t* last_loss = NULL;

    if (ret == 0) {
        /* Locate the source and reference file */
        if (picoquic_get_input_path(media_source_path, sizeof(media_source_path),
            quicrq_test_solution_dir, media_source_name) != 0) {
            ret = -1;
        }
    }

    /* Initialize a consumer context for testing */
    /* TODO: this should now be a object consumer */
    if (ret == 0) {
        cons_ctx = test_media_consumer_init(media_result_file, media_result_log);
        if (cons_ctx == NULL) {
            ret = -1;
        }
    }

    /* Init the publisher context for testing */
    if (ret == 0) {
        srce_ctx = test_media_create_source(media_source_path, NULL, 1, &next_srce_time, 0);
        if (srce_ctx != NULL) {
            if ((pub_ctx = test_media_publisher_subscribe(srce_ctx, NULL)) == NULL) {
                ret = -1;
            }
        }
    }

    /* Loop through read and consume until finished, marking some objects as lost */
    while (ret == 0) {
        /* Get the next object from the publisher */
        ret = test_media_object_publisher_fn(
            quicrq_media_source_get_data, pub_ctx, media_buffer, sizeof(media_buffer),
            &data_length, &flags, &is_new_group, &object_length,  &is_media_finished, &is_still_active, &has_backlog, current_time);
        /* TODO: manage the new group transitions */
        if (ret != 0) {
            DBG_PRINTF("Media published function: ret = %d", ret);
            break;
        }
        if (data_length == 0) {
            if (is_media_finished) {
                break;
            }
            else {
                /* Update the simulated time and continue the loop */
                current_time = test_media_publisher_next_time(pub_ctx, current_time);
            }
        }
        else {

        }
        /* Test whether to simulate losses or arrival */
        if (actual_losses < nb_losses &&
            (object_id == loss_object[actual_losses] || (loss_object[actual_losses] == UINT64_MAX && is_media_finished)) &&
            (loss_mode[actual_losses] == 3 ||
                (loss_mode[actual_losses] == 0 && object_offset == 0) ||
                (loss_mode[actual_losses] == 2 && object_offset + data_length < object_length) ||
                (loss_mode[actual_losses == 1 && object_offset != 0 && 
                object_offset + data_length >= object_length]))) {
            /* If the object packet should be seen as lost, store it for repetition */
            media_disorder_hole_t* loss = (media_disorder_hole_t*)malloc(sizeof(media_disorder_hole_t));
            if (loss == NULL) {
                ret = -1;
                break;
            }
            memset(loss, 0, sizeof(media_disorder_hole_t));
            loss->object_id = object_id;
            loss->offset = object_offset;
            loss->object_length = object_length;
            loss->length = data_length;
            loss->is_last_fragment = object_offset + data_length >= object_length;
            memcpy(loss->media_buffer, media_buffer, data_length);
            if (last_loss == NULL) {
                first_loss = loss;
            }
            else {
                last_loss->next_loss = loss;
            }
            last_loss = loss;
        }
        else {
            /* Simulate arrival of packet -- TODO: deal with group_id */
            uint64_t nb_objects_previous_group = 0;
            ret = test_media_object_consumer_cb(quicrq_media_datagram_ready, cons_ctx, current_time, media_buffer,
                    group_id, object_id, object_offset, 0, flags, nb_objects_previous_group, object_length, data_length);
            if (ret != 0) {
                DBG_PRINTF("Media consumer callback: ret = %d", ret);
                break;
            }
        }
        /* Count the fragments and the objects */
        if (object_offset + data_length >= object_length) {
            object_id++;
            object_offset = 0;
            if (actual_losses < nb_losses &&
                (object_id == loss_object[actual_losses] || (loss_object[actual_losses] == UINT64_MAX && is_media_finished))) {
                actual_losses++;
            }
        }
        else {
            object_offset += data_length;
        }
    }

    /* Indicate the final object_id, to simulate what datagrams would do */
    if (ret == 0) {
        ret = test_media_object_consumer_cb(quicrq_media_final_object_id, cons_ctx, current_time, NULL, group_id, object_id, 0, 0, 0, 0, 0, 0);
        if (ret == quicrq_consumer_finished) {
            consumer_properly_finished = 1;
            if (nb_losses > 0) {
                ret = -1;
            }
            else {
                ret = 0;
            }
        } 
        if (ret != 0) {
            DBG_PRINTF("Media consumer callback: ret = %d", ret);
        }
    }

    /* At this point, all blocks have been sent, except for the holes */
    /* TODO: holes should have object numbers and offset */
    /* TODO: use the object APIs */
    if (ret == 0 && first_loss != NULL) {
        media_disorder_hole_t* loss = first_loss;
        if (nb_dup > 0) {
            /* Fill some holes, in order to simulate duplication of repairs. */
            size_t actual_dup = 0;
            while (loss != NULL && actual_dup < nb_dup && ret == 0) {
                /* Simulate repair of a hole */
                actual_dup++;
                ret = test_media_object_consumer_cb(quicrq_media_datagram_ready, cons_ctx, current_time, loss->media_buffer,
                    loss->group_id, loss->object_id, loss->offset, 0, 0, 0, loss->object_length, loss->length);
                if (ret != 0) {
                    DBG_PRINTF("Media consumer callback: ret = %d", ret);
                }
                else {
                    /* skip the next loss */
                    loss = loss->next_loss;
                    if (loss != NULL) {
                        loss = loss->next_loss;
                    }
                }
            }
        }
        /* Fill the remaining holes */
        loss = first_loss;
        while (loss != NULL && ret == 0) {
            /* Simulate repair of a hole */
            ret = test_media_object_consumer_cb(quicrq_media_datagram_ready, cons_ctx, current_time, loss->media_buffer,
                loss->group_id, loss->object_id, loss->offset, 0, 0, 0, loss->object_length, loss->length);
            if (ret == quicrq_consumer_finished) {
                consumer_properly_finished = 1;
                ret = 0;
                break;
            }
            else if (ret != 0) {
                DBG_PRINTF("Media consumer callback: ret = %d", ret);
            }
            loss = loss->next_loss;
        }
    }

    if (ret == 0 && !consumer_properly_finished) {
        ret = -1;
        DBG_PRINTF("Consumer not properly finished, ret=%d", ret);
    }

    /* Close publisher */
    if (pub_ctx != NULL) {
        test_media_publisher_close(pub_ctx);
    }

    /* Close consumer */
    if (cons_ctx != NULL) {
        test_media_consumer_close(cons_ctx);
    }

    /* Free the memory allocated to losses.*/
    while (first_loss != NULL) {
        media_disorder_hole_t* loss = first_loss;
        first_loss = loss->next_loss;
        free(loss);
    }

    /* Compare media result to media source */

    if (ret == 0) {
        ret = quicrq_compare_media_file(media_result_file, media_source_path);
    }

    return ret;
}

int quicrq_media_object_noloss()
{
    int ret = quicrq_media_datagram_test_one(QUICRQ_TEST_VIDEO1_SOURCE, QUICRQ_TEST_MEDIA_object_RESULT, QUICRQ_TEST_MEDIA_object_LOG,
        0, NULL, NULL, 0);
    return ret;
}

int quicrq_media_object_loss()
{
    uint64_t loss_object[] = { 0, 4, 5, 6, 9, 11, 15, UINT64_MAX };
    int loss_mode[] = { 3, 3, 3, 3, 0,  1, 2,  3 };

    int ret = quicrq_media_datagram_test_one(QUICRQ_TEST_VIDEO1_SOURCE, QUICRQ_TEST_MEDIA_object_LOSS_RESULT, QUICRQ_TEST_MEDIA_object_LOSS_LOG,
        sizeof(loss_object) / sizeof(uint64_t), loss_object, loss_mode, 0);

    return ret;
}
