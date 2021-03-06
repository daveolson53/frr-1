/* Zebra VTY functions
 * Copyright (C) 2002 Kunihiro Ishiguro
 *
 * This file is part of GNU Zebra.
 *
 * GNU Zebra is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * GNU Zebra is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; see the file COPYING; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <zebra.h>

#include "memory.h"
#include "zebra_memory.h"
#include "if.h"
#include "prefix.h"
#include "command.h"
#include "table.h"
#include "rib.h"
#include "nexthop.h"
#include "vrf.h"
#include "mpls.h"
#include "routemap.h"
#include "srcdest_table.h"
#include "vxlan.h"

#include "zebra/zserv.h"
#include "zebra/zebra_vrf.h"
#include "zebra/zebra_mpls.h"
#include "zebra/zebra_rnh.h"
#include "zebra/redistribute.h"
#include "zebra/zebra_routemap.h"
#include "zebra/zebra_static.h"
#include "lib/json.h"
#include "zebra/zebra_vxlan.h"
#include "zebra/zebra_vty_clippy.c"

extern int allow_delete;

static int do_show_ip_route(struct vty *vty, const char *vrf_name, afi_t afi,
			    safi_t safi, bool use_fib, u_char use_json,
			    route_tag_t tag, struct prefix *longer_prefix_p,
			    bool supernets_only, int type,
			    u_short ospf_instance_id);
static void vty_show_ip_route_detail(struct vty *vty, struct route_node *rn,
				     int mcast);

#define ONE_DAY_SECOND 60*60*24
#define ONE_WEEK_SECOND 60*60*24*7

/* VNI range as per RFC 7432 */
#define CMD_VNI_RANGE "(1-16777215)"

/* General function for static route. */
static int zebra_static_route(struct vty *vty, afi_t afi, safi_t safi,
			      const char *negate, const char *dest_str,
			      const char *mask_str, const char *src_str,
			      const char *gate_str, const char *ifname,
			      const char *flag_str, const char *tag_str,
			      const char *distance_str, const char *vrf_id_str,
			      const char *label_str)
{
	int ret;
	u_char distance;
	struct prefix p, src;
	struct prefix_ipv6 *src_p = NULL;
	union g_addr gate;
	union g_addr *gatep = NULL;
	struct in_addr mask;
	u_char flag = 0;
	route_tag_t tag = 0;
	struct zebra_vrf *zvrf;
	unsigned int ifindex = 0;
	u_char type;
	struct static_nh_label snh_label;

	ret = str2prefix(dest_str, &p);
	if (ret <= 0) {
		vty_out(vty, "%% Malformed address\n");
		return CMD_WARNING_CONFIG_FAILED;
	}

	switch (afi) {
	case AFI_IP:
		/* Cisco like mask notation. */
		if (mask_str) {
			ret = inet_aton(mask_str, &mask);
			if (ret == 0) {
				vty_out(vty, "%% Malformed address\n");
				return CMD_WARNING_CONFIG_FAILED;
			}
			p.prefixlen = ip_masklen(mask);
		}
		break;
	case AFI_IP6:
		/* srcdest routing */
		if (src_str) {
			ret = str2prefix(src_str, &src);
			if (ret <= 0 || src.family != AF_INET6) {
				vty_out(vty, "%% Malformed source address\n");
				return CMD_WARNING_CONFIG_FAILED;
			}
			src_p = (struct prefix_ipv6 *)&src;
		}
		break;
	default:
		break;
	}

	/* Apply mask for given prefix. */
	apply_mask(&p);

	/* Administrative distance. */
	if (distance_str)
		distance = atoi(distance_str);
	else
		distance = ZEBRA_STATIC_DISTANCE_DEFAULT;

	/* tag */
	if (tag_str)
		tag = strtoul(tag_str, NULL, 10);

	/* VRF id */
	zvrf = zebra_vrf_lookup_by_name(vrf_id_str);

	if (!zvrf) {
		vty_out(vty, "%% vrf %s is not defined\n", vrf_id_str);
		return CMD_WARNING_CONFIG_FAILED;
	}

	/* Labels */
	memset(&snh_label, 0, sizeof(struct static_nh_label));
	if (label_str) {
		if (!mpls_enabled) {
			vty_out(vty,
				"%% MPLS not turned on in kernel, ignoring command\n");
			return CMD_WARNING_CONFIG_FAILED;
		}
		int rc = mpls_str2label(label_str, &snh_label.num_labels,
					snh_label.label);
		if (rc < 0) {
			switch (rc) {
			case -1:
				vty_out(vty, "%% Malformed label(s)\n");
				break;
			case -2:
				vty_out(vty,
					"%% Cannot use reserved label(s) (%d-%d)\n",
					MPLS_MIN_RESERVED_LABEL,
					MPLS_MAX_RESERVED_LABEL);
				break;
			case -3:
				vty_out(vty,
					"%% Too many labels. Enter %d or fewer\n",
					MPLS_MAX_LABELS);
				break;
			}
			return CMD_WARNING_CONFIG_FAILED;
		}
	}

	/* Null0 static route.  */
	if ((ifname != NULL)
	    && (strncasecmp(ifname, "Null0", strlen(ifname)) == 0)) {
		if (flag_str) {
			vty_out(vty, "%% can not have flag %s with Null0\n",
				flag_str);
			return CMD_WARNING_CONFIG_FAILED;
		}
		SET_FLAG(flag, ZEBRA_FLAG_BLACKHOLE);
		ifname = NULL;
	}

	/* Route flags */
	if (flag_str) {
		switch (flag_str[0]) {
		case 'r':
		case 'R': /* XXX */
			SET_FLAG(flag, ZEBRA_FLAG_REJECT);
			break;
		case 'b':
		case 'B': /* XXX */
			SET_FLAG(flag, ZEBRA_FLAG_BLACKHOLE);
			break;
		default:
			vty_out(vty, "%% Malformed flag %s \n", flag_str);
			return CMD_WARNING_CONFIG_FAILED;
		}
	}

	if (gate_str) {
		if (inet_pton(afi2family(afi), gate_str, &gate) != 1) {
			vty_out(vty, "%% Malformed nexthop address %s\n",
				gate_str);
			return CMD_WARNING_CONFIG_FAILED;
		}
		gatep = &gate;
	}

	if (ifname) {
		struct interface *ifp;
		ifp = if_lookup_by_name(ifname, zvrf_id(zvrf));
		if (!ifp) {
			vty_out(vty, "%% Malformed Interface name %s\n",
				ifname);
			ifindex = IFINDEX_DELETED;
		} else
			ifindex = ifp->ifindex;
	}

	if (gate_str == NULL && ifname == NULL)
		type = STATIC_BLACKHOLE;
	else if (gate_str && ifname) {
		if (afi == AFI_IP)
			type = STATIC_IPV4_GATEWAY_IFINDEX;
		else
			type = STATIC_IPV6_GATEWAY_IFINDEX;
	} else if (ifname)
		type = STATIC_IFINDEX;
	else {
		if (afi == AFI_IP)
			type = STATIC_IPV4_GATEWAY;
		else
			type = STATIC_IPV6_GATEWAY;
	}

	if (!negate)
		static_add_route(afi, safi, type, &p, src_p, gatep, ifindex,
				 ifname, flag, tag, distance, zvrf, &snh_label);
	else
		static_delete_route(afi, safi, type, &p, src_p, gatep, ifindex,
				    tag, distance, zvrf, &snh_label);

	return CMD_SUCCESS;
}

/* Static unicast routes for multicast RPF lookup. */
DEFPY (ip_mroute_dist,
       ip_mroute_dist_cmd,
       "[no] ip mroute A.B.C.D/M$prefix <A.B.C.D$gate|INTERFACE$ifname> [(1-255)$distance]",
       NO_STR
       IP_STR
       "Configure static unicast route into MRIB for multicast RPF lookup\n"
       "IP destination prefix (e.g. 10.0.0.0/8)\n"
       "Nexthop address\n"
       "Nexthop interface name\n"
       "Distance\n")
{
	return zebra_static_route(vty, AFI_IP, SAFI_MULTICAST, no, prefix_str,
				  NULL, NULL, gate_str, ifname, NULL, NULL,
				  distance_str, NULL, NULL);
}

DEFUN (ip_multicast_mode,
       ip_multicast_mode_cmd,
       "ip multicast rpf-lookup-mode <urib-only|mrib-only|mrib-then-urib|lower-distance|longer-prefix>",
       IP_STR
       "Multicast options\n"
       "RPF lookup behavior\n"
       "Lookup in unicast RIB only\n"
       "Lookup in multicast RIB only\n"
       "Try multicast RIB first, fall back to unicast RIB\n"
       "Lookup both, use entry with lower distance\n"
       "Lookup both, use entry with longer prefix\n")
{
	char *mode = argv[3]->text;

	if (strmatch(mode, "urib-only"))
		multicast_mode_ipv4_set(MCAST_URIB_ONLY);
	else if (strmatch(mode, "mrib-only"))
		multicast_mode_ipv4_set(MCAST_MRIB_ONLY);
	else if (strmatch(mode, "mrib-then-urib"))
		multicast_mode_ipv4_set(MCAST_MIX_MRIB_FIRST);
	else if (strmatch(mode, "lower-distance"))
		multicast_mode_ipv4_set(MCAST_MIX_DISTANCE);
	else if (strmatch(mode, "longer-prefix"))
		multicast_mode_ipv4_set(MCAST_MIX_PFXLEN);
	else {
		vty_out(vty, "Invalid mode specified\n");
		return CMD_WARNING_CONFIG_FAILED;
	}

	return CMD_SUCCESS;
}

DEFUN (no_ip_multicast_mode,
       no_ip_multicast_mode_cmd,
       "no ip multicast rpf-lookup-mode [<urib-only|mrib-only|mrib-then-urib|lower-distance|longer-prefix>]",
       NO_STR
       IP_STR
       "Multicast options\n"
       "RPF lookup behavior\n"
       "Lookup in unicast RIB only\n"
       "Lookup in multicast RIB only\n"
       "Try multicast RIB first, fall back to unicast RIB\n"
       "Lookup both, use entry with lower distance\n"
       "Lookup both, use entry with longer prefix\n")
{
	multicast_mode_ipv4_set(MCAST_NO_CONFIG);
	return CMD_SUCCESS;
}


DEFUN (show_ip_rpf,
       show_ip_rpf_cmd,
       "show ip rpf [json]",
       SHOW_STR
       IP_STR
       "Display RPF information for multicast source\n"
       JSON_STR)
{
	int uj = use_json(argc, argv);
	return do_show_ip_route(vty, VRF_DEFAULT_NAME, AFI_IP, SAFI_MULTICAST,
				false, uj, 0, NULL, false, -1, 0);
}

DEFUN (show_ip_rpf_addr,
       show_ip_rpf_addr_cmd,
       "show ip rpf A.B.C.D",
       SHOW_STR
       IP_STR
       "Display RPF information for multicast source\n"
       "IP multicast source address (e.g. 10.0.0.0)\n")
{
	int idx_ipv4 = 3;
	struct in_addr addr;
	struct route_node *rn;
	struct route_entry *re;
	int ret;

	ret = inet_aton(argv[idx_ipv4]->arg, &addr);
	if (ret == 0) {
		vty_out(vty, "%% Malformed address\n");
		return CMD_WARNING;
	}

	re = rib_match_ipv4_multicast(VRF_DEFAULT, addr, &rn);

	if (re)
		vty_show_ip_route_detail(vty, rn, 1);
	else
		vty_out(vty, "%% No match for RPF lookup\n");

	return CMD_SUCCESS;
}

