/*
 * Copyright (c) 2017-2019, AT&T Intellectual Property.  All rights reserved.
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 */

#include <stdio.h>
#include <stdint.h>
#include <arpa/inet.h>

#include "rte_debug.h"

#include "util.h"
#include "dp_event.h"
#include "urcu.h"

/*
 * Fixed size array for holding event operations pointers.
 */
static struct dp_event_ops *dp_ops[DP_EVENT_MAX_OPS];

/* Process the event for all registered operations */
static void dp_evt_notify(enum dp_evt evt, uint32_t cont_src,
		const struct dp_event_ops *ops, void *obj, uint32_t val,
		uint32_t val2, const void *data)
{
	switch (evt) {
	case DP_EVT_IF_CREATE:
		if (ops->if_create)
			ops->if_create(obj);
		break;
	case DP_EVT_IF_CREATE_FINISHED:
		if (ops->if_create_finished)
			ops->if_create_finished(obj);
		break;
	case DP_EVT_IF_DELETE:
		if (ops->if_delete)
			ops->if_delete(obj);
		break;
	case DP_EVT_IF_INDEX_SET:
		if (ops->if_index_set)
			ops->if_index_set(obj, val);
		break;
	case DP_EVT_IF_INDEX_PRE_UNSET:
		if (ops->if_index_pre_unset)
			ops->if_index_pre_unset(obj);
		break;
	case DP_EVT_IF_INDEX_UNSET:
		if (ops->if_index_unset)
			ops->if_index_unset(obj, val);
		break;
	case DP_EVT_IF_RENAME:
		if (ops->if_rename)
			ops->if_rename(obj, data);
		break;
	case DP_EVT_IF_VRF_SET:
		if (ops->if_vrf_set)
			ops->if_vrf_set(obj);
		break;
	case DP_EVT_IF_ADDR_ADD:
		/* args: cont_src, ifindex, family, addr */
		if (ops->if_addr_add)
			ops->if_addr_add(cont_src, obj, val, val2, data);
		break;
	case DP_EVT_IF_ADDR_DEL:
		/* args: cont_src, ifindex, family, addr */
		if (ops->if_addr_delete)
			ops->if_addr_delete(cont_src, obj, val, val2, data);
		break;
	case DP_EVT_RESET_CONFIG:
		if (ops->reset_config)
			ops->reset_config(cont_src);
		break;
	case DP_EVT_VRF_CREATE:
		if (ops->vrf_create)
			ops->vrf_create(obj);
		break;
	case DP_EVT_VRF_DELETE:
		if (ops->vrf_delete)
			ops->vrf_delete(obj);
		break;
	case DP_EVT_IF_MAC_ADDR_CHANGE:
		if (ops->if_mac_addr_change)
			ops->if_mac_addr_change(obj, data);
		break;
	case DP_EVT_IF_LINK_CHANGE:
		if (ops->if_link_change)
			ops->if_link_change(obj, val, val2);
		break;
	case DP_EVT_IF_VLAN_ADD:
		if (ops->if_vlan_add)
			ops->if_vlan_add(obj, val);
		break;
	case DP_EVT_IF_VLAN_DEL:
		if (ops->if_vlan_del)
			ops->if_vlan_del(obj, val);
		break;

	case DP_EVT_IF_HW_SWITCHING_CHANGE:
		if (ops->if_hw_switching_change)
			ops->if_hw_switching_change(obj, val);
		break;
	case DP_EVT_INIT:
		if (ops->init)
			ops->init();
		break;
	case DP_EVT_UNINIT:
		if (ops->uninit)
			ops->uninit();
		break;
	default:
		rte_panic("dp_event: unknown event: %u\n", evt);
	}
}

/* Process a dataplane event */
void dp_event(enum dp_evt evt, uint32_t cont_src, void *obj,
		uint32_t val, uint32_t val2, const void *data)
{
	uint32_t i;
	struct dp_event_ops *ops;

	for (i = 0; i < ARRAY_SIZE(dp_ops); i++) {
		ops = rcu_dereference(dp_ops[i]);
		if (ops)
			dp_evt_notify(evt, cont_src, ops, obj, val, val2, data);
	}
}

/* Register event ops */
void dp_event_register(const struct dp_event_ops *ops)
{
	uint32_t i;

	for (i = 0; i < ARRAY_SIZE(dp_ops); i++) {
		if (!rcu_cmpxchg_pointer(&dp_ops[i], NULL,
					(struct dp_event_ops *)ops))
			return;
	}

	rte_panic("dp_event: register: no space for ops\n");
}

/* Unregister event ops */
void dp_event_unregister(const struct dp_event_ops *op)
{
	uint32_t i;
	struct dp_event_ops *ops = (struct dp_event_ops *) op;

	for (i = 0; i < ARRAY_SIZE(dp_ops); i++) {
		if (rcu_cmpxchg_pointer(&dp_ops[i], ops, NULL) == ops)
			return;
	}
}
