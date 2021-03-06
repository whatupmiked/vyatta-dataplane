/*-
 * Copyright (c) 2017-2019, AT&T Intellectual Property. All rights reserved.
 * Copyright (c) 2011-2016 by Brocade Communications Systems, Inc.
 * All rights reserved.
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 */

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <linux/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <rte_branch_prediction.h>
#include <rte_common.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_fbk_hash.h>
#include <rte_jhash.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_memory.h>
#include <stdbool.h>
#include <stdint.h>
/*
 * IPv4 route table
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <urcu/uatomic.h>

#include "compiler.h"
#include "compat.h"
#include "dp_event.h"
#include "ecmp.h"
#include "fal.h"
#include "if_var.h"
#include "json_writer.h"
#include "lpm/lpm.h"	/* Use Vyatta modified version */
#include "mpls/mpls.h"
#include "pktmbuf.h"
#include "pd_show.h"
#include "route.h"
#include "urcu.h"
#include "vplane_debug.h"
#include "vplane_log.h"
#include "vrf.h"

struct rte_mbuf;

/*
 * The nexthop in LPM is 22 bits but dpdk hash tables currently have a
 * limit of 2^20 entries.
 */
#define NEXTHOP_HASH_TBL_SIZE RTE_FBK_HASH_ENTRIES_MAX
#define NEXTHOP_HASH_TBL_MIN  (UINT8_MAX + 1)

/* These are stored in a memory pool to allow for mapping
 * index/offset into pointer:
 *
 * addr   +----------+
 *   ---->|          |
 *        |  L P M   | idx +-----------+
 *        |          +---->| nexthop_u |
 *        |          |     +-----------+
 *        |          |     |           |
 *        +----------+     +-----------+
 *                         |  nexthop  |
 *                         |     0     |
 *                         +-----------+
 *                         |    ...    |
 *                         +-----------+
 *                         |  nexthop  |
 *                         | count - 1 |
 *                         +-----------+
 */

/*
 * This is the nexthop information result of route lookup - allows for
 * multiple nexthops in the case of ECMP
 */
struct next_hop_u {
	struct next_hop      *siblings;	/* array of next_hop */
	uint8_t              nsiblings;	/* # of next_hops */
	uint8_t              proto;	/* routing protocol */
	uint32_t             index;
	uint32_t             refcount;	/* # of LPM's referring */
	struct next_hop      hop0;      /* optimization for non-ECMP */
	struct cds_lfht_node nh_node;
	enum pd_obj_state    pd_state;
	fal_object_t         nhg_fal_obj;   /* FAL handle for next_hop_group */
	fal_object_t         *nh_fal_obj; /* Per-nh FAL handles */
	struct rcu_head      rcu;
} __rte_cache_aligned;

struct nexthop_table {
	uint32_t in_use;  /* # of entries used */
	uint32_t rover;   /* next free slot to look at */
	struct next_hop_u *entry[NEXTHOP_HASH_TBL_SIZE]; /* array of entries */
	uint32_t neigh_present;
	uint32_t neigh_created;
};

/* Nexthop entry table, could be per-namespace */
static struct nexthop_table nh_tbl __hot_data;

/* Index for next hop table */
static struct cds_lfht *nexthop_hash;

/* Well-known blackhole next_hop_u for failure cases */
static struct next_hop_u *nextu_blackhole;

struct nexthop_hash_key {
	const struct next_hop *nh;
	size_t		       size;
	uint8_t		       proto;
};

static pthread_mutex_t route_mutex = PTHREAD_MUTEX_INITIALIZER;

static const struct reserved_route {
	in_addr_t addr;
	int prefix_length;
	uint32_t flags;
	int scope;
} reserved_routes[] = {
	{
		.addr = 0,
		.prefix_length = 0,
		.flags = RTF_NOROUTE | RTF_REJECT,
		.scope = LPM_SCOPE_PAN_DIMENSIONAL,
	},
	{
		.addr = 0x7f000000, /* loopback */
		.prefix_length = 8,
		.flags = RTF_BLACKHOLE,
		.scope = RT_SCOPE_HOST,
	},
	{
		.addr = INADDR_BROADCAST,
		.prefix_length = 32,
		.flags = RTF_BROADCAST | RTF_LOCAL,
		.scope = RT_SCOPE_HOST,
	},
};

/* track the state of routes for the show commands */
static uint32_t route_sw_stats[PD_OBJ_STATE_LAST];
static uint32_t route_hw_stats[PD_OBJ_STATE_LAST];

/*
 * Wrapper round the nexthop_new function. This one keeps track of the
 * failures and successes.
 */
static int
route_nexthop_new(const struct next_hop *nh, uint16_t size, uint8_t proto,
		  uint32_t *slot)
{
	int rc;

	rc = nexthop_new(nh, size, proto, slot);
	if (rc >= 0)
		return rc;

	switch (rc) {
	case -ENOSPC:
		route_sw_stats[PD_OBJ_STATE_NO_RESOURCE]++;
		break;
	default:
		route_sw_stats[PD_OBJ_STATE_ERROR]++;
		break;
	}

	return rc;
}

/*
 * Wrapper round the lpm function. This one keeps track of the
 * failures and successes.
 */
static int
route_lpm_add(vrfid_t vrf_id, struct lpm *lpm, uint32_t ip,
	      uint8_t depth, uint32_t next_hop, int16_t scope)
{
	int rc;
	struct pd_obj_state_and_flags *pd_state;
	struct pd_obj_state_and_flags *old_pd_state;
	uint32_t old_nh;
	bool demoted = false;
	struct next_hop_u *nextu =
		rcu_dereference(nh_tbl.entry[next_hop]);
	bool update_pd_state = true;

	rc = lpm_add(lpm, ntohl(ip), depth, next_hop, scope, &pd_state,
			 &old_nh, &old_pd_state);
	switch (rc) {
	case LPM_SUCCESS:
		/* Success */
		route_sw_stats[PD_OBJ_STATE_FULL]++;
		break;
	case LPM_HIGHER_SCOPE_EXISTS:
		/*
		 * Success, but there is a higher scope rule, so this is
		 * not needed in the fal.
		 */
		route_sw_stats[PD_OBJ_STATE_NOT_NEEDED]++;
		pd_state->state = PD_OBJ_STATE_NOT_NEEDED;
		return rc;
	case LPM_LOWER_SCOPE_EXISTS:
		/* Added, but lower scope route was demoted. */
		demoted = true;
		route_sw_stats[PD_OBJ_STATE_NOT_NEEDED]++;
		break;
	case LPM_ALREADY_EXISTS:
		/* Added, but already existed. */
		return 0;
	case -ENOSPC:
		route_sw_stats[PD_OBJ_STATE_NO_RESOURCE]++;
		return rc;
	default:
		route_sw_stats[PD_OBJ_STATE_ERROR]++;
		return rc;
	}

	if (nextu->pd_state != PD_OBJ_STATE_FULL) {
		pd_state->state = nextu->pd_state;
		nextu = nextu_blackhole;
		update_pd_state = false;
	}

	if (demoted) {
		if (old_pd_state->created) {
			rc = fal_ip4_upd_route(vrf_id, ip, depth,
					       lpm_get_id(lpm),
					       nextu->siblings,
					       nextu->nsiblings,
					       nextu->nhg_fal_obj);
		} else {
			rc = fal_ip4_new_route(vrf_id, ip, depth,
					       lpm_get_id(lpm),
					       nextu->siblings,
					       nextu->nsiblings,
					       nextu->nhg_fal_obj);
		}
		if (update_pd_state)
			pd_state->state = fal_state_to_pd_state(rc);
		if (!rc || old_pd_state->created)
			pd_state->created = true;
		route_hw_stats[old_pd_state->state]--;
		old_pd_state->state = PD_OBJ_STATE_NOT_NEEDED;
		route_hw_stats[pd_state->state]++;
		/* Successfully added to SW, so return success. */
		return 0;
	}

	/*
	 * We have successfully added to the lpm, and now need to update the
	 * platform, if there is one.
	 */
	rc = fal_ip4_new_route(vrf_id, ip, depth, lpm_get_id(lpm),
			       nextu->siblings,
			       nextu->nsiblings,
			       nextu->nhg_fal_obj);
	if (update_pd_state)
		pd_state->state = fal_state_to_pd_state(rc);
	if (!rc)
		pd_state->created = true;
	route_hw_stats[pd_state->state]++;

	/*
	 * If the SW worked, but the HW failed then return success. The
	 * user needs to use the show commands and the notification infra
	 * in this case.
	 */
	return 0;
}

static int
route_lpm_delete(vrfid_t vrf_id, struct lpm *lpm, uint32_t ip,
		 uint8_t depth, uint32_t *next_hop, int16_t scope)

{
	int rc;
	struct pd_obj_state_and_flags pd_state;
	struct pd_obj_state_and_flags *new_pd_state;
	uint32_t new_nh;
	bool promoted = false;

	rc = lpm_delete(lpm, ntohl(ip), depth, next_hop, scope, &pd_state,
			    &new_nh, &new_pd_state);
	switch (rc) {
	case LPM_SUCCESS:
		/* Success */
		route_sw_stats[PD_OBJ_STATE_FULL]--;
		break;
	case LPM_HIGHER_SCOPE_EXISTS:
		/* Deleted, but was not programmed as higher scope exists */
		route_sw_stats[PD_OBJ_STATE_NOT_NEEDED]--;
		assert(pd_state.state == PD_OBJ_STATE_NOT_NEEDED);
		return rc;
	case LPM_LOWER_SCOPE_EXISTS:
		/* Deleted, but lower scope was promoted so is now programmed */
		route_sw_stats[PD_OBJ_STATE_NOT_NEEDED]--;
		promoted = true;
		break;
	default:
		/* Can happen when trying to delete an incomplete route */
		return rc;
	}

	if (promoted) {
		struct next_hop_u *nextu =
			rcu_dereference(nh_tbl.entry[new_nh]);
		bool update_new_pd_state = true;

		if (nextu->pd_state != PD_OBJ_STATE_FULL) {
			new_pd_state->state = nextu->pd_state;
			nextu = nextu_blackhole;
			update_new_pd_state = false;
		}

		if (pd_state.created) {
			rc = fal_ip4_upd_route(vrf_id, ip, depth,
					       lpm_get_id(lpm),
					       nextu->siblings,
					       nextu->nsiblings,
					       nextu->nhg_fal_obj);
		} else {
			rc = fal_ip4_new_route(vrf_id, ip, depth,
					       lpm_get_id(lpm),
					       nextu->siblings,
					       nextu->nsiblings,
					       nextu->nhg_fal_obj);
		}
		if (update_new_pd_state)
			new_pd_state->state = fal_state_to_pd_state(rc);
		if (!rc || pd_state.created)
			new_pd_state->created = true;
		route_hw_stats[pd_state.state]--;
		route_hw_stats[new_pd_state->state]++;
		return 0;
	}

	/* successfully removed and no lower scope promoted */
	if (pd_state.created) {
		rc = fal_ip4_del_route(vrf_id, ip, depth, lpm_get_id(lpm));
		switch (rc) {
		case 0:
			route_hw_stats[pd_state.state]--;
			break;
		default:
			/* General failure */
			break;
		}
	} else
		route_hw_stats[pd_state.state]--;

	/* Successfully deleted from SW, so return success. */
	return 0;
}

/* Dynamically grow LPM table if necessary.
 */
static int rt_lpm_resize(struct route_head *rt_head, uint32_t id)
{
	struct lpm **new_tbl, **old_tbl;
	uint32_t old_id;

	if (id < rt_head->rt_rtm_max)
		return 0;

	new_tbl = malloc_huge_aligned((id + 1) * sizeof(struct lpm *));
	if (new_tbl == NULL) {
		RTE_LOG(ERR, ROUTE,
			"Can't grow LPM table to %u entries\n", id);
		return -1;
	}

	/* Copy existing table */
	old_tbl = rt_head->rt_table;
	old_id = rt_head->rt_rtm_max;
	if (old_tbl)
		memcpy(new_tbl, old_tbl,
		       sizeof(struct lpm *) * rt_head->rt_rtm_max);

	rcu_set_pointer(&rt_head->rt_table, new_tbl);
	rt_head->rt_rtm_max = id + 1;

	if (old_tbl) {
		if (defer_rcu_huge(old_tbl,
				   (old_id * sizeof(struct lpm *)))) {
			RTE_LOG(ERR, LPM, "Failed to free old LPM tbl\n");
			return -1;
		}
	}
	return 0;
}