/* Static route configuration.  */
DEFPY (ip_route,
       ip_route_cmd,
       "[no] ip route\
          <A.B.C.D/M$prefix|A.B.C.D$prefix A.B.C.D$mask>\
          <\
            {A.B.C.D$gate|INTERFACE$ifname}\
            |null0$ifname\
            |<reject|blackhole>$flag\
          >\
          [{\
            tag (1-4294967295)\
            |(1-255)$distance\
            |vrf NAME\
            |label WORD\
          }]",
       NO_STR
       IP_STR
       "Establish static routes\n"
       "IP destination prefix (e.g. 10.0.0.0/8)\n"
       "IP destination prefix\n"
       "IP destination prefix mask\n"
       "IP gateway address\n"
       "IP gateway interface name\n"
       "Null interface\n"
       "Emit an ICMP unreachable when matched\n"
       "Silently discard pkts when matched\n"
       "Set tag for this route\n"
       "Tag value\n"
       "Distance value for this route\n"
       VRF_CMD_HELP_STR
       MPLS_LABEL_HELPSTR)
{
	return zebra_static_route(vty, AFI_IP, SAFI_UNICAST, no, prefix,
				  mask_str, NULL, gate_str, ifname, flag,
				  tag_str, distance_str, vrf, label);
	return 0;
}

/* New RIB.  Detailed information for IPv4 route. */
static void vty_show_ip_route_detail(struct vty *vty, struct route_node *rn,
				     int mcast)
{
	struct route_entry *re;
	struct nexthop *nexthop;
	char buf[SRCDEST2STR_BUFFER];
	struct zebra_vrf *zvrf;

	RNODE_FOREACH_RE(rn, re)
	{
		const char *mcast_info = "";
		if (mcast) {
			rib_table_info_t *info = srcdest_rnode_table_info(rn);
			mcast_info = (info->safi == SAFI_MULTICAST)
					     ? " using Multicast RIB"
					     : " using Unicast RIB";
		}

		vty_out(vty, "Routing entry for %s%s\n",
			srcdest_rnode2str(rn, buf, sizeof(buf)), mcast_info);
		vty_out(vty, "  Known via \"%s", zebra_route_string(re->type));
		if (re->instance)
			vty_out(vty, "[%d]", re->instance);
		vty_out(vty, "\"");
		vty_out(vty, ", distance %u, metric %u", re->distance,
			re->metric);
		if (re->tag)
			vty_out(vty, ", tag %d", re->tag);
		if (re->mtu)
			vty_out(vty, ", mtu %u", re->mtu);
		if (re->vrf_id != VRF_DEFAULT) {
			zvrf = vrf_info_lookup(re->vrf_id);
			vty_out(vty, ", vrf %s", zvrf_name(zvrf));
		}
		if (CHECK_FLAG(re->flags, ZEBRA_FLAG_SELECTED))
			vty_out(vty, ", best");
		if (re->refcnt)
			vty_out(vty, ", refcnt %ld", re->refcnt);
		if (CHECK_FLAG(re->flags, ZEBRA_FLAG_BLACKHOLE))
			vty_out(vty, ", blackhole");
		if (CHECK_FLAG(re->flags, ZEBRA_FLAG_REJECT))
			vty_out(vty, ", reject");
		vty_out(vty, "\n");

		if (re->type == ZEBRA_ROUTE_RIP || re->type == ZEBRA_ROUTE_OSPF
		    || re->type == ZEBRA_ROUTE_ISIS
		    || re->type == ZEBRA_ROUTE_NHRP
		    || re->type == ZEBRA_ROUTE_TABLE
		    || re->type == ZEBRA_ROUTE_BGP) {
			time_t uptime;
			struct tm *tm;

			uptime = time(NULL);
			uptime -= re->uptime;
			tm = gmtime(&uptime);

			vty_out(vty, "  Last update ");

			if (uptime < ONE_DAY_SECOND)
				vty_out(vty, "%02d:%02d:%02d", tm->tm_hour,
					tm->tm_min, tm->tm_sec);
			else if (uptime < ONE_WEEK_SECOND)
				vty_out(vty, "%dd%02dh%02dm", tm->tm_yday,
					tm->tm_hour, tm->tm_min);
			else
				vty_out(vty, "%02dw%dd%02dh", tm->tm_yday / 7,
					tm->tm_yday - ((tm->tm_yday / 7) * 7),
					tm->tm_hour);
			vty_out(vty, " ago\n");
		}

		for (ALL_NEXTHOPS(re->nexthop, nexthop)) {
			char addrstr[32];

			vty_out(vty, "  %c%s",
				CHECK_FLAG(nexthop->flags, NEXTHOP_FLAG_FIB)
					? '*'
					: ' ',
				nexthop->rparent ? "  " : "");

			switch (nexthop->type) {
			case NEXTHOP_TYPE_IPV4:
			case NEXTHOP_TYPE_IPV4_IFINDEX:
				vty_out(vty, " %s",
					inet_ntoa(nexthop->gate.ipv4));
				if (nexthop->ifindex)
					vty_out(vty, ", via %s",
						ifindex2ifname(nexthop->ifindex,
							       re->vrf_id));
				break;
			case NEXTHOP_TYPE_IPV6:
			case NEXTHOP_TYPE_IPV6_IFINDEX:
				vty_out(vty, " %s",
					inet_ntop(AF_INET6, &nexthop->gate.ipv6,
						  buf, sizeof buf));
				if (nexthop->ifindex)
					vty_out(vty, ", via %s",
						ifindex2ifname(nexthop->ifindex,
							       re->vrf_id));
				break;
			case NEXTHOP_TYPE_IFINDEX:
				vty_out(vty, " directly connected, %s",
					ifindex2ifname(nexthop->ifindex,
						       re->vrf_id));
				break;
			case NEXTHOP_TYPE_BLACKHOLE:
				vty_out(vty, " directly connected, Null0");
				break;
			default:
				break;
			}
			if (!CHECK_FLAG(nexthop->flags, NEXTHOP_FLAG_ACTIVE))
				vty_out(vty, " inactive");

			if (CHECK_FLAG(nexthop->flags, NEXTHOP_FLAG_ONLINK))
				vty_out(vty, " onlink");

			if (CHECK_FLAG(nexthop->flags, NEXTHOP_FLAG_RECURSIVE))
				vty_out(vty, " (recursive)");

			switch (nexthop->type) {
			case NEXTHOP_TYPE_IPV4:
			case NEXTHOP_TYPE_IPV4_IFINDEX:
				if (nexthop->src.ipv4.s_addr) {
					if (inet_ntop(AF_INET,
						      &nexthop->src.ipv4,
						      addrstr, sizeof addrstr))
						vty_out(vty, ", src %s",
							addrstr);
				}
				break;
			case NEXTHOP_TYPE_IPV6:
			case NEXTHOP_TYPE_IPV6_IFINDEX:
				if (!IPV6_ADDR_SAME(&nexthop->src.ipv6,
						    &in6addr_any)) {
					if (inet_ntop(AF_INET6,
						      &nexthop->src.ipv6,
						      addrstr, sizeof addrstr))
						vty_out(vty, ", src %s",
							addrstr);
				}
				break;
			default:
				break;
			}

			/* Label information */
			if (nexthop->nh_label
			    && nexthop->nh_label->num_labels) {
				vty_out(vty, ", label %s",
					mpls_label2str(
						nexthop->nh_label->num_labels,
						nexthop->nh_label->label, buf,
						sizeof buf, 1));
			}

			vty_out(vty, "\n");
		}
		vty_out(vty, "\n");
	}
}

static void vty_show_ip_route(struct vty *vty, struct route_node *rn,
			      struct route_entry *re, json_object *json)
{
	struct nexthop *nexthop;
	int len = 0;
	char buf[SRCDEST2STR_BUFFER];
	json_object *json_nexthops = NULL;
	json_object *json_nexthop = NULL;
	json_object *json_route = NULL;
	json_object *json_labels = NULL;

	if (json) {
		json_route = json_object_new_object();
		json_nexthops = json_object_new_array();

		json_object_string_add(json_route, "prefix",
				       srcdest_rnode2str(rn, buf, sizeof buf));
		json_object_string_add(json_route, "protocol",
				       zebra_route_string(re->type));

		if (re->instance)
			json_object_int_add(json_route, "instance",
					    re->instance);

		if (re->vrf_id)
			json_object_int_add(json_route, "vrfId", re->vrf_id);

		if (CHECK_FLAG(re->flags, ZEBRA_FLAG_SELECTED))
			json_object_boolean_true_add(json_route, "selected");

		if (re->type != ZEBRA_ROUTE_CONNECT
		    && re->type != ZEBRA_ROUTE_KERNEL) {
			json_object_int_add(json_route, "distance",
					    re->distance);
			json_object_int_add(json_route, "metric", re->metric);
		}

		if (CHECK_FLAG(re->flags, ZEBRA_FLAG_BLACKHOLE))
			json_object_boolean_true_add(json_route, "blackhole");

		if (CHECK_FLAG(re->flags, ZEBRA_FLAG_REJECT))
			json_object_boolean_true_add(json_route, "reject");

		if (re->type == ZEBRA_ROUTE_RIP || re->type == ZEBRA_ROUTE_OSPF
		    || re->type == ZEBRA_ROUTE_ISIS
		    || re->type == ZEBRA_ROUTE_NHRP
		    || re->type == ZEBRA_ROUTE_TABLE
		    || re->type == ZEBRA_ROUTE_BGP) {
			time_t uptime;
			struct tm *tm;

			uptime = time(NULL);
			uptime -= re->uptime;
			tm = gmtime(&uptime);

			if (uptime < ONE_DAY_SECOND)
				sprintf(buf, "%02d:%02d:%02d", tm->tm_hour,
					tm->tm_min, tm->tm_sec);
			else if (uptime < ONE_WEEK_SECOND)
				sprintf(buf, "%dd%02dh%02dm", tm->tm_yday,
					tm->tm_hour, tm->tm_min);
			else
				sprintf(buf, "%02dw%dd%02dh", tm->tm_yday / 7,
					tm->tm_yday - ((tm->tm_yday / 7) * 7),
					tm->tm_hour);

			json_object_string_add(json_route, "uptime", buf);
		}

		for (ALL_NEXTHOPS(re->nexthop, nexthop)) {
			json_nexthop = json_object_new_object();

			if (CHECK_FLAG(nexthop->flags, NEXTHOP_FLAG_FIB))
				json_object_boolean_true_add(json_nexthop,
							     "fib");

			switch (nexthop->type) {
			case NEXTHOP_TYPE_IPV4:
			case NEXTHOP_TYPE_IPV4_IFINDEX:
				json_object_string_add(
					json_nexthop, "ip",
					inet_ntoa(nexthop->gate.ipv4));
				json_object_string_add(json_nexthop, "afi",
						       "ipv4");

				if (nexthop->ifindex) {
					json_object_int_add(json_nexthop,
							    "interfaceIndex",
							    nexthop->ifindex);
					json_object_string_add(
						json_nexthop, "interfaceName",
						ifindex2ifname(nexthop->ifindex,
							       re->vrf_id));
				}
				break;
			case NEXTHOP_TYPE_IPV6:
			case NEXTHOP_TYPE_IPV6_IFINDEX:
				json_object_string_add(
					json_nexthop, "ip",
					inet_ntop(AF_INET6, &nexthop->gate.ipv6,
						  buf, sizeof buf));
				json_object_string_add(json_nexthop, "afi",
						       "ipv6");

				if (nexthop->ifindex) {
					json_object_int_add(json_nexthop,
							    "interfaceIndex",
							    nexthop->ifindex);
					json_object_string_add(
						json_nexthop, "interfaceName",
						ifindex2ifname(nexthop->ifindex,
							       re->vrf_id));
				}
				break;

			case NEXTHOP_TYPE_IFINDEX:
				json_object_boolean_true_add(
					json_nexthop, "directlyConnected");
				json_object_int_add(json_nexthop,
						    "interfaceIndex",
						    nexthop->ifindex);
				json_object_string_add(
					json_nexthop, "interfaceName",
					ifindex2ifname(nexthop->ifindex,
						       re->vrf_id));
				break;
			case NEXTHOP_TYPE_BLACKHOLE:
				json_object_boolean_true_add(json_nexthop,
							     "blackhole");
				break;
			default:
				break;
			}

			if (CHECK_FLAG(nexthop->flags, NEXTHOP_FLAG_ACTIVE))
				json_object_boolean_true_add(json_nexthop,
							     "active");

			if (CHECK_FLAG(nexthop->flags, NEXTHOP_FLAG_ONLINK))
				json_object_boolean_true_add(json_nexthop,
							     "onLink");

			if (CHECK_FLAG(nexthop->flags, NEXTHOP_FLAG_RECURSIVE))
				json_object_boolean_true_add(json_nexthop,
							     "recursive");

			switch (nexthop->type) {
			case NEXTHOP_TYPE_IPV4:
			case NEXTHOP_TYPE_IPV4_IFINDEX:
				if (nexthop->src.ipv4.s_addr) {
					if (inet_ntop(AF_INET,
						      &nexthop->src.ipv4, buf,
						      sizeof buf))
						json_object_string_add(
							json_nexthop, "source",
							buf);
				}
				break;
			case NEXTHOP_TYPE_IPV6:
			case NEXTHOP_TYPE_IPV6_IFINDEX:
				if (!IPV6_ADDR_SAME(&nexthop->src.ipv6,
						    &in6addr_any)) {
					if (inet_ntop(AF_INET6,
						      &nexthop->src.ipv6, buf,
						      sizeof buf))
						json_object_string_add(
							json_nexthop, "source",
							buf);
				}
				break;
			default:
				break;
			}

			if (nexthop->nh_label
			    && nexthop->nh_label->num_labels) {
				json_labels = json_object_new_array();

				for (int label_index = 0;
				     label_index
				     < nexthop->nh_label->num_labels;
				     label_index++)
					json_object_array_add(
						json_labels,
						json_object_new_int(
							nexthop->nh_label->label
								[label_index]));

				json_object_object_add(json_nexthop, "labels",
						       json_labels);
			}

			json_object_array_add(json_nexthops, json_nexthop);
		}

		json_object_object_add(json_route, "nexthops", json_nexthops);
		json_object_array_add(json, json_route);
		return;
	}

