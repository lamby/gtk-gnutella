/*
 * $Id$
 *
 * Copyright (c) 2001-2003, Raphael Manfredi
 * Copyright (c) 2000 Daniel Walker (dwalker@cats.ucsc.edu)
 *
 *----------------------------------------------------------------------
 * This file is part of gtk-gnutella.
 *
 *  gtk-gnutella is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  gtk-gnutella is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with gtk-gnutella; if not, write to the Free Software
 *  Foundation, Inc.:
 *      59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *----------------------------------------------------------------------
 */

/**
 * @ingroup core
 * @file
 *
 * Handles upload of our files to others users.
 *
 * @author Raphael Manfredi
 * @date 2001-2003
 * @author Daniel Walker (dwalker@cats.ucsc.edu)
 * @date 2000
 */

#include "common.h"

RCSID("$Id$");

#include "uploads.h"
#include "sockets.h"
#include "share.h"
#include "hosts.h"		/* for check_valid_host() */
#include "bsched.h"
#include "upload_stats.h"
#include "dmesh.h"
#include "http.h"
#include "version.h"
#include "nodes.h"
#include "ioheader.h"
#include "ban.h"
#include "parq.h"
#include "huge.h"
#include "settings.h"
#include "features.h"
#include "geo_ip.h"
#include "ignore.h"
#include "tx_link.h"		/* for callback structures */
#include "tx_deflate.h"

#include "if/gnet_property.h"
#include "if/gnet_property_priv.h"

#include "lib/aging.h"
#include "lib/atoms.h"
#include "lib/cq.h"
#include "lib/endian.h"
#include "lib/idtable.h"
#include "lib/getline.h"
#include "lib/glib-missing.h"
#include "lib/file.h"
#include "lib/header.h"
#include "lib/listener.h"
#include "lib/url.h"
#include "lib/urn.h"
#include "lib/walloc.h"
#include "lib/override.h"	/* Must be the last header included */

#define READ_BUF_SIZE	4096		/**< Read buffer size, if no sendfile(2) */
#define BW_OUT_MIN		256			/**< Minimum bandwidth to enable uploads */
#define IO_PRE_STALL	30			/**< Pre-stalling warning */
#define IO_STALLED		60			/**< Stalling condition */
#define IO_LONG_TIMEOUT	160			/**< Longer timeouting condition */
#define UP_SEND_BUFSIZE	8192		/**< Socket write buffer, when stalling */
#define STALL_CLEAR		600			/**< Decrease stall counter every 10 min */
#define STALL_THRESH	3			/**< If more stalls than that, workaround */

#define RQST_LINE_LENGTH	256		/**< Reasonable estimate for request line */

static GSList *list_uploads = NULL;
static gint stalled = 0;			/**< Counts stalled connections */
static time_t last_stalled;			/**< Time at which last stall occurred */

/** Used to fall back to write() if sendfile() failed */
static gboolean sendfile_failed = FALSE;

static idtable_t *upload_handle_map = NULL;


#define upload_find_by_handle(n) \
    (gnutella_upload_t *) idtable_get_value(upload_handle_map, n)

#define upload_new_handle(n) \
    idtable_new_id(upload_handle_map, n)

#define upload_free_handle(n) \
    idtable_free_id(upload_handle_map, n);

static gint running_uploads = 0;
static gint registered_uploads = 0;

/**
 * This structure is the key used in the mesh_info hash table to record
 * when we last sent mesh information to some IP about a given file
 * (identified by its SHA1).
 */
struct mesh_info_key {
	guint32 ip;						/**< Remote host IP address */
	const gchar *sha1;				/**< SHA1 atom */
};

struct mesh_info_val {
	guint32 stamp;					/**< When we last sent the mesh */
	gpointer cq_ev;					/**< Scheduled cleanup callout event */
};

/* Keep mesh info about uploaders for that long (unit: ms) */
#define MESH_INFO_TIMEOUT	((PARQ_MAX_UL_RETRY_DELAY + PARQ_GRACE_TIME)*1000)

static GHashTable *mesh_info = NULL;

/* Remember IP address of stalling uploads for a while */
static gpointer stalling_uploads = NULL;

#define STALL_FIRST		GUINT_TO_POINTER(0x1)
#define STALL_AGAIN		GUINT_TO_POINTER(0x2)

static void upload_request(gnutella_upload_t *u, header_t *header);
static void upload_error_remove(gnutella_upload_t *u, struct shared_file *sf,
	int code, const gchar *msg, ...) G_GNUC_PRINTF(4, 5);
static void upload_error_remove_ext(gnutella_upload_t *u,
	struct shared_file *sf, const gchar *extended, int code,
	const gchar *msg, ...) G_GNUC_PRINTF(5, 6);
static void upload_http_sha1_add(
	gchar *buf, gint *retval, gpointer arg, guint32 flags);
static void upload_http_xhost_add(
	gchar *buf, gint *retval, gpointer arg, guint32 flags);
static void upload_xfeatures_add(
	gchar *buf, gint *retval, gpointer arg, guint32 flags);
static void upload_writable(gpointer up, gint source, inputevt_cond_t cond);
static void upload_special_writable(gpointer up);
static void send_upload_error(gnutella_upload_t *u, struct shared_file *sf,
	int code, const gchar *msg, ...) G_GNUC_PRINTF(4, 5);

/***
 *** Callbacks
 ***/

static listeners_t upload_added_listeners   = NULL;
static listeners_t upload_removed_listeners = NULL;
static listeners_t upload_info_changed_listeners = NULL;

void
upload_add_upload_added_listener(upload_added_listener_t l)
{
    LISTENER_ADD(upload_added, l);
}

void
upload_remove_upload_added_listener(upload_added_listener_t l)
{
    LISTENER_REMOVE(upload_added, l);
}

void
upload_add_upload_removed_listener(upload_removed_listener_t l)
{
    LISTENER_ADD(upload_removed, l);
}

void
upload_remove_upload_removed_listener(upload_removed_listener_t l)
{
    LISTENER_REMOVE(upload_removed, l);
}

void
upload_add_upload_info_changed_listener(upload_info_changed_listener_t l)
{
    LISTENER_ADD(upload_info_changed, l);
}

void
upload_remove_upload_info_changed_listener(upload_info_changed_listener_t l)
{
    LISTENER_REMOVE(upload_info_changed, l);
}

static void
upload_fire_upload_added(gnutella_upload_t *n)
{
    LISTENER_EMIT(upload_added,
		(n->upload_handle, running_uploads, registered_uploads));
	gnet_prop_set_guint32_val(PROP_UL_RUNNING, running_uploads);
	gnet_prop_set_guint32_val(PROP_UL_REGISTERED, registered_uploads);
}

static void
upload_fire_upload_removed(gnutella_upload_t *n, const gchar *reason)
{
    LISTENER_EMIT(upload_removed,
		(n->upload_handle, reason, running_uploads, registered_uploads));
	gnet_prop_set_guint32_val(PROP_UL_RUNNING, running_uploads);
	gnet_prop_set_guint32_val(PROP_UL_REGISTERED, registered_uploads);
}

void
upload_fire_upload_info_changed(gnutella_upload_t *n)
{
    LISTENER_EMIT(upload_info_changed,
		(n->upload_handle, running_uploads, registered_uploads));
}

/***
 *** Private functions
 ***/

/**
 * Upload heartbeat timer.
 */
void
upload_timer(time_t now)
{
	GSList *sl, *to_remove = NULL;
	gint t;

	for (sl = list_uploads; sl; sl = g_slist_next(sl)) {
		gnutella_upload_t *u = (gnutella_upload_t *) sl->data;
		gboolean is_connecting;

		g_assert(u != NULL);

		if (UPLOAD_IS_COMPLETE(u))
			continue;					/* Complete, no timeout possible */

		/*
		 * Check for timeouts.
		 */

		is_connecting = UPLOAD_IS_CONNECTING(u);
		t = is_connecting ?
            upload_connecting_timeout :
			MAX(upload_connected_timeout, IO_STALLED);

		/*
		 * Detect frequent stalling conditions on sending.
		 */

		if (!UPLOAD_IS_SENDING(u))
			goto not_sending;		/* Avoid deep nesting level */

		if (delta_time(now, u->last_update) > IO_STALLED) {
			gboolean skip = FALSE;

			/*
			 * Check whether we know about this IP.  If we do, then it
			 * has been stalling recently, and it might be a problem on
			 * their end rather than ours, so don't increase the stalling
			 * counter.
			 */

			if (aging_lookup(stalling_uploads, GUINT_TO_POINTER(u->ip)))
				skip = TRUE;

			if (!(u->flags & UPLOAD_F_STALLED)) {
				if (!skip && stalled++ == STALL_THRESH) {
					g_warning("frequent stalling detected, using workarounds");
					gnet_prop_set_boolean_val(PROP_UPLOADS_STALLING, TRUE);
				}
				if (!skip) last_stalled = now;
				u->flags |= UPLOAD_F_STALLED;
				g_warning("connection to %s (%s) "
					"stalled after %s bytes sent,"
					" stall counter at %d%s",
					ip_to_gchar(u->ip), upload_vendor_str(u),
					uint64_to_string(u->sent), stalled,
					skip ? " (IGNORED)" : "");

				/*
				 * Record that this IP is stalling, but also record the fact
				 * that it's not the first time we're seeing it, if necessary.
				 */

				aging_insert(stalling_uploads, GUINT_TO_POINTER(u->ip),
					skip ? STALL_AGAIN : STALL_FIRST);
			}
		} else {
			gboolean skip = FALSE;
			gpointer stall;

			stall = aging_lookup(stalling_uploads, GUINT_TO_POINTER(u->ip));
			if (stall == STALL_AGAIN)
				skip = TRUE;

			if (u->flags & UPLOAD_F_STALLED) {
				g_warning("connection to %s (%s) un-stalled, %s bytes sent%s",
					ip_to_gchar(u->ip), upload_vendor_str(u),
					uint64_to_string(u->sent),
					skip ? " (IGNORED)" : "");

				if (
					!skip && stalled <= STALL_THRESH &&
					!sock_is_corked(u->socket)
				) {
					g_warning(
						"re-enabling TCP_CORK on connection to %s (%s)",
						ip_to_gchar(u->ip), upload_vendor_str(u));
					sock_cork(u->socket, TRUE);
					socket_tos_throughput(u->socket);
				}

				if (!skip && stalled > 0) /* It un-stalled, it's not too bad */
					stalled--;
			}
			u->flags &= ~UPLOAD_F_STALLED;
		}

		/* FALL THROUGH */

	not_sending:

		/*
		 * If they have experienced significant stalling conditions recently,
		 * be much more lenient about connection timeouts.
		 */

		if (!is_connecting && stalled > STALL_THRESH)
			t = MAX(t, IO_LONG_TIMEOUT);

		/*
		 * We can't call upload_remove() since it will remove the upload
		 * from the list we are traversing.
		 *
		 * Check pre-stalling condition and remove the CORK option
		 * if we are no longer transmitting.
		 */

		if (delta_time(now, u->last_update) > t)
			to_remove = g_slist_prepend(to_remove, u);
		else if (UPLOAD_IS_SENDING(u)) {
			if (delta_time(now, u->last_update) > IO_PRE_STALL) {
				if (sock_is_corked(u->socket)) {
					g_warning("connection to %s (%s) may be stalled,"
						" disabling TCP_CORK",
						ip_to_gchar(u->ip), upload_vendor_str(u));
					sock_cork(u->socket, FALSE);
					socket_tos_normal(u->socket); /* Have ACKs come faster */
				}
				u->flags |= UPLOAD_F_EARLY_STALL;
			} else
				u->flags &= ~UPLOAD_F_EARLY_STALL;
		}
	}

	if (delta_time(now, last_stalled) > STALL_CLEAR) {
		if (stalled > 0) {
			stalled /= 2;			/* Exponential decrease */
			last_stalled = now;
			if (upload_debug)
				g_warning("stall counter downgraded to %d", stalled);
			if (stalled == 0) {
				g_warning("frequent stalling condition cleared");
				gnet_prop_set_boolean_val(PROP_UPLOADS_STALLING, FALSE);
			}
		}
	}

	for (sl = to_remove; sl; sl = g_slist_next(sl)) {
		gnutella_upload_t *u = (gnutella_upload_t *) sl->data;
		if (UPLOAD_IS_CONNECTING(u)) {
			if (u->status == GTA_UL_PUSH_RECEIVED || u->status == GTA_UL_QUEUE)
				upload_remove(u, _("Connect back timeout"));
			else
				upload_error_remove(u, NULL, 408, "Request timeout");
		} else if (UPLOAD_IS_SENDING(u))
			upload_remove(u, "Data timeout after %s byte%s",
				uint64_to_string(u->sent), u->sent == 1 ? "" : "s");
		else
			upload_remove(u, _("Lifetime expired"));
	}
	g_slist_free(to_remove);
}

/**
 * Create a new upload structure, linked to a socket.
 */
gnutella_upload_t *
upload_create(struct gnutella_socket *s, gboolean push)
{
	static const gnutella_upload_t zero_upload;
	gnutella_upload_t *u;

	u = walloc(sizeof *u);
	*u = zero_upload;
    u->upload_handle = upload_new_handle(u);

	u->socket = s;
    u->ip = s->ip;
	u->country = gip_country(u->ip);
	s->resource.upload = u;

	u->push = push;
	u->status = push ? GTA_UL_PUSH_RECEIVED : GTA_UL_HEADERS;
	u->last_update = time(NULL);
	u->file_desc = -1;
	u->sendfile_ctx.map = NULL;

	/*
	 * Record pending upload in the GUI.
	 */

	registered_uploads++;

	/*
	 * Add the upload structure to the upload slist, so it's monitored
	 * from now on within the main loop for timeouts.
	 */

	list_uploads = g_slist_prepend(list_uploads, u);

	/*
	 * Add upload to the GUI
	 */
    upload_fire_upload_added(u);

	u->parq_status = 0;

	return u;
}

