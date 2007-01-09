/*
 * $Id$
 *
 * Copyright (c) 2002-2003, Ch. Tronche & Raphael Manfredi
 *
 * Started by Ch. Tronche (http://tronche.com/) 28/04/2002
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
 * HUGE support (Hash/URN Gnutella Extension).
 *
 * @author Raphael Manfredi
 * @date 2002-2003
 * @author Ch. Tronche (http://tronche.com/)
 * @date 2002-04-28
 */

#include "common.h"

RCSID("$Id$")

#include "huge.h"
#include "share.h"
#include "gmsg.h"
#include "dmesh.h"
#include "version.h"
#include "settings.h"
#include "spam.h"

#include "lib/atoms.h"
#include "lib/base32.h"
#include "lib/bg.h"
#include "lib/file.h"
#include "lib/header.h"
#include "lib/sha1.h"
#include "lib/tm.h"
#include "lib/urn.h"
#include "lib/walloc.h"

#include "if/gnet_property.h"
#include "if/gnet_property_priv.h"

#include "lib/override.h"		/* Must be the last header included */

/***
 *** Server side: computation of SHA1 hash digests and replies.
 *** SHA1 is defined in RFC 3174.
 ***/

/**
 * There's an in-core cache (the GHashTable sha1_cache), and a
 * persistent copy (normally in ~/.gtk-gnutella/sha1_cache). The
 * in-core cache is filled with the persistent one at launch. When the
 * "shared_file" (the records describing the shared files, see
 * share.h) are created, a call is made to sha1_set_digest to fill the
 * SHA1 digest part of the shared_file. If the digest isn't found in
 * the in-core cache, it's computed, stored in the in-core cache and
 * appended at the end of the persistent cache. If the digest is found
 * in the cache, a check is made based on the file size and last
 * modification time. If they're identical to the ones in the cache,
 * the digest is considered to be accurate, and is used. If the file
 * size or last modification time don't match, the digest is computed
 * again and stored in the in-core cache, but it isn't stored in the
 * persistent one. Instead, the cache is marked as dirty, and will be
 * entirely overwritten by dump_cache, called when everything has been
 * computed.
 */

struct sha1_cache_entry {
    const gchar *file_name;			/**< Full path name (atom)          */
	const gchar *digest;			/**< SHA-1 (binary; atom)			*/
    filesize_t  size;				/**< File size                      */
    time_t mtime;					/**< Last modification time         */
    gboolean shared;				/**< There's a known entry for this
                                         file in the share library      */
};

static GHashTable *sha1_cache = NULL;

/**
 * cache_dirty means that in-core cache is different from the one on disk when
 * TRUE.
 */
static gboolean cache_dirty = FALSE;

/**
 ** Elementary operations on SHA1 values
 **/

/**
 ** Handling of persistent buffer
 **/

/* In-memory cache */

/**
 * Takes an in-memory cached entry, and update its content.
 */
static void update_volatile_cache(
	struct sha1_cache_entry *sha1_cached_entry,
	filesize_t size,
	time_t mtime,
	const char *digest)
{
	sha1_cached_entry->shared = TRUE;
	sha1_cached_entry->size = size;
	sha1_cached_entry->mtime = mtime;
	if (sha1_cached_entry->digest) {
		atom_sha1_free(sha1_cached_entry->digest);
	}	
	sha1_cached_entry->digest = atom_sha1_get(digest);
}

/**
 * Add a new entry to the in-memory cache.
 */
static void
add_volatile_cache_entry(const char *filename, filesize_t size, time_t mtime,
	const char *digest, gboolean known_to_be_shared)
{
	struct sha1_cache_entry *item;
   
	item = walloc(sizeof *item);
	item->file_name = atom_str_get(filename);
	item->size = size;
	item->mtime = mtime;
	item->digest = atom_sha1_get(digest);
	item->shared = known_to_be_shared;
	g_hash_table_insert(sha1_cache, deconstify_gchar(item->file_name), item);
}

/** Disk cache */

static const char sha1_persistent_cache_file_header[] =
"#\n"
"# gtk-gnutella SHA1 cache file.\n"
"# This file is automatically generated.\n"
"# Format is: SHA1 digest<TAB>file_size<TAB>file_mtime<TAB>file_name\n"
"# Comment lines start with a sharp (#)\n"
"#\n"
"\n";

