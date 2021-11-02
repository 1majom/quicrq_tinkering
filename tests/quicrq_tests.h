/* Declaration of the test functions for the QUICR TEST library
 */
#ifndef QUICRQ_TEST_H
#define QUICRQ_TEST_H

#ifdef __cplusplus
extern "C" {
#endif
    extern char const* quicrq_test_picoquic_solution_dir;
    extern char const* quicrq_test_solution_dir;

    int quicrq_basic_test();
    int proto_msg_test();
    int quicrq_media_video1_test();
    int quicrq_media_video1_rt_test();
    int quicrq_media_source_test();
    int quicrq_media_source_rt_test();
    int quicrq_media_disorder_test();
    int quicrq_basic_rt_test();
    int quicrq_datagram_basic_test();
    int quicrq_datagram_loss_test();
    int quicrq_media_frame_noloss();
    int quicrq_media_frame_loss();

#ifdef __cplusplus
}
#endif
#endif /* QUICRQ_TEST_H */