static bool
rt_lpm_is_empty(struct lpm *lpm)
{
	assert(lpm_rule_count(lpm) >= ARRAY_SIZE(reserved_routes));
	return lpm_rule_count(lpm) == ARRAY_SIZE(reserved_routes);
}

static bool
rt_is_reserved(in_addr_t addr, int prefix_length, int scope)
{
	unsigned int rt_idx;

	for (rt_idx = 0; rt_idx < ARRAY_SIZE(reserved_routes); rt_idx++) {
		if (addr == reserved_routes[rt_idx].addr &&
		    prefix_length == reserved_routes[rt_idx].prefix_length &&
		    scope == reserved_routes[rt_idx].scope)
			return true;
	}

	return false;
}

static bool
rt_lpm_add_reserved_routes(struct lpm *lpm, struct vrf *vrf)
{
	char b[INET_ADDRSTRLEN];
	unsigned int rt_idx;

	if (vrf->v_id == VRF_INVALID_ID)
		return true;

	for (rt_idx = 0; rt_idx < ARRAY_SIZE(reserved_routes); rt_idx++) {
		in_addr_t addr = htonl(reserved_routes[rt_idx].addr);
		struct next_hop *nhop;
		uint32_t nh_idx;
		int err_code;

		nhop = nexthop_create(NULL, INADDR_ANY,
				      reserved_routes[rt_idx].flags,
				      0, NULL);
		if (!nhop)
			return false;

		err_code = route_nexthop_new(nhop, 1, RTPROT_UNSPEC, &nh_idx);
		if (err_code < 0) {
			RTE_LOG(ERR, ROUTE,
				"reserved route add %s/%u failed - cannot create nexthop: %s\n",
				inet_ntop(AF_INET,
					  &addr, b,
					  sizeof(b)),
				reserved_routes[rt_idx].prefix_length,
				strerror(-err_code));
			free(nhop);
			return false;
		}

		err_code = route_lpm_add(
			vrf->v_id,
			lpm,
			addr,
			reserved_routes[rt_idx].prefix_length,
			nh_idx, reserved_routes[rt_idx].scope);
		if (err_code < 0) {
			RTE_LOG(ERR, ROUTE,
				"reserved route %s/%u idx %u add to LPM failed (%d)\n",
				inet_ntop(AF_INET,
					  &addr,
					  b, sizeof(b)),
				reserved_routes[rt_idx].prefix_length,
				nh_idx, err_code);
		}
		free(nhop);
		if (err_code != 0) {
			nexthop_put(nh_idx);
			return false;
		}
	}

	return true;
}

static bool
rt_lpm_del_reserved_routes(struct lpm *lpm, struct vrf *vrf)
{
	char b[INET_ADDRSTRLEN];
	unsigned int rt_idx;

	if (vrf->v_id == VRF_INVALID_ID)
		return true;

	for (rt_idx = 0; rt_idx < ARRAY_SIZE(reserved_routes); rt_idx++) {
		in_addr_t addr = htonl(reserved_routes[rt_idx].addr);
		uint32_t nh_idx;
		int err_code;

		err_code = route_lpm_delete(
			vrf->v_id,
			lpm,
			addr,
			reserved_routes[rt_idx].prefix_length,
			&nh_idx,
			reserved_routes[rt_idx].scope);
		if (err_code < 0) {
			RTE_LOG(ERR, ROUTE,
				"reserved route add %s/%u idx %u failed (%d)\n",
				inet_ntop(AF_INET,
					  &addr,
					  b, sizeof(b)),
				reserved_routes[rt_idx].prefix_length,
				nh_idx, err_code);
			return false;
		}
		nexthop_put(nh_idx);
	}

	return true;
}

/* Create a new LPM table for route table id */
static struct lpm *rt_create_lpm(uint32_t id, struct vrf *vrf)
{
	struct lpm *lpm;

	if (rt_lpm_resize(&vrf->v_rt4_head, id) < 0)
		return NULL;

	lpm = lpm_create(id);
	if (lpm == NULL) {
		RTE_LOG(ERR, ROUTE, "Can't create LPM for vrf %u table %u\n",
			vrf->v_id, id);
		return NULL;
	}

	if (!rt_lpm_add_reserved_routes(lpm, vrf)) {
		DP_LOG_W_VRF(ERR, ROUTE, vrf->v_id,
			     "Failed to add reserved routes to table %u\n",
			     id);
		lpm_free(lpm);
		return NULL;
	}

	rcu_assign_pointer(vrf->v_rt4_head.rt_table[id], lpm);

	/*
	 * Alias all tables other than the main one from the default
	 * VRF into other VRFs.
	 */
	if (vrf->v_id == VRF_DEFAULT_ID && id != RT_TABLE_MAIN) {
		struct vrf *dst_vrf;
		vrfid_t vrf_id;

		VRF_FOREACH_KERNEL(dst_vrf, vrf_id) {
			if (rt_lpm_resize(&dst_vrf->v_rt4_head, id) < 0)
				return NULL;

			rcu_assign_pointer(dst_vrf->v_rt4_head.rt_table[id],
					   lpm);
		}
	}

	return lpm;
}

static struct lpm *rt_get_lpm(struct route_head *rt_head, uint32_t id)
{
	if (unlikely(id >= rt_head->rt_rtm_max))
		return NULL;

	return rcu_dereference(rt_head->rt_table[id]);
}

static struct next_hop *nexthop_mp_select(struct next_hop *next,
					  uint32_t size,
					  uint32_t hash)
{
	uint16_t path;

	if (ecmp_max_path && ecmp_max_path < size)
		size = ecmp_max_path;

	path = ecmp_lookup(size, hash);
	if (unlikely(next[path].flags & RTF_DEAD)) {
		/* retry to find a good path */
		for (path = 0; path < size; path++) {
			if (!(next[path].flags & RTF_DEAD))
				break;
		}

		if (path == size)
			return NULL;
	}
	return next + path;
}

/*
 * Obtain a nexthop from a nexthop(_u) index
 */
inline struct next_hop *nexthop_select(uint32_t nh_idx,
				       const struct rte_mbuf *m,
				       uint16_t ether_type)
{
	struct next_hop_u *nextu;
	struct next_hop *next;
	uint32_t size;

	nextu = rcu_dereference(nh_tbl.entry[nh_idx]);
	if (unlikely(!nextu))
		return NULL;

	size = nextu->nsiblings;
	next = nextu->siblings;

	if (likely(size == 1))
		return next;

	return nexthop_mp_select(next, size, ecmp_mbuf_hash(m, ether_type));
}

struct next_hop *nexthop_get(uint32_t nh_idx, uint8_t *size)
{
	struct next_hop_u *nextu;

	nextu = rcu_dereference(nh_tbl.entry[nh_idx]);
	*size = nextu->nsiblings;
	return nextu->siblings;
}

/* Check if route table exists */
bool rt_valid_tblid(vrfid_t vrfid, uint32_t tbl_id)
{
	struct vrf *vrf = vrf_get_rcu(vrfid);

	if (!vrf)
		return false;

	return vrf->v_rt4_head.rt_table[tbl_id] != NULL;
}

/*
 * Lookup nexthop based on destination address
 *
 * Returns RCU protected nexthop structure or NULL.
 */
ALWAYS_INLINE
struct next_hop *rt_lookup(in_addr_t dst, uint32_t tblid,
			   const struct rte_mbuf *m)
{
	vrfid_t vrfid = pktmbuf_get_vrf(m);
	struct vrf *vrf = vrf_get_rcu(vrfid);

	if (!vrf)
		return NULL;

	return rt_lookup_fast(vrf, dst, tblid, m);
}

/*
 * Lookup nexthop based on destination address
 *
 * Assumes both the VRF ID is valid and the VRF exists.
 *
 * Returns RCU protected nexthop structure or NULL.
 */
ALWAYS_INLINE
struct next_hop *rt_lookup_fast(struct vrf *vrf, in_addr_t dst,
				uint32_t tblid,
				const struct rte_mbuf *m)
{
	struct next_hop *nh;
	struct lpm *lpm;
	uint32_t idx;

	lpm = rcu_dereference(vrf->v_rt4_head.rt_table[tblid]);

	if (unlikely(lpm_lookup(lpm, ntohl(dst), &idx) != 0))
		return NULL;

	nh = nexthop_select(idx, m, ETHER_TYPE_IPv4);
	if (nh && unlikely(nh->flags & RTF_NOROUTE))
		return NULL;
	return nh;
}

inline bool is_local_ipv4(vrfid_t vrf_id, in_addr_t dst)
{
	struct vrf *vrf = vrf_get_rcu(vrf_id);
	struct next_hop_u *nextu;
	struct next_hop *next;
	struct lpm *lpm;
	uint32_t idx;

	if (!vrf)
		return false;

	lpm = rcu_dereference(vrf->v_rt4_head.rt_table[RT_TABLE_MAIN]);

	if (unlikely(lpm_lookup(lpm, ntohl(dst), &idx) != 0))
		return false;

	nextu = rcu_dereference(nh_tbl.entry[idx]);
	if (unlikely(!nextu))
		return false;

	next = rcu_dereference(nextu->siblings);
	if (next->flags & RTF_LOCAL)
		return true;

	return false;
}

struct next_hop *
nexthop_create(struct ifnet *ifp, in_addr_t gw, uint32_t flags,
	       uint16_t num_labels, label_t *labels)
{
	struct next_hop *next = malloc(sizeof(struct next_hop));

	if (next) {
		next->gateway = gw;
		next->flags = flags;
		nh4_set_ifp(next, ifp);

		if (!nh_outlabels_set(&next->outlabels, num_labels,
					   labels)) {
			RTE_LOG(ERR, ROUTE,
				"Failed to set outlabels for nexthop with %u labels\n",
				num_labels);
			free(next);
			return NULL;
		}
	}
	return next;
}

/*
 * Create an array of next_hops based on the hops in the NHU.
 */
static struct next_hop *nexthop_create_copy(struct next_hop_u *nhu, int *size)
{
	struct next_hop *next, *n;
	struct next_hop *array = rcu_dereference(nhu->siblings);
	int i;

	*size = nhu->nsiblings;
	n = next = calloc(sizeof(struct next_hop), *size);
	if (!next)
		return NULL;

	for (i = 0; i < nhu->nsiblings; i++) {
		struct next_hop *nhu_next = array + i;

		memcpy(n, nhu_next, sizeof(struct next_hop));
		nh_outlabels_copy(&nhu_next->outlabels, &n->outlabels);
		n++;
	}
	return next;
}

static int
nexthop_hashfn(const struct nexthop_hash_key *key,
	       unsigned long seed __rte_unused)
{
	size_t size = key->size;
	uint32_t hash_keys[size * 3];
	struct ifnet *ifp;
	uint16_t i, j = 0;

	for (i = 0; i < size; i++, j += 3) {
		hash_keys[j] = key->nh[i].gateway;
		ifp = nh4_get_ifp(&key->nh[i]);
		hash_keys[j+1] = ifp ? ifp->if_index : 0;
		hash_keys[j+2] = key->nh[i].flags & NH_FLAGS_CMP_MASK;
	}

	return rte_jhash_32b(hash_keys, size * 3, 0);
}

