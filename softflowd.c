/*
 * Copyright 2002 Damien Miller <djm@mindrot.org> All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * This is software implementation of Cisco's NetFlow(tm) traffic       
 * reporting system. It operates by listening (via libpcap) on a        
 * promiscuous interface and tracking traffic flows.                    
 *
 * Traffic flows are recorded by source/destination/protocol
 * IP address or, in the case of TCP and UDP, by
 * src_addr:src_port/dest_addr:dest_port/protocol
 *
 * Flows expire automatically after a period of inactivity (default: 1
 * hour) They may also be evicted (in order of age) in situations where
 * there are more flows than slots available.
 *
 *
 * As this implementation watches traffic promiscuously, it is likely to
 * place significant load on hosts or gateways on which it is installed.
 */

#include "common.h"
#include "sys-tree.h"
#include "softflowd.h"
#include "treetype.h"
#include "freelist.h"
#include "log.h"
#include <pcap.h>

/* Global variables */
static int verbose_flag = 0;		/* Debugging flag */
static u_int16_t if_index = 0;		/* "manual" interface index */

/* Signal handler flags */
static volatile sig_atomic_t graceful_shutdown_request = 0;	

/* Context for libpcap callback functions */
struct CB_CTXT {
	struct FLOWTRACK *ft;
	int linktype;
	int fatal;
	int want_v6;
};

/* Describes a datalink header and how to extract v4/v6 frames from it */
struct DATALINK {
	int dlt;		/* BPF datalink type */
	int skiplen;		/* Number of bytes to skip datalink header */
	int ft_off;		/* Datalink frametype offset */
	int ft_len;		/* Datalink frametype length */
	int ft_is_be;		/* Set if frametype is big-endian */
	u_int32_t ft_mask;	/* Mask applied to frametype */
	u_int32_t ft_v4;	/* IPv4 frametype */
	u_int32_t ft_v6;	/* IPv6 frametype */
};

/* Datalink types that we know about */
static const struct DATALINK lt[] = {
	{ DLT_EN10MB,	14, 12,  2,  1, 0xffffffff,  0x0800,   0x86dd },
	{ DLT_PPP,	 5,  3,  2,  1, 0xffffffff,  0x0021,   0x0057 },
#ifdef DLT_LINUX_SLL
	{ DLT_LINUX_SLL,16, 14,  2,  1, 0xffffffff,  0x0800,   0x86dd },
#endif
	{ DLT_RAW,	 0,  0,  1,  1, 0x000000f0,  0x0040,   0x0060 },
	{ DLT_NULL,	 4,  0,  4,  0, 0xffffffff, AF_INET, AF_INET6 },
#ifdef DLT_LOOP
	{ DLT_LOOP,	 4,  0,  4,  1, 0xffffffff, AF_INET, AF_INET6 },
#endif
	{ -1,		-1, -1, -1, -1, 0x00000000,  0xffff,   0xffff },
};

/* Signal handlers */
static void sighand_graceful_shutdown(int signum)
{
	graceful_shutdown_request = signum;
}

static void sighand_other(int signum)
{
	/* XXX: this may not be completely safe */
	logit(LOG_WARNING, "Exiting immediately on unexpected signal %d",
	    signum);
	_exit(0);
}

/*
 * This is the flow comparison function.
 */
static int
flow_compare(struct FLOW *a, struct FLOW *b)
{
	/* Be careful to avoid signed vs unsigned issues here */
	int r;

	if (a->af != b->af)
		return (a->af > b->af ? 1 : -1);

	if ((r = memcmp(&a->addr[0], &b->addr[0], sizeof(a->addr[0]))) != 0)
		return (r > 0 ? 1 : -1);

	if ((r = memcmp(&a->addr[1], &b->addr[1], sizeof(a->addr[1]))) != 0)
		return (r > 0 ? 1 : -1);

#ifdef notyet
	if (a->ip6_flowlabel[0] != 0 && b->ip6_flowlabel[0] != 0 && 
	    a->ip6_flowlabel[0] != b->ip6_flowlabel[0])
		return (a->ip6_flowlabel[0] > b->ip6_flowlabel[0] ? 1 : -1);

	if (a->ip6_flowlabel[1] != 0 && b->ip6_flowlabel[1] != 0 && 
	    a->ip6_flowlabel[1] != b->ip6_flowlabel[1])
		return (a->ip6_flowlabel[1] > b->ip6_flowlabel[1] ? 1 : -1);
#endif

	if (a->protocol != b->protocol)
		return (a->protocol > b->protocol ? 1 : -1);

	if (a->port[0] != b->port[0])
		return (ntohs(a->port[0]) > ntohs(b->port[0]) ? 1 : -1);

	if (a->port[1] != b->port[1])
		return (ntohs(a->port[1]) > ntohs(b->port[1]) ? 1 : -1);

	return (0);
}

/* Generate functions for flow tree */
FLOW_PROTOTYPE(FLOWS, FLOW, trp, flow_compare);
FLOW_GENERATE(FLOWS, FLOW, trp, flow_compare);

/*
 * This is the expiry comparison function.
 */
static int
expiry_compare(struct EXPIRY *a, struct EXPIRY *b)
{
	if (a->expires_at != b->expires_at)
		return (a->expires_at > b->expires_at ? 1 : -1);

	/* Make expiry entries unique by comparing flow sequence */
	if (a->flow->flow_seq != b->flow->flow_seq)
		return (a->flow->flow_seq > b->flow->flow_seq ? 1 : -1);

	return (0);
}

/* Generate functions for flow tree */
EXPIRY_PROTOTYPE(EXPIRIES, EXPIRY, trp, expiry_compare);
EXPIRY_GENERATE(EXPIRIES, EXPIRY, trp, expiry_compare);

static struct FLOW *
flow_get(struct FLOWTRACK *ft)
{
	return freelist_get(&ft->flow_freelist);
}

static void
flow_put(struct FLOWTRACK *ft, struct FLOW *flow)
{
	return freelist_put(&ft->flow_freelist, flow);
}

static struct EXPIRY *
expiry_get(struct FLOWTRACK *ft)
{
	return freelist_get(&ft->expiry_freelist);
}

static void
expiry_put(struct FLOWTRACK *ft, struct EXPIRY *expiry)
{
	return freelist_put(&ft->expiry_freelist, expiry);
}

#if 0
/* Dump a packet */
static void
dump_packet(const u_int8_t *p, int len)
{
	char buf[1024], tmp[3];
	int i;

	for (*buf = '\0', i = 0; i < len; i++) {
		snprintf(tmp, sizeof(tmp), "%02x%s", p[i], i % 2 ? " " : "");
		if (strlcat(buf, tmp, sizeof(buf) - 4) >= sizeof(buf) - 4) {
			strlcat(buf, "...", sizeof(buf));
			break;
		}
	}
	logit(LOG_INFO, "packet len %d: %s", len, buf);
}
#endif

/* Format a time in an ISOish format */
static const char *
format_time(time_t t)
{
	struct tm *tm;
	static char buf[32];

	tm = gmtime(&t);
	strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", tm);

	return (buf);

}

static const char *
tcp_flags_to_str(uint8_t tcp_flags)
{
	static char buf[64];
	memset(buf, 0, sizeof(buf));
	
	if(tcp_flags & TH_SYN)
	{
		strcat(buf,"SYN");
	}
	if(tcp_flags & TH_PUSH)
	{
		strcat(buf," PUSH");
	}
	if(tcp_flags & TH_URG)
	{
		strcat(buf," URG");
	}
	if(tcp_flags & TH_ACK)
	{
		strcat(buf," ACK");
	}
	if(tcp_flags & TH_FIN)
	{
		strcat(buf," FIN");
	}
	if(tcp_flags & TH_RST)
	{
		strcat(buf," RST");
	}
	return buf;
}

static const char *
protocol_to_str(uint8_t protocol)
{
	static char protobuf[64];
	memset(protobuf, 0, sizeof(protobuf));
	
	if(protocol == IPPROTO_IP)
	{
		strcat(protobuf, "IP");
	}
	else if(protocol == IPPROTO_TCP)
	{
		strcat(protobuf, "TCP");
	}
	else if(protocol == IPPROTO_ICMP)
	{
		strcat(protobuf, "ICMP");
	}
	else if(protocol == IPPROTO_UDP)
	{
		strcat(protobuf, "UDP");
	}
	else if(protocol == IPPROTO_IGMP)
	{
		strcat(protobuf, "IGMP");
	}
	else if(protocol == IPPROTO_IPV6)
	{
		strcat(protobuf, "IPV6");
	}
	else if(protocol == IPPROTO_GRE)
	{
		strcat(protobuf, "GRE");
	}
	else if(protocol == IPPROTO_ICMPV6)
	{
		strcat(protobuf, "ICMPV6");
	}
	else 
	{
		strcat(protobuf, "OTHERS");
	}
	return protobuf;
}

