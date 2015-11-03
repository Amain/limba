/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2014-2015 Matthias Klumpp <matthias@tenstral.net>
 * Copyright (C)      2012 Alexander Larsson <alexl@redhat.com>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the license, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE /* Required for CLONE_NEWNS */
#include <config.h>
#include <limba.h>
#include <li-config-data.h>
#include "li-utils-private.h"

#include <sys/mount.h>
#include <sched.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <linux/version.h>
#include <sys/utsname.h>
#include <sys/capability.h>
#include <sys/prctl.h>

#include <gio/gio.h>

#define REQUIRED_CAPS (CAP_TO_MASK(CAP_SYS_ADMIN))

/**
 * Ensure we have just the capabilities we need
 */
static gboolean
acquire_caps (void)
{
	struct __user_cap_header_struct hdr;
	struct __user_cap_data_struct data;

	if (getuid () != geteuid ()) {
		/* Tell kernel not clear capabilities when dropping root */
		if (prctl (PR_SET_KEEPCAPS, 1, 0, 0, 0) < 0) {
			g_printerr ("prctl(PR_SET_KEEPCAPS) failed\n");
			return FALSE;
		}

		/* Drop root uid, but retain the required permitted caps */
		if (setuid (getuid ()) < 0) {
			g_printerr ("unable to drop privileges\n");
			return FALSE;
		}
	}

	memset (&hdr, 0, sizeof(hdr));
	hdr.version = _LINUX_CAPABILITY_VERSION;

	/* Drop all non-require capabilities */
	data.effective = REQUIRED_CAPS;
	data.permitted = REQUIRED_CAPS;
	data.inheritable = 0;
	if (capset (&hdr, &data) < 0) {
		g_printerr ("capset failed\n");
		return FALSE;
	}

	return TRUE;
}

/**
 * drop_caps:
 *
 * Drop all remaining capabilities we have.
 */
static gboolean
drop_caps (void)
{
	struct __user_cap_header_struct hdr;
	struct __user_cap_data_struct data;

	memset (&hdr, 0, sizeof(hdr));
	hdr.version = _LINUX_CAPABILITY_VERSION;
	data.effective = 0;
	data.permitted = 0;
	data.inheritable = 0;

	if (capset (&hdr, &data) < 0) {
		g_printerr ("capset failed\n");
		return FALSE;
	}

	return TRUE;
}

/**
 * create_mount_namespace:
 */
static int
create_mount_namespace (void)
{
	int mount_count;
	int res;

	g_debug ("creating new namespace");

	res = unshare (CLONE_NEWNS);
	if (res != 0) {
		g_print ("Failed to create new namespace: %s\n", strerror(errno));
		return 1;
	}

	g_debug ("mount (private)");
	mount_count = 0;
	res = mount (LI_SW_ROOT_PREFIX, LI_SW_ROOT_PREFIX,
				 NULL, MS_PRIVATE, NULL);
	if (res != 0 && errno == EINVAL) {
		/* Maybe if failed because there is no mount
		 * to be made private at that point, lets
		 * add a bind mount there. */
		g_debug (("mount (bind)\n"));
		res = mount (LI_SW_ROOT_PREFIX, LI_SW_ROOT_PREFIX,
					 NULL, MS_BIND, NULL);
		/* And try again */
		if (res == 0) {
			mount_count++; /* Bind mount succeeded */
			g_debug ("mount (private)");
			res = mount (LI_SW_ROOT_PREFIX, LI_SW_ROOT_PREFIX,
						 NULL, MS_PRIVATE, NULL);
		}
	}

	if (res != 0) {
		g_error ("Failed to make prefix namespace private");
		goto error_out;
	}

	return 0;

error_out:
	while (mount_count-- > 0)
		umount (LI_SW_ROOT_PREFIX);
	return 1;
}

/**
 * mount_overlay:
 * @pkgid: A software identifier (name-version)
 */
