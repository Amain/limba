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
 * SECTION:li-installer
 * @short_description: High level installation of IPK packages
 */

#include "config.h"
#include "li-installer.h"

#include <glib/gi18n-lib.h>

#include "li-pkg-info.h"
#include "li-manager.h"
#include "li-runtime.h"

typedef struct _LiInstallerPrivate	LiInstallerPrivate;
struct _LiInstallerPrivate
{
	LiManager *mgr;
	GNode *pkgs;
	LiPkgInfo *pki;
};

G_DEFINE_TYPE_WITH_PRIVATE (LiInstaller, li_installer, G_TYPE_OBJECT)

#define GET_PRIVATE(o) (li_installer_get_instance_private (o))

static void li_installer_check_dependencies (LiInstaller *inst, GNode *root, GError **error);

/**
 * The package/dependency tree contains all packages the current to-be-installed
 * software depends on.
 *
 * Nodes have a #LiPackage as data, in case the package still needs to be installed.
 * Otherwise, a #LiPkgInfo instance is present instead.
 */

/**
 * _li_package_tree_free_node:
 */
static gboolean
_li_package_tree_free_node (GNode *node, gpointer data)
{
	if (node->data != NULL)
		g_object_unref (node->data);

	return FALSE;
}

/**
 * li_package_tree_teardown:
 */
static void
li_package_tree_teardown (GNode *root)
{
	g_node_traverse (root,
					G_IN_ORDER,
					G_TRAVERSE_ALL,
					-1,
					_li_package_tree_free_node,
					NULL);
	g_node_destroy (root);
}

/**
 * li_package_tree_add_package:
 * @pkg: The package required to be installed
 *
 * Returns: A reference to the new node
 */
static GNode*
li_package_tree_add_package (GNode *parent, LiPackage *pkg)
{
	GNode *node;

	node = g_node_new (g_object_ref (pkg));
	if (parent != NULL)
		g_node_append (parent, node);
	g_debug ("Package %s marked for installation.", li_package_get_id (pkg));

	return node;
}

/**
 * li_package_tree_add_package_info:
 * @pki: The information about an installed package
 *
 * Returns: A reference to the new node
 */
static GNode*
li_package_tree_add_package_info (GNode *parent, LiPkgInfo *pki)
{
	GNode *node;

	node = g_node_new (g_object_ref (pki));
	if (parent != NULL)
		g_node_append (parent, node);
	g_debug ("Component %s-%s found.", li_pkg_info_get_name (pki), li_pkg_info_get_version (pki));

	return node;
}

/**
 * _li_package_tree_add_pki_to_array:
 */
static gboolean
_li_package_tree_add_pki_to_array (GNode *node, gpointer data)
{
	LiPackage *pkg;
	GPtrArray *array = (GPtrArray*) data;

	if (LI_IS_PKG_INFO (node->data)) {
		g_ptr_array_add (array, node->data);
	} else {
		pkg = LI_PACKAGE (node->data);
		g_ptr_array_add (array, li_package_get_info (pkg));
	}

	return FALSE;
}

/**
 * li_package_tree_branch_to_array:
 *
 * Get an array of #LiPkgInfo objects this node depends on.
 */
static GPtrArray*
li_package_tree_branch_to_array (GNode *root)
{
	GPtrArray *array;

	if (root->children == NULL)
		return NULL;

	array = g_ptr_array_new ();
	g_node_traverse (root,
					G_PRE_ORDER,
					G_TRAVERSE_ALL,
					-1,
					_li_package_tree_add_pki_to_array,
					array);

	/* remove the root node value from the array */
	if (array->len > 0)
		g_ptr_array_remove_index (array, 0);

	/* return null in case the array is empty */
	if (array->len == 0) {
		g_ptr_array_unref (array);
		return NULL;
	}

	return array;
}

/**
 * li_package_tree_reset:
 *
 * Remove all nodes from the tree, except for the root node.
 */
static void
li_package_tree_reset (GNode *root)
{
	GNode *child = root->children;
	if (child == NULL)
		return;

	while (child->next != NULL)
		li_package_tree_teardown (child->next);
}

/**
 * li_installer_finalize:
 **/