/* Format a flow in a verbose and ugly way */
static const char *
format_flow(struct FLOW *flow)
{
	char addr1[64], addr2[64], stime[32], ftime[32];
	static char buf[1024];

	inet_ntop(flow->af, &flow->addr[0], addr1, sizeof(addr1));
	inet_ntop(flow->af, &flow->addr[1], addr2, sizeof(addr2));

	snprintf(stime, sizeof(ftime), "%s", 
	    format_time(flow->flow_start.tv_sec));
	snprintf(ftime, sizeof(ftime), "%s", 
	    format_time(flow->flow_last.tv_sec));

	snprintf(buf, sizeof(buf),  "seq:%"PRIu64" [%s]:%hu <> [%s]:%hu proto:%u,%s "
	    "octets>:%u packets>:%u octets<:%u packets<:%u "
	    "start:%s.%03ld finish:%s.%03ld tcp>:%02x %s tcp<:%02x %s "
	    "flowlabel>:%08x flowlabel<:%08x ",
	    flow->flow_seq,
	    addr1, ntohs(flow->port[0]), addr2, ntohs(flow->port[1]),
	    (int)flow->protocol, protocol_to_str(flow->protocol),
	    flow->octets[0], flow->packets[0], 
	    flow->octets[1], flow->packets[1], 
	    stime, (flow->flow_start.tv_usec + 500) / 1000, 
	    ftime, (flow->flow_last.tv_usec + 500) / 1000,
	    flow->tcp_flags[0], tcp_flags_to_str(flow->tcp_flags[0]), flow->tcp_flags[1], tcp_flags_to_str(flow->tcp_flags[1]),
	    flow->ip6_flowlabel[0], flow->ip6_flowlabel[1]);

	return (buf);
}

/* Format a flow in a brief way */
static const char *
format_flow_brief(struct FLOW *flow)
{
	char addr1[64], addr2[64];
	static char buf[1024];

	inet_ntop(flow->af, &flow->addr[0], addr1, sizeof(addr1));
	inet_ntop(flow->af, &flow->addr[1], addr2, sizeof(addr2));

	snprintf(buf, sizeof(buf), 
	    "seq:%"PRIu64" [%s]:%hu <> [%s]:%hu proto:%u, %s",
	    flow->flow_seq,
	    addr1, ntohs(flow->port[0]), addr2, ntohs(flow->port[1]),
	    (int)flow->protocol, protocol_to_str(flow->protocol));

	return (buf);
}

/* Fill in transport-layer (tcp/udp) portions of flow record */
static int
transport_to_flowrec(struct FLOW *flow, const u_int8_t *pkt, 
    const size_t caplen, int isfrag, int protocol, int ndx)
{
	const struct tcphdr *tcp = (const struct tcphdr *)pkt;
	const struct udphdr *udp = (const struct udphdr *)pkt;
	const struct icmp *icmp = (const struct icmp *)pkt;

	/*
	 * XXX to keep flow in proper canonical format, it may be necessary to
	 * swap the array slots based on the order of the port numbers does
	 * this matter in practice??? I don't think so - return flows will
	 * always match, because of their symmetrical addr/ports
	 */

	switch (protocol) {
	case IPPROTO_TCP:
		/* Check for runt packet, but don't error out on short frags */
		if (caplen < sizeof(*tcp))
			return (isfrag ? 0 : 1);
		flow->port[ndx] = tcp->th_sport;
		flow->port[ndx ^ 1] = tcp->th_dport;
		flow->tcp_flags[ndx] |= tcp->th_flags;
		break;
	case IPPROTO_UDP:
		/* Check for runt packet, but don't error out on short frags */
		if (caplen < sizeof(*udp))
			return (isfrag ? 0 : 1);
		flow->port[ndx] = udp->uh_sport;
		flow->port[ndx ^ 1] = udp->uh_dport;
		break;
	case IPPROTO_ICMP:
		/*
		 * Encode ICMP type * 256 + code into dest port like
		 * Cisco routers
		 */
		flow->port[ndx] = 0;
		flow->port[ndx ^ 1] = htons(icmp->icmp_type * 256 +
		    icmp->icmp_code);
		break;
	}
	return (0);
}

/* Convert a IPv4 packet to a partial flow record (used for comparison) */
static int
ipv4_to_flowrec(struct FLOW *flow, const u_int8_t *pkt, size_t caplen, 
    size_t len, int *isfrag, int af)
{
	const struct ip *ip = (const struct ip *)pkt;
	int ndx;

	if (caplen < 20 || caplen < ip->ip_hl * 4)
		return (-1);	/* Runt packet */
	if (ip->ip_v != 4)
		return (-1);	/* Unsupported IP version */
	
	/* Prepare to store flow in canonical format */
	ndx = memcmp(&ip->ip_src, &ip->ip_dst, sizeof(ip->ip_src)) > 0 ? 1 : 0;
	
	flow->af = af;
	flow->addr[ndx].v4 = ip->ip_src;
	flow->addr[ndx ^ 1].v4 = ip->ip_dst;
	flow->protocol = ip->ip_p;
	flow->octets[ndx] = len;
	flow->packets[ndx] = 1;

	*isfrag = (ntohs(ip->ip_off) & (IP_OFFMASK|IP_MF)) ? 1 : 0;

	/* Don't try to examine higher level headers if not first fragment */
	if (*isfrag && (ntohs(ip->ip_off) & IP_OFFMASK) != 0)
		return (0);

	return (transport_to_flowrec(flow, pkt + (ip->ip_hl * 4), 
	    caplen - (ip->ip_hl * 4), *isfrag, ip->ip_p, ndx));
}

/* Convert a IPv6 packet to a partial flow record (used for comparison) */
static int
ipv6_to_flowrec(struct FLOW *flow, const u_int8_t *pkt, size_t caplen, 
    size_t len, int *isfrag, int af)
{
	const struct ip6_hdr *ip6 = (const struct ip6_hdr *)pkt;
	const struct ip6_ext *eh6;
	const struct ip6_frag *fh6;
	int ndx, nxt;

	if (caplen < sizeof(*ip6))
		return (-1);	/* Runt packet */

	if ((ip6->ip6_vfc & IPV6_VERSION_MASK) != IPV6_VERSION)
		return (-1);	/* Unsupported IPv6 version */

	/* Prepare to store flow in canonical format */
	ndx = memcmp(&ip6->ip6_src, &ip6->ip6_dst,
	    sizeof(ip6->ip6_src)) > 0 ? 1 : 0;
	
	flow->af = af;
	flow->ip6_flowlabel[ndx] = ip6->ip6_flow & IPV6_FLOWLABEL_MASK;
	flow->addr[ndx].v6 = ip6->ip6_src;
	flow->addr[ndx ^ 1].v6 = ip6->ip6_dst;
	flow->octets[ndx] = len;
	flow->packets[ndx] = 1;

	*isfrag = 0;
	nxt = ip6->ip6_nxt;
	pkt += sizeof(*ip6);
	caplen -= sizeof(*ip6);

	/* Now loop through headers, looking for transport header */
	for (;;) {
		eh6 = (const struct ip6_ext *)pkt;
		if (nxt == IPPROTO_HOPOPTS || 
		    nxt == IPPROTO_ROUTING || 
		    nxt == IPPROTO_DSTOPTS) {
			if (caplen < sizeof(*eh6) ||
			    caplen < (eh6->ip6e_len + 1) << 3)
				return (1); /* Runt */
			nxt = eh6->ip6e_nxt;
			pkt += (eh6->ip6e_len + 1) << 3;
			caplen -= (eh6->ip6e_len + 1) << 3;
		} else if (nxt == IPPROTO_FRAGMENT) {
			*isfrag = 1;
			fh6 = (const struct ip6_frag *)eh6;
			if (caplen < sizeof(*fh6))
				return (1); /* Runt */
			/*
			 * Don't try to examine higher level headers if 
			 * not first fragment
			 */
			if ((fh6->ip6f_offlg & IP6F_OFF_MASK) != 0)
				return (0);
			nxt = fh6->ip6f_nxt;
			pkt += sizeof(*fh6);
			caplen -= sizeof(*fh6);
		} else 
			break;
	}
	flow->protocol = nxt;

	return (transport_to_flowrec(flow, pkt, caplen, *isfrag, nxt, ndx));
}

static void
flow_update_expiry(struct FLOWTRACK *ft, struct FLOW *flow)
{
	EXPIRY_REMOVE(EXPIRIES, &ft->expiries, flow->expiry);

	/* Flows over 2 GiB traffic */
	if (flow->octets[0] > (1U << 31) || flow->octets[1] > (1U << 31)) {
		flow->expiry->expires_at = 0;
		flow->expiry->reason = R_OVERBYTES;
		goto out;
	}
	
	/* Flows over maximum life seconds */
	if (ft->maximum_lifetime != 0 && 
	    flow->flow_last.tv_sec - flow->flow_start.tv_sec > 
	    ft->maximum_lifetime) {
		flow->expiry->expires_at = 0;
		flow->expiry->reason = R_MAXLIFE;
		goto out;
	}
	
	if (flow->protocol == IPPROTO_TCP) {
		/* Reset TCP flows */
		if (ft->tcp_rst_timeout != 0 &&
		    ((flow->tcp_flags[0] & TH_RST) ||
		    (flow->tcp_flags[1] & TH_RST))) {
			flow->expiry->expires_at = flow->flow_last.tv_sec + 
			    ft->tcp_rst_timeout;
			flow->expiry->reason = R_TCP_RST;
			goto out;
		}
		/* Finished TCP flows */
		if (ft->tcp_fin_timeout != 0 &&
		    ((flow->tcp_flags[0] & TH_FIN) &&
		    (flow->tcp_flags[1] & TH_FIN))) {
			flow->expiry->expires_at = flow->flow_last.tv_sec + 
			    ft->tcp_fin_timeout;
			flow->expiry->reason = R_TCP_FIN;
			goto out;
		}

		/* TCP flows */
		if (ft->tcp_timeout != 0) {
			flow->expiry->expires_at = flow->flow_last.tv_sec + 
			    ft->tcp_timeout;
			flow->expiry->reason = R_TCP;
			goto out;
		}
	}

	if (ft->udp_timeout != 0 && flow->protocol == IPPROTO_UDP) {
		/* UDP flows */
		flow->expiry->expires_at = flow->flow_last.tv_sec + 
		    ft->udp_timeout;
		flow->expiry->reason = R_UDP;
		goto out;
	}

	if (ft->icmp_timeout != 0 &&
	    ((flow->af == AF_INET && flow->protocol == IPPROTO_ICMP) || 
	    ((flow->af == AF_INET6 && flow->protocol == IPPROTO_ICMPV6)))) {
		/* ICMP flows */
		flow->expiry->expires_at = flow->flow_last.tv_sec + 
		    ft->icmp_timeout;
		flow->expiry->reason = R_ICMP;
		goto out;
	}

	/* Everything else */
	flow->expiry->expires_at = flow->flow_last.tv_sec + 
	    ft->general_timeout;
	flow->expiry->reason = R_GENERAL;

 out:
	if (ft->maximum_lifetime != 0 && flow->expiry->expires_at != 0) {
		flow->expiry->expires_at = MIN(flow->expiry->expires_at,
		    flow->flow_start.tv_sec + ft->maximum_lifetime);
	}

	EXPIRY_INSERT(EXPIRIES, &ft->expiries, flow->expiry);
}


