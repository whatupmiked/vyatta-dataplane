/*-
 * Copyright (c) 2017-2019, AT&T Intellectual Property.
 * All rights reserved.
 * Copyright (c) 1982, 1986, 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * SPDX-License-Identifier: (LGPL-2.1-only AND BSD-3-Clause)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	From: @(#)if.h	8.1 (Berkeley) 6/10/93
 * $FreeBSD$
 */

#ifndef	IF_VAR_H
#define	IF_VAR_H

#include <netinet/in.h> /* conflict with linux/if_ether.h below */
#include <arpa/inet.h>
#include <sys/queue.h>		/* get TAILQ macros */

#include "bitmask.h"
#include "config.h"
#include "main.h"
#include "util.h"
#include "vrf.h"
#include "vxlan.h"
#include <linux/if_ether.h>
#include <linux/if.h>

#include <rte_ethdev.h>
#include <rte_timer.h>

#include "urcu.h"
#include "arp_cfg.h"
#include <fal_plugin.h>
#include "storm_ctl.h"
#include "if_feat.h"

#define DATAPLANE_MAX_PORTS     RTE_MAX_ETHPORTS
#define IF_PORT_ID_INVALID UCHAR_MAX

/* Hardware port device configuration */
extern struct ifnet *ifport_table[DATAPLANE_MAX_PORTS];

/*
 * List of addresses on interface.
 * Note: the list is NOT used for packet address lookup
 *	 instead a more optimized list is used.
 */
struct if_addr {
	struct cds_list_head	 ifa_link;
	struct sockaddr_storage	 ifa_addr;	/* address of interface */
	struct sockaddr_storage	 ifa_broadcast;	/* broadcast address */
	uint8_t			 ifa_prefixlen;
	struct rcu_head		 ifa_rcu;
};

/* Forward structure definitions */
struct bridge_softc;
struct bridge_port;
struct sched_info;
struct flow_counters;
struct portmonitor_info;
struct npf_if;

/*
 * Software statistics maintained per-core,
 * therefore structure should be sizeof cache line (64 bytes)
 */
struct if_data {
	uint64_t ifi_ipackets;		/* packets received on interface */
	uint64_t ifi_ierrors;		/* input errors on interface */
	uint64_t ifi_opackets;		/* packets sent on interface */
	uint64_t ifi_oerrors;		/* output errors on interface */
	uint64_t ifi_ibytes;		/* total number of octets received */
	uint64_t ifi_obytes;		/* total number of octets sent */
	uint64_t ifi_idropped;		/* dropped by protocol */
	uint64_t ifi_odropped_txring;	/* packet ring overflow */
	uint64_t ifi_odropped_hwq;	/* h/w transmit queue full */
	uint64_t ifi_odropped_proto;	/* dropped in protocol processing */

	uint64_t ifi_ibridged;		/* packets bridged */
	uint64_t ifi_imulticast;	/* multicast packets received */
	uint64_t ifi_ivlan;		/* vlan tag packets */
	uint64_t ifi_no_address;	/* packets dropped no address */
	uint64_t ifi_no_vlan;		/* packets dropped no device for tag */
	uint64_t ifi_unknown;		/* packets non-dataplane protocol */
} __rte_cache_aligned;

static inline uint64_t ifi_odropped(const struct if_data *data)
{
	return data->ifi_odropped_txring +
	       data->ifi_odropped_hwq +
	       data->ifi_odropped_proto;
}

struct if_mpls_data {
	uint64_t ifm_in_octets;
	uint64_t ifm_in_ucastpkts;
	uint64_t ifm_out_octets;
	uint64_t ifm_out_ucastpkts;
	uint64_t ifm_in_errors;
	uint64_t ifm_out_errors;
	uint64_t ifm_lbl_lookup_failures;
	uint64_t ifm_out_fragment_pkts;
} __rte_cache_aligned;

/*
 * Interface performance counters
 * updated as part of the link watch timer
 */
#define SAMPLE_INTERVAL		5

struct if_perf {
	uint64_t cur;	/* measured per/sec value */
	uint64_t last;	/* last value of counter */
	uint64_t avg[3];/* 1min, 5min, 15min (scaled) */
};