static char *persistent_cache_file_name = NULL;

static void
cache_entry_print(FILE *f, const char *filename, const gchar *digest,
	filesize_t size, time_t mtime)
{
	const gchar *sha1;
	gchar size_buf[UINT64_DEC_BUFLEN], mtime_buf[UINT64_DEC_BUFLEN];

	uint64_to_string_buf(size, size_buf, sizeof size_buf);
	uint64_to_string_buf(mtime, mtime_buf, sizeof mtime_buf);

	sha1 = sha1_base32(digest);
	g_assert(NULL != sha1);

	fprintf(f, "%s\t%s\t%s\t%s\n", sha1, size_buf, mtime_buf, filename);
}

/**
 * Add an entry to the persistent cache.
 */
static void
add_persistent_cache_entry(const char *filename, filesize_t size,
	time_t mtime, const char *digest)
{
	FILE *f;
	struct stat sb;

	g_return_if_fail(NULL != persistent_cache_file_name);

	if (NULL == (f = file_fopen(persistent_cache_file_name, "a"))) {
		g_warning("add_persistent_cache_entry: could not open \"%s\"",
			persistent_cache_file_name);
		return;
	}

	/*
	 * If we're adding the very first entry (file empty), then emit header.
	 */

	if (fstat(fileno(f), &sb)) {
		g_warning("add_persistent_cache_entry: could not stat \"%s\"",
			persistent_cache_file_name);
		return;
	}

	if (0 == sb.st_size)
		fputs(sha1_persistent_cache_file_header, f);

	cache_entry_print(f, filename, digest, size, mtime);
	fclose(f);
}

/**
 * Dump one (in-memory) cache into the persistent cache. This is a callback
 * called by dump_cache to dump the whole in-memory cache onto disk.
 */
static void
dump_cache_one_entry(gpointer unused_key, gpointer value, gpointer udata)
{
	struct sha1_cache_entry *e = value;
	FILE *f = udata;

	(void) unused_key;

	if (e->shared)
		cache_entry_print(f, e->file_name, e->digest, e->size, e->mtime);
}

/**
 * Dump the whole in-memory cache onto disk.
 */
static void
dump_cache(void)
{
	FILE *f;

	if (NULL == (f = file_fopen(persistent_cache_file_name, "w"))) {
		g_warning("dump_cache: could not open \"%s\"",
			persistent_cache_file_name);
		return;
	}

	fputs(sha1_persistent_cache_file_header, f);
	g_hash_table_foreach(sha1_cache, dump_cache_one_entry, f);
	fclose(f);

	cache_dirty = FALSE;
}

/**
 * This function is used to read the disk cache into memory.
 *
 * It must be passed one line from the cache (ending with '\n'). It
 * performs all the syntactic processing to extract the fields from
 * the line and calls add_volatile_cache_entry() to append the record
 * to the in-memory cache.
 */
static void
parse_and_append_cache_entry(char *line)
{
	const char *sha1_digest_ascii;
	const char *file_name;
	char *file_name_end;
	const char *p, *end; /* pointers to scan the line */
	gint c, error;
	filesize_t size;
	time_t mtime;
	char digest[SHA1_RAW_SIZE];

	/* Skip comments and blank lines */
	c = line[0];
	if (c == '\0' || c == '#' || c == '\n')
		return;

	sha1_digest_ascii = line; /* SHA1 digest is the first field. */

	/* Scan until file size */

	p = line;
	while ((c = *p) != '\0' && c != '\t' && c != '\n')
		p++;

	if (
		*p != '\t' ||
		(p - sha1_digest_ascii) != SHA1_BASE32_SIZE ||
		!base32_decode_into(sha1_digest_ascii, SHA1_BASE32_SIZE,
			digest, sizeof(digest))
	) {
		g_warning("Malformed line in SHA1 cache file %s[SHA1]: %s",
			persistent_cache_file_name, line);
		return;
	}

	p++; /* Skip \t */

	/* p is now supposed to point to the beginning of the file size */

	size = parse_uint64(p, &end, 10, &error);
	if (error || *end != '\t') {
		g_warning("Malformed line in SHA1 cache file %s[size]: %s",
			persistent_cache_file_name, line);
		return;
	}

	p = ++end;

	/*
	 * p is now supposed to point to the beginning of the file last
	 * modification time.
	 */

	mtime = parse_uint64(p, &end, 10, &error);
	if (error || *end != '\t') {
		g_warning("Malformed line in SHA1 cache file %s[mtime]: %s",
			persistent_cache_file_name, line);
		return;
	}

	p = ++end;

	/* p is now supposed to point to the file name */

	file_name = p;
	file_name_end = strchr(file_name, '\n');

	if (!file_name_end) {
		g_warning("Malformed line in SHA1 cache file %s[file_name]: %s",
			persistent_cache_file_name, line);
		return;
	}

	/* Set string end markers */
	*file_name_end = '\0';

	add_volatile_cache_entry(file_name, size, mtime, digest, FALSE);
}

