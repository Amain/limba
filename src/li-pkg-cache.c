/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*-
 *
 * Copyright (C) 2015 Matthias Klumpp <matthias@tenstral.net>
 *
 * Licensed under the GNU Lesser General Public License Version 2.1
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 2.1 of the license, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * SECTION:li-pkg-cache
 * @short_description: Download information about available packages from remote sources.
 *
 */

#include "config.h"
#include "li-pkg-cache.h"

#include <glib/gstdio.h>
#include <glib/gi18n-lib.h>
#include <stdlib.h>
#include <locale.h>
#include <gpgme.h>
#include <appstream.h>
#include <curl/curl.h>
#include <errno.h>
#include <math.h>
#include <archive_entry.h>
#include <archive.h>
#include <fcntl.h>

#include "li-utils.h"
#include "li-utils-private.h"
#include "li-pkg-index.h"
#include "li-keyring.h"

#define DEFAULT_BLOCK_SIZE 65536

typedef struct _LiPkgCachePrivate	LiPkgCachePrivate;
struct _LiPkgCachePrivate
{
	LiPkgIndex *index;
	GPtrArray *repo_urls;

	LiKeyring *kr;
	gchar *cache_index_fname;
	gchar *tmp_dir;
};

G_DEFINE_TYPE_WITH_PRIVATE (LiPkgCache, li_pkg_cache, G_TYPE_OBJECT)

enum {
	SIGNAL_STATE_CHANGED,
	SIGNAL_PROGRESS,
	SIGNAL_LAST
};

static guint signals[SIGNAL_LAST] = { 0 };

#define GET_PRIVATE(o) (li_pkg_cache_get_instance_private (o))

#define LIMBA_CACHE_DIR "/var/cache/limba/"
#define APPSTREAM_CACHE "/var/cache/app-info/"

typedef struct {
	LiPkgCache *cache;
	gchar *id;
} LiCacheProgressHelper;

/**
 * li_pkg_cache_finalize:
 **/
static void
li_pkg_cache_finalize (GObject *object)
{
	LiPkgCache *cache = LI_PKG_CACHE (object);
	LiPkgCachePrivate *priv = GET_PRIVATE (cache);

	g_object_unref (priv->index);
	g_ptr_array_unref (priv->repo_urls);
	g_free (priv->cache_index_fname);
	g_object_unref (priv->kr);

	/* cleanup */
	li_delete_dir_recursive (priv->tmp_dir);
	g_free (priv->tmp_dir);

	G_OBJECT_CLASS (li_pkg_cache_parent_class)->finalize (object);
}

static void
li_pkg_cache_load_repolist (LiPkgCache *cache, const gchar *fname)
{
	gchar *content = NULL;
	gchar **lines;
	guint i;
	gboolean ret;
	LiPkgCachePrivate *priv = GET_PRIVATE (cache);

	/* Load repo list - failure to open it is no error, since it might simply be nonexistent.
	 * That could e.g. happen after a stateless system reset */
	ret = g_file_get_contents (fname, &content, NULL, NULL);
	if (!ret)
		return;

	lines = g_strsplit (content, "\n", -1);
	g_free (content);

	for (i = 0; lines[i] != NULL; i++) {
		g_strstrip (lines[i]);

		/* ignore comments */
		if (g_str_has_prefix (lines[i], "#"))
			continue;
		/* ignore empty lines */
		if (g_strcmp0 (lines[i], "") == 0)
			continue;

		g_ptr_array_add (priv->repo_urls, g_strdup (lines[i]));
	}

	g_strfreev (lines);
}

/**
 * li_pkg_cache_init:
 **/
static void
li_pkg_cache_init (LiPkgCache *cache)
{
	LiPkgCachePrivate *priv = GET_PRIVATE (cache);

	priv->index = li_pkg_index_new ();
	priv->repo_urls = g_ptr_array_new_with_free_func (g_free);
	priv->cache_index_fname = g_build_filename (LIMBA_CACHE_DIR, "available.index", NULL);
	priv->kr = li_keyring_new ();

	/* get temporary directory */
	priv->tmp_dir = li_utils_get_tmp_dir ("remote");

	/* load repository url list */
	li_pkg_cache_load_repolist (cache, "/etc/limba/sources.list"); /* defined by the user / distributor */
	li_pkg_cache_load_repolist (cache, "/var/lib/limba/update-sources.list"); /* managed automatically by Limba */
}

