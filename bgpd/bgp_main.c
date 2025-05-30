// SPDX-License-Identifier: GPL-2.0-or-later
/* Main routine of bgpd.
 * Copyright (C) 1996, 97, 98, 1999 Kunihiro Ishiguro
 */

#include <zebra.h>

#include <pthread.h>
#include "vector.h"
#include "command.h"
#include "getopt.h"
#include "frrevent.h"
#include <lib/version.h>
#include "memory.h"
#include "prefix.h"
#include "log.h"
#include "privs.h"
#include "sigevent.h"
#include "zclient.h"
#include "routemap.h"
#include "filter.h"
#include "plist.h"
#include "stream.h"
#include "queue.h"
#include "vrf.h"
#include "bfd.h"
#include "libfrr.h"
#include "ns.h"
#include "libagentx.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_attr.h"
#include "bgpd/bgp_route.h"
#include "bgpd/bgp_mplsvpn.h"
#include "bgpd/bgp_aspath.h"
#include "bgpd/bgp_dump.h"
#include "bgpd/bgp_route.h"
#include "bgpd/bgp_nexthop.h"
#include "bgpd/bgp_regex.h"
#include "bgpd/bgp_clist.h"
#include "bgpd/bgp_debug.h"
#include "bgpd/bgp_errors.h"
#include "bgpd/bgp_filter.h"
#include "bgpd/bgp_zebra.h"
#include "bgpd/bgp_packet.h"
#include "bgpd/bgp_keepalives.h"
#include "bgpd/bgp_network.h"
#include "bgpd/bgp_errors.h"
#include "bgpd/bgp_script.h"
#include "bgpd/bgp_evpn_mh.h"
#include "bgpd/bgp_nhg.h"
#include "bgpd/bgp_routemap_nb.h"
#include "bgpd/bgp_community_alias.h"

DEFINE_HOOK(bgp_hook_config_write_vrf, (struct vty *vty, struct vrf *vrf),
	    (vty, vrf));

#ifdef ENABLE_BGP_VNC
#include "bgpd/rfapi/rfapi_backend.h"
#endif

DEFINE_HOOK(bgp_hook_vrf_update, (struct vrf *vrf, bool enabled),
	    (vrf, enabled));

/* bgpd options, we use GNU getopt library. */
static const struct option longopts[] = { { "bgp_port", required_argument, NULL, 'p' },
					  { "listenon", required_argument, NULL, 'l' },
					  { "no_kernel", no_argument, NULL, 'n' },
					  { "skip_runas", no_argument, NULL, 'S' },
					  { "ecmp", required_argument, NULL, 'e' },
					  { "int_num", required_argument, NULL, 'I' },
					  { "no_zebra", no_argument, NULL, 'Z' },
					  { "socket_size", required_argument, NULL, 's' },
					  { "v6-with-v4-nexthops", no_argument, NULL, 'x' },
					  { 0 } };

/* signal definitions */
void sighup(void);
void sigint(void);
void sigusr1(void);

static void bgp_exit(int);
static void bgp_vrf_terminate(void);

static struct frr_signal_t bgp_signals[] = {
	{
		.signal = SIGHUP,
		.handler = &sighup,
	},
	{
		.signal = SIGUSR1,
		.handler = &sigusr1,
	},
	{
		.signal = SIGINT,
		.handler = &sigint,
	},
	{
		.signal = SIGTERM,
		.handler = &sigint,
	},
};

/* privileges */
static zebra_capabilities_t _caps_p[] = {ZCAP_BIND, ZCAP_NET_RAW,
					 ZCAP_NET_ADMIN, ZCAP_SYS_ADMIN};

struct zebra_privs_t bgpd_privs = {
#if defined(FRR_USER) && defined(FRR_GROUP)
	.user = FRR_USER,
	.group = FRR_GROUP,
#endif
#ifdef VTY_GROUP
	.vty_group = VTY_GROUP,
#endif
	.caps_p = _caps_p,
	.cap_num_p = array_size(_caps_p),
	.cap_num_i = 0,
};

static struct frr_daemon_info bgpd_di;

/* SIGHUP handler. */
void sighup(void)
{
	zlog_info("SIGHUP received, ignoring");

	return;

	/*
	 * This is turned off for the moment.  There is all
	 * sorts of config turned off by bgp_terminate
	 * that is not setup properly again in bgp_reset.
	 * I see no easy way to do this nor do I see that
	 * this is a desirable way to reload config
	 * given the yang work.
	 */
	/* Terminate all thread. */
	/*
	 * bgp_terminate();
	 * bgp_reset();
	 * zlog_info("bgpd restarting!");

	 * Reload config file.
	 * vty_read_config(NULL, bgpd_di.config_file, config_default);
	 */
	/* Try to return to normal operation. */
}