static int
mount_overlay (const gchar *pkgid)
{
	int res = 0;
	gchar *main_data_path = NULL;
	gchar *fname = NULL;
	gchar *wdir = NULL;
	GFile *file;
	const gchar *runtime_uuid;
	gchar *tmp;
	GError *error = NULL;
	LiPkgInfo *pki = NULL;

	/* check if the software exists */
	main_data_path = g_build_filename (LI_SOFTWARE_ROOT, pkgid, "data", NULL);
	fname = g_build_filename (LI_SOFTWARE_ROOT, pkgid, "control", NULL);
	file = g_file_new_for_path (fname);
	g_free (fname);

	if (!g_file_query_exists (file, NULL)) {
		fprintf (stderr, "The software '%s' does not exist.\n", pkgid);
		res = 1;
		g_object_unref (file);
		goto out;
	}

	pki = li_pkg_info_new ();
	li_pkg_info_load_file (pki, file, &error);
	g_object_unref (file);
	if (error != NULL) {
		fprintf (stderr, "Unable to read software metadata. %s\n", error->message);
		goto out;
	}

	runtime_uuid = li_pkg_info_get_runtime_dependency (pki);
	if (runtime_uuid == NULL) {
		fprintf (stderr, "Sorry, I can not construct a new runtime environment for this application. Please do that manually!\n");
		res = 3;
		goto out;
	}

	wdir = g_build_filename (LI_SOFTWARE_ROOT, "runtimes", "ofs_work", NULL);
	res = g_mkdir_with_parents (wdir, 0775);
	if (res != 0) {
		fprintf (stderr, "Unable to create OverlayFS workdir. %s\n", strerror (errno));
		res = 1;
		goto out;
	}

	if (g_strcmp0 (runtime_uuid, "None") != 0) {
		/* mount the desired runtime */
		LiRuntime *rt;
		gchar **rt_members;
		GString *mergedirs_str;
		guint i;

		rt = li_runtime_new ();
		li_runtime_load_by_uuid (rt, runtime_uuid, &error);
		if (error != NULL) {
			fprintf (stderr, "Unable to load runtime '%s': %s\n", runtime_uuid, error->message);
			res = 1;
			goto out;
		}

		rt_members = (gchar**) g_hash_table_get_keys_as_array (li_runtime_get_members (rt), NULL);

		/* build our lowerdir directive */
		mergedirs_str = g_string_new ("");
		for (i = 0; rt_members[i] != NULL; i++) {
			g_string_append_printf (mergedirs_str, "%s/%s/data:", LI_SOFTWARE_ROOT, rt_members[i]);
		}

		/* cleanup */
		g_free (rt_members);
		g_object_unref (rt);

		/* safeguard againt the case where only one path is set for lowerdir.
		 * OFS doesn't like that, so we always set the root path as source too. */
		g_string_append_printf (mergedirs_str, "%s", LI_SW_ROOT_PREFIX);

		tmp = g_strdup_printf ("lowerdir=%s", mergedirs_str->str);
		res = mount ("overlay", LI_SW_ROOT_PREFIX,
					"overlay", MS_MGC_VAL | MS_RDONLY | MS_NOSUID, tmp);

		g_free (tmp);
		g_string_free (mergedirs_str, TRUE);

		if (res != 0) {
			fprintf (stderr, "Unable to mount runtime directory. %s\n", strerror (errno));
			res = 1;
			goto out;
		}
	}

	/* TODO: We don't need a workdir/upperdir here - adjust that later when the sandbox is implemented */
	tmp = g_strdup_printf ("lowerdir=%s,upperdir=%s,workdir=%s", LI_SW_ROOT_PREFIX, main_data_path, wdir);
	res = mount ("overlay", LI_SW_ROOT_PREFIX,
				 "overlay", MS_MGC_VAL | MS_RDONLY | MS_NOSUID, tmp);

	g_free (tmp);
	if (res != 0) {
		fprintf (stderr, "Unable to mount directory. %s\n", strerror (errno));
		res = 1;
		goto out;
	}

out:
	if (main_data_path != NULL)
		g_free (main_data_path);
	if (pki != NULL)
		g_object_unref (pki);
	if (wdir != NULL)
		g_free (wdir);
	if (error != NULL)
		g_error_free (error);

	return res;
}

