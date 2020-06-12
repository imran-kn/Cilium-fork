// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2016-2020 Authors of Cilium */

#include <bpf/ctx/skb.h>
#include <bpf/api.h>

#include <node_config.h>
#include <ep_config.h>

#define EVENT_SOURCE HOST_EP_ID

/* These are configuartion options which have a default value in their
 * respective header files and must thus be defined beforehand:
 *
 * Pass unknown ICMPv6 NS to stack */
#define ACTION_UNKNOWN_ICMP6_NS CTX_ACT_OK

/* Include policy_can_access_ingress() */
#define REQUIRES_CAN_ACCESS

/* CB_PROXY_MAGIC overlaps with CB_ENCRYPT_MAGIC */
#define ENCRYPT_OR_PROXY_MAGIC 0

#ifdef POD_ENDPOINT
# undef ENABLE_HOST_FIREWALL
#endif

#include "lib/utils.h"
#include "lib/common.h"
#include "lib/arp.h"
#include "lib/maps.h"
#include "lib/ipv6.h"
#include "lib/ipv4.h"
#include "lib/icmp6.h"
#include "lib/eth.h"
#include "lib/dbg.h"
#include "lib/proxy.h"
#include "lib/trace.h"
#include "lib/identity.h"
#include "lib/l3.h"
#include "lib/l4.h"
#include "lib/drop.h"
#include "lib/encap.h"
#include "lib/nat.h"
#include "lib/lb.h"
#include "lib/nodeport.h"
#include "lib/eps.h"
#ifdef ENABLE_HOST_FIREWALL
# include "lib/policy.h"
# include "lib/policy_log.h"
#endif

#if defined(ENABLE_IPV4) || defined(ENABLE_IPV6)
static __always_inline int rewrite_dmac_to_host(struct __ctx_buff *ctx,
						__u32 src_identity)
{
	/* When attached to cilium_host, we rewrite the DMAC to the mac of
	 * cilium_host (peer) to ensure the packet is being considered to be
	 * addressed to the host (PACKET_HOST) */
	union macaddr cilium_net_mac = CILIUM_NET_MAC;

	/* Rewrite to destination MAC of cilium_net (remote peer) */
	if (eth_store_daddr(ctx, (__u8 *) &cilium_net_mac.addr, 0) < 0)
		return send_drop_notify_error(ctx, src_identity, DROP_WRITE_ERROR,
					      CTX_ACT_OK, METRIC_INGRESS);

	return CTX_ACT_OK;
}
#endif

#ifdef ENABLE_IPV6
static __always_inline __u32
derive_src_id(const union v6addr *node_ip, struct ipv6hdr *ip6, __u32 *identity)
{
	if (ipv6_match_prefix_64((union v6addr *) &ip6->saddr, node_ip)) {
		/* Read initial 4 bytes of header and then extract flowlabel */
		__u32 *tmp = (__u32 *) ip6;
		*identity = bpf_ntohl(*tmp & IPV6_FLOWLABEL_MASK);

		/* A remote node will map any HOST_ID source to be presented as
		 * REMOTE_NODE_ID, therefore any attempt to signal HOST_ID as
		 * source from a remote node can be droppped. */
		if (*identity == HOST_ID)
			return DROP_INVALID_IDENTITY;
	}
	return 0;
}

# ifdef ENABLE_HOST_FIREWALL
static __always_inline __u32
ipcache_lookup_srcid6(struct __ctx_buff *ctx)
{
	struct remote_endpoint_info *info = NULL;
	void *data, *data_end;
	struct ipv6hdr *ip6;
	__u32 srcid = 0;

	if (!revalidate_data(ctx, &data, &data_end, &ip6))
		return DROP_INVALID;

	info = lookup_ip6_remote_endpoint((union v6addr *) &ip6->saddr);
	if (info != NULL)
		srcid = info->sec_label;
	cilium_dbg(ctx, info ? DBG_IP_ID_MAP_SUCCEED6 : DBG_IP_ID_MAP_FAILED6,
		   ip6->saddr.s6_addr32[3], srcid);

	return srcid;
}
# endif /* ENABLE_HOST_FIREWALL */

static __always_inline __u32
resolve_srcid_ipv6(struct __ctx_buff *ctx, __u32 srcid_from_proxy,
		   const bool from_host)
{
	__u32 src_id = WORLD_ID, srcid_from_ipcache = srcid_from_proxy;
	struct remote_endpoint_info *info = NULL;
	void *data, *data_end;
	struct ipv6hdr *ip6;
	union v6addr *src;
	int ret;

	if (!revalidate_data(ctx, &data, &data_end, &ip6))
		return DROP_INVALID;

	if (!from_host) {
		union v6addr node_ip = {};

		BPF_V6(node_ip, ROUTER_IP);
		ret = derive_src_id(&node_ip, ip6, &src_id);
		if (IS_ERR(ret))
			return ret;
	}

	/* Packets from the proxy will already have a real identity. */
	if (identity_is_reserved(srcid_from_ipcache)) {
		src = (union v6addr *) &ip6->saddr;
		info = lookup_ip6_remote_endpoint(src);
		if (info != NULL && info->sec_label)
			srcid_from_ipcache = info->sec_label;
		cilium_dbg(ctx, info ? DBG_IP_ID_MAP_SUCCEED6 : DBG_IP_ID_MAP_FAILED6,
			   ((__u32 *) src)[3], srcid_from_ipcache);
	}

	if (from_host)
		src_id = srcid_from_ipcache;
#ifdef ENABLE_SECCTX_FROM_IPCACHE
	/* If we could not derive the secctx from the packet itself but
	 * from the ipcache instead, then use the ipcache identity. E.g.
	 * used in ipvlan master device's datapath on ingress.
	 */
	else if (src_id == WORLD_ID && !identity_is_reserved(srcid_from_ipcache))
		src_id = srcid_from_ipcache;
#endif

	return src_id;
}

