/* MDM - The MDM Display Manager
 * Copyright (C) 1998, 1999, 2000 Martin K. Petersen <mkp@mkp.net>
 *
 * This file Copyright (c) 2001 George Lebl
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

#ifndef MDM_LANGUAGES_H
#define MDM_LANGUAGES_H

#include <glib.h>

enum {
  LOCALE_COLUMN,
  TRANSLATED_NAME_COLUMN,
  UNTRANSLATED_NAME_COLUMN,
  NUM_COLUMNS
};

enum {
  ASK_RESTART = 0,
  ALWAYS_RESTART
};

/* This is the interface for translating languages.  Language translations
 * are now hardocded in, but that may change */

const char *	mdm_lang_group1		(void);
const char *	mdm_lang_group2		(void);

char *		mdm_lang_name		(const char *language,
					 gboolean never_encoding,
					 gboolean no_group,
					 gboolean untranslated,
					 gboolean markup);
gboolean	mdm_lang_name_translated (const char *language);

/* NULL if not found */
char *		mdm_lang_untranslated_name (const char *language,
					    gboolean markup);

GList *		mdm_lang_read_locale_file (const char *file);

GtkListStore *	mdm_lang_get_model		(void);
void		mdm_lang_initialize_model	(gchar *locale_file);
gchar *		mdm_lang_check_language		(const char      *old_language);
void		mdm_lang_set			(char *language);
void            mdm_lang_set_restart_dialog     (char *language);
gint		mdm_lang_get_savelang_setting	(void);
void		mdm_lang_handler		(gpointer user_data);
void		mdm_lang_op_lang		(const gchar *args);
void		mdm_lang_op_slang		(const gchar *args);
void		mdm_lang_op_setlang		(const gchar *args);
void		mdm_lang_op_always_restart	(const gchar *args);
gint            mdm_lang_ask_restart            (gchar *language);

#endif /* MDM_LANGUAGES_H */