/*
 * Interface types
 */
enum if_type {
	IFT_ETHER,
	IFT_L2TPETH,
	IFT_PPP,
	IFT_LOOP,
	IFT_TUNNEL_OTHER,
	IFT_TUNNEL_GRE,
	IFT_TUNNEL_VTI,
	IFT_L2VLAN,
	IFT_BRIDGE,

	IFT_VXLAN,
	IFT_MACVLAN,
	IFT_VRFMASTER,
};

/*
 * IFT_LOOP (ie. "dummy" interface) can be used as a virtual
 * feature point to attach features.
 */
enum vfp_type {
	VFP_NONE,       /* Regular loopback */
	VFP_S2S_CRYPTO, /* IPSec s2s policy feature attachment */
};

struct vfp_softc {
	enum vfp_type vfp_type;
	int refcount;
};

enum if_role {
	IF_ROLE_NONE = 0,
	IF_ROLE_PORT,  /* basic ethernet port */
	IF_ROLE_UPLINK,/* uplink interface */
	IF_ROLE_MAX
};

/*
 * TCP MSS Clamping
 */
enum tcp_mss_af {
	TCP_MSS_V4 = 0,
	TCP_MSS_V6
};
#define TCP_MSS_AF_MAX  TCP_MSS_V6
#define TCP_MSS_AF_SIZE (TCP_MSS_AF_MAX + 1)

static const uint32_t IF_LINK_SPEED_UNKNOWN = 0;

enum if_link_duplex_type {
	IF_LINK_DUPLEX_HALF = 0,
	IF_LINK_DUPLEX_FULL = 1,
	IF_LINK_DUPLEX_UNKNOWN = 2,
};

struct if_link_status {
	bool link_status;
	enum if_link_duplex_type link_duplex;
	/* Link speed in Mbps */
	uint32_t link_speed;
};

struct if_vlan_feat {
	uint16_t             vlan;
	fal_object_t         fal_vlan_feat;
	uint32_t             refcount;
	struct cds_lfht_node vlan_feat_node;
	struct rcu_head	     rcu;
};

/*
 * Structure defining a network interface.
 */
struct ifnet {
	/* Referenced on packet processing path */
	unsigned int	   if_index;	/* linux kernel global identifier */
	unsigned int	   if_flags;	/* global flags up/down, broadcast, etc. */
	uint16_t	   if_mtu;	/* Maximum Transmission Unit */
	uint16_t	   if_vlan;	/* VLAN tag */
	uint16_t           tpid;	/* VLAN protocol id */

	uint8_t		   if_port;	/* dpdk port # */
	uint8_t		   if_type;	/* ethernet, bridge, vlan, ... */
	uint16_t           ether_in_features;

	/* Network configuration bits */
	uint16_t	   capturing:1, /* capture in progress */
			   portmonitor:1,
			   vlan_modify:1,
			   qos_software_fwd:1,
			   tpid_offloaded:1,
			   flow_type:3,
			   ip_proxy_arp:1,
			   ip_mc_forwarding:1,
			   ip6_mc_forwarding:1,
			   if_uplink:1,
			   ip_rpf_strict:1,
			   if_local_port:1, /* if_port is valid */
			   qinq_outer:1,
			   qinq_inner:1;

	vrfid_t		   if_vrfid;	/* vrf tag */
	struct lltable	   *if_lltable;	/* IPv4 address mapping */
	struct lltable	   *if_lltable6; /* IPv6 address mapping */
	struct npf_if	   *if_npf;	/* NPF specific info */
	struct ifnet	   *if_parent;	/* real device for vlan */

	struct ether_addr  eth_addr;
	uint8_t            ip_encap_features;
	uint8_t            ip6_encap_features;

	void *padding1[0]   __rte_cache_aligned;
	/* --- cacheline 1 boundary (64 bytes) --- */
	struct ifnet	   **if_vlantbl; /* direct map of VLAN sub-devices */
	struct bridge_port *if_brport;

	uint16_t           ip_in_features;
	uint16_t           ip_out_features;
	uint16_t           ip6_in_features;
	uint16_t           ip6_out_features;