#ifdef ENABLE_HOST_FIREWALL
static __always_inline int
ipv6_host_policy_egress(struct __ctx_buff *ctx, __u32 srcID)
{
	int ret, verdict, l3_off = ETH_HLEN, l4_off, hdrlen;
	struct ct_state ct_state_new = {}, ct_state = {};
	__u8 policy_match_type = POLICY_MATCH_NONE;
	__u8 audited = 0;
	struct remote_endpoint_info *info;
	struct ipv6_ct_tuple tuple = {};
	__u32 dstID = 0, monitor = 0;
	union v6addr orig_dip;
	void *data, *data_end;
	struct ipv6hdr *ip6;

	/* Only enforce host policies for packets from host IPs. */
	if (srcID != HOST_ID)
		return CTX_ACT_OK;

	if (!revalidate_data(ctx, &data, &data_end, &ip6))
		return DROP_INVALID;

	/* Lookup connection in conntrack map. */
	tuple.nexthdr = ip6->nexthdr;
	ipv6_addr_copy(&tuple.daddr, (union v6addr *) &ip6->daddr);
	ipv6_addr_copy(&orig_dip, (union v6addr *) &ip6->daddr);
	hdrlen = ipv6_hdrlen(ctx, ETH_HLEN, &tuple.nexthdr);
	if (hdrlen < 0)
		return hdrlen;
	l4_off = l3_off + hdrlen;
	ret = ct_lookup6(get_ct_map6(&tuple), &tuple, ctx, l4_off, CT_EGRESS,
			 &ct_state, &monitor);
	if (ret < 0)
		return ret;

	/* Retrieve destination identity. */
	info = lookup_ip6_remote_endpoint(&orig_dip);
	if (info != NULL && info->sec_label)
		dstID = info->sec_label;
	cilium_dbg(ctx, info ? DBG_IP_ID_MAP_SUCCEED6 : DBG_IP_ID_MAP_FAILED6,
		   orig_dip.p4, dstID);

	/* Perform policy lookup. */
	verdict = policy_can_egress6(ctx, &tuple, srcID, dstID,
				     &policy_match_type, &audited);

	/* Reply traffic and related are allowed regardless of policy verdict. */
	if (ret != CT_REPLY && ret != CT_RELATED && verdict < 0) {
		send_policy_verdict_notify(ctx, dstID, tuple.dport,
					   tuple.nexthdr, POLICY_EGRESS, 1,
					   verdict, policy_match_type, audited);
		return verdict;
	}

	switch (ret) {
	case CT_NEW:
		send_policy_verdict_notify(ctx, dstID, tuple.dport,
					   tuple.nexthdr, POLICY_EGRESS, 1,
					   verdict, policy_match_type, audited);
		ct_state_new.src_sec_id = HOST_ID;
		ret = ct_create6(get_ct_map6(&tuple), &CT_MAP_ANY6, &tuple,
				 ctx, CT_EGRESS, &ct_state_new, verdict > 0);
		if (IS_ERR(ret))
			return ret;
		break;

	case CT_ESTABLISHED:
	case CT_RELATED:
	case CT_REPLY:
		break;

	default:
		return DROP_UNKNOWN_CT;
	}

	return CTX_ACT_OK;
}

static __always_inline int
ipv6_host_policy_ingress(struct __ctx_buff *ctx, __u32 *srcID)
{
	struct ct_state ct_state_new = {}, ct_state = {};
	__u8 policy_match_type = POLICY_MATCH_NONE;
	__u8 audited = 0;
	__u32 monitor = 0, dstID = WORLD_ID;
	struct remote_endpoint_info *info;
	int ret, verdict, l4_off, hdrlen;
	struct ipv6_ct_tuple tuple = {};
	union v6addr orig_sip;
	void *data, *data_end;
	struct ipv6hdr *ip6;

	if (!revalidate_data(ctx, &data, &data_end, &ip6))
		return DROP_INVALID;

	/* Retrieve destination identity. */
	ipv6_addr_copy(&tuple.daddr, (union v6addr *) &ip6->daddr);
	info = lookup_ip6_remote_endpoint(&tuple.daddr);
	if (info != NULL && info->sec_label)
		dstID = info->sec_label;
	cilium_dbg(ctx, info ? DBG_IP_ID_MAP_SUCCEED6 : DBG_IP_ID_MAP_FAILED6,
		   tuple.daddr.p4, dstID);

	/* Only enforce host policies for packets to host IPs. */
	if (dstID != HOST_ID)
		return CTX_ACT_OK;

	/* Lookup connection in conntrack map. */
	tuple.nexthdr = ip6->nexthdr;
	ipv6_addr_copy(&tuple.saddr, (union v6addr *) &ip6->saddr);
	ipv6_addr_copy(&orig_sip, (union v6addr *) &ip6->saddr);
	hdrlen = ipv6_hdrlen(ctx, ETH_HLEN, &tuple.nexthdr);
	if (hdrlen < 0)
		return hdrlen;
	l4_off = ETH_HLEN + hdrlen;
	ret = ct_lookup6(get_ct_map6(&tuple), &tuple, ctx, l4_off, CT_INGRESS,
			 &ct_state, &monitor);
	if (ret < 0)
		return ret;

	/* Retrieve source identity. */
	info = lookup_ip6_remote_endpoint(&orig_sip);
	if (info != NULL && info->sec_label)
		*srcID = info->sec_label;
	cilium_dbg(ctx, info ? DBG_IP_ID_MAP_SUCCEED6 : DBG_IP_ID_MAP_FAILED6,
		   orig_sip.p4, *srcID);

	/* Perform policy lookup */
	verdict = policy_can_access_ingress(ctx, *srcID, dstID, tuple.dport,
					    tuple.nexthdr, false,
					    &policy_match_type, &audited);

	/* Reply traffic and related are allowed regardless of policy verdict. */
	if (ret != CT_REPLY && ret != CT_RELATED && verdict < 0) {
		send_policy_verdict_notify(ctx, *srcID, tuple.dport,
					   tuple.nexthdr, POLICY_INGRESS, 1,
					   verdict, policy_match_type, audited);
		return verdict;
	}

	switch (ret) {
	case CT_NEW:
		send_policy_verdict_notify(ctx, *srcID, tuple.dport,
					   tuple.nexthdr, POLICY_INGRESS, 1,
					   verdict, policy_match_type, audited);

		/* Create new entry for connection in conntrack map. */
		ct_state_new.src_sec_id = *srcID;
		ct_state_new.node_port = ct_state.node_port;
		ret = ct_create6(get_ct_map6(&tuple), &CT_MAP_ANY6, &tuple,
				 ctx, CT_INGRESS, &ct_state_new, verdict > 0);
		if (IS_ERR(ret))
			return ret;

	case CT_ESTABLISHED:
	case CT_RELATED:
	case CT_REPLY:
		break;

	default:
		return DROP_UNKNOWN_CT;
	}

	return CTX_ACT_OK;
}
#endif /* ENABLE_HOST_FIREWALL */

