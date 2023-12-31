#ifdef _WINDOWS
#include "getopt.h"
#endif
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "quicrq_tests.h"
#include "picoquic_utils.h"
#include "quicrq.h"

typedef struct st_quicrq_test_def_t {
    char const* test_name;
    int (*test_fn)();
} quicrq_test_def_t;

typedef enum {
    test_not_run = 0,
    test_excluded,
    test_success,
    test_failed
} test_status_t;

static const quicrq_test_def_t test_table[] =
{
    { "proto_msg", proto_msg_test},
    { "basic", quicrq_basic_test },
    { "basic_rt", quicrq_basic_rt_test },
    { "congestion_basic", quicrq_congestion_basic_test },
    { "congestion_basic_half", quicrq_congestion_basic_half_test },
    { "congestion_basic_recv", quicrq_congestion_basic_recv_test },
    { "congestion_basic_loss", quicrq_congestion_basic_loss_test },
    { "congestion_basic_zero", quicrq_congestion_basic_zero_test },
    { "congestion_basic_g", quicrq_congestion_basic_g_test },
    { "congestion_datagram", quicrq_congestion_datagram_test },
    { "congestion_datagram_half", quicrq_congestion_datagram_half_test },
    { "congestion_datagram_loss", quicrq_congestion_datagram_loss_test },
    { "congestion_datagram_recv", quicrq_congestion_datagram_recv_test },
    { "congestion_datagram_rloss", quicrq_congestion_datagram_rloss_test },
    { "congestion_datagram_zero", quicrq_congestion_datagram_zero_test },
    { "congestion_datagram_g", quicrq_congestion_datagram_g_test },
    { "media_video1", quicrq_media_video1_test },
    { "media_video1_rt", quicrq_media_video1_rt_test },
    { "media_audio1", quicrq_media_audio1_test },
    { "datagram_basic", quicrq_datagram_basic_test },
    { "datagram_loss", quicrq_datagram_loss_test },
    { "datagram_extra", quicrq_datagram_extra_test },
    { "basic_client", quicrq_basic_client_test },
    { "datagram_client", quicrq_datagram_client_test },
    { "datagram_limit", quicrq_datagram_limit_test },
    { "datagram_unsubscribe", quicrq_datagram_unsubscribe_test },
    { "twomedia", quicrq_twomedia_test },
    { "twomedia_datagram", quicrq_twomedia_datagram_test },
    { "twomedia_datagram_loss", quicrq_twomedia_datagram_loss_test },
    { "twomedia_client", quicrq_twomedia_client_test },
    { "twomedia_datagram_client", quicrq_twomedia_datagram_client_test },
    { "twomedia_datagram_client_loss", quicrq_twomedia_datagram_client_loss_test },
    { "media_object_no_loss", quicrq_media_object_noloss },
    { "media_object_loss", quicrq_media_object_loss },
    { "relay_basic", quicrq_relay_basic_test },
    { "relay_datagram", quicrq_relay_datagram_test },
    { "relay_datagram_loss", quicrq_relay_datagram_loss_test },
    { "relay_basic_client", quicrq_relay_basic_client_test },
    { "relay_datagram_client", quicrq_relay_datagram_client_test },
    { "subscribe_basic", quicrq_subscribe_basic_test },
    { "subscribe_client", quicrq_subscribe_client_test },
    { "subscribe_datagram", quicrq_subscribe_datagram_test },
    { "subscribe_relay1", quicrq_subscribe_relay1_test },
    { "subscribe_relay2", quicrq_subscribe_relay2_test },
    { "subscribe_relay3", quicrq_subscribe_relay3_test },
    { "triangle_basic", quicrq_triangle_basic_test },
    { "triangle_basic_loss", quicrq_triangle_basic_loss_test },
    { "triangle_datagram", quicrq_triangle_datagram_test },
    { "triangle_datagram_loss", quicrq_triangle_datagram_loss_test },
    { "triangle_datagram_extra", quicrq_triangle_datagram_extra_test },
    { "triangle_start_point", quicrq_triangle_start_point_test },
    { "triangle_start_point_s", quicrq_triangle_start_point_s_test },
    { "triangle_start_point_w", quicrq_triangle_start_point_w_test },
    { "triangle_cache", quicrq_triangle_cache_test },
    { "triangle_cache_loss", quicrq_triangle_cache_loss_test },
    { "triangle_cache_stream", quicrq_triangle_cache_stream_test },
    { "triangle_intent", quicrq_triangle_intent_test },
    { "triangle_intent_nc", quicrq_triangle_intent_nc_test },
    { "triangle_intent_datagram", quicrq_triangle_intent_datagram_test },
    { "triangle_intent_dg_nc", quicrq_triangle_intent_dg_nc_test },
    { "triangle_intent_loss", quicrq_triangle_intent_loss_test },
    { "triangle_intent_next", quicrq_triangle_intent_next_test },
    { "triangle_intent_next_s", quicrq_triangle_intent_next_s_test },
    { "triangle_intent_that", quicrq_triangle_intent_that_test },
    { "triangle_intent_that_s", quicrq_triangle_intent_that_s_test },
    { "triangle_intent_warp", quicrq_triangle_intent_warp_test },
    { "triangle_intent_warp_nc", quicrq_triangle_intent_warp_nc_test },
    { "triangle_intent_warp_loss", quicrq_triangle_intent_warp_loss_test },
    { "triangle_intent_warp_next", quicrq_triangle_intent_warp_next_test },
    { "triangle_intent_rush", quicrq_triangle_intent_rush_test },
    { "triangle_intent_rush_nc", quicrq_triangle_intent_rush_nc_test },
    { "triangle_intent_rush_loss", quicrq_triangle_intent_rush_loss_test },
    { "triangle_intent_rush_next", quicrq_triangle_intent_rush_next_test },
    { "pyramid_basic", quicrq_pyramid_basic_test },
    { "pyramid_datagram", quicrq_pyramid_datagram_test },
    { "pyramid_datagram_loss", quicrq_pyramid_datagram_loss_test },
    { "pyramid_datagram_client", quicrq_pyramid_datagram_client_test },
    { "pyramid_datagram_delay", quicrq_pyramid_datagram_delay_test },
    { "pyramid_publish_delay", quicrq_pyramid_publish_delay_test },
    { "twoways_basic", quicrq_twoways_basic_test },
    { "twoways_datagram", quicrq_twoways_datagram_test },
    { "twoways_datagram_loss", quicrq_twoways_datagram_loss_test },
    { "twomedia_tri_stream", quicrq_twomedia_tri_stream_test },
    { "twomedia_tri_datagram", quicrq_twomedia_tri_datagram_test },
    { "twomedia_tri_later", quicrq_twomedia_tri_later_test },
    { "threelegs_basic",  quicrq_threelegs_basic_test },
    { "threelegs_datagram",  quicrq_threelegs_datagram_test },
    { "threelegs_datagram_loss",  quicrq_threelegs_datagram_loss_test },
    { "fourlegs_basic", quicrq_fourlegs_basic_test },
    { "fourlegs_basic_last", quicrq_fourlegs_basic_last_test },
    { "fourlegs_datagram", quicrq_fourlegs_datagram_test },
    { "fourlegs_datagram_last", quicrq_fourlegs_datagram_last_test },
    { "fourlegs_datagram_loss", quicrq_fourlegs_datagram_loss_test },
    { "fragment_cache_fill", quicrq_fragment_cache_fill_test },
    { "get_addr", quicrq_get_addr_test },
    { "warp_basic", quicrq_warp_basic_test },
    { "warp_basic_client", quicrq_warp_basic_client_test },
    { "warp_triangle", quicrq_triangle_warp_test },
    { "congestion_warp", quicrq_congestion_warp_test },
    { "congestion_warp_g", quicrq_congestion_warp_g_test },
    { "congestion_warp_gs", quicrq_congestion_warp_gs_test },
    { "congestion_warp_zero_s", quicrq_congestion_warp_zero_s_test },
    { "warp_relay", quicrq_warp_relay_test },
    { "warp_basic_loss", quicrq_warp_basic_loss_test },
    { "warp_relay_loss", quicrq_warp_relay_loss_test },
    { "rush_basic", quicrq_rush_basic_test },
    { "rush_basic_client", quicrq_rush_basic_client_test },
    { "rush_basic_loss", quicrq_rush_basic_loss_test },
    { "rush_triangle", quicrq_triangle_rush_test },
    { "congestion_rush", quicrq_congestion_rush_test },
    { "congestion_rush_g", quicrq_congestion_rush_g_test },
    { "congestion_rush_gs", quicrq_congestion_rush_gs_test },
    { "congestion_rush_zero_s", quicrq_congestion_rush_zero_s_test }
};