/**
 * Read the whole persistent cache into memory.
 */
static void
sha1_read_cache(void)
{
	FILE *f;
	gboolean truncated = FALSE;

	if (!settings_config_dir()) {
		g_warning("sha1_read_cache: No config dir");
		return;
	}

	persistent_cache_file_name = make_pathname(settings_config_dir(),
									"sha1_cache");

	if (NULL == (f = file_fopen(persistent_cache_file_name, "r"))) {
		cache_dirty = TRUE;
		return;
	}

	for (;;) {
		char buffer[4096];

		if (NULL == fgets(buffer, sizeof buffer, f))
			break;

		if (NULL == strchr(buffer, '\n')) {
			truncated = TRUE;
		} else if (truncated) {
			truncated = FALSE;
		} else {
			parse_and_append_cache_entry(buffer);
		}
	}

	fclose(f);
}

/**
 ** Asynchronous computation of hash value
 **/

#define HASH_BLOCK_SHIFT	12			/**< Power of two of hash unit credit */
#define HASH_BUF_SIZE		65536		/**< Size of a the reading buffer */

static gpointer sha1_task = NULL;

/* Two useful lists */

/**
 * When a hash is requested for a file and is unknown, it is first stored onto
 * this stack, waiting to be processed.
 */

static GSList *waiting_for_sha1_computation = NULL;

/* The context of the SHA1 computation being performed */

enum sha1_computation_magic_t {
	SHA1_MAGIC = 0x0886a7ddU
};

struct sha1_computation_context {
	enum sha1_computation_magic_t magic;
	SHA1Context context;
	shared_file_t *sf;
	gpointer buffer;			/**< Large buffer where data is read */
	gint fd;
	time_t start;				/**< Debugging, show computation rate */
};

static void
sha1_computation_context_free(gpointer u)
{
	struct sha1_computation_context *ctx = u;

	g_assert(ctx->magic == SHA1_MAGIC);

	if (ctx->fd != -1) {
		close(ctx->fd);
		ctx->fd = -1;
	}
	shared_file_unref(&ctx->sf);
	G_FREE_NULL(ctx->buffer);
	wfree(ctx, sizeof *ctx);
}

/**
 * When SHA1 is computed, and we know what struct share it's related
 * to, we call this function to update set the share SHA1 value.
 *
 * @return FALSE if the pointer "sf" is not valid any longer; TRUE otherwise.
 */
static gboolean
put_sha1_back_into_share_library(struct shared_file *sf, const gchar *digest)
{
	struct sha1_cache_entry *cached_sha1;
	struct stat sb;

	shared_file_check(sf);

	/*
	 * Make sure the file's timestamp is still accurate.
	 */

	if (-1 == stat(shared_file_path(sf), &sb)) {
		g_warning("discarding SHA1 for file \"%s\": can't stat(): %s",
			shared_file_path(sf), g_strerror(errno));
		shared_file_remove(sf);
		return TRUE;
	}

	if (sb.st_mtime != shared_file_modification_time(sf)) {
		g_warning("file \"%s\" was modified whilst SHA1 was computed",
			shared_file_path(sf));
		shared_file_set_modification_time(sf, sb.st_mtime);
		return request_sha1(sf);					/* Retry! */
	}

	if (spam_check_sha1(digest)) {
		g_warning("file \"%s\" is listed as spam", shared_file_path(sf));
		shared_file_remove(sf);
		return FALSE;
	}

	shared_file_set_sha1(sf, digest);

	/* Update cache */

	cached_sha1 = g_hash_table_lookup(sha1_cache,
					cast_to_gconstpointer(shared_file_path(sf)));

	if (cached_sha1) {
		update_volatile_cache(cached_sha1, shared_file_size(sf),
			shared_file_modification_time(sf), digest);
		cache_dirty = TRUE;
	} else {
		add_volatile_cache_entry(shared_file_path(sf),
			shared_file_size(sf), shared_file_modification_time(sf),
			digest, TRUE);
		add_persistent_cache_entry(shared_file_path(sf),
			shared_file_size(sf), shared_file_modification_time(sf), digest);
	}

	return TRUE;
}