/* Return values from process_packet */
#define PP_OK		0
#define PP_BAD_PACKET	-2
#define PP_MALLOC_FAIL	-3

/*
 * Main per-packet processing function. Take a packet (provided by 
 * libpcap) and attempt to find a matching flow. If no such flow exists, 
 * then create one. 
 *
 * Also marks flows for fast expiry, based on flow or packet attributes
 * (the actual expiry is performed elsewhere)
 */
static int
process_packet(struct FLOWTRACK *ft, const u_int8_t *pkt, int af,
    const u_int32_t caplen, const u_int32_t len, 
    const struct timeval *received_time)
{
	struct FLOW tmp, *flow;
	int frag;

	ft->total_packets++;

	/* Convert the IP packet to a flow identity */
	memset(&tmp, 0, sizeof(tmp));
	switch (af) {
	case AF_INET:
		if (ipv4_to_flowrec(&tmp, pkt, caplen, len, &frag, af) == -1)
			goto bad;
		break;
	case AF_INET6:
		if (ipv6_to_flowrec(&tmp, pkt, caplen, len, &frag, af) == -1)
			goto bad;
		break;
	default:
 bad: 
		ft->bad_packets++;
		return (PP_BAD_PACKET);
	}

	if (frag)
		ft->frag_packets++;

	/* Zero out bits of the flow that aren't relevant to tracking level */
	switch (ft->track_level) {
	case TRACK_IP_ONLY:
		tmp.protocol = 0;
		/* FALLTHROUGH */
	case TRACK_IP_PROTO:
		tmp.port[0] = tmp.port[1] = 0;
		tmp.tcp_flags[0] = tmp.tcp_flags[1] = 0;
		/* FALLTHROUGH */
	case TRACK_FULL:
		break;
	}

	/* If a matching flow does not exist, create and insert one */
	if ((flow = FLOW_FIND(FLOWS, &ft->flows, &tmp)) == NULL) {
		/* Allocate and fill in the flow */
		if ((flow = flow_get(ft)) == NULL) {
			logit(LOG_ERR, "process_packet: flow_get failed",
			    sizeof(*flow));
			return (PP_MALLOC_FAIL);
		}
		memcpy(flow, &tmp, sizeof(*flow));
		memcpy(&flow->flow_start, received_time,
		    sizeof(flow->flow_start));
		flow->flow_seq = ft->next_flow_seq++;
		FLOW_INSERT(FLOWS, &ft->flows, flow);

		/* Allocate and fill in the associated expiry event */
		if ((flow->expiry = expiry_get(ft)) == NULL) {
			logit(LOG_ERR, "process_packet: expiry_get failed",
			    sizeof(*flow->expiry));
			return (PP_MALLOC_FAIL);
		}
		flow->expiry->flow = flow;
		/* Must be non-zero (0 means expire immediately) */
		flow->expiry->expires_at = 1;
		flow->expiry->reason = R_GENERAL;
		EXPIRY_INSERT(EXPIRIES, &ft->expiries, flow->expiry);

		ft->num_flows++;
		if (verbose_flag)
			logit(LOG_DEBUG, "ADD FLOW %s",
			    format_flow_brief(flow));
	} else {
		/* Update flow statistics */
		flow->packets[0] += tmp.packets[0];
		flow->octets[0] += tmp.octets[0];
		flow->tcp_flags[0] |= tmp.tcp_flags[0];
		flow->packets[1] += tmp.packets[1];
		flow->octets[1] += tmp.octets[1];
		flow->tcp_flags[1] |= tmp.tcp_flags[1];
	}
	
	memcpy(&flow->flow_last, received_time, sizeof(flow->flow_last));

	if (flow->expiry->expires_at != 0)
		flow_update_expiry(ft, flow);

	return (PP_OK);
}

/*
 * Subtract two timevals. Returns (t1 - t2) in milliseconds.
 */
u_int32_t
timeval_sub_ms(const struct timeval *t1, const struct timeval *t2)
{
	struct timeval res;

	res.tv_sec = t1->tv_sec - t2->tv_sec;
	res.tv_usec = t1->tv_usec - t2->tv_usec;
	if (res.tv_usec < 0) {
		res.tv_usec += 1000000L;
		res.tv_sec--;
	}
	return ((u_int32_t)res.tv_sec * 1000 + (u_int32_t)res.tv_usec / 1000);
}

static void
update_statistic(struct STATISTIC *s, double new, double n)
{
	if (n == 1.0) {
		s->min = s->mean = s->max = new;
		return;
	}

	s->min = MIN(s->min, new);
	s->max = MAX(s->max, new);

	s->mean = s->mean + ((new - s->mean) / n);
}

/* Update global statistics */
static void
update_statistics(struct FLOWTRACK *ft, struct FLOW *flow)
{
	double tmp;
	static double n = 1.0;

	ft->flows_expired++;
	ft->flows_pp[flow->protocol % 256]++;

	tmp = (double)flow->flow_last.tv_sec +
	    ((double)flow->flow_last.tv_usec / 1000000.0);
	tmp -= (double)flow->flow_start.tv_sec +
	    ((double)flow->flow_start.tv_usec / 1000000.0);
	if (tmp < 0.0)
		tmp = 0.0;

	update_statistic(&ft->duration, tmp, n);
	update_statistic(&ft->duration_pp[flow->protocol], tmp, 
	    (double)ft->flows_pp[flow->protocol % 256]);

	tmp = flow->octets[0] + flow->octets[1];
	update_statistic(&ft->octets, tmp, n);
	ft->octets_pp[flow->protocol % 256] += tmp;

	tmp = flow->packets[0] + flow->packets[1];
	update_statistic(&ft->packets, tmp, n);
	ft->packets_pp[flow->protocol % 256] += tmp;

	n++;
}

static void 
update_expiry_stats(struct FLOWTRACK *ft, struct EXPIRY *e)
{
	switch (e->reason) {
	case R_GENERAL:
		ft->expired_general++;
		break;
	case R_TCP:
		ft->expired_tcp++;
		break;
	case R_TCP_RST:
		ft->expired_tcp_rst++;
		break;
	case R_TCP_FIN:
		ft->expired_tcp_fin++;
		break;
	case R_UDP:
		ft->expired_udp++;
		break;
	case R_ICMP:
		ft->expired_icmp++;
		break;
	case R_MAXLIFE:
		ft->expired_maxlife++;
		break;
	case R_OVERBYTES:
		ft->expired_overbytes++;
		break;
	case R_OVERFLOWS:
		ft->expired_maxflows++;
		break;
	case R_FLUSH:
		ft->expired_flush++;
		break;
	}	
}

/* How long before the next expiry event in millisecond */
static int
next_expire(struct FLOWTRACK *ft)
{
	struct EXPIRY *expiry;
	struct timeval now;
	u_int32_t expires_at, ret, fudge;

	gettimeofday(&now, NULL);

	if ((expiry = EXPIRY_MIN(EXPIRIES, &ft->expiries)) == NULL)
		return (-1); /* indefinite */

	expires_at = expiry->expires_at;

	/* Don't cluster urgent expiries */
	if (expires_at == 0 && (expiry->reason == R_OVERBYTES || 
	    expiry->reason == R_OVERFLOWS || expiry->reason == R_FLUSH))
		return (0); /* Now */

	/* Cluster expiries by expiry_interval */
	if (ft->expiry_interval > 1) {
		if ((fudge = expires_at % ft->expiry_interval) > 0)
			expires_at += ft->expiry_interval - fudge;
	}

	if (expires_at < now.tv_sec)
		return (0); /* Now */

	ret = 999 + (expires_at - now.tv_sec) * 1000;
	return (ret);
}

