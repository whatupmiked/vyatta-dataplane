/*
 * Copyright (c) 2017-2019, AT&T Intellectual Property.  All rights reserved.
 * Copyright (c) 2016 by Brocade Communications Systems, Inc.
 * All rights reserved.
 */

/*	$NetBSD: npf_state.c,v 1.12 2012/08/15 19:47:38 rmind Exp $	*/

/*-
 * Copyright (c) 2010-2012 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * SPDX-License-Identifier: (LGPL-2.1-only AND BSD-2-Clause-NETBSD)
 *
 * This material is based upon work partially supported by The
 * NetBSD Foundation under a contract with Mindaugas Rasiukevicius.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * NPF state engine to track sessions.
 */

#include <assert.h>
#include <ctype.h>
#include <rte_branch_prediction.h>
#include <rte_spinlock.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "json_writer.h"
#include "vrf.h"
#include "npf/npf.h"
#include "npf/config/npf_config.h"
#include "npf/config/npf_ruleset_type.h"
#include "npf/npf_cache.h"
#include "npf/npf_ruleset.h"
#include "npf/npf_state.h"
#include "npf/npf_timeouts.h"
#include "npf/npf_vrf.h"
#include "npf/rproc/npf_rproc.h"
#include "npf_shim.h"

struct rte_mbuf;

/*
 * Generic state name for UDP and ICMP, and other non-TCP protocols.
 *
 * Logger uses the upper-case form shown here.
 * npf commands use the lower-case form.
 * json uses use the lower-case form, plus hyphens replaced with underscores.
 */
static const char *npf_state_generic_name[NPF_ANY_SESSION_NSTATES] = {
	[NPF_ANY_SESSION_NONE]		= "NONE",
	[NPF_ANY_SESSION_NEW]		= "NEW",
	[NPF_ANY_SESSION_ESTABLISHED]	= "ESTABLISHED",
	[NPF_ANY_SESSION_TERMINATING]	= "TERMINATING",
	[NPF_ANY_SESSION_CLOSED]	= "CLOSED",
};

/*
 * TCP state name.
 *
 * Logger uses the upper-case form shown here.
 * npf commands use the lower-case form.
 * json uses use the lower-case form, plus hyphens replaced with underscores.
 */
static const char *npf_state_tcp_name[NPF_TCP_NSTATES] = {
	[NPF_TCPS_NONE]		= "NONE",
	[NPF_TCPS_SYN_SENT]	= "SYN-SENT",
	[NPF_TCPS_SIMSYN_SENT]	= "SIMSYN-SENT",
	[NPF_TCPS_SYN_RECEIVED]	= "SYN-RECEIVED",
	[NPF_TCPS_ESTABLISHED]	= "ESTABLISHED",
	[NPF_TCPS_FIN_SENT]	= "FIN-SENT",
	[NPF_TCPS_FIN_RECEIVED]	= "FIN-RECEIVED",
	[NPF_TCPS_CLOSE_WAIT]	= "CLOSE-WAIT",
	[NPF_TCPS_FIN_WAIT]	= "FIN-WAIT",
	[NPF_TCPS_CLOSING]	= "CLOSING",
	[NPF_TCPS_LAST_ACK]	= "LAST-ACK",
	[NPF_TCPS_TIME_WAIT]	= "TIME-WAIT",
	[NPF_TCPS_RST_RECEIVED]	= "RST-RECEIVED",
	[NPF_TCPS_CLOSED]	= "CLOSED",
};

static const uint8_t npf_generic_fsm[NPF_ANY_SESSION_NSTATES][2] = {
	[NPF_ANY_SESSION_NONE] = {
		[NPF_FLOW_FORW]		= NPF_ANY_SESSION_NEW,
	},
	[NPF_ANY_SESSION_NEW] = {
		[NPF_FLOW_FORW]		= NPF_ANY_SESSION_NEW,
		[NPF_FLOW_BACK]		= NPF_ANY_SESSION_ESTABLISHED,
	},
	[NPF_ANY_SESSION_ESTABLISHED] = {
		[NPF_FLOW_FORW]		= NPF_ANY_SESSION_ESTABLISHED,
		[NPF_FLOW_BACK]		= NPF_ANY_SESSION_ESTABLISHED,
	},
};

static struct npf_state_stats *stats;
static bool npf_state_icmp_strict;