/**
 * Close the file whose hash we're computing (after calculation completed) and
 * free the associated structure.
 */
static void
close_current_file(struct sha1_computation_context *ctx)
{
	if (ctx->fd != -1) {
		if (dbg > 1) {
			time_delta_t delta = delta_time(tm_time(), ctx->start);

			if (delta) {
				g_message("SHA1 computation rate: %s bytes/sec\n",
					uint64_to_string(shared_file_size(ctx->sf) / delta));
			}
		}
		close(ctx->fd);
		ctx->fd = -1;
	}
	shared_file_unref(&ctx->sf);
}

/**
 * Get the next file waiting for its hash to be computed from the queue
 * (actually a stack).
 *
 * @return this file.
 */
static shared_file_t *
get_next_file_from_list(void)
{
	GSList *sl;

	/*
	 * XXX HACK ALERT
	 *
	 * We need to be careful here, because each time the library is rescanned,
	 * we add file to the list of SHA1 to recompute if we don't have them
	 * yet.  This means that when we rescan the library during a computation,
	 * we'll add duplicates to our working queue.
	 *
	 * Fortunately, we can probe our in-core cache to see if what we have
	 * is already up-to-date.
	 *
	 * XXX It would be best to maintain a hash table of all the filenames
	 * XXX in our workqueue and not enqueue the work in the first place.
	 * XXX		--RAM, 21/05/2002
	 */

	while (NULL != (sl = waiting_for_sha1_computation)) {
		shared_file_t *sf = sl->data;
		struct sha1_cache_entry *cached;

		shared_file_check(sf);
		waiting_for_sha1_computation = g_slist_delete_link(sl, sl);
		cached = g_hash_table_lookup(sha1_cache, shared_file_path(sf));

		if (cached) {
			struct stat sb;

			if (-1 == stat(shared_file_path(sf), &sb)) {
				g_warning("ignoring SHA1 recomputation request for \"%s\": %s",
					shared_file_path(sf), g_strerror(errno));
				shared_file_unref(&sf);
				continue;
			}

			if (
				cached->size == (filesize_t) sb.st_size &&
				cached->mtime == sb.st_mtime
			) {
				if (dbg > 1)
					g_warning("ignoring duplicate SHA1 work for \"%s\"",
						shared_file_path(sf));
				shared_file_unref(&sf);
				continue;
			}
		}
		return sf;
	}
	return NULL;
}

/**
 * Open the next file waiting for its hash to be computed.
 *
 * @return TRUE if open succeeded, FALSE otherwise.
 */
static gboolean
open_next_file(struct sha1_computation_context *ctx)
{
	ctx->sf = get_next_file_from_list();

	if (!ctx->sf)
		return FALSE;			/* No more file to process */

	if (dbg > 1) {
		g_message("Computing SHA1 digest for %s\n", shared_file_path(ctx->sf));
		ctx->start = tm_time();
	}

	ctx->fd = file_open(shared_file_path(ctx->sf), O_RDONLY);

	if (ctx->fd < 0) {
		g_warning("Unable to open \"%s\" for computing SHA1 hash",
			shared_file_path(ctx->sf));
		shared_file_remove(ctx->sf);
		close_current_file(ctx);
		return FALSE;
	}

	SHA1Reset(&ctx->context);

	return TRUE;
}

/**
 * Callback to be called when a computation has completed.
 */
static void
got_sha1_result(struct sha1_computation_context *ctx, const gchar *digest)
{
	g_assert(ctx->magic == SHA1_MAGIC);
	shared_file_check(ctx->sf);

	put_sha1_back_into_share_library(ctx->sf, digest);
}