/* SIGINT handler. */
FRR_NORETURN void sigint(void)
{
	zlog_notice("Terminating on signal");
	assert(bm->terminating == false);
	bm->terminating = true;	/* global flag that shutting down */

	/* Disable BFD events to avoid wasting processing. */
	bfd_protocol_integration_set_shutdown(true);

	bgp_terminate();

	bgp_exit(0);

	/*
	 * This is being done after bgp_exit because items may be removed
	 * from the connection_fifo
	 */
	peer_connection_fifo_fini(&bm->connection_fifo);
	event_cancel(&bm->e_process_packet);
	pthread_mutex_destroy(&bm->peer_connection_mtx);

	exit(0);
}

/* SIGUSR1 handler. */
void sigusr1(void)
{
	zlog_rotate();
}

/*
  Try to free up allocations we know about so that diagnostic tools such as
  valgrind are able to better illuminate leaks.

  Zebra route removal and protocol teardown are not meant to be done here.
  For example, "retain_mode" may be set.
*/
static FRR_NORETURN void bgp_exit(int status)
{
	struct bgp *bgp, *bgp_default, *bgp_evpn;
	struct listnode *node, *nnode;

	/* it only makes sense for this to be called on a clean exit */
	assert(status == 0);

	frr_early_fini();

	bgp_close();

	bgp_default = bgp_get_default();
	bgp_evpn = bgp_get_evpn();

	/* reverse bgp_master_init */
	for (ALL_LIST_ELEMENTS(bm->bgp, node, nnode, bgp)) {
		if (bgp_default == bgp || bgp_evpn == bgp)
			continue;
		bgp_delete(bgp);
	}
	if (bgp_evpn && bgp_evpn != bgp_default)
		bgp_delete(bgp_evpn);
	if (bgp_default)
		bgp_delete(bgp_default);

	bgp_evpn_mh_finish();
	bgp_nhg_finish();

	zebra_announce_fini(&bm->zebra_announce_head);
	zebra_l2_vni_fini(&bm->zebra_l2_vni_head);
	zebra_l3_vni_fini(&bm->zebra_l3_vni_head);

	/* reverse bgp_dump_init */
	bgp_dump_finish();

	/* BGP community aliases */
	bgp_community_alias_finish();

	/* reverse bgp_route_init */
	bgp_route_finish();

	/* cleanup route maps */
	bgp_route_map_terminate();

	/* reverse bgp_attr_init */
	bgp_attr_finish();

	/* reverse bgp_labels_init */
	bgp_labels_finish();

	/* stop pthreads */
	bgp_pthreads_finish();

	/* reverse access_list_init */
	access_list_add_hook(NULL);
	access_list_delete_hook(NULL);
	access_list_reset();

	/* reverse bgp_filter_init */
	as_list_add_hook(NULL);
	as_list_delete_hook(NULL);
	bgp_filter_reset();

	/* reverse prefix_list_init */
	prefix_list_add_hook(NULL);
	prefix_list_delete_hook(NULL);
	prefix_list_reset();

	/* reverse community_list_init */
	community_list_terminate(bgp_clist);

	bgp_vrf_terminate();
#ifdef ENABLE_BGP_VNC
	vnc_zebra_destroy();
#endif
	bgp_zebra_destroy();

	bf_free(bm->rd_idspace);
	list_delete(&bm->bgp);
	list_delete(&bm->addresses);

	bgp_lp_finish();

	memset(bm, 0, sizeof(*bm));

	frr_fini();
	exit(status);
}

static int bgp_vrf_new(struct vrf *vrf)
{
	if (BGP_DEBUG(zebra, ZEBRA))
		zlog_debug("VRF Created: %s(%u)", vrf->name, vrf->vrf_id);

	return 0;
}

static int bgp_vrf_delete(struct vrf *vrf)
{
	if (BGP_DEBUG(zebra, ZEBRA))
		zlog_debug("VRF Deletion: %s(%u)", vrf->name, vrf->vrf_id);

	return 0;
}