static __always_inline int
handle_ipv6(struct __ctx_buff *ctx, __u32 secctx, const bool from_host)
{
	struct remote_endpoint_info *info = NULL;
	void *data, *data_end;
	struct ipv6hdr *ip6;
	union v6addr *dst;
	int ret, l3_off = ETH_HLEN;
	__u32 __maybe_unused remoteID = WORLD_ID;
	bool skip_redirect = false;
	struct endpoint_info *ep;

	if (!revalidate_data(ctx, &data, &data_end, &ip6))
		return DROP_INVALID;

#ifdef ENABLE_NODEPORT
	if (!from_host) {
		if (ctx_get_xfer(ctx) != XFER_PKT_NO_SVC &&
		    !bpf_skip_nodeport(ctx)) {
			ret = nodeport_lb6(ctx, secctx);
			if (ret < 0)
				return ret;
		}
#if defined(ENCAP_IFINDEX) || defined(NO_REDIRECT)
		/* See IPv4 case for NO_REDIRECT comments */
# ifdef ENABLE_HOST_FIREWALL
		skip_redirect = true;
# else
		return CTX_ACT_OK;
# endif
#endif /* ENCAP_IFINDEX || NO_REDIRECT */
		/* Verifier workaround: modified ctx access. */
		if (!revalidate_data(ctx, &data, &data_end, &ip6))
			return DROP_INVALID;
	}
#endif /* ENABLE_NODEPORT */

	if (!skip_redirect) {
		ret = icmp6_host_handle(ctx, ip6->nexthdr);
		if (ret)
			return ret;
	}

	if (from_host && !skip_redirect) {
		/* If we are attached to cilium_host at egress, this will
		 * rewrite the destination mac address to the MAC of cilium_net */
		ret = rewrite_dmac_to_host(ctx, secctx);
		/* DIRECT PACKET READ INVALID */
		if (IS_ERR(ret))
			return ret;

		if (!revalidate_data(ctx, &data, &data_end, &ip6))
			return DROP_INVALID;
	}

#ifdef ENABLE_HOST_FIREWALL
	if (from_host)
		ret = ipv6_host_policy_egress(ctx, secctx);
	else
		ret = ipv6_host_policy_ingress(ctx, &remoteID);
	if (IS_ERR(ret))
		return ret;
	if (skip_redirect)
		return CTX_ACT_OK;
	if (!revalidate_data(ctx, &data, &data_end, &ip6))
		return DROP_INVALID;
#endif /* ENABLE_HOST_FIREWALL */

	/* Lookup IPv4 address in list of local endpoints */
	if ((ep = lookup_ip6_endpoint(ip6)) != NULL) {
		/* Let through packets to the node-ip so they are
		 * processed by the local ip stack */
		if (ep->flags & ENDPOINT_F_HOST)
			return CTX_ACT_OK;

		return ipv6_local_delivery(ctx, l3_off, secctx, ep,
					   METRIC_INGRESS, from_host);
	}

#ifdef ENCAP_IFINDEX
	dst = (union v6addr *) &ip6->daddr;
	info = ipcache_lookup6(&IPCACHE_MAP, dst, V6_CACHE_KEY_LEN);
	if (info != NULL && info->tunnel_endpoint != 0) {
		ret = encap_and_redirect_with_nodeid(ctx, info->tunnel_endpoint,
							 info->key,
							 secctx, TRACE_PAYLOAD_LEN);

		/* If IPSEC is needed recirc through ingress to use xfrm stack
		 * and then result will routed back through bpf_netdev on egress
		 * but with encrypt marks.
		 */
		if (ret == IPSEC_ENDPOINT)
			return CTX_ACT_OK;
		else
			return ret;
	} else {
		struct endpoint_key key = {};

		/* IPv6 lookup key: daddr/96 */
		dst = (union v6addr *) &ip6->daddr;
		key.ip6.p1 = dst->p1;
		key.ip6.p2 = dst->p2;
		key.ip6.p3 = dst->p3;
		key.ip6.p4 = 0;
		key.family = ENDPOINT_KEY_IPV6;

		ret = encap_and_redirect_netdev(ctx, &key, secctx, TRACE_PAYLOAD_LEN);
		if (ret == IPSEC_ENDPOINT)
			return CTX_ACT_OK;
		else if (ret != DROP_NO_TUNNEL_ENDPOINT)
			return ret;
	}
#endif

	dst = (union v6addr *) &ip6->daddr;
	info = ipcache_lookup6(&IPCACHE_MAP, dst, V6_CACHE_KEY_LEN);
	if (from_host && (info == NULL || info->sec_label == WORLD_ID)) {
		/* See IPv4 comment. */
		return DROP_UNROUTABLE;
	}
#ifdef ENABLE_IPSEC
	if (info && info->key && info->tunnel_endpoint) {
		__u8 key = get_min_encrypt_key(info->key);

		set_encrypt_key_meta(ctx, key);
#ifdef IP_POOLS
		set_encrypt_dip(ctx, info->tunnel_endpoint);
#else
		set_identity_meta(ctx, secctx);
#endif
	}
#endif
	return CTX_ACT_OK;
}

static __always_inline int
tail_handle_ipv6(struct __ctx_buff *ctx, const bool from_host)
{
	__u32 proxy_identity = ctx_load_meta(ctx, CB_SRC_IDENTITY);
	int ret;

	ctx_store_meta(ctx, CB_SRC_IDENTITY, 0);

	ret = handle_ipv6(ctx, proxy_identity, from_host);
	if (IS_ERR(ret))
		return send_drop_notify_error(ctx, proxy_identity, ret,
					      CTX_ACT_DROP, METRIC_INGRESS);
	return ret;
}

__section_tail(CILIUM_MAP_CALLS, CILIUM_CALL_IPV6_FROM_HOST)
int tail_handle_ipv6_from_host(struct __ctx_buff *ctx __maybe_unused)
{
	return tail_handle_ipv6(ctx, true);
}

__section_tail(CILIUM_MAP_CALLS, CILIUM_CALL_IPV6_FROM_LXC)
int tail_handle_ipv6_from_netdev(struct __ctx_buff *ctx)
{
	return tail_handle_ipv6(ctx, false);
}

# ifdef ENABLE_HOST_FIREWALL
static __always_inline int
handle_to_netdev_ipv6(struct __ctx_buff *ctx)
{
	void *data, *data_end;
	struct ipv6hdr *ip6;
	__u32 srcID = 0;
	int ret;

	if (!revalidate_data(ctx, &data, &data_end, &ip6))
		return DROP_INVALID;

	ret = icmp6_host_handle(ctx, ip6->nexthdr);
	if (ret)
		return ret;

	/* to-netdev is attached to the egress path of the native
	 * device. */
	srcID = ipcache_lookup_srcid6(ctx);
	return ipv6_host_policy_egress(ctx, srcID);
}
# endif
#endif /* ENABLE_IPV6 */