static void
li_installer_finalize (GObject *object)
{
	LiInstaller *inst = LI_INSTALLER (object);
	LiInstallerPrivate *priv = GET_PRIVATE (inst);

	g_object_unref (priv->mgr);
	li_package_tree_teardown (priv->pkgs);

	G_OBJECT_CLASS (li_installer_parent_class)->finalize (object);
}

/**
 * li_installer_init:
 **/
static void
li_installer_init (LiInstaller *inst)
{
	LiInstallerPrivate *priv = GET_PRIVATE (inst);

	priv->mgr = li_manager_new ();
	priv->pkgs = g_node_new (NULL);
}

/**
 * li_installer_parse_dependency_string:
 */
static GPtrArray*
li_installer_parse_dependency_string (const gchar *depstr)
{
	guint i;
	gchar **slices;
	GPtrArray *array;

	if (depstr == NULL)
		return NULL;

	array = g_ptr_array_new_with_free_func (g_object_unref);
	slices = g_strsplit (depstr, ",", -1);

	for (i = 0; slices[i] != NULL; i++) {
		gchar *dep_raw;
		//gchar *dep_version;
		LiPkgInfo *ipkc;

		g_strstrip (slices[i]);
		dep_raw = slices[i];

		ipkc = li_pkg_info_new ();
		if (g_strrstr (dep_raw, "(") != NULL) {
			gchar **strv;
			//gchar *ver_tmp;

			strv = g_strsplit (dep_raw, "(", 2);
			g_strstrip (strv[0]);

			li_pkg_info_set_name (ipkc, strv[0]);
			//ver_tmp = strv[1];
			//g_strstrip (ver_tmp);


			// TODO: Extract version and relation (>>, >=, <=, ==, <<)
		} else {
			li_pkg_info_set_name (ipkc, dep_raw);
		}

		g_ptr_array_add (array, ipkc);
	}

	return array;
}

/**
 * li_installed_dep_is_installed:
 */
static LiPkgInfo*
li_installer_find_satisfying_pkg (GPtrArray *pkglist, LiPkgInfo *dep)
{
	guint i;
	const gchar *dep_name;

	if (pkglist == NULL)
		return NULL;

	dep_name = li_pkg_info_get_name (dep);
	for (i = 0; i < pkglist->len; i++) {
		const gchar *str;
		LiPkgInfo *pki = LI_PKG_INFO (g_ptr_array_index (pkglist, i));

		str = li_pkg_info_get_name (pki);
		if (g_strcmp0 (dep_name, str) == 0) {
			/* update version of the dependency to match the one of the installed software */
			str = li_pkg_info_get_version (pki);
			li_pkg_info_set_version (dep, str);
			return pki;
		}

		// TODO: Check version as well

	}

	return NULL;
}

/**
 * li_installer_find_dependency_embedded:
 */
static gboolean
li_installer_find_dependency_embedded (LiInstaller *inst, GNode *root, LiPkgInfo *dep_pki, GError **error)
{
	LiPkgInfo *epki;
	LiPackage *epkg;
	GPtrArray *embedded;
	GError *tmp_error = NULL;
	GNode *node;
	LiPackage *pkg = LI_PACKAGE (root->data);

	embedded = li_package_get_embedded_packages (pkg);
	epki = li_installer_find_satisfying_pkg (embedded, dep_pki);
	if (epki == NULL) {
		g_set_error (error,
			LI_INSTALLER_ERROR,
			LI_INSTALLER_ERROR_DEPENDENCY_NOT_FOUND,
			_("Could not find dependency: %s"), li_pkg_info_get_name (dep_pki));
		return FALSE;
	}

	/* we have found a matching dependency! */
	epkg = li_package_extract_embedded_package (pkg, epki, &tmp_error);
	if (tmp_error != NULL) {
		g_propagate_error (error, tmp_error);
		return FALSE;
	}

	node = li_package_tree_add_package (root, epkg);

	/* check if we have the dependencies, or can install them */
	li_installer_check_dependencies (inst, node, &tmp_error);
	if (tmp_error != NULL) {
		g_propagate_error (error, tmp_error);
		return FALSE;
	}
	g_object_unref (epkg);

	return TRUE;
}