int ip_is_local(char * hostname , char* ip)
{
	int sockfd;  
	struct addrinfo hints, *servinfo, *p;
	struct sockaddr_in *h;
	int rv;
 
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC; // use AF_INET6 to force IPv6
	hints.ai_socktype = SOCK_STREAM;

	if ( (rv = getaddrinfo( hostname, NULL , &hints , &servinfo)) != 0) 
	{
        	logit(LOG_ERR, "getaddrinfo: %s\n", gai_strerror(rv));
		return -1;
	}
	for(p = servinfo; p != NULL; p = p->ai_next) 
	{
		if(p->ai_family == AF_INET)
		{
			h = (struct sockaddr_in *) p->ai_addr;
			if (strcmp(ip , inet_ntoa( h->sin_addr ) ) == 0)
			{
				freeaddrinfo(servinfo);
				return 0;
			}
		}
	}
     
	freeaddrinfo(servinfo); // all done with this structure
 	return -1;
}
void *
insert_to_influxdb(struct FLOW *flow)
{
	char addr0[64], addr1[64];
	char resetbuf[2048];
	char *ipv4_src = NULL, *ipv4_dst = NULL;
	char hostname[1024];
	uint16_t port_src, port_dst;
	char tcp_flags[64];
	static char *url = "curl -i -XPOST 'http://localhost:8086/write?db=mydb' --data-binary";
	uint64_t time_start, time_end;
	//ipv4 for now
	if(flow->af != AF_INET)
		return;    

	gethostname(hostname, sizeof(hostname));	

	inet_ntop(flow->af, &flow->addr[0], addr0, sizeof(addr0));
	inet_ntop(flow->af, &flow->addr[1], addr1, sizeof(addr1));

	

	if( flow->packets[0] > 0)
	{
		ipv4_src = addr0;
		ipv4_dst = addr1;
		port_src = ntohs(flow->port[0]);
		port_dst = ntohs(flow->port[1]);
		memset(tcp_flags, 0, sizeof(tcp_flags));
		strcat(tcp_flags, tcp_flags_to_str(flow->tcp_flags[0]));
		time_start = flow->flow_start.tv_sec * 1000000000;
		time_end = flow->flow_last.tv_sec * 1000000000;
		snprintf(resetbuf, sizeof(resetbuf), "%s 'myflows,host=%s ipv4_src=\"%s\",port_src=%u,ipv4_dst=\"%s\",port_dst=%u,time_start=%" PRIu64 ",time_end=%" PRIu64 ",proto=\"%s\",tcp_flags=\"%s\",tran_bytes=%u,tran_packets=%u'"
	,url, hostname, ipv4_src, port_src, ipv4_dst, port_dst, time_start, time_end, protocol_to_str(flow->protocol), tcp_flags,flow->octets[0], flow->packets[0]);
	
		logit(LOG_DEBUG,"%s\n",resetbuf);
		system(resetbuf);
	}
	
	if( flow->packets[1] > 0)
	{
		ipv4_src = addr1;
		ipv4_dst = addr0;
		port_src = ntohs(flow->port[1]);
		port_dst = ntohs(flow->port[0]);
		memset(tcp_flags, 0, sizeof(tcp_flags));
		strcat(tcp_flags, tcp_flags_to_str(flow->tcp_flags[1]));
		time_start = flow->flow_start.tv_sec * 1000000000;
		time_end = flow->flow_last.tv_sec * 1000000000;
		snprintf(resetbuf, sizeof(resetbuf), "%s 'myflows,host=%s ipv4_src=\"%s\",port_src=%u,ipv4_dst=\"%s\",port_dst=%u,time_start=%" PRIu64 ",time_end=%" PRIu64 ",proto=\"%s\",tcp_flags=\"%s\",tran_bytes=%u,tran_packets=%u'"
	,url, hostname, ipv4_src, port_src, ipv4_dst, port_dst, time_start, time_end, protocol_to_str(flow->protocol), tcp_flags,flow->octets[1], flow->packets[1]);
	
		logit(LOG_DEBUG,"%s\n",resetbuf);
		system(resetbuf);
	}

}

void *
insert_to_elasticsearch(struct FLOW *flow)
{
	char addr0[64], addr1[64], stime[32], ftime[32];
	char resetbuf[2048];
	char *ipv4_src = NULL, *ipv4_dst = NULL;
	char hostname[1024];
	uint16_t port_src, port_dst;
	char tcp_flags_text[64];
	uint8_t tcp_flags_rst = 0;
	struct timeval now;
	static char *url = "curl -XPOST 'http://localhost:9200/my_index/my_flows/?pretty' -H 'Content-Type: application/json' -d";
	//ipv4 for now
	if(flow->af != AF_INET)
		return;    

	gettimeofday(&now, NULL);
	gethostname(hostname, sizeof(hostname));	

	inet_ntop(flow->af, &flow->addr[0], addr0, sizeof(addr0));
	inet_ntop(flow->af, &flow->addr[1], addr1, sizeof(addr1));

	snprintf(stime, sizeof(stime), "%s", 
	    format_time(flow->flow_start.tv_sec));
	snprintf(ftime, sizeof(ftime), "%s", 
	    format_time(flow->flow_last.tv_sec));
	
	if( flow->packets[0] > 0)
	{
		ipv4_src = addr0;
		ipv4_dst = addr1;
		port_src = ntohs(flow->port[0]);
		port_dst = ntohs(flow->port[1]);
		memset(tcp_flags_text, 0, sizeof(tcp_flags_text));
		strcat(tcp_flags_text, tcp_flags_to_str(flow->tcp_flags[0]));
		tcp_flags_rst = 0;
		if(flow->tcp_flags[0] & TH_RST)
		{
			tcp_flags_rst = 1;
		}
		snprintf(resetbuf, sizeof(resetbuf), "%s \'\n" 
"{\n " \
"\t\"@timestamp\"            : %" PRIu64 ",\n " \
"\t\"agent_host_name\"       : \"%s\",\n " \
"\t\"ipv4_dst_addr\"         : \"%s\",\n " \
"\t\"ipv4_src_addr\"         : \"%s\",\n " \
"\t\"l4_dst_port\"           : %u,\n " \
"\t\"l4_src_port\"           : %u,\n " \
"\t\"tcp_flags\"             : %u,\n " \
"\t\"tcp_flags_text\"        : \"%s\",\n " \
"\t\"has_tcp_rst\"           : %u,\n " \
"\t\"protocol\"              : %u,\n " \
"\t\"protocol_text\"         : \"%s\",\n " \
"\t\"first_switched\"        : %" PRIu64 ",\n " \
"\t\"first_switched_text\"   : \"%s\",\n " \
"\t\"last_switched\"         : %" PRIu64 ",\n " \
"\t\"last_switched_text\"    : \"%s\",\n " \
"\t\"in_bytes\"              : %u,\n " \
"\t\"in_pkts\"               : %u\n " \
"} \n\'",url, (uint64_t)(now.tv_sec) * 1000, hostname, ipv4_dst, ipv4_src, port_dst, port_src, flow->tcp_flags[0], \
tcp_flags_text, tcp_flags_rst, flow->protocol, protocol_to_str(flow->protocol), (uint64_t)(flow->flow_start.tv_sec) * 1000 , stime, \
(uint64_t)(flow->flow_last.tv_sec) * 1000, ftime, flow->octets[0], flow->packets[0]);
	
		logit(LOG_DEBUG,"%s\n",resetbuf);
		system(resetbuf);
	}
	
	if( flow->packets[1] > 0)
	{
		ipv4_src = addr1;
		ipv4_dst = addr0;
		port_src = ntohs(flow->port[1]);
		port_dst = ntohs(flow->port[0]);
		memset(tcp_flags_text, 0, sizeof(tcp_flags_text));
		strcat(tcp_flags_text, tcp_flags_to_str(flow->tcp_flags[1]));
		tcp_flags_rst = 0;
		if(flow->tcp_flags[1] & TH_RST)
		{
			tcp_flags_rst = 1;
		}
		
		snprintf(resetbuf, sizeof(resetbuf), "%s \'\n" 
"{\n " \
"\t\"@timestamp\"            : %" PRIu64 ",\n " \
"\t\"agent_host_name\"       : \"%s\",\n " \
"\t\"ipv4_dst_addr\"         : \"%s\",\n " \
"\t\"ipv4_src_addr\"         : \"%s\",\n " \
"\t\"l4_dst_port\"           : %u,\n " \
"\t\"l4_src_port\"           : %u,\n " \
"\t\"tcp_flags\"             : %u,\n " \
"\t\"tcp_flags_text\"        : \"%s\",\n " \
"\t\"has_tcp_rst\"           : %u,\n " \
"\t\"protocol\"              : %u,\n " \
"\t\"protocol_text\"         : \"%s\",\n " \
"\t\"first_switched\"        : %" PRIu64 ",\n " \
"\t\"first_switched_text\"   : \"%s\",\n " \
"\t\"last_switched\"         : %" PRIu64 ",\n " \
"\t\"last_switched_text\"    : \"%s\",\n " \
"\t\"in_bytes\"              : %u,\n " \
"\t\"in_pkts\"               : %u\n " \
"} \n\'",url, (uint64_t)(now.tv_sec) * 1000, hostname, ipv4_dst, ipv4_src, port_dst, port_src, flow->tcp_flags[1], tcp_flags_text, tcp_flags_rst,\
flow->protocol, protocol_to_str(flow->protocol), (uint64_t)(flow->flow_start.tv_sec) * 1000, stime, (uint64_t)(flow->flow_last.tv_sec) * 1000, \
ftime, flow->octets[1], flow->packets[1]);
	
		logit(LOG_DEBUG,"%s\n",resetbuf);
		system(resetbuf);
	}

}

/*
 * Scan the tree of expiry events and process expired flows. If zap_all
 * is set, then forcibly expire all flows.
 */
