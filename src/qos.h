/*-
 * Copyright (c) 2017-2019, AT&T Intellectual Property.  All rights reserved.
 * Copyright (c) 2013-2016 by Brocade Communications Systems, Inc.
 * All rights reserved.
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 */

#ifndef QOS_H
#define QOS_H


#include <rte_sched.h>

#include "if_var.h"
#include "npf/npf_ruleset.h"
#include "fal_plugin.h"
#include "json_writer.h"

struct rte_sched_port;

#define DEFAULT_QSIZE	64
#define MAX_QSIZE	8192
#define DEFAULT_TBSIZE	10000
#define DEFAULT_Q	3	/* class 3: queue 0 */
#define MAX_PIPES       256

#define MAX_RED_QUEUE_LENGTH 8192

#define	QOS_DPDK_ID	0
#define	QOS_HW_ID	1
#define	NUM_DEVS	2

#define QOS_MAX_DROP_PRECEDENCE 2

struct npf_act_grp;

struct red_params {
	uint32_t min_th;   /* Minimum threshold in bytes for queue */
	uint32_t max_th;   /* Maximum threshold in bytes for queue */
	uint16_t maxp_inv; /* Inverse of packet marking probability */
};

enum wred_unit {
	WRED_PACKETS,
	WRED_BYTES
};

struct queue_wred_info {
	union {
		struct rte_red_params map_params[QOS_MAX_DROP_PRECEDENCE + 1];
		struct red_params map_params_bytes[QOS_MAX_DROP_PRECEDENCE + 1];
	} params;
	char *dscp_grp_names[QOS_MAX_DROP_PRECEDENCE + 1];
	uint8_t filter_weight;
	uint8_t num_maps;
	enum wred_unit unit;
};

struct profile_wred_info {
	struct queue_wred_info queue_wred[RTE_SCHED_QUEUES_PER_PIPE];
};

struct qos_mark_map {
	struct rcu_head obj_rcu;
	struct cds_list_head list;
	uint8_t pcp_value[MAX_DSCP];
	char map_name[0];
};

struct qos_rate_info {
	bool bw_is_percent;
	union _bw_info {
		uint8_t bw_percent;
		uint32_t bandwidth;
	} rate;
	uint32_t burst; /* value of 0 indicates burst should be calculated */
	uint32_t period;
};

struct qos_tc_rate_info {
	struct qos_rate_info tc_rate[RTE_SCHED_TRAFFIC_CLASSES_PER_PIPE];
};

/* Qos Scheduler sub port (one per vlan) */
struct subport_info {
	char attach_name[IFNAMSIZ + sizeof("/4294967295")];
		/* Big enough to hold: "<if-name>/<unsigned-int>" */
	struct npf_config *npf_config;
	struct npf_act_grp *act_grp_list;
	struct mark_reqs *marks;
	uint8_t *profile_map;		/* pipe to profile */
	struct rte_sched_subport_params params;
	struct qos_rate_info subport_rate;
	struct qos_tc_rate_info sp_tc_rates;

	uint32_t match_id;		/* Used as index for npf matches */
	uint32_t vlan_id;
	struct rte_sched_subport_stats64 queue_stats; /* Non-zeroing counts */
	struct rte_sched_subport_stats64 clear_stats; /* Counts at last clear */
	uint32_t qsize[RTE_SCHED_TRAFFIC_CLASSES_PER_PIPE];
	struct rte_red_params red_params[RTE_SCHED_TRAFFIC_CLASSES_PER_PIPE]
					[e_RTE_METER_COLORS];
	bool pipe_configured[MAX_PIPES];
	struct qos_mark_map *mark_map;
};

/* DSCP and PCP maps (per profile) */
struct queue_map {
	uint8_t pcp2q[MAX_PCP];	/* Priority Code Point -> queue */
	uint8_t dscp2q[MAX_DSCP];/* DSCP -> queue */
	uint8_t local_priority_queue; /* Queue for hi-prio from-us packets */
	uint8_t pcp_enabled:1,	/* Flag to track user-defined PCP map */
		dscp_enabled:1, /* Flag to track user-defined DSCP map */
		local_priority:1, /* Local priority queue enabled */
		unused:5;
	uint8_t conf_ids[RTE_SCHED_QUEUES_PER_PIPE]; /* The configured Q ids */
};

#define CONF_ID_Q_CONFIG  0x80
#define CONF_ID_Q_DEFAULT 0x40
#define CONF_ID_Q_IN_USE (CONF_ID_Q_CONFIG | CONF_ID_Q_DEFAULT)

