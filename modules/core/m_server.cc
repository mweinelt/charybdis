/*
 *  ircd-ratbox: A slightly useful ircd.
 *  m_server.c: Introduces a server.
 *
 *  Copyright (C) 1990 Jarkko Oikarinen and University of Oulu, Co Center
 *  Copyright (C) 1996-2002 Hybrid Development Team
 *  Copyright (C) 2002-2005 ircd-ratbox development team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307
 *  USA
 */

#include <ircd/stdinc.h>
#include <ircd/client.h>		/* client struct */
#include <ircd/hash.h>		/* add_to_client_hash */
#include <ircd/match.h>
#include <ircd/ircd.h>		/* me */
#include <ircd/numeric.h>		/* ERR_xxx */
#include <ircd/s_conf.h>		/* struct ConfItem */
#include <ircd/s_newconf.h>
#include <ircd/logger.h>		/* log level defines */
#include <ircd/s_serv.h>		/* server_estab, check_server */
#include <ircd/s_stats.h>		/* ServerStats */
#include <ircd/scache.h>
#include <ircd/send.h>		/* sendto_one */
#include <ircd/msg.h>
#include <ircd/parse.h>
#include <ircd/modules.h>

using namespace ircd;

static const char server_desc[] =
	"Provides the TS6 commands to introduce a new server to the network";

static void mr_server(struct MsgBuf *, struct Client *, struct Client *, int, const char **);
static void ms_server(struct MsgBuf *, struct Client *, struct Client *, int, const char **);
static void ms_sid(struct MsgBuf *, struct Client *, struct Client *, int, const char **);

static bool bogus_host(const char *host);
static void set_server_gecos(struct Client *, const char *);

struct Message server_msgtab = {
	"SERVER", 0, 0, 0, 0,
	{{mr_server, 4}, mg_reg, mg_ignore, {ms_server, 4}, mg_ignore, mg_reg}
};
struct Message sid_msgtab = {
	"SID", 0, 0, 0, 0,
	{mg_ignore, mg_reg, mg_ignore, {ms_sid, 5}, mg_ignore, mg_reg}
};

mapi_clist_av1 server_clist[] = { &server_msgtab, &sid_msgtab, NULL };

DECLARE_MODULE_AV2(server, NULL, NULL, server_clist, NULL, NULL, NULL, NULL, server_desc);

/*
 * mr_server - SERVER message handler
 *      parv[1] = servername
 *      parv[2] = serverinfo/hopcount
 *      parv[3] = serverinfo
 */
