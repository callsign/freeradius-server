/*
 * proto_tacacs.c	TACACS+ processing.
 *
 * Version:	$Id$
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * @copyright 2017 The FreeRADIUS server project
 * @copyright 2017 Network RADIUS SARL <info@networkradius.com>
 */
#include <freeradius-devel/radiusd.h>
#include <freeradius-devel/modules.h>
#include <freeradius-devel/unlang.h>
#include <freeradius-devel/protocol.h>
#include <freeradius-devel/process.h>
#include <freeradius-devel/state.h>
#include <freeradius-devel/rad_assert.h>

#include "tacacs.h"

static fr_dict_t const *dict_freeradius;
static fr_dict_t const *dict_radius;
static fr_dict_t const *dict_tacacs;

extern fr_dict_autoload_t proto_tacacs_dict[];
fr_dict_autoload_t proto_tacacs_dict[] = {
	{ .out = &dict_freeradius, .proto = "freeradius" },
	{ .out = &dict_radius, .proto = "radius" },
	{ .out = &dict_tacacs, .proto = "tacacs" },

	{ NULL }
};

static fr_dict_attr_t const *attr_auth_type;

static fr_dict_attr_t const *attr_state;

fr_dict_attr_t const *attr_tacacs_accounting_flags;
fr_dict_attr_t const *attr_tacacs_accounting_status;
fr_dict_attr_t const *attr_tacacs_action;
fr_dict_attr_t const *attr_tacacs_authentication_flags;
fr_dict_attr_t const *attr_tacacs_authentication_method;
fr_dict_attr_t const *attr_tacacs_authentication_service;
fr_dict_attr_t const *attr_tacacs_authentication_status;
fr_dict_attr_t const *attr_tacacs_authentication_type;
fr_dict_attr_t const *attr_tacacs_authorization_status;
fr_dict_attr_t const *attr_tacacs_client_port;
fr_dict_attr_t const *attr_tacacs_data;
fr_dict_attr_t const *attr_tacacs_packet_type;
fr_dict_attr_t const *attr_tacacs_privilege_level;
fr_dict_attr_t const *attr_tacacs_remote_address;
fr_dict_attr_t const *attr_tacacs_sequence_number;
fr_dict_attr_t const *attr_tacacs_server_message;
fr_dict_attr_t const *attr_tacacs_session_id;
fr_dict_attr_t const *attr_tacacs_user_message;
fr_dict_attr_t const *attr_tacacs_user_name;
fr_dict_attr_t const *attr_tacacs_version_minor;