/* Qos queue counters (one per queue) */
struct queue_stats {
	/* The ever-increasing counts */
	uint64_t n_bytes;
	uint64_t n_bytes_dropped;
	uint64_t n_pkts;
	uint64_t n_pkts_dropped;
	uint64_t n_pkts_red_dropped;
	uint64_t n_pkts_red_dscp_dropped[RTE_NUM_DSCP_MAPS];
	/* The values of the ever-increasing counts at the last clear */
	uint64_t n_bytes_lc;
	uint64_t n_bytes_dropped_lc;
	uint64_t n_pkts_lc;
	uint64_t n_pkts_dropped_lc;
	uint64_t n_pkts_red_dropped_lc;
	uint64_t n_pkts_red_dscp_dropped_lc[RTE_NUM_DSCP_MAPS];
};

/* Qos Scheduler handles (one per physical port) */
struct sched_info {
	int dev_id;			/* Device ID - DPDK or FAL */
	union _dev_info {
		struct _dpdk {
			struct rte_sched_port *port;	/* DPDK object */
		} dpdk;
		struct _fal {
			fal_object_t hw_port_sched_group; /* FAL object */
			uint32_t hw_port_id;              /* FAL id */
		} fal;
	} dev_info;
	struct subport_info *subport;	/* Subport's */
	struct rte_sched_port_params port_params;
	struct qos_rate_info *profile_rates;
	struct qos_tc_rate_info *profile_tc_rates;
	struct qos_rate_info port_rate;
	bool    enabled;
	struct rcu_head rcu;

	/* subports and pipes as configured, actual size is in port_params */
	uint32_t n_subports;		/* Original values */
	uint32_t n_pipes;

	uint16_t vlan_map[VLAN_N_VID];	/* Vlan vid to sub-port policy */
	struct queue_map *queue_map;
	struct queue_stats *queue_stats;
	rte_spinlock_t stats_lock;      /* To control access to queue-stats */
	struct profile_wred_info *wred_profiles;
};

struct mark_reqs {
	struct mark_reqs *next;
	void             **handle;
	int		refs;
	enum qos_mark_type {
		MARK,
		POLICE
	} type;
};

struct qos_show_context {
	json_writer_t *wr;
	bool optimised_json;
	bool is_platform;
};

/*
 * This structure is used as the device plug in structure.
 * There will always be one for the DPDK, we may need a second
 * one if there's also a hardware forwarding path.
 */
struct qos_dev {
	int (*qos_disable)(struct ifnet *ifp, struct sched_info *q);
	int (*qos_enable)(struct ifnet *ifp, struct sched_info *q);
	int (*qos_start)(struct ifnet *ifp, struct sched_info *q,
			 uint64_t bps, uint16_t min_len);
	int (*qos_stop)(struct ifnet *ifp, struct sched_info *q);
	void (*qos_free)(struct sched_info *q);
	int (*qos_subport_read_stats)(struct sched_info *q, uint32_t sub,
				      struct rte_sched_subport_stats64 *st);
	int (*qos_subport_clear_stats)(struct sched_info *q, uint32_t sub);
	int (*qos_queue_read_stats)(struct sched_info *q, uint32_t subport,
				    uint32_t pipe, uint32_t tc, uint32_t queue,
				    struct queue_stats *st, uint64_t *qlen,
				    bool *qlen_in_pkts);
	int (*qos_queue_clear_stats)(struct sched_info *q, uint32_t subport,
				     uint32_t pipe, uint32_t tc,
				     uint32_t queue);
	void (*qos_dscp_resgrp_json)(struct sched_info *qinfo, uint32_t subport,
				     uint32_t pipe, uint32_t tc, uint32_t q,
				     uint64_t *random_dscp_drop,
				     json_writer_t *wr);
};

extern struct qos_dev qos_devices[];

/* Encode DPDK Traffic-Class and Queue */
#define QMAP(tc, wrr)	(wrr << RTE_SCHED_TC_BITS | (tc))

#define	QOS_FREE(qinfo)		qos_devices[qinfo->dev_id].qos_free
#define	QOS_START(qinfo)	qos_devices[qinfo->dev_id].qos_start
#define	QOS_STOP(qinfo)		qos_devices[qinfo->dev_id].qos_stop
#define	QOS_SUBPORT_RD_STATS(qinfo) \
			qos_devices[qinfo->dev_id].qos_subport_read_stats
#define	QOS_SUBPORT_CLR_STATS(qinfo) \
			qos_devices[qinfo->dev_id].qos_subport_clear_stats
#define	QOS_QUEUE_RD_STATS(qinfo) \
			qos_devices[qinfo->dev_id].qos_queue_read_stats
#define	QOS_QUEUE_CLR_STATS(qinfo) \
			qos_devices[qinfo->dev_id].qos_queue_clear_stats
#define	QOS_DISABLE(qinfo)	qos_devices[qinfo->dev_id].qos_disable
#define	QOS_ENABLE(qinfo)	qos_devices[qinfo->dev_id].qos_enable
#define QOS_DSCP_RESGRP_JSON(qinfo) \
			qos_devices[qinfo->dev_id].qos_dscp_resgrp_json
