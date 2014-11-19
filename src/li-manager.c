/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*-
 *
 * Copyright (C) 2014 Matthias Klumpp <matthias@tenstral.net>
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
 * SECTION:li-manager
 * @short_description: Work with mgralled software
 */

#include "config.h"
#include "li-manager.h"

#include <glib/gi18n-lib.h>
#include <glib/gstdio.h>

#include "li-utils.h"
#include "li-utils-private.h"
#include "li-pkg-info.h"

typedef struct _LiManagerPrivate	LiManagerPrivate;
struct _LiManagerPrivate
{
	GPtrArray *installed_sw; /* of LiPkgInfo */
	GPtrArray *installed_rt; /* of LiRuntime */
};

G_DEFINE_TYPE_WITH_PRIVATE (LiManager, li_manager, G_TYPE_OBJECT)

#define GET_PRIVATE(o) (li_manager_get_instance_private (o))

/**
 * li_manager_finalize:
 **/
static void
li_manager_finalize (GObject *object)
{
	LiManager *mgr = LI_MANAGER (object);
	LiManagerPrivate *priv = GET_PRIVATE (mgr);

	g_ptr_array_unref (priv->installed_sw);
	g_ptr_array_unref (priv->installed_rt);

	G_OBJECT_CLASS (li_manager_parent_class)->finalize (object);
}

/**
 * li_manager_init:
 **/
static void
li_manager_init (LiManager *mgr)
{
	LiManagerPrivate *priv = GET_PRIVATE (mgr);

	priv->installed_sw = g_ptr_array_new_with_free_func (g_object_unref);
	priv->installed_rt = g_ptr_array_new_with_free_func (g_object_unref);
}

/**
 * li_manager_reset_cached_data:
 */
static void
li_manager_reset_cached_data (LiManager *mgr)
{
	LiManagerPrivate *priv = GET_PRIVATE (mgr);

	g_ptr_array_unref (priv->installed_sw);
	g_ptr_array_unref (priv->installed_rt);
	priv->installed_sw = g_ptr_array_new_with_free_func (g_object_unref);
	priv->installed_rt = g_ptr_array_new_with_free_func (g_object_unref);
}

/**
 * li_manager_find_installed_software:
 **/
static gboolean
li_manager_find_installed_software (LiManager *mgr)
{
	GError *tmp_error = NULL;
	GFile *fdir;
	GFileEnumerator *enumerator = NULL;
	GFileInfo *file_info;
	LiManagerPrivate *priv = GET_PRIVATE (mgr);

	if (!g_file_test (LI_SOFTWARE_ROOT, G_FILE_TEST_IS_DIR)) {
		/* directory not found, no software to be searched for */
		return TRUE;
	}

	/* get stuff in the software directory */
	fdir = g_file_new_for_path (LI_SOFTWARE_ROOT);
	enumerator = g_file_enumerate_children (fdir, G_FILE_ATTRIBUTE_STANDARD_NAME, 0, NULL, &tmp_error);
	if (tmp_error != NULL)
		goto out;

	while ((file_info = g_file_enumerator_next_file (enumerator, NULL, &tmp_error)) != NULL) {
		gchar *path;
		if (tmp_error != NULL)
			goto out;

		if (g_file_info_get_is_hidden (file_info))
			continue;
		path = g_build_filename (LI_SOFTWARE_ROOT,
								 g_file_info_get_name (file_info),
								 "control",
								 NULL);
		if (g_file_test (path, G_FILE_TEST_IS_REGULAR)) {
			GFile *ctlfile;
			ctlfile = g_file_new_for_path (path);
			if (g_file_query_exists (ctlfile, NULL)) {
				LiPkgInfo *ctl;
				ctl = li_pkg_info_new ();
				li_pkg_info_load_file (ctl, ctlfile);
				g_ptr_array_add (priv->installed_sw, ctl);
			}
			g_object_unref (ctlfile);
		}
		g_free (path);
	}


out:
	g_object_unref (fdir);
	if (enumerator != NULL)
		g_object_unref (enumerator);
	if (tmp_error != NULL) {
		g_printerr ("Error while searching for installed software: %s\n", tmp_error->message);
		return FALSE;
	}

	return TRUE;
}

/**
 * li_manager_get_installed_software:
 *
 * Returns: (transfer none) (element-type LiPkgInfo): A list of installed software
 **/
GPtrArray*
li_manager_get_installed_software (LiManager *mgr)
{
	LiManagerPrivate *priv = GET_PRIVATE (mgr);

	if (priv->installed_sw->len == 0) {
		/* in case no software was found or we never searched for it, we
		 * do this again
		 */
		li_manager_find_installed_software (mgr);
	}

	return priv->installed_sw;
}

/**
 * li_manager_find_installed_runtimes:
 **/