static int nexthop_cmpfn(struct cds_lfht_node *node, const void *key)
{
	const struct nexthop_hash_key *h_key = key;
	const struct next_hop_u *nu =
		caa_container_of(node, const struct next_hop_u, nh_node);
	uint16_t i;

	if (h_key->size != nu->nsiblings)
		return false;

	for (i = 0; i < h_key->size; i++) {
		if ((nu->proto != h_key->proto) ||
		    (nh4_get_ifp(&nu->siblings[i]) !=
		     nh4_get_ifp(&h_key->nh[i])) ||
		    ((nu->siblings[i].flags & NH_FLAGS_CMP_MASK) !=
		     (h_key->nh[i].flags & NH_FLAGS_CMP_MASK)) ||
		    (nu->siblings[i].gateway != h_key->nh[i].gateway) ||
		    !nh_outlabels_cmpfn(&nu->siblings[i].outlabels,
					&h_key->nh[i].outlabels))
			return false;
	}
	return true;
}

static struct next_hop_u *nexthop_lookup(const struct nexthop_hash_key *key)
{
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;

	cds_lfht_lookup(nexthop_hash,
			nexthop_hashfn(key, 0),
			nexthop_cmpfn, key, &iter);
	node = cds_lfht_iter_get_node(&iter);
	if (node)
		return caa_container_of(node, struct next_hop_u, nh_node);
	else
		return NULL;
}

static int
nexthop_hash_insert(struct next_hop_u *nu,
		    const struct nexthop_hash_key *key)
{
	struct cds_lfht_node *ret_node;

	cds_lfht_node_init(&nu->nh_node);
	unsigned long hash = nexthop_hashfn(key, 0);

	ret_node = cds_lfht_add_unique(nexthop_hash, hash,
				       nexthop_cmpfn, key,
				       &nu->nh_node);

	return (ret_node != &nu->nh_node) ? EEXIST : 0;
}

static struct next_hop_u *nexthop_alloc(int size)
{
	struct next_hop_u *nextu;

	nextu = calloc(1, sizeof(*nextu));
	if (unlikely(!nextu)) {
		RTE_LOG(ERR, ROUTE, "can't alloc next_hop_u\n");
		return NULL;
	}

	nextu->nh_fal_obj = calloc(size, sizeof(*nextu->nh_fal_obj));
	if (!nextu->nh_fal_obj) {
		free(nextu);
		return NULL;
	}

	if (size == 1) {
		/* Optimize for non-ECMP case by staying in cache line */
		nextu->siblings = &nextu->hop0;
	} else {
		nextu->siblings = calloc(1, size * sizeof(struct next_hop));
		if (unlikely(nextu->siblings == NULL)) {
			free(nextu->nh_fal_obj);
			free(nextu);
			return NULL;
		}
	}
	nextu->nsiblings = size;
	return nextu;
}

static void __nexthop_destroy(struct next_hop_u *nextu)
{
	unsigned int i;

	for (i = 0; i < nextu->nsiblings; i++)
		nh_outlabels_destroy(&nextu->siblings[i].outlabels);
	if (nextu->siblings != &nextu->hop0)
		free(nextu->siblings);

	free(nextu->nh_fal_obj);
	free(nextu);
}

/*
 * Remove the old NH from the hash and add the new one. Can not
 * use a call to cds_lfht_add_replace() or any of the variants
 * as the key for the new NH may be very different in the case
 * where there are a different number of paths.
 */
static int
nexthop_hash_del_add(struct next_hop_u *old_nu,
		     struct next_hop_u *new_nu)
{
	struct nexthop_hash_key key = {.nh = new_nu->siblings,
				       .size = new_nu->nsiblings,
				       .proto = new_nu->proto };
	int rc;

	/* Remove old one */
	rc = cds_lfht_del(nexthop_hash, &old_nu->nh_node);
	assert(rc == 0);
	if (rc != 0)
		return rc;

	/* add new one */
	return nexthop_hash_insert(new_nu, &key);
}

/* Reuse existing next hop entry */
static struct next_hop_u *nexthop_reuse(const struct nexthop_hash_key *key,
					uint32_t *slot)
{
	struct next_hop_u *nu;
	int index;

	nu = nexthop_lookup(key);
	if (!nu)
		return NULL;

	index = nu->index;

	*slot = index;
	++nu->refcount;

	DP_DEBUG(ROUTE, DEBUG, ROUTE,
		 "nexthop reuse: nexthop %d, refs %u\n",
		 index, nu->refcount);

	return nu;
}

/* Callback from RCU after all other threads are done. */
static void nexthop_destroy(struct rcu_head *head)
{
	struct next_hop_u *nextu
		= caa_container_of(head, struct next_hop_u, rcu);

	__nexthop_destroy(nextu);
}

/* Lookup (or create) nexthop based on hop information */
int nexthop_new(const struct next_hop *nh, uint16_t size, uint8_t proto,
		uint32_t *slot)
{
	struct nexthop_hash_key key = {
				.nh = nh, .size = size, .proto = proto };
	struct next_hop_u *nextu;
	uint32_t rover = nh_tbl.rover;
	uint32_t nh_iter;
	int ret;

	nextu = nexthop_reuse(&key, slot);
	if (nextu)
		return 0;

	if (unlikely(nh_tbl.in_use == NEXTHOP_HASH_TBL_SIZE)) {
		RTE_LOG(ERR, ROUTE, "next_hop_u full\n");
		return -ENOSPC;
	}

	nextu = nexthop_alloc(size);
	if (!nextu)
		return -ENOMEM;

	nextu->nsiblings = size;
	nextu->refcount = 1;
	nextu->index = rover;
	nextu->proto = proto;
	if (size == 1) {
		nextu->hop0 = *nh;
	} else {
		memcpy(nextu->siblings, nh, size * sizeof(struct next_hop));
	}
	if (unlikely(nexthop_hash_insert(nextu, &key))) {
		__nexthop_destroy(nextu);
		return -ENOMEM;
	}

	ret = fal_ip4_new_next_hops(nextu->nsiblings, nextu->siblings,
				    &nextu->nhg_fal_obj,
				    nextu->nh_fal_obj);
	if (ret < 0) {
		if (ret != -EOPNOTSUPP)
			RTE_LOG(ERR, ROUTE,
				"FAL IPv4 next-hop-group create failed: %s\n",
				strerror(-ret));
		nextu->pd_state = fal_state_to_pd_state(ret);
	} else
		nextu->pd_state = PD_OBJ_STATE_FULL;

	nh_iter = rover;
	do {
		nh_iter++;
		if (nh_iter >= NEXTHOP_HASH_TBL_SIZE)
			nh_iter = 0;
	} while ((rcu_dereference(nh_tbl.entry[nh_iter]) != NULL) &&
		 likely(nh_iter != rover));

	nh_tbl.rover = nh_iter;
	*slot = rover;
	nh_tbl.in_use++;

	rcu_assign_pointer(nh_tbl.entry[rover], nextu);

	return 0;
}

static bool nh_is_connected(const struct next_hop *nh)
{
	if (nh->flags & (RTF_BLACKHOLE | RTF_REJECT |
			 RTF_SLOWPATH | RTF_GATEWAY |
			 RTF_LOCAL | RTF_NOROUTE))
		return false;

	return true;
}

static bool nh_is_local(const struct next_hop *nh)
{
	if (nh->flags & RTF_LOCAL)
		return true;

	return false;
}

static bool nh_is_gw(const struct next_hop *nh)
{
	if (nh->flags & RTF_GATEWAY)
		return true;

	return false;
}

/*
 * Modifying a NH in non atomic way, so this must be atomically swapped
 * into the forwarding state when ready
 */
static void nh4_set_neigh_present(struct next_hop *next_hop,
				  struct llentry *lle)
{
	assert((next_hop->flags & RTF_NEIGH_PRESENT) == 0);
	next_hop->flags |= RTF_NEIGH_PRESENT;
	next_hop->u.lle = lle;
	nh_tbl.neigh_present++;
}

static void nh4_clear_neigh_present(struct next_hop *next_hop)
{
	assert(next_hop->flags & RTF_NEIGH_PRESENT);
	next_hop->flags &= ~RTF_NEIGH_PRESENT;
	next_hop->u.ifp = next_hop->u.lle->ifp;
	nh_tbl.neigh_present--;
}

static void nh4_set_neigh_created(struct next_hop *next_hop,
				  struct llentry *lle)
{
	assert((next_hop->flags & RTF_NEIGH_CREATED) == 0);
	next_hop->flags |= RTF_NEIGH_CREATED;
	next_hop->u.lle = lle;
	nh_tbl.neigh_created++;
}

static void nh4_clear_neigh_created(struct next_hop *next_hop)
{
	assert(next_hop->flags & RTF_NEIGH_CREATED);
	next_hop->flags &= ~RTF_NEIGH_CREATED;
	next_hop->u.ifp = next_hop->u.lle->ifp;
	nh_tbl.neigh_created--;
}

static int nextu_nc_count(const struct next_hop_u *nhu)
{
	int count = 0;
	int i;
	struct next_hop *array = rcu_dereference(nhu->siblings);

	for (i = 0; i < nhu->nsiblings; i++) {
		struct next_hop *next = array + i;

		if (nh4_is_neigh_created(next))
			count++;
	}
	return count;
}

static struct next_hop *nextu_find_path_using_ifp(struct next_hop_u *nhu,
						  struct ifnet *ifp,
						  int *sibling)
{
	int i;
	struct next_hop *array = rcu_dereference(nhu->siblings);

	for (i = 0; i < nhu->nsiblings; i++) {
		struct next_hop *next = array + i;

		if (nh4_get_ifp(next) == ifp) {
			*sibling = i;
			return next;
		}
	}
	return NULL;
}

static bool nextu_is_any_connected(const struct next_hop_u *nhu)
{
	int i;
	struct next_hop *array = rcu_dereference(nhu->siblings);

	for (i = 0; i < nhu->nsiblings; i++) {
		struct next_hop *next = array + i;

		if (nh_is_connected(next))
			return true;
	}
	return false;
}

/*
 * Drops reference to nexthop
 * and if last reference frees it for reuse.
 */
void nexthop_put(uint32_t idx)
{
	struct next_hop_u *nextu = rcu_dereference(nh_tbl.entry[idx]);

	if (--nextu->refcount == 0) {
		struct next_hop *array = nextu->siblings;
		int ret;
		int i;

		nh_tbl.entry[idx] = NULL;
		--nh_tbl.in_use;

		for (i = 0; i < nextu->nsiblings; i++) {
			struct next_hop *nh = array + i;

			if (nh4_is_neigh_present(nh))
				nh_tbl.neigh_present--;
			if (nh4_is_neigh_created(nh))
				nh_tbl.neigh_created--;
		}

		if (nextu->pd_state == PD_OBJ_STATE_FULL) {
			ret = fal_ip4_del_next_hops(nextu->nhg_fal_obj,
						    nextu->nsiblings,
						    nextu->siblings,
						    nextu->nh_fal_obj);
			if (ret < 0) {
				RTE_LOG(ERR, ROUTE,
					"FAL IPv4 next-hop-group delete failed: %s\n",
					strerror(-ret));
			}
		}

		cds_lfht_del(nexthop_hash, &nextu->nh_node);
		call_rcu(&nextu->rcu, nexthop_destroy);
	}
}

enum nh_change {
	NH_NO_CHANGE,
	NH_SET_NEIGH_CREATED,
	NH_CLEAR_NEIGH_CREATED,
	NH_SET_NEIGH_PRESENT,
	NH_CLEAR_NEIGH_PRESENT,
	NH_DELETE,
};

/*
 * Replace a NH. If valid then add the llentry. In not valid
 * then remove it.
 */