#ifdef ENABLE_IPV4
# ifdef ENABLE_HOST_FIREWALL
static __always_inline __u32
ipcache_lookup_srcid4(struct __ctx_buff *ctx)
{
	struct remote_endpoint_info *info = NULL;
	void *data, *data_end;
	struct iphdr *ip4;
	__u32 srcid = 0;

	if (!revalidate_data(ctx, &data, &data_end, &ip4))
		return DROP_INVALID;

	info = lookup_ip4_remote_endpoint(ip4->saddr);
	if (info != NULL)
		srcid = info->sec_label;
	cilium_dbg(ctx, info ? DBG_IP_ID_MAP_SUCCEED4 : DBG_IP_ID_MAP_FAILED4,
		   ip4->saddr, srcid);

	return srcid;
}
# endif /* ENABLE_HOST_FIREWALL */

static __always_inline __u32
resolve_srcid_ipv4(struct __ctx_buff *ctx, __u32 srcid_from_proxy,
		   const bool from_host)
{
	__u32 src_id = WORLD_ID, srcid_from_ipcache = srcid_from_proxy;
	struct remote_endpoint_info *info = NULL;
	void *data, *data_end;
	struct iphdr *ip4;

	if (!revalidate_data(ctx, &data, &data_end, &ip4))
		return DROP_INVALID;

	/* Packets from the proxy will already have a real identity. */
	if (identity_is_reserved(srcid_from_ipcache)) {
		info = lookup_ip4_remote_endpoint(ip4->saddr);
		if (info != NULL) {
			__u32 sec_label = info->sec_label;
			if (sec_label) {
				/* When SNAT is enabled on traffic ingressing
				 * into Cilium, all traffic from the world will
				 * have a source IP of the host. It will only
				 * actually be from the host if
				 * "srcid_from_proxy" (passed into this
				 * function) reports the src as the host. So we
				 * can ignore the ipcache if it reports the
				 * source as HOST_ID.
				 */
#ifndef ENABLE_EXTRA_HOST_DEV
				if (from_host && sec_label != HOST_ID)
#endif
					srcid_from_ipcache = sec_label;
			}
		}
		cilium_dbg(ctx, info ? DBG_IP_ID_MAP_SUCCEED4 : DBG_IP_ID_MAP_FAILED4,
			   ip4->saddr, srcid_from_ipcache);
	}

	if (from_host)
		src_id = srcid_from_ipcache;
#ifdef ENABLE_SECCTX_FROM_IPCACHE
	/* If we could not derive the secctx from the packet itself but
	 * from the ipcache instead, then use the ipcache identity. E.g.
	 * used in ipvlan master device's datapath on ingress.
	 */
	else if (!identity_is_reserved(srcid_from_ipcache))
		src_id = srcid_from_ipcache;
#endif

	return src_id;
}

#ifdef ENABLE_HOST_FIREWALL
static __always_inline int
ipv4_host_policy_egress(struct __ctx_buff *ctx, __u32 srcID)
{
	struct ct_state ct_state_new = {}, ct_state = {};
	int ret, verdict, l4_off, l3_off = ETH_HLEN;
	__u8 policy_match_type = POLICY_MATCH_NONE;
	__u8 audited = 0;
	struct remote_endpoint_info *info;
	struct ipv4_ct_tuple tuple = {};
	__u32 dstID = 0, monitor = 0;
	void *data, *data_end;
	struct iphdr *ip4;

	/* Only enforce host policies for packets from host IPs. */
	if (srcID != HOST_ID)
		return CTX_ACT_OK;

	if (!revalidate_data(ctx, &data, &data_end, &ip4))
		return DROP_INVALID;

	/* Lookup connection in conntrack map. */
	tuple.nexthdr = ip4->protocol;
	tuple.daddr = ip4->daddr;
	tuple.saddr = ip4->saddr;
	l4_off = l3_off + ipv4_hdrlen(ip4);
	ret = ct_lookup4(get_ct_map4(&tuple), &tuple, ctx, l4_off, CT_EGRESS,
			 &ct_state, &monitor);
	if (ret < 0)
		return ret;

	/* Retrieve destination identity. */
	info = lookup_ip4_remote_endpoint(ip4->daddr);
	if (info != NULL && info->sec_label)
		dstID = info->sec_label;
	cilium_dbg(ctx, info ? DBG_IP_ID_MAP_SUCCEED4 : DBG_IP_ID_MAP_FAILED4,
		   ip4->daddr, dstID);

	/* Perform policy lookup. */
	verdict = policy_can_egress4(ctx, &tuple, srcID, dstID,
				     &policy_match_type, &audited);

	/* Reply traffic and related are allowed regardless of policy verdict. */
	if (ret != CT_REPLY && ret != CT_RELATED && verdict < 0) {
		send_policy_verdict_notify(ctx, dstID, tuple.dport,
					   tuple.nexthdr, POLICY_EGRESS, 0,
					   verdict, policy_match_type, audited);
		return verdict;
	}

	switch (ret) {
	case CT_NEW:
		send_policy_verdict_notify(ctx, dstID, tuple.dport,
					   tuple.nexthdr, POLICY_EGRESS, 0,
					   verdict, policy_match_type, audited);
		ct_state_new.src_sec_id = HOST_ID;
		ret = ct_create4(get_ct_map4(&tuple), &CT_MAP_ANY4, &tuple,
				 ctx, CT_EGRESS, &ct_state_new, verdict > 0);
		if (IS_ERR(ret))
			return ret;
		break;

	case CT_ESTABLISHED:
	case CT_RELATED:
	case CT_REPLY:
		break;

	default:
		return DROP_UNKNOWN_CT;
	}

	return CTX_ACT_OK;
}