	struct mvl_tbl	   *if_macvlantbl; /* table of MACVLAN sub-devices */
	struct sched_info  *if_qos;	/* Qos (if any) */
	struct capture_info *cap_info;  /* capture thread info */

	struct ifnet	   *if_xconnect; /* cross-connect destination i/f */

	void		   *if_softc;	/* device type dependent data */

	void *padding2[0]   __rte_cache_aligned;
	/* --- cacheline 2 boundary (128 bytes) --- */

	/* Feature state */
	struct flow_counters *if_sample;
	struct portmonitor_info *pminfo; /* portmonitor info */

	struct cds_lfht	   *mpls_label_table;

	/* Referenced on local packet to/from kernel path */
	struct ifnet       *aggregator; /* part of team */
	struct cds_lfht   *if_mcfltr_hash;   /* Table of filtered mcast pkts*/
	uint8_t            if_mac_filtr_supported:1,
			   if_mac_filtr_active:1,
			   if_mac_filtr_reprogram:1,
			   hw_forwarding:1; /* switch port hw fwded*/

	int8_t		   if_socket;	/* NUMA node (or -1 for ANY) */

	/* Administrative */
	uint16_t           if_mtu_adjusted;  /* MTU allowing for QinQ header */
	char		   if_name[IFNAMSIZ];	/* from controller "dp0p4p1" */
	enum cont_src_en   if_cont_src;

	int                if_allmcast_ref;  /* Number of mcast apps */
	uint16_t	   qinq_vif_cnt;
	uint16_t	   vif_cnt;
	unsigned int	   if_pcount;	/* promiscuous mode */

	struct ether_addr  perm_addr;   /* "permanent" MAC address */

	uint16_t	   mpls_labelspace;

	struct cds_list_head if_addrhead; /* list of addresses per if */
	struct cds_list_head if_list; /* List of all interfaces */
	struct cds_lfht_node ifname_hash; /* ifname hash table */
	struct cds_lfht_node ifindex_hash; /* ifindex hash table */
	struct rcu_head	   if_rcu;

	bool		   if_poe : 1,        /* poe is enabled */
			   unplugged : 1,     /* hot unplug event in progress */
			   if_team : 1,       /* this is a bonding device */
			   if_created : 1;   /* All i/f build actions done */
	fal_object_t       fal_l3;

	/* Software statistics */
	struct if_perf	   if_txpps;	/* packets rate */
	struct if_perf	   if_txbps;	/* bandwidth */
	struct if_perf	   if_rxpps;	/* packets rate */
	struct if_perf	   if_rxbps;	/* bandwidth */
	struct rte_timer   if_stats_timer; /* update performance */

	struct if_data	   if_data[RTE_MAX_LCORE];

	struct if_mpls_data if_mpls_data[RTE_MAX_LCORE];

	/* TCP MSS clamping feature type and value */
	uint16_t            tcp_mss_type[TCP_MSS_AF_SIZE];
	uint16_t            tcp_mss_value[TCP_MSS_AF_SIZE];

	/* GARP processing config */
	struct garp_cfg     ip_garp_op;

	/* storm control config */
	struct if_storm_ctl_info   *sc_info;

	/* ref counts for pipeline features */
	uint16_t if_feat_refcnt[IF_FEAT_COUNT];

	/* vlan feature object table */
	struct cds_lfht	   *vlan_feat_table;

	/* vlan-modify lookup table */
	struct vlan_mod_tbl_entry *vlan_mod_tbl;

	/* vlan-modify default entry */
	struct vlan_mod_tbl_entry *vlan_mod_default;
};

static inline uint16_t if_tpid(const struct ifnet *ifp)
{
	return ifp->tpid;
}

static inline vrfid_t if_vrfid(const struct ifnet *ifp)
{
	return CMM_ACCESS_ONCE(ifp->if_vrfid);
}

static inline struct vrf *if_vrf(const struct ifnet *ifp)
{
	/*
	 * It is guaranteed for the interface's vrf id to always be
	 * valid so the fast accessor can be used here, avoiding
	 * unnecessary checks.
	 */
	return vrf_get_rcu_fast(ifp->if_vrfid);
}

