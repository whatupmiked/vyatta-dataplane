/*
 * Copyright (c) 2017-2019, AT&T Intellectual Property.  All rights reserved.
 * Copyright (c) 2013-2016 by Brocade Communications Systems, Inc.
 * All rights reserved.
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 */

/*
 * NPF ALG for TFTP
 *
 * A TFTP ALG based on RFCs 1350 and 2347-2349.
 */

#include <errno.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <rte_log.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "compiler.h"
#include "npf/npf.h"
#include "npf/alg/npf_alg_private.h"
#include "npf/npf_cache.h"
#include "npf/npf_nat.h"
#include "npf/npf_session.h"
#include "util.h"
#include "vplane_log.h"

struct ifnet;
struct rte_mbuf;

/* For verifying tftp packets.. */
#define TFTP_OPCODE_SIZE 2

/* Default port */
#define TFTP_DEFAULT_PORT	69

/* ALG's specific flags*/
#define TFTP_ALG_CNTL	0x040000  /* tftp control flow. means WRQ or RRQ */
#define TFTP_ALG_SNAT	0x000010
#define TFTP_ALG_DNAT	0x000020

/* tftp_alg_config() - Config routine for tftp */
static int tftp_alg_config(struct npf_alg *tftp, int op, int argc,
			char * const argv[])
{
	int rc = 0;
	int i;
	struct npf_alg_config_item ci = {
		.ci_proto = IPPROTO_UDP,
		.ci_flags = (NPF_TUPLE_KEEP | NPF_TUPLE_MATCH_PROTO_PORT)
	};

	if (strcmp(argv[0], "port"))
		return -EINVAL;
	argc--; argv++;

	for (i = 0; i < argc; i++) {
		ci.ci_datum = npf_port_from_str(argv[i]);
		if (!ci.ci_datum)
			continue;
		rc = npf_alg_manage_config_item(tftp, &tftp->na_configs[0],
				op, &ci);
		if (rc)
			return rc;
	}
	return rc;
}

/* Create and insert a tuple for an expected flow */
static int tftp_alg_tuple_insert(const struct npf_alg *tftp,
				npf_cache_t *npc, npf_session_t *se,
				const npf_addr_t *saddr, in_port_t sport,
				const npf_addr_t *daddr, in_port_t dport,
				uint32_t alg_flags)
{
	struct npf_alg_tuple *nt;
	int rc  = -ENOMEM;

	nt = npf_alg_tuple_alloc();
	if (nt) {
		nt->nt_se = se;
		nt->nt_flags = NPF_TUPLE_MATCH_ANY_SPORT;
		nt->nt_proto = IPPROTO_UDP;
		nt->nt_ifx = npf_session_get_if_index(se);
		nt->nt_alg = tftp;
		nt->nt_alg_flags = alg_flags;
		nt->nt_alen = npc->npc_alen;
		nt->nt_dport = dport;
		nt->nt_sport = sport;
		nt->nt_timeout = 10;
		memcpy(&nt->nt_srcip, saddr,  nt->nt_alen);
		memcpy(&nt->nt_dstip, daddr, nt->nt_alen);
		rc = npf_alg_tuple_add_replace(tftp->na_ai, nt);
		if (rc) {
			npf_alg_tuple_free(nt);
			RTE_LOG(ERR, FIREWALL, "TFTP: tuple insert:%d\n", rc);
		}
	}
	return rc;
}

/*
 * tftp_parse_and_decide() - Parse a tftp opcode and return a decision to
 * insert a tuple.
 */