	/* Nexthop information. */
	for (ALL_NEXTHOPS(re->nexthop, nexthop)) {
		if (nexthop == re->nexthop) {
			/* Prefix information. */
			len = vty_out(vty, "%c", zebra_route_char(re->type));
			if (re->instance)
				len += vty_out(vty, "[%d]", re->instance);
			len += vty_out(
				vty, "%c%c %s",
				CHECK_FLAG(re->flags, ZEBRA_FLAG_SELECTED)
					? '>'
					: ' ',
				CHECK_FLAG(nexthop->flags, NEXTHOP_FLAG_FIB)
					? '*'
					: ' ',
				srcdest_rnode2str(rn, buf, sizeof buf));

			/* Distance and metric display. */
			if (re->type != ZEBRA_ROUTE_CONNECT
			    && re->type != ZEBRA_ROUTE_KERNEL)
				len += vty_out(vty, " [%d/%d]", re->distance,
					       re->metric);
		} else
			vty_out(vty, "  %c%*c",
				CHECK_FLAG(nexthop->flags, NEXTHOP_FLAG_FIB)
					? '*'
					: ' ',
				len - 3 + (2 * nexthop_level(nexthop)), ' ');

		switch (nexthop->type) {
		case NEXTHOP_TYPE_IPV4:
		case NEXTHOP_TYPE_IPV4_IFINDEX:
			vty_out(vty, " via %s", inet_ntoa(nexthop->gate.ipv4));
			if (nexthop->ifindex)
				vty_out(vty, ", %s",
					ifindex2ifname(nexthop->ifindex,
						       re->vrf_id));
			break;
		case NEXTHOP_TYPE_IPV6:
		case NEXTHOP_TYPE_IPV6_IFINDEX:
			vty_out(vty, " via %s",
				inet_ntop(AF_INET6, &nexthop->gate.ipv6, buf,
					  sizeof buf));
			if (nexthop->ifindex)
				vty_out(vty, ", %s",
					ifindex2ifname(nexthop->ifindex,
						       re->vrf_id));
			break;

		case NEXTHOP_TYPE_IFINDEX:
			vty_out(vty, " is directly connected, %s",
				ifindex2ifname(nexthop->ifindex, re->vrf_id));
			break;
		case NEXTHOP_TYPE_BLACKHOLE:
			vty_out(vty, " is directly connected, Null0");
			break;
		default:
			break;
		}
		if (!CHECK_FLAG(nexthop->flags, NEXTHOP_FLAG_ACTIVE))
			vty_out(vty, " inactive");

		if (CHECK_FLAG(nexthop->flags, NEXTHOP_FLAG_ONLINK))
			vty_out(vty, " onlink");

		if (CHECK_FLAG(nexthop->flags, NEXTHOP_FLAG_RECURSIVE))
			vty_out(vty, " (recursive)");

		switch (nexthop->type) {
		case NEXTHOP_TYPE_IPV4:
		case NEXTHOP_TYPE_IPV4_IFINDEX:
			if (nexthop->src.ipv4.s_addr) {
				if (inet_ntop(AF_INET, &nexthop->src.ipv4, buf,
					      sizeof buf))
					vty_out(vty, ", src %s", buf);
			}
			break;
		case NEXTHOP_TYPE_IPV6:
		case NEXTHOP_TYPE_IPV6_IFINDEX:
			if (!IPV6_ADDR_SAME(&nexthop->src.ipv6, &in6addr_any)) {
				if (inet_ntop(AF_INET6, &nexthop->src.ipv6, buf,
					      sizeof buf))
					vty_out(vty, ", src %s", buf);
			}
			break;
		default:
			break;
		}

		/* Label information */
		if (nexthop->nh_label && nexthop->nh_label->num_labels) {
			vty_out(vty, ", label %s",
				mpls_label2str(nexthop->nh_label->num_labels,
					       nexthop->nh_label->label, buf,
					       sizeof buf, 1));
		}

		if (CHECK_FLAG(re->flags, ZEBRA_FLAG_BLACKHOLE))
			vty_out(vty, ", bh");
		if (CHECK_FLAG(re->flags, ZEBRA_FLAG_REJECT))
			vty_out(vty, ", rej");

		if (re->type == ZEBRA_ROUTE_RIP || re->type == ZEBRA_ROUTE_OSPF
		    || re->type == ZEBRA_ROUTE_ISIS
		    || re->type == ZEBRA_ROUTE_NHRP
		    || re->type == ZEBRA_ROUTE_TABLE
		    || re->type == ZEBRA_ROUTE_BGP) {
			time_t uptime;
			struct tm *tm;

			uptime = time(NULL);
			uptime -= re->uptime;
			tm = gmtime(&uptime);

			if (uptime < ONE_DAY_SECOND)
				vty_out(vty, ", %02d:%02d:%02d", tm->tm_hour,
					tm->tm_min, tm->tm_sec);
			else if (uptime < ONE_WEEK_SECOND)
				vty_out(vty, ", %dd%02dh%02dm", tm->tm_yday,
					tm->tm_hour, tm->tm_min);
			else
				vty_out(vty, ", %02dw%dd%02dh", tm->tm_yday / 7,
					tm->tm_yday - ((tm->tm_yday / 7) * 7),
					tm->tm_hour);
		}
		vty_out(vty, "\n");
	}
}

static bool use_fib(struct cmd_token *token)
{
	return strncmp(token->arg, "route", strlen(token->arg));
}

static int do_show_ip_route(struct vty *vty, const char *vrf_name, afi_t afi,
			    safi_t safi, bool use_fib, u_char use_json,
			    route_tag_t tag, struct prefix *longer_prefix_p,
			    bool supernets_only, int type,
			    u_short ospf_instance_id)
{
	struct route_table *table;
	struct route_node *rn;
	struct route_entry *re;
	int first = 1;
	struct zebra_vrf *zvrf = NULL;
	char buf[BUFSIZ];
	json_object *json = NULL;
	json_object *json_prefix = NULL;
	u_int32_t addr;

	if (!(zvrf = zebra_vrf_lookup_by_name(vrf_name))) {
		if (use_json)
			vty_out(vty, "{}\n");
		else
			vty_out(vty, "vrf %s not defined\n", vrf_name);
		return CMD_SUCCESS;
	}

	if (zvrf_id(zvrf) == VRF_UNKNOWN) {
		if (use_json)
			vty_out(vty, "{}\n");
		else
			vty_out(vty, "vrf %s inactive\n", vrf_name);
		return CMD_SUCCESS;
	}

	table = zebra_vrf_table(afi, safi, zvrf_id(zvrf));
	if (!table) {
		if (use_json)
			vty_out(vty, "{}\n");
		return CMD_SUCCESS;
	}

	if (use_json)
		json = json_object_new_object();

	/* Show all routes. */
	for (rn = route_top(table); rn; rn = route_next(rn)) {
		RNODE_FOREACH_RE(rn, re)
		{
			if (use_fib
			    && !CHECK_FLAG(re->status,
					   ROUTE_ENTRY_SELECTED_FIB))
				continue;

			if (tag && re->tag != tag)
				continue;

			if (longer_prefix_p
			    && !prefix_match(longer_prefix_p, &rn->p))
				continue;

			/* This can only be true when the afi is IPv4 */
			if (supernets_only) {
				addr = ntohl(rn->p.u.prefix4.s_addr);

				if (IN_CLASSC(addr) && rn->p.prefixlen >= 24)
					continue;

				if (IN_CLASSB(addr) && rn->p.prefixlen >= 16)
					continue;

				if (IN_CLASSA(addr) && rn->p.prefixlen >= 8)
					continue;
			}

			if (type && re->type != type)
				continue;

			if (ospf_instance_id
			    && (re->type != ZEBRA_ROUTE_OSPF
				|| re->instance != ospf_instance_id))
				continue;

			if (use_json) {
				if (!json_prefix)
					json_prefix = json_object_new_array();
			} else {
				if (first) {
					if (afi == AFI_IP)
						vty_out(vty,
							SHOW_ROUTE_V4_HEADER);
					else
						vty_out(vty,
							SHOW_ROUTE_V6_HEADER);

					if (zvrf_id(zvrf) != VRF_DEFAULT)
						vty_out(vty, "\nVRF %s:\n",
							zvrf_name(zvrf));

					first = 0;
				}
			}

			vty_show_ip_route(vty, rn, re, json_prefix);
		}

		if (json_prefix) {
			prefix2str(&rn->p, buf, sizeof buf);
			json_object_object_add(json, buf, json_prefix);
			json_prefix = NULL;
		}
	}

	if (use_json) {
		vty_out(vty, "%s\n", json_object_to_json_string_ext(
					     json, JSON_C_TO_STRING_PRETTY));
		json_object_free(json);
	}

	return CMD_SUCCESS;
}

DEFUN (show_ip_nht,
       show_ip_nht_cmd,
       "show ip nht [vrf NAME]",
       SHOW_STR
       IP_STR
       "IP nexthop tracking table\n"
       VRF_CMD_HELP_STR)
{
	int idx_vrf = 4;
	vrf_id_t vrf_id = VRF_DEFAULT;

	if (argc == 5)
		VRF_GET_ID(vrf_id, argv[idx_vrf]->arg);

	zebra_print_rnh_table(vrf_id, AF_INET, vty, RNH_NEXTHOP_TYPE);
	return CMD_SUCCESS;
}


DEFUN (show_ip_nht_vrf_all,
       show_ip_nht_vrf_all_cmd,
       "show ip nht vrf all",
       SHOW_STR
       IP_STR
       "IP nexthop tracking table\n"
       VRF_ALL_CMD_HELP_STR)
{
	struct vrf *vrf;
	struct zebra_vrf *zvrf;

	RB_FOREACH(vrf, vrf_name_head, &vrfs_by_name)
	if ((zvrf = vrf->info) != NULL) {
		vty_out(vty, "\nVRF %s:\n", zvrf_name(zvrf));
		zebra_print_rnh_table(zvrf_id(zvrf), AF_INET, vty,
				      RNH_NEXTHOP_TYPE);
	}

	return CMD_SUCCESS;
}

