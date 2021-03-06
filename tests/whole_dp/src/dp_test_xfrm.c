/*-
 * Copyright (c) 2017-2018, AT&T Intellectual Property.  All rights reserved.
 * Copyright (c) 2015-2016 by Brocade Communications Systems, Inc.
 * All rights reserved.
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 * XFRM tests
 */

#include "dp_test.h"
#include "dp_test_lib.h"
#include "dp_test_netlink_state.h"
#include "dp_test_crypto_utils.h"

#include "crypto/crypto_forward.h"

#define LOCAL_ADDRESS "10.10.2.2"
#define NETWORK_LOCAL "10.10.1.0/24"
#define PEER_ADDRESS "10.10.2.3"
#define NETWORK_REMOTE "10.10.3.0/24"
#define TUNNEL_REQID 1234
#define TUNNEL_PRIORITY 2048
#define SPI_OUTBOUND 0x10
#define SPI_INBOUND 0x11
#define TEST_VRF 42

DP_DECL_TEST_SUITE(xfrm_suite);

DP_DECL_TEST_CASE(xfrm_suite, xfrm_policy, NULL, NULL);

/* can we create two policies? */
DP_START_TEST(xfrm_policy, create_two_policies)
{
	struct dp_test_crypto_policy output_policy = {
		.d_prefix = NETWORK_REMOTE,
		.s_prefix = NETWORK_LOCAL,
		.proto = IPPROTO_ICMP,
		.dst = PEER_ADDRESS,
		.dst_family = AF_INET,
		.dir = XFRM_POLICY_OUT,
		.family = AF_INET,
		.reqid = TUNNEL_REQID,
		.priority = TUNNEL_PRIORITY,
		.mark = 0,
		.vrfid = VRF_DEFAULT_ID
	};

	struct dp_test_crypto_policy input_policy = {
		.d_prefix = NETWORK_LOCAL,
		.s_prefix = NETWORK_REMOTE,
		.proto = IPPROTO_ICMP,
		.dst = LOCAL_ADDRESS,
		.dst_family = AF_INET,
		.dir = XFRM_POLICY_IN,
		.family = AF_INET,
		.reqid = TUNNEL_REQID,
		.priority = TUNNEL_PRIORITY,
		.mark = 0,
		.vrfid = VRF_DEFAULT_ID
	};

	dp_test_check_state_show("ipsec spd", "\"ipv4\": 0", false);

	dp_test_crypto_create_policy(&input_policy);
	dp_test_crypto_create_policy(&output_policy);

	dp_test_check_state_show("ipsec spd", "\"ipv4\": 2", false);

	dp_test_crypto_delete_policy(&input_policy);
	dp_test_crypto_delete_policy(&output_policy);

	dp_test_check_state_show("ipsec spd", "\"ipv4\": 0", false);

} DP_END_TEST;

DP_START_TEST(xfrm_policy, create_two_policies_vrf)
{
	vrfid_t vrfid;
	char cmd_str[100];
	struct dp_test_crypto_policy output_policy = {
		.d_prefix = NETWORK_REMOTE,
		.s_prefix = NETWORK_LOCAL,
		.proto = IPPROTO_ICMP,
		.dst = PEER_ADDRESS,
		.dst_family = AF_INET,
		.dir = XFRM_POLICY_OUT,
		.family = AF_INET,
		.reqid = TUNNEL_REQID,
		.priority = TUNNEL_PRIORITY,
		.mark = 0,
		.vrfid = TEST_VRF
	};

	struct dp_test_crypto_policy input_policy = {
		.d_prefix = NETWORK_LOCAL,
		.s_prefix = NETWORK_REMOTE,
		.proto = IPPROTO_ICMP,
		.dst = LOCAL_ADDRESS,
		.dst_family = AF_INET,
		.dir = XFRM_POLICY_IN,
		.family = AF_INET,
		.reqid = TUNNEL_REQID,
		.priority = TUNNEL_PRIORITY,
		.mark = 0,
		.vrfid = TEST_VRF
	};

	dp_test_check_state_show("ipsec spd", "\"ipv4\": 0", false);

	dp_test_netlink_add_vrf(TEST_VRF, 1);

	/* Create VRF context via interface */
	dp_test_nl_add_ip_addr_and_connected_vrf("dp1T0", "16.1.1.1/24",
						 TEST_VRF);
	dp_test_crypto_create_policy(&input_policy);
	dp_test_crypto_create_policy(&output_policy);

	dp_test_check_state_show("ipsec spd", "\"ipv4\": 0", false);
	vrfid = dp_test_translate_vrf_id(TEST_VRF);
	snprintf(cmd_str, sizeof(cmd_str), "ipsec spd vrf_id %d", vrfid);
	dp_test_check_state_show(cmd_str, "\"ipv4\": 2", false);

	dp_test_crypto_delete_policy(&input_policy);
	dp_test_crypto_delete_policy(&output_policy);

	dp_test_check_state_show(cmd_str, "\"ipv4\": 0", false);
	dp_test_nl_del_ip_addr_and_connected_vrf("dp1T0", "16.1.1.1/24",
						 TEST_VRF);
	dp_test_netlink_del_vrf(TEST_VRF, 0);

} DP_END_TEST;