static int bgp_vrf_enable(struct vrf *vrf)
{
	struct bgp *bgp;
	vrf_id_t old_vrf_id;

	if (BGP_DEBUG(zebra, ZEBRA))
		zlog_debug("VRF enable add %s id %u", vrf->name, vrf->vrf_id);

	bgp = bgp_lookup_by_name(vrf->name);
	if (bgp && bgp->vrf_id != vrf->vrf_id) {
		old_vrf_id = bgp->vrf_id;
		/* We have instance configured, link to VRF and make it "up". */
		bgp_vrf_link(bgp, vrf);

		bgp_handle_socket(bgp, vrf, old_vrf_id, true);
		bgp_instance_up(bgp);
		hook_call(bgp_hook_vrf_update, vrf, true);
		vpn_leak_zebra_vrf_label_update(bgp, AFI_IP);
		vpn_leak_zebra_vrf_label_update(bgp, AFI_IP6);
		vpn_leak_zebra_vrf_sid_update(bgp, AFI_IP);
		vpn_leak_zebra_vrf_sid_update(bgp, AFI_IP6);
		vpn_leak_postchange(BGP_VPN_POLICY_DIR_TOVPN, AFI_IP,
				    bgp_get_default(), bgp);
		vpn_leak_postchange(BGP_VPN_POLICY_DIR_FROMVPN, AFI_IP,
				    bgp_get_default(), bgp);
		vpn_leak_postchange(BGP_VPN_POLICY_DIR_TOVPN, AFI_IP6,
				    bgp_get_default(), bgp);
		vpn_leak_postchange(BGP_VPN_POLICY_DIR_FROMVPN, AFI_IP6,
				    bgp_get_default(), bgp);
	}

	return 0;
}

static int bgp_vrf_disable(struct vrf *vrf)
{
	struct bgp *bgp;

	if (vrf->vrf_id == VRF_DEFAULT)
		return 0;

	if (BGP_DEBUG(zebra, ZEBRA))
		zlog_debug("VRF disable %s id %d", vrf->name, vrf->vrf_id);

	bgp = bgp_lookup_by_name_filter(vrf->name, false);
	if (bgp) {

		vpn_leak_zebra_vrf_label_withdraw(bgp, AFI_IP);
		vpn_leak_zebra_vrf_label_withdraw(bgp, AFI_IP6);
		vpn_leak_prechange(BGP_VPN_POLICY_DIR_TOVPN, AFI_IP,
				   bgp_get_default(), bgp);
		vpn_leak_prechange(BGP_VPN_POLICY_DIR_FROMVPN, AFI_IP,
				   bgp_get_default(), bgp);
		vpn_leak_prechange(BGP_VPN_POLICY_DIR_TOVPN, AFI_IP6,
				   bgp_get_default(), bgp);
		vpn_leak_prechange(BGP_VPN_POLICY_DIR_FROMVPN, AFI_IP6,
				   bgp_get_default(), bgp);

		bgp_handle_socket(bgp, vrf, VRF_UNKNOWN, false);
		/* We have instance configured, unlink from VRF and make it
		 * "down". */
		bgp_instance_down(bgp);
		bgp_vrf_unlink(bgp, vrf);
		hook_call(bgp_hook_vrf_update, vrf, false);
	}

	/* Note: This is a callback, the VRF will be deleted by the caller. */
	return 0;
}

static int bgp_vrf_config_write(struct vty *vty)
{
	struct vrf *vrf;

	RB_FOREACH (vrf, vrf_name_head, &vrfs_by_name) {
		if (vrf->vrf_id == VRF_DEFAULT) {
			vty_out(vty, "!\n");
			continue;
		}
		vty_out(vty, "vrf %s\n", vrf->name);

		hook_call(bgp_hook_config_write_vrf, vty, vrf);

		vty_out(vty, "exit-vrf\n!\n");
	}

	return 0;
}

static void bgp_vrf_init(void)
{
	vrf_init(bgp_vrf_new, bgp_vrf_enable, bgp_vrf_disable, bgp_vrf_delete);
	vrf_cmd_init(bgp_vrf_config_write);
}

static void bgp_vrf_terminate(void)
{
	vrf_terminate();
}

static const struct frr_yang_module_info *const bgpd_yang_modules[] = {
	&frr_filter_info,
	&frr_interface_info,
	&frr_route_map_info,
	&frr_vrf_info,
	&frr_bgp_route_map_info,
};

/* clang-format off */
FRR_DAEMON_INFO(bgpd, BGP,
	.vty_port = BGP_VTY_PORT,
	.proghelp = "Implementation of the BGP routing protocol.",

	.signals = bgp_signals,
	.n_signals = array_size(bgp_signals),

	.privs = &bgpd_privs,

	.yang_modules = bgpd_yang_modules,
	.n_yang_modules = array_size(bgpd_yang_modules),
);
/* clang-format on */

#define DEPRECATED_OPTIONS ""

/* Main routine of bgpd. Treatment of argument and start bgp finite
   state machine is handled at here. */