/**
 * Send a GIV string to the specified IP:port.
 *
 * `ip' and `port' is where we need to connect.
 * `hops' and `ttl' are the values from the PUSH message we received, just
 * for logging in case we cannot connect.
 * `file_index' and `file_name' are the values we determined from PUSH.
 * `banning' must be TRUE when we determined connections to the IP were
 * currently prohibited.
 */
void
upload_send_giv(guint32 ip, guint16 port, guint8 hops, guint8 ttl,
	guint32 file_index, const gchar *file_name, gboolean banning)
{
	gnutella_upload_t *u;
	struct gnutella_socket *s;

	s = socket_connect(ip, port, SOCK_TYPE_UPLOAD);
	if (!s) {
		g_warning("PUSH request (hops=%d, ttl=%d) dropped: can't connect to %s",
			hops, ttl, ip_port_to_gchar(ip, port));
		return;
	}

	u = upload_create(s, TRUE);
	u->index = file_index;
	u->name = atom_str_get(file_name);

	if (banning) {
		gchar *msg = ban_message(ip);
		if (msg != NULL)
			upload_remove(u, _("Banned: %s"), msg);
		else
			upload_remove(u, _("Banned for %s"), short_time(ban_delay(ip)));
		return;
	}

	upload_fire_upload_info_changed(u);

	/* Now waiting for the connection CONF -- will call upload_connect_conf() */
}

/**
 * Called when we receive a Push request on Gnet.
 *
 * If it is not for us, discard it.
 * If we are the target, then connect back to the remote servent.
 */
void
handle_push_request(struct gnutella_node *n)
{
	struct shared_file *req_file;
	guint32 file_index;
	guint32 ip;
	guint16 port;
	gchar *info;
	gboolean show_banning = FALSE;
	const gchar *file_name = "<invalid file index>";

	/* Servent ID matches our GUID? */
	if (0 != memcmp(n->data, servent_guid, 16))
		return;								/* No: not for us */

	/*
	 * We are the target of the push.
	 */

	info = n->data + 16;					/* Start of file information */

	READ_GUINT32_LE(info, file_index);
	READ_GUINT32_BE(info + 4, ip);
	READ_GUINT16_LE(info + 8, port);

	/*
	 * Quick sanity check on file index.
	 *
	 * Note that even if the file index is wrong, we still open the
	 * connection.  After all, the PUSH message was bearing our GUID.
	 * We'll let the remote end figure out what to do.
	 *		--RAM. 18/07/2003
	 */

	req_file = shared_file(file_index);

	if (req_file == SHARE_REBUILDING) {
		g_warning("PUSH request (hops=%d, ttl=%d) whilst rebuilding library",
			n->header.hops, n->header.ttl);
	} else if (req_file == NULL) {
		g_warning("PUSH request (hops=%d, ttl=%d) for invalid file index %u",
			n->header.hops, n->header.ttl, file_index);
	} else
		file_name = req_file->name_nfc;

	/*
	 * XXX might be run inside corporations (private IPs), must be smarter.
	 * XXX maybe a configuration variable? --RAM, 31/12/2001
	 *
	 * Don't waste time and resources connecting to something that will fail.
	 *
	 * NB: we allow the PUSH if we're already connected to that node.  This
	 * allows easy local testing. -- RAM, 11/11/2002
	 */

	if (!host_is_valid(ip, port) && !node_is_connected(ip, port, TRUE)) {
		g_warning("PUSH request (hops=%d, ttl=%d) from invalid address %s",
			n->header.hops, n->header.ttl, ip_port_to_gchar(ip, port));
		return;
	}

	/*
	 * Protect from PUSH flood: since each push requires us to connect
	 * back, it uses resources and could be used to conduct a subtle denial
	 * of service attack.	-- RAM, 03/11/2002
	 */

	switch (ban_allow(ip)) {
	case BAN_OK:				/* Connection authorized */
		break;
	case BAN_MSG:				/* Refused: host forcefully banned */
	case BAN_FIRST:				/* Refused, negative ack (can't do for PUSH) */
		show_banning = TRUE;
		/* FALL THROUGH */
	case BAN_FORCE:				/* Refused, no ack */
		if (upload_debug)
			g_warning("PUSH flood (hops=%d, ttl=%d) to %s [ban %s]: %s\n",
				n->header.hops, n->header.ttl, ip_port_to_gchar(ip, port),
				short_time(ban_delay(ip)), file_name);
		if (!show_banning)
			return;
		break;
	default:
		g_assert(0);
	}

	/*
	 * OK, start the upload by opening a connection to the remote host.
	 */

	if (upload_debug > 4)
		g_message("PUSH (hops=%d, ttl=%d) to %s: %s\n",
			n->header.hops, n->header.ttl, ip_port_to_gchar(ip, port),
			file_name);

	upload_send_giv(ip, port, n->header.hops, n->header.ttl,
		file_index, file_name, show_banning);
}

void
upload_real_remove(void)
{
	/* XXX UNUSED
	 * XXX Currently, we remove failed uploads from the list, but we should
	 * XXX do as we do for downloads, and have an extra option to remove
	 * XXX failed uploads immediately.	--RAM, 24/12/2001
	 */
}

static void
upload_free_resources(gnutella_upload_t *u)
{
	parq_upload_upload_got_freed(u);

	if (u->name != NULL) {
		atom_str_free(u->name);
		u->name = NULL;
	}
	if (u->file_desc != -1) {
		close(u->file_desc);
		u->file_desc = -1;
	}
#ifdef USE_MMAP
	if (u->sendfile_ctx.map != NULL) {
		size_t len = u->sendfile_ctx.map_end - u->sendfile_ctx.map_start;

		g_assert(len > 0 && len <= INT_MAX);
		munmap(u->sendfile_ctx.map, len);
		u->sendfile_ctx.map = NULL;
	}
#endif /* USE_MMAP */
	if (u->socket != NULL) {
		g_assert(u->socket->resource.upload == u);
		socket_free(u->socket);
		u->socket = NULL;
	}
	if (u->buffer != NULL) {
		g_free(u->buffer);
		u->buffer = NULL;
	}
	if (u->io_opaque) {				/* I/O data */
		io_free(u->io_opaque);
		g_assert(u->io_opaque == NULL);
	}
	if (u->bio != NULL) {
		bsched_source_remove(u->bio);
		u->bio = NULL;
	}
	if (u->user_agent) {
		atom_str_free(u->user_agent);
		u->user_agent = NULL;
	}
	if (u->sha1) {
		atom_sha1_free(u->sha1);
		u->sha1 = NULL;
	}
	if (u->special) {
		u->special->close(u->special);
		u->special = NULL;
	}

    upload_free_handle(u->upload_handle);
}

/**
 * Clone upload, resetting all dynamically allocated structures in the
 * original, since they are shallow-copied to the new upload.
 *
 * (This routine is used because each different upload from the same host
 * will become a line in the GUI, and the GUI stores upload structures in
 * its row data, and will call upload_remove() to clear them.)
 */
static gnutella_upload_t *
upload_clone(gnutella_upload_t *u)
{
	gnutella_upload_t *cu = walloc(sizeof(gnutella_upload_t));

	*cu = *u;		/* Struct copy */

	g_assert(u->io_opaque == NULL);		/* If cloned, we were transferrring! */

	parq_upload_upload_got_cloned(u, cu);

    cu->upload_handle = upload_new_handle(cu); /* fetch new handle */
	cu->bio = NULL;						/* Recreated on each transfer */
	cu->file_desc = -1;					/* File re-opened each time */
	cu->sendfile_ctx.map = NULL;		/* File re-opened each time */
	cu->socket->resource.upload = cu;	/* Takes ownership of socket */
	cu->accounted = FALSE;
    cu->skip = 0;
    cu->end = 0;
	cu->sent = 0;

	/*
	 * The following have been copied and appropriated by the cloned upload.
	 * They are reset so that an upload_free_resource() on the original will
	 * not free them.
	 */

	u->name = NULL;
	u->socket = NULL;
	u->buffer = NULL;
	u->user_agent = NULL;
	u->country = -1;
	u->sha1 = NULL;
	u->special = NULL;

	/*
	 * Add the upload structure to the upload slist, so it's monitored
	 * from now on within the main loop for timeouts.
	 */

	list_uploads = g_slist_prepend(list_uploads, cu);

	/*
	 * Add upload to the GUI
	 */
    upload_fire_upload_added(cu);

	return cu;
}

/**
 * The vectorized (message-wise) version of send_upload_error().
 */
static void
send_upload_error_v(
	gnutella_upload_t *u,
	struct shared_file *sf,
	const gchar *ext,
	int code,
	const gchar *msg, va_list ap)
{
	gchar reason[1024];
	gchar extra[1024];
	size_t slen = 0;
	http_extra_desc_t hev[6];
	guint hevcnt = 0;
	struct upload_http_cb cb_parq_arg, cb_sha1_arg;

	if (msg) {
		gm_vsnprintf(reason, sizeof(reason), msg, ap);
	} else
		reason[0] = '\0';

	if (u->error_sent) {
		g_warning("Already sent an error %d to %s, not sending %d (%s)",
			u->error_sent, ip_to_gchar(u->socket->ip), code, reason);
		return;
	}

	extra[0] = '\0';

	/*
	 * If `ext' is not null, we have extra header information to propagate.
	 */

	if (ext) {
		slen = g_strlcpy(extra, ext, sizeof(extra));

		if (slen < sizeof(extra)) {
			hev[hevcnt].he_type = HTTP_EXTRA_LINE;
			hev[hevcnt++].he_msg = extra;
		} else
			g_warning("send_upload_error_v: "
				"ignoring too large extra header (%d bytes)", (int) slen);
	}

	/*
	 * Send X-Features on error too.
	 *		-- JA, 03/11/2003
	 */
	hev[hevcnt].he_type = HTTP_EXTRA_CALLBACK;
	hev[hevcnt].he_cb = upload_xfeatures_add;
	hev[hevcnt++].he_arg = NULL;

	/*
	 * If the download got queued, also add the queueing information
	 *		--JA, 07/02/2003
	 */
	if (parq_upload_queued(u)) {
		cb_parq_arg.u = u;
		cb_parq_arg.sf = sf;

		hev[hevcnt].he_type = HTTP_EXTRA_CALLBACK;
		hev[hevcnt].he_cb = parq_upload_add_header;
		hev[hevcnt++].he_arg = &cb_parq_arg;
	}

	/*
	 * If this is a pushed upload, and we are not firewalled, then tell
	 * them they can reach us directly by outputting an X-Host line.
	 *
	 * If we are firewalled, let them know about our push proxies, if we
	 * have ones.
	 */

	if (u->push && !is_firewalled) {
		hev[hevcnt].he_type = HTTP_EXTRA_CALLBACK;
		hev[hevcnt].he_cb = upload_http_xhost_add;
		hev[hevcnt++].he_arg = NULL;
	} else if (is_firewalled) {
		hev[hevcnt].he_type = HTTP_EXTRA_CALLBACK;
		hev[hevcnt].he_cb = node_http_proxies_add;
		hev[hevcnt++].he_arg = NULL;
	}

	/*
	 * If they chose to advertise a hostname, include it in our reply.
	 */

	if (!is_firewalled && give_server_hostname && 0 != *server_hostname) {
		hev[hevcnt].he_type = HTTP_EXTRA_CALLBACK;
		hev[hevcnt].he_cb = http_hostname_add;
		hev[hevcnt++].he_arg = NULL;
	}

	/*
	 * If `sf' is not null, propagate the SHA1 for the file if we have it,
	 * as well as the download mesh.
	 */
	if (sf && sha1_hash_available(sf)) {
		cb_sha1_arg.u = u;
		cb_sha1_arg.sf = sf;

		hev[hevcnt].he_type = HTTP_EXTRA_CALLBACK;
		hev[hevcnt].he_cb = upload_http_sha1_add;
		hev[hevcnt++].he_arg = &cb_sha1_arg;
	}

	g_assert(hevcnt <= G_N_ELEMENTS(hev));

	/*
	 * Keep connection alive when activly queued
	 * 		-- JA, 22/4/2003
	 */
	if (u->status == GTA_UL_QUEUED)
		http_send_status(u->socket, code, TRUE,
			hevcnt ? hev : NULL, hevcnt, "%s", reason);
	else
		http_send_status(u->socket, code, FALSE,
			hevcnt ? hev : NULL, hevcnt, "%s", reason);

	u->error_sent = code;
}

/**
 * Send error message to requestor.
 *
 * This can only be done once per connection.
 */
static void
send_upload_error(
	gnutella_upload_t *u,
	struct shared_file *sf,
	int code,
	const gchar *msg, ...)
{
	va_list args;

	va_start(args, msg);
	send_upload_error_v(u, sf, NULL, code, msg, args);
	va_end(args);
}

/**
 * The vectorized (message-wise) version of upload_remove().
 */