static size_t const nb_tests = sizeof(test_table) / sizeof(quicrq_test_def_t);

static int do_one_test(size_t i, FILE* F)
{
    int ret = 0;

    if (i >= nb_tests) {
        fprintf(F, "Invalid test number %zu\n", i);
        ret = -1;
    }
    else {
        fprintf(F, "Starting test number %zu, %s\n", i, test_table[i].test_name);

        fflush(F);

        ret = test_table[i].test_fn();
        if (ret == 0) {
            fprintf(F, "    Success.\n");
        }
        else {
            fprintf(F, "    Fails, error: %d.\n", ret);
        }
    }

    fflush(F);

    return ret;
}

int usage(char const* argv0)
{
    fprintf(stderr, "QUICRQ test execution\n");
    fprintf(stderr, "\nUsage: %s [test1 [test2 ..[testN]]]\n\n", argv0);
    fprintf(stderr, "   Or: %s [-x test]*", argv0);
    fprintf(stderr, "Valid test names are: \n");
    for (size_t x = 0; x < nb_tests; x++) {
        fprintf(stderr, "    ");

        for (int j = 0; j < 4 && x < nb_tests; j++, x++) {
            fprintf(stderr, "%s, ", test_table[x].test_name);
        }
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "Options: \n");
    fprintf(stderr, "  -x test           Do not run the specified test.\n");
    fprintf(stderr, "  -n                Disable debug prints.\n");
    fprintf(stderr, "  -r                Retry failed tests with debug print enabled.\n");
    fprintf(stderr, "  -h                Print this help message\n");
    fprintf(stderr, "  -S solution_dir   Set the path to the source files to find the default files\n");
    fprintf(stderr, "  -P picoquic_dir   Obsolete, not used anymore.\n");

    return -1;
}

