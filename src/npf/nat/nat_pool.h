/*
 * Copyright (c) 2019, AT&T Intellectual Property.  All rights reserved.
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 */

#ifndef _NAT_POOL_H_
#define _NAT_POOL_H_

#include <stdint.h>
#include <stdio.h>
#include <rte_atomic.h>
#include <netinet/in.h>

#include "urcu.h"
#include "npf/nat/nat_proto.h"
#include "npf/nat/nat_pool_public.h"


enum nat_pool_type {
	NPT_CGNAT,
};

/*
 * Max number of address ranges or prefixes in an address pool
 */
#define NAT_POOL_MAX_RANGES	16

/*
 * NAT pool address ranges may be configured as a range or a prefix and
 * length.  All types are converted to a useable range.
 *
 * NPA_PREFIX    prefix and mask. First and last address are *not* useable
 * NPA_RANGE     address range
 */
enum nat_pool_range_type {
	NPA_PREFIX,
	NPA_RANGE,
};

/*
 * NAT_AP_PAIRED     A source is always allocated a mapping from the same
 *                   pool address.
 * NAT_AP_ARBITRARY  A source may be allocated a mapping from any pool
 *                   address.
 */
enum nat_addr_pooling {
	NAT_AP_PAIRED,
	NAT_AP_ARBITRARY,
};

/*
 * NAT_AA_ROUND_ROBIN  Pool address is incremented for each new source
 * NAT_AA_SEQUENTIAL   All ports from a pool address are exhausted before
 *                     moving to the next pool address.
 */
enum nat_addr_allcn {
	NAT_AA_ROUND_ROBIN,
	NAT_AA_SEQUENTIAL,
};

/*
 * Port allocation within either full-range or block (depending on use).
 */
enum nat_port_allcn {
	NAT_PA_RANDOM,
	NAT_PA_SEQUENTIAL,
};

/*
 * NAT pool flags
 *
 * Only active pools may be returned in a hash table lookup.  The active flag
 * is cleared when a pool is unconfigured.  However a pool is not freed until
 * all references are released.
 */
#define NP_ACTIVE	0x0001

/*
 * An address pool has one or more nat_pool_range structures.  Regardless of
 * the type configured, all nat_pool_range entries are converted to a useable
 * range and stored in na_addr_start and naddr_stop.  Prefixes and addresses
 * are in host byte order.
 */
struct nat_pool_range {
	char			*pr_name;
	uint32_t		pr_prefix;
	uint8_t			pr_mask;
	enum nat_pool_range_type pr_type;
	uint8_t			pr_range;	/* Range number */
	uint8_t			pr_pad1[1];

	/* address range */
	uint32_t		pr_addr_start;
	uint32_t		pr_addr_stop;
	uint32_t		pr_naddrs;
};

struct nat_pool_ranges {
	uint8_t			nr_nranges;	/* number of addr ranges */
	uint32_t		nr_naddrs;	/* total address count */
	struct nat_pool_range	nr_range[NAT_POOL_MAX_RANGES];

	/*
	 * Record of last allocated address per differentiated protocol.
	 */
	rte_atomic32_t		nr_addr_hint[NAT_PROTO_COUNT];
};

/*
 * nat address pool
 */
struct nat_pool {
	struct cds_lfht_node	np_node;
	struct rcu_head		np_rcu_head;
	rte_atomic32_t		np_refcnt;
	uint16_t		np_flags;

	/* Pool identity */
	char			*np_name;
	enum nat_pool_type	np_type;  /* cgnat or ? */

	/* Config for address allocation */
	enum nat_addr_pooling	np_ap;
	enum nat_addr_allcn	np_aa;

	/* Config for port blocks */
	uint16_t		np_block_sz; /* Port block size */
	uint16_t		np_mbpu;     /* max blocks per user */

	/* Config for port allocation */
	uint16_t		np_port_start;
	uint16_t		np_port_end;
	enum nat_port_allcn	np_pa;

	/* Logging control */
	bool			np_log_pba;  /* Log port-block alloc/release */
	bool			np_full;