static void
upload_remove_v(gnutella_upload_t *u, const gchar *reason, va_list ap)
{
	const gchar *logreason;
	gchar errbuf[1024];

	g_assert(u != NULL);

	if (reason) {
		gm_vsnprintf(errbuf, sizeof(errbuf), reason, ap);
		logreason = errbuf;
	} else {
		if (u->error_sent) {
			gm_snprintf(errbuf, sizeof(errbuf), "HTTP %d", u->error_sent);
			logreason = errbuf;
		} else {
			errbuf[0] = '\0';
			logreason = "No reason given";
		}
	}

	if (!UPLOAD_IS_COMPLETE(u) && upload_debug > 1) {
		if (u->name) {
			g_message("Cancelling upload for \"%s\" from %s (%s): %s\n",
				u->name,
				u->socket ? ip_to_gchar(u->socket->ip) : "<no socket>",
				upload_vendor_str(u),
				logreason);
		} else {
			g_message("Cancelling upload from %s (%s): %s\n",
				u->socket ? ip_to_gchar(u->socket->ip) : "<no socket>",
				upload_vendor_str(u),
				logreason);
		}
	}

	/*
	 * If the upload is still connecting, we have not started sending
	 * any data yet, so we send an HTTP error code before closing the
	 * connection.
	 *		--RAM, 24/12/2001
	 *
	 * Push requests still connecting don't have anything to send, hence
	 * we check explicitely for GTA_UL_PUSH_RECEIVED.
	 *		--RAM, 31/12/2001
	 * 	Same goes for a parq QUEUE 'push' send.
	 *		-- JA, 12/04/2003
	 */

	if (
		UPLOAD_IS_CONNECTING(u) &&
		!u->error_sent &&
		u->status != GTA_UL_PUSH_RECEIVED && u->status != GTA_UL_QUEUE
	) {
		if (reason == NULL)
			logreason = "Bad Request";
		send_upload_error(u, NULL, 400, "%s", logreason);
	}

	/*
	 * If COMPLETE, we've already decremented `running_uploads' and
	 * `registered_uploads'.
	 * Moreover, if it's still connecting, then we've not even
	 * incremented the `running_uploads' counter yet.
	 * For keep-alive uploads still in the GTA_UL_WAITING state, the upload
	 * slot is reserved so it must be decremented as well (we know it's a
	 * follow-up request since u->keep_alive is set).
	 */

	if (!UPLOAD_IS_COMPLETE(u))
		registered_uploads--;

	switch (u->status) {
	case GTA_UL_QUEUED:
	case GTA_UL_PFSP_WAITING:
		/* running_uploads was already decremented */
		break;
	default:
		if (!UPLOAD_IS_COMPLETE(u) && !UPLOAD_IS_CONNECTING(u)) {
			running_uploads--;
		} else if (u->keep_alive && UPLOAD_IS_CONNECTING(u)) {
			running_uploads--;
		}
		break;
	}

	/*
	 * If we were sending data, and we have not accounted the download yet,
	 * then update the stats, not marking the upload as completed.
	 */

	if (UPLOAD_IS_SENDING(u) && !u->browse_host && !u->accounted)
		upload_stats_file_aborted(u);

    if (!UPLOAD_IS_COMPLETE(u)) {
        if (u->status == GTA_UL_WAITING)
            u->status = GTA_UL_CLOSED;
        else
            u->status = GTA_UL_ABORTED;
        upload_fire_upload_info_changed(u);
    }

	parq_upload_remove(u);
    upload_fire_upload_removed(u, reason ? errbuf : NULL);

	upload_free_resources(u);
	wfree(u, sizeof *u);
	list_uploads = g_slist_remove(list_uploads, u);
}

/**
 * Remove upload entry, log reason.
 *
 * If no status has been sent back on the HTTP stream yet, give them
 * a 400 error with the reason.
 */
void
upload_remove(gnutella_upload_t *u, const gchar *reason, ...)
{
	va_list args;

	g_assert(u != NULL);

	va_start(args, reason);
	upload_remove_v(u, reason, args);
	va_end(args);
}

/**
 * Utility routine.  Cancel the upload, sending back the HTTP error message.
 *
 * @note The parameter "msg" is passed to gettext(). Do not pass already
 *       translated strings because it's send as HTTP response message.
 */
static void
upload_error_remove(
	gnutella_upload_t *u,
	struct shared_file *sf,
	int code,
	const gchar *msg, ...)
{
	va_list args, errargs;

	g_assert(u != NULL);

	va_start(args, msg);

	VA_COPY(errargs, args);
	send_upload_error_v(u, sf, NULL, code, msg, errargs);
	va_end(errargs);

	upload_remove_v(u, msg ? _(msg) : NULL, args);
	va_end(args);
}

/**
 * Utility routine.  Cancel the upload, sending back the HTTP error message.
 * `ext' contains additionnal header information to propagate back.
 */
static void
upload_error_remove_ext(
	gnutella_upload_t *u,
	struct shared_file *sf,
	const gchar *ext,
	int code,
	const gchar *msg, ...)
{
	va_list args, errargs;

	g_assert(u != NULL);

	va_start(args, msg);

	VA_COPY(errargs, args);
	send_upload_error_v(u, sf, ext, code, msg, errargs);
	va_end(errargs);

	upload_remove_v(u, msg, args);

	va_end(args);
}

/**
 * Stop all uploads dealing with partial file `fi'.
 */
void upload_stop_all(struct dl_file_info *fi, const gchar *reason)
{
	GSList *sl, *to_stop = NULL;
	gint count = 0;

	for (sl = list_uploads; sl; sl = g_slist_next(sl)) {
		gnutella_upload_t *up = (gnutella_upload_t *) (sl->data);
		g_assert(up);
		if (up->file_info == fi) {
			to_stop = g_slist_prepend(to_stop, up);
			count++;
		}
	}

	if (to_stop == NULL)
		return;

	if (upload_debug)
		g_warning("stopping %d uploads for \"%s\": %s",
			count, fi->file_name, reason);

	for (sl = to_stop; sl; sl = g_slist_next(sl)) {
		gnutella_upload_t *up = sl->data;
		upload_remove(up, "%s", reason);
	}

	g_slist_free(to_stop);
}

/***
 *** I/O header parsing callbacks.
 ***/

static inline gnutella_upload_t *
cast_to_upload(gpointer p)
{
	return p;
}
#define UPLOAD(x)	cast_to_upload(x)

static void
err_line_too_long(gpointer obj)
{
	upload_error_remove(UPLOAD(obj), NULL, 413, "Header too large");
}

static void
err_header_error_tell(gpointer obj, gint error)
{
	send_upload_error(UPLOAD(obj), NULL, 413, "%s", header_strerror(error));
}

static void
err_header_error(gpointer obj, gint error)
{
	upload_remove(UPLOAD(obj), _("Failed (%s)"), header_strerror(error));
}

static void
err_input_exception(gpointer obj)
{
	upload_remove(UPLOAD(obj), _("Failed (Input Exception)"));
}

static void
err_input_buffer_full(gpointer obj)
{
	upload_error_remove(UPLOAD(obj), NULL, 500, "Input buffer full");
}

static void
err_header_read_error(gpointer obj, gint error)
{
	upload_remove(UPLOAD(obj), _("Failed (Input error: %s)"), g_strerror(error));
}

static void
err_header_read_eof(gpointer obj)
{
	gnutella_upload_t * u = UPLOAD(obj);
	u->error_sent = 999;		/* No need to send anything on EOF condition */
	upload_remove(u, _("Failed (EOF)"));
}

static void
err_header_extra_data(gpointer obj)
{
	upload_error_remove(UPLOAD(obj), NULL, 400, "Extra data after HTTP header");
}

static const struct io_error upload_io_error = {
	err_line_too_long,
	err_header_error_tell,
	err_header_error,
	err_input_exception,
	err_input_buffer_full,
	err_header_read_error,
	err_header_read_eof,
	err_header_extra_data,
};

static void
call_upload_request(gpointer obj, header_t *header)
{
	upload_request(UPLOAD(obj), header);
}

#undef UPLOAD

/***
 *** Upload mesh info tracking.
 ***/

static struct mesh_info_key *
mi_key_make(guint32 ip, const gchar *sha1)
{
	struct mesh_info_key *mik;

	mik = walloc(sizeof(*mik));
	mik->ip = ip;
	mik->sha1 = atom_sha1_get(sha1);

	return mik;
}

static void
mi_key_free(struct mesh_info_key *mik)
{
	g_assert(mik);

	atom_sha1_free(mik->sha1);
	wfree(mik, sizeof(*mik));
}

static guint
mi_key_hash(gconstpointer key)
{
	const struct mesh_info_key *mik = (const struct mesh_info_key *) key;

	return sha1_hash((gconstpointer) mik->sha1) ^ mik->ip;
}

static gint
mi_key_eq(gconstpointer a, gconstpointer b)
{
	const struct mesh_info_key *mika = (const struct mesh_info_key *) a;
	const struct mesh_info_key *mikb = (const struct mesh_info_key *) b;

	return mika->ip == mikb->ip &&
		sha1_eq((gconstpointer) mika->sha1, (gconstpointer) mikb->sha1);
}

static struct mesh_info_val *
mi_val_make(guint32 stamp)
{
	struct mesh_info_val *miv;

	miv = walloc(sizeof(*miv));
	miv->stamp = stamp;
	miv->cq_ev = NULL;

	return miv;
}

static void
mi_val_free(struct mesh_info_val *miv)
{
	g_assert(miv);

	if (miv->cq_ev)
		cq_cancel(callout_queue, miv->cq_ev);

	wfree(miv, sizeof(*miv));
}

/**
 * Hash table iterator callback.
 */
static void
mi_free_kv(gpointer key, gpointer value, gpointer unused_udata)
{
	(void) unused_udata;
	mi_key_free((struct mesh_info_key *) key);
	mi_val_free((struct mesh_info_val *) value);
}

/**
 * Callout queue callback invoked to clear the entry.
 */
static void
mi_clean(cqueue_t *unused_cq, gpointer obj)
{
	struct mesh_info_key *mik = (struct mesh_info_key *) obj;
	gpointer key;
	gpointer value;
	gboolean found;

	(void) unused_cq;
	found = g_hash_table_lookup_extended(mesh_info, mik, &key, &value);

	g_assert(found);
	g_assert(obj == key);
	g_assert(((struct mesh_info_val *) value)->cq_ev);

	if (upload_debug > 4)
		g_message("upload MESH info (%s/%s) discarded\n",
			ip_to_gchar(mik->ip), sha1_base32(mik->sha1));

	g_hash_table_remove(mesh_info, mik);
	((struct mesh_info_val *) value)->cq_ev = NULL;
	mi_free_kv(key, value, NULL);
}

/**
 * Get timestamp at which we last sent download mesh information for (IP,SHA1).
 * If we don't remember sending it, return 0.
 * Always records `now' as the time we sent mesh information.
 */
static guint32
mi_get_stamp(guint32 ip, const gchar *sha1, time_t now)
{
	struct mesh_info_key mikey;
	struct mesh_info_val *miv;
	struct mesh_info_key *mik;

	mikey.ip = ip;
	mikey.sha1 = sha1;

	miv = g_hash_table_lookup(mesh_info, &mikey);

	/*
	 * If we have an entry, reschedule the cleanup in MESH_INFO_TIMEOUT.
	 * Then return the timestamp.
	 */

	if (miv) {
		guint32 oldstamp;

		g_assert(miv->cq_ev);
		cq_resched(callout_queue, miv->cq_ev, MESH_INFO_TIMEOUT);

		oldstamp = miv->stamp;
		miv->stamp = (guint32) now;

		if (upload_debug > 4)
			g_message("upload MESH info (%s/%s) has stamp=%u\n",
				ip_to_gchar(ip), sha1_base32(sha1), oldstamp);

		return oldstamp;
	}

	/*
	 * Create new entry.
	 */

	mik = mi_key_make(ip, sha1);
	miv = mi_val_make((guint32) now);
	miv->cq_ev = cq_insert(callout_queue, MESH_INFO_TIMEOUT, mi_clean, mik);

	g_hash_table_insert(mesh_info, mik, miv);

	if (upload_debug > 4)
		g_message("new upload MESH info (%s/%s) stamp=%u\n",
			ip_to_gchar(ip), sha1_base32(sha1), (guint32) now);

	return 0;			/* Don't remember sending info about this file */
}


/**
 * Create a new upload request, and begin reading HTTP headers.
 */
void
upload_add(struct gnutella_socket *s)
{
	gnutella_upload_t *u;

	s->type = SOCK_TYPE_UPLOAD;

	u = upload_create(s, FALSE);

	/*
	 * Read HTTP headers fully, then call upload_request() when done.
	 */

	io_get_header(u, &u->io_opaque, bws.in, s, IO_HEAD_ONLY,
		call_upload_request, NULL, &upload_io_error);
}

/**
 * Prepare reception of a full HTTP header, including the leading request.
 * Will call upload_request() when everything has been parsed.
 */
void
expect_http_header(gnutella_upload_t *u, upload_stage_t new_status)
{
	struct gnutella_socket *s = u->socket;

	g_assert(s->resource.upload == u);
	g_assert(s->getline == NULL);
	g_assert(u->io_opaque == NULL);

	u->status = new_status;
	upload_fire_upload_info_changed(u);

	/*
	 * We're requesting the reading of a "status line", which will be the
	 * HTTP request.  It will be stored in a created s->getline entry.
	 * Once we're done, we'll end-up in upload_request(): the path joins
	 * with the one used for direct uploading.
	 */

	io_get_header(u, &u->io_opaque, bws.in, s, IO_SAVE_FIRST,
		call_upload_request, NULL, &upload_io_error);
}

/**
 * This is used for HTTP/1.1 persistent connections.
 *
 * Move the upload back to a waiting state, until a new HTTP request comes
 * on the socket.
 */
static void
upload_wait_new_request(gnutella_upload_t *u)
{
	socket_tos_normal(u->socket);
	expect_http_header(u, GTA_UL_WAITING);
}

/**
 * Got confirmation that the connection to the remote host was OK.
 * Send the GIV/QUEUE string, then prepare receiving back the HTTP request.
 */