/**
 * curl_dl_write_data:
 */
static size_t
curl_dl_write_data (gpointer ptr, size_t size, size_t nmemb, FILE *stream)
{
	return fwrite (ptr, size, nmemb, stream);
}

/**
 * curl_dl_read_data:
 */
static size_t
curl_dl_read_data (gpointer ptr, size_t size, size_t nmemb, FILE *stream)
{
	return fread (ptr, size, nmemb, stream);
}

/**
 * li_pkg_cache_curl_progress_cb:
 */
gint
li_pkg_cache_curl_progress_cb (LiCacheProgressHelper *helper, double dltotal, double dlnow, double ultotal, double ulnow)
{
	guint percentage;

	percentage = round (100/dltotal*dlnow);
	g_signal_emit (helper->cache, signals[SIGNAL_PROGRESS], 0,
					percentage, helper->id);

	return 0;
}

/**
 * li_pkg_cache_download_file_sync:
 */
static void
li_pkg_cache_download_file_sync (LiPkgCache *cache, const gchar *url, const gchar *dest, const gchar *id, GError **error)
{
	CURL *curl;
	CURLcode res;
	FILE *outfile;
	long http_code = 0;
	LiCacheProgressHelper helper;

	curl = curl_easy_init();
	if (curl == NULL) {
		g_set_error (error,
				LI_PKG_CACHE_ERROR,
				LI_PKG_CACHE_ERROR_FAILED,
				_("Could not initialize CURL!"));
		return;
	}

	outfile = fopen (dest, "w");
	if (outfile == NULL) {
		g_set_error (error,
				LI_PKG_CACHE_ERROR,
				LI_PKG_CACHE_ERROR_FAILED,
				_("Could not open file '%s' for writing."), dest);

		curl_easy_cleanup (curl);
		return;
	}

	helper.cache = g_object_ref (cache);
	helper.id = g_strdup (id);

	curl_easy_setopt (curl, CURLOPT_URL, url);
	curl_easy_setopt (curl, CURLOPT_WRITEDATA, outfile);
	curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION, curl_dl_write_data);
	curl_easy_setopt (curl, CURLOPT_READFUNCTION, curl_dl_read_data);
	curl_easy_setopt (curl, CURLOPT_NOPROGRESS, FALSE);
	curl_easy_setopt (curl, CURLOPT_FAILONERROR, TRUE);
	curl_easy_setopt (curl, CURLOPT_PROGRESSFUNCTION, li_pkg_cache_curl_progress_cb);
	curl_easy_setopt (curl, CURLOPT_PROGRESSDATA, &helper);

	res = curl_easy_perform (curl);

	if (res != CURLE_OK) {
		curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &http_code);
		if (http_code == 404) {
			/* TODO: Perform the same check for FTP? */
			g_set_error (error,
					LI_PKG_CACHE_ERROR,
					LI_PKG_CACHE_ERROR_REMOTE_NOT_FOUND,
					_("Could not find remote data '%s': %s."), url, curl_easy_strerror (res));
		} else {
			g_set_error (error,
					LI_PKG_CACHE_ERROR,
					LI_PKG_CACHE_ERROR_DOWNLOAD_FAILED,
					_("Unable to download data from '%s': %s."), url, curl_easy_strerror (res));
		}

		g_remove (dest);
	}

	/* cleanup */
	fclose (outfile);
	curl_easy_cleanup (curl);

	/* free our helper */
	g_object_unref (helper.cache);
	g_free (helper.id);
}

/**
 * _li_pkg_cache_signature_hash_matches:
 *
 * Helper function to verify the repository signature
 */