#define stats_inc_tcp(a)	(stats[dp_lcore_id()].ss_tcp_ct[(a)]++)
#define stats_inc(a, b)		(stats[dp_lcore_id()].ss_ct[(a)][(b)]++)
#define stats_dec(a, b)		(stats[dp_lcore_id()].ss_ct[(a)][(b)]--)
#define stats_dec_tcp(a)	(stats[dp_lcore_id()].ss_tcp_ct[(a)]--)

/* state stats - create/destroy */
void npf_state_stats_create(void)
{
	stats = zmalloc_aligned((get_lcore_max() + 1) *
		sizeof(struct npf_state_stats));
}

void npf_state_stats_destroy(void)
{
	free(stats);
}

/* Control strict icmp echo direction checks */
void npf_state_set_icmp_strict(bool value)
{
	npf_state_icmp_strict = value;
}

/*
 * npf_sess_state_init: initialise the state structure.
 *
 * Should normally be called on a first packet, which also determines the
 * direction in a case of connection-orientated protocol.  Returns true on
 * success and false otherwise (e.g. if protocol is not supported).
 */
void npf_state_init(vrfid_t vrfid, uint8_t proto_idx, npf_state_t *nst)
{
	assert(NPF_ANY_SESSION_LAST < 255);
	assert(NPF_TCPS_LAST < 255);
	assert(NPF_TCPS_OK <= 255);
	assert(NPF_TCPS_OK > NPF_TCPS_LAST);

	rte_spinlock_init(&nst->nst_lock);

	/* Take reference on vrf npf timeout struct */
	nst->nst_to = npf_timeout_ref_get(vrf_get_npf_timeout_rcu(vrfid));

	if (proto_idx == NPF_PROTO_IDX_TCP) {
		nst->nst_state = NPF_TCPS_NONE;
		stats_inc_tcp(NPF_TCPS_NONE);
	} else {
		nst->nst_state = NPF_ANY_SESSION_NONE;
		stats_inc(proto_idx, NPF_ANY_SESSION_NONE);
	}
}

/* Called from npf_session_destroy */
void npf_state_destroy(npf_state_t *nst, uint8_t proto_idx)
{
	if (proto_idx == NPF_PROTO_IDX_TCP)
		stats_dec_tcp(nst->nst_state);
	else
		stats_dec(proto_idx, nst->nst_state);

	/* Release reference on vrf npf timeout struct */
	npf_timeout_ref_put(nst->nst_to);
}

/*
* Set generic session state.
*/
static inline void
npf_state_generic_state_set(npf_state_t *nst, uint8_t proto_idx,
		uint8_t state, bool *state_changed)
{
	if (unlikely(nst->nst_state != state)) {
		uint8_t old_state = nst->nst_state;

		stats_dec(proto_idx, old_state);
		stats_inc(proto_idx, state);

		nst->nst_state = state;
		*state_changed = true;
	}
}

/*
* Set TCP session state.
*/
static inline void
npf_state_tcp_state_set(npf_state_t *nst, uint8_t state, bool *state_changed)
{
	if (unlikely(state != NPF_TCPS_OK && nst->nst_state != state)) {
		uint8_t old_state = nst->nst_state;

		stats_dec_tcp(old_state);
		stats_inc_tcp(state);

		nst->nst_state = state;
		*state_changed = true;
	}
}

/*
 * npf_state_inspect: inspect the packet according to the protocol state.
 *
 * Return true if packet is considered to match the state (e.g. for TCP,
 * the packet belongs to the tracked connection) and false otherwise.
 */
bool npf_state_inspect(const npf_cache_t *npc, struct rte_mbuf *nbuf,
		       npf_state_t *nst, bool forw)
{
	const uint8_t proto_idx = npf_cache_proto_idx(npc);
	const int di = forw ? NPF_FLOW_FORW : NPF_FLOW_BACK;
	bool ret = true;
	bool state_changed = false;
	uint8_t state;
	uint8_t old_state;

	rte_spinlock_lock(&nst->nst_lock);

	old_state = nst->nst_state;

	switch (proto_idx) {
	case NPF_PROTO_IDX_TCP:
		state = npf_state_tcp(npc, nbuf, nst, di);
		if (unlikely(state == NPF_TCPS_ERR)) {
			ret = false;
			break;
		}
		npf_state_tcp_state_set(nst, state, &state_changed);
		break;
	case NPF_PROTO_IDX_ICMP:
		state = nst->nst_state;
		if ((npf_state_icmp_strict || state == NPF_ANY_SESSION_NONE) &&
		    unlikely(forw ^ npf_iscached(npc, NPC_ICMP_ECHO_REQ))) {
			ret = false;
			break;
		}
		/* fall through */
	default:
		state = npf_generic_fsm[nst->nst_state][di];

		npf_state_generic_state_set(nst, proto_idx, state,
				&state_changed);
		break;
	}
	rte_spinlock_unlock(&nst->nst_lock);

	if (state_changed)
		npf_session_state_change(nst, old_state, state, proto_idx);
	return ret;
}

