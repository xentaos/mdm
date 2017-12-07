/* MDM - The MDM Display Manager
 * Copyright (C) 1999, 2000 Martin K. Petersen <mkp@mkp.net>
 *
 * This file Copyright (c) 2003 George Lebl
 * - Common routines for the greeters.
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

#include "misc.h"

#ifndef MDM_USER_H
#define MDM_USER_H

typedef struct _MdmUser MdmUser;
struct _MdmUser {
    uid_t uid;
    char *login;
    char *homedir;
    char *gecos;
    GdkPixbuf *picture;
};

gboolean    mdm_is_user_valid		(const char *username);
gint        mdm_user_uid                (const char *username);
const char *get_root_user               (void);
void        mdm_users_init              (GList **users, GList **users_string,
					char *exclude_user, GdkPixbuf *defface,
					int *size_of_users, gboolean is_local,
					gboolean read_faces);

#endif /* MDM_USER_H */