static int tftp_parse_and_decide(npf_cache_t *npc, struct rte_mbuf *nbuf,
		bool *do_insert)
{
	char buf[TFTP_OPCODE_SIZE];
	uint16_t len;

	/*
	 * All tftp packets must have an opcode as the first
	 * two bytes of the packet.  So verify
	 */
	len = npf_payload_fetch(npc, nbuf, buf,
			TFTP_OPCODE_SIZE, TFTP_OPCODE_SIZE);
	if (!len)
		return -EINVAL;  /* always UDP */

	/* op codes are ascii and not strings */
	if (*buf)
		return -EINVAL;

	/* Only insert a tuple for read/write reqs. */
	switch (buf[1]) {
	case 1:
	case 2:
		*do_insert = true;
		return 0;
	case 3:
	case 4:
	case 5:
	case 6:
		return 0;
	default:
		return -EINVAL;
	}

	return 0;
}

/* tftp_alg_natout() - Packet NAT out*/
static int tftp_alg_nat_out(npf_session_t *se, npf_cache_t *npc,
			struct rte_mbuf *nbuf __unused, npf_nat_t *nat)
{
	npf_addr_t taddr;
	const struct npf_alg *tftp = npf_alg_session_get_alg(se);
	in_port_t tport;
	bool insert = false;
	int rc;

	rc = tftp_parse_and_decide(npc, nbuf, &insert);
	if (insert) {
		npf_nat_get_trans(nat, &taddr, &tport);
		rc = tftp_alg_tuple_insert(tftp, npc, se, npf_cache_dstip(npc),
				0, &taddr, tport, TFTP_ALG_SNAT);
		/* Turn off inspection, we are natting */
		npf_alg_session_set_inspect(se, false);
	}
	return rc;
}

/* tftp_alg_nat_in() - Packet NAT in */
static int tftp_alg_nat_in(npf_session_t *se, npf_cache_t *npc,
			struct rte_mbuf *nbuf __unused, npf_nat_t *nat)
{
	npf_addr_t addr;
	const struct npf_alg *tftp = npf_alg_session_get_alg(se);
	in_port_t port;
	struct udphdr *uh = &npc->npc_l4.udp;
	bool insert = false;
	int rc;

	rc = tftp_parse_and_decide(npc, nbuf, &insert);
	if (insert) {
		npf_nat_get_trans(nat, &addr, &port);
		rc = tftp_alg_tuple_insert(tftp, npc, se, &addr, 0,
			npf_cache_srcip(npc), uh->source, TFTP_ALG_DNAT);
		/* Turn off inspection, we are natting */
		npf_alg_session_set_inspect(se, false);
	}
	return rc;
}

/*
 * Create a reverse nat for tftp. Can only be done on
 * first data packet - we need the server src port
 */
static int tftp_create_nat(npf_session_t *se, npf_nat_t *pnat, npf_cache_t *npc,
		const int di, struct npf_alg_tuple *nt)
{
	struct npf_ports *p;
	npf_addr_t taddr;
	npf_addr_t oaddr;
	in_port_t oport;
	in_port_t tport;
	struct npf_alg_nat *an;

	/* Ignore stateful sessions */
	if (!(nt->nt_alg_flags & (TFTP_ALG_SNAT | TFTP_ALG_DNAT)))
		return 0;

	an = zmalloc_aligned(sizeof(struct npf_alg_nat));
	if (!an)
		return -ENOMEM;

	p = &npc->npc_l4.ports;

	npf_nat_get_trans(pnat, &taddr, &tport);
	npf_nat_get_orig(pnat, &oaddr, &oport);

	an->an_flags = NPF_NAT_REVERSE;
	an->an_taddr = taddr;
	an->an_oaddr = oaddr;
	an->an_vrfid = npf_session_get_vrfid(se);

	if (nt->nt_alg_flags & TFTP_ALG_DNAT) {
		/* Only translate the address, port comes from server */
		an->an_tport = an->an_oport = p->s_port;
	} else if (nt->nt_alg_flags & TFTP_ALG_SNAT) {
		/* Translate both addr and port */
		an->an_tport = tport;
		an->an_oport = oport;
	}

	nt->nt_nat = an;
	return npf_alg_session_nat(se, pnat, npc, di, nt);
}