#define CE_EXPIRE_NORMAL	0  /* Normal expiry processing */
#define CE_EXPIRE_ALL		-1 /* Expire all flows immediately */
#define CE_EXPIRE_FORCED	1  /* Only expire force-expired flows */
static int
check_expired(struct FLOWTRACK *ft, int ex)
{
	struct FLOW **expired_flows, **oldexp;
	int num_expired, i, r;
	struct timeval now;

	struct EXPIRY *expiry, *nexpiry;

	gettimeofday(&now, NULL);
	r = 0;
	num_expired = 0;
	expired_flows = NULL;

	if (verbose_flag)
		logit(LOG_DEBUG, "Starting expiry scan: mode %d", ex);

	for(expiry = EXPIRY_MIN(EXPIRIES, &ft->expiries);
	    expiry != NULL;
	    expiry = nexpiry) {
		nexpiry = EXPIRY_NEXT(EXPIRIES, &ft->expiries, expiry);
		if ((expiry->expires_at == 0) || (ex == CE_EXPIRE_ALL) || 
		    (ex != CE_EXPIRE_FORCED &&
		    (expiry->expires_at < now.tv_sec))) {
			/* Flow has expired */

			if (ft->maximum_lifetime != 0 && 
	    		    expiry->flow->flow_last.tv_sec - 
			    expiry->flow->flow_start.tv_sec >= 
	    		    ft->maximum_lifetime)
					expiry->reason = R_MAXLIFE;

			if (verbose_flag)
			{
				char *reasonbuf;
				if(expiry->reason == R_GENERAL)
				{
					reasonbuf = "R_GENERAL";
				}
				else if(expiry->reason == R_TCP)
				{
					reasonbuf = "R_TCP";
				}
				else if(expiry->reason == R_TCP_RST)
				{
					reasonbuf = "R_TCP_RST";
				}
				else if(expiry->reason == R_TCP_FIN)
				{
					reasonbuf = "R_TCP_FIN";
				}
				else if(expiry->reason == R_UDP)
				{
					reasonbuf = "R_UDP";
				}
				else if(expiry->reason == R_ICMP)
				{
					reasonbuf = "R_ICMP";
				}
				else if(expiry->reason == R_MAXLIFE)
				{
					reasonbuf = "R_MAXLIFE";
				}
				else if(expiry->reason == R_OVERBYTES)
				{
					reasonbuf = "R_OVERFLOWS";
				}
				else if(expiry->reason == R_FLUSH)
				{
					reasonbuf = "R_FLUSH";
				}
				else
				{
					reasonbuf = "OTHERS";
				}
				logit(LOG_DEBUG,
				    "Queuing flow seq:%"PRIu64" (%p) for expiry "
				    "reason %d, %s", expiry->flow->flow_seq,
				    expiry->flow, expiry->reason, reasonbuf);
			}

			/* Add to array of expired flows */
			oldexp = expired_flows;
			expired_flows = realloc(expired_flows,
			    sizeof(*expired_flows) * (num_expired + 1));
			/* Don't fatal on realloc failures */
			if (expired_flows == NULL)
				expired_flows = oldexp;
			else {
				expired_flows[num_expired] = expiry->flow;
				num_expired++;
			}

			if (ex == CE_EXPIRE_ALL)
				expiry->reason = R_FLUSH;

			update_expiry_stats(ft, expiry);

			/* Remove from flow tree, destroy expiry event */
			FLOW_REMOVE(FLOWS, &ft->flows, expiry->flow);
			EXPIRY_REMOVE(EXPIRIES, &ft->expiries, expiry);
			expiry->flow->expiry = NULL;
			expiry_put(ft, expiry);

			ft->num_flows--;
		}
	}

	if (verbose_flag)
		logit(LOG_DEBUG, "Finished scan %d flow(s) to be evicted",
		    num_expired);
	
	/* Processing for expired flows */
	if (num_expired > 0) {
		for (i = 0; i < num_expired; i++) {
			if (verbose_flag) {
				logit(LOG_DEBUG, "EXPIRED: %s (%p)", 
				    format_flow(expired_flows[i]),
				    expired_flows[i]);
			}
			//insert_to_influxdb(expired_flows[i]);
			insert_to_elasticsearch(expired_flows[i]);
			update_statistics(ft, expired_flows[i]);
			flow_put(ft, expired_flows[i]);
		}
	
		free(expired_flows);
	}

	return (r == -1 ? -1 : num_expired);
}

/*
 * Force expiry of num_to_expire flows (e.g. when flow table overfull) 
 */
static void
force_expire(struct FLOWTRACK *ft, u_int32_t num_to_expire)
{
	struct EXPIRY *expiry, **expiryv;
	int i;

	/* XXX move all overflow processing here (maybe) */
	if (verbose_flag)
		logit(LOG_INFO, "Forcing expiry of %d flows",
		    num_to_expire);

	/*
	 * Do this in two steps, as it is dangerous to change a key on 
	 * a tree entry without first removing it and then re-adding it.
	 * It is even worse when this has to be done during a FOREACH :)
	 * To get around this, we make a list of expired flows and _then_ 
	 * alter them 
	 */
	 
	if ((expiryv = calloc(num_to_expire, sizeof(*expiryv))) == NULL) {
		/*
		 * On malloc failure, expire ALL flows. I assume that 
		 * setting all the keys in a tree to the same value is 
		 * safe.
		 */
		logit(LOG_ERR, "Out of memory while expiring flows - "
		    "all flows expired");
		EXPIRY_FOREACH(expiry, EXPIRIES, &ft->expiries) {
			expiry->expires_at = 0;
			expiry->reason = R_OVERFLOWS;
			ft->flows_force_expired++;
		}
		return;
	}
	
	/* Make the list of flows to expire */
	i = 0;
	EXPIRY_FOREACH(expiry, EXPIRIES, &ft->expiries) {
		if (i >= num_to_expire)
			break;
		expiryv[i++] = expiry;
	}
	if (i < num_to_expire) {
		logit(LOG_ERR, "Needed to expire %d flows, "
		    "but only %d active", num_to_expire, i);
		num_to_expire = i;
	}

	for(i = 0; i < num_to_expire; i++) {
		EXPIRY_REMOVE(EXPIRIES, &ft->expiries, expiryv[i]);
		expiryv[i]->expires_at = 0;
		expiryv[i]->reason = R_OVERFLOWS;
		EXPIRY_INSERT(EXPIRIES, &ft->expiries, expiryv[i]);
	}
	ft->flows_force_expired += num_to_expire;
	free(expiryv);
	/* XXX - this is overcomplicated, perhaps use a separate queue */
}

/* Delete all flows that we know about without processing */
static int
delete_all_flows(struct FLOWTRACK *ft)
{
	struct FLOW *flow, *nflow;
	int i;
	
	i = 0;
	for(flow = FLOW_MIN(FLOWS, &ft->flows); flow != NULL; flow = nflow) {
		nflow = FLOW_NEXT(FLOWS, &ft->flows, flow);
		FLOW_REMOVE(FLOWS, &ft->flows, flow);
		
		EXPIRY_REMOVE(EXPIRIES, &ft->expiries, flow->expiry);
		expiry_put(ft, flow->expiry);

		ft->num_flows--;
		flow_put(ft, flow);
		i++;
	}
	
	return (i);
}

/*
 * Log our current status. 
 * Includes summary counters and (in verbose mode) the list of current flows
 * and the tree of expiry events.
 */
static int
statistics(struct FLOWTRACK *ft, FILE *out, pcap_t *pcap)
{
	int i;
	struct protoent *pe;
	char proto[32];
	struct pcap_stat ps;

	fprintf(out, "Number of active flows: %d\n", ft->num_flows);
	fprintf(out, "Packets processed: %"PRIu64"\n", ft->total_packets);
	if (ft->non_sampled_packets) 
		fprintf(out, "Packets non-sampled: %"PRIu64"\n",
			ft->non_sampled_packets);
	fprintf(out, "Fragments: %"PRIu64"\n", ft->frag_packets);
	fprintf(out, "Ignored packets: %"PRIu64" (%"PRIu64" non-IP, %"PRIu64" too short)\n",
	    ft->non_ip_packets + ft->bad_packets, ft->non_ip_packets, ft->bad_packets);
	fprintf(out, "Flows expired: %"PRIu64" (%"PRIu64" forced)\n", 
	    ft->flows_expired, ft->flows_force_expired);

	if (pcap_stats(pcap, &ps) == 0) {
		fprintf(out, "Packets received by libpcap: %lu\n",
		    (unsigned long)ps.ps_recv);
		fprintf(out, "Packets dropped by libpcap: %lu\n",
		    (unsigned long)ps.ps_drop);
		fprintf(out, "Packets dropped by interface: %lu\n",
		    (unsigned long)ps.ps_ifdrop);
	}

	fprintf(out, "\n");

	if (ft->flows_expired != 0) {
		fprintf(out, "Expired flow statistics:  minimum       average       maximum\n");
		fprintf(out, "  Flow bytes:        %12.0f  %12.0f  %12.0f\n", 
		    ft->octets.min, ft->octets.mean, ft->octets.max);
		fprintf(out, "  Flow packets:      %12.0f  %12.0f  %12.0f\n", 
		    ft->packets.min, ft->packets.mean, ft->packets.max);
		fprintf(out, "  Duration:          %12.2fs %12.2fs %12.2fs\n", 
		    ft->duration.min, ft->duration.mean, ft->duration.max);

		fprintf(out, "\n");
		fprintf(out, "Expired flow reasons:\n");
		fprintf(out, "       tcp = %9"PRIu64"   tcp.rst = %9"PRIu64"   "
		    "tcp.fin = %9"PRIu64"\n", ft->expired_tcp, ft->expired_tcp_rst,
		    ft->expired_tcp_fin);
		fprintf(out, "       udp = %9"PRIu64"      icmp = %9"PRIu64"   "
		    "general = %9"PRIu64"\n", ft->expired_udp, ft->expired_icmp,
		    ft->expired_general);
		fprintf(out, "   maxlife = %9"PRIu64"\n", ft->expired_maxlife);
		fprintf(out, "over 2 GiB = %9"PRIu64"\n", ft->expired_overbytes);
		fprintf(out, "  maxflows = %9"PRIu64"\n", ft->expired_maxflows);
		fprintf(out, "   flushed = %9"PRIu64"\n", ft->expired_flush);

		fprintf(out, "\n");

		fprintf(out, "Per-protocol statistics:     Octets      "
		    "Packets   Avg Life    Max Life\n");
		for(i = 0; i < 256; i++) {
			if (ft->packets_pp[i]) {
				pe = getprotobynumber(i);
				snprintf(proto, sizeof(proto), "%s (%d)", 
				    pe != NULL ? pe->p_name : "Unknown", i);
				fprintf(out, "  %17s: %14"PRIu64" %12"PRIu64"   %8.2fs "
				    "%10.2fs\n", proto,
				    ft->octets_pp[i], 
				    ft->packets_pp[i],
				    ft->duration_pp[i].mean,
				    ft->duration_pp[i].max);
			}
		}
	}

	return (0);
}