extern fr_dict_attr_autoload_t proto_tacacs_dict_attr[];
fr_dict_attr_autoload_t proto_tacacs_dict_attr[] = {
	{ .out = &attr_auth_type, .name = "Auth-Type", .type = FR_TYPE_UINT32, .dict = &dict_freeradius },
	{ .out = &attr_state, .name = "State", .type = FR_TYPE_OCTETS, .dict = &dict_radius },
	{ .out = &attr_tacacs_accounting_flags, .name = "TACACS-Accounting-Flags", .type = FR_TYPE_UINT8, .dict = &dict_tacacs },
	{ .out = &attr_tacacs_accounting_status, .name = "TACACS-Accounting-Status", .type = FR_TYPE_UINT8, .dict = &dict_tacacs },
	{ .out = &attr_tacacs_action, .name = "TACACS-Action", .type = FR_TYPE_UINT8, .dict = &dict_tacacs },
	{ .out = &attr_tacacs_authentication_flags, .name = "TACACS-Authentication-Flags", .type = FR_TYPE_UINT8, .dict = &dict_tacacs },
	{ .out = &attr_tacacs_authentication_method, .name = "TACACS-Authentication-Method", .type = FR_TYPE_UINT8, .dict = &dict_tacacs },
	{ .out = &attr_tacacs_authentication_service, .name = "TACACS-Authentication-Service", .type = FR_TYPE_UINT8, .dict = &dict_tacacs },
	{ .out = &attr_tacacs_authentication_status, .name = "TACACS-Authentication-Status", .type = FR_TYPE_UINT8, .dict = &dict_tacacs },
	{ .out = &attr_tacacs_authentication_type, .name = "TACACS-Authentication-Type", .type = FR_TYPE_UINT8, .dict = &dict_tacacs },
	{ .out = &attr_tacacs_authorization_status, .name = "TACACS-Authorization-Status", .type = FR_TYPE_UINT8, .dict = &dict_tacacs },
	{ .out = &attr_tacacs_client_port, .name = "TACACS-Client-Port", .type = FR_TYPE_STRING, .dict = &dict_tacacs },
	{ .out = &attr_tacacs_data, .name = "TACACS-Data", .type = FR_TYPE_STRING, .dict = &dict_tacacs },
	{ .out = &attr_tacacs_packet_type, .name = "TACACS-Packet-Type", .type = FR_TYPE_UINT8, .dict = &dict_tacacs },
	{ .out = &attr_tacacs_privilege_level, .name = "TACACS-Privilege-Level", .type = FR_TYPE_UINT8, .dict = &dict_tacacs },
	{ .out = &attr_tacacs_remote_address, .name = "TACACS-Remote-Address", .type = FR_TYPE_STRING, .dict = &dict_tacacs },
	{ .out = &attr_tacacs_sequence_number, .name = "TACACS-Sequence-Number", .type = FR_TYPE_UINT8, .dict = &dict_tacacs },
	{ .out = &attr_tacacs_server_message, .name = "TACACS-Server-Message", .type = FR_TYPE_STRING, .dict = &dict_tacacs },
	{ .out = &attr_tacacs_session_id, .name = "TACACS-Session-Id", .type = FR_TYPE_UINT32, .dict = &dict_tacacs },
	{ .out = &attr_tacacs_user_message, .name = "TACACS-User-Message", .type = FR_TYPE_STRING, .dict = &dict_tacacs },
	{ .out = &attr_tacacs_user_name, .name = "TACACS-User-Name", .type = FR_TYPE_STRING, .dict = &dict_tacacs },
	{ .out = &attr_tacacs_version_minor, .name = "TACACS-Version-Minor", .type = FR_TYPE_UINT8, .dict = &dict_tacacs },
	{ NULL }
};

/*
 *	Debug the packet if requested - cribbed from common_packet_debug
 */
static void tacacs_packet_debug(REQUEST *request, RADIUS_PACKET *packet, bool received)
{
	char src_ipaddr[FR_IPADDR_STRLEN];
	char dst_ipaddr[FR_IPADDR_STRLEN];

	if (!packet) return;
	if (!RDEBUG_ENABLED) return;

	RDEBUG("%s %s Id %u from %s%s%s:%i to %s%s%s:%i "
	       "length %zu",
	       received ? "Received" : "Sending",
	       tacacs_lookup_packet_code(request->packet),
	       tacacs_session_id(request->packet),
	       packet->src_ipaddr.af == AF_INET6 ? "[" : "",
	       fr_inet_ntop(src_ipaddr, sizeof(src_ipaddr), &packet->src_ipaddr),
	       packet->src_ipaddr.af == AF_INET6 ? "]" : "",
	       packet->src_port,
	       packet->dst_ipaddr.af == AF_INET6 ? "[" : "",
	       fr_inet_ntop(dst_ipaddr, sizeof(dst_ipaddr), &packet->dst_ipaddr),
	       packet->dst_ipaddr.af == AF_INET6 ? "]" : "",
	       packet->dst_port,
	       packet->data_len);

	log_request_pair_list(L_DBG_LVL_1, request, packet->vps, NULL);
}