/*
 * Mark session state as 'closed' for the period that it is going through
 * garbage collection.
 */
void npf_state_set_closed_state(npf_state_t *nst, bool lock, uint8_t proto_idx)
{
	uint8_t old_state;
	uint8_t state;
	bool state_changed = false;

	if (lock)
		rte_spinlock_lock(&nst->nst_lock);

	old_state = nst->nst_state;

	if (proto_idx == NPF_PROTO_IDX_TCP) {
		state = NPF_TCPS_CLOSED;
		npf_state_tcp_state_set(nst, NPF_TCPS_CLOSED,
				&state_changed);
	} else {
		state = NPF_ANY_SESSION_CLOSED;
		npf_state_generic_state_set(nst, proto_idx,
				NPF_ANY_SESSION_CLOSED, &state_changed);
	}

	if (lock)
		rte_spinlock_unlock(&nst->nst_lock);

	if (state_changed)
		npf_session_state_change(nst, old_state, state, proto_idx);
}

/*
 * Update the dataplane session (if present) state/timeout with the
 * current NPF protocol state.
 *
 * This is called during NPF activation and protocol state changes.
 */
void npf_state_update_session_state(struct session *s, uint8_t proto_idx,
		const npf_state_t *nst)
{
	uint32_t to;

	if (s) {
		to = npf_timeout_get(nst, proto_idx, s->se_custom_timeout);
		session_set_protocol_state_timeout(s, nst->nst_state, to);
	}
}

static const char *npf_state_get_state_generic_name(uint8_t index)
{
	if (!npf_state_generic_state_is_valid(index))
		return NULL;
	return npf_state_generic_name[index];
}

static const char *npf_state_get_state_tcp_name(uint8_t index)
{
	if (!npf_state_tcp_state_is_valid(index))
		return NULL;
	return npf_state_tcp_name[index];
}

/*
 * npf_state_get_state_name: return state name for logging purpose
 */
const char *npf_state_get_state_name(uint8_t state, uint8_t proto_idx)
{
	if (proto_idx == NPF_PROTO_IDX_TCP)
		return npf_state_get_state_tcp_name(state);
	else
		return npf_state_get_state_generic_name(state);
}

/*
 * Json strings are lower case, with underscores in place of hyphens.
 */
static void npf_str_to_json_name(const char *src, char *dst, int len)
{
	int i;

	for (i = 0; i < len-1 && src[i] != '\0'; i++) {
		if (src[i] == '-')
			dst[i] = '_';
		else
			dst[i] = tolower(src[i]);
	}
	dst[i] = '\0';
}

/*
 * Log strings are upper case, with hyphens in place of underscores
 */
static void npf_str_to_log_name(const char *src, char *dst, int len)
{
	int i;

	for (i = 0; i < len-1 && src[i] != '\0'; i++)
		if (src[i] == '_')
			dst[i] = '-';
		else
			dst[i] = toupper(src[i]);
	dst[i] = '\0';
}

static void
npf_state_get_state_name_json(uint8_t state, uint8_t proto_idx,
			      char *dst, int len)
{
	const char *upper;

	if (proto_idx == NPF_PROTO_IDX_TCP)
		upper = npf_state_get_state_tcp_name(state);
	else
		upper = npf_state_get_state_generic_name(state);

	npf_str_to_json_name(upper, dst, len);
}

bool npf_state_is_steady(const npf_state_t *nst, const uint8_t proto_idx)
{
	if (proto_idx == NPF_PROTO_IDX_TCP)
		return (nst->nst_state == NPF_TCPS_ESTABLISHED ? true : false);
	else
		return (nst->nst_state == NPF_ANY_SESSION_ESTABLISHED) ?
			true : false;
}

/*
 * Returns true if protocol is TCP and state is CLOSED
 */
bool npf_tcp_state_is_closed(const npf_state_t *nst, const uint8_t proto_idx)
{
	if (proto_idx == NPF_PROTO_IDX_TCP)
		return nst->nst_state == NPF_TCPS_CLOSED;
	return false;
}

