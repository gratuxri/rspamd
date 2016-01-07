/*
 * Copyright (c) 2015, Vsevolod Stakhov
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *	 * Redistributions of source code must retain the above copyright
 *	   notice, this list of conditions and the following disclaimer.
 *	 * Redistributions in binary form must reproduce the above copyright
 *	   notice, this list of conditions and the following disclaimer in the
 *	   documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHOR ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "stat_api.h"
#include "rspamd.h"
#include "cfg_rcl.h"
#include "stat_internal.h"

static struct rspamd_stat_ctx *stat_ctx = NULL;

static struct rspamd_stat_classifier stat_classifiers[] = {
	{
		.name = "bayes",
		.init_func = bayes_init,
		.classify_func = bayes_classify,
		.learn_spam_func = bayes_learn_spam,
	}
};

static struct rspamd_stat_tokenizer stat_tokenizers[] = {
	{
		.name = "osb-text",
		.get_config = rspamd_tokenizer_osb_get_config,
		.tokenize_func = rspamd_tokenizer_osb,
	},
	{
		.name = "osb",
		.get_config = rspamd_tokenizer_osb_get_config,
		.tokenize_func = rspamd_tokenizer_osb,
	},
};

#define RSPAMD_STAT_BACKEND_ELT(nam, eltn) { \
		.name = #nam, \
		.init = rspamd_##eltn##_init, \
		.runtime = rspamd_##eltn##_runtime, \
		.process_tokens = rspamd_##eltn##_process_tokens, \
		.finalize_process = rspamd_##eltn##_finalize_process, \
		.learn_tokens = rspamd_##eltn##_learn_tokens, \
		.finalize_learn = rspamd_##eltn##_finalize_learn, \
		.total_learns = rspamd_##eltn##_total_learns, \
		.inc_learns = rspamd_##eltn##_inc_learns, \
		.dec_learns = rspamd_##eltn##_dec_learns, \
		.get_stat = rspamd_##eltn##_get_stat, \
		.load_tokenizer_config = rspamd_##eltn##_load_tokenizer_config, \
		.close = rspamd_##eltn##_close \
	}

static struct rspamd_stat_backend stat_backends[] = {
		RSPAMD_STAT_BACKEND_ELT(mmap, mmaped_file),
		RSPAMD_STAT_BACKEND_ELT(sqlite3, sqlite3),
#ifdef WITH_HIREDIS
		RSPAMD_STAT_BACKEND_ELT(redis, redis)
#endif
};

static struct rspamd_stat_cache stat_caches[] = {
	{
		.name = RSPAMD_DEFAULT_CACHE,
		.init = rspamd_stat_cache_sqlite3_init,
		.process = rspamd_stat_cache_sqlite3_process,
		.close = rspamd_stat_cache_sqlite3_close
	}
};

void
rspamd_stat_init (struct rspamd_config *cfg, struct event_base *ev_base)
{
	GList *cur, *curst;
	struct rspamd_classifier_config *clf;
	struct rspamd_statfile_config *stf;
	struct rspamd_stat_backend *bk;
	struct rspamd_statfile *st;
	struct rspamd_classifier *cl;
	const ucl_object_t *cache_obj = NULL, *cache_name_obj;
	const gchar *cache_name = NULL;

	if (stat_ctx == NULL) {
		stat_ctx = g_slice_alloc0 (sizeof (*stat_ctx));
	}

	stat_ctx->backends_subrs = stat_backends;
	stat_ctx->backends_count = G_N_ELEMENTS (stat_backends);
	stat_ctx->classifiers_subrs = stat_classifiers;
	stat_ctx->classifiers_count = G_N_ELEMENTS (stat_classifiers);
	stat_ctx->tokenizers_subrs = stat_tokenizers;
	stat_ctx->tokenizers_count = G_N_ELEMENTS (stat_tokenizers);
	stat_ctx->caches_subrs = stat_caches;
	stat_ctx->caches_count = G_N_ELEMENTS (stat_caches);
	stat_ctx->cfg = cfg;
	stat_ctx->statfiles = g_ptr_array_new ();
	stat_ctx->classifiers = g_ptr_array_new ();
	stat_ctx->async_elts = g_queue_new ();
	stat_ctx->ev_base = ev_base;
	REF_RETAIN (stat_ctx->cfg);

	/* Create statfiles from the classifiers */
	cur = cfg->classifiers;

	while (cur) {
		clf = cur->data;
		bk = rspamd_stat_get_backend (clf->backend);
		g_assert (bk != NULL);

		/* XXX:
		 * Here we get the first classifier tokenizer config as the only one
		 * We NO LONGER support multiple tokenizers per rspamd instance
		 */
		if (stat_ctx->tkcf == NULL) {
			stat_ctx->tokenizer = rspamd_stat_get_tokenizer (clf->tokenizer->name);
			g_assert (stat_ctx->tokenizer != NULL);
			stat_ctx->tkcf = stat_ctx->tokenizer->get_config (cfg->cfg_pool,
					clf->tokenizer, NULL);
		}

		cl = g_slice_alloc0 (sizeof (*cl));
		cl->cfg = clf;
		cl->ctx = stat_ctx;
		cl->statfiles_ids = g_array_new (FALSE, FALSE, sizeof (gint));
		cl->subrs = rspamd_stat_get_classifier (clf->classifier);
		g_assert (cl->subrs != NULL);
		cl->subrs->init_func (cfg->cfg_pool, cl);

		/* Init classifier cache */
		if (clf->opts) {
			cache_obj = ucl_object_find_key (clf->opts, "cache");
			cache_name_obj = NULL;

			if (cache_obj) {
				cache_name_obj = ucl_object_find_key (cache_obj, "name");
			}

			if (cache_name_obj) {
				cache_name = ucl_object_tostring (cache_name_obj);
			}
		}

		cl->cache = rspamd_stat_get_cache (cache_name);
		g_assert (cl->cache != NULL);
		cl->cachecf = cl->cache->init (stat_ctx, cfg, cache_obj);

		curst = clf->statfiles;

		while (curst) {
			stf = curst->data;
			st = g_slice_alloc0 (sizeof (*st));
			st->classifier = cl;
			st->stcf = stf;
			st->backend = bk;
			st->bkcf = bk->init (stat_ctx, cfg, st);
			msg_debug_config ("added backend %s for symbol %s",
					bk->name, stf->symbol);

			if (st->bkcf == NULL) {
				msg_err_config ("cannot init backend %s for statfile %s",
						clf->backend, stf->symbol);

				g_slice_free1 (sizeof (*st), st);
			}
			else {
				st->id = stat_ctx->statfiles->len;
				g_ptr_array_add (stat_ctx->statfiles, st);
				g_array_append_val (cl->statfiles_ids, st->id);
			}

			curst = curst->next;
		}

		g_ptr_array_add (stat_ctx->classifiers, cl);

		cur = cur->next;
	}
}

