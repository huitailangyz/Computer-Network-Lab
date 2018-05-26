#include "ip.h"
#include "icmp.h"
#include "packet.h"
#include "arpcache.h"
#include "rtable.h"
#include "arp.h"

#include "mospf_proto.h"
#include "mospf_daemon.h"

#include "log.h"

#include <stdlib.h>
#include <assert.h>
#include <pthread.h>

// initialize ip header 
void ip_init_hdr(struct iphdr *ip, u32 saddr, u32 daddr, u16 len, u8 proto)
{
	ip->version = 4;
	ip->ihl = 5;
	ip->tos = 0;
	ip->tot_len = htons(len);
	ip->id = rand();
	ip->frag_off = htons(IP_DF);
	ip->ttl = DEFAULT_TTL;
	ip->protocol = proto;
	ip->saddr = htonl(saddr);
	ip->daddr = htonl(daddr);
	ip->checksum = ip_checksum(ip);
}

// lookup in the routing table, to find the entry with the same and longest prefix
rt_entry_t *longest_prefix_match(u32 dst)
{
	rt_entry_t *entry = NULL, *selected = NULL;
	pthread_mutex_lock(&rtable_lock);
	list_for_each_entry(entry, &rtable, list) {
		if ((dst & entry->mask) == (entry->dest & entry->mask)) {
			if (!selected || selected->mask < entry->mask)
				selected = entry;
		}
	}
	pthread_mutex_unlock(&rtable_lock);
	return selected;
}

// forward the IP packet from the interface specified by longest_prefix_match, 
// when forwarding the packet, you should check the TTL, update the checksum,
// determine the next hop to forward the packet, then send the packet by 
// iface_send_packet_by_arp
void ip_forward_packet(u32 ip_dst, char *packet, int len)
{
	struct iphdr *ip = packet_to_ip_hdr(packet);
	if (ip->ttl <= 1) {
		icmp_send_packet(packet, len, ICMP_TIME_EXCEEDED, ICMP_EXC_TTL);
		free(packet);
		return ;
	}

	ip->ttl -= 1;
	ip->checksum = ip_checksum(ip);

	rt_entry_t *entry = longest_prefix_match(ip_dst);
	if (!entry) {
		icmp_send_packet(packet, len, ICMP_DEST_UNREACH, ICMP_NET_UNREACH);
		free(packet);
		return ;
	}

	u32 next_hop = get_next_hop(entry, ip_dst);

	iface_info_t *iface = entry->iface;

	iface_send_packet_by_arp(iface, next_hop, packet, len);
}

// handle ip packet
//
// If the packet is ICMP echo request and the destination IP address is equal to
// the IP address of the iface, send ICMP echo reply; otherwise, forward the
// packet.
void handle_ip_packet(iface_info_t *iface, char *packet, int len)
{
	struct iphdr *ip = packet_to_ip_hdr(packet);
	u32 daddr = ntohl(ip->daddr);
	if (daddr == iface->ip) {
		if (ip->protocol == IPPROTO_ICMP) {
			struct icmphdr *icmp = (struct icmphdr *)IP_DATA(ip);
			if (icmp->type == ICMP_ECHOREQUEST) {
				icmp_send_packet(packet, len, ICMP_ECHOREPLY, 0);
			}
		}
		else if (ip->protocol == IPPROTO_MOSPF) {
			handle_mospf_packet(iface, packet, len);
		}

		free(packet);
	}
	else if (ip->daddr == htonl(MOSPF_ALLSPFRouters)) {
		assert(ip->protocol == IPPROTO_MOSPF);
		handle_mospf_packet(iface, packet, len);

		free(packet);
	}
	else {
		ip_forward_packet(daddr, packet, len);
	}
}

// send IP packet
//
// Different from ip_forward_packet, ip_send_packet sends packet generated by
// router itself. This function is used to send ICMP packets.
void ip_send_packet(char *packet, int len)
{
	struct iphdr *ip = packet_to_ip_hdr(packet);
	u32 dst = ntohl(ip->daddr);
	rt_entry_t *entry = longest_prefix_match(dst);
	if (!entry) {
		log(ERROR, "Could not find forwarding rule for IP (dst:"IP_FMT") packet.", 
				HOST_IP_FMT_STR(dst));
		free(packet);
		return ;
	}

	u32 next_hop = get_next_hop(entry, dst);
	iface_info_t *iface = entry->iface;

	iface_send_packet_by_arp(iface, next_hop, packet, len);
}