#define QOS_CONFIGURED(qinfo) \
	(qinfo->dev_info.dpdk.port || qinfo->dev_info.fal.hw_port_id)

/*
 * Given an interface walk back to the parent device (if a vlan)
 * to find the QoS handle (if any)
 */
static inline struct sched_info *qos_handle(const struct ifnet *ifp)
{
	if (likely(!ifp->qos_software_fwd))
		return NULL;

	return rcu_dereference(ifp->if_qos);
}

/*
 * The bottom RTE_SCHED_TC_BITS bits is the TC.
 * The next RTE_SCHED_WRR_BITS is the q index.
 * The next two bits are the drop precedence
 */
static inline unsigned int q_from_mask(unsigned int value)
{
	return ((value & RTE_SCHED_TC_MASK) *
		RTE_SCHED_QUEUES_PER_TRAFFIC_CLASS) +
		((value >> RTE_SCHED_TC_BITS) & RTE_SCHED_WRR_MASK);
}

/*
 * Functions to insert/extract { traffic-class, queue, drop-precedence } from
 * the map
 */
static inline uint8_t qmap_to_tc(uint8_t m)
{
	return m & RTE_SCHED_TC_MASK;
}

static inline uint8_t qmap_to_wrr(uint8_t m)
{
	return (m >> RTE_SCHED_TC_BITS) & RTE_SCHED_WRR_MASK;
}

static inline uint8_t qmap_to_dp(uint8_t m)
{
	return m >> RTE_SCHED_TC_WRR_BITS;
}

_Static_assert(sizeof(struct rte_sched_subport_params) !=
	       sizeof(struct rte_sched_pipe_params),
	       "Structures should be the same size.");

void qos_sched_subport_params_check(struct rte_sched_subport_params *params,
				struct qos_rate_info *config_rate,
				struct qos_rate_info *config_tc_rate,
				uint16_t max_pkt_len, uint32_t bps);


static inline void qos_sched_pipe_check(struct sched_info *qinfo,
					uint16_t max_pkt_len, uint32_t bps)
{
	unsigned int profile;

	for (profile = 0;
	     profile < qinfo->port_params.n_pipe_profiles;
	     profile++) {
		unsigned int subport;
		uint32_t parent_rate = bps;
		struct rte_sched_pipe_params *p
			= qinfo->port_params.pipe_profiles + profile;

		/*
		 * Find the subport each pipe profile is associated with.
		 * This will allow the pipe rate (and its TCs) to be restored
		 * based on its parent subport rate.
		 */
		for (subport = 0;
		     subport < qinfo->port_params.n_subports_per_port;
		     subport++) {
			unsigned int pipe;
			struct subport_info *sinfo = qinfo->subport + subport;

			for (pipe = 0; pipe < MAX_PIPES; pipe++) {
				if (sinfo->pipe_configured[pipe] &&
				    sinfo->profile_map[pipe] == profile) {
					parent_rate = sinfo->params.tb_rate;
					break;
				}
			}
			if (pipe != MAX_PIPES)
				break;
		}

		/*
		 * Make sure no pipe is faster than actual line or subport rate.
		 * We're casting the pipe params to the subport params, which
		 * will work as long as they share the first 4 fields. If this
		 * breaks, check if these structures have changed.
		 */
		qos_sched_subport_params_check(
			(struct rte_sched_subport_params *)p,
			&qinfo->profile_rates[profile],
			qinfo->profile_tc_rates[profile].tc_rate,
			max_pkt_len, parent_rate);
	}
}

void qos_init(void);
int qos_sched_start(struct ifnet *ifp, uint64_t link_speed);
void qos_sched_stop(struct ifnet *ifp);
uint32_t qos_sched_calc_qindex(struct sched_info *qinfo, unsigned int subport,
			       unsigned int pipe, unsigned int tc,
			       unsigned int q);
struct sched_info;
int qos_sched(struct ifnet *ifp, struct sched_info *info,
	      struct rte_mbuf **in, uint32_t n_in,
	      struct rte_mbuf **out, uint32_t n_out);
struct subport_info *qos_get_subport(const char *name, struct ifnet **ifp);
struct npf_act_grp *qos_ag_get_head(struct subport_info *subport);
struct npf_act_grp *qos_ag_set_or_get_head(struct subport_info *subport,
					   struct npf_act_grp *act_grp);
int16_t qos_get_overhead(const char *name);
int16_t qos_get_overhead_from_ifnet(struct ifnet *ifp);
void qos_sched_update_if_stats(const struct ifnet *ifp);
bool qos_sched_subport_get_stats(struct sched_info *qinfo, uint16_t vlan_id,
				 struct rte_sched_subport_stats64 *stats);