void
upload_connect_conf(gnutella_upload_t *u)
{
	gchar giv[MAX_LINE_SIZE];
	struct gnutella_socket *s;
	size_t rw;
	ssize_t sent;

	g_assert(u);

	/*
	 * PARQ should send QUEUE information header here.
	 *		-- JA, 13/04/2003
	 */

	if (u->status == GTA_UL_QUEUE) {
		parq_upload_send_queue_conf(u);
		return;
	}

	g_assert(u->name);

	/*
	 * Send the GIV string, using our servent GUID.
	 */

	rw = gm_snprintf(giv, sizeof(giv), "GIV %u:%s/%s\n\n",
		u->index, guid_hex_str(servent_guid), u->name);

	s = u->socket;
	sent = bws_write(bws.out, &s->wio, giv, rw);
	if ((ssize_t) -1 == sent) {
		g_warning("Unable to send back GIV for \"%s\" to %s: %s",
			u->name, ip_to_gchar(s->ip), g_strerror(errno));
	} else if ((size_t) sent < rw) {
		g_warning("Only sent %d out of %d bytes of GIV for \"%s\" to %s: %s",
			(gint) sent, (gint) rw, u->name, ip_to_gchar(s->ip),
			g_strerror(errno));
	} else if (upload_debug > 2) {
		g_message("----Sent GIV to %s:\n%.*s----\n", ip_to_gchar(s->ip),
			(gint) rw, giv);
	}

	if ((size_t) sent != rw) {
		upload_remove(u, _("Unable to send GIV"));
		return;
	}

	/*
	 * We're now expecting HTTP headers on the connection we've made.
	 */

	expect_http_header(u, GTA_UL_HEADERS);
}

/**
 * Send back an HTTP error 404: file not found,
 */
static void
upload_error_not_found(gnutella_upload_t *u, const gchar *request)
{
	g_warning("Returned 404 for %s: %s", ip_to_gchar(u->socket->ip), request);
	upload_error_remove(u, NULL, 404, "Not Found");
}

/**
 * Check that we got an HTTP request, extracting the protocol version.
 *
 * @return TRUE if ok or FALSE otherwise (upload must then be aborted)
 */
static gboolean
upload_http_version(gnutella_upload_t *u, gchar *request, gint len)
{
	guint http_major, http_minor;

	/*
	 * Check HTTP protocol version. --RAM, 11/04/2002
	 */

	if (!http_extract_version(request, len, &http_major, &http_minor)) {
		upload_error_remove(u, NULL, 500, "Unknown/Missing Protocol Tag");
		return FALSE;
	}

	u->http_major = http_major;
	u->http_minor = http_minor;

	return TRUE;
}

/**
 * Get the shared_file to upload. Request has been extracted already, and is
 * passed as request. The same holds for the file index, which is passed as
 * index.
 *
 * @return the shared_file if found, NULL otherwise.
 */
static struct shared_file *
get_file_to_upload_from_index(
	gnutella_upload_t *u,
	header_t *header,
	gchar *uri,
	guint idx)
{
	struct shared_file *sf;
	gchar *buf;
	gchar *basename;
	gboolean sent_sha1 = FALSE;
	gchar digest[SHA1_RAW_SIZE];
	gchar *p;

	/*
	 * We must be cautious about file index changing between two scans,
	 * which may happen when files are moved around on the local library.
	 * If we serve the wrong file, and it's a resuming request, this will
	 * result in a corrupted file!
	 *		--RAM, 26/09/2001
	 *
	 * We now support URL-escaped queries.
	 *		--RAM, 16/01/2002
	 */

	sf = shared_file(idx);

	if (sf == SHARE_REBUILDING) {
		/* Retry-able by user, hence 503 */
		upload_error_remove(u, NULL, 503, "Library being rebuilt");
		return NULL;
	}

	/*
	 * Go to the basename of the file requested in the query.
	 * If we have one, `c' will point to '/' and `buf' to the start
	 * of the requested filename.
	 */

	buf = uri;
	if (!url_unescape(buf, TRUE)) {		/* Index is escape-safe anyway */
		upload_error_remove(u, NULL, 400, "Malformed Gnutella HTTP request");
		return NULL;
	}

	buf = strchr(buf, '/');
	if (!buf) {
		g_warning("Invalid encoded Gnutella HTTP URI: %s", uri);
		upload_error_remove(u, NULL, 400,
			"Invalid encoded Gnutella HTTP request");
		return NULL;
	}

	/*
	 * Go patch the first space we encounter before HTTP to be a NUL.
	 * Indeed, the request shoud be "GET /get/12/foo.txt HTTP/1.0".
	 *
	 * Note that if we don't find HTTP/ after the space, it's not an
	 * error: they're just sending an HTTP/0.9 request, which is awkward
	 * but we accept it.
	 */

	p = strrchr(buf, ' ');
	if (p && p[1]=='H' && p[2]=='T' && p[3]=='T' && p[4]=='P' && p[5]=='/')
		*p = '\0';
	else
		p = NULL;

	buf++; /* Skip the '/' */
	basename = buf;

    if (u->name != NULL)
        atom_str_free(u->name);
    u->name = atom_str_get(basename);

	/*
	 * If we have a X-Gnutella-Content-Urn, check whether we got a valid
	 * SHA1 URN in there and extract it.
	 */

	if ((buf = header_get(header, "X-Gnutella-Content-Urn")))
		sent_sha1 = dmesh_collect_sha1(buf, digest);

	/*
	 * If they sent a SHA1, look whether we got a matching file.
	 * If we do, let them know the URL changed by returning a 301, otherwise
	 * it's a 404.
	 */

	if (sent_sha1) {
		struct shared_file *sfn;

		/*
		 * If they sent a SHA1, maybe they have a download mesh as well?
		 *
		 * We ignore any mesh information when the SHA1 is not present
		 * because we cannot be sure that they are exact replicate of the
		 * file requested here.
		 *
		 *		--RAM, 19/06/2002
		 */

		huge_collect_locations(digest, header);

		/*
		 * They can share serveral clones of the same files, i.e. bearing
		 * distinct names yet having the same SHA1.  Therefore, check whether
		 * the SHA1 matches with what we found so far, and if it does,
		 * we found what they want.
		 */

		if (sf && sha1_hash_available(sf)) {
			if (!sha1_hash_is_uptodate(sf))
				goto sha1_recomputed;
			if (sha1_eq(digest, sf->sha1_digest))
				goto found;
		}

		/*
		 * Look whether we know this SHA1 at all, and compare the results
		 * with the file we found, if any.  Note that `sf' can be NULL at
		 * this point, in which case we'll redirect them with 301 if we
		 * know the hash.
		 */

		sfn = shared_file_by_sha1(digest);

		g_assert(sfn != SHARE_REBUILDING);	/* Or we'd have trapped above */

		if (sfn && sf != sfn) {
			gchar location[1024];
			const gchar *escaped;

			if (!sha1_hash_is_uptodate(sfn))
				goto sha1_recomputed;

			/*
			 * Be nice to pushed downloads: returning a 301 currently means
			 * a connection close, and they might not be able to reach us.
			 * Transparently remap their request.
			 *
			 * We don't do it for regular connections though, because servents
			 * MUST be prepared to deal with redirection requests.
			 *
			 *		--RAM, 14/10/2002
			 */

			if (u->push) {
				if (upload_debug > 4)
					g_message("INDEX FIXED (push, SHA1 = %s): "
						"requested %u, serving %u: %s\n",
						sha1_base32(digest), idx,
						sfn->file_index, sfn->file_path);
				sf = sfn;
				goto found;
			}

			/*
			 * Be nice for PFSP as well.  They must have learned about
			 * this from an alt-loc, and alt-locs we emit for those partially
			 * shared files are URNs.  Why did they request it by name?
			 *		--RAM, 12/10/2003
			 */

			if (sfn->fi != NULL) {
				if (upload_debug > 4)
					g_message("REQUEST FIXED (partial, SHA1 = %s): "
						"requested \"%s\", serving \"%s\"\n",
						sha1_base32(digest), basename,
						sfn->file_path);
				sf = sfn;
				goto found;
			}

			escaped = url_escape(sfn->name_nfc);

			gm_snprintf(location, sizeof(location),
				"Location: http://%s/get/%u/%s\r\n",
				ip_port_to_gchar(listen_ip(), listen_port),
				sfn->file_index, escaped);

			upload_error_remove_ext(u, sfn, location,
				301, "Moved Permanently");

			if (escaped != sfn->name_nfc) {
				g_free(deconstify_gchar(escaped));
				escaped = NULL;	/* Don't use G_FREE_NULL b/c of lvalue cast */
			}
			return NULL;
		}
		else if (sf == NULL)
			goto urn_not_found;

		/* FALL THROUGH */
	}

	/*
	 * If `sf' is NULL, the index was incorrect.
	 *
	 * Maybe we have a unique file with the same basename.  If we do,
	 * transparently return it instead of what they requested.
	 *
	 * We don't return a 301 in that case because the user did not supply
	 * the X-Gnutella-Content-Urn.  Therefore it's an old servent, and it
	 * cannot know about the new 301 return I've introduced.
	 *
	 * (RAM notified the GDF about 301 handling on June 5th, 2002 only)
	 */

	if (sf == NULL) {
		sf = shared_file_by_name(basename);

		g_assert(sf != SHARE_REBUILDING);	/* Or we'd have trapped above */

		if (upload_debug > 4) {
			if (sf)
				g_message("BAD INDEX FIXED: requested %u, serving %u: %s\n",
					idx, sf->file_index, sf->file_path);
			else
				g_message("BAD INDEX NOT FIXED: requested %u: %s\n",
					idx, basename);
		}

	} else if (0 != strcmp(basename, sf->name_nfc)) {
		struct shared_file *sfn = shared_file_by_name(basename);

		g_assert(sfn != SHARE_REBUILDING);	/* Or we'd have trapped above */

		if (upload_debug > 4) {
			if (sfn)
				g_message("INDEX FIXED: requested %u, serving %u: %s\n",
					idx, sfn->file_index, sfn->file_path);
			else
				g_message("INDEX MISMATCH: requested %u: %s (has %s)\n",
					idx, basename, sf->name_nfc);
		}

		if (sfn == NULL) {
			upload_error_remove(u, NULL, 409, "File index/name mismatch");
			return NULL;
		} else
			sf = sfn;
	}

	if (sf == NULL) {
		upload_error_not_found(u, uri);
		return NULL;
	}

found:
	g_assert(sf != NULL);

	if (p) *p = ' ';			/* Restore patched space */

	return sf;

urn_not_found:
	upload_error_remove(u, NULL, 404, "URN Not Found (urn:sha1)");
	return NULL;

sha1_recomputed:
	upload_error_remove(u, NULL, 503, "SHA1 is being recomputed");
	return NULL;
}

/**
 * Get the shared_file to upload from a given URN.
 * @return the shared_file if we have it, NULL otherwise
 */
static struct shared_file *
get_file_to_upload_from_urn(
	gnutella_upload_t *u,
	header_t *header,
	const gchar *uri)
{
	static const gchar * const urn_prefixes[] = {
		"urn:sha1:",
		"urn:bitprint:",
	};
	gchar hash[SHA1_BASE32_SIZE + 1];
	gchar digest[SHA1_RAW_SIZE];
	const gchar *p, *urn = uri;
	struct shared_file *sf;
	guint i;
	gchar *filename;

	/*
	 * We currently only support SHA1, but this allows us to process
	 * both "urn:sha1:" and "urn:bitprint:" URNs.
	 *		--RAM, 16/11/2002
	 */

	p = NULL; /* dumb compiler */
	for (i = 0; i < G_N_ELEMENTS(urn_prefixes); i++) {
		if (NULL != (p = is_strcaseprefix(urn, urn_prefixes[i])))
			break;
	}	

	if (!p)
		goto not_found;

	u->n2r = TRUE;		/* Remember we saw an N2R request */

	if (g_strlcpy(hash, p, sizeof hash) < SHA1_BASE32_SIZE)
		goto malformed;

	if (!urn_get_http_sha1(hash, digest))
		goto malformed;

	huge_collect_locations(digest, header);

	sf = shared_file_by_sha1(digest);

	/*
	 * Try to compute a suitable filename for the SHA1 digest.
	 * If we are sharing the file, then we have its filename.  Otherwise,
	 * it may be some file we were sharing via PFS and which has been
	 * completed, in which case we know about it via the "ignore database".
	 *		--RAM, 2005-08-01
	 */

	if (sf == NULL || sf == SHARE_REBUILDING) {
		filename = ignore_sha1_filename(digest);
		filename = filename == NULL ? atom_str_get(urn) :
			atom_str_get(filename);
	} else
		filename = atom_str_get(sf->name_nfc);

    if (u->name != NULL)
        atom_str_free(u->name);
    u->name = filename;

	if (sf == SHARE_REBUILDING) {
		/* Retry-able by user, hence 503 */
		upload_error_remove(u, NULL, 503, "Library being rebuilt");
		return NULL;
	}
	
	if (sf == NULL) {
		upload_error_not_found(u, uri);
		return NULL;
	} else if (!sha1_hash_is_uptodate(sf)) {
		upload_error_remove(u, NULL, 503, "SHA1 is being recomputed");
		return NULL;
	}

	return sf;

malformed:
	upload_error_remove(u, NULL, 400, "Malformed URN in /uri-res request");
	return NULL;

not_found:
	upload_error_not_found(u, uri);			/* Unknown URN => not found */
	return NULL;
}

/**
 * A dispatcher function to call either get_file_to_upload_from_index or
 * get_file_to_upload_from_sha1 depending on the syntax of the request.
 *
 * @return the shared_file if we got it, or NULL otherwise.
 * When NULL is returned, we have sent the error back to the client.
 */