DP_DECL_TEST_CASE(xfrm_suite, xfrm_sa, NULL, NULL);

/* can we create to SAs with only key and hmac args? */
DP_START_TEST(xfrm_sa, create_two_sas_crypto_only)
{
	const unsigned char crypto_key_128[] = {
		0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
		0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};

	const unsigned char auth_key[] = {
		0x0b, 0x1b, 0x2b, 0x3b, 0x4b, 0x5b, 0x6b, 0x7b,
		0x8b, 0x9b, 0xab, 0xbb, 0xcb, 0xeb, 0xfb, 0x1c,
		0x2c, 0x3c, 0x4c, 0x5c};

	struct dp_test_crypto_sa output_sa = {
		.cipher_algo = CRYPTO_CIPHER_AES_CBC,
		.cipher_key = crypto_key_128,
		.cipher_key_len = (sizeof(crypto_key_128) * 8),
		.auth_algo = CRYPTO_AUTH_HMAC_SHA1,
		.auth_key = auth_key,
		.auth_key_len = (sizeof(auth_key) * 8),
		.spi = SPI_OUTBOUND,
		.d_addr = PEER_ADDRESS,
		.s_addr = LOCAL_ADDRESS,
		.family = AF_INET,
		.mode = XFRM_MODE_TUNNEL,
		.reqid = TUNNEL_REQID,
		.mark = 0,
		.vrfid = VRF_DEFAULT_ID
	};

	struct dp_test_crypto_sa input_sa = {
		.cipher_algo = CRYPTO_CIPHER_AES_CBC,
		.cipher_key = crypto_key_128,
		.cipher_key_len = (sizeof(crypto_key_128) * 8),
		.auth_algo = CRYPTO_AUTH_HMAC_SHA1,
		.auth_key = auth_key,
		.auth_key_len = (sizeof(auth_key) * 8),
		.spi = SPI_INBOUND,
		.d_addr = LOCAL_ADDRESS,
		.s_addr = PEER_ADDRESS,
		.family = AF_INET,
		.mode = XFRM_MODE_TUNNEL,
		.reqid = TUNNEL_REQID,
		.mark = 0,
		.vrfid = VRF_DEFAULT_ID
	};

	dp_test_crypto_check_sa_count(VRF_DEFAULT_ID, 0);

	dp_test_crypto_create_sa(&input_sa);
	dp_test_crypto_create_sa(&output_sa);

	dp_test_check_state_show("ipsec sad",
				 "\"cipher\": \"CBS(AES) 128\",\n"
				 "            \"digest\": \"hmac(sha1)\"",
				 false);
	dp_test_crypto_check_sa_count(VRF_DEFAULT_ID, 2);

	dp_test_crypto_delete_sa(&input_sa);
	dp_test_crypto_delete_sa(&output_sa);

	dp_test_crypto_check_sa_count(VRF_DEFAULT_ID, 0);

}  DP_END_TEST;

/*
 * sa_expire: Check that an XFRM_MSG_EXPIRE message removes an SA
 * from the dataplane if 'hard' is true, but not if it's false.
 */
DP_START_TEST(xfrm_sa, sa_expire)
{
	struct dp_test_crypto_sa output_sa = {
		/* Use default algorithms */
		.spi = SPI_OUTBOUND,
		.d_addr = PEER_ADDRESS,
		.s_addr = LOCAL_ADDRESS,
		.family = AF_INET,
		.mode = XFRM_MODE_TUNNEL,
		.reqid = TUNNEL_REQID,
		.mark = 0,
		.vrfid = VRF_DEFAULT_ID
	};

	struct dp_test_crypto_sa input_sa = {
		/* Use default algorithms */
		.spi = SPI_INBOUND,
		.d_addr = LOCAL_ADDRESS,
		.s_addr = PEER_ADDRESS,
		.family = AF_INET,
		.mode = XFRM_MODE_TUNNEL,
		.reqid = TUNNEL_REQID,
		.mark = 0,
		.vrfid = VRF_DEFAULT_ID
	};

	dp_test_crypto_check_sa_count(VRF_DEFAULT_ID, 0);

	dp_test_crypto_create_sa(&input_sa);
	dp_test_crypto_create_sa(&output_sa);

	dp_test_crypto_check_sa_count(VRF_DEFAULT_ID, 2);

	/* Soft expire should not remove the SAs */
	dp_test_crypto_expire_sa(&input_sa, false);
	dp_test_crypto_check_sa_count(VRF_DEFAULT_ID, 2);

	/* Hard expire should delete the SA */
	dp_test_crypto_expire_sa(&output_sa, true);
	dp_test_crypto_check_sa_count(VRF_DEFAULT_ID, 1);

	dp_test_crypto_delete_sa(&input_sa);

	dp_test_crypto_check_sa_count(VRF_DEFAULT_ID, 0);

}  DP_END_TEST;