gboolean
_li_pkg_cache_signature_hash_matches (gchar **sigparts, const gchar *fname, const gchar *id)
{
	gint i;
	gboolean valid;
	_cleanup_free_ gchar *hash = NULL;
	_cleanup_free_ gchar *expected_hash = NULL;

	for (i = 0; sigparts[i] != NULL; i++) {
		if (g_str_has_suffix (sigparts[i], id)) {
			gchar **tmp;
			tmp = g_strsplit (sigparts[i], "\t", 2);
			if (g_strv_length (tmp) != 2) {
				g_strfreev (tmp);
				continue;
			}
			expected_hash = g_strdup (tmp[0]);
			g_strfreev (tmp);
			break;
		}
	}

	hash = li_compute_checksum_for_file (fname);
	valid = g_strcmp0 (hash, expected_hash) == 0;
	if (!valid)
		g_debug ("Hash value of repository index '%s' do not match file.", id);

	return valid;
}

/**
 * li_pkg_cache_extract_icon_tarball:
 */
static void
li_pkg_cache_extract_icon_tarball (LiPkgCache *cache, const gchar *tarball_fname, const gchar *dest_dir, GError **error)
{
	struct archive *ar;
	struct archive_entry* e;
	gint res;

	/* open the archive */
	ar = archive_read_new ();
	archive_read_support_filter_gzip (ar);
	archive_read_support_format_tar (ar);

	/* open the file, exit on error */
	res = archive_read_open_filename (ar, tarball_fname, DEFAULT_BLOCK_SIZE);
	if (res != ARCHIVE_OK) {
		g_set_error (error,
				LI_PKG_CACHE_ERROR,
				LI_PKG_CACHE_ERROR_UNPACK,
				_("Could not open icon tarball! Error: %s"), archive_error_string (ar));
		archive_read_free (ar);
		return;
	}

	while (archive_read_next_header (ar, &e) == ARCHIVE_OK) {
		_cleanup_free_ gchar *fname = NULL;
		_cleanup_free_ gchar *dest_fname = NULL;
		gint fd;
		gboolean ret;

		const void *buff = NULL;
		gsize size = 0UL;
		off_t offset = {0};
		off_t output_offset = {0};
		gsize bytes_to_write = 0UL;
		gssize bytes_written = 0L;

		if (!g_str_has_suffix (archive_entry_pathname (e), ".png"))
			continue;

		g_mkdir_with_parents (dest_dir, 0755);

		fname = g_path_get_basename (archive_entry_pathname (e));
		dest_fname = g_build_filename (dest_dir, fname, NULL);

		fd = open (dest_fname, (O_CREAT | O_WRONLY) | O_TRUNC, ((S_IRUSR | S_IWUSR) | S_IRGRP) | S_IROTH);
		if (fd < 0) {
			g_set_error (error,
					LI_PKG_CACHE_ERROR,
					LI_PKG_CACHE_ERROR_UNPACK,
					_("Unable to extract file. Error: %s"), g_strerror (errno));
			goto out;
		}

		ret = TRUE;
		while (archive_read_data_block (ar, &buff, &size, &offset) == ARCHIVE_OK) {
			if (offset > output_offset) {
				lseek (fd, offset - output_offset, SEEK_CUR);
				output_offset = offset;
			}
			while (size > (gssize) 0) {
				bytes_to_write = size;
				if (bytes_to_write > DEFAULT_BLOCK_SIZE)
					bytes_to_write = DEFAULT_BLOCK_SIZE;

				bytes_written = write (fd, buff, bytes_to_write);
				if (bytes_written < ((gssize) 0)) {
					g_set_error (error,
						LI_PKG_CACHE_ERROR,
						LI_PKG_CACHE_ERROR_UNPACK,
						_("Unable to extract file. Error: %s"), g_strerror (errno));
					ret = FALSE;
					break;
				}
				output_offset += bytes_written;
				buff += bytes_written;
				size -= bytes_written;
			}
			if (!ret)
				break;
		}

		if (close (fd) != 0) {
			g_set_error (error,
				LI_PKG_CACHE_ERROR,
				LI_PKG_CACHE_ERROR_UNPACK,
				_("Closing of file desriptor failed. Error: %s"), g_strerror (errno));
			goto out;
		}
	}

out:
	archive_read_close (ar);
	archive_read_free (ar);
}

/**
 * li_pkg_cache_update_icon_cache_for_size:
 */
