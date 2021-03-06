/*
 * pl_fused.h
 *
 *
 * Copyright (c) 2017-2019, AT&T Intellectual Property.  All rights reserved.
 * Copyright (c) 2016, 2017 by Brocade Communications Systems, Inc.
 * All rights reserved.
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 */
#ifndef PL_FUSED_H
#define PL_FUSED_H

#include "pl_fused_gen.h"

/*
 * Fused-mode feature ids. This need to be enumerated in the same
 * order as the graph generated by the visit_[before|after] property
 * on each feature node
 */
enum pl_ether_lookup_fused_feat {
	PL_ETHER_LOOKUP_FUSED_FEAT_HW_HDR = 1,
	PL_ETHER_LOOKUP_FUSED_FEAT_SW_VLAN = 2,
	PL_ETHER_LOOKUP_FUSED_FEAT_CAPTURE = 3,
	PL_ETHER_LOOKUP_FUSED_FEAT_PORTMONITOR = 4,
	PL_ETHER_LOOKUP_FUSED_FEAT_VLAN_MOD_INGRESS = 5,
	PL_ETHER_LOOKUP_FUSED_FEAT_BRIDGE = 6,
	PL_ETHER_LOOKUP_FUSED_FEAT_CROSS_CONNECT = 7,
	PL_ETHER_LOOKUP_FUSED_FEAT_FLOW_CAPTURE = 8,
};

enum pl_l3_v4_in_fused_feat {
	PL_L3_V4_IN_FUSED_FEAT_RPF = 1,
	PL_L3_V4_IN_FUSED_FEAT_ACL,
	PL_L3_V4_IN_FUSED_FEAT_TCP_MSS,
	PL_L3_V4_IN_FUSED_FEAT_DEFRAG,
	PL_L3_V4_IN_FUSED_FEAT_FW,
	PL_L3_V4_IN_FUSED_FEAT_CGNAT,
	PL_L3_V4_IN_FUSED_FEAT_DPI,
	PL_L3_V4_IN_FUSED_FEAT_PBR,
	/*
	 * no-address feature should be near the end to give other
	 * features a chance to see the packet first.
	 */
	PL_L3_V4_IN_FUSED_FEAT_NO_ADDRESS = 15,
	PL_L3_V4_IN_FUSED_FEAT_NO_FORWARDING = 16,
};

enum pl_l3_v6_in_fused_feat {
	PL_L3_V6_IN_FUSED_FEAT_ACL = 1,
	PL_L3_V6_IN_FUSED_FEAT_TCP_MSS,
	PL_L3_V6_IN_FUSED_FEAT_DEFRAG,
	PL_L3_V6_IN_FUSED_FEAT_FW,
	PL_L3_V6_IN_FUSED_FEAT_NPTV6,
	PL_L3_V6_IN_FUSED_FEAT_DPI,
	PL_L3_V6_IN_FUSED_FEAT_PBR,
	/*
	 * no-address feature should be near the end to give other
	 * features a chance to see the packet first.
	 */
	PL_L3_V6_IN_FUSED_FEAT_NO_ADDRESS = 15,
	PL_L3_V6_IN_FUSED_FEAT_NO_FORWARDING = 16,
};

enum pl_l3_v4_out_fused_feat {
	PL_L3_V4_OUT_FUSED_FEAT_DEFRAG = 1,
	PL_L3_V4_OUT_FUSED_FEAT_CGNAT,
	PL_L3_V4_OUT_FUSED_FEAT_FW,
	PL_L3_V4_OUT_FUSED_FEAT_DPI,
	PL_L3_V4_OUT_FUSED_FEAT_TCP_MSS,
};

enum pl_l3_v6_out_fused_feat {
	PL_L3_V6_OUT_FUSED_FEAT_NPTV6 = 1,
	PL_L3_V6_OUT_FUSED_FEAT_DEFRAG,
	PL_L3_V6_OUT_FUSED_FEAT_FW,
	PL_L3_V6_OUT_FUSED_FEAT_DPI,
	PL_L3_V6_OUT_FUSED_FEAT_TCP_MSS,
};

enum pl_l3_v4_encap_fused_feat {
	PL_L3_V4_ENCAP_FUSED_FEAT_ACL = 1,
};

enum pl_l3_v6_encap_fused_feat {
	PL_L3_V6_ENCAP_FUSED_FEAT_ACL = 1,
};

enum pl_l3_v4_route_lookup_fused_feat {
	PL_L3_V4_ROUTE_LOOKUP_FUSED_FEAT_IPSEC = 1,
};

enum pl_l3_v6_route_lookup_fused_feat {
	PL_L3_V6_ROUTE_LOOKUP_FUSED_FEAT_IPSEC = 1,
};

#endif /* PL_FUSED_H */
