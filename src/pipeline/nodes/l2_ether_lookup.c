/*
 * l2_ether_lookup.c
 *
 * Copyright (c) 2017-2018, AT&T Intellectual Property.  All rights reserved.
 * Copyright (c) 2016, 2017 by Brocade Communications Systems, Inc.
 * All rights reserved.
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 */
#include <netinet/in.h>
#include <linux/if.h>
#include <rte_branch_prediction.h>
#include <rte_ether.h>
#include <rte_mbuf.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "compiler.h"
#include "ether.h"
#include "if_var.h"
#include "macvlan.h"
#include "main.h"
#include "pktmbuf.h"
#include "pl_common.h"
#include "pl_fused.h"
#include "pl_node.h"
#include "pl_nodes_common.h"
#include "util.h"

struct pl_node;

static inline struct pl_node *ifp_to_ether_lookup_node(struct ifnet *ifp)
{
	/* our imaginary node */
	return (struct pl_node *)ifp;
}

static inline struct ifnet *ether_lookup_node_to_ifp(struct pl_node *node)
{
	/* the node is a fiction of our imagination */
	return (struct ifnet *)node;
}

/*
 * Lookup vlan id on interface.
 * Returns vlan interface pointer or
 *  NULL if case of vlan id is not used (and mbuf is consumed).
 */
static struct ifnet *vlan_lookup(struct ifnet *ifp,
				 struct rte_mbuf *m, uint16_t vid)
{
	uint16_t lcore_id = dp_lcore_id();
	struct if_data *ifstat = &ifp->if_data[lcore_id];
	++ifstat->ifi_ivlan;

	ifp = if_vlan_lookup(ifp, vid);
	if (!ifp)
		goto no_vlan;

	if (!(ifp->if_flags & IFF_UP))
		goto drop;

	if_incr_in(ifp, m);

	/* q-in-q */
	if (ifp->qinq_outer) {
		ifstat = &ifp->if_data[lcore_id];
		++ifstat->ifi_ivlan;

		vid = vid_from_pkt(m, ETHER_TYPE_VLAN);

		ifp = if_vlan_lookup(ifp, vid);
		if (!ifp)
			goto no_vlan;

		if (!(ifp->if_flags & IFF_UP))
			goto drop;

		if_incr_in(ifp, m);
		vid_decap(m, ETHER_TYPE_VLAN);
	}

	return ifp;

drop: __cold_label;
	rte_pktmbuf_free(m);
	return NULL;

no_vlan: __cold_label;
	++ifstat->ifi_no_vlan;
	goto drop;
}

/* Identify interface to receive packet on
 * We support maximum three layers of interface nesting:
 *    dp0portx [vlan ] [macvlan]
 */