static void
mr_server(struct MsgBuf *msgbuf_p, struct Client *client_p, struct Client *source_p, int parc, const char *parv[])
{
	char info[REALLEN + 1];
	const char *name;
	struct Client *target_p;
	int hop;
	unsigned int required_mask;
	int ret;

	name = parv[1];
	hop = atoi(parv[2]);
	rb_strlcpy(info, parv[3], sizeof(info));

	if (IsHandshake(client_p) && irccmp(client_p->name, name))
	{
		sendto_realops_snomask(SNO_GENERAL, is_remote_connect(client_p) ? L_NETWIDE : L_ALL,
				"Server %s has unexpected name %s",
				client_p->name, name);
		ilog(L_SERVER, "Server %s has unexpected name %s",
				log_client_name(client_p, SHOW_IP), name);
		exit_client(client_p, client_p, client_p, "Server name mismatch");
		return;
	}

	/*
	 * Reject a direct nonTS server connection if we're TS_ONLY -orabidoo
	 */
	if(!DoesTS(client_p))
	{
		sendto_realops_snomask(SNO_GENERAL, L_ALL, "Link %s dropped, non-TS server",
				     client_p->name);
		exit_client(client_p, client_p, client_p, "Non-TS server");
		return;
	}

	if(bogus_host(name))
	{
		exit_client(client_p, client_p, client_p, "Bogus server name");
		return;
	}

	/* Now we just have to call check_server and everything should be
	 * check for us... -A1kmm. */
	ret = check_server(name, client_p);
	switch (ret)
	{
	case 0:
		/* success */
		break;
	case -1:
		if(ConfigFileEntry.warn_no_nline)
		{
			sendto_realops_snomask(SNO_GENERAL, L_ALL,
					     "Unauthorised server connection attempt from %s: "
					     "No entry for servername %s",
					     "[@255.255.255.255]", name);

			ilog(L_SERVER, "Access denied, no connect block for server %s%s",
			     EmptyString(client_p->name) ? name : "",
			     log_client_name(client_p, SHOW_IP));
		}

		exit_client(client_p, client_p, client_p, "Invalid servername.");
		return;
		/* NOT REACHED */
		break;

	case -2:
		sendto_realops_snomask(SNO_GENERAL, is_remote_connect(client_p) ? L_NETWIDE : L_ALL,
				     "Unauthorised server connection attempt from %s: "
				     "Bad credentials for server %s",
				     "[@255.255.255.255]", name);

		ilog(L_SERVER, "Access denied, invalid credentials for server %s%s",
		     EmptyString(client_p->name) ? name : "",
		     log_client_name(client_p, SHOW_IP));

		exit_client(client_p, client_p, client_p, "Invalid credentials.");
		return;
		/* NOT REACHED */
		break;

	case -3:
		sendto_realops_snomask(SNO_GENERAL, L_ALL,
				     "Unauthorised server connection attempt from %s: "
				     "Invalid host for server %s",
				     "[@255.255.255.255]", name);

		ilog(L_SERVER, "Access denied, invalid host for server %s%s",
		     EmptyString(client_p->name) ? name : "",
		     log_client_name(client_p, SHOW_IP));

		exit_client(client_p, client_p, client_p, "Invalid host.");
		return;
		/* NOT REACHED */
		break;

		/* servername is > HOSTLEN */
	case -4:
		sendto_realops_snomask(SNO_GENERAL, L_ALL,
				     "Invalid servername %s from %s",
				     name, "[@255.255.255.255]");
		ilog(L_SERVER, "Access denied, invalid servername from %s",
		     log_client_name(client_p, SHOW_IP));

		exit_client(client_p, client_p, client_p, "Invalid servername.");
		return;
		/* NOT REACHED */
		break;
	case -5:
		sendto_realops_snomask(SNO_GENERAL, L_ALL,
		     "Connection from servername %s requires SSL/TLS but is plaintext",
		     name);
		ilog(L_SERVER, "Access denied, requires SSL/TLS but is plaintext from %s",
		     log_client_name(client_p, SHOW_IP));

		exit_client(client_p, client_p, client_p, "Access denied, requires SSL/TLS but is plaintext");
		return;
	case -6:
		sendto_realops_snomask(SNO_GENERAL, L_ALL,
		     "Connection from servername %s has invalid certificate fingerprint %s",
		     name, client_p->certfp);
		ilog(L_SERVER, "Access denied, invalid certificate fingerprint %s from %s",
		     client_p->certfp, log_client_name(client_p, SHOW_IP));

		exit_client(client_p, client_p, client_p, "Invalid fingerprint.");
		return;
	default:
		sendto_realops_snomask(SNO_GENERAL, L_ALL,
		     "Connection from servername %s rejected, unknown error %d",
		     name, ret);
		ilog(L_SERVER, "Access denied, unknown error %d for server %s%s", ret,
		     EmptyString(client_p->name) ? name : "",
		     log_client_name(client_p, SHOW_IP));

		exit_client(client_p, client_p, client_p, "Unknown error.");
		return;
	}

	/* require TS6 for direct links */
	if(!IsCapable(client_p, CAP_TS6))
	{
		sendto_realops_snomask(SNO_GENERAL, is_remote_connect(client_p) ? L_NETWIDE : L_ALL,
					"Link %s dropped, TS6 protocol is required", name);
		exit_client(client_p, client_p, client_p, "Incompatible TS version");
		return;
	}

	/* check to ensure any "required" caps are set. --nenolod */
	required_mask = serv_capindex.required();
	if (!IsCapable(client_p, required_mask))
	{
		const auto missing(serv_capindex.list(required_mask & ~client_p->localClient->caps));
		sendto_realops_snomask(SNO_GENERAL, is_remote_connect(client_p) ? L_NETWIDE : L_ALL,
					"Link %s dropped, required CAPABs [%s] are missing",
					name, missing.c_str());
		ilog(L_SERVER, "Link %s%s dropped, required CAPABs [%s] are missing",
				EmptyString(client_p->name) ? name : "",
				log_client_name(client_p, SHOW_IP), missing.c_str());
		/* Do not use '[' in the below message because it would cause
		 * it to be considered potentially unsafe (might disclose IP
		 * addresses)
		 */
		sendto_one(client_p, "ERROR :Missing required CAPABs (%s)", missing.c_str());
		exit_client(client_p, client_p, client_p, "Missing required CAPABs");

		return;
	}

	if((target_p = find_server(NULL, name)))
	{
		/*
		 * This link is trying feed me a server that I already have
		 * access through another path -- multiple paths not accepted
		 * currently, kill this link immediately!!
		 *
		 * Rather than KILL the link which introduced it, KILL the
		 * youngest of the two links. -avalon
		 *
		 * Definitely don't do that here. This is from an unregistered
		 * connect - A1kmm.
		 */
		if (target_p->servptr->flags & FLAGS_SERVICE)
		{
			/* Assume any servers introduced by services
			 * are jupes.
			 * -- jilles
			 */
			sendto_one(client_p, "ERROR :Server juped.");
		}
		else
		{
			sendto_realops_snomask(SNO_GENERAL, L_ALL,
					     "Attempt to re-introduce server %s from %s",
					     name, "[@255.255.255.255]");
			ilog(L_SERVER, "Attempt to re-introduce server %s from %s",
					name, log_client_name(client_p, SHOW_IP));

			sendto_one(client_p, "ERROR :Server already exists.");
		}
		exit_client(client_p, client_p, client_p, "Server Exists");
		return;
	}

	if(has_id(client_p) && (target_p = find_id(client_p->id)) != NULL)
	{
		sendto_realops_snomask(SNO_GENERAL, is_remote_connect(client_p) ? L_NETWIDE : L_ALL,
				     "Attempt to re-introduce SID %s from %s%s (already in use by %s)",
				     client_p->id,
				     EmptyString(client_p->name) ? name : "",
				     client_p->name, target_p->name);
		ilog(L_SERVER, "Attempt to re-introduce SID %s from %s%s (already in use by %s)",
				client_p->id,
				EmptyString(client_p->name) ? name : "",
				log_client_name(client_p, SHOW_IP),
				target_p->name);

		sendto_one(client_p, "ERROR :SID already exists.");
		exit_client(client_p, client_p, client_p, "SID Exists");
		return;
	}

	/*
	 * if we are connecting (Handshake), we already have the name from the
	 * C:line in client_p->name
	 */

	rb_strlcpy(client_p->name, name, sizeof(client_p->name));
	set_server_gecos(client_p, info);
	client_p->hopcount = hop;
	server_estab(client_p);
}