DEFUN (show_ipv6_nht,
       show_ipv6_nht_cmd,
       "show ipv6 nht [vrf NAME]",
       SHOW_STR
       IPV6_STR
       "IPv6 nexthop tracking table\n"
       VRF_CMD_HELP_STR)
{
	int idx_vrf = 4;
	vrf_id_t vrf_id = VRF_DEFAULT;

	if (argc == 5)
		VRF_GET_ID(vrf_id, argv[idx_vrf]->arg);

	zebra_print_rnh_table(vrf_id, AF_INET6, vty, RNH_NEXTHOP_TYPE);
	return CMD_SUCCESS;
}


DEFUN (show_ipv6_nht_vrf_all,
       show_ipv6_nht_vrf_all_cmd,
       "show ipv6 nht vrf all",
       SHOW_STR
       IP_STR
       "IPv6 nexthop tracking table\n"
       VRF_ALL_CMD_HELP_STR)
{
	struct vrf *vrf;
	struct zebra_vrf *zvrf;

	RB_FOREACH(vrf, vrf_name_head, &vrfs_by_name)
	if ((zvrf = vrf->info) != NULL) {
		vty_out(vty, "\nVRF %s:\n", zvrf_name(zvrf));
		zebra_print_rnh_table(zvrf_id(zvrf), AF_INET6, vty,
				      RNH_NEXTHOP_TYPE);
	}

	return CMD_SUCCESS;
}

DEFUN (ip_nht_default_route,
       ip_nht_default_route_cmd,
       "ip nht resolve-via-default",
       IP_STR
       "Filter Next Hop tracking route resolution\n"
       "Resolve via default route\n")
{
	if (zebra_rnh_ip_default_route)
		return CMD_SUCCESS;

	zebra_rnh_ip_default_route = 1;
	zebra_evaluate_rnh(0, AF_INET, 1, RNH_NEXTHOP_TYPE, NULL);
	return CMD_SUCCESS;
}

DEFUN (no_ip_nht_default_route,
       no_ip_nht_default_route_cmd,
       "no ip nht resolve-via-default",
       NO_STR
       IP_STR
       "Filter Next Hop tracking route resolution\n"
       "Resolve via default route\n")
{
	if (!zebra_rnh_ip_default_route)
		return CMD_SUCCESS;

	zebra_rnh_ip_default_route = 0;
	zebra_evaluate_rnh(0, AF_INET, 1, RNH_NEXTHOP_TYPE, NULL);
	return CMD_SUCCESS;
}

DEFUN (ipv6_nht_default_route,
       ipv6_nht_default_route_cmd,
       "ipv6 nht resolve-via-default",
       IP6_STR
       "Filter Next Hop tracking route resolution\n"
       "Resolve via default route\n")
{
	if (zebra_rnh_ipv6_default_route)
		return CMD_SUCCESS;

	zebra_rnh_ipv6_default_route = 1;
	zebra_evaluate_rnh(0, AF_INET6, 1, RNH_NEXTHOP_TYPE, NULL);
	return CMD_SUCCESS;
}

DEFUN (no_ipv6_nht_default_route,
       no_ipv6_nht_default_route_cmd,
       "no ipv6 nht resolve-via-default",
       NO_STR
       IP6_STR
       "Filter Next Hop tracking route resolution\n"
       "Resolve via default route\n")
{
	if (!zebra_rnh_ipv6_default_route)
		return CMD_SUCCESS;

	zebra_rnh_ipv6_default_route = 0;
	zebra_evaluate_rnh(0, AF_INET6, 1, RNH_NEXTHOP_TYPE, NULL);
	return CMD_SUCCESS;
}

DEFUN (show_ip_route,
       show_ip_route_cmd,
       "show ip <fib|route> [vrf NAME] [tag (1-4294967295)|A.B.C.D/M longer-prefixes|supernets-only|" FRR_IP_REDIST_STR_ZEBRA "|ospf (1-65535)] [json]",
       SHOW_STR
       IP_STR
       "IP forwarding table\n"
       "IP routing table\n"
       VRF_CMD_HELP_STR
       "Show only routes with tag\n"
       "Tag value\n"
       "IP prefix <network>/<length>, e.g., 35.0.0.0/8\n"
       "Show route matching the specified Network/Mask pair only\n"
       "Show supernet entries only\n"
       FRR_IP_REDIST_HELP_STR_ZEBRA
       "Open Shortest Path First (OSPFv2)\n"
       "Instance ID\n"
       JSON_STR)
{
	bool uf = use_fib(argv[2]);
	struct route_table *table;
	int vrf_all = 0;
	route_tag_t tag = 0;
	vrf_id_t vrf_id = VRF_DEFAULT;
	struct vrf *vrf;
	struct zebra_vrf *zvrf;
	int uj = use_json(argc, argv);
	int idx = 0;
	struct prefix p;
	bool longer_prefixes = false;
	bool supernets_only = false;
	int type = 0;
	u_short ospf_instance_id = 0;

	if (argv_find(argv, argc, "vrf", &idx)) {
		if (strmatch(argv[idx + 1]->arg, "all"))
			vrf_all = 1;
		else
			VRF_GET_ID(vrf_id, argv[idx + 1]->arg);
	}

	if (argv_find(argv, argc, "tag", &idx))
		tag = strtoul(argv[idx + 1]->arg, NULL, 10);

	else if (argv_find(argv, argc, "A.B.C.D/M", &idx)) {
		str2prefix(argv[idx]->arg, &p);
		longer_prefixes = true;
	}

	else if (argv_find(argv, argc, "supernets_only", &idx))
		supernets_only = true;

	else {
		if (argv_find(argv, argc, "kernel", &idx))
			type = proto_redistnum(AFI_IP, argv[idx]->text);
		else if (argv_find(argv, argc, "babel", &idx))
			type = proto_redistnum(AFI_IP, argv[idx]->text);
		else if (argv_find(argv, argc, "connected", &idx))
			type = proto_redistnum(AFI_IP, argv[idx]->text);
		else if (argv_find(argv, argc, "static", &idx))
			type = proto_redistnum(AFI_IP, argv[idx]->text);
		else if (argv_find(argv, argc, "rip", &idx))
			type = proto_redistnum(AFI_IP, argv[idx]->text);
		else if (argv_find(argv, argc, "ospf", &idx))
			type = proto_redistnum(AFI_IP, argv[idx]->text);
		else if (argv_find(argv, argc, "isis", &idx))
			type = proto_redistnum(AFI_IP, argv[idx]->text);
		else if (argv_find(argv, argc, "bgp", &idx))
			type = proto_redistnum(AFI_IP, argv[idx]->text);
		else if (argv_find(argv, argc, "pim", &idx))
			type = proto_redistnum(AFI_IP, argv[idx]->text);
		else if (argv_find(argv, argc, "eigrp", &idx))
			type = proto_redistnum(AFI_IP, argv[idx]->text);
		else if (argv_find(argv, argc, "nhrp", &idx))
			type = proto_redistnum(AFI_IP, argv[idx]->text);
		else if (argv_find(argv, argc, "table", &idx))
			type = proto_redistnum(AFI_IP, argv[idx]->text);
		else if (argv_find(argv, argc, "vnc", &idx))
			type = proto_redistnum(AFI_IP, argv[idx]->text);

		if (argv_find(argv, argc, "(1-65535)", &idx))
			ospf_instance_id = strtoul(argv[idx]->arg, NULL, 10);

		if (type < 0) {
			vty_out(vty, "Unknown route type\n");
			return CMD_WARNING;
		}
	}

	if (vrf_all) {
		RB_FOREACH(vrf, vrf_name_head, &vrfs_by_name)
		{
			if ((zvrf = vrf->info) == NULL
			    || (table = zvrf->table[AFI_IP][SAFI_UNICAST])
				       == NULL)
				continue;

			do_show_ip_route(
				vty, zvrf_name(zvrf), AFI_IP, SAFI_UNICAST, uf,
				uj, tag, longer_prefixes ? &p : NULL,
				supernets_only, type, ospf_instance_id);
		}
	} else {
		vrf = vrf_lookup_by_id(vrf_id);
		do_show_ip_route(vty, vrf->name, AFI_IP, SAFI_UNICAST, uf, uj,
				 tag, longer_prefixes ? &p : NULL,
				 supernets_only, type, ospf_instance_id);
	}
	return CMD_SUCCESS;
}

DEFUN (show_ip_route_addr,
       show_ip_route_addr_cmd,
       "show ip route [vrf NAME] A.B.C.D",
       SHOW_STR
       IP_STR
       "IP routing table\n"
       VRF_CMD_HELP_STR
       "Network in the IP routing table to display\n")
{
	int ret;
	struct prefix_ipv4 p;
	struct route_table *table;
	struct route_node *rn;
	vrf_id_t vrf_id = VRF_DEFAULT;

	if (strmatch(argv[3]->text, "vrf")) {
		VRF_GET_ID(vrf_id, argv[4]->arg);
		ret = str2prefix_ipv4(argv[5]->arg, &p);
	} else {
		ret = str2prefix_ipv4(argv[3]->arg, &p);
	}

	if (ret <= 0) {
		vty_out(vty, "%% Malformed IPv4 address\n");
		return CMD_WARNING;
	}

	table = zebra_vrf_table(AFI_IP, SAFI_UNICAST, vrf_id);
	if (!table)
		return CMD_SUCCESS;

	rn = route_node_match(table, (struct prefix *)&p);
	if (!rn) {
		vty_out(vty, "%% Network not in table\n");
		return CMD_WARNING;
	}

	vty_show_ip_route_detail(vty, rn, 0);

	route_unlock_node(rn);

	return CMD_SUCCESS;
}

DEFUN (show_ip_route_prefix,
       show_ip_route_prefix_cmd,
       "show ip route [vrf NAME] A.B.C.D/M",
       SHOW_STR
       IP_STR
       "IP routing table\n"
       VRF_CMD_HELP_STR
       "IP prefix <network>/<length>, e.g., 35.0.0.0/8\n")
{
	int ret;
	struct prefix_ipv4 p;
	struct route_table *table;
	struct route_node *rn;
	vrf_id_t vrf_id = VRF_DEFAULT;

	if (strmatch(argv[3]->text, "vrf")) {
		VRF_GET_ID(vrf_id, argv[4]->arg);
		ret = str2prefix_ipv4(argv[5]->arg, &p);
	} else {
		ret = str2prefix_ipv4(argv[3]->arg, &p);
	}

	if (ret <= 0) {
		vty_out(vty, "%% Malformed IPv4 address\n");
		return CMD_WARNING;
	}

	table = zebra_vrf_table(AFI_IP, SAFI_UNICAST, vrf_id);
	if (!table)
		return CMD_SUCCESS;

	rn = route_node_match(table, (struct prefix *)&p);
	if (!rn || rn->p.prefixlen != p.prefixlen) {
		vty_out(vty, "%% Network not in table\n");
		return CMD_WARNING;
	}

	vty_show_ip_route_detail(vty, rn, 0);

	route_unlock_node(rn);

	return CMD_SUCCESS;
}


static void vty_show_ip_route_summary(struct vty *vty,
				      struct route_table *table)
{
	struct route_node *rn;
	struct route_entry *re;
#define ZEBRA_ROUTE_IBGP  ZEBRA_ROUTE_MAX
#define ZEBRA_ROUTE_TOTAL (ZEBRA_ROUTE_IBGP + 1)
	u_int32_t rib_cnt[ZEBRA_ROUTE_TOTAL + 1];
	u_int32_t fib_cnt[ZEBRA_ROUTE_TOTAL + 1];
	u_int32_t i;
	u_int32_t is_ibgp;

	memset(&rib_cnt, 0, sizeof(rib_cnt));
	memset(&fib_cnt, 0, sizeof(fib_cnt));
	for (rn = route_top(table); rn; rn = srcdest_route_next(rn))
		RNODE_FOREACH_RE(rn, re)
		{
			is_ibgp = (re->type == ZEBRA_ROUTE_BGP
				   && CHECK_FLAG(re->flags, ZEBRA_FLAG_IBGP));

			rib_cnt[ZEBRA_ROUTE_TOTAL]++;
			if (is_ibgp)
				rib_cnt[ZEBRA_ROUTE_IBGP]++;
			else
				rib_cnt[re->type]++;

			if (CHECK_FLAG(re->flags, ZEBRA_FLAG_SELECTED)) {
				fib_cnt[ZEBRA_ROUTE_TOTAL]++;

				if (is_ibgp)
					fib_cnt[ZEBRA_ROUTE_IBGP]++;
				else
					fib_cnt[re->type]++;
			}
		}