static void tacacs_status(REQUEST * const request, rlm_rcode_t rcode)
{
	VALUE_PAIR *vp;

	switch (tacacs_type(request->packet)) {
	case TAC_PLUS_AUTHEN:
		switch (rcode) {
		case RLM_MODULE_OK:
			MEM(pair_update_reply(&vp, attr_tacacs_authentication_status) >= 0);
			fr_pair_value_from_str(vp, "Pass", -1);
			break;

		case RLM_MODULE_FAIL:
		case RLM_MODULE_REJECT:
		case RLM_MODULE_USERLOCK:
			MEM(pair_update_reply(&vp, attr_tacacs_authentication_status) >= 0);
			fr_pair_value_from_str(vp, "Fail", -1);
			break;

		case RLM_MODULE_INVALID:
			MEM(pair_update_reply(&vp, attr_tacacs_authentication_status) >= 0);
			fr_pair_value_from_str(vp, "Error", -1);
			break;

		case RLM_MODULE_HANDLED:	/* unlang set status */
			return;

		default:
noop:
			WARN("ignoring request to add TACACS status with code %d", rcode);
			return;
		}
		break;

	case TAC_PLUS_AUTHOR:
		switch (rcode) {
		case RLM_MODULE_OK:
			MEM(pair_update_reply(&vp, attr_tacacs_authorization_status) >= 0);
			fr_pair_value_from_str(vp, "Pass-Repl", -1);
			break;

		case RLM_MODULE_FAIL:
		case RLM_MODULE_REJECT:
		case RLM_MODULE_USERLOCK:
			MEM(pair_update_reply(&vp, attr_tacacs_authorization_status) >= 0);
			fr_pair_value_from_str(vp, "Fail", -1);
			break;

		case RLM_MODULE_INVALID:
			MEM(pair_update_reply(&vp, attr_tacacs_authorization_status) >= 0);
			fr_pair_value_from_str(vp, "Error", -1);
			break;

		default:
			goto noop;
		}
		break;

	case TAC_PLUS_ACCT:
		switch (rcode) {
		case RLM_MODULE_OK:
			MEM(pair_update_reply(&vp, attr_tacacs_accounting_status) >= 0);
			fr_pair_value_from_str(vp, "Success", -1);
			break;

		case RLM_MODULE_FAIL:
		case RLM_MODULE_REJECT:
		case RLM_MODULE_USERLOCK:
		case RLM_MODULE_INVALID:
			MEM(pair_update_reply(&vp, attr_tacacs_accounting_status) >= 0);
			fr_pair_value_from_str(vp, "Error", -1);
			break;

		default:
			goto noop;
		}
		break;
	}
}

static void state_add(REQUEST *request, RADIUS_PACKET *packet)
{
	VALUE_PAIR *vp;
	uint32_t session_id;
	uint8_t buf[16] = { 0 };	/* FIXME state.c:sizeof(struct state_comp) */

	rad_assert(sizeof(request->listener) + sizeof(vp->vp_uint32) <= sizeof(buf));		//-V568

	/* session_id is per TCP connection */
	memcpy(&buf[0], &request->listener, sizeof(request->listener));				//-V568

	session_id = tacacs_session_id(request->packet);
	memcpy(&buf[sizeof(buf) - sizeof(session_id)], &session_id, sizeof(session_id));

	MEM(vp = fr_pair_afrom_da(packet, attr_state));
	fr_pair_value_memcpy(vp, (uint8_t const *)buf, sizeof(buf));
	fr_pair_add(&packet->vps, vp);
}