static void
li_pkg_cache_update_icon_cache_for_size (LiPkgCache *cache, const gchar *tmp_dir, const gchar *url, const gchar *destination, const gchar *size, GError **error)
{
	_cleanup_free_ gchar *icon_url = NULL;
	_cleanup_free_ gchar *tar_dest = NULL;
	_cleanup_free_ gchar *icons_dest = NULL;
	GError *tmp_error = NULL;

	/* download and extract icons */
	icon_url = g_strdup_printf ("%s/indices/icons_%s.tar.gz", url, size);
	tar_dest = g_strdup_printf ("%s/icons_%s.tar.gz", tmp_dir, size);
	li_pkg_cache_download_file_sync (cache, icon_url, tar_dest, NULL, &tmp_error);
	if (tmp_error != NULL) {
		if (tmp_error->code == LI_PKG_CACHE_ERROR_REMOTE_NOT_FOUND) {
			/* we can ignore the error here, no icons found */
			g_debug ("Skipping '%s' icons for repository: %s", size, url);
			g_error_free (tmp_error);
			tmp_error = NULL;
			return;
		} else {
			g_propagate_error (error, tmp_error);
			return;
		}
	}
	icons_dest = g_build_filename (destination, size, NULL);
	li_pkg_cache_extract_icon_tarball (cache, tar_dest, icons_dest, &tmp_error);
	if (tmp_error != NULL) {
		g_propagate_error (error, tmp_error);
		return;
	}
}

/**
 * li_pkg_cache_update_icon_cache:
 */
static void
li_pkg_cache_update_icon_cache (LiPkgCache *cache, const gchar *repo_cache, const gchar *url, const gchar *destination, GError **error)
{
	_cleanup_free_ gchar *tmp_dir = NULL;
	GError *tmp_error = NULL;

	tmp_dir = g_build_filename (repo_cache, "icon-tmp", NULL);
	g_mkdir_with_parents (tmp_dir, 0755);

	li_pkg_cache_update_icon_cache_for_size (cache, tmp_dir, url, destination, "64x64", &tmp_error);
	if (tmp_error != NULL) {
		g_propagate_error (error, tmp_error);
		return;
	}

	li_pkg_cache_update_icon_cache_for_size (cache, tmp_dir, url, destination, "128x128", &tmp_error);
	if (tmp_error != NULL) {
		g_propagate_error (error, tmp_error);
		return;
	}

	/* cleanup */
	li_delete_dir_recursive (tmp_dir);
}

/**
 * li_pkg_cache_update:
 *
 * Update the package cache by downloading new package indices from the web.
 */
