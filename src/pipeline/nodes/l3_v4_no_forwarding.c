/*
 * IPv4 no forwarding feature
 *
 * Copyright (c) 2018, AT&T Intellectual Property.  All rights reserved.
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 */
#include "pl_common.h"
#include "pl_fused.h"
#include "pl_node.h"
#include "pl_nodes_common.h"

/* Register Features */
PL_REGISTER_FEATURE(ipv4_in_no_forwarding_feat) = {
	.name = "vyatta:ipv4-in-no-forwarding",
	.node_name = "ipv4-route-lookup-host",
	.feature_point = "ipv4-validate",
	.visit_after = "ipv4-in-no-address",
	.id = PL_L3_V4_IN_FUSED_FEAT_NO_FORWARDING,
};