static void tacacs_running(REQUEST *request, fr_state_signal_t action)
{
	rlm_rcode_t		rcode;
	CONF_SECTION		*unlang;
	fr_dict_enum_t const	*dv = NULL;
	VALUE_PAIR *vp,		*auth_type;
	vp_cursor_t		cursor;
	int			rc;

	REQUEST_VERIFY(request);

	switch (action) {
	case FR_SIGNAL_CANCEL:
		goto done;

	default:
		break;
	}

	switch (request->request_state) {
	case REQUEST_INIT:
		rc = tacacs_decode(request->packet);
		if (rc == -2)	/* client abort no reply */
			goto done;
		else if (rc < 0) {
			fr_strerror_printf("Failed decoding TACACS+ packet");
			goto setup_send;
		}

		if (RDEBUG_ENABLED) tacacs_packet_debug(request, request->packet, true);

		request->server_cs = request->listener->server_cs;
		request->component = "tacacs";

		unlang = cf_section_find(request->server_cs, "recv", tacacs_lookup_packet_code(request->packet));
		if (!unlang) unlang = cf_section_find(request->server_cs, "recv", "*");
		if (!unlang) {
			REDEBUG("Failed to find 'recv' section");
			goto setup_send;
		}

		/* FIXME only for seq_id greater than 1 */
		if (tacacs_type(request->packet) == TAC_PLUS_AUTHEN) {
			state_add(request, request->packet);
			fr_state_to_request(global_state, request, request->packet);
		}

		RDEBUG("Running 'recv %s' from file %s", cf_section_name2(unlang), cf_filename(unlang));
		unlang_push_section(request, unlang, RLM_MODULE_REJECT, UNLANG_TOP_FRAME);

		request->request_state = REQUEST_RECV;
		/* FALL-THROUGH */

	case REQUEST_RECV:
		rcode = unlang_interpret_continue(request);

		if (request->master_state == REQUEST_STOP_PROCESSING) {
stop_processing:
			if (tacacs_type(request->packet) == TAC_PLUS_AUTHEN)
				fr_state_discard(global_state, request, request->packet);
			goto done;
		}

		if (rcode == RLM_MODULE_YIELD) return;

		rad_assert(request->log.unlang_indent == 0);

		switch (rcode) {
		case RLM_MODULE_NOOP:
		case RLM_MODULE_NOTFOUND:
		case RLM_MODULE_OK:
		case RLM_MODULE_UPDATED:
			break;

		case RLM_MODULE_HANDLED:
			goto setup_send;

		case RLM_MODULE_FAIL:
		case RLM_MODULE_INVALID:
		case RLM_MODULE_REJECT:
		case RLM_MODULE_USERLOCK:
		default:
			tacacs_status(request, rcode);
			goto setup_send;
		}

		/*
		 *	Find Auth-Type, and complain if they have too many.
		 */
		fr_pair_cursor_init(&cursor, &request->control);
		auth_type = NULL;
		while ((vp = fr_pair_cursor_next_by_da(&cursor, attr_auth_type, TAG_ANY)) != NULL) {
			if (!auth_type) {
				auth_type = vp;
				continue;
			}

			RWDEBUG("Ignoring extra Auth-Type = %s",
				fr_dict_enum_alias_by_value(auth_type->da, &vp->data));
		}

		/*
		 *	No Auth-Type, force it to reject.
		 */
		if (!auth_type) {
			REDEBUG2("No Auth-Type available: rejecting the user.");
			tacacs_status(request, RLM_MODULE_REJECT);
			goto setup_send;
		}

		/*
		 *	Handle hard-coded Accept and Reject.
		 */
		if (auth_type->vp_uint32 == FR_AUTH_TYPE_ACCEPT) {
			RDEBUG2("Auth-Type = Accept, allowing user");
			tacacs_status(request, RLM_MODULE_OK);
			goto setup_send;
		}

		if (auth_type->vp_uint32 == FR_AUTH_TYPE_REJECT) {
			RDEBUG2("Auth-Type = Reject, rejecting user");
			tacacs_status(request, RLM_MODULE_REJECT);
			goto setup_send;
		}

		/*
		 *	Find the appropriate Auth-Type by name.
		 */
		vp = auth_type;
		dv = fr_dict_enum_by_value(vp->da, &vp->data);
		if (!dv) {
			REDEBUG2("Unknown Auth-Type %d found: rejecting the user", vp->vp_uint32);
			tacacs_status(request, RLM_MODULE_FAIL);
			goto setup_send;
		}

		unlang = cf_section_find(request->server_cs, "process", dv->alias);
		if (!unlang) {
			REDEBUG2("No 'process %s' section found: rejecting the user.", dv->alias);
			tacacs_status(request, RLM_MODULE_FAIL);
			goto setup_send;
		}

		RDEBUG("Running 'process %s' from file %s", cf_section_name2(unlang), cf_filename(unlang));
		unlang_push_section(request, unlang, RLM_MODULE_NOTFOUND, UNLANG_TOP_FRAME);

		request->request_state = REQUEST_PROCESS;
		/* FALL-THROUGH */

	case REQUEST_PROCESS:
		rcode = unlang_interpret_continue(request);

		if (request->master_state == REQUEST_STOP_PROCESSING) goto stop_processing;

		if (rcode == RLM_MODULE_YIELD) return;

		rad_assert(request->log.unlang_indent == 0);

		switch (rcode) {
			/*
			 *	An authentication module FAIL
			 *	return code, or any return code that
			 *	is not expected from authentication,
			 *	is the same as an explicit REJECT!
			 */
		case RLM_MODULE_FAIL:
		case RLM_MODULE_INVALID:
		case RLM_MODULE_NOOP:
		case RLM_MODULE_NOTFOUND:
		case RLM_MODULE_REJECT:
		case RLM_MODULE_UPDATED:
		case RLM_MODULE_USERLOCK:
		default:
			RDEBUG2("Failed to authenticate the user");
			tacacs_status(request, RLM_MODULE_FAIL);
			goto setup_send;

		case RLM_MODULE_OK:
			tacacs_status(request, RLM_MODULE_OK);
			break;

		case RLM_MODULE_HANDLED:
			goto setup_send;
		}

setup_send:
		unlang = NULL;
		if (dv) {
			unlang = cf_section_find(request->server_cs, "send", tacacs_lookup_packet_code(request->packet));
		}
		if (!unlang) unlang = cf_section_find(request->server_cs, "send", "*");
		if (!unlang) goto send_reply;

		RDEBUG("Running 'send %s' from file %s", cf_section_name2(unlang), cf_filename(unlang));
		unlang_push_section(request, unlang, RLM_MODULE_NOOP, UNLANG_TOP_FRAME);

		request->request_state = REQUEST_SEND;
		/* FALL-THROUGH */

	case REQUEST_SEND:
		rcode = unlang_interpret_continue(request);

		if (request->master_state == REQUEST_STOP_PROCESSING) goto stop_processing;

		if (rcode == RLM_MODULE_YIELD) return;

		rad_assert(request->log.unlang_indent == 0);

send_reply:
		gettimeofday(&request->reply->timestamp, NULL);

		if (tacacs_type(request->packet) == TAC_PLUS_AUTHEN) {
			vp = fr_pair_find_by_da(request->reply->vps, attr_tacacs_authentication_status, TAG_ANY);
			if (vp) {
				switch ((tacacs_authen_reply_status_t)vp->vp_uint8) {
				case TAC_PLUS_AUTHEN_STATUS_PASS:
				case TAC_PLUS_AUTHEN_STATUS_FAIL:
				case TAC_PLUS_AUTHEN_STATUS_RESTART:
				case TAC_PLUS_AUTHEN_STATUS_ERROR:
				case TAC_PLUS_AUTHEN_STATUS_FOLLOW:
					fr_state_discard(global_state, request, request->packet);
					break;
				default:
					vp = fr_pair_find_by_da(request->packet->vps,
								attr_tacacs_sequence_number, TAG_ANY);
					if (!vp) {
						REDEBUG("No sequence number found");
						goto done;
					}

					/* authentication would continue but seq_no cannot continue */
					if (vp->vp_uint8 == 253) {
						RWARN("Sequence number would wrap, restarting authentication");
						fr_state_discard(global_state, request, request->packet);
						fr_pair_list_free(&request->reply->vps);

						MEM(pair_update_reply(&vp, attr_tacacs_authentication_status) >= 0);
						vp->vp_uint8 = TAC_PLUS_AUTHEN_STATUS_RESTART;
					} else {
						state_add(request, request->reply);
						request->reply->code = 1;	/* FIXME: util.c:request_verify() */
						fr_request_to_state(global_state, request,
								    request->packet, request->reply);
					}
				}
			} else {
				fr_state_discard(global_state, request, request->packet);
			}
		}

		if (RDEBUG_ENABLED) tacacs_packet_debug(request, request->reply, false);

		if (tacacs_send(request->reply, request->packet, request->client->secret) < 0) {
			RPEDEBUG("Failed sending TACACS reply");
			goto done;
		}

		/* FALL-THROUGH */

done:
	default:
		(void) fr_heap_extract(request->backlog, request);
		request_delete(request);
		break;
	}
}