void
li_pkg_cache_update (LiPkgCache *cache, GError **error)
{
	guint i;
	GError *tmp_error = NULL;
	_cleanup_object_unref_ LiPkgIndex *global_index = NULL;
	_cleanup_free_ gchar *current_arch = NULL;
	LiPkgCachePrivate *priv = GET_PRIVATE (cache);

	/* ensure AppStream cache exists */
	g_mkdir_with_parents (APPSTREAM_CACHE, 0755);

	/* create index of available packages */
	global_index = li_pkg_index_new ();

	current_arch = li_get_current_arch_h ();

	for (i = 0; i < priv->repo_urls->len; i++) {
		const gchar *url;
		_cleanup_free_ gchar *url_index_all = NULL;
		_cleanup_free_ gchar *url_index_arch = NULL;
		_cleanup_free_ gchar *url_signature = NULL;
		_cleanup_free_ gchar *url_asdata_all = NULL;
		_cleanup_free_ gchar *url_asdata_arch = NULL;
		_cleanup_free_ gchar *dest = NULL;
		_cleanup_free_ gchar *dest_index_all = NULL;
		_cleanup_free_ gchar *dest_index_arch = NULL;
		_cleanup_free_ gchar *dest_asdata_all = NULL;
		_cleanup_free_ gchar *dest_asdata_arch = NULL;
		_cleanup_free_ gchar *dest_repoconf = NULL;
		_cleanup_free_ gchar *dest_signature = NULL;
		_cleanup_object_unref_ GFile *idxfile = NULL;
		_cleanup_object_unref_ GFile *asfile = NULL;
		_cleanup_object_unref_ LiPkgIndex *tmp_index = NULL;
		_cleanup_strv_free_ gchar **hashlist = NULL;
		_cleanup_free_ gchar *fpr = NULL;
		_cleanup_object_unref_ AsMetadata *metad = NULL;
		_cleanup_free_ gchar *dest_ascache = NULL;
		_cleanup_free_ gchar *md5sum = NULL;
		gboolean index_read;
		GPtrArray *pkgs;
		guint j;
		gchar *tmp;
		gchar *tmp2;

		LiTrustLevel tlevel;

		url = (const gchar*) g_ptr_array_index (priv->repo_urls, i);

		metad = as_metadata_new ();
		/* do not filter languages */
		as_metadata_set_locale (metad, "ALL");

		/* create temporary index */
		tmp_index = li_pkg_index_new ();

		url_index_all   = g_build_filename (url, "indices", "all", "Index.gz", NULL);
		url_asdata_all  = g_build_filename (url, "indices", "all", "Metadata.xml.gz", NULL);
		url_index_arch  = g_build_filename (url, "indices", current_arch, "Index.gz", NULL);
		url_asdata_arch = g_build_filename (url, "indices", current_arch, "Metadata.xml.gz", NULL);
		url_signature   = g_build_filename (url, "indices", "Indices.gpg", NULL);

		/* prepare cache dir and ensure it exists */
		md5sum = g_compute_checksum_for_string (G_CHECKSUM_MD5, url, -1);

		/* set cache directory */
		dest = g_build_filename (LIMBA_CACHE_DIR, md5sum, NULL);
		g_mkdir_with_parents (dest, 0755);

		/* set AppStream target file */
		tmp = g_build_filename (APPSTREAM_CACHE, "xmls", NULL);
		g_mkdir_with_parents (tmp, 0755);
		g_free (tmp);
		tmp = g_strdup_printf ("limba_%s.xml.gz", md5sum);
		dest_ascache = g_build_filename (APPSTREAM_CACHE, "xmls", tmp, NULL);
		g_free (tmp);

		dest_index_all = g_build_filename (dest, "Index-all.gz", NULL);
		dest_index_arch = g_strdup_printf ("%s/Index-%s.gz", dest, current_arch);
		dest_asdata_all = g_build_filename (dest, "Metadata-all.xml.gz", NULL);
		dest_asdata_arch = g_strdup_printf ("%s/Metadata-%s.xml.gz", dest, current_arch);
		dest_signature = g_build_filename (dest, "Indices.gpg", NULL);

		g_debug ("Updating cached data for repository: %s", url);

		/* download indices */
		li_pkg_cache_download_file_sync (cache, url_index_all, dest_index_all, NULL, &tmp_error);
		if (tmp_error != NULL) {
			if (tmp_error->code == LI_PKG_CACHE_ERROR_REMOTE_NOT_FOUND) {
				/* we can ignore the error here, this repository is not for us */
				g_debug ("Skipping arch 'all' for repository: %s", url);
				g_error_free (tmp_error);
				tmp_error = NULL;
			} else {
				g_propagate_error (error, tmp_error);
				return;
			}
		}
		li_pkg_cache_download_file_sync (cache, url_index_arch, dest_index_arch, NULL, &tmp_error);
		if (tmp_error != NULL) {
			if (tmp_error->code == LI_PKG_CACHE_ERROR_REMOTE_NOT_FOUND) {
				/* we can ignore the error here, this repository is not for us */
				g_debug ("Skipping arch '%s' for repository: %s", current_arch, url);
				g_error_free (tmp_error);
				tmp_error = NULL;
			} else {
				g_propagate_error (error, tmp_error);
				return;
			}
		}

		/* download AppStream metadata */
		li_pkg_cache_download_file_sync (cache, url_asdata_all, dest_asdata_all, NULL, &tmp_error);
		if (tmp_error != NULL) {
			if (tmp_error->code == LI_PKG_CACHE_ERROR_REMOTE_NOT_FOUND) {
				/* we can ignore the error here, no AppStream metadata for us */
				g_debug ("No arch-indep AppStream metadata on repository: %s", url);
				g_error_free (tmp_error);
				tmp_error = NULL;
			} else {
				g_propagate_error (error, tmp_error);
				return;
			}
		}
		li_pkg_cache_download_file_sync (cache, url_asdata_arch, dest_asdata_arch, NULL, &tmp_error);
		if (tmp_error != NULL) {
			if (tmp_error->code == LI_PKG_CACHE_ERROR_REMOTE_NOT_FOUND) {
				/* we can ignore the error here, no AppStream metadata for us */
				g_debug ("No AppStream metadata for arch '%s' on repository: %s", current_arch, url);
				g_error_free (tmp_error);
				tmp_error = NULL;
			} else {
				g_propagate_error (error, tmp_error);
				return;
			}
		}

		/* download signature */
		li_pkg_cache_download_file_sync (cache, url_signature, dest_signature, NULL, &tmp_error);
		if (tmp_error != NULL) {
			g_propagate_error (error, tmp_error);
			return;
		}

		/* write repo hints file */
		dest_repoconf = g_build_filename (dest, "repo", NULL);
		g_file_set_contents (dest_repoconf, url, -1, &tmp_error);
		if (tmp_error != NULL) {
			g_propagate_error (error, tmp_error);
			return;
		}

		g_debug ("Updated data for repository: %s", url);

		/* check signature */
		tmp = NULL;
		g_file_get_contents (dest_signature, &tmp, NULL, &tmp_error);
		if (tmp_error != NULL) {
			g_propagate_prefixed_error (error, tmp_error, "Unable to read signature data:");
			return;
		}
		tlevel = li_keyring_process_signature (priv->kr, tmp, &tmp2, &fpr, &tmp_error);
		g_free (tmp);
		if (tmp_error != NULL) {
			g_propagate_error (error, tmp_error);
			return;
		}
		hashlist = g_strsplit (tmp2, "\n", -1);
		g_free (tmp2);

		if (tlevel < LI_TRUST_LEVEL_MEDIUM) {
			g_set_error (error,
					LI_PKG_CACHE_ERROR,
					LI_PKG_CACHE_ERROR_VERIFICATION,
					_("Repository '%s' (signed with key '%s') is untrusted."), url, fpr);
			return;
		}

		/* load AppStream metadata */
		asfile = g_file_new_for_path (dest_asdata_all);
		if (g_file_query_exists (asfile, NULL)) {
			if (!_li_pkg_cache_signature_hash_matches (hashlist, dest_asdata_all, "indices/all/Metadata.xml.gz")) {
				g_set_error (error,
						LI_PKG_CACHE_ERROR,
						LI_PKG_CACHE_ERROR_VERIFICATION,
						_("Siganture on '%s' is invalid."), url);
				return;
			}

			as_metadata_parse_file (metad, asfile, &tmp_error);
			if (tmp_error != NULL) {
				g_propagate_prefixed_error (error, tmp_error, "Unable to load AppStream data for: %s", url);
				return;
			}
		}

		g_object_unref (asfile);
		asfile = g_file_new_for_path (dest_asdata_arch);
		if (g_file_query_exists (asfile, NULL)) {
			tmp = g_strdup_printf ("indices/%s/Metadata.xml.gz", current_arch);
			if (!_li_pkg_cache_signature_hash_matches (hashlist, dest_asdata_arch, tmp)) {
				g_free (tmp);
				g_set_error (error,
						LI_PKG_CACHE_ERROR,
						LI_PKG_CACHE_ERROR_VERIFICATION,
						_("Siganture on '%s' is invalid."), url);
				return;
			}
			g_free (tmp);

			as_metadata_parse_file (metad, asfile, &tmp_error);
			if (tmp_error != NULL) {
				g_propagate_prefixed_error (error, tmp_error, "Unable to load AppStream data for: %s", url);
				return;
			}
		}

		/* load index */
		idxfile = g_file_new_for_path (dest_index_all);
		if (g_file_query_exists (idxfile, NULL)) {
			/* validate */
			if (!_li_pkg_cache_signature_hash_matches (hashlist, dest_index_all, "indices/all/Index.gz")) {
				g_set_error (error,
						LI_PKG_CACHE_ERROR,
						LI_PKG_CACHE_ERROR_VERIFICATION,
						_("Siganture on '%s' is invalid."), url);
				return;
			}

			li_pkg_index_load_file (tmp_index, idxfile, &tmp_error);
			if (tmp_error != NULL) {
				g_propagate_prefixed_error (error, tmp_error, "Unable to load index for repository: %s", url);
				return;
			}
			index_read = TRUE;
		}

		g_object_unref (idxfile);
		idxfile = g_file_new_for_path (dest_index_arch);
		if (g_file_query_exists (idxfile, NULL)) {
			/* validate */
			tmp = g_strdup_printf ("indices/%s/Index.gz", current_arch);
			if (!_li_pkg_cache_signature_hash_matches (hashlist, dest_index_arch, tmp)) {
				g_free (tmp);
				g_set_error (error,
						LI_PKG_CACHE_ERROR,
						LI_PKG_CACHE_ERROR_VERIFICATION,
						_("Siganture on '%s' is invalid."), url);
				return;
			}
			g_free (tmp);

			li_pkg_index_load_file (tmp_index, idxfile, &tmp_error);
			if (tmp_error != NULL) {
				g_propagate_prefixed_error (error, tmp_error, "Unable to load index for repository: %s", url);
				return;
			}
			index_read = TRUE;
		}

		if (!index_read)
			g_warning ("Repository '%s' does not seem to contain any index file!", url);

		/* ensure we have a somewhat sane metadata origin */
		if (as_metadata_get_origin (metad) == NULL)
			as_metadata_set_origin (metad, md5sum);

		/* fetch icons */
		tmp = g_build_filename (APPSTREAM_CACHE,
								"icons",
								as_metadata_get_origin (metad),
								NULL);
		g_debug ("Icon cache target set: %s", tmp);
		li_pkg_cache_update_icon_cache (cache, dest, url, tmp, &tmp_error);
		g_free (tmp);
		if (tmp_error != NULL) {
			g_propagate_prefixed_error (error, tmp_error, "Unable to fetch AppStream icons:");
			return;
		}

		/* ensure that all locations are set properly */
		pkgs = li_pkg_index_get_packages (tmp_index);
		for (j = 0; j < pkgs->len; j++) {
			_cleanup_free_ gchar *new_url = NULL;
			LiPkgInfo *pki = LI_PKG_INFO (g_ptr_array_index (pkgs, j));

			/* mark package as available for installation */
			li_pkg_info_add_flag (pki, LI_PACKAGE_FLAG_AVAILABLE);

			new_url = g_build_filename (url, li_pkg_info_get_repo_location (pki), NULL);
			li_pkg_info_set_repo_location (pki, new_url);

			/* add to pkg cache */
			li_pkg_index_add_package (global_index, pki);
		}

		/* save AppStream XML data */
		as_metadata_save_distro_xml (metad, dest_ascache, &tmp_error);
		if (tmp_error != NULL) {
			g_propagate_prefixed_error (error, tmp_error, "Unable to save metadata.");
			return;
		}

		g_debug ("Loaded index of repository.");
	}

	/* save global index file */
	li_pkg_index_save_to_file (global_index, priv->cache_index_fname);
}

