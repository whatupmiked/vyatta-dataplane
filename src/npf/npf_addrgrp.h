/*
 * Copyright (c) 2017-2019, AT&T Intellectual Property.  All rights reserved.
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 */

#ifndef NPF_ADDRGRP_H
#define NPF_ADDRGRP_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#include "json_writer.h"
#include "npf/npf.h"
#include "npf_ptree.h"
#include "npf_tblset.h"

struct npf_addrgrp;
struct npf_addrgrp_entry;

/**
 * @file npf_addrgrp.h
 * @brief npf address resource groups
 *
 * An address resource-group is a collection of host addresses, host address
 * ranges, and/or subnet prefixes.  They are referenced from config via a
 * name, and from the forwarding threads via a table ID.  Used by npf rules
 * and NAT.
 */

/*
 * Two lists/trees per address-group
 */
enum npf_addrgrp_af {
	AG_IPv4 = 0,
	AG_IPv6 = 1
};
#define AG_MAX 2


/*************************************************************************
 * Address-group forwarding thread api
 *************************************************************************/

/**
 * @brief Lookup an address in an address-group.
 *
 * Called from forwarding thread, so access to underlying ptree is protected
 * with read-write lock.
 *
 * @param af   Address-group address family.  AG_IPv4 or AG_IPv6.
 * @param ag   Address-group handle
 * @param addr Address to lookup
 * @return 0 if address found else a negative error code if not found:
 *         -EINVAL if address-group not found
 *         -ENOENT if entry in address-group not found
 */
int npf_addrgrp_lookup(enum npf_addrgrp_af af, struct npf_addrgrp *ag,
		       npf_addr_t *addr);

/**
 * @brief Lookup an IPv4 address in an address-group.
 *
 * Called from forwarding thread, so access to underlying ptree is protected
 * with read-write lock.
 *
 * @param ag   Address-group handle
 * @param addr IPv4 address to lookup
 * @return 0 if address found else a negative error code if not found:
 *         -EINVAL if ag is NULL
 *         -ENOENT if entry in address-group not found
 */
int npf_addrgrp_lookup_v4(struct npf_addrgrp *ag, uint32_t addr);

/**
 * @brief Lookup an IPv6 address in an address-group.
 *
 * Called from forwarding thread, so access to underlying ptree is protected
 * with read-write lock.
 *
 * @param ag   Address-group handle
 * @param addr IPv6 address to lookup
 * @return 0 if address found else a negative error code if not found:
 *         -EINVAL if ag is NULL
 *         -ENOENT if entry in address-group not found
 */
int npf_addrgrp_lookup_v6(struct npf_addrgrp *ag, uint8_t *addr);

/**
 * @brief Get name from address group handle
 */
char *npf_addrgrp_handle2name(struct npf_addrgrp *ag);


/*************************************************************************
 * Address-group tableset management api
 *************************************************************************/

/**
 * @brief Get number of address-groups in the address-group tableset
 */
uint npf_addrgrp_ntables(void);

/**
 * @brief Lookup an address group for a given table ID
 */
struct npf_addrgrp *npf_addrgrp_tid_lookup(int tid);

/**
 * @brief Is this a valid address-group table ID?
 *
 * Called by the bytecode verification function, so check the table ID is
 * valid *and* a table exists.
 */
bool npf_addrgrp_tid_valid(uint32_t tid);

/**
 * @brief Address-group table ID to name
 */
const char *npf_addrgrp_tid2name(uint32_t tid);

/**
 * @brief Address-group name to table ID
 */
int npf_addrgrp_name2tid(const char *name, uint32_t *tid);

/**
 * @brief Get an address-groups table ID
 */
int npf_addrgrp_get_tid(struct npf_addrgrp *ag);

/**
 * @brief Lookup an address-group by name
 */
struct npf_addrgrp *npf_addrgrp_lookup_name(const char *name);

/**
 * @brief Create an address-group and insert it into tableset
 */
struct npf_addrgrp *npf_addrgrp_create(const char *name);

/**
 * @brief Remove an address-group from the tableset and destroy it
 */
int npf_addrgrp_destroy(const char *name);

/**
 * @brief Destroy address-group tableset
 *
 * Removes and destroy all address groups in the address-group tableset, then
 * destroys the tableset.
 */
int npf_addrgrp_tbl_destroy(void);


/*************************************************************************
 * Address-group management api
 *************************************************************************/

/**
 * @brief Returns number of entries in an address-group
 *
 * @param name Address group name
 * @return Number of entries in the IPv4 and IPv6 lists
 */
int npf_addrgrp_nentries(const char *name);

/**
 * @brief Insert an address prefix into an address-group
 *
 * A prefix may not overlap with an address range.  A prefix may match an
 * existing prefix if it has a different mask.  Up to 8 masks are allowed for
 * any one prefix entry.
 *
 * @param name Address group name
 * @param addr Prefix to insert, in network byte order
 * @param alen Address length. 4 or 16.
 * @param mask Mask length. 1 to 32 or 128.
 *
 * @return 0 if successful, else < 0.
 */
int npf_addrgrp_prefix_insert(const char *name, npf_addr_t *addr,
			      uint8_t alen, uint8_t mask);

/**
 * @brief Insert an address range into an address-group
 *
 * The address range may not overlap with an existing prefix or address range.
 *
 * @param name Address group name
 * @param start First address in range, in network byte order
 * @param end Last address in range (network byte order)
 * @param alen Address length. 4 or 16.
 *
 * @return 0 if successful, else < 0.
 */