int get_test_number(char const* test_name)
{
    int test_number = -1;

    for (size_t i = 0; i < nb_tests; i++) {
        if (strcmp(test_name, test_table[i].test_name) == 0) {
            test_number = (int)i;
        }
    }

    return test_number;
}

int main(int argc, char** argv)
{
    int ret = 0;
    int nb_test_tried = 0;
    int nb_test_failed = 0;
    test_status_t* test_status = (test_status_t*)calloc(nb_tests, sizeof(test_status_t));
    int opt;
    int disable_debug = 0;
    int retry_failed_test = 0;

    fprintf(stdout, "Testing QUICRQ Version %s, Picoquic version %s\n", QUICRQ_VERSION, PICOQUIC_VERSION);

    if (test_status == NULL)
    {
        fprintf(stderr, "Could not allocate memory.\n");
        ret = -1;
    }
    else
    {
        while (ret == 0 && (opt = getopt(argc, argv, "P:S:x:nrh")) != -1) {
            switch (opt) {
            case 'x': {
                int test_number = get_test_number(optarg);

                if (test_number < 0) {
                    fprintf(stderr, "Incorrect test name: %s\n", optarg);
                    ret = usage(argv[0]);
                }
                else {
                    test_status[test_number] = test_excluded;
                }
                break;
            }
            case 'P':
                break;
            case 'S':
                quicrq_test_solution_dir = optarg;
                break;
            case 'n':
                disable_debug = 1;
                break;
            case 'r':
                retry_failed_test = 1;
                break;
            case 'h':
                usage(argv[0]);
                exit(0);
                break;
            default:
                ret = usage(argv[0]);
                break;
            }
        }

        if (disable_debug) {
            debug_printf_suspend();
        }
        else {
            debug_printf_push_stream(stderr);
            DBG_PRINTF("%s", "Debug print enabled");
        }

        if (ret == 0)
        {
            if (optind >= argc) {
                for (size_t i = 0; i < nb_tests; i++) {
                    if (test_status[i] == test_not_run) {
                        nb_test_tried++;
                        if (do_one_test(i, stdout) != 0) {
                            test_status[i] = test_failed;
                            nb_test_failed++;
                            ret = -1;
                        }
                        else {
                            test_status[i] = test_success;
                        }
                    }
                    else {
                        fprintf(stdout, "Test number %d (%s) is bypassed.\n", (int)i, test_table[i].test_name);
                    }
                }
            }
            else {
                for (int arg_num = optind; arg_num < argc; arg_num++) {
                    int test_number = get_test_number(argv[arg_num]);

                    if (test_number < 0) {
                        fprintf(stderr, "Incorrect test name: %s\n", argv[arg_num]);
                        ret = usage(argv[0]);
                    }
                    else {
                        nb_test_tried++;
                        if (do_one_test(test_number, stdout) != 0) {
                            test_status[test_number] = test_failed;
                            nb_test_failed++;
                            ret = -1;
                        }
                        else if (test_status[test_number] == test_not_run) {
                            test_status[test_number] = test_success;
                        }
                        break;
                    }
                }
            }
        }

        if (nb_test_tried > 1) {
            fprintf(stdout, "Tried %d tests, %d fail%s.\n", nb_test_tried,
                nb_test_failed, (nb_test_failed > 1) ? "" : "s");
        }

        if (nb_test_failed > 0) {
            fprintf(stdout, "Failed test(s): ");
            for (size_t i = 0; i < nb_tests; i++) {
                if (test_status[i] == test_failed) {
                    fprintf(stdout, "%s ", test_table[i].test_name);
                }
            }
            fprintf(stdout, "\n");

            if (disable_debug && retry_failed_test) {
                /* debug_printf_push_stream(stderr); */
                debug_printf_resume();
                fprintf(stdout, "Retrying failed tests.\n");
                ret = 0;
                for (size_t i = 0; i < nb_tests; i++) {
                    if (test_status[i] == test_failed) {
                        fprintf(stdout, "Retrying %s:\n", test_table[i].test_name);
                        if (do_one_test(i, stdout) != 0) {
                            test_status[i] = test_failed;
                            fprintf(stdout, "Test %s: still failing\n", test_table[i].test_name);
                            ret = -1;
                        }
                        else {
                            /* This was a Heisenbug.. */
                            test_status[i] = test_success;
                            fprintf(stdout, "Test %s: passing now.\n", test_table[i].test_name);
                        }
                    }
                }
                if (ret == 0) {
                    fprintf(stdout, "All tests pass after second try.\n");
                }
                else {
                    fprintf(stdout, "Still failing: ");
                    for (size_t i = 0; i < nb_tests; i++) {
                        if (test_status[i] == test_failed) {
                            fprintf(stdout, "%s ", test_table[i].test_name);
                        }
                    }
                    fprintf(stdout, "\n");
                }
            }
        }

        free(test_status);
    }
    return (ret);
}