/**
 * The timer calls repeatedly this function, consuming one unit of
 * credit every call.
 */
static void
sha1_timer_one_step(struct sha1_computation_context *ctx,
	gint ticks, gint *used)
{
	ssize_t r;
	size_t amount;

	if (!ctx->sf && !open_next_file(ctx)) {
		*used = 1;
		return;
	}

	shared_file_check(ctx->sf);

	/*
	 * Each tick we have can buy us 2^HASH_BLOCK_SHIFT bytes.
	 * We read into a HASH_BUF_SIZE bytes buffer.
	 */

	amount = ticks << HASH_BLOCK_SHIFT;
	amount = MIN(amount, HASH_BUF_SIZE);

	r = read(ctx->fd, ctx->buffer, amount);

	if ((ssize_t) -1 == r) {
		g_warning("Error while reading %s for computing SHA1 hash: %s\n",
			shared_file_path(ctx->sf), g_strerror(errno));
		shared_file_remove(ctx->sf);
		close_current_file(ctx);
		*used = 1;
		return;
	}

	/*
	 * Any partially read block counts as one block, hence the second term.
	 */

	*used = (r >> HASH_BLOCK_SHIFT) +
		((r & ((1 << HASH_BLOCK_SHIFT) - 1)) ? 1 : 0);

	if (r > 0) {
		int res;

		res = SHA1Input(&ctx->context, (const guint8 *) ctx->buffer, r);
		if (res != shaSuccess) {
			g_warning("SHA1 error while computing hash for %s\n",
				shared_file_path(ctx->sf));
			close_current_file(ctx);
			return;
		}
	}

	if ((size_t) r < amount) {					/* EOF reached */
		guint8 digest[SHA1HashSize];
		SHA1Result(&ctx->context, digest);
		got_sha1_result(ctx, (const gchar *) digest);
		close_current_file(ctx);
	}
}

/**
 * The routine doing all the work.
 */
static bgret_t
sha1_step_compute(gpointer h, gpointer u, gint ticks)
{
	struct sha1_computation_context *ctx = u;
	gint credit = ticks;

	g_assert(ctx->magic == SHA1_MAGIC);

	if (dbg > 4)
		g_message("sha1_step_compute: ticks = %d", ticks);

	while (credit > 0 && (ctx->sf || waiting_for_sha1_computation)) {
		gint used;
		sha1_timer_one_step(ctx, credit, &used);
		credit -= used;
	}

	/*
	 * If we didn't use all our credit, tell the background task scheduler.
	 */

	if (credit > 0)
		bg_task_ticks_used(h, ticks - credit);

	if (dbg > 4)
		g_message("sha1_step_compute: file=0x%lx wait_comp=0x%lx",
			(gulong) ctx->sf, (gulong) waiting_for_sha1_computation);

	if (!ctx->sf && !waiting_for_sha1_computation) {
		if (dbg > 1)
			g_message("sha1_step_compute: was last call for now");
		sha1_task = NULL;
		gnet_prop_set_boolean_val(PROP_SHA1_REBUILDING, FALSE);
		return BGR_NEXT;
	}

	return BGR_MORE;
}

/**
 * Dump SHA1 cache if it is dirty.
 */
static bgret_t
sha1_step_dump(gpointer unused_h, gpointer unused_u, gint unused_ticks)
{
	(void) unused_h;
	(void) unused_u;
	(void) unused_ticks;

	if (cache_dirty)
		dump_cache();

	return BGR_DONE;			/* Finished */
}

/**
 ** External interface
 **/

/* This is the external interface. During the share library building,
 * computation of SHA1 values for shared_file is repeatedly requested
 * through sha1_set_digest. If the value is found in the cache (and
 * the cache is up to date), it's set immediately. Otherwise, the file
 * is put in a queue for it's SHA1 digest to be computed.
 */

/**
 * Put the shared file on the stack of the things to do. Activate the timer if
 * this wasn't done already.
 */