static struct shared_file *
get_file_to_upload(gnutella_upload_t *u, header_t *header, gchar *request)
{
	const gchar *endptr;
	gchar *uri, *arg;

	/*
	 * We have either "GET uri" or "HEAD uri" at this point. The following
	 * will skip the request along with trailing blanks * and point to the
	 * beginning of the requested URI.
	 */

	uri = is_strprefix(request, "GET ");
	if (!uri)
		uri = is_strprefix(request, "HEAD ");
	g_assert(uri != NULL);
	uri = skip_ascii_blanks(uri);

    if (u->name == NULL)
        u->name = atom_str_get(uri);

	if (NULL != (arg = is_strprefix(uri, "/get/"))) {
		guint32 idx;
		gint error;
		
		idx = parse_uint32(arg, &endptr, 10, &error);
		if (!error && *endptr == '/')
			return get_file_to_upload_from_index(u, header, arg, idx);
	} else if (NULL != (arg = is_strprefix(uri, "/uri-res/N2R?"))) {
		return get_file_to_upload_from_urn(u, header, arg);
	}

	upload_error_not_found(u, request);
	return NULL;
}

/**
 * This routine is called by http_send_status() to generate the
 * X-Host line (added to the HTTP status) into `buf'.
 */
static void
upload_http_xhost_add(gchar *buf, gint *retval,
		gpointer unused_arg, guint32 unused_flags)
{
	size_t rw = 0;
	size_t length = *retval;
	guint32 ip;
	guint16 port;

	(void) unused_arg;
	(void) unused_flags;
	g_assert(length <= INT_MAX);
	g_assert(!is_firewalled);

	ip = listen_ip();
	port = listen_port;

	if (host_is_valid(ip, port)) {
		const gchar *xhost = ip_port_to_gchar(ip, port);
		size_t needed_room = strlen(xhost) + sizeof("X-Host: \r\n") - 1;

		if (length > needed_room)
			rw = gm_snprintf(buf, length, "X-Host: %s\r\n", xhost);
	}

	g_assert(rw < length);

	*retval = rw;
}

/**
 */
static void
upload_xfeatures_add(gchar *buf, gint *retval,
		gpointer unused_arg, guint32 unused_flags)
{
	size_t rw = 0;
	size_t length = *retval;

	(void) unused_arg;
	(void) unused_flags;
	g_assert(length <= INT_MAX);

	header_features_generate(&xfeatures.uploads, buf, length, &rw);

	*retval = rw;
}
/**
 * This routine is called by http_send_status() to generate the
 * SHA1-specific headers (added to the HTTP status) into `buf'.
 */
static void
upload_http_sha1_add(gchar *buf, gint *retval, gpointer arg, guint32 flags)
{
	gint rw = 0;
	gint length = *retval;
	struct upload_http_cb *a = (struct upload_http_cb *) arg;
	gnutella_upload_t *u = a->u;
	shared_file_t *sf = a->sf;
	gint needed_room;
	gint range_length;
	time_t now = time(NULL);
	guint32 last_sent;
	gchar tmp[160];
	gint mesh_len;
	gboolean need_available_ranges = FALSE;

	/*
	 * Room for header + base32 SHA1 + crlf
	 *
	 * We don't send the SHA1 if we're short on bandwidth and they
	 * made a request via the N2R resolver.  This will leave more room
	 * for the mesh information.
	 * NB: we use HTTP_CBF_BW_SATURATED, not HTTP_CBF_SMALL_REPLY on purpose.
	 *
	 * Also, if we sent mesh information for THIS upload, it means we're
	 * facing a follow-up request and we don't need to send them the SHA1
	 * again.
	 *		--RAM, 18/10/2003
	 */

	needed_room = 33 + SHA1_BASE32_SIZE + 2;

	if (
		length > needed_room &&
		!((flags & HTTP_CBF_BW_SATURATED) && u->n2r) &&
		u->last_dmesh == 0
	)
		rw += gm_snprintf(buf, length,
			"X-Gnutella-Content-URN: urn:sha1:%s\r\n",
			sha1_base32(a->sf->sha1_digest));


	/*
	 * PFSP-server: if they requested a partial file, let them know about
	 * the set of available ranges.
	 *
	 * To know how much room we can use for ranges, try to see how much
	 * locations we are going to fill.  In case we are under stringent
	 * size control, it would be a shame to not emit ranges because we
	 * want to leave size for alt-locs and yet there are none to emit!
	 */

	range_length = length - sizeof(tmp);

	/*
	 * Because of possible persistent uplaods, we have to keep track on
	 * the last time we sent download mesh information within the upload
	 * itself: the time for them to download a range will be greater than
	 * our expiration timer on the external mesh information.
	 */

	last_sent = u->last_dmesh ?
		u->last_dmesh :
		mi_get_stamp(u->socket->ip, sf->sha1_digest, now);

	/*
	 * Ranges are only emitted for partial files, so no pre-estimation of
	 * the size of the mesh entries is needed when replying for a full file.
	 *
	 * However, we're not going to include the available ranges when we
	 * are returning a 503 "busy" or "queued" indication, or any 4xx indication
	 * since the data will be stale by the time it is needed.  We only dump
	 * then when explicitly requested to do so.
	 */

	if (sf->fi != NULL && (flags & HTTP_CBF_SHOW_RANGES))
		need_available_ranges = TRUE;

	if (need_available_ranges) {
		mesh_len = dmesh_alternate_location(
			sf->sha1_digest, tmp, sizeof(tmp), u->socket->ip,
			last_sent, u->user_agent, NULL, FALSE);

		if ((guint) mesh_len < sizeof(tmp) - 5)
			range_length = length - mesh_len;	/* Leave more room for ranges */
	} else
		mesh_len = 1;			/* Try to emit alt-locs later */

	/*
	 * Emit the X-Available-Ranges: header if file is partial and we're
	 * not returning a busy signal.
	 */

	if (need_available_ranges && rw < range_length) {
		g_assert(pfsp_server);		/* Or we would not have a partial file */
		rw += file_info_available_ranges(sf->fi, &buf[rw], range_length - rw);
	}

	/*
	 * Emit alt-locs only if there is anything to emit, using all the
	 * remaining space, which may be larger than the room we tried to
	 * emit locations to in the above pre-check, in case there was only
	 * a little amount of ranges written!
	 */

	if (mesh_len > 0) {
		gint maxlen = length - rw;

		g_assert(length >= rw);
		g_assert(maxlen >= 0);
		
		/*
		 * If we're trying to limit the reply size, limit the size of the mesh.
		 * When we send X-Alt: locations, this leaves room for quite a few
		 * locations nonetheless!
		 *		--RAM, 18/10/2003
		 */

		if (flags & HTTP_CBF_SMALL_REPLY)
			maxlen = MIN((guint) maxlen, sizeof(tmp));

		rw += dmesh_alternate_location(
			sf->sha1_digest, &buf[rw], maxlen, u->socket->ip,
			last_sent, u->user_agent, NULL, FALSE);

		u->last_dmesh = now;
	}

	*retval = rw;
}

/**
 * This routine is called by http_send_status() to generate the
 * additionnal headers on a "416 Request range not satisfiable" error.
 */
static void
upload_416_extra(gchar *buf, gint *retval, gpointer arg, guint32 unused_flags)
{
	size_t rw = 0;
	size_t len = *retval;
	const struct upload_http_cb *a = (const struct upload_http_cb *) arg;
	const gnutella_upload_t *u = a->u;
	gchar fsize[32];

	(void) unused_flags;
	g_assert(len <= INT_MAX);
	rw = gm_snprintf(buf, len, "Content-Range: bytes */%s\r\n",
			uint64_to_string_buf(fsize, sizeof fsize, u->file_size));
	g_assert(rw < len);

	*retval = rw;
}

/**
 * This routine is called by http_send_status() to generate the
 * upload-specific headers into `buf'.
 */
static void
upload_http_status(gchar *buf, gint *retval, gpointer arg, guint32 unused_flags)
{
	gint rw = 0;
	gint length = *retval;
	struct upload_http_cb *a = (struct upload_http_cb *) arg;
	gnutella_upload_t *u = a->u;
	gchar csize[32];

	(void) unused_flags;

	if (!u->keep_alive)
		rw = gm_snprintf(buf, length, "Connection: close\r\n");

	rw += gm_snprintf(&buf[rw], length - rw,
		"Last-Modified: %s\r\n"
		"Content-Type: application/binary\r\n"
		"Content-Length: %s\r\n",
			date_to_rfc1123_gchar(a->mtime),
			uint64_to_string_buf(csize, sizeof csize, u->end - u->skip + 1));

	g_assert(rw < length);

	if (u->skip || u->end != (u->file_size - 1)) {
		gchar rsize[32], start_buf[32], end_buf[32];

		rw += gm_snprintf(&buf[rw], length - rw,
				"Content-Range: bytes %s-%s/%s\r\n",
				uint64_to_string_buf(start_buf, sizeof start_buf, u->skip),
				uint64_to_string_buf(end_buf, sizeof end_buf, u->end),
				uint64_to_string_buf(rsize, sizeof rsize, u->file_size));
	}

	g_assert(rw < length);

	*retval = rw;
}

/***
 *** TX deflate and link callbacks.
 ***/

static void
upload_tx_error(gpointer o, const gchar *reason, ...)
{
	gnutella_upload_t *u = o;
	va_list args;

	va_start(args, reason);
	socket_eof(u->socket);
	upload_remove_v(u, reason, args);
	va_end(args);
}

static struct tx_deflate_cb upload_tx_deflate_cb = {
	NULL,				/* add_tx_deflated */
	upload_tx_error,	/* shutdown */
};

static struct tx_link_cb upload_tx_link_cb = {
	NULL,				/* add_tx_written */
	upload_tx_error,	/* eof_remove */
	upload_tx_error,	/* eof_shutdown */
	NULL,				/* unflushq -- XXX rename that, it's node specific */
};

/**
 * Called to initiate the upload once all the HTTP headers have been
 * read.  Validate the request, and begin processing it if all OK.
 * Otherwise cancel the upload.
 */