static int
route_nh_replace(struct next_hop_u *nextu, uint32_t nh_idx, struct llentry *lle,
		 uint32_t *new_nextu_idx_for_del,
		 enum nh_change (*nh_processing_cb)(struct next_hop *next,
						    int sibling,
						    void *arg),
		 void *arg)
{
	struct next_hop_u *new_nextu = NULL;
	struct next_hop *old_array;
	struct next_hop *new_array = NULL;
	enum nh_change nh_change;
	bool any_change = false;
	int i;
	int deleted = 0;

	ASSERT_MASTER();

	/* walk all the NHs, copying as we go */
	old_array = rcu_dereference(nextu->siblings);
	new_nextu = nexthop_alloc(nextu->nsiblings);
	if (!new_nextu)
		return 0;
	new_nextu->proto = nextu->proto;
	new_nextu->index = nextu->index;
	new_nextu->refcount = nextu->refcount;
	new_array = rcu_dereference(new_nextu->siblings);

	for (i = 0; i < nextu->nsiblings; i++) {
		struct next_hop *next = old_array + i;
		struct next_hop *new_next = new_array + i - deleted;

		nh_change = nh_processing_cb(next, i, arg);

		/* Copy across old NH */
		memcpy(new_next, next, sizeof(struct next_hop));
		nh_outlabels_copy(&next->outlabels, &new_next->outlabels);

		switch (nh_change) {
		case NH_NO_CHANGE:
			break;
		case NH_SET_NEIGH_CREATED:
			any_change = true;
			nh4_set_neigh_created(new_next, lle);
			break;
		case NH_CLEAR_NEIGH_CREATED:
			any_change = true;
			nh4_clear_neigh_created(new_next);
			break;
		case NH_SET_NEIGH_PRESENT:
			any_change = true;
			nh4_set_neigh_present(new_next, lle);
			break;
		case NH_CLEAR_NEIGH_PRESENT:
			any_change = true;
			nh4_clear_neigh_present(new_next);
			break;
		case NH_DELETE:
			if (!new_nextu_idx_for_del) {
				__nexthop_destroy(new_nextu);
				return -1;
			}
			any_change = true;
			deleted++;
			break;
		}
	}

	/* Did we make any changes?  If not then we can return */
	if (!any_change) {
		__nexthop_destroy(new_nextu);
		return 0;
	}

	if (deleted) {
		/*
		 * We are deleting at least one nh - create a new
		 * nextu for caller to deal with.
		 */
		if (deleted != nextu->nsiblings &&
		    route_nexthop_new(nextu->siblings, nextu->nsiblings,
				      nextu->proto, new_nextu_idx_for_del) < 0)
			deleted = nextu->nsiblings;
		__nexthop_destroy(new_nextu);
		return deleted;
	}

	if (nexthop_hash_del_add(nextu, new_nextu)) {
		__nexthop_destroy(new_nextu);
		RTE_LOG(ERR, ROUTE, "nh replace failed\n");
		return 0;
	}

	/*
	 * It's safe to copy over the FAL objects without
	 * notifications as there are no FAL-visible changes to the
	 * object - it maintains its own linkage to the neighbour
	 */
	new_nextu->nhg_fal_obj = nextu->nhg_fal_obj;
	memcpy(new_nextu->nh_fal_obj, nextu->nh_fal_obj,
	       new_nextu->nsiblings * sizeof(*new_nextu->nh_fal_obj));

	assert(nh_tbl.entry[nh_idx] == nextu);
	rcu_xchg_pointer(&nh_tbl.entry[nh_idx], new_nextu);

	call_rcu(&nextu->rcu, nexthop_destroy);
	return 0;
}

struct subtree_walk_arg {
	uint32_t ip;
	uint8_t depth;
	bool delete;
	struct vrf *vrf;
};

static void subtree_walk_route_cleanup_cb(struct lpm *lpm,
					  uint32_t masked_ip,
					  uint8_t depth, uint32_t idx,
					  void *arg)
{
	struct subtree_walk_arg *changing = arg;
	struct next_hop_u *nextu = rcu_dereference(nh_tbl.entry[idx]);
	uint32_t cover_ip;
	uint8_t cover_depth;
	uint32_t cover_nh_idx;
	int neigh_created = 0;
	int ret;

	if (!nextu)
		return;

	neigh_created = nextu_nc_count(nextu);
	if (neigh_created == 0)
		return;

	/*
	 * If we're changing this route itself then remove the
	 * neigbour created route.
	 */
	if (masked_ip != changing->ip && depth != changing->depth) {
		/*
		 * Is the route we are about to delete the cover of
		 * this route
		 */
		if (lpm_find_cover(lpm, masked_ip, depth, &cover_ip,
				   &cover_depth, &cover_nh_idx)) {
			/*
			 * we must have a cover as this is a subtree
			 * walk
			 */
			assert(0);
			return;
		}

		/*
		 * If changing route is not the immediate cover return
		 * early.
		 */
		if (changing->ip != cover_ip ||
		    changing->depth != cover_depth)
			return;
	}

	/*
	 * We created at least one of the paths in here. It is covered by the
	 * route we are about to delete. Delete this too. It will be recreated
	 * later if required. This is ok as packets using this route will
	 * still be forwarded, but with an arp lookup required until the
	 * entry is recreaetd with correct values.
	 */
	ret = route_lpm_delete(changing->vrf->v_id,
				   lpm, htonl(masked_ip), 32, &cover_nh_idx,
				   RT_SCOPE_LINK);
	if (ret < 0) {
		char b[INET_ADDRSTRLEN];
		in_addr_t dst = htonl(masked_ip);

		DP_LOG_W_VRF(
			ERR, ROUTE, changing->vrf->v_id,
			"route delete %s/32 failed (%d)\n",
			inet_ntop(AF_INET, &dst, b, sizeof(b)),
			ret);
	}

	nexthop_put(idx);
}

static unsigned int lle_routing_insert_arp_cb(struct lltable *llt __unused,
					  struct llentry *lle,
					  void *arg __unused)
{
	pthread_mutex_unlock(&route_mutex);
	routing_insert_arp_safe(lle, false);
	pthread_mutex_lock(&route_mutex);
	return 0;
}


static void route_change_process_nh(struct next_hop_u *nhu,
				    enum nh_change (*upd_neigh_present_cb)(
					    struct next_hop *next,
					    int sibling,
					    void *arg))
{
	const struct next_hop *array;
	int index;
	int i;


	index = nhu->index;
	array = rcu_dereference(nhu->siblings);
	for (i = 0; i < nhu->nsiblings; i++) {
		const struct next_hop *next = array + i;
		const struct ifnet *ifp = nh4_get_ifp(next);

		if (!ifp)
			/* happens for local routes */
			continue;

		if (!nh_is_gw(next))
			continue;

		/*
		 * Is there an lle on this interface with a
		 * matching address.
		 */
		struct llentry *lle = in_lltable_find((struct ifnet *)ifp,
						      next->gateway);
		if (lle) {
			route_nh_replace(nhu, nhu->index, lle, NULL,
					 upd_neigh_present_cb,
					 lle);
			/*
			 * Need to reread as may have been
			 * replaced by prev func, and will not
			 * then be found in hash table.
			 */
			nhu = rcu_dereference(nh_tbl.entry[index]);
			if (!nhu)
				break;
		}
	}
}

static void
walk_nhs_for_route_change(enum nh_change (*upd_neigh_present_cb)(
				  struct next_hop *next,
				  int sibling,
				  void *arg))
{
	struct next_hop_u *nhu;
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;

	ASSERT_MASTER();

	cds_lfht_for_each(nexthop_hash, &iter, node) {
		nhu = caa_container_of(node, struct next_hop_u, nh_node);

		route_change_process_nh(nhu, upd_neigh_present_cb);
	}
}

/*
 * On an arp add, should we set NEIGH_PRESENT from this NH.
 */
static enum nh_change routing_arp_add_gw_nh_replace_cb(struct next_hop *next,
						       int sibling __unused,
						       void *arg)
{
	struct llentry *lle = arg;
	struct in_addr *ip = ll_ipv4_addr(lle);
	struct ifnet *ifp = rcu_dereference(lle->ifp);

	if (!nh_is_gw(next) || (next->gateway != ip->s_addr))
		return NH_NO_CHANGE;
	if (nh4_get_ifp(next) != ifp)
		return NH_NO_CHANGE;
	if (nh_is_local(next) || nh4_is_neigh_present(next) ||
		nh4_is_neigh_created(next))
		return NH_NO_CHANGE;

	return NH_SET_NEIGH_PRESENT;
}

/*
 * This entry has just been added.
 *
 * Invalidate any old arp links
 * Create new arp links as required.
 */
static void
route_change_link_arp(struct vrf *vrf, struct lpm *lpm,
		      uint32_t ip, uint8_t depth, uint32_t next_hop,
		      int16_t scope __unused)
{
	int i;
	const struct next_hop_u *nextu;
	const struct next_hop *array;
	struct subtree_walk_arg subtree_arg = {
		.ip = ip,
		.depth = depth,
		.delete = false,
		.vrf = vrf,
	};
	uint32_t cover_ip;
	uint8_t cover_depth;
	uint32_t cover_idx;

	/*
	 * If the entry we have just created is connected OR its
	 * cover is connected then there may be routes that are marked
	 * as NEIGH_CREATED that should no longer be. This can be due to
	 * the cover no longer being connected, or being connected out of
	 * a different interface.
	 * In this case do a subtree walk of the entry just added.
	 * Any NHs we find that are NEIGH_CREATED, that have this entry
	 * as the cover need to be checked to see if they are still accurate,
	 * and removed if not.
	 */
	nextu = rcu_dereference(nh_tbl.entry[next_hop]);
	if (nextu_is_any_connected(nextu)) {
		lpm_subtree_walk(lpm, ip, depth,
				 subtree_walk_route_cleanup_cb,
				 &subtree_arg);
	} else if (lpm_find_cover(lpm, ip, depth, &cover_ip,
				  &cover_depth, &cover_idx) == 0) {
		const struct next_hop_u *cover_nextu;

		cover_nextu = rcu_dereference(nh_tbl.entry[cover_idx]);
		if (nextu_is_any_connected(cover_nextu)) {
			lpm_subtree_walk(lpm, ip, depth,
					 subtree_walk_route_cleanup_cb,
					 &subtree_arg);
		}
	}

	/* Walk all the interfaces arp entries to do /32 processing */
	array = rcu_dereference(nextu->siblings);
	for (i = 0; i < nextu->nsiblings; i++) {
		const struct next_hop *next = array + i;
		const struct ifnet *ifp = nh4_get_ifp(next);

		if (!ifp)
			/* happens for local routes */
			continue;

		lltable_walk(ifp->if_lltable, lle_routing_insert_arp_cb, NULL);
	}

	/* Now do the gateway processing. */
	walk_nhs_for_route_change(routing_arp_add_gw_nh_replace_cb);
}

/*
 * This entry is about to be deleted.
 *
 * Invalidate any old arp links
 */
static void
route_delete_unlink_arp(struct vrf *vrf, struct lpm *lpm, uint32_t ip,
			uint8_t depth)
{
	const struct next_hop_u *nextu;
	uint32_t nh_idx;
	struct subtree_walk_arg subtree_arg = {
		.ip = ip,
		.depth = depth,
		.delete = true,
		.vrf = vrf,
	};
	uint32_t cover_ip;
	uint8_t cover_depth;
	uint32_t cover_idx;

	/*
	 * If the entry being deleted is connected there may be routes that
	 * are NEIGH_CREATED that will not be  after this is deleted.
	 * This can be due to the new cover no longer being connected,
	 * or being connected out of a different interface.
	 *
	 * In this case do a subtree walk of the entry we are about
	 * to delete. Any NHs we find that are NEIGH_CREATED, that have this
	 * entry as the cover need to be checked to see if they are still
	 * accurate, and removed if not.
	 */
	if (lpm_lookup_exact(lpm, ip, depth, &nh_idx))
		return;

	nextu = rcu_dereference(nh_tbl.entry[nh_idx]);
	if (nextu_is_any_connected(nextu)) {
		subtree_walk_route_cleanup_cb(lpm, ip, depth, nh_idx,
					      &subtree_arg);
		lpm_subtree_walk(lpm, ip, depth,
				 subtree_walk_route_cleanup_cb,
				 &subtree_arg);
	} else if (lpm_find_cover(lpm, ip, depth, &cover_ip,
				  &cover_depth, &cover_idx) == 0) {
		const struct next_hop_u *cover_nextu;

		cover_nextu = rcu_dereference(nh_tbl.entry[cover_idx]);
		if (nextu_is_any_connected(cover_nextu)) {
			lpm_subtree_walk(lpm, ip, depth,
					 subtree_walk_route_cleanup_cb,
					 &subtree_arg);
			subtree_walk_route_cleanup_cb(lpm, ip, depth, nh_idx,
						      &subtree_arg);
		}
	}
}