void
rspamd_stat_close (void)
{
	struct rspamd_classifier *cl;
	struct rspamd_statfile *st;
	struct rspamd_stat_ctx *st_ctx;
	struct rspamd_stat_async_elt *aelt;
	GList *cur;
	guint i, j;
	gint id;

	st_ctx = rspamd_stat_get_ctx ();
	g_assert (st_ctx != NULL);

	for (i = 0; i < st_ctx->classifiers->len; i ++) {
		cl = g_ptr_array_index (st_ctx->classifiers, i);

		for (j = 0; j < cl->statfiles_ids->len; j ++) {
			id = g_array_index (cl->statfiles_ids, gint, j);
			st = g_ptr_array_index (st_ctx->statfiles, id);
			st->backend->close (st->bkcf);

			g_slice_free1 (sizeof (*st), st);
		}

		g_array_free (cl->statfiles_ids, TRUE);
		g_slice_free1 (sizeof (*cl), cl);
	}

	cur = st_ctx->async_elts->head;

	while (cur) {
		aelt = cur->data;

		if (aelt->cleanup) {
			aelt->cleanup (aelt, aelt->ud);
		}

		cur = g_list_next (cur);
	}

	g_queue_free (stat_ctx->async_elts);
	g_ptr_array_free (st_ctx->statfiles, TRUE);
	g_ptr_array_free (st_ctx->classifiers, TRUE);
	REF_RELEASE (stat_ctx->cfg);
	g_slice_free1 (sizeof (*st_ctx), st_ctx);

	/* Set global var to NULL */
	stat_ctx = NULL;
}

struct rspamd_stat_ctx *
rspamd_stat_get_ctx (void)
{
	return stat_ctx;
}

struct rspamd_stat_classifier *
rspamd_stat_get_classifier (const gchar *name)
{
	guint i;

	if (name == NULL || name[0] == '\0') {
		name = RSPAMD_DEFAULT_CLASSIFIER;
	}

	for (i = 0; i < stat_ctx->classifiers_count; i ++) {
		if (strcmp (name, stat_ctx->classifiers_subrs[i].name) == 0) {
			return &stat_ctx->classifiers_subrs[i];
		}
	}

	return NULL;
}

struct rspamd_stat_backend *
rspamd_stat_get_backend (const gchar *name)
{
	guint i;

	if (name == NULL || name[0] == '\0') {
		name = RSPAMD_DEFAULT_BACKEND;
	}

	for (i = 0; i < stat_ctx->backends_count; i ++) {
		if (strcmp (name, stat_ctx->backends_subrs[i].name) == 0) {
			return &stat_ctx->backends_subrs[i];
		}
	}

	return NULL;
}

struct rspamd_stat_tokenizer *
rspamd_stat_get_tokenizer (const gchar *name)
{
	guint i;

	if (name == NULL || name[0] == '\0') {
		name = RSPAMD_DEFAULT_TOKENIZER;
	}

	for (i = 0; i < stat_ctx->tokenizers_count; i ++) {
		if (strcmp (name, stat_ctx->tokenizers_subrs[i].name) == 0) {
			return &stat_ctx->tokenizers_subrs[i];
		}
	}

	return NULL;
}

struct rspamd_stat_cache *
rspamd_stat_get_cache (const gchar *name)
{
	guint i;

	if (name == NULL || name[0] == '\0') {
		name = RSPAMD_DEFAULT_CACHE;
	}

	for (i = 0; i < stat_ctx->caches_count; i++) {
		if (strcmp (name, stat_ctx->caches_subrs[i].name) == 0) {
			return &stat_ctx->caches_subrs[i];
		}
	}

	return NULL;
}