static void
dump_flows(struct FLOWTRACK *ft, FILE *out)
{
	struct EXPIRY *expiry;
	time_t now;

	now = time(NULL);

	EXPIRY_FOREACH(expiry, EXPIRIES, &ft->expiries) {
		fprintf(out, "ACTIVE %s\n", format_flow(expiry->flow));
		if ((long int) expiry->expires_at - now < 0) {
			fprintf(out, 
			    "EXPIRY EVENT for flow %"PRIu64" now%s\n",
			    expiry->flow->flow_seq, 
			    expiry->expires_at == 0 ? " (FORCED)": "");
		} else {
			fprintf(out, 
			    "EXPIRY EVENT for flow %"PRIu64" in %ld seconds\n",
			    expiry->flow->flow_seq, 
			    (long int) expiry->expires_at - now);
		}
		fprintf(out, "\n");
	}
}

/*
 * Figure out how many bytes to skip from front of packet to get past 
 * datalink headers. If pkt is specified, also check whether determine
 * whether or not it is one that we are interested in (IPv4 or IPv6 for now)
 *
 * Returns number of bytes to skip or -1 to indicate that entire 
 * packet should be skipped
 */
static int 
datalink_check(int linktype, const u_int8_t *pkt, u_int32_t caplen, int *af)
{
	int i, j;
	u_int32_t frametype;
	static const struct DATALINK *dl = NULL;

	/* Try to cache last used linktype */
	if (dl == NULL || dl->dlt != linktype) {
		for (i = 0; lt[i].dlt != linktype && lt[i].dlt != -1; i++)
			;
		dl = &lt[i];
	}
	if (dl->dlt == -1 || pkt == NULL)
		return (dl->dlt);
	if (caplen <= dl->skiplen)
		return (-1);

	/* Suck out the frametype */
	frametype = 0;
	if (dl->ft_is_be) {
		for (j = 0; j < dl->ft_len; j++) {
			frametype <<= 8;
			frametype |= pkt[j + dl->ft_off];
		}
	} else {
		for (j = dl->ft_len - 1; j >= 0 ; j--) {
			frametype <<= 8;
			frametype |= pkt[j + dl->ft_off];
		}
	}
	frametype &= dl->ft_mask;

	if (frametype == dl->ft_v4)
		*af = AF_INET;
	else if (frametype == dl->ft_v6)
		*af = AF_INET6;
	else
		return (-1);
	
	return (dl->skiplen);
}

/*
 * Per-packet callback function from libpcap. Pass the packet (if it is IP)
 * sans datalink headers to process_packet.
 */
static void
flow_cb(u_char *user_data, const struct pcap_pkthdr* phdr, 
    const u_char *pkt)
{
	int s, af;
	struct CB_CTXT *cb_ctxt = (struct CB_CTXT *)user_data;
	struct timeval tv;

	if (cb_ctxt->ft->option.sample &&
	    (cb_ctxt->ft->total_packets +
	     cb_ctxt->ft->non_sampled_packets) %
	    cb_ctxt->ft->option.sample > 0) {
		cb_ctxt->ft->non_sampled_packets++;
		return;
	}
	s = datalink_check(cb_ctxt->linktype, pkt, phdr->caplen, &af);
	if (s < 0 || (!cb_ctxt->want_v6 && af == AF_INET6)) {
		cb_ctxt->ft->non_ip_packets++;
	} else {
		tv.tv_sec = phdr->ts.tv_sec;
		tv.tv_usec = phdr->ts.tv_usec;
		if (process_packet(cb_ctxt->ft, pkt + s, af,
		    phdr->caplen - s, phdr->len - s, &tv) == PP_MALLOC_FAIL)
			cb_ctxt->fatal = 1;
	}
}

static void
print_timeouts(struct FLOWTRACK *ft, FILE *out)
{
	fprintf(out, "           TCP timeout: %ds\n", ft->tcp_timeout);
	fprintf(out, "  TCP post-RST timeout: %ds\n", ft->tcp_rst_timeout);
	fprintf(out, "  TCP post-FIN timeout: %ds\n", ft->tcp_fin_timeout);
	fprintf(out, "           UDP timeout: %ds\n", ft->udp_timeout);
	fprintf(out, "          ICMP timeout: %ds\n", ft->icmp_timeout);
	fprintf(out, "       General timeout: %ds\n", ft->general_timeout);
	fprintf(out, "      Maximum lifetime: %ds\n", ft->maximum_lifetime);
	fprintf(out, "       Expiry interval: %ds\n", ft->expiry_interval);
}

static int
accept_control(int lsock, struct FLOWTRACK *ft,
    pcap_t *pcap, int *exit_request, int *stop_collection_flag)
{
	unsigned char buf[64], *p;
	FILE *ctlf;
	int fd, ret;

	if ((fd = accept(lsock, NULL, NULL)) == -1) {
		logit(LOG_ERR, "ctl accept: %s - exiting",
		    strerror(errno));
		return(-1);
	}
	if ((ctlf = fdopen(fd, "r+")) == NULL) {
		logit(LOG_ERR, "fdopen: %s - exiting\n",
		    strerror(errno));
		close(fd);
		return (-1);
	}
	setlinebuf(ctlf);

	if (fgets(buf, sizeof(buf), ctlf) == NULL) {
		logit(LOG_ERR, "Control socket yielded no data");
		return (0);
	}
	if ((p = strchr(buf, '\n')) != NULL)
		*p = '\0';
	
	if (verbose_flag)
		logit(LOG_DEBUG, "Control socket \"%s\"", buf);

	/* XXX - use dispatch table */
	ret = -1;
	if (strcmp(buf, "help") == 0) {
		fprintf(ctlf, "Valid control words are:\n");
		fprintf(ctlf, "\tdebug+ debug- delete-all dump-flows exit "
		    "expire-all\n");
		fprintf(ctlf, "\tshutdown start-gather statistics stop-gather "
		    "timeouts\n");
		fprintf(ctlf, "\tsend-template\n");
		ret = 0;
	} else if (strcmp(buf, "shutdown") == 0) {
		fprintf(ctlf, "softflowd[%u]: Shutting down gracefully...\n", 
		    getpid());
		graceful_shutdown_request = 1;
		ret = 1;
	} else if (strcmp(buf, "exit") == 0) {
		fprintf(ctlf, "softflowd[%u]: Exiting now...\n", getpid());
		*exit_request = 1;
		ret = 1;
	} else if (strcmp(buf, "expire-all") == 0) {
		fprintf(ctlf, "softflowd[%u]: Expired %d flows.\n", getpid(), 
		    check_expired(ft, CE_EXPIRE_ALL));
		ret = 0;
	} else if (strcmp(buf, "delete-all") == 0) {
		fprintf(ctlf, "softflowd[%u]: Deleted %d flows.\n", getpid(), 
		    delete_all_flows(ft));
		ret = 0;
	} else if (strcmp(buf, "statistics") == 0) {
		fprintf(ctlf, "softflowd[%u]: Accumulated statistics "
		    "since %s UTC:\n", getpid(),
		    format_time(ft->system_boot_time.tv_sec));
		statistics(ft, ctlf, pcap);
		ret = 0;
	} else if (strcmp(buf, "debug+") == 0) {
		fprintf(ctlf, "softflowd[%u]: Debug level increased.\n",
		    getpid());
		verbose_flag = 1;
		ret = 0;
	} else if (strcmp(buf, "debug-") == 0) {
		fprintf(ctlf, "softflowd[%u]: Debug level decreased.\n",
		    getpid());
		verbose_flag = 0;
		ret = 0;
	} else if (strcmp(buf, "stop-gather") == 0) {
		fprintf(ctlf, "softflowd[%u]: Data collection stopped.\n",
		    getpid());
		*stop_collection_flag = 1;
		ret = 0;
	} else if (strcmp(buf, "start-gather") == 0) {
		fprintf(ctlf, "softflowd[%u]: Data collection resumed.\n",
		    getpid());
		*stop_collection_flag = 0;
		ret = 0;
	} else if (strcmp(buf, "dump-flows") == 0) {
		fprintf(ctlf, "softflowd[%u]: Dumping flow data:\n",
		    getpid());
		dump_flows(ft, ctlf);
		ret = 0;
	} else if (strcmp(buf, "timeouts") == 0) {
		fprintf(ctlf, "softflowd[%u]: Printing timeouts:\n",
		    getpid());
		print_timeouts(ft, ctlf);
		ret = 0;
	} else {
		fprintf(ctlf, "Unknown control commmand \"%s\"\n", buf);
		ret = 0;
	}

	fclose(ctlf);
	close(fd);
	
	return (ret);
}