/*
 * This route has just been deleted. Create new arp links as required.
 */
static void
route_delete_relink_arp(struct lpm *lpm, uint32_t ip, uint8_t depth)
{
	const struct next_hop_u *nextu;
	uint32_t cover_ip;
	uint8_t cover_depth;
	uint32_t cover_nh_idx;
	const struct next_hop *array;
	int i;

	/*
	 * Find the cover of the entry just deleted. Walk all neighbours
	 * on that interface to see if there is work to do.
	 */
	if (lpm_find_cover(lpm, ip, depth, &cover_ip, &cover_depth,
				&cover_nh_idx)) {
		return;
	}

	/* Walk all the interfaces arp entries to do /32 processing */
	nextu = rcu_dereference(nh_tbl.entry[cover_nh_idx]);
	array = rcu_dereference(nextu->siblings);
	for (i = 0; i < nextu->nsiblings; i++) {
		const struct next_hop *next = array + i;
		const struct ifnet *ifp = nh4_get_ifp(next);

		if (!ifp)
			/* happens for local routes */
			continue;

		if (nh_is_connected(next))
			lltable_walk(ifp->if_lltable,
				     lle_routing_insert_arp_cb, NULL);
	}

	/* Now do the gateway processing. */
	walk_nhs_for_route_change(routing_arp_add_gw_nh_replace_cb);
}

/*
 * Add new route entry.
 */
int rt_insert(vrfid_t vrf_id, in_addr_t dst, uint8_t depth, uint32_t tableid,
	      uint8_t scope, uint8_t proto, struct next_hop hops[],
	      size_t size, bool replace)
{
	uint32_t idx = 0;
	int err_code;
	char b[INET_ADDRSTRLEN];
	struct lpm *lpm;
	struct vrf *vrf = NULL;

	/* use main table for local route */
	if (tableid == RT_TABLE_LOCAL)
		tableid = RT_TABLE_MAIN;

	/*
	 * This is reserved for our own purposes so don't accept any
	 * routes for it.
	 */
	if (tableid == RT_TABLE_UNSPEC)
		return -ENOENT;

	vrf = vrf_get_rcu(vrf_id);
	lpm = vrf ? rt_get_lpm(&vrf->v_rt4_head, tableid) : NULL;
	if (lpm == NULL) {
		if (is_nondefault_vrf(vrf_id)) {
			/*
			 * Should have a VRF by this point since we
			 * needed it to find the VRF ID from the table ID
			 */
			if (!vrf)
				return -ENOENT;
			/* Add refcount on default VRF */
			if (!vrf_find_or_create(VRF_DEFAULT_ID))
				return -ENOENT;
		} else {
			vrf = vrf_find_or_create(vrf_id);
			if (vrf == NULL)
				return -ENOENT;
		}

		lpm = rt_create_lpm(tableid, vrf);
		if (lpm == NULL) {
			err_code = -ENOENT;
			goto err;
		}
	} else if (rt_lpm_is_empty(lpm)) {
		/* Incr ref count when first route is added */
		if (!is_nondefault_vrf(vrf_id))
			vrf = vrf_find_or_create(vrf_id);
		else if (!vrf_find_or_create(VRF_DEFAULT_ID))
			return -ENOENT;
	}

	/*
	 * If a /32 and not a GW then  we want to set the GW (but
	 * not the GW flag) so that we do not share with non /32
	 * routes.  This allows us to then link the arp entries
	 * without using the arp for a /32 entry when we should not.
	 */
	if (depth == 32) {
		unsigned int i;

		for (i = 0; i < size; i++) {
			if (hops[i].flags & RTF_GATEWAY)
				continue;

			assert(hops[i].gateway == 0);
			hops[i].gateway = dst;
		}
	}

	err_code = route_nexthop_new(hops, size, proto, &idx);
	if (err_code < 0) {
		RTE_LOG(ERR, ROUTE,
			"route %s %s/%u failed - cannot create nexthop: %s\n",
			replace ? "replace" : "add",
			inet_ntop(AF_INET, &dst, b, sizeof(b)),
			depth,
			strerror(-err_code));
		goto err;
	}

	pthread_mutex_lock(&route_mutex);
	if (replace) {
		uint32_t old;

		route_delete_unlink_arp(vrf, lpm, ntohl(dst), depth);
		if (route_lpm_delete(vrf_id, lpm, dst, depth, &old,
				     scope) >= 0)
			nexthop_put(old);
		else
			replace = false;
	}

	err_code = route_lpm_add(vrf_id, lpm, dst, depth, idx, scope);
	if (err_code >= 0)
		route_change_link_arp(vrf, lpm, ntohl(dst), depth, idx, scope);

	pthread_mutex_unlock(&route_mutex);

	if (err_code < 0) {
		DP_LOG_W_VRF(ERR, ROUTE, vrf_id,
			     "route %s %s/%u idx %u failed (%d)\n",
			     replace ? "replace" : "add",
			     inet_ntop(AF_INET, &dst, b, sizeof(b)),
			     depth, idx, err_code);
		nexthop_put(idx);
		goto err;
	}

	DP_DEBUG_W_VRF(ROUTE, INFO, ROUTE, vrf_id,
		       "route %s %s/%u index %u table %u scope %u size %zu\n",
		       replace ? "replace" : "add",
		       inet_ntop(AF_INET, &dst, b, sizeof(b)),
		       depth, idx, tableid, scope, size);
	return 0;

err:
	if (vrf && (lpm == NULL || (lpm && rt_lpm_is_empty(lpm)))) {
		if (!is_nondefault_vrf(vrf->v_id))
			vrf_delete_by_ptr(vrf);
		else
			vrf_delete(VRF_DEFAULT_ID);
	}

	return err_code;
}

int rt_delete(vrfid_t vrf_id, in_addr_t dst, uint8_t depth,
	      uint32_t id, uint8_t scope)
{
	uint32_t idx;
	int err;
	char b[INET_ADDRSTRLEN];
	struct lpm *lpm;
	struct vrf *vrf = vrf_get_rcu(vrf_id);

	if (vrf == NULL)
		return -ENOENT;

	/* use main table for local route */
	if (id == RT_TABLE_LOCAL)
		id = RT_TABLE_MAIN;

	/*
	 * This is reserved for our own purposes so don't accept any
	 * deletes for it.
	 */
	if (id == RT_TABLE_UNSPEC)
		return -ENOENT;

	lpm = rt_get_lpm(&vrf->v_rt4_head, id);
	if (lpm == NULL || rt_lpm_is_empty(lpm))
		return -ENOENT;

	pthread_mutex_lock(&route_mutex);
	route_delete_unlink_arp(vrf, lpm, ntohl(dst), depth);
	err = route_lpm_delete(vrf_id, lpm, dst, depth, &idx, scope);
	if (err >= 0) {
		/* Drop reference count on nexthop entry. */
		nexthop_put(idx);
		route_delete_relink_arp(lpm, ntohl(dst), depth);
	}

	pthread_mutex_unlock(&route_mutex);

	/*unlock the VRF if this is the last route in this LPM*/
	if (rt_lpm_is_empty(lpm)) {
		if (!is_nondefault_vrf(vrf->v_id))
			vrf_delete_by_ptr(vrf);
		else
			vrf_delete(VRF_DEFAULT_ID);
	}

	if (err)
		/*
		 * Expected now we get all deletes from RIB and still act on
		 * link down and purge.
		 */
		return -ENOENT;

	DP_DEBUG_W_VRF(ROUTE, DEBUG, ROUTE, vrf_id,
		       "route delete %s/%u table %d, index %d\n",
		       inet_ntop(AF_INET, &dst, b, sizeof(b)), depth, id, idx);
	return 0;
}

/* cleaner for the next hop */
static void flush_cleanup(struct lpm *lpm __rte_unused,
			  uint32_t ip,
			  uint8_t depth,
			  int16_t scope __rte_unused,
			  uint32_t idx,
			  struct pd_obj_state_and_flags pd_state,
			  void *arg)
{
	struct vrf *vrf = arg;
	int ret;

	route_sw_stats[PD_OBJ_STATE_FULL]--;

	if (pd_state.created) {
		ret = fal_ip4_del_route(vrf->v_id, htonl(ip), depth,
					lpm_get_id(lpm));
		switch (ret) {
		case 0:
			route_hw_stats[pd_state.state]--;
			break;
		default:
			/* General failure */
			if (ret < 0) {
				char b[INET_ADDRSTRLEN];
				in_addr_t dst = htonl(ip);

				DP_LOG_W_VRF(
					ERR, ROUTE, vrf->v_id,
					"route delete %s/%d failed via FAL (%d)\n",
					inet_ntop(AF_INET, &dst, b, sizeof(b)),
					depth, ret);
			}
			break;
		}
	} else
		route_hw_stats[pd_state.state]--;

	nexthop_put(idx);
}

void rt_flush(struct vrf *vrf)
{
	unsigned int id;
	struct route_head rt_head = vrf->v_rt4_head;

	pthread_mutex_lock(&route_mutex);
	for (id = 0; id < rt_head.rt_rtm_max; id++) {
		struct lpm *lpm = rt_head.rt_table[id];

		/*
		 * Tables in VRFs alias those in the default VRF, so
		 * don't flush them
		 */
		if (is_nondefault_vrf(vrf->v_id) &&
		    id != RT_TABLE_UNSPEC)
			continue;

		if (lpm && !rt_lpm_is_empty(lpm)) {
			lpm_delete_all(lpm, flush_cleanup, vrf);
			/* decrement ref cnt for empty LPM */
			if (!rt_lpm_add_reserved_routes(lpm, vrf)) {
				DP_LOG_W_VRF(ERR, ROUTE, vrf->v_id,
					"Failed to replace reserved routes %s\n",
					vrf->v_name);
			}
			if (!is_nondefault_vrf(vrf->v_id))
				vrf_delete_by_ptr(vrf);
			else
				vrf_delete(VRF_DEFAULT_ID);

		}
	}
	pthread_mutex_unlock(&route_mutex);
}

void rt_flush_all(enum cont_src_en cont_src)
{
	vrfid_t vrf_id;
	struct vrf *vrf;

	if (cont_src == CONT_SRC_MAIN)
		VRF_FOREACH_KERNEL(vrf, vrf_id)
			rt_flush(vrf);
	else
		VRF_FOREACH_UPLINK(vrf, vrf_id)
			rt_flush(vrf);
}

void nexthop_tbl_init(void)
{
	struct next_hop nh_drop = {
		.flags = RTF_BLACKHOLE,
	};
	uint32_t idx;

	nexthop_hash = cds_lfht_new(NEXTHOP_HASH_TBL_MIN,
				    NEXTHOP_HASH_TBL_MIN,
				    NEXTHOP_HASH_TBL_SIZE,
				    CDS_LFHT_AUTO_RESIZE,
				    NULL);
	if (!nexthop_hash)
		rte_panic("nexthop_tbl_init: can't create nexthop hash\n");

	/* reserve a drop nexthop */
	if (nexthop_new(&nh_drop, 1, RTPROT_UNSPEC, &idx))
		rte_panic("%s: can't create drop nexthop\n", __func__);
	nextu_blackhole =
		rcu_dereference(nh_tbl.entry[idx]);
	if (!nextu_blackhole)
		rte_panic("%s: can't create drop nexthop\n", __func__);
}