static __always_inline int
ipv4_host_policy_ingress(struct __ctx_buff *ctx, __u32 *srcID)
{
	struct ct_state ct_state_new = {}, ct_state = {};
	int ret, verdict, l4_off, l3_off = ETH_HLEN;
	__u8 policy_match_type = POLICY_MATCH_NONE;
	__u8 audited = 0;
	__u32 monitor = 0, dstID = WORLD_ID;
	struct remote_endpoint_info *info;
	struct ipv4_ct_tuple tuple = {};
	bool is_untracked_fragment = false;
	void *data, *data_end;
	struct iphdr *ip4;

	if (!revalidate_data(ctx, &data, &data_end, &ip4))
		return DROP_INVALID;

	/* Retrieve destination identity. */
	info = lookup_ip4_remote_endpoint(ip4->daddr);
	if (info != NULL && info->sec_label)
		dstID = info->sec_label;
	cilium_dbg(ctx, info ? DBG_IP_ID_MAP_SUCCEED4 : DBG_IP_ID_MAP_FAILED4,
		   ip4->daddr, dstID);

	/* Only enforce host policies for packets to host IPs. */
	if (dstID != HOST_ID)
		return CTX_ACT_OK;

	/* Lookup connection in conntrack map. */
	tuple.nexthdr = ip4->protocol;
	tuple.daddr = ip4->daddr;
	tuple.saddr = ip4->saddr;
	l4_off = l3_off + ipv4_hdrlen(ip4);
#ifndef IPV4_FRAGMENTS
	/* Indicate that this is a datagram fragment for which we cannot
	 * retrieve L4 ports. Do not set flag if we support fragmentation.
	 */
	is_untracked_fragment = ipv4_is_fragment(ip4);
#endif
	ret = ct_lookup4(get_ct_map4(&tuple), &tuple, ctx, l4_off, CT_INGRESS,
			 &ct_state, &monitor);
	if (ret < 0)
		return ret;

	/* Retrieve source identity. */
	info = lookup_ip4_remote_endpoint(ip4->saddr);
	if (info != NULL && info->sec_label)
		*srcID = info->sec_label;
	cilium_dbg(ctx, info ? DBG_IP_ID_MAP_SUCCEED4 : DBG_IP_ID_MAP_FAILED4,
		   ip4->saddr, *srcID);

	/* Perform policy lookup */
	verdict = policy_can_access_ingress(ctx, *srcID, dstID, tuple.dport,
					    tuple.nexthdr,
					    is_untracked_fragment,
					    &policy_match_type, &audited);

	/* Reply traffic and related are allowed regardless of policy verdict. */
	if (ret != CT_REPLY && ret != CT_RELATED && verdict < 0) {
		send_policy_verdict_notify(ctx, *srcID, tuple.dport,
					   tuple.nexthdr, POLICY_INGRESS, 0,
					   verdict, policy_match_type, audited);
		return verdict;
	}

	switch (ret) {
	case CT_NEW:
		send_policy_verdict_notify(ctx, *srcID, tuple.dport,
					   tuple.nexthdr, POLICY_INGRESS, 0,
					   verdict, policy_match_type, audited);

		/* Create new entry for connection in conntrack map. */
		ct_state_new.src_sec_id = *srcID;
		ct_state_new.node_port = ct_state.node_port;
		ret = ct_create4(get_ct_map4(&tuple), &CT_MAP_ANY4, &tuple,
				 ctx, CT_INGRESS, &ct_state_new, verdict > 0);
		if (IS_ERR(ret))
			return ret;

	case CT_ESTABLISHED:
	case CT_RELATED:
	case CT_REPLY:
		break;

	default:
		return DROP_UNKNOWN_CT;
	}

	return CTX_ACT_OK;
}
#endif /* ENABLE_HOST_FIREWALL */

static __always_inline int
handle_ipv4(struct __ctx_buff *ctx, __u32 secctx, const bool from_host)
{
	struct remote_endpoint_info *info = NULL;
	__u32 __maybe_unused remoteID = 0;
	struct ipv4_ct_tuple tuple = {};
	bool skip_redirect = false;
	struct endpoint_info *ep;
	void *data, *data_end;
	struct iphdr *ip4;
	int ret;

	if (!revalidate_data(ctx, &data, &data_end, &ip4))
		return DROP_INVALID;

#ifdef ENABLE_NODEPORT
	if (!from_host) {
		if (ctx_get_xfer(ctx) != XFER_PKT_NO_SVC &&
		    !bpf_skip_nodeport(ctx)) {
			ret = nodeport_lb4(ctx, secctx);
			if (ret < 0)
				return ret;
		}
#if defined(ENCAP_IFINDEX) || defined(NO_REDIRECT)
		/* We cannot redirect a packet to a local endpoint in the direct
		 * routing mode, as the redirect bypasses nf_conntrack table.
		 * This makes a second reply from the endpoint to be MASQUERADEd
		 * or to be DROPed by k8s's "--ctstate INVALID -j DROP"
		 * depending via which interface it was inputed. */
# ifdef ENABLE_HOST_FIREWALL
		skip_redirect = true;
# else
		return CTX_ACT_OK;
#endif /* ENABLE_HOST_FIREWALL */
#endif /* ENCAP_IFINDEX || NO_REDIRECT */
		/* Verifier workaround: modified ctx access. */
		if (!revalidate_data(ctx, &data, &data_end, &ip4))
			return DROP_INVALID;
	}
#endif /* ENABLE_NODEPORT */

	tuple.nexthdr = ip4->protocol;

	if (from_host && !skip_redirect) {
		/* If we are attached to cilium_host at egress, this will
		 * rewrite the destination mac address to the MAC of cilium_net */
		ret = rewrite_dmac_to_host(ctx, secctx);
		/* DIRECT PACKET READ INVALID */
		if (IS_ERR(ret))
			return ret;

		if (!revalidate_data(ctx, &data, &data_end, &ip4))
			return DROP_INVALID;
	}

#ifdef ENABLE_HOST_FIREWALL
	if (from_host)
		/* We're on the egress path of cilium_host. */
		ret = ipv4_host_policy_egress(ctx, secctx);
	else
		/* We're on the ingress path of the native device. */
		ret = ipv4_host_policy_ingress(ctx, &remoteID);
	if (IS_ERR(ret))
		return ret;
	if (skip_redirect)
		return CTX_ACT_OK;

	if (!revalidate_data(ctx, &data, &data_end, &ip4))
		return DROP_INVALID;
#endif /* ENABLE_HOST_FIREWALL */

	/* Lookup IPv4 address in list of local endpoints and host IPs */
	if ((ep = lookup_ip4_endpoint(ip4)) != NULL) {
		/* Let through packets to the node-ip so they are
		 * processed by the local ip stack */
		if (ep->flags & ENDPOINT_F_HOST)
#ifdef HOST_REDIRECT_TO_INGRESS
			/* This is required for L7 proxy to send packets to the host. */
			return redirect(HOST_IFINDEX, BPF_F_INGRESS);
#else
			return CTX_ACT_OK;
#endif

		return ipv4_local_delivery(ctx, ETH_HLEN, secctx, ip4, ep,
					   METRIC_INGRESS, from_host);
	}

#ifdef ENCAP_IFINDEX
	info = ipcache_lookup4(&IPCACHE_MAP, ip4->daddr, V4_CACHE_KEY_LEN);
	if (info != NULL && info->tunnel_endpoint != 0) {
		ret = encap_and_redirect_with_nodeid(ctx, info->tunnel_endpoint,
						     info->key, secctx,
						     TRACE_PAYLOAD_LEN);

		if (ret == IPSEC_ENDPOINT)
			return CTX_ACT_OK;
		else
			return ret;
	} else {
		/* IPv4 lookup key: daddr & IPV4_MASK */
		struct endpoint_key key = {};

		key.ip4 = ip4->daddr & IPV4_MASK;
		key.family = ENDPOINT_KEY_IPV4;

		cilium_dbg(ctx, DBG_NETDEV_ENCAP4, key.ip4, secctx);
		ret = encap_and_redirect_netdev(ctx, &key, secctx, TRACE_PAYLOAD_LEN);
		if (ret == IPSEC_ENDPOINT)
			return CTX_ACT_OK;
		else if (ret != DROP_NO_TUNNEL_ENDPOINT)
			return ret;
	}
#endif

#ifdef HOST_REDIRECT_TO_INGRESS
    return redirect(HOST_IFINDEX, BPF_F_INGRESS);
#else

	info = ipcache_lookup4(&IPCACHE_MAP, ip4->daddr, V4_CACHE_KEY_LEN);
	if (from_host && (info == NULL || info->sec_label == WORLD_ID)) {
		/* We have received a packet for which no ipcache entry exists,
		 * we do not know what to do with this packet, drop it.
		 *
		 * The info == NULL test is soley to satisfy verifier requirements
		 * as in Cilium case we'll always hit the 0.0.0.0/32 catch-all
		 * entry. Therefore we need to test for WORLD_ID. It is clearly
		 * wrong to route a ctx to cilium_host for which we don't know
		 * anything about it as otherwise we'll run into a routing loop.
		 */
		return DROP_UNROUTABLE;
	}
#ifdef ENABLE_IPSEC
	if (info && info->key && info->tunnel_endpoint) {
		__u8 key = get_min_encrypt_key(info->key);

		set_encrypt_key_meta(ctx, key);
#ifdef IP_POOLS
		set_encrypt_dip(ctx, info->tunnel_endpoint);
#else
		set_identity_meta(ctx, secctx);
#endif
	}
#endif
	return CTX_ACT_OK;
#endif
}