static void
upload_request(gnutella_upload_t *u, header_t *header)
{
	struct gnutella_socket *s = u->socket;
	struct shared_file *reqfile = NULL;
    guint32 idx = 0;
	filesize_t skip = 0, end = 0;
	const gchar *fpath = NULL;
	gchar *user_agent = 0;
	gchar *buf;
	gchar *request = getline_str(s->getline);
	GSList *sl;
	gboolean head_only;
	gboolean has_end = FALSE;
	struct stat statbuf;
	time_t mtime, now = time((time_t *) NULL);
	struct upload_http_cb cb_parq_arg, cb_sha1_arg, cb_status_arg, cb_416_arg;
	gint http_code;
	const gchar *http_msg;
	http_extra_desc_t hev[8];
	guint hevcnt = 0;
	gchar *sha1 = NULL;
	gboolean is_followup =
		(u->status == GTA_UL_WAITING || u->status == GTA_UL_PFSP_WAITING);
	gboolean was_actively_queued = u->status == GTA_UL_QUEUED;
	gboolean range_unavailable = FALSE;
	gboolean replacing_stall = FALSE;
	gboolean use_sendfile;
	gchar *token;
	gboolean known_for_stalling;
	gint bh_flags = 0;

	if (upload_debug) {
		g_message("----%s Request from %s:\n%s",
			is_followup ? "Follow-up" : "Incoming",
			ip_to_gchar(s->ip),
			request);
		header_dump(header, stdout);
		g_message("----");
	}

	/*
	 * If we remove the upload in upload_remove(), we'll decrement
	 * running_uploads.  However, for followup-requests, the upload slot
	 * is already accounted for.
	 *
	 * Exceptions:
	 * We decremented `running_uploads' when moving to the GTA_UL_PFSP_WAITING
	 * state, since we don't know whether they will re-emit something.
	 * Therefore, it is necessary to re-increment it here.
	 *
	 *
	 * This is for the moment being done if the upload really seems to be
	 * getting an upload slot. This is to avoid messing with active queuing
	 *		-- JA, 09/05/03
	 */

	if (!is_followup || u->status == GTA_UL_PFSP_WAITING)
		running_uploads++;

	/*
	 * Technically, we have not started sending anything yet, but this
	 * also serves as a marker in case we need to call upload_remove().
	 * It will not send an HTTP reply by itself.
	 */

	u->status = GTA_UL_SENDING;
	u->last_update = time((time_t *) 0);	/* Done reading headers */

	/*
	 * If `head_only' is true, the request was a HEAD and we're only going
	 * to send back the headers.
	 */

	head_only = (request[0] == 'H');

	/*
	 * Extract User-Agent.
	 *
	 * X-Token: GTKG token
	 * User-Agent: whatever
	 * Server: whatever (in case no User-Agent)
	 */

	token = header_get(header, "X-Token");
	user_agent = header_get(header, "User-Agent");

	/* Maybe they sent a Server: line, thinking they're a server? */
	if (user_agent == NULL)
		user_agent = header_get(header, "Server");

	if (u->user_agent == NULL && user_agent != NULL) {
		gboolean faked = !version_check(user_agent, token, u->ip);
		if (faked) {
			gchar name[1024];

			name[0] = '!';
			g_strlcpy(&name[1], user_agent, sizeof name - 1);
			u->user_agent = atom_str_get(name);
		} else
			u->user_agent = atom_str_get(user_agent);
	}

	/*
	 * Make sure there is the HTTP/x.x tag at the end of the request,
	 * thereby ruling out the HTTP/0.9 requests.
	 *
	 * This has to be done early, and before calling get_file_to_upload()
	 * or the getline_length() call will no longer represent the length of
	 * the string, since URL-unescaping happens inplace and can "shrink"
	 * the request.
	 */

	if (!upload_http_version(u, request, getline_length(s->getline)))
		return;

	/*
	 * IDEA
	 *
	 * To prevent people from hammering us, we should setup a priority queue
	 * coupled to a hash table for fast lookups, where we would record the
	 * last failed attempt and when it was.	As soon as there is a request,
	 * we would move the record for the IP address at the beginning of the
	 * queue, and drop the tail when we reach our size limit.
	 *
	 * Then, if we discover that a given IP re-issues too frequent requests,
	 * we would start differing our reply by not sending the error immediately
	 * but scheduling that some time in the future.	We would begin to use
	 * many file descriptors that way, so we trade CPU time for another scarce
	 * resource.  However, if someone is hammering us with connections,
	 * he would have to wait for our reply before knowing the failure, and
	 * it would slow him down, even if he retried immediately.
	 *
	 * Alternatively, instead of differing the 503 reply, we could send a
	 * "403 Forbidden to bad citizens" instead, and chances are that servents
	 * abort retries on failures other than 503...
	 *
	 *				--RAM, 09/09/2001
	 */

	if (
		NULL != is_strprefix(request, "GET / HTTP/") ||
		NULL != is_strprefix(request, "HEAD / HTTP/")
	) {
		u->browse_host = TRUE;
		u->name = atom_str_get(_("<Browse Host Request>"));
		
		if (!browse_host_enabled) {
			upload_error_remove(u, NULL, 403, "Browse Host Disabled");
			return;
		}

		/*
		 * If it's a HEAD request, let them know we support Browse Host.
		 */

		if (NULL != is_strprefix(request, "HEAD ")) {
			static const gchar msg[] = N_("Browse Host Enabled");
			http_send_status(u->socket, 200, FALSE, hev, hevcnt, msg);
			upload_remove(u, _(msg));
			return;
		}

		/*
		 * Look at an Accept: line containing "application/x-gnutella-packets".
		 * If we get that, then we can send query hits backs.  Otherwise,
		 * we'll send HTML output.
		 */

		buf = header_get(header, "Accept");
		if (buf) {
			/* XXX needs more rigourous parsing */
			if (strstr(buf, "application/x-gnutella-packets"))
				bh_flags |= BH_QHITS;
			else if (strstr(buf, "text/html"))
				bh_flags |= BH_HTML;
			else if (strstr(buf, "*/*") || strstr(buf, "text/*"))
				bh_flags |= BH_HTML;	/* A browser probably */
			else {
				upload_error_remove(u, NULL, 406, "Not Acceptable");
				return;
			}
		} else
			bh_flags |= BH_HTML;		/* No Accept, default to HTML */

	    g_assert(hevcnt < G_N_ELEMENTS(hev));
		hev[hevcnt].he_type = HTTP_EXTRA_LINE;
// XXX	hev[hevcnt].he_msg = (bh_flags & BH_HTML) ?
		hev[hevcnt].he_msg = (TRUE) ?
			"Content-Type: text/html; charset=utf-8\r\n" :
			"Content-Type: application/x-gnutella-packets\r\n";
		hev[hevcnt++].he_arg = NULL;

		/*
		 * Accept-Encoding -- see whether they want compressed output.
		 */

		buf = header_get(header, "Accept-Encoding");
		/* XXX needs more rigourous parsing */
		if (NULL != buf && strstr(buf, "deflate")) {
			bh_flags |= BH_DEFLATE;

			g_assert(hevcnt < G_N_ELEMENTS(hev));
			hev[hevcnt].he_type = HTTP_EXTRA_LINE;
			hev[hevcnt].he_msg = "Content-Encoding: deflate\r\n";
			hev[hevcnt++].he_arg = NULL;
		}

		/*
		 * Starting at HTTP/1.1, we can send chunked data back.
		 */

		if (u->http_major > 1 || (u->http_major == 1 && u->http_minor >= 1)) {
			bh_flags |= BH_CHUNKED;

			g_assert(hevcnt < G_N_ELEMENTS(hev));
			hev[hevcnt].he_type = HTTP_EXTRA_LINE;
			hev[hevcnt].he_msg = "Transfer-Encoding: chunked\r\n";
			hev[hevcnt++].he_arg = NULL;
		}
	} else {
		u->browse_host = FALSE;
		
		reqfile = get_file_to_upload(u, header, request);
		if (!reqfile) {
			/* get_file_to_upload() has signaled the error already */
			return;
		}
	}
	

	/*
	 * Check vendor-specific banning.
	 */

	if (user_agent) {
		const gchar *msg = ban_vendor(user_agent);

		if (msg != NULL) {
			ban_record(u->ip, msg);
			upload_error_remove(u, NULL, 403, "%s", msg);
			return;
		}
	}

	/* Pick up the X-Remote-IP or Remote-IP header */
	node_check_remote_ip_header(u->ip, header);

	if (reqfile) {
		idx = reqfile->file_index;
		sha1 = sha1_hash_available(reqfile) ? reqfile->sha1_digest : NULL;


		/*
		 * If we pushed this upload, and they are not requesting the same
		 * file, that's OK, but warn.
		 *		--RAM, 31/12/2001
		 */

		if (u->push && idx != u->index)
			g_warning("Host %s sent PUSH for %u (%s), now requesting %u (%s)",
				ip_to_gchar(u->ip), u->index, u->name, idx, reqfile->name_nfc);

		/*
		 * We already have a non-NULL u->name in the structure, because we
		 * saved the uri there or the name from a push request.
		 * However, we want to display the actual name of the shared file.
		 *		--Richard, 20/11/2002
		 */

		u->index = idx;
		/* Identify file for follow-up reqs */
		if (!u->sha1 && sha1)
			u->sha1 = atom_sha1_get(sha1);

		if (u->name != NULL)
			atom_str_free(u->name);

		u->name = atom_str_get(reqfile->name_nfc);
		u->file_info = reqfile->fi;

		/*
		 * Range: bytes=10453-23456
		 */

		buf = header_get(header, "Range");
		if (buf && reqfile->file_size != 0) {
			http_range_t *r;
			GSList *ranges =
				http_range_parse("Range", buf,  reqfile->file_size, user_agent);

			if (ranges == NULL) {
				upload_error_remove(u, NULL, 400, "Malformed Range request");
				return;
			}

			/*
			 * We don't properly support multiple ranges yet.
			 * Just pick the first one, but warn so we know when people start
			 * requesting multiple ranges at once.
			 *		--RAM, 27/01/2003
			 */

			if (g_slist_next(ranges) != NULL) {
				if (upload_debug)
					g_warning("client %s <%s> requested several ranges "
						"for \"%s\": %s", ip_to_gchar(u->ip),
						u->user_agent ? u->user_agent : "", reqfile->name_nfc,
						http_range_to_gchar(ranges));
			}

			r = (http_range_t *) ranges->data;

			g_assert(r->start <= r->end);
			g_assert(r->end < reqfile->file_size);

			skip = r->start;
			end = r->end;
			has_end = TRUE;

			http_range_free(ranges);
		}

		/*
		 * Validate the requested range.
		 */

		fpath = reqfile->file_path;
		u->file_size = reqfile->file_size;

		if (!has_end)
			end = u->file_size - 1;

		/*
		 * PFSP-server: restrict the end of the requested range if the file
		 * we're about to upload is only partially available.  If the range
		 * is not yet available, signal it but don't break the connection.
		 *		--RAM, 11/10/2003
		 */

		if (
				reqfile->fi != NULL &&
				!file_info_restrict_range(reqfile->fi, skip, &end)
		   ) {
			g_assert(pfsp_server);
			range_unavailable = TRUE;
		} else {
			if (u->unavailable_range)	/* Previous request was for bad chunk */
				is_followup = FALSE;		/* Perform as if original request */
			u->unavailable_range = FALSE;
		}

		u->skip = skip;
		u->end = end;
		u->pos = skip;

	} /* reqfile */

	hev[hevcnt].he_type = HTTP_EXTRA_CALLBACK;
	hev[hevcnt].he_cb = upload_xfeatures_add;
	hev[hevcnt++].he_arg = NULL;

	/*
	 * If this is a pushed upload, and we are not firewalled, then tell
	 * them they can reach us directly by outputting an X-Host line.
	 *
	 * Otherwise, if we are firewalled, tell them about possible push
	 * proxies we could have.
	 */

	if (u->push && !is_firewalled) {
		/* Only send X-Host the first time we reply */
		if (!is_followup) {
			hev[hevcnt].he_type = HTTP_EXTRA_CALLBACK;
			hev[hevcnt].he_cb = upload_http_xhost_add;
			hev[hevcnt++].he_arg = NULL;
		}
	} else if (is_firewalled) {
		/* Send X-Push-Proxy each time: might have changed! */
		hev[hevcnt].he_type = HTTP_EXTRA_CALLBACK;
		hev[hevcnt].he_cb = node_http_proxies_add;
		hev[hevcnt++].he_arg = NULL;
	}

	/*
	 * Include X-Hostname if not in a followup reply and if we have a
	 * known hostname, for which the user gave permission to advertise.
	 */

	if (
		!is_firewalled && !is_followup &&
		give_server_hostname && 0 != *server_hostname
	) {
		hev[hevcnt].he_type = HTTP_EXTRA_CALLBACK;
		hev[hevcnt].he_cb = http_hostname_add;
		hev[hevcnt++].he_arg = NULL;
	}

	g_assert(hevcnt <= G_N_ELEMENTS(hev));

	/*
	 * When requested range is invalid, the HTTP 416 reply should contain
	 * a Content-Range header giving the total file size, so that they
	 * know the limits of what they can request.
	 *
	 * XXX due to the use of http_range_parse() above, the following can
	 * XXX no longer trigger here.  However, http_range_parse() should be
	 * XXX able to report out-of-range errors so we can report a true 416
	 * XXX here.  Hence I'm not removing this code.  --RAM, 11/10/2003
	 */

	if (reqfile && (skip >= u->file_size || end >= u->file_size)) {
		static const gchar msg[] = "Requested range not satisfiable";

		cb_416_arg.u = u;
		cb_416_arg.sf = reqfile;

		hev[hevcnt].he_type = HTTP_EXTRA_CALLBACK;
		hev[hevcnt].he_cb = upload_416_extra;
		hev[hevcnt++].he_arg = &cb_416_arg;

		g_assert(hevcnt <= G_N_ELEMENTS(hev));

		(void) http_send_status(u->socket, 416, FALSE, hev, hevcnt, msg);
		upload_remove(u, msg);
		return;
	}

	/*
	 * If HTTP/1.1 or above, check the Host header.
	 *
	 * We require it because HTTP does, but we don't really care for
	 * now.  Moreover, we might not know our external IP correctly,
	 * so we have little ways to check that the Host refers to us.
	 *
	 *		--RAM, 11/04/2002
	 */

	if ((u->http_major == 1 && u->http_minor >= 1) || u->http_major > 1) {
		const gchar *host = header_get(header, "Host");

		if (host == NULL) {
			upload_error_remove(u, NULL, 400, "Missing Host Header");
			return;
		}
	}

	/*
	 * If we don't share, abort. --RAM, 11/01/2002
	 * Use 5xx error code, it's a server-side problem --RAM, 11/04/2002
	 *
	 * We do that quite late in the process to be able to gather as
	 * much as possible from the request for tracing in the GUI.
	 * Also, if they request something wrong, they ought to know it ASAP.
	 */

	if (!upload_is_enabled()) {
		upload_error_remove(u, NULL, 503, "Sharing currently disabled");
		return;
	}

	/*
	 * We now have enough information to display the request in the GUI.
	 */

	upload_fire_upload_info_changed(u);

	/*
	 * A follow-up request must be for the same file, since the slot is
	 * allocated on the basis of one file.  We compare SHA1s if available,
	 * otherwise indices, in case the library has been rebuilt.
	 */

	if (
		is_followup &&
		!(sha1 && u->sha1 && sha1_eq(sha1, u->sha1)) && idx != u->index
	) {
		g_warning("Host %s sent initial request for %u (%s), "
			"now requesting %u (%s)",
			ip_to_gchar(s->ip),
			u->index, u->name, idx, reqfile->name_nfc);
		upload_error_remove(u, NULL, 400, "Change of Resource Forbidden");
		return;
	}

	/*
	 * Do we have to keep the connection after this request?
	 */

	buf = header_get(header, "Connection");

	if (u->http_major > 1 || (u->http_major == 1 && u->http_minor >= 1)) {
		/* HTTP/1.1 or greater -- defaults to persistent connections */
		u->keep_alive = TRUE;
		if (buf && 0 == ascii_strcasecmp(buf, "close"))
			u->keep_alive = FALSE;
	} else {
		/* HTTP/1.0 or lesser -- must request persistence */
		u->keep_alive = FALSE;
		if (buf && 0 == ascii_strcasecmp(buf, "keep-alive"))
			u->keep_alive = TRUE;
	}

	/*
	 * Implement Tranfer-Encoding "chunked" to allow keep-alive
	 * connections with unknown content-lengths. This is a
	 * required feature of HTTP/1.1 - HTTP/1.0 doesn't support it.
	 *
	 * We don't want to determine the content-length in advance and we
	 * do not support chunked transfers either. Thus, the only way to
	 * indicate the end-of-message is an EOF which means we keep-alive
	 * connections must be disabled for this purpose.
	 */

	if (u->browse_host && !(bh_flags & BH_CHUNKED))
		u->keep_alive = FALSE;
	
	/*
	 * If the requested range was determined to be unavailable, signal it
	 * to them.  Break the connection if it was a HEAD request, but allow
	 * them an extra request if the last one was for a valid range.
	 *		--RAM, 11/10/2003
	 */

	if (reqfile && range_unavailable) {
		static const gchar msg[] = "Requested range not available yet";

		g_assert(sha1_hash_available(reqfile));
		g_assert(pfsp_server);

		cb_sha1_arg.u = u;
		cb_sha1_arg.sf = reqfile;

		hev[hevcnt].he_type = HTTP_EXTRA_CALLBACK;
		hev[hevcnt].he_cb = upload_http_sha1_add;
		hev[hevcnt++].he_arg = &cb_sha1_arg;

		g_assert(hevcnt <= G_N_ELEMENTS(hev));

		if (!head_only && u->keep_alive && !u->unavailable_range) {
			/*
			 * Cleanup data structures.
			 */

			io_free(u->io_opaque);
			u->io_opaque = NULL;

			getline_free(s->getline);
			s->getline = NULL;

			u->unavailable_range = TRUE;
			(void) http_send_status(u->socket, 416, TRUE, hev, hevcnt, msg);
			running_uploads--;		/* Re-incremented if they ever come back */
			expect_http_header(u, GTA_UL_PFSP_WAITING);
		} else {
			(void) http_send_status(u->socket, 416, FALSE, hev, hevcnt, msg);
			upload_remove(u, msg);
		}
		return;
	}

	if (!head_only) {
		GSList *to_remove = NULL;

		/*
		 * Ensure that noone tries to download the same file twice, and
		 * that they don't get beyond the max authorized downloads per IP.
		 * NB: SHA1 are atoms, so it's OK to compare their addresses.
		 *
		 * This needs to be done before the upload enters PARQ. PARQ doesn't
		 * handle multiple uploads for the same file very well as it tries to
		 * keep 1 pointer to the upload structure as long as that structure
		 * exists.
		 * 		-- JA 12/7/'03
		 */

		for (sl = list_uploads; sl; sl = g_slist_next(sl)) {
			gnutella_upload_t *up = (gnutella_upload_t *) (sl->data);
			g_assert(up);
			if (up == u)
				continue;				/* Current upload is already in list */
			if (!UPLOAD_IS_SENDING(up) && up->status != GTA_UL_QUEUED)
				continue;
			if (
				up->socket->ip == s->ip && (
					(up->index != URN_INDEX && up->index == idx) ||
					(u->sha1 && up->sha1 == u->sha1)
				)
			) {
				/*
				 * If the duplicate upload we have is stalled or showed signs
				 * of early stalling, the remote end might have seen no data
				 * and is trying to reconnect.  Kill that old upload.
				 *		--RAM, 07/12/2003
				 */

				if (up->flags & (UPLOAD_F_STALLED|UPLOAD_F_EARLY_STALL))
					to_remove = g_slist_prepend(to_remove, up);
				else {
					upload_error_remove(u, NULL, 503,
						"Already downloading that file");
					g_slist_free(to_remove);
					return;
				}
			}
		}

		/*
		 * Kill pre-stalling or stalling uploads we spotted as being
		 * identical to their current request.  There should be only one
		 * at most.
		 */

		for (sl = to_remove; sl; sl = g_slist_next(sl)) {
			gnutella_upload_t *up = (gnutella_upload_t *) (sl->data);
			g_assert(up);

			g_warning("stalling connection to %s (%s) replaced "
				"after %s bytes sent, stall counter at %d",
				ip_to_gchar(up->ip), upload_vendor_str(up),
				uint64_to_string(up->sent), stalled);

			upload_remove(up, _("Stalling upload replaced"));
			replacing_stall = TRUE;
		}
		g_slist_free(to_remove);
	}

	/*
	 * We let all HEAD request go through, whether we're busy or not, since
	 * we only send back the header.
	 *
	 * Follow-up requests already have their slots.
	 */

	if (reqfile && !head_only) {
		if (is_followup && parq_upload_lookup_position(u) == (guint) -1) {
			/*
			 * Allthough the request is an follow up request, the last time the
			 * upload didn't get a parq slot. There is probably a good reason
			 * for this. The most logical explantion is that the client did a
			 * HEAD only request with a keep-alive. However, no parq structure
			 * is set for such an upload. So we should treat as a new upload.
			 *		-- JA, 1/06/'03
			 */
			is_followup = FALSE;
		}

		if (parq_upload_queue_full(u)) {
			upload_error_remove(u, reqfile, 503, "Queue full");
			return;
		}

		u->parq_opaque = parq_upload_get(u, header, replacing_stall);

		if (u->parq_opaque == NULL) {
			upload_error_remove(u, reqfile, 503,
				"Another connection is still active");
			return;
		}
	}

	if (reqfile && !(head_only || is_followup)) {
		/*
		 * Even though this test is less costly than the previous ones, doing
		 * it afterwards allows them to be notified of a mismatch whilst they
		 * wait for a download slot.  It would be a pity for them to get
		 * a slot and be told about the mismatch only then.
		 *		--RAM, 15/12/2001
		 *
 		 * Althought the uploads slots are full, we could try to queue
		 * the download in PARQ. If this also fails, than the requesting client
		 * is out of luck.
		 *		--JA, 05/02/2003
		 *
		 */

		if (!parq_upload_request(u, u->parq_opaque, running_uploads - 1)) {
			gboolean parq_allows = FALSE;

			if (parq_upload_lookup_position(u) == (guint) -1) {
				time_t expire = parq_banned_source_expire(u->ip);
				gchar retry_after[80];
				gint delay = delta_time(expire, time(NULL));

				if (delay <= 0)
					delay = 60;		/* Let them retry in a minute, only */


				gm_snprintf(retry_after, sizeof(retry_after),
					"Retry-After: %d\r\n", (gint) delay);

				/*
				 * Looks like upload got removed from PARQ queue. For now this
				 * only happens when a client got banned. Bye bye!
			 	 *		-- JA, 19/05/'03
				 */
				upload_error_remove_ext(u, reqfile, retry_after, 403,
					"%s not honoured; removed from PARQ queue",
					was_actively_queued ?
						"Minimum retry delay" : "Retry-After");
				return;
			}

			/*
			 * Support for bandwith-dependent number of upload slots.
		 	 * The upload bandwith limitation has to be enabled, otherwise
		 	 * we cannot be sure that we have reasonable values for the
		 	 * outgoing bandwith set.
		 	 *		--TF 30/05/2002
		 	 *
		 	 * NB: if max_uploads is 0, then we disable sharing, period.
		 	 *
		 	 * Require that BOTH the average and "instantaneous" usage be
		 	 * lower than the minimum to trigger the override.  This will
		 	 * make it more robust when bandwidth stealing is enabled.
		 	 *		--RAM, 27/01/2003
		 	 *
			 */

			if (
				upload_is_enabled() &&
				bw_ul_usage_enabled &&
				bws_out_enabled &&
				(gulong) bsched_pct(bws.out) < ul_usage_min_percentage &&
				(gulong) bsched_avg_pct(bws.out) < ul_usage_min_percentage
			) {
				if (parq_upload_request_force(
						u, u->parq_opaque, running_uploads - 1)) {
					parq_allows = TRUE;
					if (upload_debug > 4)
						g_message(
							"Overriden slot limit because u/l b/w used at "
							"%d%% (minimum set to %d%%)\n",
							bsched_avg_pct(bws.out), ul_usage_min_percentage);
				}
			}

			if (!parq_allows) {
				if (u->status == GTA_UL_QUEUED) {
					/*
					 * Cleanup data structures.
					 */

					io_free(u->io_opaque);
					g_assert(u->io_opaque == NULL);

					getline_free(s->getline);
					s->getline = NULL;

					send_upload_error(u, reqfile, 503,
						  "Queued (slot %d, ETA: %s)",
						  parq_upload_lookup_position(u),
						  short_time(parq_upload_lookup_eta(u)));

					u->error_sent = 0;	/* Any new request should be allowed
										   to retrieve an error code */

					/* Avoid data timeout */
					u->last_update = parq_upload_lookup_lifetime(u) -
						  upload_connected_timeout;

					running_uploads--;	/* will get increased next time
										   upload_request is called */

					expect_http_header(u, GTA_UL_QUEUED);
					return;
				} else
				if (parq_upload_queue_full(u)) {
					upload_error_remove(u, reqfile, 503, "Queue full");
				} else {
					upload_error_remove(u, reqfile,	503,
						N_("Queued (slot %d, ETA: %s)"),
						parq_upload_lookup_position(u),
						short_time(parq_upload_lookup_eta(u)));
				}
				return;
			}
		}
	}

	if (reqfile && !head_only) {
		/*
		 * Avoid race conditions in case of QUEUE callback answer: they might
		 * already have got an upload slot since we sent the QUEUE and they
		 * replied.  Not sure this is the right fix though, but it does
		 * the job.
		 *		--RAM, 24/12/2003
		 */

		if (!is_followup && !parq_upload_ip_can_proceed(u)) {
			upload_error_remove(u, reqfile, 503,
				"Too many uploads to this IP address (limit=%d)",
				max_uploads_ip);
			return;
		}

		parq_upload_busy(u, u->parq_opaque);
	}

	if (reqfile) {

		if (-1 == stat(fpath, &statbuf)) {
			upload_error_not_found(u, request);
			return;
		}

		/*
		 * Ensure that a given persistent connection never requests more than
		 * the total file length.  Add 10% to account for partial overlapping
		 * ranges.
		 */

		u->total_requested += end - skip + 1;

		if ((u->total_requested / 11) * 10 > u->file_size) {
			g_warning("host %s (%s) requesting more than there is to %u (%s)",
				ip_to_gchar(s->ip), upload_vendor_str(u), u->index, u->name);
			upload_error_remove(u, NULL, 400, "Requesting Too Much");
			return;
		}

		/* Open the file for reading , READONLY just in case. */
		if ((u->file_desc = file_open(fpath, O_RDONLY)) < 0) {
			upload_error_not_found(u, request);
			return;
		}
	}

#if defined(USE_MMAP) || defined(HAVE_SENDFILE)
	use_sendfile = !sendfile_failed && !SOCKET_USES_TLS(u->socket) && reqfile;
#else
	use_sendfile = FALSE;
#endif /* USE_MMAP || HAVE_SENDFILE */
	
	if (!use_sendfile) {
		
		/* If we got a valid skip amount then jump ahead to that position */
		if (u->skip > 0) {
			if (-1 == lseek(u->file_desc, u->skip, SEEK_SET)) {
				upload_error_remove(u, NULL,
					500, "File seek error: %s", g_strerror(errno));
				return;
			}
		}

		u->bpos = 0;
		u->bsize = 0;

		if (!is_followup) {
			u->buf_size = READ_BUF_SIZE;
			u->buffer = (gchar *) g_malloc(u->buf_size);
		}
	}

	/*
	 * Set remaining upload information
	 */

	u->start_date = time((time_t *) NULL);
	u->last_update = time((time_t *) 0);

	/*
	 * Prepare date and modification time of file.
	 */

	mtime = statbuf.st_mtime;
	if (delta_time(mtime, now) > 0)
		mtime = now;			/* Clock skew on file server */

	/*
	 * On linux, turn TCP_CORK on so that we only send out full TCP/IP
	 * frames.  The exact size depends on your LAN interface, but on
	 * Ethernet, it's about 1500 bytes.
	 *
	 * If they have some connections stalling recently, reduce the send buffer
	 * size.  This will lower TCP's throughput but will prevent us from
	 * writing too much before detecting the stall.
	 */

	known_for_stalling = NULL != aging_lookup(stalling_uploads,
									GUINT_TO_POINTER(u->ip));

	if (stalled <= STALL_THRESH && !known_for_stalling) {
		sock_cork(s, TRUE);
		socket_tos_throughput(s);
	} else {
		socket_tos_normal(s);	/* Make sure ACKs come back faster */
		sock_send_buf(s, UP_SEND_BUFSIZE, TRUE);	/* Shrink TX buffer */
	}

	/*
	 * Send back HTTP status.
	 */

	if (reqfile && (u->skip || u->end != (u->file_size - 1))) {
		http_code = 206;
		http_msg = "Partial Content";
	} else {
		http_code = 200;
		http_msg = "OK";
	}

	/*
	 * PARQ ID, emitted if needed.
	 *
	 * We do that before calling upload_http_status() to avoid lacking
	 * room in the headers, should there by any alternate location present.
	 *
	 * We never emit the queue ID for HEAD requests, nor during follow-ups
	 * (which always occur for the same resource, meaning the PARQ ID was
	 * arlready sent for those).
	 */

	if (reqfile && !head_only && !is_followup && !parq_ul_id_sent(u)) {
		cb_parq_arg.u = u;

		hev[hevcnt].he_type = HTTP_EXTRA_CALLBACK;
		hev[hevcnt].he_cb = parq_upload_add_header_id;
		hev[hevcnt++].he_arg = &cb_parq_arg;
	}

	if (reqfile) {
		/*
		 * Date, Content-Length, etc...
		 */

		cb_status_arg.u = u;
		cb_status_arg.now = now;
		cb_status_arg.mtime = mtime;
		cb_status_arg.sf = reqfile;

		hev[hevcnt].he_type = HTTP_EXTRA_CALLBACK;
		hev[hevcnt].he_cb = upload_http_status;
		hev[hevcnt++].he_arg = &cb_status_arg;
	}

	g_assert(hevcnt <= G_N_ELEMENTS(hev));
	
	/*
	 * Propagate the SHA1 information for the file, if we have it.
	 */

	if (sha1) {	
		cb_sha1_arg.u = u;
		cb_sha1_arg.sf = reqfile;

		hev[hevcnt].he_type = HTTP_EXTRA_CALLBACK;
		hev[hevcnt].he_cb = upload_http_sha1_add;
		hev[hevcnt++].he_arg = &cb_sha1_arg;
		g_assert(hevcnt <= G_N_ELEMENTS(hev));
	}


	if (
		!http_send_status(u->socket, http_code, u->keep_alive,
			hev, hevcnt, "%s", http_msg)
	) {
		upload_remove(u, _("Cannot send whole HTTP status"));
		return;
	}

	/*
	 * Cleanup data structures.
	 */

	io_free(u->io_opaque);
	u->io_opaque = NULL;

	getline_free(s->getline);
	s->getline = NULL;

	/*
	 * If we need to send only the HEAD, we're done. --RAM, 26/12/2001
	 */

	if (head_only) {
		if (u->keep_alive)
			upload_wait_new_request(u);
		else
			upload_remove(u, NULL);		/* No message, everything was OK */
		return;
	}

	/*
	 * Install the output I/O, which is via a bandwidth limited source.
	 */

	g_assert(s->gdk_tag == 0);
	g_assert(u->bio == NULL);

	if (u->browse_host) {
		gnet_host_t host;

		host.ip = u->socket->ip;
		host.port = u->socket->port;

		u->special = browse_host_open(
			u, &host, upload_special_writable,
			&upload_tx_deflate_cb, &upload_tx_link_cb,
			&u->socket->wio, bh_flags);
	} else
		u->bio = bsched_source_add(bws.out, &s->wio,
			BIO_F_WRITE, upload_writable, u);

	if (reqfile)
		upload_stats_file_begin(u);
}