	/* Number of ports per addr. derived from port start/end */
	uint16_t		np_nports;

	/*
	 * Total # of private addrs in rules or policies attached to this
	 * pool.
	 */
	uint32_t		np_nusers;
	uint64_t		np_nuser_addrs;

	/*
	 * Mapping stats.
	 */
	rte_atomic32_t		np_map_active;	/* Active mappings */
	rte_atomic64_t		np_map_reqs;	/* Mapping requests */
	rte_atomic64_t		np_map_fails;	/* Mapping failures */

	rte_atomic32_t		np_pb_active;	/* Active port blocks */
	rte_atomic64_t		np_pb_allocs;	/* Port blocks allocd */
	rte_atomic64_t		np_pb_fails;	/* Port block alloc fails */
	rte_atomic64_t		np_pb_freed;	/* Port blocks freed */
	rte_atomic64_t		np_pb_limit;	/* mbpu limit reached */

	/* address ranges */
	struct nat_pool_ranges	*np_ranges;

	/*
	 * We rely on the yang data model to keep the addr group in existence
	 * while a nat pool is using it.
	 */
	struct npf_addrgrp	*np_blacklist;
};

/*
 * Get next address in an address pool
 */
uint32_t nat_pool_next_addr(struct nat_pool *np, uint32_t addr);

/*
 * Get the range index that an address is in.  Returns -1 if address is not in
 * any range.
 */
int nat_pool_addr_range(struct nat_pool *np, uint32_t addr);

/* Return true if address-pool paired is enabled */
static inline bool
nat_pool_is_ap_paired(const struct nat_pool *np)
{
	return np->np_ap == NAT_AP_PAIRED;
}

/* Return true if port allocation is sequential  */
static inline bool
nat_pool_is_pa_sequential(const struct nat_pool *np)
{
	return np->np_pa == NAT_PA_SEQUENTIAL;
}

/* Get max-blocks-per-user limit */
static inline uint16_t nat_pool_get_mbpu(const struct nat_pool *np)
{
	return np->np_mbpu;
}

/*
 * Remember the last address allocated from a pool.  We start looking from the
 * address after this one when doing the next allocation.
 */
static inline void
nat_pool_hint_set(struct nat_pool *np, uint32_t addr, uint8_t proto)
{
	rte_atomic32_set(&np->np_ranges->nr_addr_hint[proto], addr);
}

static inline uint32_t
nat_pool_hint(struct nat_pool *np, uint8_t proto)
{
	return rte_atomic32_read(&np->np_ranges->nr_addr_hint[proto]);
}

static inline void
nat_pool_incr_map_reqs(struct nat_pool *np)
{
	rte_atomic64_inc(&np->np_map_reqs);
}

static inline void
nat_pool_incr_map_fails(struct nat_pool *np)
{
	rte_atomic64_inc(&np->np_map_fails);
}

static inline void
nat_pool_incr_map_active(struct nat_pool *np)
{
	rte_atomic32_inc(&np->np_map_active);
}

static inline void
nat_pool_decr_map_active(struct nat_pool *np)
{
	rte_atomic32_dec(&np->np_map_active);
}

static inline void
nat_pool_incr_block_allocs(struct nat_pool *np)
{
	rte_atomic64_inc(&np->np_pb_allocs);
}

static inline void
nat_pool_incr_block_freed(struct nat_pool *np)
{
	rte_atomic64_inc(&np->np_pb_freed);
}

static inline void
nat_pool_incr_block_fails(struct nat_pool *np)
{
	rte_atomic64_inc(&np->np_pb_fails);
}

static inline void
nat_pool_incr_block_active(struct nat_pool *np)
{
	rte_atomic32_inc(&np->np_pb_active);
}

static inline void
nat_pool_decr_block_active(struct nat_pool *np)
{
	rte_atomic32_dec(&np->np_pb_active);
}

static inline void
nat_pool_incr_block_limit(struct nat_pool *np)
{
	rte_atomic64_inc(&np->np_pb_limit);
}

#endif