int route_init(struct vrf *vrf)
{
	struct lpm *lpm;

	lpm = rt_create_lpm(RT_TABLE_MAIN, vrf);
	if (!lpm) {
		DP_LOG_W_VRF(ERR, ROUTE, vrf->v_id,
			     "unable to create IPv4 route table\n");
		return -1;
	}

	/*
	 * All tables other than main alias tables in the default VRF.
	 * This is necessary in order to easily support PBR setvrf + tableid.
	 */
	if (is_nondefault_vrf(vrf->v_id)) {
		struct vrf *default_vrf = vrf_get_rcu(VRF_DEFAULT_ID);
		struct route_head *rt_head = &default_vrf->v_rt4_head;
		uint32_t id;

		/*
		 * Stash the main table LPM point so it can be freed
		 * and also so it can be assigned over RT_TABLE_MAIN
		 * when unaliasing later.
		 */
		rcu_assign_pointer(vrf->v_rt4_head.rt_table[RT_TABLE_UNSPEC],
				   lpm);

		for (id = 1; id < rt_head->rt_rtm_max; id++) {
			struct lpm *src_lpm = rt_head->rt_table[id];

			if (!src_lpm || id == RT_TABLE_MAIN)
				continue;

			if (rt_lpm_resize(&vrf->v_rt4_head, id) < 0)
				return -1;

			rcu_assign_pointer(vrf->v_rt4_head.rt_table[id],
					   src_lpm);
		}
	}

	return 0;
}

void route_uninit(struct vrf *vrf, struct route_head *rt_head)
{
	uint32_t id;

	if (rt_head == NULL)
		return;
	for (id = 0; id < rt_head->rt_rtm_max; id++) {
		struct lpm *lpm = rt_head->rt_table[id];

		/*
		 * VRF tables alias those in the default VRF so don't
		 * free them.
		 */
		if (is_nondefault_vrf(vrf->v_id) &&
		    id != RT_TABLE_UNSPEC)
			continue;

		if (lpm) {
			/* rule_count == 0, means table has been flushed */
			if (lpm_rule_count(lpm) != 0) {
				if (!rt_lpm_is_empty(lpm)) {
					RTE_LOG(ERR, ROUTE,
						"%s:non empty lpm vrf %u table %u\n",
						__func__, vrf->v_id, id);
					return;
				}
				rt_lpm_del_reserved_routes(lpm, vrf);
			}
			lpm_free(lpm);
		}
	}
	free_huge(rt_head->rt_table, (rt_head->rt_rtm_max *
				      sizeof(struct lpm *)));
	rt_head->rt_table = NULL;
}

bool route_link_vrf_to_table(struct vrf *vrf, uint32_t tableid)
{
	struct vrf *default_vrf = vrf_get_rcu(VRF_DEFAULT_ID);
	struct route_head *rt_head = &default_vrf->v_rt4_head;
	struct lpm *new_lpm;

	new_lpm = rt_get_lpm(rt_head, tableid);
	if (!new_lpm) {
		new_lpm = rt_create_lpm(tableid, default_vrf);
		if (!new_lpm)
			return false;
	}

	/*
	 * Alias the main table to the given tableid in the default
	 * VRF.
	 */
	rcu_assign_pointer(vrf->v_rt4_head.rt_table[RT_TABLE_MAIN],
			   new_lpm);

	return true;
}

bool route_unlink_vrf_from_table(struct vrf *vrf)
{
	struct lpm *old_main_lpm = vrf->v_rt4_head.rt_table[
		RT_TABLE_UNSPEC];

	/*
	 * Unalias the main table. We require the pointer to be valid
	 * so we use the table we initially created the VRF with.
	 */
	rcu_assign_pointer(vrf->v_rt4_head.rt_table[RT_TABLE_MAIN],
			   old_main_lpm);

	return true;
}

void rt_print_nexthop(json_writer_t *json, uint32_t next_hop)
{
	const struct next_hop_u *nextu =
		rcu_dereference(nh_tbl.entry[next_hop]);
	const struct next_hop *array;
	unsigned int i, j;

	jsonw_uint_field(json, "nh_index", next_hop);
	if (unlikely(!nextu))
		return;
	array = rcu_dereference(nextu->siblings);
	jsonw_uint_field(json, "nh_refcount", nextu->refcount);
	jsonw_name(json, "next_hop");
	jsonw_start_array(json);
	for (i = 0; i < nextu->nsiblings; i++) {
		const struct next_hop *next = array + i;
		const struct ifnet *ifp;

		jsonw_start_object(json);
		if (next->flags & RTF_BLACKHOLE)
			jsonw_string_field(json, "state", "blackhole");
		else if (next->flags & RTF_REJECT ||
			 next->flags & RTF_NOROUTE)
			jsonw_string_field(json, "state", "unreachable");
		else if (next->flags & RTF_LOCAL)
			jsonw_string_field(json, "state", "local");
		else if (next->flags & RTF_SLOWPATH)
			jsonw_string_field(json,
					   "state", "non-dataplane interface");
		else if (next->flags & RTF_GATEWAY) {
			char b1[INET_ADDRSTRLEN];

			jsonw_string_field(json, "state", "gateway");
			jsonw_string_field(json, "via",
					   inet_ntop(AF_INET, &next->gateway,
						     b1, sizeof(b1)));
		} else
			jsonw_string_field(json, "state", "directly connected");

		if (next->flags & RTF_DEAD)
			jsonw_bool_field(json, "dead", true);
		if (next->flags & RTF_NEIGH_PRESENT)
			jsonw_bool_field(json, "neigh_present", true);
		if (next->flags & RTF_NEIGH_CREATED)
			jsonw_bool_field(json, "neigh_created", true);

		ifp = nh4_get_ifp(next);
		if (ifp)
			jsonw_string_field(json, "ifname", ifp->if_name);

		if (nh_outlabels_present(&next->outlabels)) {
			label_t label;

			jsonw_name(json, "labels");
			jsonw_start_array(json);

			NH_FOREACH_OUTLABEL_TOP(&next->outlabels, j, label)
				jsonw_uint(json, label);

			jsonw_end_array(json);
		}

		jsonw_end_object(json);
	}
	jsonw_end_array(json);
}

/*
 * Walk FIB table.
 */
static void rt_local_display(
	struct lpm *lpm __rte_unused,
	uint32_t ip, uint8_t depth __rte_unused,
	int16_t scope __rte_unused,
	uint32_t next_hop,
	struct pd_obj_state_and_flags pd_state __rte_unused,
	void *arg)
{
	FILE *f = arg;
	in_addr_t dst = htonl(ip);
	char b[INET_ADDRSTRLEN];
	const struct next_hop_u *nextu =
		rcu_dereference(nh_tbl.entry[next_hop]);
	const struct next_hop *nh;

	if (unlikely(!nextu))
		return;
	nh = rcu_dereference(nextu->siblings);

	if (nh->flags & RTF_LOCAL && !rt_is_reserved(ip, depth, scope))
		fprintf(f, "\t%s\n", inet_ntop(AF_INET, &dst, b, sizeof(b)));
}

static void __rt_display(json_writer_t *json, in_addr_t *dst, uint8_t depth,
			 int16_t scope, const struct next_hop_u *nextu,
			 uint32_t next_hop)
{
	char b1[INET_ADDRSTRLEN];
	char b2[INET6_ADDRSTRLEN]; /* extra room for mask, not for ipv6 here */

	jsonw_start_object(json);

	sprintf(b2, "%s/%u",
		inet_ntop(AF_INET, dst, b1, sizeof(b1)), depth);
	jsonw_string_field(json, "prefix", b2);
	jsonw_int_field(json, "scope", scope);
	jsonw_uint_field(json, "proto", nextu->proto);
	rt_print_nexthop(json, next_hop);

	jsonw_end_object(json);
}

static void rt_display(struct lpm *lpm __rte_unused,
		       uint32_t ip, uint8_t depth, int16_t scope,
		       uint32_t next_hop,
		       struct pd_obj_state_and_flags pd_state __rte_unused,
		       void *arg)
{
	json_writer_t *json = arg;
	in_addr_t dst = htonl(ip);

	const struct next_hop_u *nextu =
		rcu_dereference(nh_tbl.entry[next_hop]);
	const struct next_hop *nh;

	if (unlikely(!nextu))
		return;
	nh = rcu_dereference(nextu->siblings);
	/* Filter local route being displayed */
	if (nh->flags & RTF_LOCAL)
		return;

	/* Don't show if any paths are NEIGH_CREATED. */
	if (nextu_nc_count(nextu))
		return;

	if (rt_is_reserved(ip, depth, scope))
		return;

	__rt_display(json, &dst, depth, scope, nextu, next_hop);
}

static void rt_display_all(struct lpm *lpm __rte_unused,
			   uint32_t ip, uint8_t depth, int16_t scope,
			   uint32_t next_hop,
			   struct pd_obj_state_and_flags pd_state __rte_unused,
			   void *arg)
{
	json_writer_t *json = arg;
	in_addr_t dst = htonl(ip);
	const struct next_hop_u *nextu =
		rcu_dereference(nh_tbl.entry[next_hop]);

	if (unlikely(!nextu))
		return;
	__rt_display(json, &dst, depth, scope, nextu, next_hop);
}

/* Route rule list (RB-tree) is not RCU safe */
static uint32_t
lpm_walk_safe(struct lpm *lpm, lpm_walk_func_t func,
		  struct lpm_walk_arg *r_arg)
{
	uint32_t ret;

	pthread_mutex_lock(&route_mutex);
	ret = lpm_walk(lpm, func, r_arg);
	pthread_mutex_unlock(&route_mutex);

	return ret;
}

static void
lpm_walk_all_safe(struct lpm *lpm, lpm_walk_func_t func, void *arg)
{
	struct lpm_walk_arg r_arg = {
		.is_segment = false,
		.walk_arg = arg,
	};

	lpm_walk_safe(lpm, func, &r_arg);
}

enum if_state_rx {
	IF_RX_LINK_DEL,
};

static void rt_if_dead(struct lpm *lpm, struct vrf *vrf,
		       uint32_t ip, uint8_t depth, int16_t scope,
		       uint32_t idx, void *arg, enum if_state_rx state_rx)
{
	struct next_hop_u *nextu = rcu_dereference(nh_tbl.entry[idx]);
	const struct ifnet *ifp = arg;
	unsigned int i, matches = 0;

	for (i = 0; i < nextu->nsiblings; i++) {
		struct next_hop *nh = nextu->siblings + i;

		if (nh4_get_ifp(nh) == ifp) {
			/* No longer check if connected, as kernel will not
			 * signal explicitly for flushing
			 */
			nh->flags |= RTF_DEAD;
			++matches;
		} else if (nh->flags & RTF_DEAD)
			++matches;
	}

	if (matches == 0)
		return;

	if (matches == nextu->nsiblings || state_rx == IF_RX_LINK_DEL) {
		/*
		 * Delete entire route if;
		 * Either all nh's for this route are dead
		 * Or interface on one nh has been deleted. This mimics Kernel
		 * behaviour but is bad as we have other ECMP nh's available
		 */
		route_lpm_delete(vrf->v_id, lpm, htonl(ip), depth, NULL,
				 scope);
		nexthop_put(idx);
	}
}

/* Interface is being deleted */
static void rt_if_deleted(struct lpm *lpm, struct vrf *vrf,
			  uint32_t ip, uint8_t depth, int16_t scope,
			  uint32_t idx,
			  struct pd_obj_state_and_flags pd_state __rte_unused,
			  void *arg)
{
	rt_if_dead(lpm, vrf, ip, depth, scope, idx, arg, IF_RX_LINK_DEL);
}

static void rt_if_clear_slowpath_flag(
	struct lpm *lpm __rte_unused,
	struct vrf *vrf __rte_unused,
	uint32_t ip __rte_unused,
	uint8_t depth __rte_unused,
	int16_t scope __rte_unused,
	uint32_t idx,
	struct pd_obj_state_and_flags pd_state __rte_unused,
	void *arg)
{
	struct next_hop_u *nextu = rcu_dereference(nh_tbl.entry[idx]);
	const struct ifnet *ifp = arg;
	unsigned int i;

	for (i = 0; i < nextu->nsiblings; i++) {
		struct next_hop *nh = nextu->siblings + i;

		if (nh4_get_ifp(nh) == ifp)
			nh->flags &= ~RTF_SLOWPATH;
	}
}

