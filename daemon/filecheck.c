/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * MDM - The MDM Display Manager
 * Copyright (C) 1998, 1999, 2000 Martin K. Petersen <mkp@mkp.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "config.h"

#include <glib/gi18n.h>
#include <sys/stat.h>

#include "mdm.h"
#include "mdm-common.h"
#include "mdm-daemon-config.h"
#include "mdm-log.h"

#include "filecheck.h"

/**
 * mdm_file_check:
 * @caller: String to be prepended to error messages.
 * @user: User id for the user owning the file/dir.
 * @dir: Directory to be examined.
 * @file: File to be examined.
 * @absentok: Accept absent files if TRUE.
 * @absentdirok: Absent directory returns FALSE but without complaining
 * @maxsize: Maximum acceptable filesize in KB. 0 to disable.
 * @perms: 0 to allow user writable file/dir only. 1 to allow group and 2 to allow global writable file/dir.
 *
 * Examines a file to determine whether it is safe for the daemon to write to it.
 */

/* we should be euid the user BTW */
gboolean
mdm_file_check (const gchar *caller,
                uid_t user,
                const gchar *dir,
		const gchar *file,
                gboolean absentok,
		gboolean absentdirok,
                gint maxsize,
                gint perms)
{
	struct stat statbuf;
	gchar *fullpath;
	gchar *dirautofs;
	int r;

	if (ve_string_empty (dir) ||
	    ve_string_empty (file))
		return FALSE;

	/* Stat on automounted directory - append the '/.' to dereference mount point.
	   Do this only if MdmSupportAutomount is true (default is false)
	   2006-09-22, Jerzy Borkowski, CAMK */
	if G_UNLIKELY (mdm_daemon_config_get_value_bool (MDM_KEY_SUPPORT_AUTOMOUNT)) {
		dirautofs = g_strconcat(dir, "/.", NULL);
		VE_IGNORE_EINTR (r = stat (dirautofs, &statbuf));
		g_free(dirautofs);
	}
	/* Stat directory */
	else {
		VE_IGNORE_EINTR (r = stat (dir, &statbuf));
	}

	if (r < 0) {
		if ( ! absentdirok)
			mdm_debug ("%s: Directory %s does not exist.",
				   caller, dir);
		return FALSE;
	}

	/* Check if dir is owned by the user ...
	   Only, if MDM_KEY_CHECK_DIR_OWNER is true (default)
	   This is a "hack" for directories not owned by
	   the user.
	   2004-06-22, Andreas Schubert, MATHEMA Software GmbH */

	if G_UNLIKELY (mdm_daemon_config_get_value_bool (MDM_KEY_CHECK_DIR_OWNER) && (statbuf.st_uid != user)) {
		mdm_debug ("%s: %s is not owned by uid %d.", caller, dir, user);
		return FALSE;
	}

	/* ... if group has write permission ... */
	if G_UNLIKELY (perms < 1 && (statbuf.st_mode & S_IWGRP) == S_IWGRP) {
		mdm_debug ("%s: %s is writable by group.", caller, dir);
		return FALSE;
	}

	/* ... and if others have write permission. */
	if G_UNLIKELY (perms < 2 && (statbuf.st_mode & S_IWOTH) == S_IWOTH) {
		mdm_debug ("%s: %s is writable by other.", caller, dir);
		return FALSE;
	}

	fullpath = g_build_filename (dir, file, NULL);

	/* Stat file */
	VE_IGNORE_EINTR (r = g_stat (fullpath, &statbuf));
	if (r < 0) {
		/* Return true if file does not exist and that is ok */
		if (absentok) {
			g_free (fullpath);
			return TRUE;
		}
		else {
			mdm_debug ("%s: %s does not exist but must exist.", caller, fullpath);
			g_free (fullpath);
			return FALSE;
		}
	}

	/* Check that it is a regular file ... */
	if G_UNLIKELY (! S_ISREG (statbuf.st_mode)) {
		mdm_debug ("%s: %s is not a regular file.", caller, fullpath);
		g_free (fullpath);
		return FALSE;
	}

	/* ... owned by the user ... */
	if G_UNLIKELY (statbuf.st_uid != user) {
		mdm_debug ("%s: %s is not owned by uid %d.", caller, fullpath, user);
		g_free (fullpath);
		return FALSE;
	}

	/* ... unwritable by group ... */
	if G_UNLIKELY (perms < 1 && (statbuf.st_mode & S_IWGRP) == S_IWGRP) {
		mdm_debug ("%s: %s is writable by group.", caller, fullpath);
		g_free (fullpath);
		return FALSE;
	}

	/* ... unwritable by others ... */
	if G_UNLIKELY (perms < 2 && (statbuf.st_mode & S_IWOTH) == S_IWOTH) {
		mdm_debug ("%s: %s is writable by group/other.", caller, fullpath);
		g_free (fullpath);
		return FALSE;
	}

	/* ... and smaller than sysadmin specified limit. */
	if G_UNLIKELY (maxsize && statbuf.st_size > maxsize) {
		mdm_debug ("%s: %s is bigger than sysadmin specified maximum file size.",
			   caller, fullpath);
		g_free (fullpath);
		return FALSE;
	}

	g_free (fullpath);

	/* Yeap, this file is ok */
	return TRUE;
}

/* we should be euid the user BTW */
gboolean
mdm_auth_file_check (const gchar *caller,
                     uid_t user,
                     const gchar *authfile,
                     gboolean absentok,
                     struct stat *s)
{
	struct stat statbuf;
	gint usermaxfile;
	int r;

	if (ve_string_empty (authfile))
		return FALSE;

	/* Stat file */
	VE_IGNORE_EINTR (r = g_lstat (authfile, &statbuf));
	if (s != NULL)
		*s = statbuf;
	if (r < 0) {
		if (absentok)
			return TRUE;
		mdm_debug ("%s: %s does not exist but must exist.", caller, authfile);
		return FALSE;
	}

	/* Check that it is a regular file ... */
	if G_UNLIKELY (! S_ISREG (statbuf.st_mode)) {
		mdm_debug ("%s: %s is not a regular file.", caller, authfile);
		return FALSE;
	}

	/* ... owned by the user ... */
	if G_UNLIKELY (statbuf.st_uid != user) {
		mdm_debug ("%s: %s is not owned by uid %d.", caller, authfile, user);
		return FALSE;
	}

	/* ... has right permissions ... */
	if G_UNLIKELY (statbuf.st_mode & 0077) {
		mdm_debug ("%s: %s has wrong permissions (should be 0600)", caller, authfile);
		return FALSE;
	}

	usermaxfile = mdm_daemon_config_get_value_int (MDM_KEY_USER_MAX_FILE);
	/* ... and smaller than sysadmin specified limit. */
	if G_UNLIKELY (usermaxfile && statbuf.st_size > usermaxfile) {
		mdm_debug ("%s: %s is bigger than sysadmin specified maximum file size.",
			caller, authfile);
		return FALSE;
	}

	/* Yeap, this file is ok */
	return TRUE;
}