enum if_dump_state_type {
	/*
	 * Dump "regular" interface-specific stats, including
	 * per-queue stats.
	 */
	IF_DS_STATS,
	/*
	 * Dump extended interface-specific stats, including per-queue
	 * stats. These stats may have semantics specific to
	 * underlying drivers and so shouldn't be relied upon in the
	 * rest of the system.
	 */
	IF_DS_XSTATS,
	/*
	 * Dump device information. This can dump the location of the
	 * device in the system, as well as capabilities and limits of
	 * the device.
	 */
	IF_DS_DEV_INFO,
	/*
	 * Dump interface-specific state. This can be any state
	 * associated with the interface that may need to be displayed
	 * to the user, or is useful for debugging purposes that isn't
	 * covered by one of the other options.
	 */
	IF_DS_STATE,
	/*
	 * Dump more verbose interface state.  These include information
	 * detailed SFF-8472.
	 */
	IF_DS_STATE_VERBOSE,
};

enum if_vlan_header_type {
	IF_VLAN_HEADER_OUTER,
	IF_VLAN_HEADER_INNER,
};

/*
 * Per-interface-type functions
 */
struct ift_ops {
	int (*ifop_set_mtu)(struct ifnet *ifp, uint32_t new_mtu);
	/*
	 * Set the primary L2 address of port.
	 * Return 0 on success, 1 if no change to address, < 0 on error.
	 */
	int (*ifop_set_l2_address)(struct ifnet *ifp, uint32_t l2_addr_len,
				   void *l2_addr);

	/*
	 * Start the interface, i.e. bring it admin-up
	 */
	int (*ifop_start)(struct ifnet *ifp);

	/*
	 * Stop the interface, i.e. bring it admin-down
	 */
	int (*ifop_stop)(struct ifnet *ifp);

	/*
	 * Add a secondary L2 address.
	 *
	 * May be a unicast or multicast address.
	 */
	int (*ifop_add_l2_addr)(struct ifnet *ifp, void *l2_addr);

	/*
	 * Delete a secondary L2 address.
	 *
	 * May be a unicast or multicast address.
	 */
	int (*ifop_del_l2_addr)(struct ifnet *ifp, void *l2_addr);

	/*
	 * Initialise the interface
	 *
	 * Called before the interface is started (if admin-up at
	 * create time) and before it is added to interface databases.
	 */
	int (*ifop_init)(struct ifnet *ifp);

	/*
	 * Inform that the interface is about to uninitialised
	 *
	 * Called just before the interface is stopped and removed
	 * from interface databases.
	 */
	void (*ifop_pre_uninit)(struct ifnet *ifp);

	/*
	 * Uninitialise the interface
	 *
	 * Called with the interface stopped and after it is remove
	 * from interface databases.
	 */
	void (*ifop_uninit)(struct ifnet *ifp);

	/*
	 * Enable/disable VLAN filtering
	 *
	 * Enable/disable reception of packets with the specified VLAN
	 * for interfaces that support filtering of VLAN packets.
	 */
	int (*ifop_set_vlan_filter)(struct ifnet *ifp, uint16_t vlan,
				    bool enable);

	/*
	 * Set the VLAN protocol in the Ethernet header for VLAN RX
	 * stripping and TX encap offload.
	 *
	 * The protocol is in host endian format.
	 */
	int (*ifop_set_vlan_proto)(struct ifnet *ifp,
				   enum if_vlan_header_type type,
				   uint16_t proto);

	/*
	 * Enable/disable broadcast
	 *
	 * Enable/disable reception of broadcast packets.
	 */
	int (*ifop_set_broadcast)(struct ifnet *ifp, bool enable);

	/*
	 * Enable/disable promiscuous mode
	 *
	 * Enable/disable reception of all unicast packets vs. only
	 * for-us unicast packets.
	 */
	int (*ifop_set_promisc)(struct ifnet *ifp, bool enable);

	/*
	 * Dump interface state
	 *
	 * Dump interface state in JSON form.
	 */
	int (*ifop_dump)(struct ifnet *ifp, json_writer_t *wr,
			 enum if_dump_state_type type);