	vty_out(vty, "%-20s %-20s %s  (vrf %s)\n", "Route Source", "Routes",
		"FIB", zvrf_name(((rib_table_info_t *)table->info)->zvrf));

	for (i = 0; i < ZEBRA_ROUTE_MAX; i++) {
		if ((rib_cnt[i] > 0) || (i == ZEBRA_ROUTE_BGP
					 && rib_cnt[ZEBRA_ROUTE_IBGP] > 0)) {
			if (i == ZEBRA_ROUTE_BGP) {
				vty_out(vty, "%-20s %-20d %-20d \n", "ebgp",
					rib_cnt[ZEBRA_ROUTE_BGP],
					fib_cnt[ZEBRA_ROUTE_BGP]);
				vty_out(vty, "%-20s %-20d %-20d \n", "ibgp",
					rib_cnt[ZEBRA_ROUTE_IBGP],
					fib_cnt[ZEBRA_ROUTE_IBGP]);
			} else
				vty_out(vty, "%-20s %-20d %-20d \n",
					zebra_route_string(i), rib_cnt[i],
					fib_cnt[i]);
		}
	}

	vty_out(vty, "------\n");
	vty_out(vty, "%-20s %-20d %-20d \n", "Totals",
		rib_cnt[ZEBRA_ROUTE_TOTAL], fib_cnt[ZEBRA_ROUTE_TOTAL]);
	vty_out(vty, "\n");
}

/*
 * Implementation of the ip route summary prefix command.
 *
 * This command prints the primary prefixes that have been installed by various
 * protocols on the box.
 *
 */
static void vty_show_ip_route_summary_prefix(struct vty *vty,
					     struct route_table *table)
{
	struct route_node *rn;
	struct route_entry *re;
	struct nexthop *nexthop;
#define ZEBRA_ROUTE_IBGP  ZEBRA_ROUTE_MAX
#define ZEBRA_ROUTE_TOTAL (ZEBRA_ROUTE_IBGP + 1)
	u_int32_t rib_cnt[ZEBRA_ROUTE_TOTAL + 1];
	u_int32_t fib_cnt[ZEBRA_ROUTE_TOTAL + 1];
	u_int32_t i;
	int cnt;

	memset(&rib_cnt, 0, sizeof(rib_cnt));
	memset(&fib_cnt, 0, sizeof(fib_cnt));
	for (rn = route_top(table); rn; rn = srcdest_route_next(rn))
		RNODE_FOREACH_RE(rn, re)
		{

			/*
			 * In case of ECMP, count only once.
			 */
			cnt = 0;
			for (nexthop = re->nexthop; (!cnt && nexthop);
			     nexthop = nexthop->next) {
				cnt++;
				rib_cnt[ZEBRA_ROUTE_TOTAL]++;
				rib_cnt[re->type]++;
				if (CHECK_FLAG(nexthop->flags,
					       NEXTHOP_FLAG_FIB)) {
					fib_cnt[ZEBRA_ROUTE_TOTAL]++;
					fib_cnt[re->type]++;
				}
				if (re->type == ZEBRA_ROUTE_BGP
				    && CHECK_FLAG(re->flags, ZEBRA_FLAG_IBGP)) {
					rib_cnt[ZEBRA_ROUTE_IBGP]++;
					if (CHECK_FLAG(nexthop->flags,
						       NEXTHOP_FLAG_FIB))
						fib_cnt[ZEBRA_ROUTE_IBGP]++;
				}
			}
		}

	vty_out(vty, "%-20s %-20s %s  (vrf %s)\n", "Route Source",
		"Prefix Routes", "FIB",
		zvrf_name(((rib_table_info_t *)table->info)->zvrf));

	for (i = 0; i < ZEBRA_ROUTE_MAX; i++) {
		if (rib_cnt[i] > 0) {
			if (i == ZEBRA_ROUTE_BGP) {
				vty_out(vty, "%-20s %-20d %-20d \n", "ebgp",
					rib_cnt[ZEBRA_ROUTE_BGP]
						- rib_cnt[ZEBRA_ROUTE_IBGP],
					fib_cnt[ZEBRA_ROUTE_BGP]
						- fib_cnt[ZEBRA_ROUTE_IBGP]);
				vty_out(vty, "%-20s %-20d %-20d \n", "ibgp",
					rib_cnt[ZEBRA_ROUTE_IBGP],
					fib_cnt[ZEBRA_ROUTE_IBGP]);
			} else
				vty_out(vty, "%-20s %-20d %-20d \n",
					zebra_route_string(i), rib_cnt[i],
					fib_cnt[i]);
		}
	}

	vty_out(vty, "------\n");
	vty_out(vty, "%-20s %-20d %-20d \n", "Totals",
		rib_cnt[ZEBRA_ROUTE_TOTAL], fib_cnt[ZEBRA_ROUTE_TOTAL]);
	vty_out(vty, "\n");
}

/* Show route summary.  */
DEFUN (show_ip_route_summary,
       show_ip_route_summary_cmd,
       "show ip route [vrf NAME] summary",
       SHOW_STR
       IP_STR
       "IP routing table\n"
       VRF_CMD_HELP_STR
       "Summary of all routes\n")
{
	struct route_table *table;
	vrf_id_t vrf_id = VRF_DEFAULT;

	if (strmatch(argv[3]->text, "vrf"))
		VRF_GET_ID(vrf_id, argv[4]->arg);

	table = zebra_vrf_table(AFI_IP, SAFI_UNICAST, vrf_id);
	if (!table)
		return CMD_SUCCESS;

	vty_show_ip_route_summary(vty, table);

	return CMD_SUCCESS;
}

/* Show route summary prefix.  */
DEFUN (show_ip_route_summary_prefix,
       show_ip_route_summary_prefix_cmd,
       "show ip route [vrf NAME] summary prefix",
       SHOW_STR
       IP_STR
       "IP routing table\n"
       VRF_CMD_HELP_STR
       "Summary of all routes\n"
       "Prefix routes\n")
{
	struct route_table *table;
	vrf_id_t vrf_id = VRF_DEFAULT;

	if (strmatch(argv[3]->text, "vrf"))
		VRF_GET_ID(vrf_id, argv[4]->arg);

	table = zebra_vrf_table(AFI_IP, SAFI_UNICAST, vrf_id);
	if (!table)
		return CMD_SUCCESS;

	vty_show_ip_route_summary_prefix(vty, table);

	return CMD_SUCCESS;
}


DEFUN (show_ip_route_vrf_all_addr,
       show_ip_route_vrf_all_addr_cmd,
       "show ip route vrf all A.B.C.D",
       SHOW_STR
       IP_STR
       "IP routing table\n"
       VRF_ALL_CMD_HELP_STR
       "Network in the IP routing table to display\n")
{
	int idx_ipv4 = 5;
	int ret;
	struct prefix_ipv4 p;
	struct route_table *table;
	struct route_node *rn;
	struct vrf *vrf;
	struct zebra_vrf *zvrf;

	ret = str2prefix_ipv4(argv[idx_ipv4]->arg, &p);
	if (ret <= 0) {
		vty_out(vty, "%% Malformed IPv4 address\n");
		return CMD_WARNING;
	}

	RB_FOREACH(vrf, vrf_name_head, &vrfs_by_name)
	{
		if ((zvrf = vrf->info) == NULL
		    || (table = zvrf->table[AFI_IP][SAFI_UNICAST]) == NULL)
			continue;

		rn = route_node_match(table, (struct prefix *)&p);
		if (!rn)
			continue;

		vty_show_ip_route_detail(vty, rn, 0);

		route_unlock_node(rn);
	}

	return CMD_SUCCESS;
}

DEFUN (show_ip_route_vrf_all_prefix,
       show_ip_route_vrf_all_prefix_cmd,
       "show ip route vrf all A.B.C.D/M",
       SHOW_STR
       IP_STR
       "IP routing table\n"
       VRF_ALL_CMD_HELP_STR
       "IP prefix <network>/<length>, e.g., 35.0.0.0/8\n")
{
	int idx_ipv4_prefixlen = 5;
	int ret;
	struct prefix_ipv4 p;
	struct route_table *table;
	struct route_node *rn;
	struct vrf *vrf;
	struct zebra_vrf *zvrf;

	ret = str2prefix_ipv4(argv[idx_ipv4_prefixlen]->arg, &p);
	if (ret <= 0) {
		vty_out(vty, "%% Malformed IPv4 address\n");
		return CMD_WARNING;
	}

	RB_FOREACH(vrf, vrf_name_head, &vrfs_by_name)
	{
		if ((zvrf = vrf->info) == NULL
		    || (table = zvrf->table[AFI_IP][SAFI_UNICAST]) == NULL)
			continue;

		rn = route_node_match(table, (struct prefix *)&p);
		if (!rn)
			continue;
		if (rn->p.prefixlen != p.prefixlen) {
			route_unlock_node(rn);
			continue;
		}

		vty_show_ip_route_detail(vty, rn, 0);

		route_unlock_node(rn);
	}

	return CMD_SUCCESS;
}

DEFUN (show_ip_route_vrf_all_summary,
       show_ip_route_vrf_all_summary_cmd,
       "show ip route vrf all summary ",
       SHOW_STR
       IP_STR
       "IP routing table\n"
       VRF_ALL_CMD_HELP_STR
       "Summary of all routes\n")
{
	struct vrf *vrf;
	struct zebra_vrf *zvrf;

	RB_FOREACH(vrf, vrf_name_head, &vrfs_by_name)
	if ((zvrf = vrf->info) != NULL)
		vty_show_ip_route_summary(vty,
					  zvrf->table[AFI_IP][SAFI_UNICAST]);

	return CMD_SUCCESS;
}

DEFUN (show_ip_route_vrf_all_summary_prefix,
       show_ip_route_vrf_all_summary_prefix_cmd,
       "show ip route vrf all summary prefix",
       SHOW_STR
       IP_STR
       "IP routing table\n"
       VRF_ALL_CMD_HELP_STR
       "Summary of all routes\n"
       "Prefix routes\n")
{
	struct vrf *vrf;
	struct zebra_vrf *zvrf;

	RB_FOREACH(vrf, vrf_name_head, &vrfs_by_name)
	if ((zvrf = vrf->info) != NULL)
		vty_show_ip_route_summary_prefix(
			vty, zvrf->table[AFI_IP][SAFI_UNICAST]);

	return CMD_SUCCESS;
}