static __always_inline int
tail_handle_ipv4(struct __ctx_buff *ctx, const bool from_host)
{
	__u32 proxy_identity = ctx_load_meta(ctx, CB_SRC_IDENTITY);
	int ret;

	ctx_store_meta(ctx, CB_SRC_IDENTITY, 0);

	ret = handle_ipv4(ctx, proxy_identity, from_host);
	if (IS_ERR(ret))
		return send_drop_notify_error(ctx, proxy_identity,
					      ret, CTX_ACT_DROP, METRIC_INGRESS);
	return ret;
}

__section_tail(CILIUM_MAP_CALLS, CILIUM_CALL_IPV4_FROM_HOST)
int tail_handle_ipv4_from_host(struct __ctx_buff *ctx)
{
	return tail_handle_ipv4(ctx, true);
}

__section_tail(CILIUM_MAP_CALLS, CILIUM_CALL_IPV4_FROM_LXC)
int tail_handle_ipv4_from_netdev(struct __ctx_buff *ctx)
{
	return tail_handle_ipv4(ctx, false);
}
#endif /* ENABLE_IPV4 */

#ifdef ENABLE_IPSEC
#ifndef ENCAP_IFINDEX
static __always_inline int
do_netdev_encrypt_pools(struct __ctx_buff *ctx __maybe_unused)
{
	int ret = 0;
#ifdef IP_POOLS
	__u32 tunnel_endpoint = 0;
	void *data, *data_end;
	__u32 tunnel_source = IPV4_ENCRYPT_IFACE;
	struct iphdr *iphdr;
	__be32 sum;

	tunnel_endpoint = ctx_load_meta(ctx, 4);
	ctx->mark = 0;

	if (!revalidate_data(ctx, &data, &data_end, &iphdr)) {
		ret = DROP_INVALID;
		goto drop_err;
	}

	/* When IP_POOLS is enabled ip addresses are not
	 * assigned on a per node basis so lacking node
	 * affinity we can not use IP address to assign the
	 * destination IP. Instead rewrite it here from cb[].
	 */
	sum = csum_diff(&iphdr->daddr, 4, &tunnel_endpoint, 4, 0);
	if (ctx_store_bytes(ctx, ETH_HLEN + offsetof(struct iphdr, daddr),
	    &tunnel_endpoint, 4, 0) < 0) {
		ret = DROP_WRITE_ERROR;
		goto drop_err;
	}
	if (l3_csum_replace(ctx, ETH_HLEN + offsetof(struct iphdr, check),
	    0, sum, 0) < 0) {
		ret = DROP_CSUM_L3;
		goto drop_err;
	}

	if (!revalidate_data(ctx, &data, &data_end, &iphdr)) {
		ret = DROP_INVALID;
		goto drop_err;
	}

	sum = csum_diff(&iphdr->saddr, 4, &tunnel_source, 4, 0);
	if (ctx_store_bytes(ctx, ETH_HLEN + offsetof(struct iphdr, saddr),
	    &tunnel_source, 4, 0) < 0) {
		ret = DROP_WRITE_ERROR;
		goto drop_err;
	}
	if (l3_csum_replace(ctx, ETH_HLEN + offsetof(struct iphdr, check),
	    0, sum, 0) < 0) {
		ret = DROP_CSUM_L3;
		goto drop_err;
	}
drop_err:
#endif /* IP_POOLS */
	return ret;
}