static void rt_if_set_slowpath_flag(
	struct lpm *lpm __rte_unused,
	struct vrf *vrf __rte_unused,
	uint32_t ip __rte_unused,
	uint8_t depth __rte_unused,
	int16_t scope __rte_unused,
	uint32_t idx,
	struct pd_obj_state_and_flags pd_state __rte_unused,
	void *arg)
{
	struct next_hop_u *nextu = rcu_dereference(nh_tbl.entry[idx]);
	const struct ifnet *ifp = arg;
	unsigned int i;

	for (i = 0; i < nextu->nsiblings; i++) {
		struct next_hop *nh = nextu->siblings + i;

		if (nh4_get_ifp(nh) == ifp)
			nh->flags |= RTF_SLOWPATH;
	}
}

struct rt_vrf_lpm_walk_ctx {
	struct vrf *vrf;
	void (*func)(struct lpm *lpm, struct vrf *vrf,
		     uint32_t ip, uint8_t depth, int16_t scope,
		     uint32_t next_hop, struct pd_obj_state_and_flags pd_state,
		     void *arg);
	void *arg;
};

static void rt_vrf_lpm_walk_cb(struct lpm *lpm, uint32_t ip,
			       uint8_t depth, int16_t scope,
			       uint32_t idx,
			       struct pd_obj_state_and_flags pd_state,
			       void *arg)
{
	const struct rt_vrf_lpm_walk_ctx *ctx = arg;

	ctx->func(lpm, ctx->vrf, ip, depth, scope, idx, pd_state, ctx->arg);
}


static void rt_lpm_walk_util(
	void (*func)(struct lpm *lpm, struct vrf *vrf,
		     uint32_t ip, uint8_t depth, int16_t scope,
		     uint32_t next_hop, struct pd_obj_state_and_flags pd_state,
		     void *arg),
	void *arg)
{
	unsigned int id;
	vrfid_t vrf_id;
	struct vrf *vrf;

	VRF_FOREACH(vrf, vrf_id) {
		for (id = 1; id < vrf->v_rt4_head.rt_rtm_max; id++) {
			struct lpm *lpm = vrf->v_rt4_head.rt_table[id];
			struct rt_vrf_lpm_walk_ctx ctx = {
				.vrf = vrf,
				.func = func,
				.arg = arg,
			};

			/*
			 * We have some alaising of vrf table.
			 *
			 * Each table is linked into every vrf,
			 * so only show tables less than MAIN
			 * in the default vrf. Each non default vrf has
			 * aliased a default vrf table, starting with an
			 * ID of 256. Show these in the non default vrf.
			 */
			if (vrf->v_id == VRF_DEFAULT_ID &&
			    id > RT_TABLE_MAIN)
				continue;
			if (vrf->v_id != VRF_DEFAULT_ID &&
			    id != RT_TABLE_MAIN)
				continue;

			if (lpm && !rt_lpm_is_empty(lpm)) {
				lpm_walk_all_safe(lpm, rt_vrf_lpm_walk_cb,
						      &ctx);
				if (rt_lpm_is_empty(lpm)) {
					if (!is_nondefault_vrf(vrf->v_id))
						vrf_delete_by_ptr(vrf);
					else
						vrf_delete(VRF_DEFAULT_ID);
				}
			}
		}
	}
}

/* Interface is being deleted clear all routes */
static void rt_if_purge(struct ifnet *ifp, uint32_t idx __unused)
{
	/*
	 * Walk through all the VRFs to delete the routes
	 * pointing to this interface.
	 */
	rt_lpm_walk_util(rt_if_deleted, ifp);
}

/* Explicitly stop routes pointing to this interface punting to slowpath */
void rt_if_handle_in_dataplane(struct ifnet *ifp)
{
	rt_lpm_walk_util(rt_if_clear_slowpath_flag, ifp);
}

/* Explicitly make routes pointing to this interface punt to slowpath */
void rt_if_punt_to_slowpath(struct ifnet *ifp)
{
	rt_lpm_walk_util(rt_if_set_slowpath_flag, ifp);
}

int rt_walk(struct route_head *rt_head, json_writer_t *json, uint32_t id,
	    uint32_t cnt, enum rt_walk_type type)
{
	lpm_walk_func_t cb = rt_display;
	struct lpm *lpm = rt_get_lpm(rt_head, id);
	struct lpm_walk_arg arg = {
		.is_segment = (cnt != UINT32_MAX),
		.walk_arg = json,
		.addr = 0,
		.depth = 0,
		.cnt = cnt,
	};

	if (lpm == NULL) {
		RTE_LOG(ERR, ROUTE, "Unknown route table\n");
		return 0;
	}

	if (type == RT_WALK_ALL)
		cb = rt_display_all;

	if (lpm_walk_safe(lpm, cb, &arg)) {
		jsonw_start_object(json);
		jsonw_string_field(json, "prefix", "more");
		jsonw_end_object(json);
	}

	return 0;
}


int rt_walk_next(struct route_head *rt_head, json_writer_t *json,
		 uint32_t id, const struct in_addr *addr,
		 uint8_t plen, uint32_t cnt, enum rt_walk_type type)
{
	lpm_walk_func_t cb = rt_display;
	struct lpm *lpm = rt_get_lpm(rt_head, id);
	struct lpm_walk_arg arg = {
		.is_segment = true,
		.get_next = true,
		.walk_arg = json,
		.depth = plen,
		.cnt = cnt,
		.addr = ntohl(addr->s_addr),
	};

	if (lpm == NULL) {
		RTE_LOG(ERR, ROUTE, "Unknown route table\n");
		return 0;
	}

	if (type == RT_WALK_ALL)
		cb = rt_display_all;

	if (lpm_walk_safe(lpm, cb, &arg)) {
		jsonw_start_object(json);
		jsonw_string_field(json, "prefix", "more");
		jsonw_end_object(json);
	}

	return 0;
}

int rt_local_show(struct route_head *rt_head, uint32_t id, FILE *f)
{
	struct lpm *lpm = rt_get_lpm(rt_head, id);

	if (lpm == NULL) {
		RTE_LOG(ERR, ROUTE, "Unknown route table\n");
		return 0;
	}

	lpm_walk_all_safe(lpm, rt_local_display, f);

	return 0;
}

int rt_show(struct route_head *rt_head, json_writer_t *json, uint32_t tblid,
	    const struct in_addr *addr)
{
	struct lpm *lpm = rt_get_lpm(rt_head, tblid);
	uint32_t next_hop;

	if (lpm == NULL) {
		RTE_LOG(ERR, ROUTE, "Unknown route table\n");
		return 0;
	}

	jsonw_start_object(json);
	jsonw_string_field(json, "address", inet_ntoa(*addr));

	if (lpm_lookup(lpm, ntohl(addr->s_addr), &next_hop) != 0)
		jsonw_string_field(json, "state", "nomatch");
	else
		rt_print_nexthop(json, next_hop);
	jsonw_end_object(json);
	return 0;
}

static void rt_summarize(struct lpm *lpm __rte_unused,
			 uint32_t ip, uint8_t depth,
			 int16_t scope,
			 uint32_t nh_idx,
			 struct pd_obj_state_and_flags pd_state __rte_unused,
			 void *arg)
{
	uint32_t *rt_used = arg;
	const struct next_hop_u *nextu = rcu_dereference(nh_tbl.entry[nh_idx]);
	const struct next_hop *nh;

	if (unlikely(!nextu))
		return;
	nh = rcu_dereference(nextu->siblings);
	/* Filter local route being displayed */
	if (nh->flags & RTF_LOCAL)
		return;

	/* Don't show if any paths are NEIGH_CREATED */
	if (nextu_nc_count(nextu))
		return;

	if (rt_is_reserved(ip, depth, scope))
		return;

	++rt_used[depth];
}

static double nexthop_hash_load_factor(void)
{
	unsigned long count;
	long dummy;
	double factor;

	cds_lfht_count_nodes(nexthop_hash, &dummy, &count, &dummy);
	factor = count / NEXTHOP_HASH_TBL_SIZE;
	return factor;
}

int rt_stats(struct route_head *rt_head, json_writer_t *json, uint32_t id)
{
	uint8_t depth;
	unsigned int total = 0;
	uint32_t rt_used[LPM_MAX_DEPTH] = { 0 };
	struct lpm *lpm = rt_get_lpm(rt_head, id);

	if (lpm == NULL) {
		RTE_LOG(ERR, ROUTE, "Unknown route table\n");
		return 0;
	}

	lpm_walk_all_safe(lpm, rt_summarize, rt_used);
	jsonw_name(json, "prefix");
	jsonw_start_object(json);
	for (depth = 0; depth < LPM_MAX_DEPTH; depth++) {
		total += rt_used[depth];
		if (rt_used[depth]) {
			char buf[20];

			snprintf(buf, sizeof(buf), "%u", depth);
			jsonw_uint_field(json, buf, rt_used[depth]);
		}
	}
	jsonw_end_object(json);

	jsonw_uint_field(json, "total", total);
	jsonw_uint_field(json, "used", lpm_tbl8_count(lpm));
	jsonw_uint_field(json, "free", lpm_tbl8_free_count(lpm));

	jsonw_name(json, "nexthop");
	jsonw_start_object(json);
	jsonw_uint_field(json, "used", nh_tbl.in_use);
	jsonw_uint_field(json, "free", NEXTHOP_HASH_TBL_SIZE - nh_tbl.in_use);
	jsonw_uint_field(json, "hash",
			 100. * nexthop_hash_load_factor());
	jsonw_uint_field(json, "neigh_present", nh_tbl.neigh_present);
	jsonw_uint_field(json, "neigh_created", nh_tbl.neigh_created);
	jsonw_end_object(json);

	return 0;
}

/*
 * Get egress interface for destination address.
 *
 * Must only be used on master thread.
 * Note for multipath routes, the first interface is always returned.
 */
struct ifnet *nhif_dst_lookup(const struct vrf *vrf,
			      in_addr_t dst,
			      bool *connected)
{
	struct ifnet *ifp;
	const struct next_hop_u *nextu;
	const struct next_hop *next;
	uint32_t nhindex;

	if (lpm_lookup(vrf->v_rt4_head.rt_table[RT_TABLE_MAIN],
			   ntohl(dst), &nhindex) != 0)
		return NULL;

	nextu = nh_tbl.entry[nhindex];
	if (nextu == NULL)
		return NULL;

	next = nextu->siblings;
	if (next == NULL)
		return NULL;

	ifp = nh4_get_ifp(next);
	if (ifp && connected)
		*connected = nh_is_connected(next);

	return ifp;
}

/*
 * Lookup NH information based on NH index, and use the hash in case
 * the NH is a multi-path nexthop
 *
 * INPUT:
 *    nhindex - NH index
 *    hash    - Hash value used to obtain the path information in case
 *              of multi-path nexthop
 * OUTPUT:
 *    nh      - IP address of the next hop
 *    ifindex - If index of the outgoing interface
 */
int nh_lookup_by_index(uint32_t nhindex, uint32_t hash, in_addr_t *nh,
		       uint32_t *ifindex)
{
	const struct next_hop_u *nextu;
	struct next_hop *next;
	struct ifnet *ifp;
	uint32_t size;

	nextu = rcu_dereference(nh_tbl.entry[nhindex]);
	if (nextu == NULL)
		return -1;

	next = nextu->siblings;
	if (!next)
		return -1;

	size = nextu->nsiblings;
	if (size > 1)
		next = nexthop_mp_select(next, size, hash);

	if (next->flags & RTF_GATEWAY)
		*nh = next->gateway;
	else
		*nh = INADDR_ANY;

	ifp = nh4_get_ifp(next);
	if (!ifp)
		return -1;

	*ifindex = ifp->if_index;
	return 0;
}

static void
route_create_arp(struct vrf *vrf, struct lpm *lpm,
		 struct in_addr *ip, struct llentry *lle)
{
	struct next_hop_u *nextu;
	uint32_t nh_idx;
	struct next_hop *nh;
	struct next_hop *cover_nh;
	struct ifnet *ifp = rcu_dereference(lle->ifp);
	int sibling;
	int size;