	/*
	 * Get interface stats for packets not seen by the software
	 * forwarding path
	 */
	int (*ifop_get_stats)(struct ifnet *ifp,
			      struct if_data *stats);

	/*
	 * Blink the interface LED (or beacon)
	 */
	int (*ifop_blink)(struct ifnet *ifp, bool on);

	/*
	 * set the backplane interface for this interface
	 */
	int (*ifop_set_backplane)(struct ifnet *ifp,
				  unsigned int bp_ifindex);

	/*
	 * get the backplane interface for this interface
	 */
	int (*ifop_get_backplane)(struct ifnet *ifp,
				  unsigned int *bp_ifindex);

	/*
	 * The interface create has finished in SW
	 */
	void (*ifop_create_finished)(struct ifnet *ifp,
				     const struct ether_addr *mac_addr);
};

struct lltable *in_domifattach(struct ifnet *);

static inline struct sockaddr_in *
satosin(struct sockaddr *sa)
{
	return (struct sockaddr_in *)sa;
}

static inline struct sockaddr_in6 *
satosin6(struct sockaddr *sa)
{
	return (struct sockaddr_in6 *)sa;
}

/*
 * Add embedded vlan tag to packet
 */
void if_add_vlan(struct ifnet *ifp, struct rte_mbuf **m);

static inline struct ifnet *
if_vlan_lookup(const struct ifnet *ifp, uint16_t vid)
{
	if (ifp->if_vlantbl != NULL)
		return rcu_dereference(ifp->if_vlantbl[vid]);
	else
		return NULL;
}

struct ifnet *if_alloc(const char *name, enum if_type type,
		       unsigned int mtu, const struct ether_addr *eth_addr,
		       int socketid);
void if_set_ifindex(struct ifnet *ifp, unsigned int ifindex);
void if_unset_ifindex(struct ifnet *ifp);
struct ifnet *if_hwport_alloc(unsigned int port,
			      const struct ether_addr *eth_addr,
			      int socketid);
void if_free(struct ifnet *ifp);
void netlink_if_free(struct ifnet *ifp);
void if_cleanup(enum cont_src_en cont_src);
bool if_setup_vlan_storage(struct ifnet *ifp);
void if_finish_create(struct ifnet *ifp, const char *ifi_type,
		      const char *kind,
		      const struct ether_addr *mac_addr);

int if_blink(struct ifnet *ifp, bool on);
bool if_stats(struct ifnet *ifp, struct if_data *stats);
void if_mpls_stats(const struct ifnet *ifp, struct if_mpls_data *stats);

const char *if_flags2str(char *buf, unsigned int flags);

struct ifnet *ifnet_byifindex(unsigned int ifindex);
struct ifnet *ifnet_byifname(const char *name);
struct ifnet *ifnet_byethname(const char *name);

struct ifnet *ifnet_byifname_cont_src(enum cont_src_en cont_src,
				      const char *ifname);
void if_set_vrf(struct ifnet *ifp, vrfid_t vrf_id);
void fal_if_update_forwarding_all(struct ifnet *ifp);
void fal_if_update_forwarding(struct ifnet *ifp, uint8_t family,
			      bool multicast);

char *if_port_info(const struct ifnet *ifp);

static inline struct ifnet *ifnet_byport(portid_t port)
{
	if (likely(port < DATAPLANE_MAX_PORTS))
		return rcu_dereference(ifport_table[port]);
	else
		return NULL;
}

static inline int ifnet_nametoindex(const char *ifname)
{
	struct ifnet *ifp = ifnet_byifname(ifname);

	return ifp ? ifp->if_index : 0;
}

static inline const char *ifnet_indextoname(int ifindex)
{
	struct ifnet *ifp = ifnet_byifindex(ifindex);

	return ifp ? ifp->if_name : NULL;
}

static inline const char *ifnet_indextoname_safe(int ifindex)
{
	const char *name = ifnet_indextoname(ifindex);

	return name ? name : "unknown";
}

void if_enable_poll(int port_id);
void if_disable_poll(portid_t port_id);