/* Nat inspect */
static void tftp_alg_nat_inspect(npf_session_t *se, npf_cache_t *npc __unused,
				npf_nat_t *nt, int di __unused)
{
	/* Only for the control flow */
	if (npf_alg_session_test_flag(se, TFTP_ALG_CNTL))
		npf_nat_setalg(nt, npf_alg_session_get_alg(se));
}

/* ALG inspect routine */
static void tftp_alg_inspect(npf_session_t *se, npf_cache_t *npc,
		struct rte_mbuf *nbuf, struct ifnet *ifp __unused,
		int di __unused)
{
	const struct npf_alg *tftp = npf_alg_session_get_alg(se);
	struct udphdr *uh = &npc->npc_l4.udp;
	bool insert = false;

	if (npf_iscached(npc, NPC_NATTED))
		return;

	tftp_parse_and_decide(npc, nbuf, &insert);
	if (insert) {
		tftp_alg_tuple_insert(tftp, npc, se, npf_cache_dstip(npc), 0,
			npf_cache_srcip(npc), uh->source, 0);
		/*
		 * We cannot turn off inspection here since it is
		 * possible this session handle could be re-used
		 */
	}
}

/*
 * Session init
 */
static int tftp_alg_session_init(npf_session_t *se, npf_cache_t *npc,
		struct npf_alg_tuple *nt, const int di)
{
	int rc = 0;

	npf_alg_session_set_inspect(se, true);

	switch (nt->nt_flags & NPF_TUPLE_MATCH_MASK) {
	case NPF_TUPLE_MATCH_PROTO_PORT:  /* parent flow */
		npf_alg_session_set_flag(se, TFTP_ALG_CNTL);
		break;
	case NPF_TUPLE_MATCH_ANY_SPORT:       /* child flow */
		rc = tftp_create_nat(se, npf_alg_parent_nat(nt->nt_se),
				npc, di, nt);
		if (!rc)
			npf_session_link_child(nt->nt_se, se);
		break;
	}
	return rc;
}

/* alg struct */
static const struct npf_alg_ops tftp_ops = {
	.name		= NPF_ALG_TFTP_NAME,
	.se_init	= tftp_alg_session_init,
	.config		= tftp_alg_config,
	.inspect	= tftp_alg_inspect,
	.nat_inspect	= tftp_alg_nat_inspect,
	.nat_in		= tftp_alg_nat_in,
	.nat_out	= tftp_alg_nat_out,
};

/* Default port config */
static const struct npf_alg_config_item tftp_ports[] = {
	{ IPPROTO_UDP, (NPF_TUPLE_KEEP | NPF_TUPLE_MATCH_PROTO_PORT),
		0, TFTP_DEFAULT_PORT }
};

/* Create instance */
struct npf_alg *npf_alg_tftp_create_instance(struct npf_alg_instance *ai)
{
	struct npf_alg *tftp;
	int rc = -ENOMEM;

	tftp = npf_alg_create_alg(ai, NPF_ALG_ID_TFTP);
	if (!tftp)
		goto bad;

	tftp->na_ops = &tftp_ops;

	tftp->na_num_configs = 1;
	tftp->na_configs[0].ac_items = tftp_ports;
	tftp->na_configs[0].ac_item_cnt = ARRAY_SIZE(tftp_ports);
	tftp->na_configs[0].ac_handler = npf_alg_port_handler;

	rc = npf_alg_register(tftp);
	if (rc)
		goto bad;

	/* Take reference on an alg application instance */
	npf_alg_get(tftp);

	return tftp;

bad:
	if (net_ratelimit())
		RTE_LOG(ERR, FIREWALL, "ALG: TFTP instance failed: %d\n", rc);
	free(tftp);
	return NULL;
}

void npf_alg_tftp_destroy_instance(struct npf_alg *tftp)
{
	if (tftp) {
		tftp->na_enabled = false;
		tftp->na_ai = NULL;
		/* Release reference on an alg application instance */
		npf_alg_put(tftp);
	}

}