int main(int argc, char **argv)
{
	int opt;
	int tmp_port;

	int bgp_port = BGP_PORT_DEFAULT;
	struct list *addresses = list_new();
	int no_fib_flag = 0;
	int no_zebra_flag = 0;
	int skip_runas = 0;
	int instance = 0;
	int buffer_size = BGP_SOCKET_SNDBUF_SIZE;
	char *address;
	struct listnode *node;
	bool v6_with_v4_nexthops = false;

	addresses->cmp = (int (*)(void *, void *))strcmp;

	frr_preinit(&bgpd_di, argc, argv);
	frr_opt_add("p:l:SnZe:I:s:x" DEPRECATED_OPTIONS, longopts,
		    "  -p, --bgp_port           Set BGP listen port number (0 means do not listen).\n"
		    "  -l, --listenon           Listen on specified address (implies -n)\n"
		    "  -n, --no_kernel          Do not install route to kernel.\n"
		    "  -Z, --no_zebra           Do not communicate with Zebra.\n"
		    "  -S, --skip_runas         Skip capabilities checks, and changing user and group IDs.\n"
		    "  -e, --ecmp               Specify ECMP to use.\n"
		    "  -I, --int_num            Set instance number (label-manager)\n"
		    "  -s, --socket_size        Set BGP peer socket send buffer size\n"
		    "  -x, --v6-with-v4-nexthop Allow BGP to form v6 neighbors using v4 nexthops\n");

	/* Command line argument treatment. */
	while (1) {
		opt = frr_getopt(argc, argv, 0);

		if (opt && opt < 128 && strchr(DEPRECATED_OPTIONS, opt)) {
			fprintf(stderr,
				"The -%c option no longer exists.\nPlease refer to the manual.\n",
				opt);
			continue;
		}

		if (opt == EOF)
			break;

		switch (opt) {
		case 0:
			break;
		case 'p':
			tmp_port = atoi(optarg);
			if (tmp_port < 0 || tmp_port > 0xffff)
				bgp_port = BGP_PORT_DEFAULT;
			else
				bgp_port = tmp_port;
			break;
		case 'e': {
			unsigned long int parsed_multipath =
				strtoul(optarg, NULL, 10);
			if (parsed_multipath == 0
			    || parsed_multipath > MULTIPATH_NUM
			    || parsed_multipath > UINT_MAX) {
				flog_err(
					EC_BGP_MULTIPATH,
					"Multipath Number specified must be less than %u and greater than 0",
					MULTIPATH_NUM);
				return 1;
			}
			multipath_num = parsed_multipath;
			break;
		}
		case 'l':
			listnode_add_sort_nodup(addresses, optarg);
			break;
		case 'n':
			no_fib_flag = 1;
			break;
		case 'Z':
			no_zebra_flag = 1;
			break;
		case 'S':
			skip_runas = 1;
			break;
		case 'I':
			instance = atoi(optarg);
			if (instance > (unsigned short)-1)
				zlog_err("Instance %i out of range (0..%u)",
					 instance, (unsigned short)-1);
			break;
		case 's':
			buffer_size = atoi(optarg);
			break;
		case 'x':
			v6_with_v4_nexthops = true;
			break;
		default:
			frr_help_exit(1);
		}
	}
	if (skip_runas)
		memset(&bgpd_privs, 0, sizeof(bgpd_privs));

	/* BGP master init. */
	bgp_master_init(frr_init(), buffer_size, addresses);
	bm->startup_time = monotime(NULL);
	bm->port = bgp_port;
	bm->v6_with_v4_nexthops = v6_with_v4_nexthops;
	if (bgp_port == 0)
		bgp_option_set(BGP_OPT_NO_LISTEN);
	if (no_fib_flag || no_zebra_flag)
		bgp_option_set(BGP_OPT_NO_FIB);
	if (no_zebra_flag)
		bgp_option_set(BGP_OPT_NO_ZEBRA);
	if (bgpd_di.graceful_restart)
		SET_FLAG(bm->flags, BM_FLAG_GRACEFUL_RESTART);

	bgp_error_init();
	/* Initializations. */
	libagentx_init();
	bgp_vrf_init();


#ifdef HAVE_SCRIPTING
	bgp_script_init();
#endif

	/* BGP related initialization.  */
	bgp_init((unsigned short)instance);

	if (list_isempty(bm->addresses)) {
		snprintf(bgpd_di.startinfo, sizeof(bgpd_di.startinfo),
			 ", bgp@<all>:%d", bm->port);
	} else {
		for (ALL_LIST_ELEMENTS_RO(bm->addresses, node, address))
			snprintf(bgpd_di.startinfo + strlen(bgpd_di.startinfo),
				 sizeof(bgpd_di.startinfo)
					 - strlen(bgpd_di.startinfo),
				 ", bgp@%s:%d", address, bm->port);
	}

	bgp_if_init();

	frr_config_fork();
	/* must be called after fork() */
	bgp_gr_apply_running_config();
	bgp_pthreads_run();
	frr_run(bm->master);

	/* Not reached. */
	return 0;
}