/*
 * ms_server - SERVER message handler
 *      parv[1] = servername
 *      parv[2] = serverinfo/hopcount
 *      parv[3] = serverinfo
 */
static void
ms_server(struct MsgBuf *msgbuf_p, struct Client *client_p, struct Client *source_p, int parc, const char *parv[])
{
	char info[REALLEN + 1];
	/* same size as in s_misc.c */
	const char *name;
	struct Client *target_p;
	struct remote_conf *hub_p;
	hook_data_client hdata;
	int hop;
	int hlined = 0;
	int llined = 0;
	rb_dlink_node *ptr;
	char squitreason[160];

	name = parv[1];
	hop = atoi(parv[2]);
	rb_strlcpy(info, parv[3], sizeof(info));

	if(find_server(NULL, name))
	{
		/*
		 * This link is trying feed me a server that I already have
		 * access through another path -- multiple paths not accepted
		 * currently, kill this link immediately!!
		 *
		 * Rather than KILL the link which introduced it, KILL the
		 * youngest of the two links. -avalon
		 *
		 * I think that we should exit the link itself, not the introducer,
		 * and we should always exit the most recently received(i.e. the
		 * one we are receiving this SERVER for. -A1kmm
		 *
		 * You *cant* do this, if you link somewhere, it bursts you a server
		 * that already exists, then sends you a client burst, you squit the
		 * server, but you keep getting the burst of clients on a server that
		 * doesnt exist, although ircd can handle it, its not a realistic
		 * solution.. --fl_
		 */
		ilog(L_SERVER, "Link %s cancelled, server %s already exists",
			client_p->name, name);

		snprintf(squitreason, sizeof squitreason,
				"Server %s already exists",
				name);
		exit_client(client_p, client_p, &me, squitreason);
		return;
	}

	/*
	 * User nicks never have '.' in them and server names
	 * must always have '.' in them.
	 */
	if(strchr(name, '.') == NULL)
	{
		/*
		 * Server trying to use the same name as a person. Would
		 * cause a fair bit of confusion. Enough to make it hellish
		 * for a while and servers to send stuff to the wrong place.
		 */
		sendto_one(client_p, "ERROR :Nickname %s already exists!", name);
		sendto_realops_snomask(SNO_GENERAL, L_ALL,
				     "Link %s cancelled: Server/nick collision on %s",
				     client_p->name, name);
		ilog(L_SERVER, "Link %s cancelled: Server/nick collision on %s",
			client_p->name, name);

		exit_client(client_p, client_p, client_p, "Nick as Server");
		return;
	}

	/*
	 * Server is informing about a new server behind
	 * this link. Create REMOTE server structure,
	 * add it to list and propagate word to my other
	 * server links...
	 */

	/*
	 * See if the newly found server is behind a guaranteed
	 * leaf. If so, close the link.
	 *
	 */
	RB_DLINK_FOREACH(ptr, hubleaf_conf_list.head)
	{
		hub_p = (remote_conf *)ptr->data;

		if(match(hub_p->server, client_p->name) && match(hub_p->host, name))
		{
			if(hub_p->flags & CONF_HUB)
				hlined++;
			else
				llined++;
		}
	}

	/* Ok, this way this works is
	 *
	 * A server can have a CONF_HUB allowing it to introduce servers
	 * behind it.
	 *
	 * connect {
	 *            name = "irc.bighub.net";
	 *            hub_mask="*";
	 *            ...
	 *
	 * That would allow "irc.bighub.net" to introduce anything it wanted..
	 *
	 * However
	 *
	 * connect {
	 *            name = "irc.somehub.fi";
	 *            hub_mask="*";
	 *            leaf_mask="*.edu";
	 *...
	 * Would allow this server in finland to hub anything but
	 * .edu's
	 */

	/* Ok, check client_p can hub the new server */
	if(!hlined)
	{
		/* OOOPs nope can't HUB */
		sendto_realops_snomask(SNO_GENERAL, L_ALL, "Non-Hub link %s introduced %s.",
				     client_p->name, name);
		ilog(L_SERVER, "Non-Hub link %s introduced %s.",
			client_p->name, name);

		snprintf(squitreason, sizeof squitreason,
				"No matching hub_mask for %s",
				name);
		exit_client(NULL, client_p, &me, squitreason);
		return;
	}

	/* Check for the new server being leafed behind this HUB */
	if(llined)
	{
		/* OOOPs nope can't HUB this leaf */
		sendto_realops_snomask(SNO_GENERAL, L_ALL,
				     "Link %s introduced leafed server %s.",
				     client_p->name, name);
		ilog(L_SERVER, "Link %s introduced leafed server %s.",
			client_p->name, name);

		snprintf(squitreason, sizeof squitreason,
				"Matching leaf_mask for %s",
				name);
		exit_client(NULL, client_p, &me, squitreason);
		return;
	}



	if(strlen(name) > HOSTLEN)
	{
		sendto_realops_snomask(SNO_GENERAL, L_ALL,
				     "Link %s introduced server with invalid servername %s",
				     client_p->name, name);
		ilog(L_SERVER, "Link %s introduced server with invalid servername %s",
			client_p->name, name);

		exit_client(NULL, client_p, &me, "Invalid servername introduced.");
		return;
	}

	target_p = make_client(client_p);
	make_server(target_p);
	target_p->hopcount = hop;

	rb_strlcpy(target_p->name, name, sizeof(target_p->name));

	set_server_gecos(target_p, info);

	target_p->servptr = source_p;

	SetServer(target_p);

	rb_dlinkAddTail(target_p, &target_p->node, &global_client_list);
	rb_dlinkAddTailAlloc(target_p, &global_serv_list);
	add_to_client_hash(target_p->name, target_p);
	rb_dlinkAdd(target_p, &target_p->lnode, &target_p->servptr->serv->servers);

	target_p->serv->nameinfo = scache_connect(target_p->name, target_p->info, IsHidden(target_p));

	sendto_server(client_p, NULL, NOCAPS, NOCAPS,
		      ":%s SERVER %s %d :%s%s",
		      source_p->name, target_p->name, target_p->hopcount + 1,
		      IsHidden(target_p) ? "(H) " : "", target_p->info);

	sendto_realops_snomask(SNO_EXTERNAL, L_ALL,
			     "Server %s being introduced by %s", target_p->name, source_p->name);

	/* quick, dirty EOB.  you know you love it. */
	sendto_one(target_p, ":%s PING %s %s", get_id(&me, target_p), me.name, target_p->name);

	hdata.client = source_p;
	hdata.target = target_p;
	call_hook(h_server_introduced, &hdata);
}