/* Write static route configuration. */
static int static_config(struct vty *vty, afi_t afi, safi_t safi,
			 const char *cmd)
{
	struct route_node *rn;
	struct static_route *si;
	struct route_table *stable;
	struct vrf *vrf;
	struct zebra_vrf *zvrf;
	char buf[SRCDEST2STR_BUFFER];
	int write = 0;

	RB_FOREACH(vrf, vrf_name_head, &vrfs_by_name)
	{
		if (!(zvrf = vrf->info))
			continue;
		if ((stable = zvrf->stable[afi][safi]) == NULL)
			continue;

		for (rn = route_top(stable); rn; rn = srcdest_route_next(rn))
			for (si = rn->info; si; si = si->next) {
				vty_out(vty, "%s %s", cmd,
					srcdest_rnode2str(rn, buf, sizeof buf));

				switch (si->type) {
				case STATIC_IPV4_GATEWAY:
					vty_out(vty, " %s",
						inet_ntoa(si->addr.ipv4));
					break;
				case STATIC_IPV6_GATEWAY:
					vty_out(vty, " %s",
						inet_ntop(AF_INET6,
							  &si->addr.ipv6, buf,
							  sizeof buf));
					break;
				case STATIC_IFINDEX:
					vty_out(vty, " %s", si->ifname);
					break;
				/* blackhole and Null0 mean the same thing */
				case STATIC_BLACKHOLE:
					if (CHECK_FLAG(si->flags, ZEBRA_FLAG_REJECT))
						vty_out(vty, " reject");
					else
						vty_out(vty, " Null0");
					break;
				case STATIC_IPV4_GATEWAY_IFINDEX:
					vty_out(vty, " %s %s",
						inet_ntop(AF_INET,
							  &si->addr.ipv4, buf,
							  sizeof buf),
						ifindex2ifname(si->ifindex,
							       si->vrf_id));
					break;
				case STATIC_IPV6_GATEWAY_IFINDEX:
					vty_out(vty, " %s %s",
						inet_ntop(AF_INET6,
							  &si->addr.ipv6, buf,
							  sizeof buf),
						ifindex2ifname(si->ifindex,
							       si->vrf_id));
					break;
				}

				/* flags are incompatible with STATIC_BLACKHOLE
				 */
				if (si->type != STATIC_BLACKHOLE) {
					if (CHECK_FLAG(si->flags,
						       ZEBRA_FLAG_REJECT))
						vty_out(vty, " %s", "reject");

					if (CHECK_FLAG(si->flags,
						       ZEBRA_FLAG_BLACKHOLE))
						vty_out(vty, " %s",
							"blackhole");
				}

				if (si->tag)
					vty_out(vty, " tag %" ROUTE_TAG_PRI,
						si->tag);

				if (si->distance
				    != ZEBRA_STATIC_DISTANCE_DEFAULT)
					vty_out(vty, " %d", si->distance);

				if (si->vrf_id != VRF_DEFAULT)
					vty_out(vty, " vrf %s",
						zvrf_name(zvrf));

				/* Label information */
				if (si->snh_label.num_labels)
					vty_out(vty, " label %s",
						mpls_label2str(
							si->snh_label
								.num_labels,
							si->snh_label.label,
							buf, sizeof buf, 0));

				vty_out(vty, "\n");

				write = 1;
			}
	}
	return write;
}

DEFPY (ipv6_route,
       ipv6_route_cmd,
       "[no] ipv6 route X:X::X:X/M$prefix [from X:X::X:X/M]\
          <\
            {X:X::X:X$gate|INTERFACE$ifname}\
            |null0$ifname\
            |<reject|blackhole>$flag\
          >\
          [{\
            tag (1-4294967295)\
            |(1-255)$distance\
            |vrf NAME\
            |label WORD\
          }]",
       NO_STR
       IPV6_STR
       "Establish static routes\n"
       "IPv6 destination prefix (e.g. 3ffe:506::/32)\n"
       "IPv6 source-dest route\n"
       "IPv6 source prefix\n"
       "IPv6 gateway address\n"
       "IPv6 gateway interface name\n"
       "Null interface\n"
       "Emit an ICMP unreachable when matched\n"
       "Silently discard pkts when matched\n"
       "Set tag for this route\n"
       "Tag value\n"
       "Distance value for this prefix\n"
       VRF_CMD_HELP_STR
       MPLS_LABEL_HELPSTR)
{
	return zebra_static_route(vty, AFI_IP6, SAFI_UNICAST, no, prefix_str,
				  NULL, from_str, gate_str, ifname, flag,
				  tag_str, distance_str, vrf, label);
}

DEFUN (show_ipv6_route,
       show_ipv6_route_cmd,
       "show ipv6 <fib|route> [vrf NAME] [tag (1-4294967295)|X:X::X:X/M longer-prefixes|" FRR_IP6_REDIST_STR_ZEBRA "] [json]",
       SHOW_STR
       IP_STR
       "IP forwarding table\n"
       "IP routing table\n"
       VRF_CMD_HELP_STR
       "Show only routes with tag\n"
       "Tag value\n"
       "IPv6 prefix\n"
       "Show route matching the specified Network/Mask pair only\n"
       FRR_IP6_REDIST_HELP_STR_ZEBRA
       JSON_STR)
{
	bool uf = use_fib(argv[2]);
	struct route_table *table;
	int vrf_all = 0;
	route_tag_t tag = 0;
	vrf_id_t vrf_id = VRF_DEFAULT;
	struct vrf *vrf;
	struct zebra_vrf *zvrf;
	int uj = use_json(argc, argv);
	int idx = 0;
	struct prefix p;
	bool longer_prefixes = false;
	bool supernets_only = false;
	int type = 0;

	if (argv_find(argv, argc, "vrf", &idx)) {
		if (strmatch(argv[idx + 1]->arg, "all"))
			vrf_all = 1;
		else
			VRF_GET_ID(vrf_id, argv[idx + 1]->arg);
	}

	if (argv_find(argv, argc, "tag", &idx))
		tag = strtoul(argv[idx + 1]->arg, NULL, 10);

	else if (argv_find(argv, argc, "X:X::X:X/M", &idx)) {
		str2prefix(argv[idx]->arg, &p);
		longer_prefixes = true;
	}

	else {
		if (argv_find(argv, argc, "kernel", &idx))
			type = proto_redistnum(AFI_IP6, argv[idx]->text);
		else if (argv_find(argv, argc, "babel", &idx))
			type = proto_redistnum(AFI_IP6, argv[idx]->text);
		else if (argv_find(argv, argc, "connected", &idx))
			type = proto_redistnum(AFI_IP6, argv[idx]->text);
		else if (argv_find(argv, argc, "static", &idx))
			type = proto_redistnum(AFI_IP6, argv[idx]->text);
		else if (argv_find(argv, argc, "ripng", &idx))
			type = proto_redistnum(AFI_IP6, argv[idx]->text);
		else if (argv_find(argv, argc, "ospf6", &idx))
			type = proto_redistnum(AFI_IP6, argv[idx]->text);
		else if (argv_find(argv, argc, "isis", &idx))
			type = proto_redistnum(AFI_IP6, argv[idx]->text);
		else if (argv_find(argv, argc, "bgp", &idx))
			type = proto_redistnum(AFI_IP6, argv[idx]->text);
		else if (argv_find(argv, argc, "nhrp", &idx))
			type = proto_redistnum(AFI_IP6, argv[idx]->text);
		else if (argv_find(argv, argc, "table", &idx))
			type = proto_redistnum(AFI_IP6, argv[idx]->text);
		else if (argv_find(argv, argc, "vnc", &idx))
			type = proto_redistnum(AFI_IP6, argv[idx]->text);

		if (type < 0) {
			vty_out(vty, "Unknown route type\n");
			return CMD_WARNING;
		}
	}

	if (vrf_all) {
		RB_FOREACH(vrf, vrf_name_head, &vrfs_by_name)
		{
			if ((zvrf = vrf->info) == NULL
			    || (table = zvrf->table[AFI_IP6][SAFI_UNICAST])
				       == NULL)
				continue;

			do_show_ip_route(vty, zvrf_name(zvrf), AFI_IP6,
					 SAFI_UNICAST, uf, uj, tag,
					 longer_prefixes ? &p : NULL,
					 supernets_only, type, 0);
		}
	} else {
		vrf = vrf_lookup_by_id(vrf_id);
		do_show_ip_route(vty, vrf->name, AFI_IP6, SAFI_UNICAST, uf, uj,
				 tag, longer_prefixes ? &p : NULL,
				 supernets_only, type, 0);
	}
	return CMD_SUCCESS;
}

DEFUN (show_ipv6_route_addr,
       show_ipv6_route_addr_cmd,
       "show ipv6 route [vrf NAME] X:X::X:X",
       SHOW_STR
       IP_STR
       "IPv6 routing table\n"
       VRF_CMD_HELP_STR
       "IPv6 Address\n")
{
	int ret;
	struct prefix_ipv6 p;
	struct route_table *table;
	struct route_node *rn;
	vrf_id_t vrf_id = VRF_DEFAULT;

	if (strmatch(argv[3]->text, "vrf")) {
		VRF_GET_ID(vrf_id, argv[4]->arg);
		ret = str2prefix_ipv6(argv[5]->arg, &p);
	} else {
		ret = str2prefix_ipv6(argv[3]->arg, &p);
	}

	if (ret <= 0) {
		vty_out(vty, "Malformed IPv6 address\n");
		return CMD_WARNING;
	}

	table = zebra_vrf_table(AFI_IP6, SAFI_UNICAST, vrf_id);
	if (!table)
		return CMD_SUCCESS;

	rn = route_node_match(table, (struct prefix *)&p);
	if (!rn) {
		vty_out(vty, "%% Network not in table\n");
		return CMD_WARNING;
	}

	vty_show_ip_route_detail(vty, rn, 0);

	route_unlock_node(rn);

	return CMD_SUCCESS;
}

DEFUN (show_ipv6_route_prefix,
       show_ipv6_route_prefix_cmd,
       "show ipv6 route [vrf NAME] X:X::X:X/M",
       SHOW_STR
       IP_STR
       "IPv6 routing table\n"
       VRF_CMD_HELP_STR
       "IPv6 prefix\n")
{
	int ret;
	struct prefix_ipv6 p;
	struct route_table *table;
	struct route_node *rn;
	vrf_id_t vrf_id = VRF_DEFAULT;

	if (strmatch(argv[3]->text, "vrf")) {
		VRF_GET_ID(vrf_id, argv[4]->arg);
		ret = str2prefix_ipv6(argv[5]->arg, &p);
	} else
		ret = str2prefix_ipv6(argv[3]->arg, &p);

	if (ret <= 0) {
		vty_out(vty, "Malformed IPv6 prefix\n");
		return CMD_WARNING;
	}

	table = zebra_vrf_table(AFI_IP6, SAFI_UNICAST, vrf_id);
	if (!table)
		return CMD_SUCCESS;

	rn = route_node_match(table, (struct prefix *)&p);
	if (!rn || rn->p.prefixlen != p.prefixlen) {
		vty_out(vty, "%% Network not in table\n");
		return CMD_WARNING;
	}

	vty_show_ip_route_detail(vty, rn, 0);

	route_unlock_node(rn);

	return CMD_SUCCESS;
}


/* Show route summary.  */
DEFUN (show_ipv6_route_summary,
       show_ipv6_route_summary_cmd,
       "show ipv6 route [vrf NAME] summary",
       SHOW_STR
       IP_STR
       "IPv6 routing table\n"
       VRF_CMD_HELP_STR
       "Summary of all IPv6 routes\n")
{
	struct route_table *table;
	vrf_id_t vrf_id = VRF_DEFAULT;

	if (strmatch(argv[3]->text, "vrf"))
		VRF_GET_ID(vrf_id, argv[4]->arg);

	table = zebra_vrf_table(AFI_IP6, SAFI_UNICAST, vrf_id);
	if (!table)
		return CMD_SUCCESS;

	vty_show_ip_route_summary(vty, table);

	return CMD_SUCCESS;
}


/* Show ipv6 route summary prefix.  */
DEFUN (show_ipv6_route_summary_prefix,
       show_ipv6_route_summary_prefix_cmd,
       "show ipv6 route [vrf NAME] summary prefix",
       SHOW_STR
       IP_STR
       "IPv6 routing table\n"
       VRF_CMD_HELP_STR
       "Summary of all IPv6 routes\n"
       "Prefix routes\n")
{
	struct route_table *table;
	vrf_id_t vrf_id = VRF_DEFAULT;

	if (strmatch(argv[3]->text, "vrf"))
		VRF_GET_ID(vrf_id, argv[4]->arg);

	table = zebra_vrf_table(AFI_IP6, SAFI_UNICAST, vrf_id);
	if (!table)
		return CMD_SUCCESS;

	vty_show_ip_route_summary_prefix(vty, table);

	return CMD_SUCCESS;
}


/*
 * Show IPv6 mroute command.Used to dump
 * the Multicast routing table.
 */