/**
 * li_pkg_cache_open:
 *
 * Open the package cache and load a list of available packages.
 */
void
li_pkg_cache_open (LiPkgCache *cache, GError **error)
{
	GError *tmp_error = NULL;
	_cleanup_object_unref_ GFile *file = NULL;
	LiPkgCachePrivate *priv = GET_PRIVATE (cache);

	/* TODO: Implement li_pkg_index_clear() */
	g_object_unref (priv->index);
	priv->index = li_pkg_index_new ();

	file = g_file_new_for_path (priv->cache_index_fname);

	if (g_file_query_exists (file, NULL)) {
		li_pkg_index_load_file (priv->index, file, &tmp_error);
	}
	if (tmp_error != NULL) {
		g_propagate_prefixed_error (error, tmp_error, "Unable to load package cache: ");
		return;
	}
}

/**
 * li_pkg_cache_get_packages:
 *
 * Returns: (transfer none) (element-type LiPkgInfo): Packages in the index
 */
GPtrArray*
li_pkg_cache_get_packages (LiPkgCache *cache)
{
	LiPkgCachePrivate *priv = GET_PRIVATE (cache);
	return li_pkg_index_get_packages (priv->index);
}

/**
 * li_pkg_cache_get_pkg_info:
 *
 * Returns: (transfer none): The #LiPkgInfo for pkid, or %NULL in case
 * no package with that id was found in the cache.
 */