/**
 * li_installer_find_embedded_parent:
 *
 * This function checks for embedded dependencies in the parent package, which
 * might satisfy dependencies in the child.
 *
 * Returns: %TRUE if dependency was found and added to the graph
 */
static gboolean
li_installer_find_embedded_parent (LiInstaller *inst, GNode *pkinode, LiPkgInfo *dep)
{
	GNode *parent;

	if (pkinode == NULL)
		return FALSE;

	parent = pkinode->parent;
	/* parent needs to be non-null (in case we hit root)... */
	if (parent == NULL)
		return FALSE;
	/*... and a package node */
	if (!LI_IS_PACKAGE (parent))
		return FALSE;

	return li_installer_find_dependency_embedded (inst, parent, dep, NULL);
}

/**
 * li_installer_check_dependencies:
 */
static void
li_installer_check_dependencies (LiInstaller *inst, GNode *root, GError **error)
{
	guint i;
	GPtrArray *installed_sw;
	GError *tmp_error = NULL;
	LiPackage *pkg;
	LiPkgInfo *pki;
	GPtrArray *deps;
	LiInstallerPrivate *priv = GET_PRIVATE (inst);

	pkg = LI_PACKAGE (root->data);
	pki = li_package_get_info (pkg);
	deps = li_installer_parse_dependency_string (li_pkg_info_get_dependencies (pki));

	/* do we have dependencies at all? */
	if (deps == NULL)
		return;

	installed_sw = li_manager_get_installed_software (priv->mgr);
	for (i = 0; i < deps->len; i++) {
		LiPkgInfo *dep = LI_PKG_INFO (g_ptr_array_index (deps, i));

		/* test if this package is already in the installed set */
		if (li_installer_find_satisfying_pkg (installed_sw, dep) == NULL) {
			gboolean ret;
			/* check if the parent package has this dependency */
			ret = li_installer_find_embedded_parent (inst, root->parent, dep);
			if (ret)
				continue;

			/* maybe we find this dependency as embedded copy? */
			li_installer_find_dependency_embedded (inst, root, dep, &tmp_error);
			if (tmp_error != NULL) {
				g_propagate_error (error, tmp_error);
				return;
			}
		} else {
			/* dependency is already installed, add it as satisfied */
			li_package_tree_add_package_info (root, dep);
		}
	}

	g_ptr_array_unref (deps);
}

/**
 * li_installer_install_node:
 */
gboolean
li_installer_install_node (LiInstaller *inst, GNode *node, GError **error)
{
	GError *tmp_error = NULL;
	GNode *child;
	LiPackage *pkg;
	LiPkgInfo *info;
	GPtrArray *full_deps = NULL;
	gboolean ret = FALSE;
	LiInstallerPrivate *priv = GET_PRIVATE (inst);

	child = node->children;
	if (child != NULL) {
		while (TRUE) {
			li_installer_install_node (inst, child, &tmp_error);
			if (tmp_error != NULL) {
				g_propagate_error (error, tmp_error);
				goto out;
			}

			child = child->next;
			if (child == NULL)
				break;
		}
	}

	/* already installed nodes are not really interesting here */
	if (LI_IS_PKG_INFO (node->data))
		return TRUE;

	pkg = LI_PACKAGE (node->data);
	info = li_package_get_info (pkg);

	/* create runtime for this software */
	full_deps = li_package_tree_branch_to_array (node);
	if (full_deps != NULL) {
		LiRuntime *rt;

		/* now get the runtime-env id for the new application */
		rt = li_manager_find_runtime_with_members (priv->mgr, full_deps);
		if (rt == NULL) {
			/* no runtime was found, create a new one */
			rt = li_runtime_create_with_members (full_deps, &tmp_error);
			if ((tmp_error != NULL) || (rt == NULL)) {
				g_propagate_error (error, tmp_error);
				goto out;
			}
		}
		li_pkg_info_set_runtime_dependency (info,
										li_runtime_get_uuid (rt));
		g_object_unref (rt);
	} else {
		/* if the installed software does not need a runtime to run, we explicity state that */
		li_pkg_info_set_runtime_dependency (info, "None");
	}

	li_package_install (pkg, &tmp_error);
	if (tmp_error != NULL) {
		g_propagate_error (error, tmp_error);
		goto out;
	}
	g_debug ("Installed package: %s", li_package_get_id (pkg));

	ret = TRUE;
out:
	if (full_deps != NULL)
		g_ptr_array_unref (full_deps);

	return ret;
}