/* wait until forwarding threads no longer poll this port */
static inline void if_disable_poll_rcu(portid_t port_id)
{
	if_disable_poll(port_id);
	synchronize_rcu();
}

static inline bool if_port_isup(portid_t portid)
{
	if (likely(portid < DATAPLANE_MAX_PORTS))
		return (bitmask_isset(&linkup_port_mask, portid)) != 0;
	else
		return false;
}

static inline void if_incr_in(struct ifnet *ifp, struct rte_mbuf *m)
{
	struct if_data *ifstat = &ifp->if_data[dp_lcore_id()];

	++ifstat->ifi_ipackets;
	ifstat->ifi_ibytes += rte_pktmbuf_pkt_len(m);
}

static inline void if_incr_out(struct ifnet *ifp, struct rte_mbuf *m)
{
	struct if_data *ifstat = &ifp->if_data[dp_lcore_id()];

	++ifstat->ifi_opackets;
	ifstat->ifi_obytes += rte_pktmbuf_pkt_len(m);
}

static inline void if_incr_dropped(struct ifnet *ifp)
{
	struct if_data *ifstat = &ifp->if_data[dp_lcore_id()];

	++ifstat->ifi_idropped;
}

static inline void if_incr_full_txring(struct ifnet *ifp, unsigned int count)
{
	struct if_data *ifstat = &ifp->if_data[dp_lcore_id()];

	ifstat->ifi_odropped_txring += count;
}

static inline void if_incr_full_hwq(struct ifnet *ifp, unsigned int count)
{
	struct if_data *ifstat = &ifp->if_data[dp_lcore_id()];

	ifstat->ifi_odropped_hwq += count;
}

static inline void if_incr_full_proto(struct ifnet *ifp, unsigned int count)
{
	struct if_data *ifstat = &ifp->if_data[dp_lcore_id()];

	ifstat->ifi_odropped_proto += count;
}

static inline void if_incr_error(struct ifnet *ifp)
{
	struct if_data *ifstat = &ifp->if_data[dp_lcore_id()];

	++ifstat->ifi_ierrors;
}

static inline void if_incr_oerror(struct ifnet *ifp)
{
	struct if_data *ifstat = &ifp->if_data[dp_lcore_id()];

	++ifstat->ifi_oerrors;
}

static inline void if_incr_unknown(struct ifnet *ifp)
{
	struct if_data *ifstat = &ifp->if_data[dp_lcore_id()];
	++ifstat->ifi_unknown;
}

static inline void if_incr_no_vlan(struct ifnet *ifp)
{
	struct if_data *ifstat = &ifp->if_data[dp_lcore_id()];
	++ifstat->ifi_no_vlan;
}

static inline void mpls_if_incr_in_ucastpkts(struct ifnet *ifp, uint16_t len)
{
	struct if_mpls_data *ifstat = &ifp->if_mpls_data[dp_lcore_id()];

	++ifstat->ifm_in_ucastpkts;
	ifstat->ifm_in_octets += len;
}

static inline void mpls_if_incr_out_ucastpkts(struct ifnet *ifp, uint16_t len)
{
	struct if_mpls_data *ifstat = &ifp->if_mpls_data[dp_lcore_id()];

	++ifstat->ifm_out_ucastpkts;
	ifstat->ifm_out_octets += len;
}

static inline void mpls_if_incr_in_errors(struct ifnet *ifp)
{
	struct if_mpls_data *ifstat = &ifp->if_mpls_data[dp_lcore_id()];

	++ifstat->ifm_in_errors;
}

static inline void mpls_if_incr_out_errors(struct ifnet *ifp)
{
	struct if_mpls_data *ifstat = &ifp->if_mpls_data[dp_lcore_id()];

	++ifstat->ifm_out_errors;
}

static inline void mpls_if_incr_lbl_lookup_failures(struct ifnet *ifp)
{
	struct if_mpls_data *ifstat = &ifp->if_mpls_data[dp_lcore_id()];

	++ifstat->ifm_lbl_lookup_failures;
}

static inline void mpls_if_incr_out_fragment_pkts(struct ifnet *ifp)
{
	struct if_mpls_data *ifstat = &ifp->if_mpls_data[dp_lcore_id()];

	++ifstat->ifm_out_fragment_pkts;
}