static __always_inline int
do_netdev_encrypt_fib(struct __ctx_buff *ctx __maybe_unused,
		      __u16 proto __maybe_unused,
		      int *encrypt_iface __maybe_unused)
{
	int ret = 0;
#ifdef BPF_HAVE_FIB_LOOKUP
	struct bpf_fib_lookup fib_params = {};
	void *data, *data_end;
	int err;

	if (proto ==  bpf_htons(ETH_P_IP)) {
		struct iphdr *ip4;

		if (!revalidate_data(ctx, &data, &data_end, &ip4)) {
			ret = DROP_INVALID;
			goto drop_err_fib;
		}

		fib_params.family = AF_INET;
		fib_params.ipv4_src = ip4->saddr;
		fib_params.ipv4_dst = ip4->daddr;
	} else {
		struct ipv6hdr *ip6;

		if (!revalidate_data(ctx, &data, &data_end, &ip6)) {
			ret = DROP_INVALID;
			goto drop_err_fib;
		}

		fib_params.family = AF_INET6;
		ipv6_addr_copy((union v6addr *) &fib_params.ipv6_src, (union v6addr *) &ip6->saddr);
		ipv6_addr_copy((union v6addr *) &fib_params.ipv6_dst, (union v6addr *) &ip6->daddr);
	}

	fib_params.ifindex = *encrypt_iface;

	err = fib_lookup(ctx, &fib_params, sizeof(fib_params),
		    BPF_FIB_LOOKUP_DIRECT | BPF_FIB_LOOKUP_OUTPUT);
	if (err != 0) {
		ret = DROP_NO_FIB;
		goto drop_err_fib;
	}
	if (eth_store_daddr(ctx, fib_params.dmac, 0) < 0) {
		ret = DROP_WRITE_ERROR;
		goto drop_err_fib;
	}
	if (eth_store_saddr(ctx, fib_params.smac, 0) < 0) {
		ret = DROP_WRITE_ERROR;
		goto drop_err_fib;
	}
	*encrypt_iface = fib_params.ifindex;
drop_err_fib:
#endif /* BPF_HAVE_FIB_LOOKUP */
	return ret;
}

static __always_inline int do_netdev_encrypt(struct __ctx_buff *ctx, __u16 proto)
{
	int encrypt_iface = 0;
	int ret = 0;
#if defined(ENCRYPT_IFACE)
	encrypt_iface = ENCRYPT_IFACE;
#endif
	ret = do_netdev_encrypt_pools(ctx);
	if (ret)
		return send_drop_notify_error(ctx, 0, ret, CTX_ACT_DROP, METRIC_INGRESS);

	ret = do_netdev_encrypt_fib(ctx, proto, &encrypt_iface);
	if (ret)
		return send_drop_notify_error(ctx, 0, ret, CTX_ACT_DROP, METRIC_INGRESS);

	bpf_clear_meta(ctx);
	if (encrypt_iface)
		return redirect(encrypt_iface, 0);
	return CTX_ACT_OK;
}

#else /* ENCAP_IFINDEX */
static __always_inline int do_netdev_encrypt_encap(struct __ctx_buff *ctx)
{
	__u32 seclabel, tunnel_endpoint = 0;

	seclabel = get_identity(ctx);
	tunnel_endpoint = ctx_load_meta(ctx, 4);
	ctx->mark = 0;

	bpf_clear_meta(ctx);
	return __encap_and_redirect_with_nodeid(ctx, tunnel_endpoint, seclabel, TRACE_PAYLOAD_LEN);
}

static __always_inline int do_netdev_encrypt(struct __ctx_buff *ctx, __u16 proto __maybe_unused)
{
	return do_netdev_encrypt_encap(ctx);
}
#endif /* ENCAP_IFINDEX */
#endif /* ENABLE_IPSEC */

static __always_inline int
do_netdev(struct __ctx_buff *ctx, __u16 proto, const bool from_host)
{
	__u32 __maybe_unused identity = 0;
	int ret;

#ifdef ENABLE_IPSEC
	if (1) {
		__u32 magic = ctx->mark & MARK_MAGIC_HOST_MASK;

		if (magic == MARK_MAGIC_ENCRYPT)
			return do_netdev_encrypt(ctx, proto);
	}
#endif
	bpf_clear_meta(ctx);

	if (from_host) {
#ifdef HOST_REDIRECT_TO_INGRESS
		if (proto == bpf_htons(ETH_P_ARP)) {
			union macaddr mac = HOST_IFINDEX_MAC;
			union macaddr smac;
			__be32 sip;
			__be32 tip;

			/* Pass any unknown ARP requests to the Linux stack */
			if (!arp_validate(ctx, &mac, &smac, &sip, &tip))
				return CTX_ACT_OK;

			return arp_respond(ctx, &mac, tip, &smac, sip,
					   BPF_F_INGRESS);
		}
#endif

		int trace = TRACE_FROM_HOST;
		bool from_proxy;

		from_proxy = inherit_identity_from_host(ctx, &identity);
		if (from_proxy)
			trace = TRACE_FROM_PROXY;
		send_trace_notify(ctx, trace, identity, 0, 0,
				  ctx->ingress_ifindex, 0, TRACE_PAYLOAD_LEN);
	} else {
		bpf_skip_nodeport_clear(ctx);
		send_trace_notify(ctx, TRACE_FROM_STACK, 0, 0, 0,
				  ctx->ingress_ifindex, 0, TRACE_PAYLOAD_LEN);
	}

	switch (proto) {
# if defined ENABLE_ARP_PASSTHROUGH || defined ENABLE_ARP_RESPONDER
	case bpf_htons(ETH_P_ARP):
		ret = CTX_ACT_OK;
		break;
# endif
#ifdef ENABLE_IPV6
	case bpf_htons(ETH_P_IPV6):
		identity = resolve_srcid_ipv6(ctx, identity, from_host);
		ctx_store_meta(ctx, CB_SRC_IDENTITY, identity);
		if (from_host)
			ep_tail_call(ctx, CILIUM_CALL_IPV6_FROM_HOST);
		else
			ep_tail_call(ctx, CILIUM_CALL_IPV6_FROM_LXC);
		/* See comment below for IPv4. */
		return send_drop_notify_error(ctx, identity, DROP_MISSED_TAIL_CALL,
					      CTX_ACT_OK, METRIC_INGRESS);
#endif
#ifdef ENABLE_IPV4
	case bpf_htons(ETH_P_IP):
		identity = resolve_srcid_ipv4(ctx, identity, from_host);
		ctx_store_meta(ctx, CB_SRC_IDENTITY, identity);
		if (from_host)
			ep_tail_call(ctx, CILIUM_CALL_IPV4_FROM_HOST);
		else
			ep_tail_call(ctx, CILIUM_CALL_IPV4_FROM_LXC);
		/* We are not returning an error here to always allow traffic to
		 * the stack in case maps have become unavailable.
		 *
		 * Note: Since drop notification requires a tail call as well,
		 * this notification is unlikely to succeed. */
		return send_drop_notify_error(ctx, identity, DROP_MISSED_TAIL_CALL,
		                              CTX_ACT_OK, METRIC_INGRESS);
#endif
	default:
#ifdef ENABLE_HOST_FIREWALL
		ret = send_drop_notify_error(ctx, identity, DROP_UNKNOWN_L3,
					     CTX_ACT_DROP, METRIC_INGRESS);
#else
		/* Pass unknown traffic to the stack */
		ret = CTX_ACT_OK;
#endif /* ENABLE_HOST_FIREWALL */
	}

	return ret;
}