static int 
unix_listener(const char *path)
{
	struct sockaddr_un addr;
	socklen_t addrlen;
	int s;

	memset(&addr, '\0', sizeof(addr));
	addr.sun_family = AF_UNIX;
	
	if (strlcpy(addr.sun_path, path, sizeof(addr.sun_path)) >=
	    sizeof(addr.sun_path)) {
		fprintf(stderr, "control socket path too long\n");
		exit(1);
	}
	
	addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';
	
	addrlen = offsetof(struct sockaddr_un, sun_path) + strlen(path) + 1;
#ifdef SOCK_HAS_LEN 
	addr.sun_len = addrlen;
#endif

	if ((s = socket(PF_UNIX, SOCK_STREAM, 0)) < 0) {
		fprintf(stderr, "unix domain socket() error: %s\n", 
		    strerror(errno));
		exit(1);
	}
	unlink(path);
	if (bind(s, (struct sockaddr*)&addr, addrlen) == -1) {
		fprintf(stderr, "unix domain bind(\"%s\") error: %s\n",
		    addr.sun_path, strerror(errno));
		exit(1);
	}
	if (listen(s, 64) == -1) {
		fprintf(stderr, "unix domain listen() error: %s\n",
		    strerror(errno));
		exit(1);
	}
	
	return (s);
}

static void
setup_packet_capture(struct pcap **pcap, int *linktype, 
    char *dev, char *capfile, char *bpf_prog, int need_v6)
{
	char ebuf[PCAP_ERRBUF_SIZE];
	struct bpf_program prog_c;
	u_int32_t bpf_mask, bpf_net;

	/* Open pcap */
	if (dev != NULL) {
		if ((*pcap = pcap_open_live(dev, 
		    need_v6 ? LIBPCAP_SNAPLEN_V6 : LIBPCAP_SNAPLEN_V4, 
		    1, 0, ebuf)) == NULL) {
			fprintf(stderr, "pcap_open_live: %s\n", ebuf);
			exit(1);
		}
		if (pcap_lookupnet(dev, &bpf_net, &bpf_mask, ebuf) == -1)
			bpf_net = bpf_mask = 0;
	} else {
		if ((*pcap = pcap_open_offline(capfile, ebuf)) == NULL) {
			fprintf(stderr, "pcap_open_offline(%s): %s\n", 
			    capfile, ebuf);
			exit(1);
		}
		bpf_net = bpf_mask = 0;
	}
	*linktype = pcap_datalink(*pcap);
	if (datalink_check(*linktype, NULL, 0, NULL) == -1) {
		fprintf(stderr, "Unsupported datalink type %d\n", *linktype);
		exit(1);
	}
	/* Attach BPF filter, if specified */
	if (bpf_prog != NULL) {
		if (pcap_compile(*pcap, &prog_c, bpf_prog, 1, bpf_mask) == -1) {
			fprintf(stderr, "pcap_compile(\"%s\"): %s\n", 
			    bpf_prog, pcap_geterr(*pcap));
			exit(1);
		}
		if (pcap_setfilter(*pcap, &prog_c) == -1) {
			fprintf(stderr, "pcap_setfilter: %s\n", 
			    pcap_geterr(*pcap));
			exit(1);
		}
	}

#ifdef BIOCLOCK
	/*
	 * If we are reading from an device (not a file), then 
	 * lock the underlying BPF device to prevent changes in the 
	 * unprivileged child
	 */
	if (dev != NULL && ioctl(pcap_fileno(*pcap), BIOCLOCK) < 0) {
		fprintf(stderr, "ioctl(BIOCLOCK) failed: %s\n",
		    strerror(errno));
		exit(1);
	}
#endif
}

static void
init_flowtrack(struct FLOWTRACK *ft)
{
	/* Set up flow-tracking structure */
	memset(ft, '\0', sizeof(*ft));
	ft->next_flow_seq = 1;
	FLOW_INIT(&ft->flows);
	EXPIRY_INIT(&ft->expiries);
	
	freelist_init(&ft->flow_freelist, sizeof(struct FLOW));
	freelist_init(&ft->expiry_freelist, sizeof(struct EXPIRY));

	ft->max_flows = DEFAULT_MAX_FLOWS;

	ft->track_level = TRACK_FULL;

	ft->tcp_timeout = DEFAULT_TCP_TIMEOUT;
	ft->tcp_rst_timeout = DEFAULT_TCP_RST_TIMEOUT;
	ft->tcp_fin_timeout = DEFAULT_TCP_FIN_TIMEOUT;
	ft->udp_timeout = DEFAULT_UDP_TIMEOUT;
	ft->icmp_timeout = DEFAULT_ICMP_TIMEOUT;
	ft->general_timeout = DEFAULT_GENERAL_TIMEOUT;
	ft->maximum_lifetime = DEFAULT_MAXIMUM_LIFETIME;
	ft->expiry_interval = DEFAULT_EXPIRY_INTERVAL;
}

static char *
argv_join(int argc, char **argv)
{
	int i;
	size_t ret_len;
	char *ret;

	ret_len = 0;
	ret = NULL;
	for (i = 0; i < argc; i++) {
		ret_len += strlen(argv[i]);
		if ((ret = realloc(ret, ret_len + 2)) == NULL) {
			fprintf(stderr, "Memory allocation failed.\n");
			exit(1);
		}
		if (i == 0)
			ret[0] = '\0';
		else {
			ret_len++; /* Make room for ' ' */
			strlcat(ret, " ", ret_len + 1);
		}
			
		strlcat(ret, argv[i], ret_len + 1);
	}

	return (ret);
}

/* Display commandline usage information */
static void
usage(void)
{
	fprintf(stderr, 
"Usage: %s [options] [bpf_program]\n"
"This is %s version %s. Valid commandline options:\n"
"  -i [idx:]interface Specify interface to listen on\n"
"  -r pcap_file       Specify packet capture file to read\n"
"  -m max_flows       Specify maximum number of flows to track (default %d)\n"
"  -p pidfile         Record pid in specified file\n"
"                     (default: %s)\n"
"  -c pidfile         Location of control socket\n"
"                     (default: %s)\n"
"  -L hoplimit        Set TTL/hoplimit for export datagrams\n"
"  -T full|proto|ip   Set flow tracking level (default: full)\n"
"  -d                 Don't daemonise (run in foreground)\n"
"  -D                 Debug mode: foreground + verbosity + track v6 flows\n"
"  -s sampling_rate   Specify periodical sampling rate (denominator)\n"
"  -h                 Display this help\n"
"\n"
"Valid timeout names and default values:\n"
"  tcp     (default %6d)"
"  tcp.rst (default %6d)"
"  tcp.fin (default %6d)\n"
"  udp     (default %6d)"
"  icmp    (default %6d)"
"  general (default %6d)\n"
"  maxlife (default %6d)"
"  expint  (default %6d)\n"
"\n" ,
	    PROGNAME, PROGNAME, PROGVER, DEFAULT_MAX_FLOWS, DEFAULT_PIDFILE,
	    DEFAULT_CTLSOCK, DEFAULT_TCP_TIMEOUT, DEFAULT_TCP_RST_TIMEOUT,
	    DEFAULT_TCP_FIN_TIMEOUT, DEFAULT_UDP_TIMEOUT, DEFAULT_ICMP_TIMEOUT,
	    DEFAULT_GENERAL_TIMEOUT, DEFAULT_MAXIMUM_LIFETIME,
	    DEFAULT_EXPIRY_INTERVAL);
}

/* 
 * Drop privileges and chroot, will exit on failure
 */