static void
queue_shared_file_for_sha1_computation(shared_file_t *sf)
{
 	shared_file_check(sf);

	waiting_for_sha1_computation =
		g_slist_prepend(waiting_for_sha1_computation, shared_file_ref(sf));

	if (sha1_task == NULL) {
		static const struct sha1_computation_context zero_ctx;
		struct sha1_computation_context *ctx;
		bgstep_cb_t steps[] = {
			sha1_step_compute,
			sha1_step_dump,
		};

		ctx = walloc(sizeof *ctx);
		*ctx = zero_ctx;
		ctx->magic = SHA1_MAGIC;
		ctx->fd = -1;
		ctx->buffer = g_malloc(HASH_BUF_SIZE);

		sha1_task = bg_task_create("SHA1 computation",
			steps, 2,  ctx, sha1_computation_context_free, NULL, NULL);

		gnet_prop_set_boolean_val(PROP_SHA1_REBUILDING, TRUE);
	}
}

/**
 * Check to see if an (in-memory) entry cache is up to date.
 *
 * @return true (in the C sense) if it is, or false otherwise.
 */
static gboolean
cached_entry_up_to_date(const struct sha1_cache_entry *cache_entry,
	const struct shared_file *sf)
{
	return cache_entry->size == shared_file_size(sf)
		&& cache_entry->mtime == shared_file_modification_time(sf);
}

/**
 * External interface to check whether the sha1 for shared_file is known.
 */
gboolean
sha1_is_cached(const struct shared_file *sf)
{
	const struct sha1_cache_entry *cached_sha1;

	cached_sha1 = g_hash_table_lookup(sha1_cache, shared_file_path(sf));
	return cached_sha1 && cached_entry_up_to_date(cached_sha1, sf);
}


/**
 * External interface to call for getting the hash for a shared_file.
 *
 * @return if shared_file_remove() was called, FALSE is returned and
 *         "sf" is no longer valid. Otherwise TRUE is returned.
 */
gboolean
request_sha1(struct shared_file *sf)
{
	struct sha1_cache_entry *cached_sha1;

	shared_file_check(sf);

	cached_sha1 = g_hash_table_lookup(sha1_cache, shared_file_path(sf));
	if (cached_sha1 && cached_entry_up_to_date(cached_sha1, sf)) {
		if (spam_check_sha1(cached_sha1->digest)) {
			g_warning("file \"%s\" is listed as spam", shared_file_path(sf));
			shared_file_remove(sf);
			return FALSE;
		}

		shared_file_set_sha1(sf, cached_sha1->digest);
		cached_sha1->shared = TRUE;
		return TRUE;
	}

	if (dbg > 4) {
		if (cached_sha1)
			g_message(
				"Cached SHA1 entry for \"%s\" outdated: had mtime %lu, now %lu",
				shared_file_path(sf),
				(gulong) cached_sha1->mtime,
				(gulong) shared_file_modification_time(sf));
		else
			g_message("Queuing \"%s\" for SHA1 computation",
				shared_file_path(sf));
	}

	queue_shared_file_for_sha1_computation(sf);
	return TRUE;
}

/**
 ** Init
 **/

/**
 * Initialize SHA1 module.
 */
void
huge_init(void)
{
	sha1_cache = g_hash_table_new(pointer_hash_func, NULL);
	sha1_read_cache();
}

/**
 * Free SHA1 cache entry.
 */
static gboolean
cache_free_entry(gpointer unused_key, gpointer v, gpointer unused_udata)
{
	struct sha1_cache_entry *e = v;

	(void) unused_key;
	(void) unused_udata;

	atom_str_free(e->file_name);
	e->file_name = NULL;
	atom_sha1_free(e->digest);
	e->digest = NULL;
	wfree(e, sizeof *e);

	return TRUE;
}

/**
 * Called when servent is shutdown.
 */
void
huge_close(void)
{
	GSList *sl;

	if (sha1_task)
		bg_task_cancel(sha1_task);

	if (cache_dirty)
		dump_cache();

	G_FREE_NULL(persistent_cache_file_name);

	g_hash_table_foreach_remove(sha1_cache, cache_free_entry, NULL);
	g_hash_table_destroy(sha1_cache);

	while (NULL != (sl = waiting_for_sha1_computation)) {
		shared_file_t *sf = sl->data;

		shared_file_check(sf);
		waiting_for_sha1_computation = g_slist_delete_link(sl, sl);
		shared_file_unref(&sf);
	}
}

/**
 * Test whether the SHA1 in its base32/binary form is improbable.
 *
 * This is used to detect "urn:sha1:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" and
 * things using the same pattern with other letters, as being rather
 * improbable hashes.
 */