static void
ms_sid(struct MsgBuf *msgbuf_p, struct Client *client_p, struct Client *source_p, int parc, const char *parv[])
{
	struct Client *target_p;
	struct remote_conf *hub_p;
	hook_data_client hdata;
	rb_dlink_node *ptr;
	int hlined = 0;
	int llined = 0;
	char squitreason[160];

	/* collision on the name? */
	if(find_server(NULL, parv[1]) != NULL)
	{
		ilog(L_SERVER, "Link %s cancelled, server %s already exists",
			client_p->name, parv[1]);

		snprintf(squitreason, sizeof squitreason,
				"Server %s already exists",
				parv[1]);
		exit_client(NULL, client_p, &me, squitreason);
		return;
	}

	/* collision on the SID? */
	if((target_p = find_id(parv[3])) != NULL)
	{
		sendto_wallops_flags(UMODE_WALLOP, &me,
				     "Link %s cancelled, SID %s for server %s already in use by %s",
				     client_p->name, parv[3], parv[1], target_p->name);
		sendto_server(NULL, NULL, CAP_TS6, NOCAPS,
				     ":%s WALLOPS :Link %s cancelled, SID %s for server %s already in use by %s",
				     me.id, client_p->name, parv[3], parv[1], target_p->name);
		ilog(L_SERVER, "Link %s cancelled, SID %s for server %s already in use by %s",
			client_p->name, parv[3], parv[1], target_p->name);

		snprintf(squitreason, sizeof squitreason,
				"SID %s for %s already in use by %s",
				parv[3], parv[1], target_p->name);
		exit_client(NULL, client_p, &me, squitreason);
		return;
	}

	if(bogus_host(parv[1]) || strlen(parv[1]) > HOSTLEN)
	{
		sendto_one(client_p, "ERROR :Invalid servername");
		sendto_realops_snomask(SNO_GENERAL, L_ALL,
				     "Link %s cancelled, servername %s invalid",
				     client_p->name, parv[1]);
		ilog(L_SERVER, "Link %s cancelled, servername %s invalid",
			client_p->name, parv[1]);

		exit_client(NULL, client_p, &me, "Bogus server name");
		return;
	}

	if(!IsDigit(parv[3][0]) || !IsIdChar(parv[3][1]) ||
	   !IsIdChar(parv[3][2]) || parv[3][3] != '\0')
	{
		sendto_one(client_p, "ERROR :Invalid SID");
		sendto_realops_snomask(SNO_GENERAL, L_ALL,
				     "Link %s cancelled, SID %s invalid",
				     client_p->name, parv[3]);
		ilog(L_SERVER, "Link %s cancelled, SID %s invalid",
			client_p->name, parv[3]);

		exit_client(NULL, client_p, &me, "Bogus SID");
		return;
	}

	/* for the directly connected server:
	 * H: allows it to introduce a server matching that mask
	 * L: disallows it introducing a server matching that mask
	 */
	RB_DLINK_FOREACH(ptr, hubleaf_conf_list.head)
	{
		hub_p = (remote_conf *)ptr->data;

		if(match(hub_p->server, client_p->name) && match(hub_p->host, parv[1]))
		{
			if(hub_p->flags & CONF_HUB)
				hlined++;
			else
				llined++;
		}
	}

	/* no matching hub_mask */
	if(!hlined)
	{
		sendto_realops_snomask(SNO_GENERAL, L_ALL,
				     "Non-Hub link %s introduced %s.",
				     client_p->name, parv[1]);
		ilog(L_SERVER, "Non-Hub link %s introduced %s.",
			client_p->name, parv[1]);

		snprintf(squitreason, sizeof squitreason,
				"No matching hub_mask for %s",
				parv[1]);
		exit_client(NULL, client_p, &me, squitreason);
		return;
	}

	/* matching leaf_mask */
	if(llined)
	{
		sendto_realops_snomask(SNO_GENERAL, L_ALL,
				     "Link %s introduced leafed server %s.",
				     client_p->name, parv[1]);
		ilog(L_SERVER, "Link %s introduced leafed server %s.",
			client_p->name, parv[1]);

		snprintf(squitreason, sizeof squitreason,
				"Matching leaf_mask for %s",
				parv[1]);
		exit_client(NULL, client_p, &me, squitreason);
		return;
	}

	/* ok, alls good */
	target_p = make_client(client_p);
	make_server(target_p);

	rb_strlcpy(target_p->name, parv[1], sizeof(target_p->name));
	target_p->hopcount = atoi(parv[2]);
	rb_strlcpy(target_p->id, parv[3], sizeof(target_p->id));
	set_server_gecos(target_p, parv[4]);

	target_p->servptr = source_p;
	SetServer(target_p);

	rb_dlinkAddTail(target_p, &target_p->node, &global_client_list);
	rb_dlinkAddTailAlloc(target_p, &global_serv_list);
	add_to_client_hash(target_p->name, target_p);
	add_to_id_hash(target_p->id, target_p);
	rb_dlinkAdd(target_p, &target_p->lnode, &target_p->servptr->serv->servers);

	target_p->serv->nameinfo = scache_connect(target_p->name, target_p->info, IsHidden(target_p));

	sendto_server(client_p, NULL, CAP_TS6, NOCAPS,
		      ":%s SID %s %d %s :%s%s",
		      source_p->id, target_p->name, target_p->hopcount + 1,
		      target_p->id, IsHidden(target_p) ? "(H) " : "", target_p->info);

	sendto_realops_snomask(SNO_EXTERNAL, L_ALL,
			     "Server %s being introduced by %s", target_p->name, source_p->name);

	/* quick, dirty EOB.  you know you love it. */
	sendto_one(target_p, ":%s PING %s %s",
		   get_id(&me, target_p), me.name, get_id(target_p, target_p));

	hdata.client = source_p;
	hdata.target = target_p;
	call_hook(h_server_introduced, &hdata);
}