static void tacacs_queued(REQUEST *request, fr_state_signal_t action)
{
	REQUEST_VERIFY(request);

	switch (action) {
	case FR_SIGNAL_RUN:
		request->process = tacacs_running;
		request->process(request, action);
		break;

	case FR_SIGNAL_CANCEL:
		(void) fr_heap_extract(request->backlog, request);
		request_delete(request);
		break;

	default:
		break;
	}
}

/*
 *	Check if an incoming request is "ok"
 *
 *	It takes packets, not requests.  It sees if the packet looks
 *	OK.  If so, it does a number of sanity checks on it.
 */
static int tacacs_socket_recv(rad_listen_t *listener)
{
	int		rcode;
	RADIUS_PACKET	*packet;
	TALLOC_CTX	*ctx;
	REQUEST		*request;
	listen_socket_t *sock = listener->data;
	RADCLIENT	*client = sock->client;

	if (!fr_cond_assert(client != NULL)) return 0;

	if (listener->status != RAD_LISTEN_STATUS_KNOWN) return 0;

	ctx = talloc_pool(listener, main_config.talloc_pool_size);
	if (!ctx) return 0;
	talloc_set_name_const(ctx, "tacacs_listener_pool");

	/*
	 *	Allocate a packet for partial reads.
	 */
	if (!sock->packet) {
		sock->packet = fr_radius_alloc(ctx, false);
		if (!sock->packet) return 0;

		sock->packet->sockfd = listener->fd;
		sock->packet->src_ipaddr = sock->other_ipaddr;
		sock->packet->src_port = sock->other_port;
		sock->packet->dst_ipaddr = sock->my_ipaddr;
		sock->packet->dst_port = sock->my_port;
		sock->packet->proto = sock->proto;
	}

	/*
	 *	Grab the packet currently being processed.
	 */
	packet = sock->packet;

	rcode = tacacs_read_packet(packet, client->secret);
	if (rcode == 0) return 0;	/* partial packet */
	if (rcode == -1) {		/* error reading packet */
		char buffer[256];

		PERROR("Invalid packet from %s port %d, closing socket",
		       fr_inet_ntoh(&packet->src_ipaddr, buffer, sizeof(buffer)), packet->src_port);
	}
	if (rcode < 0) {		/* error or connection reset */
		DEBUG("Client has closed connection");

		listener->status = RAD_LISTEN_STATUS_EOL;
		radius_update_listener(listener);

		return 0;
	}

	request = request_setup(ctx, listener, packet, client, NULL);
	if (!request) {
		talloc_free(ctx);
		return 0;
	}

	request->process = tacacs_queued;
//	request_enqueue(request);

	sock->packet = NULL;	/* we have no need for more partial reads */
	return 1;
}