struct ifnet *qos_get_vlan_ifp(const char *att_pnt, uint16_t *vlan_id);
void qos_save_mark_req(const char *att_pnt, enum qos_mark_type type,
		       uint16_t refs, void **handle);
void qos_save_mark_v_pol(npf_rule_t *rl, void *po);
void qos_enable_inner_marking(struct ifnet *ifp, uint16_t vlan_id);
void qos_disable_inner_marking(struct ifnet *ifp, uint16_t vlan_id);
void qos_subport_npf_free(struct sched_info *qinfo);
struct sched_info *qos_sched_new(struct ifnet *ifp, unsigned int subports,
				 unsigned int pipes, unsigned int profiles,
				 int32_t overhead);
void qos_sched_free(struct sched_info *qinfo);
void qos_sched_free_rcu(struct rcu_head *head);

int qos_hw_show_port(struct ifnet *ifp, void *arg);
void qos_hw_dump_map(json_writer_t *wr, const struct sched_info *qinfo,
		     uint32_t subport, uint32_t pipe);
void qos_hw_dump_subport(json_writer_t *wr, const struct sched_info *qinfo,
			 uint32_t subport);

/* The DPDK plugin functions */
void qos_dpdk_dscp_resgrp_json(struct sched_info *qinfo, uint32_t subport,
			       uint32_t pipe, uint32_t tc, uint32_t q,
			       uint64_t *random_dscp_drop,
			       json_writer_t *wr);
int qos_dpdk_subport_read_stats(struct sched_info *qinfo, uint32_t subport,
				struct rte_sched_subport_stats64 *stats);
int qos_dpdk_subport_clear_stats(struct sched_info *qinfo, uint32_t subport);
int qos_dpdk_queue_read_stats(struct sched_info *qinfo, uint32_t subport,
			      uint32_t pipe, uint32_t tc, uint32_t q,
			      struct queue_stats *st, uint64_t *qlen,
			      bool *qlen_in_pkts);
int qos_dpdk_queue_clear_stats(struct sched_info *qinfo,
			       uint32_t subport, uint32_t pipe,
			       uint32_t tc, uint32_t q);
void qos_dpdk_free(struct sched_info *qinfo);
int qos_dpdk_port(struct ifnet *ifp,
		  unsigned int subports, unsigned int pipes,
		  unsigned int profiles, unsigned int overhead);
int qos_dpdk_disable(struct ifnet *ifp, struct sched_info *qinfo);
int qos_dpdk_enable(struct ifnet *ifp,
		    struct sched_info *qinfo);
int qos_dpdk_stop(__unused struct ifnet *ifp, struct sched_info *qinfo);
int qos_dpdk_start(struct ifnet *ifp, struct sched_info *qinfo,
		   uint64_t bps, uint16_t max_pkt_len);

/* The HW forwarding plugin functions */
fal_object_t
qos_hw_get_ingress_map(uint32_t port_obj_id, uint32_t subport_id,
		       uint32_t pipe_id);
fal_object_t
qos_hw_get_egress_map(uint32_t port_obj_id, uint32_t subport_id,
		      uint32_t pipe_id);
fal_object_t
qos_hw_get_subport_sg(uint32_t port_obj_id, uint32_t subport_id);
void qos_hw_dscp_resgrp_json(struct sched_info *qinfo, uint32_t subport,
			     uint32_t pipe, uint32_t tc, uint32_t q,
			     uint64_t *random_dscp_drop,
			     json_writer_t *wr);
int qos_hw_subport_read_stats(struct sched_info *qinfo, uint32_t subport,
			      struct rte_sched_subport_stats64 *stats);
int qos_hw_subport_clear_stats(struct sched_info *qinfo, uint32_t subport);
int qos_hw_queue_read_stats(struct sched_info *qinfo, uint32_t subport,
			    uint32_t pipe, uint32_t tc, uint32_t queue,
			    struct queue_stats *stats, uint64_t *qlen,
			    bool *qlen_in_pkts);
int qos_hw_queue_clear_stats(struct sched_info *qinfo, uint32_t subport,
			     uint32_t pipe, uint32_t tc, uint32_t q);
void qos_hw_free(__unused struct sched_info *qinfo);
int qos_hw_port(struct ifnet *ifp,
		unsigned int subports, unsigned int pipes,
		unsigned int profiles, unsigned int overhead);
int qos_hw_disable(__unused struct ifnet *ifp, struct sched_info *qinfo);
int qos_hw_enable(struct ifnet *ifp, struct sched_info *qinfo);
int qos_hw_stop(__unused struct ifnet *ifp,
		__unused struct sched_info *qinfo);
int qos_hw_start(__unused struct ifnet *ifp, struct sched_info *qinfo,
		 uint64_t bps, uint16_t max_pkt_len);

#endif /* QOS_H */