static inline bool is_gre(const struct ifnet *ifp)
{
	return ifp->if_type == IFT_TUNNEL_GRE;
}

static inline bool is_vti(const struct ifnet *ifp)
{
	return ifp->if_type == IFT_TUNNEL_VTI;
}

static inline bool is_tunnel(const struct ifnet *ifp)
{
	switch (ifp->if_type) {
	case IFT_TUNNEL_VTI:
	case IFT_TUNNEL_GRE:
	case IFT_TUNNEL_OTHER:
		return true;
	}
	return false;
}

static inline bool is_bridge(const struct ifnet *ifp)
{
	return ifp->if_type == IFT_BRIDGE;
}

static inline bool is_l2vlan(const struct ifnet *ifp)
{
	return ifp->if_type == IFT_L2VLAN;
}

/* Choose whether interface is handled by team daemon.
 * if_team values:
 *    0: not handled by team daemon
 *    1: handled by team daemon
 */
static inline bool is_team(const struct ifnet *ifp)
{
	return ifp && ifp->if_team;
}

bool is_lo(const struct ifnet *ifp);
bool is_s2s_feat_attach(const struct ifnet *ifp);
int cmd_set_vfp(FILE *f, int argc, char **argv);

typedef void ifnet_iter_func_t(struct ifnet *ifp, void *arg);
void ifnet_walk(ifnet_iter_func_t func, void *arg);

int if_vlan_proto_set(struct ifnet *ifp, uint16_t proto);
void if_qinq_created(struct ifnet *phy_ifp);
void if_qinq_deleted(struct ifnet *phy_ifp);
int if_add_l2_addr(struct ifnet *ifp, struct ether_addr *addr);
int if_del_l2_addr(struct ifnet *ifp, struct ether_addr *addr);

void ifpromisc(struct ifnet *ifp, int onswitch);
void if_allmulti(struct ifnet *ifp, int onswitch);
int if_start(struct ifnet *ifp);
int if_stop(struct ifnet *ifp);
int if_set_vlan_filter(struct ifnet *ifp, uint16_t vlan, bool enable);
int if_set_broadcast(struct ifnet *ifp, bool enable);
void if_create_finished(struct ifnet *ifp, const struct ether_addr *mac_addr);
uint64_t if_scaled(uint64_t value);
void send_if_stats(const struct ifnet *ifp, const struct if_data *stats);

bool ifa_broadcast(struct ifnet *ifp, uint32_t dst);
void ifa_add(int ifindex, int family, uint32_t scope,
	     const void *addr, uint8_t prefixlen, const void *broadcast);
void ifa_remove(int ifindex, int family, const void *addr, uint8_t prefixlen);
void ifa_flush(struct ifnet *ifp);
uint32_t ifa_count_addr(struct ifnet *ifp, int family);
bool ifa_has_addr(struct ifnet *ifp, int family);
void if_rename(struct ifnet *ifp, const char *ifname);
void if_port_inherit(struct ifnet *parent, struct ifnet *child);
void interface_init(void);
void interface_cleanup(void);

void incomplete_interface_init(void);
void incomplete_interface_cleanup(void);
bool is_ignored_interface(uint32_t ifindex);
void incomplete_if_add_ignored(uint32_t ifindex);
void incomplete_if_del_ignored(uint32_t ifindex);
void incomplete_routes_make_complete(void);
void incomplete_route_add(vrfid_t vrf_id, const void *dst,
			  uint8_t family, uint8_t depth, uint32_t table,
			  uint8_t scope, uint8_t proto,
			  const struct nlmsghdr *nlh);
void incomplete_route_del(vrfid_t vrf_id, const void *dst,
			  uint8_t family, uint8_t depth, uint32_t table,
			  uint8_t scope, uint8_t proto);
void missed_netlink_replay(unsigned int ifindex);
void missed_nl_unspec_link_add(unsigned int ifindex,
			       const struct nlmsghdr *nlh);
void missed_nl_unspec_link_del(unsigned int ifindex);
void missed_nl_unspec_addr_add(unsigned int ifindex,
			       const struct ether_addr *addr,
			       const struct nlmsghdr *nlh);