static gboolean
li_manager_find_installed_runtimes (LiManager *mgr)
{
	GError *tmp_error = NULL;
	GFile *fdir;
	GFileEnumerator *enumerator = NULL;
	GFileInfo *file_info;
	_cleanup_free_ gchar *runtime_root;
	LiManagerPrivate *priv = GET_PRIVATE (mgr);

	runtime_root = g_build_filename (LI_SOFTWARE_ROOT, "runtimes", NULL);
	if (!g_file_test (runtime_root, G_FILE_TEST_IS_DIR)) {
		/* directory not found, no software to be searched for */
		return TRUE;
	}

	/* get stuff in the software-runtime directory */
	fdir =  g_file_new_for_path (runtime_root);
	enumerator = g_file_enumerate_children (fdir, G_FILE_ATTRIBUTE_STANDARD_NAME, 0, NULL, &tmp_error);
	if (tmp_error != NULL)
		goto out;

	while ((file_info = g_file_enumerator_next_file (enumerator, NULL, &tmp_error)) != NULL) {
		gchar *path;
		if (tmp_error != NULL)
			goto out;

		if (g_file_info_get_is_hidden (file_info))
			continue;
		path = g_build_filename (runtime_root,
								 g_file_info_get_name (file_info),
								 "control",
								 NULL);
		if (g_file_test (path, G_FILE_TEST_IS_REGULAR)) {
			gchar *rt_path;
			gboolean ret;
			LiRuntime *rt;

			rt_path = g_build_filename (runtime_root, g_file_info_get_name (file_info), NULL);

			rt = li_runtime_new ();
			ret = li_runtime_load_directory (rt, rt_path, &tmp_error);
			if (ret)
				g_ptr_array_add (priv->installed_rt, g_object_ref (rt));

			g_free (rt_path);
			g_object_unref (rt);
		}
		g_free (path);
	}


out:
	g_object_unref (fdir);
	if (enumerator != NULL)
		g_object_unref (enumerator);
	if (tmp_error != NULL) {
		g_printerr ("Error while searching for installed runtimes: %s\n", tmp_error->message);
		return FALSE;
	}

	return TRUE;
}

/**
 * li_manager_get_installed_runtimes:
 *
 * Returns: (transfer none) (element-type LiRuntime): A list of registered runtimes
 **/
GPtrArray*
li_manager_get_installed_runtimes (LiManager *mgr)
{
	LiManagerPrivate *priv = GET_PRIVATE (mgr);

	if (priv->installed_rt->len == 0) {
		/* in case no runtime was found or we never searched for it, we
		 * do this again
		 */
		li_manager_find_installed_runtimes (mgr);
	}

	return priv->installed_rt;
}

/**
 * li_manager_find_runtime_with_members:
 * @mgr: An instance of #LiManager
 * @members: (element-type LiPkgInfo): Software components which should be present in the runtime
 *
 * Get an installed runtime which contains the specified members.
 * If none is available, %NULL is returned.
 * The resulting runtime needs to be unref'ed with g_object_unref()
 * if it is no longer needed.
 *
 * Returns: (transfer full): A #LiRuntime containing @members or %NULL
 */
LiRuntime*
li_manager_find_runtime_with_members (LiManager *mgr, GPtrArray *members)
{
	guint i, j, k;
	LiManagerPrivate *priv = GET_PRIVATE (mgr);

	/* ensure we have all installed runtimes cached */
	li_manager_get_installed_runtimes (mgr);

	/* NOTE: If we ever have more frameworks with more members, we need a more efficient implementation here */
	for (i = 0; i < priv->installed_rt->len; i++) {
		GPtrArray *test_members;
		gboolean ret = FALSE;
		LiRuntime *rt = LI_RUNTIME (g_ptr_array_index (priv->installed_rt, i));

		test_members = li_runtime_get_members (rt);
		for (j = 0; j < members->len; j++) {
			const gchar *pkid;
			LiPkgInfo *pki = LI_PKG_INFO (g_ptr_array_index (members, j));
			pkid = li_pkg_info_get_id (pki);

			for (k = 0; k < test_members->len; k++) {
				const gchar *member_id = (const gchar *) g_ptr_array_index (test_members, k);
				if (g_strcmp0 (pkid, member_id) == 0) {
					ret = TRUE;
					break;
				}
			}
			if (!ret)
				break;
		}

		if (ret)
			return g_object_ref (rt);
	}

	return NULL;
}

/**
 * li_manager_remove_exported_files:
 */