gboolean
huge_improbable_sha1(const gchar *buf, size_t len)
{
	size_t ilen = 0;			/* Length of the improbable sequence */
	size_t i, longest = 0;

	for (i = 1; i < len; i++) {
		guchar previous, c;
		
		previous = buf[i - 1];
		c = buf[i];

		if (c == previous || (c + 1 == previous) || (c - 1 == previous)) {
			ilen++;
		} else {
			longest = MAX(longest, ilen);
			ilen = 0;		/* Reset sequence, we broke out of the pattern */
		}
	}

	return (longest >= len / 2) ? TRUE : FALSE;
}

/**
 * Validate `len' bytes starting from `buf' as a proper base32 encoding
 * of a SHA1 hash, and write decoded value in `retval'.
 * Also make sure that the SHA1 is not an improbable value.
 *
 * `header' is the header of the packet where we found the SHA1, so that we
 * may trace errors if needed.
 *
 * When `check_old' is true, check the encoding against an earlier version
 * of the base32 alphabet.
 *
 * @return TRUE if the SHA1 was valid and properly decoded, FALSE on error.
 */
gboolean
huge_sha1_extract32(const gchar *buf, size_t len, gchar *retval,
	gpointer header, gboolean check_old)
{
	if (len != SHA1_BASE32_SIZE || huge_improbable_sha1(buf, len))
		goto bad;

	if (base32_decode_into(buf, len, retval, SHA1_RAW_SIZE))
		goto ok;

	if (!check_old) {
		if (dbg) {
			if (base32_decode_old_into(buf, len, retval, SHA1_RAW_SIZE))
				g_warning("%s old SHA1 ignored: %32s",
					gmsg_infostr(header), buf);
			else
				goto bad;
		}
		return FALSE;
	}

	if (!base32_decode_old_into(buf, len, retval, SHA1_RAW_SIZE))
		goto bad;

	if (dbg)
		g_warning("%s old SHA1: %32s", gmsg_infostr(header), buf);

ok:
	/*
	 * Make sure the decoded value in `retval' is "valid".
	 */

	if (huge_improbable_sha1(retval, SHA1_RAW_SIZE)) {
		if (dbg) {
			if (is_printable(buf, len)) {
				g_warning("%s has bad SHA1 (len=%d): %.*s, hex: %s",
					gmsg_infostr(header), (gint) len, (gint) len, buf,
					data_hex_str(retval, SHA1_RAW_SIZE));
			} else
				goto bad;		/* SHA1 should be printable originally */
		}
		return FALSE;
	}

	return TRUE;

bad:
	if (dbg) {
		if (is_printable(buf, len)) {
			g_warning("%s has bad SHA1 (len=%d): %.*s",
				gmsg_infostr(header), (gint) len, (gint) len, buf);
		} else {
			g_warning("%s has bad SHA1 (len=%d)",
					gmsg_infostr(header), (gint) len);
			if (len)
				dump_hex(stderr, "Base32 SHA1", buf, len);
		}
	}

	return FALSE;
}

/**
 * Parse the "X-Gnutella-Alternate-Location" header if present to learn
 * about other sources for this file.
 */
void
huge_collect_locations(const gchar *sha1, header_t *header)
{
	gchar *alt;
   

	g_return_if_fail(sha1);
	g_return_if_fail(header);

	alt = header_get(header, "X-Gnutella-Alternate-Location");

	/*
	 * Unfortunately, clueless people broke the HUGE specs and made up their
	 * own headers.  They should learn about header continuations, and
	 * that "X-Gnutella-Alternate-Location" does not need to be repeated.
	 */

	if (alt == NULL)
		alt = header_get(header, "Alternate-Location");
	if (alt == NULL)
		alt = header_get(header, "Alt-Location");

	if (alt) {
		dmesh_collect_locations(sha1, alt, TRUE);
		return;
	}

	alt = header_get(header, "X-Alt");

	if (alt) {
		dmesh_collect_compact_locations(sha1, alt);
    }
}

/*
 * Emacs stuff:
 * Local Variables: ***
 * c-indentation-style: "bsd" ***
 * fill-column: 80 ***
 * tab-width: 4 ***
 * indent-tabs-mode: nil ***
 * End: ***
 * vi: set ts=4 sw=4 cindent:
 */