static void 
drop_privs(void)
{
	struct passwd *pw;
	
	if ((pw = getpwnam(PRIVDROP_USER)) == NULL) {
		logit(LOG_ERR, "Unable to find unprivileged user \"%s\"", 
		    PRIVDROP_USER);
		exit(1);
	}
	if (chdir(PRIVDROP_CHROOT_DIR) != 0) {
		logit(LOG_ERR, "Unable to chdir to chroot directory \"%s\": %s",
		    PRIVDROP_CHROOT_DIR, strerror(errno));
		exit(1);
	}
	if (chroot(PRIVDROP_CHROOT_DIR) != 0) {
		logit(LOG_ERR, "Unable to chroot to directory \"%s\": %s",
		    PRIVDROP_CHROOT_DIR, strerror(errno));
		exit(1);
	}
	if (chdir("/") != 0) {
		logit(LOG_ERR, "Unable to chdir to chroot root: %s",
		    strerror(errno));
		exit(1);
	}
	if (setgroups(1, &pw->pw_gid) != 0) {
		logit(LOG_ERR, "Couldn't setgroups (%u): %s",
		    (unsigned int)pw->pw_gid, strerror(errno));
		exit(1);
	}
#if defined(HAVE_SETRESGID)
	if (setresgid(pw->pw_gid, pw->pw_gid, pw->pw_gid) == -1) {
#elif defined(HAVE_SETREGID)
	if (setregid(pw->pw_gid, pw->pw_gid) == -1) {
#else
	if (setegid(pw->pw_gid) == -1 || setgid(pw->pw_gid) == -1) {
#endif
		logit(LOG_ERR, "Couldn't set gid (%u): %s",
		    (unsigned int)pw->pw_gid, strerror(errno));
		exit(1);
	}

#if defined(HAVE_SETRESUID)
	if (setresuid(pw->pw_uid, pw->pw_uid, pw->pw_uid) == -1) {
#elif defined(HAVE_SETREUID)
	if (setreuid(pw->pw_uid, pw->pw_uid) == -1) {
#else
	if (seteuid(pw->pw_uid) == -1 || setuid(pw->pw_uid) == -1) {
#endif
		logit(LOG_ERR, "Couldn't set uid (%u): %s",
		    (unsigned int)pw->pw_uid, strerror(errno));
		exit(1);
	}
}

int
main(int argc, char **argv)
{
	char *dev, *capfile, *bpf_prog, dest_addr[256], dest_serv[256];
	const char *pidfile_path, *ctlsock_path;
	extern char *optarg;
	extern int optind;
	int ch, dontfork_flag, linktype, ctlsock, i, err, always_v6, r;
	int stop_collection_flag, exit_request, hoplimit;
	pcap_t *pcap = NULL;
	struct sockaddr_storage dest;
	struct FLOWTRACK flowtrack;
	socklen_t dest_len;
	struct CB_CTXT cb_ctxt;
	struct pollfd pl[2];
	struct timeval now;
	static struct timeval since_last_expire_all;
	gettimeofday(&since_last_expire_all, NULL);

	closefrom(STDERR_FILENO + 1);

	init_flowtrack(&flowtrack);

	memset(&dest, '\0', sizeof(dest));
	dest_len = 0;
	hoplimit = -1;
	bpf_prog = NULL;
	ctlsock = -1;
	dev = capfile = NULL;
	pidfile_path = DEFAULT_PIDFILE;
	ctlsock_path = DEFAULT_CTLSOCK;
	dontfork_flag = 0;
	always_v6 = 0;

	while ((ch = getopt(argc, argv, "6hdDL:T:i:r:f:t:n:m:p:c:v:s:")) != -1) {
		switch (ch) {
		case 'h':
			usage();
			return (0);
		case 'D':
			verbose_flag = 1;
			/* FALLTHROUGH */
		case 'd':
			dontfork_flag = 1;
			break;
		case 'i':
			if (capfile != NULL || dev != NULL) {
				fprintf(stderr, "Packet source already "
				    "specified.\n\n");
				usage();
				exit(1);
			}
			dev = strsep(&optarg, ":");
			if (optarg != NULL) {
				if_index = (u_int16_t) atoi(dev);
				dev = optarg;
			}
			if (verbose_flag)
				fprintf(stderr, "Using %s (idx: %d)\n", dev, if_index);
			break;
		case 'r':
			if (capfile != NULL || dev != NULL) {
				fprintf(stderr, "Packet source already "
				    "specified.\n\n");
				usage();
				exit(1);
			}
			capfile = optarg;
			dontfork_flag = 1;
			ctlsock_path = NULL;
			break;
		case 'T':
			if (strcasecmp(optarg, "full") == 0)
				flowtrack.track_level = TRACK_FULL;
			else if (strcasecmp(optarg, "proto") == 0)
				flowtrack.track_level = TRACK_IP_PROTO;
			else if (strcasecmp(optarg, "ip") == 0)
				flowtrack.track_level = TRACK_IP_ONLY;
			else {
				fprintf(stderr, "Unknown flow tracking "
				    "level\n");
				usage();
				exit(1);
			}
			break;
		case 'L':
			hoplimit = atoi(optarg);
			if (hoplimit < 0 || hoplimit > 255) {
				fprintf(stderr, "Invalid hop limit\n\n");
				usage();
				exit(1);
			}
			break;
		case 'm':
			if ((flowtrack.max_flows = atoi(optarg)) < 0) {
				fprintf(stderr, "Invalid maximum flows\n\n");
				usage();
				exit(1);
			}
			break;
		case 'p':
			pidfile_path = optarg;
			break;
		case 'c':
			if (strcmp(optarg, "none") == 0)
				ctlsock_path = NULL;
			else
				ctlsock_path = optarg;
			break;
		case 's':
			flowtrack.option.sample = atoi(optarg);
			if (flowtrack.option.sample < 2) {
				flowtrack.option.sample = 0;
			}
			break;
		default:
			fprintf(stderr, "Invalid commandline option.\n");
			usage();
			exit(1);
		}
	}

	if (capfile == NULL && dev == NULL) {
		fprintf(stderr, "-i or -r option not specified.\n");
		usage();
		exit(1);
	}
	
	/* join remaining arguments (if any) into bpf program */
	bpf_prog = argv_join(argc - optind, argv + optind);

	/* Will exit on failure */
	setup_packet_capture(&pcap, &linktype, dev, capfile, bpf_prog,
	    always_v6);
	
	/* Control socket */
	if (ctlsock_path != NULL)
		ctlsock = unix_listener(ctlsock_path); /* Will exit on fail */
	
	if (dontfork_flag) {
		loginit(PROGNAME, 1);
	} else {	
		FILE *pidfile;

		r = daemon(0, 0);
		loginit(PROGNAME, 0);

		if ((pidfile = fopen(pidfile_path, "w")) == NULL) {
			fprintf(stderr, "Couldn't open pidfile %s: %s\n",
			    pidfile_path, strerror(errno));
			exit(1);
		}
		fprintf(pidfile, "%u\n", getpid());
		fclose(pidfile);

		signal(SIGINT, sighand_graceful_shutdown);
		signal(SIGTERM, sighand_graceful_shutdown);
		signal(SIGSEGV, sighand_other);

		setprotoent(1);
		drop_privs();
	}

	logit(LOG_NOTICE, "%s v%s starting data collection", 
	    PROGNAME, PROGVER);
	if (dest.ss_family != 0) {
		logit(LOG_NOTICE, "Exporting flows to [%s]:%s",
		    dest_addr, dest_serv);
	}

	/* Main processing loop */
	gettimeofday(&flowtrack.system_boot_time, NULL);
	stop_collection_flag = 0;
	memset(&cb_ctxt, '\0', sizeof(cb_ctxt));
	cb_ctxt.ft = &flowtrack;
	cb_ctxt.linktype = linktype;
	cb_ctxt.want_v6 = always_v6;

	for (r = 0; graceful_shutdown_request == 0; r = 0) {
		/*
		 * Silly libpcap's timeout function doesn't work, so we
		 * do it here (only if we are reading live)
		 */
		if (capfile == NULL) {
			memset(pl, '\0', sizeof(pl));

			/* This can only be set via the control socket */
			if (!stop_collection_flag) {
				pl[0].events = POLLIN|POLLERR|POLLHUP;
				pl[0].fd = pcap_fileno(pcap);
			}
			if (ctlsock != -1) {
				pl[1].fd = ctlsock;
				pl[1].events = POLLIN|POLLERR|POLLHUP;
			}

			r = poll(pl, (ctlsock == -1) ? 1 : 2, 
			    next_expire(&flowtrack));
			if (r == -1 && errno != EINTR) {
				logit(LOG_ERR, "Exiting on poll: %s", 
				    strerror(errno));
				break;
			}
		}

		/* Accept connection on control socket if present */
		if (ctlsock != -1 && pl[1].revents != 0) {
			if (accept_control(ctlsock, &flowtrack, pcap,
			    &exit_request, &stop_collection_flag) != 0)
				break;
		}

		/* If we have data, run it through libpcap */
		if (!stop_collection_flag && 
		    (capfile != NULL || pl[0].revents != 0)) {
			r = pcap_dispatch(pcap, flowtrack.max_flows, flow_cb,
			    (void*)&cb_ctxt);
			if (r == -1) {
				logit(LOG_ERR, "Exiting on pcap_dispatch: %s", 
				    pcap_geterr(pcap));
				break;
			} else if (r == 0 && capfile != NULL) {
				logit(LOG_NOTICE, "Shutting down after "
				    "pcap EOF");
				graceful_shutdown_request = 1;
				break;
			}
		}
		r = 0;

		/* Fatal error from per-packet functions */
		if (cb_ctxt.fatal) {
			logit(LOG_WARNING, "Fatal error - exiting immediately");
			break;
		}

		/*
		 * Expiry processing happens every recheck_rate seconds
		 * or whenever we have exceeded the maximum number of active 
		 * flows
		 */
		if (flowtrack.num_flows > flowtrack.max_flows || 
		    next_expire(&flowtrack) == 0) {
expiry_check:
			/*
			 * If we are reading from a capture file, we never
			 * expire flows based on time - instead we only 
			 * expire flows when the flow table is full. 
			 */
			if (check_expired(&flowtrack, 
			    capfile == NULL ? CE_EXPIRE_NORMAL :
			    CE_EXPIRE_FORCED) < 0)
				logit(LOG_WARNING, "Unable to export flows");
	
			/*
			 * If we are over max_flows, force-expire the oldest 
			 * out first and immediately reprocess to evict them
			 */
			if (flowtrack.num_flows > flowtrack.max_flows) {
				force_expire(&flowtrack,
				    flowtrack.num_flows - flowtrack.max_flows);
				goto expiry_check;
			}
		}

#if 0
		gettimeofday(&now, NULL);
		if( (now.tv_sec - since_last_expire_all.tv_sec) > 5)
		{
			check_expired(&flowtrack, CE_EXPIRE_ALL);
			gettimeofday(&since_last_expire_all, NULL);
			logit(LOG_DEBUG, "CE_EXPIRE_ALL in 5s");
		}
#endif
	}

	/* Flags set by signal handlers or control socket */
	if (graceful_shutdown_request) {
		logit(LOG_WARNING, "Shutting down on user request");
		check_expired(&flowtrack, CE_EXPIRE_ALL);
	} else if (exit_request)
		logit(LOG_WARNING, "Exiting immediately on user request");
	else
		logit(LOG_ERR, "Exiting immediately on internal error");
		
	if (capfile != NULL && dontfork_flag)
		statistics(&flowtrack, stdout, pcap);

	pcap_close(pcap);
	
	unlink(pidfile_path);
	if (ctlsock_path != NULL)
		unlink(ctlsock_path);
	
	return(r == 0 ? 0 : 1);
}