static int tacacs_socket_error(rad_listen_t *listener, UNUSED int fd)
{
	listener->status = RAD_LISTEN_STATUS_EOL;
	radius_update_listener(listener);

	return 1;
}

static int tacacs_compile_section(CONF_SECTION *server_cs, char const *name1, char const *name2, rlm_components_t component)
{
	CONF_SECTION *cs;
	int ret;

	cs = cf_section_find(server_cs, name1, name2);
	if (!cs) {
		cf_log_err(server_cs, "Failed finding '%s %s { ... }' section of virtual server %s",
			name1, name2, cf_section_name2(server_cs));
		return -1;
	}

	cf_log_debug(cs, "Loading %s %s {...}", name1, name2);

	ret = unlang_compile(cs, component);
	if (ret < 0) {
		cf_log_err(cs, "Failed compiling '%s %s { ... }' section", name1, name2);
		return -1;
	}

	return 0;
}

static int tacacs_listen_compile(CONF_SECTION *server_cs, UNUSED CONF_SECTION *listen_cs)
{
	int rcode;
	CONF_SECTION *subcs = NULL;

	rcode = tacacs_compile_section(server_cs, "recv", "Authentication", MOD_AUTHORIZE);
	if (rcode < 0) return rcode;

	rcode = tacacs_compile_section(server_cs, "send", "Authentication", MOD_POST_AUTH);
	if (rcode < 0) return rcode;

	rcode = tacacs_compile_section(server_cs, "recv", "Authorization", MOD_AUTHORIZE);
	if (rcode < 0) return rcode;

	rcode = tacacs_compile_section(server_cs, "send", "Authorization", MOD_POST_AUTH);
	if (rcode < 0) return rcode;

	rcode = tacacs_compile_section(server_cs, "recv", "Accounting", MOD_PREACCT);
	if (rcode < 0) return rcode;

	rcode = tacacs_compile_section(server_cs, "send", "Accounting", MOD_ACCOUNTING);
	if (rcode < 0) return rcode;

	while ((subcs = cf_section_find_next(server_cs, subcs, "process", NULL))) {
		char const *name2;

		name2 = cf_section_name2(subcs);
		rcode = tacacs_compile_section(server_cs, "process", name2, MOD_AUTHENTICATE);
		if (rcode < 0) return rcode;
	}

	return 0;
}

extern rad_protocol_t proto_tacacs;
rad_protocol_t proto_tacacs = {
	.name		= "tacacs",
	.magic		= RLM_MODULE_INIT,
	.inst_size	= sizeof(listen_socket_t),
	.transports	= TRANSPORT_TCP,
	.tls		= false,
	.compile	= tacacs_listen_compile,
	.parse		= common_socket_parse,
	.open		= common_socket_open,
	.recv		= tacacs_socket_recv,
	.send		= NULL,
	.error		= tacacs_socket_error,
	.print		= common_socket_print,
	.debug		= tacacs_packet_debug,
	.encode		= NULL,
	.decode		= NULL,
};