/**
 * li_installer_set_package:
 */
static void
li_installer_set_package (LiInstaller *inst, LiPackage *pkg)
{
	LiInstallerPrivate *priv = GET_PRIVATE (inst);

	/* remove the current package tree */
	li_package_tree_teardown (priv->pkgs);

	priv->pkgs = li_package_tree_add_package (NULL, pkg);
}

/**
 * li_installer_install:
 */
gboolean
li_installer_install (LiInstaller *inst, GError **error)
{
	gboolean ret = FALSE;
	GError *tmp_error = NULL;
	LiInstallerPrivate *priv = GET_PRIVATE (inst);

	if (priv->pkgs->data == NULL) {
		g_set_error (error,
			LI_INSTALLER_ERROR,
			LI_INSTALLER_ERROR_FAILED,
			_("No package is loaded."));
		return FALSE;
	}

	/* create a dependency tree for this package installation */
	/* NOTE: The root node is always the to-be-installed package (a LiPackage instance) */
	li_installer_check_dependencies (inst, priv->pkgs, &tmp_error);
	if (tmp_error != NULL) {
		g_propagate_error (error, tmp_error);
		goto out;
	}

	/* install the package tree */
	ret = li_installer_install_node (inst, priv->pkgs, &tmp_error);
	if (tmp_error != NULL) {
		g_propagate_error (error, tmp_error);
		goto out;
	}

out:
	/* teardown current dependency tree */
	li_package_tree_reset (priv->pkgs);

	return ret;
}

/**
 * li_installer_open_file:
 *
 * Open a package file
 */
gboolean
li_installer_open_file (LiInstaller *inst, const gchar *filename, GError **error)
{
	LiPackage *pkg;
	GError *tmp_error = NULL;

	pkg = li_package_new ();
	li_package_open_file (pkg, filename, &tmp_error);
	if (tmp_error != NULL) {
		g_propagate_error (error, tmp_error);
		g_object_unref (pkg);
		return FALSE;
	}
	li_installer_set_package (inst, pkg);
	g_object_unref (pkg);

	return TRUE;
}

/**
 * li_installer_get_package_info:
 *
 * Returns: (transfer none): The #LiPkgInfo of the to-be-installed package
 */
LiPkgInfo*
li_installer_get_package_info (LiInstaller *inst)
{
	LiInstallerPrivate *priv = GET_PRIVATE (inst);
	return priv->pki;
}

/**
 * li_installer_get_appstream_data:
 *
 * Dump of AppStream XML data describing the software which will be installed.
 *
 * Returns: (transfer full): AppStream XML data or %NULL, free with g_free()
 */
gchar*
li_installer_get_appstream_data (LiInstaller *inst)
{
	LiPackage *pkg;
	LiInstallerPrivate *priv = GET_PRIVATE (inst);

	pkg = priv->pkgs->data;
	if (pkg == NULL)
		return NULL;

	return li_package_get_appstream_data (pkg);
}
/**
 * li_installer_error_quark:
 *
 * Return value: An error quark.
 **/
GQuark
li_installer_error_quark (void)
{
	static GQuark quark = 0;
	if (!quark)
		quark = g_quark_from_static_string ("LiInstallerError");
	return quark;
}

/**
 * li_installer_class_init:
 **/
static void
li_installer_class_init (LiInstallerClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = li_installer_finalize;
}

/**
 * li_installer_new:
 *
 * Creates a new #LiInstaller.
 *
 * Returns: (transfer full): a #LiInstaller
 *
 **/
LiInstaller *
li_installer_new (void)
{
	LiInstaller *inst;
	inst = g_object_new (LI_TYPE_INSTALLER, NULL);
	return LI_INSTALLER (inst);
}