/**
 * handle_netdev
 * @ctx		The packet context for this program
 * @from_host	True if the packet is from the local host
 *
 * Handle netdev traffic coming towards the Cilium-managed network.
 */
static __always_inline int
handle_netdev(struct __ctx_buff *ctx, const bool from_host)
{
	__u16 proto;

	if (!validate_ethertype(ctx, &proto)) {
#ifdef ENABLE_HOST_FIREWALL
		int ret = DROP_UNSUPPORTED_L2;

		return send_drop_notify(ctx, SECLABEL, WORLD_ID, 0, ret,
					CTX_ACT_DROP, METRIC_EGRESS);
#else
		send_trace_notify(ctx, TRACE_TO_STACK, HOST_ID, 0, 0, 0,
				  REASON_FORWARDED, 0);
		/* Pass unknown traffic to the stack */
		return CTX_ACT_OK;
#endif /* ENABLE_HOST_FIREWALL */
	}

	return do_netdev(ctx, proto, from_host);
}

__section("from-netdev")
int from_netdev(struct __ctx_buff *ctx)
{
	return handle_netdev(ctx, false);
}

__section("from-host")
int from_host(struct __ctx_buff *ctx)
{
	return handle_netdev(ctx, true);
}

__section("to-netdev")
int to_netdev(struct __ctx_buff *ctx __maybe_unused)
{
	__u32 __maybe_unused remoteID = WORLD_ID, srcID = 0;
	__u16 __maybe_unused proto = 0;
	int ret = CTX_ACT_OK;

#ifdef ENABLE_HOST_FIREWALL
	if (!proto && !validate_ethertype(ctx, &proto)) {
		ret = DROP_UNSUPPORTED_L2;
		goto out;
	}

	policy_clear_mark(ctx);

	switch (proto) {
# if defined ENABLE_ARP_PASSTHROUGH || defined ENABLE_ARP_RESPONDER
	case bpf_htons(ETH_P_ARP):
		ret = CTX_ACT_OK;
		break;
# endif
# ifdef ENABLE_IPV6
	case bpf_htons(ETH_P_IPV6):
		ret = handle_to_netdev_ipv6(ctx);
		break;
# endif
# ifdef ENABLE_IPV4
	case bpf_htons(ETH_P_IP):
		/* to-netdev is attached to the egress path of the native
		 * device. */
		srcID = ipcache_lookup_srcid4(ctx);
		ret = ipv4_host_policy_egress(ctx, srcID);
		break;
# endif
	default:
		ret = DROP_UNKNOWN_L3;
		break;
	}

out:
	if (IS_ERR(ret))
		return send_drop_notify_error(ctx, srcID, ret, CTX_ACT_DROP,
					      METRIC_EGRESS);
#else
	ret = CTX_ACT_OK;
#endif /* ENABLE_HOST_FIREWALL */

#if defined(ENABLE_NODEPORT) && \
	(!defined(ENABLE_DSR) || \
	 (defined(ENABLE_DSR) && defined(ENABLE_DSR_HYBRID)))
	if ((ctx->mark & MARK_MAGIC_SNAT_DONE) != MARK_MAGIC_SNAT_DONE) {
		ret = nodeport_nat_fwd(ctx, false);
		if (IS_ERR(ret))
			return send_drop_notify_error(ctx, 0, ret,
						      CTX_ACT_DROP,
						      METRIC_EGRESS);
	}
#endif

	return ret;
}

__section("to-host")
int to_host(struct __ctx_buff *ctx)
{
	__u32 magic = ctx_load_meta(ctx, ENCRYPT_OR_PROXY_MAGIC);
	__u16 __maybe_unused proto = 0;
	int ret = CTX_ACT_OK;
	bool traced = false;
	__u32 srcID = 0;

	if ((magic & MARK_MAGIC_HOST_MASK) == MARK_MAGIC_ENCRYPT) {
		ctx->mark = magic; // CB_ENCRYPT_MAGIC
		srcID = ctx_load_meta(ctx, CB_ENCRYPT_IDENTITY);
		set_identity_mark(ctx, srcID);
	} else if ((magic & 0xFFFF) == MARK_MAGIC_TO_PROXY) {
		/* Upper 16 bits may carry proxy port number */
		__be16 port = magic >> 16;

		ctx_store_meta(ctx, 0, CB_PROXY_MAGIC);
		ctx_redirect_to_proxy_first(ctx, port);
		/* We already traced this in the previous prog with
		 * more background context, skip trace here. */
		traced = true;
	}

	if (!traced)
		send_trace_notify(ctx, TRACE_TO_STACK, srcID, 0, 0,
				  CILIUM_IFINDEX, ret, 0);

#ifdef ENABLE_HOST_FIREWALL
	if (!validate_ethertype(ctx, &proto)) {
		ret = DROP_UNSUPPORTED_L2;
		goto out;
	}

	policy_clear_mark(ctx);

	switch (proto) {
# if defined ENABLE_ARP_PASSTHROUGH || defined ENABLE_ARP_RESPONDER
	case bpf_htons(ETH_P_ARP):
		ret = CTX_ACT_OK;
		break;
# endif
# ifdef ENABLE_IPV6
	case bpf_htons(ETH_P_IPV6):
		ret = ipv6_host_policy_ingress(ctx, &srcID);
		break;
# endif
# ifdef ENABLE_IPV4
	case bpf_htons(ETH_P_IP):
		ret = ipv4_host_policy_ingress(ctx, &srcID);
		break;
# endif
	default:
		ret = DROP_UNKNOWN_L3;
		break;
	}

out:
	if (IS_ERR(ret))
		return send_drop_notify_error(ctx, srcID, ret, CTX_ACT_DROP,
					      METRIC_INGRESS);
#else
	ret = CTX_ACT_OK;
#endif /* ENABLE_HOST_FIREWALL */

	return ret;
}

BPF_LICENSE("GPL");