/* set_server_gecos()
 *
 * input	- pointer to client
 * output	- none
 * side effects - servers gecos field is set
 */
static void
set_server_gecos(struct Client *client_p, const char *info)
{
	/* check the info for [IP] */
	if(info[0])
	{
		char *p;
		char *s;

		s = LOCAL_COPY(info);

		/* we should only check the first word for an ip */
		if((p = strchr(s, ' ')))
			*p = '\0';

		/* check for a ] which would symbolise an [IP] */
		if(strchr(s, ']'))
		{
			/* set s to after the first space */
			if(p)
				s = ++p;
			else
				s = NULL;
		}
		/* no ], put the space back */
		else if(p)
			*p = ' ';

		/* p may have been set to a trailing space, so check s exists and that
		 * it isnt \0 */
		if(s && (*s != '\0'))
		{
			/* a space? if not (H) could be the last part of info.. */
			if((p = strchr(s, ' ')))
				*p = '\0';

			/* check for (H) which is a hidden server */
			if(!strcmp(s, "(H)"))
			{
				SetHidden(client_p);

				/* if there was no space.. theres nothing to set info to */
				if(p)
					s = ++p;
				else
					s = NULL;
			}
			else if(p)
				*p = ' ';

			/* if there was a trailing space, s could point to \0, so check */
			if(s && (*s != '\0'))
			{
				rb_strlcpy(client_p->info, s, sizeof(client_p->info));
				return;
			}
		}
	}

	rb_strlcpy(client_p->info, "(Unknown Location)", sizeof(client_p->info));
}

/*
 * bogus_host
 *
 * inputs	- hostname
 * output	- true if a bogus hostname input, false if its valid
 * side effects	- none
 */
static bool
bogus_host(const char *host)
{
	bool bogus_server = false;
	const char *s;
	int dots = 0;

	for(s = host; *s; s++)
	{
		if(!IsServChar(*s))
		{
			bogus_server = true;
			break;
		}
		if('.' == *s)
			++dots;
	}

	if(!dots || bogus_server)
		return true;

	return false;
}