static void
upload_completed(gnutella_upload_t *u)
{
	/*
	 * We do the following before cloning, since this will reset most
	 * of the information, including the upload name.  If they chose
	 * to clear uploads immediately, they will incur a small overhead...
	 */
	u->status = GTA_UL_COMPLETE;

	gnet_prop_set_guint32_val(PROP_TOTAL_UPLOADS, total_uploads + 1);
	upload_fire_upload_info_changed(u); /* gui must update last state */

	/*
	 * If we're going to keep the connection, we must clone the upload
	 * structure, since it is associated to the GUI entry.
	 */

	if (u->keep_alive) {
		gnutella_upload_t *cu = upload_clone(u);
		upload_wait_new_request(cu);
		/*
		 * Don't decrement counters, we're still using the same slot.
		 */
	} else {
		registered_uploads--;
		running_uploads--;
	}

	upload_remove(u, NULL);
}

/**
 * @return TRUE if an exception occured, the upload has been removed
 *         in this case. FALSE if everything is OK.
 */
static gboolean 
upload_handle_exception(gnutella_upload_t *u, inputevt_cond_t cond)
{
	if (cond & INPUT_EVENT_EXCEPTION) {
		/* If we can't write then we don't want it, kill the socket */
		socket_eof(u->socket);
		upload_remove(u, _("Write exception"));
		return TRUE;
	}

	return FALSE;
}