static void
li_manager_remove_exported_files (GFile *file, GError **error)
{
	gchar *line = NULL;
	GFileInputStream* ir;
	GDataInputStream* dis;
	gint res;

	ir = g_file_read (file, NULL, NULL);
	dis = g_data_input_stream_new ((GInputStream*) ir);
	g_object_unref (ir);

	while (TRUE) {
		gchar **parts;
		line = g_data_input_stream_read_line (dis, NULL, NULL, NULL);
		if (line == NULL) {
			break;
		}

		parts = g_strsplit (line, "\t", 2);
		if (parts[1] == NULL) {
			g_strfreev (parts);
			continue;
		}

		if (g_str_has_prefix (parts[1], "/")) {
			/* delete file */
			res = g_remove (parts[1]);
			if (res != 0) {
				g_set_error (error,
					LI_MANAGER_ERROR,
					LI_MANAGER_ERROR_REMOVE_FAILED,
					_("Could not delete file '%s'"), parts[1]);
				g_strfreev (parts);
				goto out;
			}
		}
		g_strfreev (parts);
	}

out:
	g_object_unref (dis);
}

/**
 * li_manager_remove_software:
 **/
gboolean
li_manager_remove_software (LiManager *mgr, const gchar *pkgid, GError **error)
{
	_cleanup_free_ gchar *swpath = NULL;
	GFile *expfile;
	gchar *tmp;
	GError *tmp_error = NULL;
	_cleanup_ptrarray_unref_ GPtrArray *pkg_array = NULL;
	GFile *ctlfile;
	LiPkgInfo *pki;
	LiRuntime *rt;

	swpath = g_build_filename (LI_SOFTWARE_ROOT, pkgid, NULL);

	tmp = g_build_filename (swpath, "control", NULL);
	ctlfile = g_file_new_for_path (tmp);
	g_free (tmp);
	if (!g_file_query_exists (ctlfile, NULL)) {
		g_object_unref (ctlfile);
		g_set_error (error,
					LI_MANAGER_ERROR,
					LI_MANAGER_ERROR_NOT_FOUND,
					_("Could not find software: %s"), pkgid);
		return FALSE;
	}
	pki = li_pkg_info_new ();
	li_pkg_info_load_file (pki, ctlfile);
	g_object_unref (ctlfile);

	/* test if a runtime uses this software */
	pkg_array = g_ptr_array_new_with_free_func (g_object_unref);
	g_ptr_array_add (pkg_array, pki);
	rt = li_manager_find_runtime_with_members (mgr, pkg_array);
	if (rt != NULL) {
		GPtrArray *sw;
		guint i;
		gboolean dependency_found = FALSE;
		/* this software is in use somewhere */
		sw = li_manager_get_installed_software (mgr);
		for (i = 0; i < sw->len; i++) {
			LiPkgInfo *pki2 = LI_PKG_INFO (g_ptr_array_index (sw, i));

			if (g_strcmp0 (li_pkg_info_get_runtime_dependency (pki2), li_runtime_get_uuid (rt)) == 0) {
				/* TODO: Emit broken packages here, don't misuse GError */
				g_set_error (error,
					LI_MANAGER_ERROR,
					LI_MANAGER_ERROR_DEPENDENCY,
						_("Removing  '%s' would break at least '%s' as well."), pkgid, li_pkg_info_get_name (pki2));
				dependency_found = TRUE;
				break;
			}
		}
		if (dependency_found) {
			g_object_unref (rt);
			return FALSE;
		} else {
			/* apparently nothing uses this runtime anymore - remove it */
			li_runtime_remove (rt);
			g_debug ("Removed runtime: %s", li_runtime_get_uuid (rt));
		}
		g_object_unref (rt);
	}

	/* remove exported files */
	tmp = g_build_filename (swpath, "exported", NULL);
	expfile = g_file_new_for_path (tmp);
	g_free (tmp);
	if (g_file_query_exists (expfile, NULL)) {
		li_manager_remove_exported_files (expfile, &tmp_error);
	}
	g_object_unref (expfile);
	if (tmp_error != NULL) {
		g_propagate_error (error, tmp_error);
		return FALSE;
	}

	/* now delete the directory */
	if (!li_delete_dir_recursive (swpath)) {
		g_set_error (error,
					LI_MANAGER_ERROR,
					LI_MANAGER_ERROR_REMOVE_FAILED,
					_("Could not remove software directory."));
		return FALSE;
	}

	g_debug ("Removed package: %s", pkgid);

	/* we need to recreate the caches, now that the installed software has changed */
	li_manager_reset_cached_data (mgr);

	return TRUE;
}

/**
 * li_manager_error_quark:
 *
 * Return value: An error quark.
 **/
GQuark
li_manager_error_quark (void)
{
	static GQuark quark = 0;
	if (!quark)
		quark = g_quark_from_static_string ("LiManagerError");
	return quark;
}

/**
 * li_manager_class_init:
 **/
static void
li_manager_class_init (LiManagerClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = li_manager_finalize;
}

/**
 * li_manager_new:
 *
 * Creates a new #LiManager.
 *
 * Returns: (transfer full): a #LiManager
 *
 **/
LiManager *
li_manager_new (void)
{
	LiManager *mgr;
	mgr = g_object_new (LI_TYPE_MANAGER, NULL);
	return LI_MANAGER (mgr);
}