	if (lpm_lookup(lpm, ntohl(ip->s_addr), &nh_idx) == 0) {
		nextu = rcu_dereference(nh_tbl.entry[nh_idx]);

		/*
		 * Note that this does not support a connected with multiple
		 * paths that use the same ifp.
		 */
		cover_nh = nextu_find_path_using_ifp(nextu, ifp, &sibling);
		if (cover_nh && nh_is_connected(cover_nh)) {
			/*
			 * Have a connected cover so create a new entry for
			 * this. Will only be 1 NEIGH_CREATED path, but
			 * need to inherit other paths from the cover.
			 */
			nh = nexthop_create_copy(nextu, &size);
			if (!nh)
				return;

			/*
			 * Set the correct NH to be NEIGH_CREATED. As this
			 * is copied from the cover nextu, the sibling gives
			 * the NH for the correct interface
			 */
			nh4_set_neigh_created(&nh[sibling], lle);
			/*
			 * This is a /32 we are creating, therefore not a GW.
			 * Set the GW (but not the flag) so that we do not
			 * share with non /32 routes such as the connected
			 * cover.
			 */
			assert(nh[sibling].gateway == 0);
			nh[sibling].gateway = ip->s_addr;
			if (route_nexthop_new(nh, size, RTPROT_UNSPEC,
					      &nh_idx) < 0) {
				free(nh);
				return;
			}
			route_lpm_add(vrf->v_id, lpm, ip->s_addr,
				      32, nh_idx, RT_SCOPE_LINK);
			free(nh);
		}
	}
}

/*
 * On an arp del, should we clear NEIGH_PRESENT from this NH.
 */
static enum nh_change routing_arp_del_gw_nh_replace_cb(struct next_hop *next,
						       int sibling __unused,
						       void *arg)
{
	struct llentry *lle = arg;
	struct in_addr *ip = ll_ipv4_addr(lle);
	struct ifnet *ifp = rcu_dereference(lle->ifp);

	if (!nh_is_gw(next) || (next->gateway != ip->s_addr))
		return NH_NO_CHANGE;
	if (nh4_get_ifp(next) != ifp)
		return NH_NO_CHANGE;
	if (nh_is_local(next) || !nh4_is_neigh_present(next))
		return NH_NO_CHANGE;

	return NH_CLEAR_NEIGH_PRESENT;
}


static void
walk_nhs_for_arp_change(struct llentry *lle,
			enum nh_change (*upd_neigh_present_cb)(
				struct next_hop *next,
				int sibling,
				void *arg))
{
	struct next_hop_u *nhu;
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;

	ASSERT_MASTER();

	cds_lfht_for_each(nexthop_hash, &iter, node) {
		nhu = caa_container_of(node, struct next_hop_u, nh_node);
		route_nh_replace(nhu, nhu->index, lle, NULL,
				 upd_neigh_present_cb, lle);
	}
}

struct arp_add_nh_replace_arg {
	struct ifnet *ifp;
	bool count;
};

/*
 * On an arp add, should we set this NH as NEIGH_PRESENT, OR NEIGH_CREATED
 *
 * Set to NEIGH_PRESENT in the case where the route existed already, but not
 * because of an ARP entry. If there are any NHs that are NEIGH_CREATED then
 * it only exists due to the ARP entry, so this hop can become NEIGH_CREATED
 * too.
 */
static enum nh_change routing_arp_add_nh_replace_cb(struct next_hop *next,
						    int sibling __unused,
						    void *arg)
{
	struct arp_add_nh_replace_arg *args = arg;

	if (!nh_is_connected(next))
		return NH_NO_CHANGE;
	if (nh4_is_neigh_present(next) || nh4_is_neigh_created(next))
		return NH_NO_CHANGE;
	if (args->ifp != nh4_get_ifp(next))
		return NH_NO_CHANGE;

	if (args->count)
		return NH_SET_NEIGH_CREATED;

	return NH_SET_NEIGH_PRESENT;
}

/*
 * On an arp del, should we clear NEIGH_PRESENT from this NH.
 */
static enum nh_change routing_arp_del_nh_replace_cb(struct next_hop *next,
						    int sibling __unused,
						    void *arg)
{
	struct ifnet *ifp = arg;

	if (!nh_is_connected(next) || !nh4_is_neigh_present(next))
		return NH_NO_CHANGE;
	if (ifp != nh4_get_ifp(next))
		return NH_NO_CHANGE;

	return NH_CLEAR_NEIGH_PRESENT;
}

struct arp_remove_purge_arg {
	int count; /* Count of number of NEIGH_CREATED in parent nextu */
	int sibling; /* Sibling that had the arp entry removed */
};

/*
 * Do we need to purge this NH. If the route was NEIGH_CREATED (any of the
 * paths were NEIGH_CREATED) and this path has had the ARP entry removed then
 * it either needs to be removed, or have NEIGH_CREATED removed.
 * If it is the last NEIGH_CREATED path then all paths to be removed.
 * If there will still be a NEIGH_CREATED path then this path should have
 * NEIGH_CREATED removed and revert back to inheriting from the cover.
 */
static enum nh_change arp_removal_nh_purge_cb(struct next_hop *next __unused,
					      int sibling,
					      void *arg)
{
	struct arp_remove_purge_arg *args = arg;

	if (sibling == args->sibling) {
		if (args->count > 1)
			return NH_CLEAR_NEIGH_CREATED;
		else
			return NH_DELETE;
	}

	if (args->count > 1)
		return NH_NO_CHANGE;

	return NH_DELETE;
}

/*
 * When we are given a new arp entry we need to insert entries into the
 * routing table(s) for it.
 *
 * For the VRF the interface is in:
 *  - is there is already a /32 for this addr, mark it as NEIGH_PRESENT
 *    if connected.
 *  - is not then create it and mark it as NEIGH_PRESENT & NEIGH_CREATED
 *    if cover is connected.
 *
 *  - Walk the NHs that use this interface and have the GW as this IP addr.
 *    - mark as NEIGH_PRESESNT.
 *
 * All entries marked as NEIGH_PRESENT will then have the arp ptr stored,
 * so that forwarders can use that are entry without a lookup.
 */
void
routing_insert_arp_safe(struct llentry *lle, bool arp_change)
{
	struct in_addr *ip = ll_ipv4_addr(lle);
	struct vrf *vrf = get_vrf(if_vrfid(lle->ifp));
	struct lpm *lpm;
	struct next_hop_u *nextu;
	uint32_t nh_idx;
	struct ifnet *ifp = rcu_dereference(lle->ifp);
	struct next_hop *nh;
	int sibling;

	lpm = rcu_dereference(vrf->v_rt4_head.rt_table[RT_TABLE_MAIN]);
	pthread_mutex_lock(&route_mutex);
	if (lpm_lookup_exact(lpm, ntohl(ip->s_addr), 32, &nh_idx) == 0) {
		/* We already have a /32 so add the shortcut if connected */
		nextu = rcu_dereference(nh_tbl.entry[nh_idx]);

		/*
		 * Do we already have a nh for this interface?
		 * If so then we might need to modify it. As this is
		 * called when a route changes, we migh also need to
		 * modify the set of NHs, to reflect the ones the
		 * cover has.
		 */
		nh = nextu_find_path_using_ifp(nextu, ifp, &sibling);
		if (nh) {
			struct arp_add_nh_replace_arg arg = {
				.ifp = ifp,
				.count = nextu_nc_count(nextu),
			};

			route_nh_replace(nextu, nh_idx, lle, NULL,
					 routing_arp_add_nh_replace_cb, &arg);
		}
	} else {
		/* Have to create a /32. but only if cover is connected. */
		route_create_arp(vrf, lpm, ip, lle);
	}
	pthread_mutex_unlock(&route_mutex);

	/*
	 * If this is not an arp change don't do this here as it will lead
	 * to something like an n squared issue as we call this func for all
	 * lle entries. The caller will do an equivalent after.
	 */
	if (arp_change)
		/*
		 * Now walk the NHs using this interface that have the GW
		 * set as this IP address. For each of them add the link
		 * to the arp entry and mark as NEIGH_PRESENT.
		 */
		walk_nhs_for_arp_change(lle, routing_arp_add_gw_nh_replace_cb);
}

/*
 * The arp entry is going away. If we have any references to it then clean them
 * up.
 */
void
routing_remove_arp_safe(struct llentry *lle)
{
	struct in_addr *ip = ll_ipv4_addr(lle);
	struct vrf *vrf = get_vrf(if_vrfid(lle->ifp));
	struct lpm *lpm;
	struct next_hop_u *nextu;
	uint32_t nh_idx;
	struct ifnet *ifp = rcu_dereference(lle->ifp);
	int sibling;
	struct next_hop *nh;

	lpm = rcu_dereference(vrf->v_rt4_head.rt_table[RT_TABLE_MAIN]);
	pthread_mutex_lock(&route_mutex);
	if (lpm_lookup_exact(lpm, ntohl(ip->s_addr), 32, &nh_idx) == 0) {
		/* We have a /32 so unlink the arp (if there) */
		nextu = rcu_dereference(nh_tbl.entry[nh_idx]);

		/* Do we already have a nh for this interface? */
		nh = nextu_find_path_using_ifp(nextu, ifp, &sibling);
		if (nh && nh4_is_neigh_created(nh)) {
			/* Are we removing a path or the entire NH */
			if (nextu->nsiblings == 1) {
				route_lpm_delete(vrf->v_id,
						     lpm, ip->s_addr, 32,
						     &nh_idx, RT_SCOPE_LINK);
				nexthop_put(nh_idx);
			} else {
				struct arp_remove_purge_arg args = {
					.count = nextu_nc_count(nextu),
					.sibling = sibling,
				};
				int del;
				uint32_t new_nh_idx;

				del = route_nh_replace(nextu, nh_idx, lle,
						       &new_nh_idx,
						       arp_removal_nh_purge_cb,
						       &args);
				/* Can not delete a subset of paths here */
				if (del == nextu->nsiblings) {
					route_lpm_delete(vrf->v_id,
							     lpm, ip->s_addr,
							     32, &nh_idx,
							     RT_SCOPE_LINK);
					nexthop_put(nh_idx);
				}
			}
		} else {
			route_nh_replace(nextu, nh_idx, NULL, NULL,
					routing_arp_del_nh_replace_cb, ifp);
		}
	}
	pthread_mutex_unlock(&route_mutex);

	/*
	 * Now walk the NHs using this interface that have the GW
	 * set as this IP address. For each of them remove the link
	 * to the arp entry as it is going away
	 */
	walk_nhs_for_arp_change(lle, routing_arp_del_gw_nh_replace_cb);
}

uint32_t *route_sw_stats_get(void)
{
	return route_sw_stats;
}

uint32_t *route_hw_stats_get(void)
{
	return route_hw_stats;
}


struct rt_show_subset {
	json_writer_t *json;
	enum pd_obj_state subset;
	vrfid_t vrf;
};

static void rt_show_subset(struct lpm *lpm, struct vrf *vrf,
			   uint32_t ip, uint8_t depth, int16_t scope,
			   uint32_t idx,
			   struct pd_obj_state_and_flags pd_state,
			   void *arg)
{
	struct rt_show_subset *subset = arg;

	if (subset->vrf != vrf->v_id) {
		subset->vrf = vrf->v_id;
		jsonw_start_object(subset->json);
		jsonw_uint_field(subset->json, "vrf_id",
				 vrf_get_external_id(vrf->v_id));
		jsonw_uint_field(subset->json, "table",
				 lpm_get_id(lpm));
		jsonw_end_object(subset->json);
	}

	if (subset->subset == pd_state.state)
		rt_display_all(lpm, ip, depth, scope, idx, pd_state,
			       subset->json);
}

/*
 * Return the json for the given subset of stats.
 */
int route_get_pd_subset_data(json_writer_t *json, enum pd_obj_state subset)
{
	struct rt_show_subset arg = {
		.json = json,
		.subset = subset,
		.vrf = VRF_INVALID_ID,
	};
	rt_lpm_walk_util(rt_show_subset, &arg);

	return 0;
}

static const struct dp_event_ops route_events = {
	.if_index_unset = rt_if_purge,
};

DP_STARTUP_EVENT_REGISTER(route_events);