DEFUN (show_ipv6_mroute,
       show_ipv6_mroute_cmd,
       "show ipv6 mroute [vrf NAME]",
       SHOW_STR
       IP_STR
       "IPv6 Multicast routing table\n"
       VRF_CMD_HELP_STR)
{
	struct route_table *table;
	struct route_node *rn;
	struct route_entry *re;
	int first = 1;
	vrf_id_t vrf_id = VRF_DEFAULT;

	if (argc == 5)
		VRF_GET_ID(vrf_id, argv[4]->arg);

	table = zebra_vrf_table(AFI_IP6, SAFI_MULTICAST, vrf_id);
	if (!table)
		return CMD_SUCCESS;

	/* Show all IPv6 route. */
	for (rn = route_top(table); rn; rn = srcdest_route_next(rn))
		RNODE_FOREACH_RE(rn, re)
		{
			if (first) {
				vty_out(vty, SHOW_ROUTE_V6_HEADER);
				first = 0;
			}
			vty_show_ip_route(vty, rn, re, NULL);
		}
	return CMD_SUCCESS;
}

DEFUN (show_ipv6_route_vrf_all_addr,
       show_ipv6_route_vrf_all_addr_cmd,
       "show ipv6 route vrf all X:X::X:X",
       SHOW_STR
       IP_STR
       "IPv6 routing table\n"
       VRF_ALL_CMD_HELP_STR
       "IPv6 Address\n")
{
	int idx_ipv6 = 5;
	int ret;
	struct prefix_ipv6 p;
	struct route_table *table;
	struct route_node *rn;
	struct vrf *vrf;
	struct zebra_vrf *zvrf;

	ret = str2prefix_ipv6(argv[idx_ipv6]->arg, &p);
	if (ret <= 0) {
		vty_out(vty, "Malformed IPv6 address\n");
		return CMD_WARNING;
	}

	RB_FOREACH(vrf, vrf_name_head, &vrfs_by_name)
	{
		if ((zvrf = vrf->info) == NULL
		    || (table = zvrf->table[AFI_IP6][SAFI_UNICAST]) == NULL)
			continue;

		rn = route_node_match(table, (struct prefix *)&p);
		if (!rn)
			continue;

		vty_show_ip_route_detail(vty, rn, 0);

		route_unlock_node(rn);
	}

	return CMD_SUCCESS;
}

DEFUN (show_ipv6_route_vrf_all_prefix,
       show_ipv6_route_vrf_all_prefix_cmd,
       "show ipv6 route vrf all X:X::X:X/M",
       SHOW_STR
       IP_STR
       "IPv6 routing table\n"
       VRF_ALL_CMD_HELP_STR
       "IPv6 prefix\n")
{
	int idx_ipv6_prefixlen = 5;
	int ret;
	struct prefix_ipv6 p;
	struct route_table *table;
	struct route_node *rn;
	struct vrf *vrf;
	struct zebra_vrf *zvrf;

	ret = str2prefix_ipv6(argv[idx_ipv6_prefixlen]->arg, &p);
	if (ret <= 0) {
		vty_out(vty, "Malformed IPv6 prefix\n");
		return CMD_WARNING;
	}

	RB_FOREACH(vrf, vrf_name_head, &vrfs_by_name)
	{
		if ((zvrf = vrf->info) == NULL
		    || (table = zvrf->table[AFI_IP6][SAFI_UNICAST]) == NULL)
			continue;

		rn = route_node_match(table, (struct prefix *)&p);
		if (!rn)
			continue;
		if (rn->p.prefixlen != p.prefixlen) {
			route_unlock_node(rn);
			continue;
		}

		vty_show_ip_route_detail(vty, rn, 0);

		route_unlock_node(rn);
	}

	return CMD_SUCCESS;
}

DEFUN (show_ipv6_route_vrf_all_summary,
       show_ipv6_route_vrf_all_summary_cmd,
       "show ipv6 route vrf all summary",
       SHOW_STR
       IP_STR
       "IPv6 routing table\n"
       VRF_ALL_CMD_HELP_STR
       "Summary of all IPv6 routes\n")
{
	struct vrf *vrf;
	struct zebra_vrf *zvrf;

	RB_FOREACH(vrf, vrf_name_head, &vrfs_by_name)
	if ((zvrf = vrf->info) != NULL)
		vty_show_ip_route_summary(vty,
					  zvrf->table[AFI_IP6][SAFI_UNICAST]);

	return CMD_SUCCESS;
}

DEFUN (show_ipv6_mroute_vrf_all,
       show_ipv6_mroute_vrf_all_cmd,
       "show ipv6 mroute vrf all",
       SHOW_STR
       IP_STR
       "IPv6 Multicast routing table\n"
       VRF_ALL_CMD_HELP_STR)
{
	struct route_table *table;
	struct route_node *rn;
	struct route_entry *re;
	struct vrf *vrf;
	struct zebra_vrf *zvrf;
	int first = 1;

	RB_FOREACH(vrf, vrf_name_head, &vrfs_by_name)
	{
		if ((zvrf = vrf->info) == NULL
		    || (table = zvrf->table[AFI_IP6][SAFI_MULTICAST]) == NULL)
			continue;

		/* Show all IPv6 route. */
		for (rn = route_top(table); rn; rn = srcdest_route_next(rn))
			RNODE_FOREACH_RE(rn, re)
			{
				if (first) {
					vty_out(vty, SHOW_ROUTE_V6_HEADER);
					first = 0;
				}
				vty_show_ip_route(vty, rn, re, NULL);
			}
	}
	return CMD_SUCCESS;
}

DEFUN (show_ipv6_route_vrf_all_summary_prefix,
       show_ipv6_route_vrf_all_summary_prefix_cmd,
       "show ipv6 route vrf all summary prefix",
       SHOW_STR
       IP_STR
       "IPv6 routing table\n"
       VRF_ALL_CMD_HELP_STR
       "Summary of all IPv6 routes\n"
       "Prefix routes\n")
{
	struct vrf *vrf;
	struct zebra_vrf *zvrf;

	RB_FOREACH(vrf, vrf_name_head, &vrfs_by_name)
	if ((zvrf = vrf->info) != NULL)
		vty_show_ip_route_summary_prefix(
			vty, zvrf->table[AFI_IP6][SAFI_UNICAST]);

	return CMD_SUCCESS;
}

DEFUN (allow_external_route_update,
       allow_external_route_update_cmd,
       "allow-external-route-update",
       "Allow FRR routes to be overwritten by external processes\n")
{
	allow_delete = 1;

	return CMD_SUCCESS;
}

DEFUN (no_allow_external_route_update,
       no_allow_external_route_update_cmd,
       "no allow-external-route-update",
       NO_STR
       "Allow FRR routes to be overwritten by external processes\n")
{
	allow_delete = 0;

	return CMD_SUCCESS;
}

/* show vrf */
DEFUN (show_vrf,
       show_vrf_cmd,
       "show vrf",
       SHOW_STR
       "VRF\n")
{
	struct vrf *vrf;
	struct zebra_vrf *zvrf;

	RB_FOREACH(vrf, vrf_name_head, &vrfs_by_name)
	{
		if (!(zvrf = vrf->info))
			continue;
		if (!zvrf_id(zvrf))
			continue;

		vty_out(vty, "vrf %s ", zvrf_name(zvrf));
		if (zvrf_id(zvrf) == VRF_UNKNOWN)
			vty_out(vty, "inactive");
		else
			vty_out(vty, "id %u table %u", zvrf_id(zvrf),
				zvrf->table_id);
		vty_out(vty, "\n");
	}

	return CMD_SUCCESS;
}

DEFUN (show_evpn_vni,
       show_evpn_vni_cmd,
       "show evpn vni",
       SHOW_STR
       "EVPN\n"
       "VxLAN information\n")
{
	struct zebra_vrf *zvrf;

	zvrf = vrf_info_lookup(VRF_DEFAULT);
	zebra_vxlan_print_vnis(vty, zvrf);
	return CMD_SUCCESS;
}

DEFUN (show_evpn_vni_vni,
       show_evpn_vni_vni_cmd,
       "show evpn vni " CMD_VNI_RANGE,
       SHOW_STR
       "EVPN\n"
       "VxLAN Network Identifier\n"
       "VNI number\n")
{
	struct zebra_vrf *zvrf;
	vni_t vni;

	vni = strtoul(argv[3]->arg, NULL, 10);
	zvrf = vrf_info_lookup(VRF_DEFAULT);
	zebra_vxlan_print_vni(vty, zvrf, vni);
	return CMD_SUCCESS;
}

DEFUN (show_evpn_mac_vni,
       show_evpn_mac_vni_cmd,
       "show evpn mac vni " CMD_VNI_RANGE,
       SHOW_STR
       "EVPN\n"
       "MAC addresses\n"
       "VxLAN Network Identifier\n"
       "VNI number\n")
{
	struct zebra_vrf *zvrf;
	vni_t vni;

	vni = strtoul(argv[4]->arg, NULL, 10);
	zvrf = vrf_info_lookup(VRF_DEFAULT);
	zebra_vxlan_print_macs_vni(vty, zvrf, vni);
	return CMD_SUCCESS;
}

DEFUN (show_evpn_mac_vni_all,
       show_evpn_mac_vni_all_cmd,
       "show evpn mac vni all",
       SHOW_STR
       "EVPN\n"
       "MAC addresses\n"
       "VxLAN Network Identifier\n"
       "All VNIs\n")
{
	struct zebra_vrf *zvrf;

	zvrf = vrf_info_lookup(VRF_DEFAULT);
	zebra_vxlan_print_macs_all_vni(vty, zvrf);
	return CMD_SUCCESS;
}

DEFUN (show_evpn_mac_vni_all_vtep,
       show_evpn_mac_vni_all_vtep_cmd,
       "show evpn mac vni all vtep A.B.C.D",
       SHOW_STR
       "EVPN\n"
       "MAC addresses\n"
       "VxLAN Network Identifier\n"
       "All VNIs\n"
       "Remote VTEP\n"
       "Remote VTEP IP address\n")
{
	struct zebra_vrf *zvrf;
	struct in_addr vtep_ip;

	if (!inet_aton(argv[6]->arg, &vtep_ip)) {
		vty_out(vty, "%% Malformed VTEP IP address\n");
		return CMD_WARNING;
	}
	zvrf = vrf_info_lookup(VRF_DEFAULT);
	zebra_vxlan_print_macs_all_vni_vtep(vty, zvrf, vtep_ip);

	return CMD_SUCCESS;
}


DEFUN (show_evpn_mac_vni_mac,
       show_evpn_mac_vni_mac_cmd,
       "show evpn mac vni " CMD_VNI_RANGE " mac WORD",
       SHOW_STR
       "EVPN\n"
       "MAC addresses\n"
       "VxLAN Network Identifier\n"
       "VNI number\n"
       "MAC\n"
       "MAC address (e.g., 00:e0:ec:20:12:62)\n")
{
	struct zebra_vrf *zvrf;
	vni_t vni;
	struct ethaddr mac;

	vni = strtoul(argv[4]->arg, NULL, 10);
	if (!prefix_str2mac(argv[6]->arg, &mac)) {
		vty_out(vty, "%% Malformed MAC address");
		return CMD_WARNING;
	}
	zvrf = vrf_info_lookup(VRF_DEFAULT);
	zebra_vxlan_print_specific_mac_vni(vty, zvrf, vni, &mac);
	return CMD_SUCCESS;
}

DEFUN (show_evpn_mac_vni_vtep,
       show_evpn_mac_vni_vtep_cmd,
       "show evpn mac vni " CMD_VNI_RANGE " vtep A.B.C.D",
       SHOW_STR
       "EVPN\n"
       "MAC addresses\n"
       "VxLAN Network Identifier\n"
       "VNI number\n"
       "Remote VTEP\n"
       "Remote VTEP IP address\n")
{
	struct zebra_vrf *zvrf;
	vni_t vni;
	struct in_addr vtep_ip;

	vni = strtoul(argv[4]->arg, NULL, 10);
	if (!inet_aton(argv[6]->arg, &vtep_ip)) {
		vty_out(vty, "%% Malformed VTEP IP address\n");
		return CMD_WARNING;
	}

	zvrf = vrf_info_lookup(VRF_DEFAULT);
	zebra_vxlan_print_macs_vni_vtep(vty, zvrf, vni, vtep_ip);
	return CMD_SUCCESS;
}

