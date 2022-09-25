#include "pch.h"
#include "CppUnitTest.h"
#include "quicrq_tests.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace UnitTest
{
	TEST_CLASS(UnitTest)
	{
	public:

		TEST_METHOD(basic)
		{
			int ret = quicrq_basic_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(basic_rt)
		{
			int ret = quicrq_basic_rt_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(congestion_basic)
		{
			int ret = quicrq_congestion_basic_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(congestion_basic_recv)
		{
			int ret = quicrq_congestion_basic_recv_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(congestion_basic_loss)
		{
			int ret = quicrq_congestion_basic_loss_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(congestion_datagram)
		{
			int ret = quicrq_congestion_datagram_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(congestion_datagram_loss)
		{
			int ret = quicrq_congestion_datagram_loss_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(congestion_datagram_recv)
		{
			int ret = quicrq_congestion_datagram_recv_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(congestion_datagram_rloss)
		{
			int ret = quicrq_congestion_datagram_rloss_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(media_video1)
		{
			int ret = quicrq_media_video1_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(media_video1_rt)
		{
			int ret = quicrq_media_video1_rt_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(media_audio1)
		{
			int ret = quicrq_media_audio1_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(media_object_noloss)
		{
			int ret = quicrq_media_object_noloss();

			Assert::AreEqual(ret, 0);
		}
		TEST_METHOD(media_object_loss)
		{
			int ret = quicrq_media_object_loss();

			Assert::AreEqual(ret, 0);
		}
		TEST_METHOD(datagram_basic)
		{
			int ret = quicrq_datagram_basic_test();

			Assert::AreEqual(ret, 0);
		}
		TEST_METHOD(datagram_loss)
		{
			int ret = quicrq_datagram_loss_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(datagram_extra)
		{
			int ret = quicrq_datagram_extra_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(basic_client) {
			int ret = quicrq_basic_client_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(datagram_client) {
			int ret = quicrq_datagram_client_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(datagram_limit) {
			int ret = quicrq_datagram_limit_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(twomedia)
		{
			int ret = quicrq_twomedia_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(twomedia_datagram)
		{
			int ret = quicrq_twomedia_datagram_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(twomedia_datagram_loss)
		{
			int ret = quicrq_twomedia_datagram_loss_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(twomedia_client)
		{
			int ret = quicrq_twomedia_client_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(twomedia_datagram_client)
		{
			int ret = quicrq_twomedia_datagram_client_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(twomedia_datagram_client_loss)
		{
			int ret = quicrq_twomedia_datagram_client_loss_test();

			Assert::AreEqual(ret, 0);
		}
		TEST_METHOD(proto_msg) {
			int ret = proto_msg_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(relay_basic) {
			int ret = quicrq_relay_basic_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(relay_datagram) {
			int ret = quicrq_relay_datagram_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(relay_datagram_loss) {
			int ret = quicrq_relay_datagram_loss_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(relay_basic_client) {
			int ret = quicrq_relay_basic_client_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(relay_datagram_client) {
			int ret = quicrq_relay_datagram_client_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(subscribe_basic) {
			int ret = quicrq_subscribe_basic_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(subscribe_client) {
			int ret = quicrq_subscribe_client_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(subscribe_datagram) {
			int ret = quicrq_subscribe_datagram_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(subscribe_relay1) {
			int ret = quicrq_subscribe_relay1_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(subscribe_relay2) {
			int ret = quicrq_subscribe_relay2_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(subscribe_relay3) {
			int ret = quicrq_subscribe_relay3_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(triangle_basic) {
			int ret = quicrq_triangle_basic_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(triangle_basic_loss) {
			int ret = quicrq_triangle_basic_loss_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(triangle_datagram) {
			int ret = quicrq_triangle_datagram_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(triangle_datagram_loss) {
			int ret = quicrq_triangle_datagram_loss_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(triangle_datagram_extra) {
			int ret = quicrq_triangle_datagram_extra_test();

			Assert::AreEqual(ret, 0);
		}

#if 0
		/* Start point function needs to be revised after "subscriber intent" */
		TEST_METHOD(triangle_start_point) {
			int ret = quicrq_triangle_start_point_test();

			Assert::AreEqual(ret, 0);
		}
#endif
		TEST_METHOD(triangle_cache) {
			int ret = quicrq_triangle_cache_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(triangle_cache_loss) {
			int ret = quicrq_triangle_cache_loss_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(triangle_cache_stream) {
			int ret = quicrq_triangle_cache_stream_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(triangle_intent) {
			int ret = quicrq_triangle_intent_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(triangle_intent_datagram) {
			int ret = quicrq_triangle_intent_datagram_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(triangle_intent_loss) {
			int ret = quicrq_triangle_intent_loss_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(triangle_intent_next) {
			int ret = quicrq_triangle_intent_next_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(pyramid_basic) {
			int ret = quicrq_pyramid_basic_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(pyramid_datagram) {
			int ret = quicrq_pyramid_datagram_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(pyramid_datagram_loss) {
			int ret = quicrq_pyramid_datagram_loss_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(pyramid_datagram_client) {
			int ret = quicrq_pyramid_datagram_client_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(pyramid_datagram_delay) {
			int ret = quicrq_pyramid_datagram_delay_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(pyramid_publish_delay) {
			int ret = quicrq_pyramid_publish_delay_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(twoways_basic) {
			int ret = quicrq_twoways_basic_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(twoways_datagram) {
			int ret = quicrq_twoways_datagram_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(twoways_datagram_loss) {
			int ret = quicrq_twoways_datagram_loss_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(threelegs_basic) {
			int ret = quicrq_threelegs_basic_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(threelegs_datagram) {
			int ret = quicrq_threelegs_datagram_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(threelegs_datagram_loss) {
			int ret = quicrq_threelegs_datagram_loss_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(fourlegs_basic) {
			int ret = quicrq_fourlegs_basic_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(fourlegs_basic_last) {
			int ret = quicrq_fourlegs_basic_last_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(fourlegs_datagram) {
			int ret = quicrq_fourlegs_datagram_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(fourlegs_datagram_last) {
			int ret = quicrq_fourlegs_datagram_last_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(fourlegs_datagram_loss) {
			int ret = quicrq_fourlegs_datagram_loss_test();

			Assert::AreEqual(ret, 0);
		}

		TEST_METHOD(fragment_cache_fill) {
			int ret = quicrq_fragment_cache_fill_test();

			Assert::AreEqual(ret, 0);
		}
		TEST_METHOD(get_addr) {
			int ret = quicrq_get_addr_test();

			Assert::AreEqual(ret, 0);
		}
	};
}