int npf_addrgrp_range_insert(const char *name, npf_addr_t *start,
			     npf_addr_t *end, uint8_t alen);

/**
 * @brief Remove an address prefix and mask from an address-group
 *
 * Removes the mask from the prefix entry.  If this is the last mask, then
 * removes and destroy prefix entry.
 *
 * @param name Address group name
 * @param addr Prefix to remove, in network byte order
 * @param alen Address length. 4 or 16.
 * @param mask Mask length. 1 to 32 or 128.
 *
 * @return 0 if successful, else < 0.
 */
int npf_addrgrp_prefix_remove(const char *name, npf_addr_t *addr,
			      uint8_t alen, uint8_t mask);

/**
 * @brief Remove an address range from an address-group
 *
 * Remove and destroy an address range entry.
 *
 * @param name Address group name
 * @param start First address in range, in network byte order
 * @param end Last address in range (network byte order)
 * @param alen Address length. 4 or 16.
 *
 * @return 0 if successful, else < 0.
 */
int npf_addrgrp_range_remove(const char *name, npf_addr_t *start,
			     npf_addr_t *end, uint8_t alen);


/********************************************************************
 * Address group walks
 *******************************************************************/

/**
 * @brief Walk all entries in an address-group tree
 *
 * Walk terminates if callback returns non-zero.
 *
 * @return <0 if parameter error or address-group not found,
 *         0 if address-group tree has no entries
 *         else return value of callback function
 */
typedef pt_walk_cb npf_ag_tree_walk_cb;

int npf_addrgrp_tree_walk(enum npf_addrgrp_af af, int tid,
			  npf_ag_tree_walk_cb *cb, void *ctx);

/*
 * Walk IPv4 address group list, and callback for each list entry providing:
 * start address, end address and number of useable addresses. Example cb:
 *
 * int npf_addrgrp_ipv4_range_cb(uint32_t start, uint32_t stop,
 *				 uint32_t range, void *ctx)
 *
 * Start and stop address are returned in host byte order.
 */
typedef int (ag_ipv4_range_cb)(uint32_t, uint32_t, uint32_t, void *);

int npf_addrgrp_ipv4_range_walk(int tid, ag_ipv4_range_cb *cb, void *ctx);

/**
 * @brief Get number of useable addresses in an address-group
 */
uint64_t npf_addrgrp_naddrs(enum npf_addrgrp_af af, int tid);


/********************************************************************
 * Address group show
 *******************************************************************/

/*
 * Filter what we return in the json
 */
struct npf_show_ag_ctl {
	json_writer_t *json;
	char *name;      /* show this named address-group or .. */
	int tid;         /* .. show address-group with this ID */
	bool list;       /* show list entries */
	bool range_pfxs; /* show list entry range prefixes */
	bool tree;       /* show tree entries */
	bool af[AG_MAX]; /* show IPv4 and/or IPv6 entries */
};

/**
 * @brief Return the json representation of an address-group
 *
 * Uses the above control structure to control exactly what json is returned.
 *
 * Example json:
 *
 * {
 *   "address-group": {
 *     {
 *       "name":"ADDR_GRP1",
 *       "id":1,
 *       "ipv4":{
 *         "list-entries":[
 *           {
 *             "type":0,
 *             "prefix":"6.0.0.5",
 *             "mask":32
 *           },
 *           {
 *             "type":1,
 *             "start":"7.1.1.3",
 *             "end":"7.1.1.6",
 *             "range-prefixes":[
 *               {
 *                 "type":0,
 *                 "prefix":"7.1.1.3",
 *                 "mask":32
 *               },
 *               {
 *                 "type":0,
 *                 "prefix":"7.1.1.4",
 *                 "mask":31
 *               },
 *               {
 *                 "type":0,
 *                 "prefix":"7.1.1.6",
 *                 "mask":32
 *               }
 *             ]
 *           }
 *         ],
 *         "tree":[
 *           {
 *             "type":0,
 *             "prefix":"6.0.0.5",
 *             "mask":32
 *           },
 *           {
 *             "type":0,
 *             "prefix":"7.1.1.3",
 *             "mask":32
 *           },
 *           {
 *             "type":0,
 *             "prefix":"7.1.1.4",
 *             "mask":31
 *           },
 *           {
 *             "type":0,
 *             "prefix":"7.1.1.6",
 *             "mask":32
 *           }
 *         ]
 *       },
 *       "ipv6":{
 *         "list-entries":[
 *           {
 *             "type":0,
 *             "prefix":"2001:1:1::",
 *             "mask":64
 *           }
 *         ],
 *         "tree":[
 *           {
 *             "type":0,
 *             "prefix":"2001:1:1::",
 *             "mask":64
 *           }
 *         ]
 *       }
 *     }
 *   }
 * }
 *
 */
void npf_addrgrp_show_json(FILE *fp, struct npf_show_ag_ctl *ctl);

/**
 * @brief Return json for optimal set of address subblocks
 *
 * Determine the optimal set of CIDR address subblocks that may be used to
 * represent the current prefix and range entries for an address-group,
 * and returns the result to the user.
 *
 * Returns json in the following format:
 *
 * {
 *   "address-group": {
 *     {
 *       "name":"ADDR_GRP1",
 *       "id":1,
 *       "ipv4":{
 *       "tree":[
 *          {
 *             "type":0,
 *             "prefix":"4.0.0.0",
 *             "mask":20
 *           },
 *       ]
 *     }
 *   }
 * }
 */
void npf_addrgrp_show_json_opt(FILE *fp, struct npf_show_ag_ctl *ctl);

#endif