ALWAYS_INLINE unsigned int
ether_lookup_process_common(struct pl_packet *pkt, enum pl_mode mode)
{
	struct rte_mbuf *m = pkt->mbuf;
	struct ifnet *ifp = pkt->in_ifp;
	const struct ether_hdr *eth;
	struct if_data *ifstat;

	switch (mode) {
	case PL_MODE_FUSED:
		if (!pipeline_fused_ether_lookup_features(
			    pkt, ifp_to_ether_lookup_node(ifp)))
			return ETHER_LOOKUP_FINISH;
		break;
	case PL_MODE_FUSED_NO_DYN_FEATS:
		if (!pipeline_fused_ether_lookup_no_dyn_features(
			    pkt, ifp_to_ether_lookup_node(ifp)))
			return ETHER_LOOKUP_FINISH;
		break;
	case PL_MODE_REGULAR:
		if (!pl_node_invoke_enabled_features(
			    ether_lookup_node_ptr,
			    ifp_to_ether_lookup_node(ifp),
			    pkt))
			return ETHER_LOOKUP_FINISH;
		break;
	}

	eth = ethhdr(m);
	if (unlikely(is_multicast_ether_addr(&eth->d_addr))) {
		ifstat = &ifp->if_data[dp_lcore_id()];
		ifstat->ifi_imulticast++;

		macvlan_flood(ifp, m);

		if (is_broadcast_ether_addr(&eth->d_addr)) {
			pkt->l2_pkt_type = L2_PKT_BROADCAST;
			pkt_mbuf_set_l2_traffic_type(pkt->mbuf,
						     L2_PKT_BROADCAST);
		} else {
			pkt->l2_pkt_type = L2_PKT_MULTICAST;
			pkt_mbuf_set_l2_traffic_type(pkt->mbuf,
						     L2_PKT_MULTICAST);
		}
	} else {
		pkt->l2_pkt_type = L2_PKT_UNICAST;

		if (unlikely(!ether_addr_equal(&ifp->eth_addr, &eth->d_addr)) &&
		    (!(m->ol_flags & PKT_RX_VLAN) ||
		     !ifp->if_vlantbl)) {
			struct ifnet *macvlan_ifp;

			/* Does not match primary ethernet address -
			 * check virtual intf (!ifp->if_vlantbl) OR
			 * has no vlan tag on it
			 */
			macvlan_ifp = macvlan_input(ifp, m);
			if (!macvlan_ifp)
				goto no_address;

			ifp = pkt->in_ifp = macvlan_ifp;

			/* Account for packet received on macvlan */
			if_incr_in(ifp, m);
		}
	}

	if (m->ol_flags & PKT_RX_VLAN && ifp->if_type != IFT_L2VLAN) {
		uint16_t vid = m->vlan_tci & VLAN_VID_MASK;

		if (vid != 0) {
			ifp = vlan_lookup(ifp, m, vid);
			if (!ifp)
				return ETHER_LOOKUP_FINISH;
			pkt->in_ifp = ifp;
			return ETHER_LOOKUP_LOOKUP;
		}
	}

	if (unlikely(!(ifp->if_flags & IFF_UP))) {
		rte_pktmbuf_free(m);
		goto no_address;
	}

	return ETHER_LOOKUP_ACCEPT;

no_address: __cold_label;
	ifstat = &ifp->if_data[dp_lcore_id()];
	++ifstat->ifi_no_address;
	return ETHER_LOOKUP_FINISH;
}

ALWAYS_INLINE unsigned int
ether_lookup_process(struct pl_packet *p)
{
	return ether_lookup_process_common(p, PL_MODE_REGULAR);
}

static int
ether_lookup_feat_change(struct pl_node *node,
			 struct pl_feature_registration *feat,
			 enum pl_node_feat_action action)
{
	struct ifnet *ifp = ether_lookup_node_to_ifp(node);

	return pl_node_feat_change_u16(&ifp->ether_in_features, feat, action);
}

ALWAYS_INLINE bool
ether_lookup_feat_iterate(struct pl_node *node, bool first,
			  unsigned int *feature_id, void **context)
{
	struct ifnet *ifp = ether_lookup_node_to_ifp(node);

	return pl_node_feat_iterate_u16(&ifp->ether_in_features, first,
					feature_id, context);
}

static struct pl_node *
ether_lookup_node_lookup(const char *name)
{
	struct ifnet *ifp = ifnet_byifname(name);
	return ifp ? ifp_to_ether_lookup_node(ifp) : NULL;
}

/* Register Node */
PL_REGISTER_NODE(ether_lookup_node) = {
	.name = "vyatta:ether-lookup",
	.type = PL_PROC,
	.handler = ether_lookup_process,
	.feat_change = ether_lookup_feat_change,
	.feat_iterate = ether_lookup_feat_iterate,
	.lookup_by_name = ether_lookup_node_lookup,
	.num_next = ETHER_LOOKUP_NUM,
	.next = {
		[ETHER_LOOKUP_ACCEPT] = "ether-forward",
		[ETHER_LOOKUP_FINISH] = "term-finish",
		[ETHER_LOOKUP_LOOKUP] = "ether-lookup",
	}
};

struct pl_node_registration *const ether_lookup_node_ptr =
	&ether_lookup_node;