DEFUN (show_evpn_neigh_vni,
       show_evpn_neigh_vni_cmd,
       "show evpn arp-cache vni " CMD_VNI_RANGE,
       SHOW_STR
       "EVPN\n"
       "ARP and ND cache\n"
       "VxLAN Network Identifier\n"
       "VNI number\n")
{
	struct zebra_vrf *zvrf;
	vni_t vni;

	vni = strtoul(argv[4]->arg, NULL, 10);
	zvrf = vrf_info_lookup(VRF_DEFAULT);
	zebra_vxlan_print_neigh_vni(vty, zvrf, vni);
	return CMD_SUCCESS;
}

DEFUN (show_evpn_neigh_vni_all,
       show_evpn_neigh_vni_all_cmd,
       "show evpn arp-cache vni all",
       SHOW_STR
       "EVPN\n"
       "ARP and ND cache\n"
       "VxLAN Network Identifier\n"
       "All VNIs\n")
{
	struct zebra_vrf *zvrf;

	zvrf = vrf_info_lookup(VRF_DEFAULT);
	zebra_vxlan_print_neigh_all_vni(vty, zvrf);
	return CMD_SUCCESS;
}

DEFUN (show_evpn_neigh_vni_neigh,
       show_evpn_neigh_vni_neigh_cmd,
       "show evpn arp-cache vni " CMD_VNI_RANGE " ip WORD",
       SHOW_STR
       "EVPN\n"
       "ARP and ND cache\n"
       "VxLAN Network Identifier\n"
       "VNI number\n"
       "Neighbor\n"
       "Neighbor address (IPv4 or IPv6 address)\n")
{
	struct zebra_vrf *zvrf;
	vni_t vni;
	struct ipaddr ip;

	vni = strtoul(argv[4]->arg, NULL, 10);
	if (str2ipaddr(argv[6]->arg, &ip) != 0) {
		vty_out(vty, "%% Malformed Neighbor address\n");
		return CMD_WARNING;
	}
	zvrf = vrf_info_lookup(VRF_DEFAULT);
	zebra_vxlan_print_specific_neigh_vni(vty, zvrf, vni, &ip);
	return CMD_SUCCESS;
}

DEFUN (show_evpn_neigh_vni_vtep,
       show_evpn_neigh_vni_vtep_cmd,
       "show evpn arp-cache vni " CMD_VNI_RANGE " vtep A.B.C.D",
       SHOW_STR
       "EVPN\n"
       "ARP and ND cache\n"
       "VxLAN Network Identifier\n"
       "VNI number\n"
       "Remote VTEP\n"
       "Remote VTEP IP address\n")
{
	struct zebra_vrf *zvrf;
	vni_t vni;
	struct in_addr vtep_ip;

	vni = strtoul(argv[4]->arg, NULL, 10);
	if (!inet_aton(argv[6]->arg, &vtep_ip)) {
		vty_out(vty, "%% Malformed VTEP IP address\n");
		return CMD_WARNING;
	}

	zvrf = vrf_info_lookup(VRF_DEFAULT);
	zebra_vxlan_print_neigh_vni_vtep(vty, zvrf, vni, vtep_ip);
	return CMD_SUCCESS;
}

/* Static ip route configuration write function. */
static int zebra_ip_config(struct vty *vty)
{
	int write = 0;

	write += static_config(vty, AFI_IP, SAFI_UNICAST, "ip route");
	write += static_config(vty, AFI_IP, SAFI_MULTICAST, "ip mroute");
	write += static_config(vty, AFI_IP6, SAFI_UNICAST, "ipv6 route");

	write += zebra_import_table_config(vty);
	return write;
}

DEFUN (ip_zebra_import_table_distance,
       ip_zebra_import_table_distance_cmd,
       "ip import-table (1-252) [distance (1-255)] [route-map WORD]",
       IP_STR
       "import routes from non-main kernel table\n"
       "kernel routing table id\n"
       "Distance for imported routes\n"
       "Default distance value\n"
       "route-map for filtering\n"
       "route-map name\n")
{
	u_int32_t table_id = 0;

	table_id = strtoul(argv[2]->arg, NULL, 10);
	int distance = ZEBRA_TABLE_DISTANCE_DEFAULT;
	char *rmap =
		strmatch(argv[argc - 2]->text, "route-map")
			? XSTRDUP(MTYPE_ROUTE_MAP_NAME, argv[argc - 1]->arg)
			: NULL;
	int ret;

	if (argc == 7 || (argc == 5 && !rmap))
		distance = strtoul(argv[4]->arg, NULL, 10);

	if (!is_zebra_valid_kernel_table(table_id)) {
		vty_out(vty,
			"Invalid routing table ID, %d. Must be in range 1-252\n",
			table_id);
		return CMD_WARNING;
	}

	if (is_zebra_main_routing_table(table_id)) {
		vty_out(vty,
			"Invalid routing table ID, %d. Must be non-default table\n",
			table_id);
		return CMD_WARNING;
	}

	ret = zebra_import_table(AFI_IP, table_id, distance, rmap, 1);
	if (rmap)
		XFREE(MTYPE_ROUTE_MAP_NAME, rmap);

	return ret;
}

DEFUN (no_ip_zebra_import_table,
       no_ip_zebra_import_table_cmd,
       "no ip import-table (1-252) [distance (1-255)] [route-map NAME]",
       NO_STR
       IP_STR
       "import routes from non-main kernel table\n"
       "kernel routing table id\n"
       "Distance for imported routes\n"
       "Default distance value\n"
       "route-map for filtering\n"
       "route-map name\n")
{
	u_int32_t table_id = 0;
	table_id = strtoul(argv[3]->arg, NULL, 10);

	if (!is_zebra_valid_kernel_table(table_id)) {
		vty_out(vty,
			"Invalid routing table ID. Must be in range 1-252\n");
		return CMD_WARNING;
	}

	if (is_zebra_main_routing_table(table_id)) {
		vty_out(vty,
			"Invalid routing table ID, %d. Must be non-default table\n",
			table_id);
		return CMD_WARNING;
	}

	if (!is_zebra_import_table_enabled(AFI_IP, table_id))
		return CMD_SUCCESS;

	return (zebra_import_table(AFI_IP, table_id, 0, NULL, 0));
}

static int config_write_protocol(struct vty *vty)
{
	if (allow_delete)
		vty_out(vty, "allow-external-route-update\n");

	if (zebra_rnh_ip_default_route)
		vty_out(vty, "ip nht resolve-via-default\n");

	if (zebra_rnh_ipv6_default_route)
		vty_out(vty, "ipv6 nht resolve-via-default\n");

	enum multicast_mode ipv4_multicast_mode = multicast_mode_ipv4_get();

	if (ipv4_multicast_mode != MCAST_NO_CONFIG)
		vty_out(vty, "ip multicast rpf-lookup-mode %s\n",
			ipv4_multicast_mode == MCAST_URIB_ONLY
				? "urib-only"
				: ipv4_multicast_mode == MCAST_MRIB_ONLY
					  ? "mrib-only"
					  : ipv4_multicast_mode
							    == MCAST_MIX_MRIB_FIRST
						    ? "mrib-then-urib"
						    : ipv4_multicast_mode
								      == MCAST_MIX_DISTANCE
							      ? "lower-distance"
							      : "longer-prefix");

	zebra_routemap_config_write_protocol(vty);

	return 1;
}

/* IP node for static routes. */
static struct cmd_node ip_node = {IP_NODE, "", 1};
static struct cmd_node protocol_node = {PROTOCOL_NODE, "", 1};

/* Route VTY.  */
void zebra_vty_init(void)
{
	install_node(&ip_node, zebra_ip_config);
	install_node(&protocol_node, config_write_protocol);

	install_element(CONFIG_NODE, &allow_external_route_update_cmd);
	install_element(CONFIG_NODE, &no_allow_external_route_update_cmd);
	install_element(CONFIG_NODE, &ip_mroute_dist_cmd);
	install_element(CONFIG_NODE, &ip_multicast_mode_cmd);
	install_element(CONFIG_NODE, &no_ip_multicast_mode_cmd);
	install_element(CONFIG_NODE, &ip_route_cmd);
	install_element(CONFIG_NODE, &ip_zebra_import_table_distance_cmd);
	install_element(CONFIG_NODE, &no_ip_zebra_import_table_cmd);

	install_element(VIEW_NODE, &show_vrf_cmd);
	install_element(VIEW_NODE, &show_ip_route_cmd);
	install_element(VIEW_NODE, &show_ip_nht_cmd);
	install_element(VIEW_NODE, &show_ip_nht_vrf_all_cmd);
	install_element(VIEW_NODE, &show_ipv6_nht_cmd);
	install_element(VIEW_NODE, &show_ipv6_nht_vrf_all_cmd);
	install_element(VIEW_NODE, &show_ip_route_addr_cmd);
	install_element(VIEW_NODE, &show_ip_route_prefix_cmd);
	install_element(VIEW_NODE, &show_ip_route_summary_cmd);
	install_element(VIEW_NODE, &show_ip_route_summary_prefix_cmd);

	install_element(VIEW_NODE, &show_ip_rpf_cmd);
	install_element(VIEW_NODE, &show_ip_rpf_addr_cmd);

	install_element(VIEW_NODE, &show_ip_route_vrf_all_addr_cmd);
	install_element(VIEW_NODE, &show_ip_route_vrf_all_prefix_cmd);
	install_element(VIEW_NODE, &show_ip_route_vrf_all_summary_cmd);
	install_element(VIEW_NODE, &show_ip_route_vrf_all_summary_prefix_cmd);

	install_element(CONFIG_NODE, &ipv6_route_cmd);
	install_element(CONFIG_NODE, &ip_nht_default_route_cmd);
	install_element(CONFIG_NODE, &no_ip_nht_default_route_cmd);
	install_element(CONFIG_NODE, &ipv6_nht_default_route_cmd);
	install_element(CONFIG_NODE, &no_ipv6_nht_default_route_cmd);
	install_element(VIEW_NODE, &show_ipv6_route_cmd);
	install_element(VIEW_NODE, &show_ipv6_route_summary_cmd);
	install_element(VIEW_NODE, &show_ipv6_route_summary_prefix_cmd);
	install_element(VIEW_NODE, &show_ipv6_route_addr_cmd);
	install_element(VIEW_NODE, &show_ipv6_route_prefix_cmd);
	install_element(VIEW_NODE, &show_ipv6_mroute_cmd);

	/* Commands for VRF */
	install_element(VIEW_NODE, &show_ipv6_route_vrf_all_summary_cmd);
	install_element(VIEW_NODE, &show_ipv6_route_vrf_all_summary_prefix_cmd);
	install_element(VIEW_NODE, &show_ipv6_route_vrf_all_addr_cmd);
	install_element(VIEW_NODE, &show_ipv6_route_vrf_all_prefix_cmd);

	install_element(VIEW_NODE, &show_ipv6_mroute_vrf_all_cmd);

	install_element(VIEW_NODE, &show_evpn_vni_cmd);
	install_element(VIEW_NODE, &show_evpn_vni_vni_cmd);
	install_element(VIEW_NODE, &show_evpn_mac_vni_cmd);
	install_element(VIEW_NODE, &show_evpn_mac_vni_all_cmd);
	install_element(VIEW_NODE, &show_evpn_mac_vni_all_vtep_cmd);
	install_element(VIEW_NODE, &show_evpn_mac_vni_mac_cmd);
	install_element(VIEW_NODE, &show_evpn_mac_vni_vtep_cmd);
	install_element(VIEW_NODE, &show_evpn_neigh_vni_cmd);
	install_element(VIEW_NODE, &show_evpn_neigh_vni_all_cmd);
	install_element(VIEW_NODE, &show_evpn_neigh_vni_neigh_cmd);
	install_element(VIEW_NODE, &show_evpn_neigh_vni_vtep_cmd);
}