void missed_nl_unspec_addr_del(unsigned int ifindex,
			       const struct ether_addr *addr);
void missed_nl_inet_addr_add(unsigned int ifindex,
			     unsigned char family,
			     const void *addr,
			     const struct nlmsghdr *nlh);
void missed_nl_inet_addr_del(unsigned int ifindex,
			     unsigned char family,
			     const void *addr);
void missed_nl_inet_netconf_add(unsigned int ifindex,
				unsigned char family,
				const struct nlmsghdr *nlh);
void missed_nl_inet_netconf_del(unsigned int ifindex,
				unsigned char family);
void missed_nl_child_link_add(unsigned int ifindex,
			      unsigned int child_ifindex,
			      const struct nlmsghdr *nlh);
void missed_nl_child_link_del(unsigned int ifindex,
			      unsigned int child_ifindex);
void if_set_cont_src(struct ifnet *ifp, enum cont_src_en cont_src);
bool if_port_is_uplink(portid_t portid);
bool if_is_control_channel(struct ifnet *ifp);
bool if_port_is_owned_by_src(enum cont_src_en cont_src, portid_t portid);
bool if_port_is_bkplane(portid_t portid);

void if_create_l3_intf(struct ifnet *ifp,
		       const struct ether_addr *mac_addr);
void if_delete_l3_intf(struct ifnet *ifp);
int if_set_l3_intf_attr(struct ifnet *ifp,
			struct fal_attribute_t *attr);

/* TODO:  Look into consolidating the if_is_uplink and
 * is_local_controller checks across the codebase.
 */
static inline bool if_is_uplink(struct ifnet *ifp)
{
	return ifp->if_uplink;
}

static inline enum if_role if_role(struct ifnet *ifp)
{
	if (ifp->if_uplink)
		return IF_ROLE_UPLINK;
	if (ifp->if_type == IFT_ETHER)
		return IF_ROLE_PORT;

	return IF_ROLE_NONE;
}

const char *iftype_name(uint8_t type);

bool if_ignore_df(const struct ifnet *ifp);

/*
 * Register operations for a certain type of interface
 */
int if_register_type(enum if_type type, const struct ift_ops *fns);

/*
 * Set the MTU of an interface
 */
int if_set_mtu(struct ifnet *ifp, uint32_t mtu, bool force_update);

/*
 * Set the Layer2 address of an interface
 */
int if_set_l2_address(struct ifnet *ifp, uint32_t l2_addr_len, void *l2_addr);

/*
 * Set PoE status on an interface
 */
int if_set_poe(struct ifnet *ifp, bool enable);

/*
 * Get PoE status on an interface
 */
int if_get_poe(struct ifnet *ifp, bool *admin_status, bool *oper_status);

/*
 * Get link status.
 *
 * Up means signal detected and auto-negotiate successfully completed.
 */
void if_get_link_status(struct ifnet *ifp,
			struct if_link_status *if_link);
/*
 * Dump state for an interface in JSON format.
 */
int if_dump_state(struct ifnet *ifp, json_writer_t *wr,
		  enum if_dump_state_type type);

/*
 * APIs for vlan feature block on interface
 */
int if_vlan_feat_create(struct ifnet *ifp, uint16_t vlan,
			fal_object_t fal_obj);

struct if_vlan_feat *if_vlan_feat_get(struct ifnet *ifp, uint16_t vlan);

int if_vlan_feat_delete(struct ifnet *ifp, uint16_t vlan);

/*
 * APIs to set/get backplane interface
 * Currently supported only on ethernet interfaces
 */
int if_set_backplane(struct ifnet *ifp, unsigned int ifindex);
int if_get_backplane(struct ifnet *ifp, unsigned int *ifindex);

static inline bool
if_is_hwport(struct ifnet *ifp)
{
	/* If if_local_port is set and it has not been inherited from a parent
	 * then this interface really is the underlying dpdk 'hardware'
	 * interface.
	 */
	return ifp->if_local_port && !ifp->if_parent;
}

#endif /* !IF_VAR_H */