LiPkgInfo*
li_pkg_cache_get_pkg_info (LiPkgCache *cache, const gchar *pkid)
{
	guint i;
	GPtrArray *pkgs;
	LiPkgInfo *pki = NULL;

	pkgs = li_pkg_cache_get_packages (cache);
	for (i = 0; i < pkgs->len; i++) {
		LiPkgInfo *tmp_pki = LI_PKG_INFO (g_ptr_array_index (pkgs, i));
		if (g_strcmp0 (li_pkg_info_get_id (tmp_pki), pkid) == 0) {
			pki = tmp_pki;
			break;
		}
	}

	return pki;
}

/**
 * li_pkg_cache_fetch_remote:
 * @cache: an instance of #LiPkgCache
 * @pkgid: id of the package to download
 *
 * Download a package from a remote source.
 *
 * Returns: (transfer full): Path to the downloaded package file.
 */
gchar*
li_pkg_cache_fetch_remote (LiPkgCache *cache, const gchar *pkgid, GError **error)
{
	GPtrArray *pkgs;
	GError *tmp_error = NULL;
	guint i;
	LiPkgInfo *pki = NULL;
	gchar *tmp;
	_cleanup_free_ gchar *dest_fname = NULL;
	LiPkgCachePrivate *priv = GET_PRIVATE (cache);

	/* find our package metadata */
	pkgs = li_pkg_cache_get_packages (cache);
	for (i = 0; i < pkgs->len; i++) {
		LiPkgInfo *tmp_pki = LI_PKG_INFO (g_ptr_array_index (pkgs, i));
		if (g_strcmp0 (li_pkg_info_get_id (tmp_pki), pkgid) == 0) {
			pki = tmp_pki;
			break;
		}
	}

	if (pki == NULL) {
		g_set_error (error,
				LI_PKG_CACHE_ERROR,
				LI_PKG_CACHE_ERROR_NOT_FOUND,
				_("Could not find package matching id '%s'."), pkgid);
		return NULL;
	}

	tmp = g_path_get_basename (li_pkg_info_get_repo_location (pki));
	dest_fname = g_build_filename (priv->tmp_dir, tmp, NULL);
	g_free (tmp);

	g_debug ("Fetching remote package from: %s", li_pkg_info_get_repo_location (pki));
	li_pkg_cache_download_file_sync (cache,
								li_pkg_info_get_repo_location (pki),
								dest_fname,
								pkgid,
								&tmp_error);
	if (tmp_error != NULL) {
		g_propagate_error (error, tmp_error);
		return NULL;
	}
	g_debug ("Package '%s' downloaded from remote.", pkgid);


	return g_strdup (dest_fname);
}

/**
 * li_pkg_cache_error_quark:
 *
 * Return value: An error quark.
 **/
GQuark
li_pkg_cache_error_quark (void)
{
	static GQuark quark = 0;
	if (!quark)
		quark = g_quark_from_static_string ("LiPkgCacheError");
	return quark;
}

/**
 * li_pkg_cache_class_init:
 **/
static void
li_pkg_cache_class_init (LiPkgCacheClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = li_pkg_cache_finalize;

	signals[SIGNAL_PROGRESS] =
		g_signal_new ("progress",
				G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
				0, NULL, NULL, g_cclosure_marshal_VOID__UINT_POINTER,
				G_TYPE_NONE, 2, G_TYPE_UINT, G_TYPE_POINTER);
}

/**
 * li_pkg_cache_new:
 *
 * Creates a new #LiPkgCache.
 *
 * Returns: (transfer full): a #LiPkgCache
 *
 **/
LiPkgCache *
li_pkg_cache_new (void)
{
	LiPkgCache *cache;
	cache = g_object_new (LI_TYPE_PKG_CACHE, NULL);
	return LI_PKG_CACHE (cache);
}