/**
 * Called when output source can accept more data.
 */
static void
upload_writable(gpointer up, gint unused_source, inputevt_cond_t cond)
{
	gnutella_upload_t *u = up;
	ssize_t written;
	filesize_t amount;
	size_t available;
	gboolean use_sendfile;

	(void) unused_source;

	if (upload_handle_exception(u, cond))
		return;

   /*
 	* Compute the amount of bytes to send.
 	*/

	amount = u->end - u->pos + 1;
	g_assert(amount > 0);

#if defined(USE_MMAP) || defined(HAVE_SENDFILE)
	use_sendfile = !sendfile_failed && !SOCKET_USES_TLS(u->socket);
#else
	use_sendfile = FALSE; 
#endif /* USE_MMAP || HAVE_SENDFILE */
	
	if (use_sendfile) {
		off_t pos, before;			/**< For sendfile() sanity checks */
		/*
	 	 * Compute the amount of bytes to send.
	 	 * Use the two variables to avoid warnings about unused vars by
		 * compiler.
	 	 */

		available = MIN(amount, READ_BUF_SIZE);
		before = pos = u->pos;
		written = bio_sendfile(&u->sendfile_ctx, u->bio, u->file_desc,
					&pos, available);

		g_assert((ssize_t) -1 == written || (off_t) written == pos - before);
		u->pos = pos;

	} else {
		/*
		 * If sendfile() failed on a different connection meanwhile
		 * u->buffer is still NULL for this connection.
		 */
		if (sendfile_failed && NULL == u->buffer) {
			u->buf_size = READ_BUF_SIZE;
			u->buffer = (gchar *) g_malloc(u->buf_size);
		}
		
		/*
	 	 * If the buffer position reached the size, then we need to read
	 	 * more data from the file.
	 	 */

		if (u->bpos == u->bsize) {
			ssize_t ret;

			g_assert(u->buffer != NULL);
			g_assert(u->buf_size > 0);
			u->bsize = ret = read(u->file_desc, u->buffer, u->buf_size);
			if ((ssize_t) -1 == ret) {
				upload_remove(u, _("File read error: %s"), g_strerror(errno));
				return;
			}
			if (0 == ret) {
				upload_remove(u, _("File EOF?"));
				return;
			}
			u->bpos = 0;
		}

		available = u->bsize - u->bpos;
		if (available > amount)
			available = amount;

		g_assert(available > 0 && available <= INT_MAX);

		written = bio_write(u->bio, &u->buffer[u->bpos], available);
	}

	if ((ssize_t) -1 == written) {
		gint e = errno;

		if (
			use_sendfile &&
			e != EINTR &&
			e != EAGAIN &&
			e != EPIPE &&
			e != ECONNRESET &&
			e != ENOTCONN &&
			e != ENOBUFS
		) {
			g_warning("sendfile() failed: \"%s\"\n"
				"Disabling sendfile() for this session", g_strerror(e));
			sendfile_failed = TRUE;
		}
		if (e != EAGAIN && e != EINTR) {
			socket_eof(u->socket);
			upload_remove(u, _("Data write error: %s"), g_strerror(e));
		}
		return;
	} else if (written == 0) {
		upload_remove(u, _("No bytes written, source may be gone"));
		return;
	}

	if (!use_sendfile) {
		/*
	 	 * Only required when not using sendfile(), otherwise the u->pos field
	 	 * is directly updated by the kernel, and u->bpos is unused.
	 	 *		--RAM, 21/02/2002
	 	 */

		u->pos += written;
		u->bpos += written;
	}

	gnet_prop_set_guint64_val(PROP_UL_BYTE_COUNT, ul_byte_count + written);

	u->last_update = time((time_t *) NULL);
	u->sent += written;

	/* This upload is complete */
	if (u->pos > u->end) {
		
		upload_stats_file_complete(u);
		u->accounted = TRUE;		/* Called upload_stats_file_complete() */
		upload_completed(u);
		
		return;
	}
}

static inline ssize_t
upload_special_read(gnutella_upload_t *u)
{
	g_assert(NULL != u->special);
	g_assert(NULL != u->special->read);

	return u->special->read(u->special, u->buffer, u->buf_size);
}

static inline ssize_t
upload_special_write(gnutella_upload_t *u, gpointer data, size_t len)
{
	g_assert(NULL != u->special);
	g_assert(NULL != u->special->write);

	return u->special->write(u->special, data, len);
}

/**
 * Callback invoked when the special stack has been fully flushed.
 */
static void
upload_special_flushed(gpointer arg)
{
	gnutella_upload_t *u = arg;

	upload_completed(u);	/* We're done, wait for next request if any */
}

static inline void
upload_special_flush(gnutella_upload_t *u)
{
	g_assert(NULL != u->special);
	g_assert(NULL != u->special->flush);

	u->special->flush(u->special, upload_special_flushed, u);
}

/**
 * Called when output source can accept more data.
 */
static void
upload_special_writable(gpointer up)
{
	gnutella_upload_t *u = up;
	ssize_t written;
	size_t available;

	g_assert(NULL != u->special);

	/*
 	 * If the buffer position reached the size, then we need to read
 	 * more data from the file.
 	 */

	if (u->bpos == u->bsize) {
		ssize_t ret;

		g_assert(u->buffer != NULL);
		g_assert(u->buf_size > 0);
		ret = u->bsize = upload_special_read(u);
		if ((ssize_t) -1 == ret) {
			upload_remove(u, _("Special read error: %s"), g_strerror(errno));
			return;
		}
		if (0 == ret) {
			/*
			 * We're done.  Flush the stack asynchronously.
			 */

			upload_special_flush(u);
			return;
		}
		u->bpos = 0;
	}

	available = u->bsize - u->bpos;
	g_assert(available > 0 && available <= INT_MAX);

	written = upload_special_write(u, &u->buffer[u->bpos], available);

	if ((ssize_t) -1 == written)
		return;		/* TX stack already removed the upload */

	u->pos += written;
	u->bpos += written;

	gnet_prop_set_guint64_val(PROP_UL_BYTE_COUNT, ul_byte_count + written);

	u->last_update = time(NULL);
	u->sent += written;
}
	
/**
 * Kill a running upload.
 */
void
upload_kill(gnet_upload_t upload)
{
    gnutella_upload_t *u = upload_find_by_handle(upload);

    g_assert(u != NULL);

    if (!UPLOAD_IS_COMPLETE(u)) {
		parq_upload_force_remove(u);
        upload_remove(u, _("Explicitly killed"));
	}
}

/**
 * Kill all running uploads by IP.
 */
void
upload_kill_ip(guint32 ip)
{
	GSList *sl, *to_remove = NULL;

	for (sl = list_uploads; sl; sl = g_slist_next(sl)) {
		gnutella_upload_t *u = (gnutella_upload_t *) (sl->data);

		g_assert(u != NULL);

		if (u->ip == ip && !UPLOAD_IS_COMPLETE(u))
			to_remove = g_slist_prepend(to_remove, u);
	}

	for (sl = to_remove; sl; sl = g_slist_next(sl)) {
		gnutella_upload_t *u = (gnutella_upload_t *) (sl->data);

		parq_upload_force_remove(u);
		upload_remove(u, _("IP denying uploads"));
	}
	g_slist_free(to_remove);
}

/**
 * Check whether uploading is enabled: we have slots, and bandwidth.
 */
gboolean
upload_is_enabled(void)
{
	if (max_uploads == 0)
		return FALSE;
	if (bsched_bwps(bws.out) < BW_OUT_MIN)
		return FALSE;

	return TRUE;
}

/**
 * Initialize uploads.
 */
void upload_init(void)
{
	mesh_info = g_hash_table_new(mi_key_hash, mi_key_eq);
	stalling_uploads = aging_make(STALL_CLEAR,
							NULL, NULL, NULL, NULL, NULL, NULL);
    upload_handle_map = idtable_new(32, 32);
}

void
upload_close(void)
{
	GSList *sl, *to_remove = NULL;

	for (sl = list_uploads; sl; sl = g_slist_next(sl))
		to_remove = g_slist_prepend(to_remove, sl->data);

	for (sl = to_remove; sl; sl = g_slist_next(sl)) {
		gnutella_upload_t *u = sl->data;

		if (UPLOAD_IS_SENDING(u) && !u->browse_host && !u->accounted)
			upload_stats_file_aborted(u);
		upload_free_resources(u);
		wfree(u, sizeof *u);
	}
	g_slist_free(to_remove);

    idtable_destroy(upload_handle_map);
    upload_handle_map = NULL;

	g_slist_free(list_uploads);
	list_uploads = NULL;

	g_hash_table_foreach(mesh_info, mi_free_kv, NULL);
	g_hash_table_destroy(mesh_info);

	aging_destroy(stalling_uploads);
}

gnet_upload_info_t *
upload_get_info(gnet_upload_t uh)
{
    gnutella_upload_t *u = upload_find_by_handle(uh);
    gnet_upload_info_t *info;

    info = walloc(sizeof(*info));

    info->name          = u->name ? atom_str_get(u->name) : NULL;
    info->ip            = u->ip;
    info->file_size     = u->file_size;
    info->range_start   = u->skip;
    info->range_end     = u->end;
    info->start_date    = u->start_date;
    info->user_agent    = u->user_agent ? atom_str_get(u->user_agent) : NULL;
    info->country       = u->country;
    info->upload_handle = u->upload_handle;
	info->push          = u->push;
	info->partial       = u->file_info != NULL;

    return info;
}

void
upload_free_info(gnet_upload_info_t *info)
{
    g_assert(info != NULL);

	if (info->user_agent)
		atom_str_free(info->user_agent);
	if (info->name)
		atom_str_free(info->name);

    wfree(info, sizeof(*info));
}

void
upload_get_status(gnet_upload_t uh, gnet_upload_status_t *si)
{
    gnutella_upload_t *u = upload_find_by_handle(uh);
	time_t now = time((time_t *) NULL);
    g_assert(si != NULL);

    si->status      = u->status;
    si->pos         = u->pos;
    si->bps         = 1;
    si->avg_bps     = 1;
    si->last_update = u->last_update;

	si->parq_queue_no = parq_upload_lookup_queue_no(u);
	si->parq_position = parq_upload_lookup_position(u);
	si->parq_size = parq_upload_lookup_size(u);
	si->parq_lifetime = MAX(0, delta_time(parq_upload_lookup_lifetime(u), now));
	si->parq_retry = MAX(0, delta_time(parq_upload_lookup_retry(u), now));

    if (u->bio) {
        si->bps = bio_bps(u->bio);
		si->avg_bps = bio_avg_bps(u->bio);
	}

    if (si->avg_bps <= 10 && u->last_update != u->start_date)
        si->avg_bps = (u->pos - u->skip)
			/ delta_time(u->last_update, u->start_date);
	if (si->avg_bps == 0)
        si->avg_bps++;
}

/* vi: set ts=4 sw=4 cindent: */