/**
 * update_env_var_list:
 */
static void
update_env_var_list (const gchar *var, const gchar *item)
{
	const gchar *env;
	gchar *value;

	env = getenv (var);
	if (env == NULL || *env == 0) {
		setenv (var, item, 1);
	} else {
		value = g_strconcat (item, ":", env, NULL);
		setenv (var, value, 1);
		free (value);
	}
}

/**
 * main:
 */
int
main (gint argc, gchar *argv[])
{
	int ret;
	g_autofree gchar *swname = NULL;
	g_autofree gchar *scope_name = NULL;
	g_autofree gchar *executable = NULL;
	struct utsname uts_data;
	gchar *ma_lib_path = NULL;
	gchar *tmp;
	gchar **strv;
	gchar **child_argv = NULL;
	guint i;
	GError *error = NULL;

	/* ensue we have required capabilities, and drop all the ones we don't need */
	if (!acquire_caps ()) {
		g_printerr ("This program needs the suid bit to be set to function correctly.\n");
		return 3;
	}

	if (argc <= 1) {
		fprintf (stderr, "No application-id was specified.\n");
		return 1;
	}

	strv = g_strsplit (argv[1], ":", 2);
	if (g_strv_length (strv) != 2) {
		g_strfreev (strv);
		fprintf (stderr, "No installed software with that name or executable found.\n");
		ret = 1;
		goto error;
	}

	uname (&uts_data);
	/* we need at least Linux 4.0 for Limba to work properly */
	if (li_compare_versions ("4.0", uts_data.release) > 0)
		g_warning ("Running on Linux %s. Runapp needs at least Linux 4.0 to be sure all needed features are present.", uts_data.release);

	swname = g_strdup (strv[0]);
	executable = g_build_filename (LI_SW_ROOT_PREFIX, strv[1], NULL);
	g_strfreev (strv);

	ret = create_mount_namespace ();
	if (ret > 0)
		goto error;

	ret = mount_overlay (swname);
	if (ret > 0)
		goto error;

	/* Now we have everything we need CAP_SYS_ADMIN for, so drop that capability */
	if (!drop_caps ()) {
		g_printerr ("Unable to drop capabilities.\n");
		return 3;
	}

	/* place this process in a new cgroup */
	scope_name = li_str_replace (swname, "/", "");
	li_add_to_new_scope ("app", scope_name, &error);
	if (error != NULL) {
		fprintf (stderr, "Could not add process to new scope. %s\n", error->message);
		g_error_free (error);
		goto error;
	}

	/* add generic library path */
	update_env_var_list ("LD_LIBRARY_PATH", LI_SW_ROOT_PREFIX "/lib");

	/* add multiarch library path for compatibility reasons */
	tmp = li_get_arch_triplet ();
	ma_lib_path = g_build_filename (LI_SW_ROOT_PREFIX, "lib", tmp, NULL);
	g_free (tmp);
	update_env_var_list ("LD_LIBRARY_PATH", ma_lib_path);
	g_free (ma_lib_path);

	/* add generic binary directory to PATH */
	update_env_var_list ("PATH", LI_SW_ROOT_PREFIX "/bin");

	child_argv = malloc ((argc) * sizeof (char *));
	if (child_argv == NULL) {
		ret = FALSE;
		fprintf (stderr, "Out of memory!\n");
		goto error;
	}

	if (!g_file_test (executable, G_FILE_TEST_EXISTS)) {
		ret = FALSE;
		fprintf (stderr, "Executable '%s' was not found.\n", executable);
		goto error;
	}

	/* give absolute executable path as argv[0] */
	child_argv[0] = executable;

	for (i = 1; i < argc - 1; i++) {
		child_argv[i] = argv[i+1];
	}
	child_argv[i++] = NULL;

	return execv (executable, child_argv);

error:
	return ret;
}