/* convert CLI generic state to numerical value */
uint8_t npf_map_str_to_generic_state(const char *name)
{
	uint8_t state;
	char upper[40];

	npf_str_to_log_name(name, upper, sizeof(upper));

	for (state = NPF_ANY_SESSION_FIRST;
	     state <= NPF_ANY_SESSION_LAST;  state++)
		if (strcmp(upper, npf_state_generic_name[state]) == 0)
			return state;

	return NPF_ANY_SESSION_NSTATES;
}

/* convert CLI TCP state to numerical value */
uint8_t npf_map_str_to_tcp_state(const char *name)
{
	uint8_t state;
	char upper[40];

	npf_str_to_log_name(name, upper, sizeof(upper));

	for (state = NPF_TCPS_FIRST;
	     state <= NPF_TCPS_LAST; state++)
		if (strcmp(upper, npf_state_tcp_name[state]) == 0)
			return state;

	return NPF_TCP_NSTATES;
}

/*
 * Test the packet to see if it matches a custom session timeout.
 */
uint32_t npf_state_get_custom_timeout(vrfid_t vrfid, npf_cache_t *npc,
				      struct rte_mbuf *nbuf)
{
	/* Test the packet */
	struct npf_config *npf_config = vrf_get_npf_conf_rcu(vrfid);
	const npf_ruleset_t *npf_rs =
			npf_get_ruleset(npf_config, NPF_RS_CUSTOM_TIMEOUT);
	npf_rule_t *rl =
		npf_ruleset_inspect(npc, nbuf, npf_rs, NULL, NULL, PFIL_IN);

	/* The custom timeout handle is stored as a tag */
	bool tag_present = false;
	uint32_t tag_val = (rl) ? npf_rule_rproc_tag(rl, &tag_present) : 0;

	return (tag_present) ? tag_val : 0;
}

void npf_state_stats_json(json_writer_t *json)
{
	uint8_t state, proto;
	uint32_t tmp;
	uint32_t i;
	char name[40];

	/* Temporary fixup until vplane-config-npf is updated */
	FOREACH_DP_LCORE(i) {
		stats[i].ss_tcp_ct[NPF_TCPS_CLOSED] +=
			stats[i].ss_tcp_ct[NPF_TCPS_NONE];
	}

	FOREACH_DP_LCORE(i) {
		for (proto = NPF_PROTO_IDX_FIRST;
				proto <= NPF_PROTO_IDX_LAST; proto++)
			stats[i].ss_ct[proto][NPF_ANY_SESSION_CLOSED] +=
				stats[i].ss_ct[proto][NPF_ANY_SESSION_NONE];
	}

	jsonw_name(json, "tcp");
	jsonw_start_object(json);

	for (state = NPF_TCPS_FIRST; state <= NPF_TCPS_LAST; state++) {
		npf_state_get_state_name_json(state, NPF_PROTO_IDX_TCP, name,
					      sizeof(name));
		tmp = 0;
		FOREACH_DP_LCORE(i)
			tmp += stats[i].ss_tcp_ct[state];
		jsonw_uint_field(json, name, tmp);
	}

	jsonw_end_object(json);

	/*
	 * udp, icmp and other
	 */
	for (proto = NPF_PROTO_IDX_FIRST;
	     proto <= NPF_PROTO_IDX_LAST; proto++) {
		if (proto == NPF_PROTO_IDX_TCP)
			continue;

		jsonw_name(json, npf_get_protocol_name_from_idx(proto));
		jsonw_start_object(json);

		for (state = NPF_ANY_SESSION_FIRST;
		     state <= NPF_ANY_SESSION_LAST;  state++) {
			npf_state_get_state_name_json(state, proto, name,
						      sizeof(name)),
			tmp = 0;
			FOREACH_DP_LCORE(i)
				tmp +=  stats[i].ss_ct[proto][state];
			jsonw_uint_field(json, name, tmp);
		}

		jsonw_end_object(json);
	}
}

#ifdef _NPF_TESTING
void
npf_state_dump(const npf_state_t *nst __unused)
{
	const npf_tcpstate_t *fst = &nst->nst_tcpst[0];
	const npf_tcpstate_t *tst = &nst->nst_tcpst[1];

	printf("\tstate (%p) %d:\n\t\t"
	    "F { end %u maxend %u mwin %u wscale %u }\n\t\t"
	    "T { end %u maxend %u mwin %u wscale %u }\n",
	    nst, nst->nst_state,
	    fst->nst_end, fst->nst_maxend, fst->nst_maxwin, fst->nst_wscale,
	    tst->nst_end, tst->nst_maxend, tst->nst_maxwin, tst->nst_wscale
	);
}
#endif
