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

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>

#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#ifdef HAVE_XFREE_XINERAMA
#include <X11/extensions/Xinerama.h>
#elif HAVE_SOLARIS_XINERAMA
#include <X11/extensions/xinerama.h>
#endif

#include "mdmwm.h"
#include "mdm.h"
#include "mdmcommon.h"
#include "mdmconfig.h"

#include "mdm-common.h"
#include "mdm-log.h"

typedef struct _MdmWindow MdmWindow;
struct _MdmWindow {
	int x, y;
	Window win;
	Window deco;
	Window shadow;
	gboolean ignore_size_hints; /* for mdm windows */
	gboolean center; /* do centering */
	gboolean recenter; /* do re-centering */
        gboolean takefocus; /* permit take focus */

	/* hack, when we reparent, we will get an unmap and then
	 * an map, and we want to ignore those */
	int ignore_next_map;
	int ignore_next_unmap;
};

static GList *windows = NULL;
static gboolean focus_new_windows = FALSE;
static int no_focus_login = 0;
static Display *wm_disp = NULL;
static Window wm_root = None;
static Window wm_login_window = None;
static Window wm_focus_window = None;

static Atom XA_WM_PROTOCOLS = 0;
static Atom XA_WM_STATE = 0;
static Atom XA_WM_TAKE_FOCUS = 0;
static Atom XA_COMPOUND_TEXT = 0;
static Atom XA_NET_WM_STRUT = 0;

static int trap_depth = 0;

GdkRectangle *mdm_wm_all_monitors = NULL;
int mdm_wm_num_monitors = 0;
GdkRectangle mdm_wm_screen = {0,0,0,0}; // This is the drawing area used by the greeter

static Window strut_owners[4] = {None, None, None, None};
static guint save_struts[4] = {0, 0, 0, 0};

void 
mdm_wm_screen_init (gchar * monitor_id)
{
	GdkScreen *screen;
	int i;

	if (g_getenv ("FAKE_XINERAMA_MDM") != NULL) {
	/* for testing Xinerama support on non-xinerama setups */
		mdm_wm_screen.x = 100;
		mdm_wm_screen.y = 100;
		mdm_wm_screen.width = gdk_screen_width () / 2 - 100;
		mdm_wm_screen.height = gdk_screen_height () / 2 - 100;

		mdm_wm_all_monitors = g_new0 (GdkRectangle, 2);
		mdm_wm_all_monitors[0] = mdm_wm_screen;
		mdm_wm_all_monitors[1].x = gdk_screen_width () / 2;
		mdm_wm_all_monitors[1].y = gdk_screen_height () / 2;
		mdm_wm_all_monitors[1].width = gdk_screen_width () / 2;
		mdm_wm_all_monitors[1].height = gdk_screen_height () / 2;
		mdm_wm_num_monitors = 2;
		return;
	}

	screen = gdk_screen_get_default ();

	mdm_wm_num_monitors = gdk_screen_get_n_monitors (screen);

	mdm_wm_all_monitors = g_new (GdkRectangle, mdm_wm_num_monitors);

	int current_monitor_num = gdk_screen_get_primary_monitor (screen);

	for (i = 0; i < mdm_wm_num_monitors; i++) {
		gdk_screen_get_monitor_geometry (screen, i, mdm_wm_all_monitors + i);
		gchar * plugname = gdk_screen_get_monitor_plug_name (screen, i);
		gchar * monitor_index = g_strdup_printf ("%d", i);
		mdm_debug("mdm_wm_screen_init: Found monitor #%s with plug name: '%s'.", monitor_index, plugname);
		if (g_strcmp0(plugname, monitor_id) == 0 || g_strcmp0(monitor_index, monitor_id) == 0) {
			current_monitor_num = i;
			mdm_debug("mdm_wm_screen_init: Using monitor '%s' to render greeter.", monitor_id);
		}
		g_free (plugname);
		g_free (monitor_index);
	}

	mdm_wm_screen = mdm_wm_all_monitors[current_monitor_num];
}

/* Not really a WM function, center a gtk window by setting uposition */
void
mdm_wm_center_window (GtkWindow *cw) 
{
        gint x, y;
        gint w, h;

	gtk_window_get_size (cw, &w, &h); 

	x = mdm_wm_screen.x + (mdm_wm_screen.width - w)/2;
	y = mdm_wm_screen.y + (mdm_wm_screen.height - h)/2;	

	if (x < mdm_wm_screen.x)
		x = mdm_wm_screen.x;
	if (y < mdm_wm_screen.y)
		y = mdm_wm_screen.y;

 	gtk_window_move (GTK_WINDOW (cw), x, y);	
}

void
mdm_wm_center_cursor (void)
{
    XWarpPointer (wm_disp, None, wm_root, 0, 0, 0, 0, mdm_wm_screen.x + mdm_wm_screen.width / 2, mdm_wm_screen.y + mdm_wm_screen.height / 2 + 20);
}

static void
trap_push (void)
{
	trap_depth++;
	gdk_error_trap_push ();
}

static int
trap_pop (void)
{
	trap_depth --;
	if (trap_depth <= 0)
		XSync (wm_disp, False);
	return gdk_error_trap_pop ();
}

/* stolen from gwmh */
static gpointer
get_typed_property_data (Display *xdisplay,
			 Window   xwindow,
			 Atom     property,
			 Atom     requested_type,
			 gint    *size_p,
			 guint    expected_format)
{
  static const guint prop_buffer_lengh = 1024 * 1024;
  unsigned char *prop_data = NULL;
  Atom type_returned = 0;
  unsigned long nitems_return = 0, bytes_after_return = 0;
  int format_returned = 0;
  gpointer data = NULL;
  gboolean abort = FALSE;

  g_return_val_if_fail (size_p != NULL, NULL);
  *size_p = 0;

  gdk_error_trap_push ();

  abort = XGetWindowProperty (xdisplay,
			      xwindow,
			      property,
			      0, prop_buffer_lengh,
			      False,
			      requested_type,
			      &type_returned, &format_returned,
			      &nitems_return,
			      &bytes_after_return,
			      &prop_data) != Success;
  if (gdk_error_trap_pop () ||
      type_returned == None)
    abort++;
  if (!abort &&
      requested_type != AnyPropertyType &&
      requested_type != type_returned)
    {
      /* aparently this can happen for some properties of broken apps, be silent */
      abort++;
    }
  if (!abort && bytes_after_return)
    {
      g_warning (G_GNUC_PRETTY_FUNCTION "(): Eeek, property has more than %u bytes, stored on harddisk?",
		 prop_buffer_lengh);
      abort++;
    }
  if (!abort && expected_format && expected_format != format_returned)
    {
      g_warning (G_GNUC_PRETTY_FUNCTION "(): Expected format (%u) unmatched (%d)",
		 expected_format, format_returned);
      abort++;
    }
  if (!abort && prop_data && nitems_return && format_returned)
    {
      switch (format_returned)
	{
	case 32:
	  *size_p = nitems_return * 4;
	  if (sizeof (gulong) == 8)
	    {
	      guint32 i, *mem = g_malloc0 (*size_p + 1);
	      gulong *prop_longs = (gulong*) prop_data;

	      for (i = 0; i < *size_p / 4; i++)
		mem[i] = prop_longs[i];
	      data = mem;
	    }
	  break;
	case 16:
	  *size_p = nitems_return * 2;
	  break;
	case 8:
	  *size_p = nitems_return;
	  break;
	default:
	  g_warning ("Unknown property data format with %d bits (extraterrestrial?)",
		     format_returned);
	  break;
	}
      if (!data && *size_p)
	{
	  guint8 *mem = NULL;

	  if (format_returned == 8 && type_returned == XA_COMPOUND_TEXT)
	    {
	      gchar **tlist = NULL;
	      gint count = gdk_text_property_to_text_list
		      (gdk_x11_xatom_to_atom (type_returned), 8, prop_data,
		       nitems_return, &tlist);

	      if (count && tlist && tlist[0])
		{
		  mem = (guint8 *)g_strdup (tlist[0]);
		  *size_p = strlen ((char *)mem);
		}
	      if (tlist)
		gdk_free_text_list (tlist);
	    }
	  if (!mem)
	    {
	      mem = g_malloc (*size_p + 1);
	      memcpy (mem, prop_data, *size_p);
	      mem[*size_p] = 0;
	    }
	  data = mem;
	}
    }

  if (prop_data)
    XFree (prop_data);
  
  return data;
}

/* 
 * Update the mdm_wm_screen 'effective' screen area when a window reserves struts.
 * This only works if struts don't "collide", i.e. there is a max of one strut setter
 * per edge.  Of course this should be the case at mdm time...
 */
static void
mdm_wm_update_struts (Display *xdisplay, Window xwindow)
{
	gint     size = 0;
	guint32 *struts = get_typed_property_data (xdisplay, xwindow, XA_NET_WM_STRUT, 
						   XA_CARDINAL, &size, 32);
	if (size == 16)
	{
		gint i;
		for (i = 0; i < 4; ++i) 
		{
			/* strut owners are the only windows whose 'zero' struts are reflected */
			if (struts[i] != 0 || (strut_owners[i] == xwindow))
			{
			/* if any window re-specifies a strut, it becomes the new owner */
				strut_owners[i] = xwindow;
				save_struts[i] = struts[i];
			}
		}
	}
	g_free (struts);
}

/* stolen from gwmh */
static gboolean
wm_protocol_check_support (Window xwin,
			   Atom   check_atom)
{
  Atom *pdata = NULL;
  guint32 *gdata = NULL;
  int n_pids = 0;
  gboolean is_supported = FALSE;
  guint i, n_gids = 0;

  trap_push ();

  if (!XGetWMProtocols (wm_disp,
			xwin,
			&pdata,
			&n_pids))
    {
      gint size = 0;

      gdata = get_typed_property_data (wm_disp,
				       xwin,
				       XA_WM_PROTOCOLS,
				       XA_WM_PROTOCOLS,
				       &size, 32);
      n_gids = size / 4;
    }

  trap_pop ();

  for (i = 0; i < n_pids; i++)
    if (pdata[i] == check_atom)
      {
	is_supported = TRUE;
	break;
      }
  if (pdata)
    XFree (pdata);
  if (!is_supported)
    for (i = 0; i < n_gids; i++)
      if (gdata[i] == check_atom)
        {
	  is_supported = TRUE;
	  break;
        }
  g_free (gdata);

  return is_supported;
}

static GList *
find_window_list (Window w, gboolean deco_ok)
{
	GList *li;

	for (li = windows; li != NULL; li = li->next) {
		MdmWindow *gw = li->data;

		if (gw->win == w)
			return li;
		if (deco_ok &&
		    (gw->deco == w ||
		     gw->shadow == w))
			return li;
	}

	return NULL;
}

static MdmWindow *
find_window (Window w, gboolean deco_ok)
{
	GList *li = find_window_list (w, deco_ok);
	if (li == NULL)
		return NULL;
	else
		return li->data;
}

void
mdm_wm_focus_window (Window window)
{
	XWindowAttributes attribs = {0};
	MdmWindow *win;

	if (no_focus_login > 0 &&
	    window == wm_login_window)
		return;

	win = find_window (window, TRUE);
	if (win != NULL &&
	    ! win->takefocus)
		return;

	trap_push ();

	XGetWindowAttributes (wm_disp, window, &attribs);
	if (attribs.map_state == IsUnmapped) {
		trap_pop ();
		return;
	}

	if (wm_protocol_check_support (window, XA_WM_TAKE_FOCUS)) {
		XEvent xevent = { 0, };

		xevent.type = ClientMessage;
		xevent.xclient.window = window;
		xevent.xclient.message_type = XA_WM_PROTOCOLS;
		xevent.xclient.format = 32;
		xevent.xclient.data.l[0] = XA_WM_TAKE_FOCUS;
		xevent.xclient.data.l[1] = CurrentTime;

		XSendEvent (wm_disp, window, False, 0, &xevent);
		XSync (wm_disp, False);
	}

	XSetInputFocus (wm_disp,
			window,
			RevertToPointerRoot,
			CurrentTime);
	trap_pop ();

	wm_focus_window = window;
}

static void
constrain_window (MdmWindow *gw)
{
/* constrain window to lie within screen geometry, with struts reserved */
	int x, y, screen_x = 0, screen_y = 0;
	Window root;
	unsigned int width, height, border, depth;
	unsigned int screen_width = gdk_screen_width (), screen_height = gdk_screen_height ();

	/* exclude any strut areas not owned by this window */
	if (strut_owners[0] != gw->win) 
	{
		screen_x = save_struts[0];
		screen_width -= save_struts[0];
	}
	if (strut_owners[2] != gw->win) 
	{
		screen_y = save_struts[2];
		screen_height -= save_struts[2];
	}
	if (strut_owners[1] != gw->win) 
		screen_width -= save_struts[1];
	if (strut_owners[3] != gw->win) 
		screen_height -= save_struts[3];
	
	if (gw->deco == None) 
	    return;

	trap_push ();

	XGetGeometry (wm_disp, gw->deco,
		      &root, &x, &y, &width, &height, &border, &depth);

	if (width > screen_width)
	    width = screen_width;
	if (height > screen_height)
	    height = screen_height;
	
	if (x < screen_x)
	    x = screen_x;
	if (y < screen_y)
	    y = screen_y;
	if ((x - screen_x + width) > screen_width)
	    x = screen_width - width;
	if ((y - screen_y + height) > screen_height)
	    y = screen_height - height;

	XMoveResizeWindow (wm_disp, gw->deco, x, y, width, height);

	trap_pop ();
}

static void
constrain_all_windows (void)
{
	GList *winlist = windows;

	while (winlist)
	{
		MdmWindow *gw = winlist->data;
		constrain_window (gw);
		winlist = winlist->next;
	}
}

static void
center_x_window (MdmWindow *gw, Window w, Window hintwin)
{
	XSizeHints hints;
	Status status;
	long ret;
	int x, y;
	Window root;
	unsigned int width, height, border, depth;
	gboolean can_resize, can_reposition;

	trap_push ();

	status = XGetWMNormalHints (wm_disp,
				    hintwin,
				    &hints,
				    &ret);

	if ( ! status) {
		trap_pop ();
		return;
	}

	/* allow resizing when PSize is given, just don't allow centering when
	 * PPosition is goven */
	can_resize = ! (hints.flags & USSize);
	can_reposition = ! (hints.flags & USPosition ||
			    hints.flags & PPosition);

	if (can_reposition && ! gw->center)
		can_reposition = FALSE;

	if (gw->ignore_size_hints) {
		can_resize = TRUE;
		can_reposition = TRUE;
	}

	if ( ! can_resize &&
	     ! can_reposition) {
		trap_pop ();
		return;
	}

	XGetGeometry (wm_disp, w,
		      &root, &x, &y, &width, &height, &border, &depth);

	/* we replace the x,y and width,height with some new values */

	if (can_resize) {
		if (width > mdm_wm_screen.width)
			width = mdm_wm_screen.width;
		if (height > mdm_wm_screen.height)
			height = mdm_wm_screen.height;
	}

	if (can_reposition) {
		/* we wipe the X with some new values */
		x = mdm_wm_screen.x + (mdm_wm_screen.width - width)/2;
		y = mdm_wm_screen.y + (mdm_wm_screen.height - height)/2;	

		if (x < mdm_wm_screen.x)
			x = mdm_wm_screen.x;
		if (y < mdm_wm_screen.y)
			y = mdm_wm_screen.y;
	}
	
	XMoveResizeWindow (wm_disp, w, x, y, width, height);

	if (gw->center && ! gw->recenter) {
		gw->center = FALSE;
	}

	trap_pop ();
}

#ifndef MWMUTIL_H_INCLUDED

typedef struct {
    unsigned long flags;
    unsigned long functions;
    unsigned long decorations;
    long input_mode;
    unsigned long status;
} MotifWmHints, MwmHints;

#define MWM_HINTS_DECORATIONS   (1L << 1)

#define MWM_DECOR_BORDER        (1L << 1)

#endif /* MWMUTIL_H_INCLUDED */

static gboolean
has_deco (Window win)
{
	static Atom hints_atom = None;
	unsigned char *foo;
	MotifWmHints *hints;
	Atom type;
	gint format;
	gulong nitems;
	gulong bytes_after;
	gboolean border = TRUE;

	trap_push ();

	if (hints_atom == None)
		hints_atom = XInternAtom (wm_disp, "_MOTIF_WM_HINTS", False);

	hints = NULL;

	XGetWindowProperty (wm_disp, win,
			    hints_atom, 0,
			    sizeof (MotifWmHints) / sizeof (long),
			    False, AnyPropertyType, &type, &format, &nitems,
			    &bytes_after, &foo);
	hints = (MotifWmHints *)foo;

	if (type != None &&
	    hints != NULL &&
	    hints->flags & MWM_HINTS_DECORATIONS &&
	    ! (hints->decorations & MWM_DECOR_BORDER)) {
		border = FALSE;
	}

	if (hints != NULL)
		XFree (hints);

	trap_pop ();

	return border;
}


static void
add_deco (MdmWindow *w, gboolean is_mapped)
{
	int x, y;
	Window root;
	unsigned int width, height, border, depth;
	XWindowAttributes attribs = { 0, };
	int black;

	trap_push ();

	XGetWindowAttributes (wm_disp, w->win, &attribs);
	XSelectInput (wm_disp, w->win,
		      attribs.your_event_mask |
		      PropertyChangeMask);

	if ( ! has_deco (w->win)) {
		trap_pop ();
		return;
	}

	XGetGeometry (wm_disp, w->win,
		      &root, &x, &y, &width, &height, &border, &depth);

	black = BlackPixel (wm_disp, DefaultScreen (wm_disp));

	/* all but the login window has shadows */
	if (w->win != wm_login_window) {
		w->shadow = XCreateSimpleWindow (wm_disp,
						 wm_root,
						 x + 4, y + 4,
						 width + 2 + 2 * border,
						 height + 2 + 2 * border,
						 0, 
						 black, black);

		XMapWindow (wm_disp, w->shadow);
	}

	w->deco = XCreateSimpleWindow (wm_disp,
				       wm_root,
				       x - 1, y - 1,
				       width + 2 + 2 * border,
				       height + 2 + 2 * border,
				       0, 
				       black, black);

	XGetWindowAttributes (wm_disp, w->deco, &attribs);
	XSelectInput (wm_disp, w->deco,
		      attribs.your_event_mask |
		      EnterWindowMask |
		      PropertyChangeMask |
		      SubstructureNotifyMask |
		      SubstructureRedirectMask);

	XMapWindow (wm_disp, w->deco);

	XSync (wm_disp, False);
	trap_pop ();

	trap_push ();
	XReparentWindow (wm_disp, w->win, w->deco, 1, 1);
	XSync (wm_disp, False);
	if (trap_pop () == 0) {
		if (is_mapped) {
			/* Ignore the next unmap/map, but only
			 * if reparent window really succeeded */
			w->ignore_next_map++;
			w->ignore_next_unmap++;
		}
	}
}

static gboolean
is_wm_class (XClassHint *hint, const char *string, int len)
{
	if (len > 0) {
		return ((hint->res_name != NULL &&
			 strncmp (hint->res_name, string, len) == 0) ||
			(hint->res_class != NULL &&
			 strncmp (hint->res_class, string, len) == 0));
	} else {
		return ((hint->res_name != NULL &&
			 strcmp (hint->res_name, string) == 0) ||
			(hint->res_class != NULL &&
			 strcmp (hint->res_class, string) == 0));
	}
}

static MdmWindow *
add_window (Window w, gboolean center, gboolean is_mapped)
{
	MdmWindow *gw;

	gw = find_window (w, FALSE);
	if (gw == NULL) {
		XClassHint hint = { NULL, NULL };
		XWMHints *wmhints;
		int x, y;
		Window root;
		unsigned int width, height, border, depth;

		gw = g_new0 (MdmWindow, 1);
		gw->win = w;
		windows = g_list_prepend (windows, gw);

		trap_push ();

		/* add "centering" */
		gw->ignore_size_hints = FALSE;
		gw->center = center;
		gw->recenter = FALSE;
		gw->takefocus = TRUE;

		gw->ignore_next_map = 0;
		gw->ignore_next_unmap = 0;

		wmhints = XGetWMHints (wm_disp, w);
		if (wmhints != NULL) {
			/* NoInput windows */
			if ((wmhints->flags & InputHint) &&
			    ! wmhints->input) {
				gw->takefocus = FALSE;
			}
			XFree (wmhints);
		}

		/* hack, set USpos/size on login window */
		if (w == wm_login_window) {
			long ret;
			XSizeHints hints;
			XGetWMNormalHints (wm_disp, w, &hints, &ret);
			hints.flags |= USPosition | USSize;
			XSetWMNormalHints (wm_disp, w, &hints);
			gw->center = FALSE;
			gw->recenter = FALSE;
		} else if (XGetClassHint (wm_disp, w, &hint)) {
			if (is_wm_class (&hint, "mdm", 3)) {
				gw->ignore_size_hints = TRUE;
				gw->center = TRUE;
				gw->recenter = TRUE;
			} else if (is_wm_class (&hint, "gkrellm", 0)) {
				/* hack, gkrell is stupid and doesn't set
				 * right hints, such as USPosition and other
				 * such stuff */
				gw->center = FALSE;
				gw->recenter = FALSE;
			} else if (is_wm_class (&hint, "xscribble", 0)) {
				/* hack, xscribble mustn't take focus */
				gw->takefocus = FALSE;
			}
			if (hint.res_name != NULL)
				XFree (hint.res_name);
			if (hint.res_class != NULL)
				XFree (hint.res_class);
		}

		XGetGeometry (wm_disp, w,
			      &root, &x, &y, &width, &height, &border, &depth);

		gw->x = x;
		gw->y = x;

		center_x_window (gw, w, w);
		add_deco (gw, is_mapped);

		XAddToSaveSet (wm_disp, w);

		trap_pop ();
	}
	return gw;
}

static void
remove_window (Window w)
{
	GList *li = find_window_list (w, FALSE);

	if (w == wm_focus_window)
		wm_focus_window = None;

	if (li != NULL) {
		MdmWindow *gw = li->data;

		li->data = NULL;

		trap_push ();

		XRemoveFromSaveSet (wm_disp, w);

		gw->win = None;

		if (gw->deco != None) {
			XDestroyWindow (wm_disp, gw->deco);
			gw->deco = None;
		}
		if (gw->shadow != None) {
			XDestroyWindow (wm_disp, gw->shadow);
			gw->shadow = None;
		}
		trap_pop ();

		windows = g_list_remove_link (windows, li);
		g_list_free_1 (li);

		g_free (gw);
	}
}

static void
revert_focus_to_login (void)
{
	if (wm_login_window != None) {
		mdm_wm_focus_window (wm_login_window);
	}
}

static void
add_all_current_windows (void)
{
	Window *children = NULL;
	Window xparent, xroot;
	guint size = 0;

	gdk_flush ();
	XSync (wm_disp, False);
	trap_push ();

	XGrabServer (wm_disp);

	if (XQueryTree (wm_disp, 
			wm_root,
			&xroot,
			&xparent,
			&children,
			&size)) {
		int i;

		for (i = 0; i < size; i++) {
			XWindowAttributes attribs = {0};

			XGetWindowAttributes (wm_disp,
					      children[i],
					      &attribs);

			if ( ! attribs.override_redirect &&
			    attribs.map_state != IsUnmapped) {
				add_window (children[i],
					    FALSE /*center*/,
					    TRUE /* is_mapped */);
			}
		}

		if (children != NULL)
			XFree (children);
	}

	XUngrabServer (wm_disp);

	trap_pop ();
}

static void
reparent_to_root (MdmWindow *gw)
{
	/* only if reparented */
	if (gw->deco != None) {
		trap_push ();

		XReparentWindow (wm_disp, gw->win, wm_root, gw->x, gw->y);
		XSync (wm_disp, False);

		trap_pop ();
	}
}

static void
shadow_follow (MdmWindow *gw)
{
	int x, y;
	Window root;
	unsigned int width, height, border, depth;

	if (gw->shadow == None)
		return;

	trap_push ();

	XGetGeometry (wm_disp, gw->deco,
		      &root, &x, &y, &width, &height, &border, &depth);

	x += 5;
	y += 5;

	XMoveResizeWindow (wm_disp, gw->shadow, x, y, width, height);

	trap_pop ();
}

static void
event_process (XEvent *ev)
{
	MdmWindow *gw;
	Window w;
	XWindowChanges wchanges;

	trap_push ();

	switch (ev->type) {
	case MapRequest:
		w = ev->xmaprequest.window;
		gw = find_window (w, FALSE);
		if (gw == NULL) {
			if (ev->xmaprequest.parent == wm_root) {
				XGrabServer (wm_disp);
				gw = add_window (w,
						 TRUE /* center */,
						 FALSE /* is_mapped */);
				XUngrabServer (wm_disp);
			}
		}
		XMapWindow (wm_disp, w);
		break;
	case ConfigureRequest:
		XGrabServer (wm_disp);
		w = ev->xconfigurerequest.window;
		gw = find_window (w, FALSE);
		wchanges.border_width = ev->xconfigurerequest.border_width;
		wchanges.sibling = ev->xconfigurerequest.above;
		wchanges.stack_mode = ev->xconfigurerequest.detail;
		if (gw == NULL ||
		    gw->deco == None) {
			wchanges.x = ev->xconfigurerequest.x;
			wchanges.y = ev->xconfigurerequest.y;
		} else {
			wchanges.x = 1;
			wchanges.y = 1;
		}
		wchanges.width = ev->xconfigurerequest.width;
		wchanges.height = ev->xconfigurerequest.height;
		XConfigureWindow (wm_disp,
				  w,
				  ev->xconfigurerequest.value_mask,
				  &wchanges);
		if (gw != NULL) {
			gw->x = ev->xconfigurerequest.x;
			gw->y = ev->xconfigurerequest.y;
			if (gw->deco != None) {
				wchanges.x = ev->xconfigurerequest.x - 1;
				wchanges.y = ev->xconfigurerequest.y - 1;
				wchanges.width = ev->xconfigurerequest.width + 2
					+ 2*ev->xconfigurerequest.border_width;;
				wchanges.height = ev->xconfigurerequest.height + 2
					+ 2*ev->xconfigurerequest.border_width;;
				wchanges.border_width = 0;
				XConfigureWindow (wm_disp,
						  gw->deco,
						  ev->xconfigurerequest.value_mask,
						  &wchanges);
				center_x_window (gw, gw->deco, gw->win);
			} else {
				center_x_window (gw, gw->win, gw->win);
			}
			shadow_follow (gw);
		}
		XUngrabServer (wm_disp);
		break;
	case CirculateRequest:
		w = ev->xcirculaterequest.window;
		gw = find_window (w, FALSE);
		if (gw == NULL) {
			if (ev->xcirculaterequest.place == PlaceOnTop)
				XRaiseWindow (wm_disp, w);
			else
				XLowerWindow (wm_disp, w);
		} else {
			if (ev->xcirculaterequest.place == PlaceOnTop) {
				if (gw->shadow != None)
					XRaiseWindow (wm_disp, gw->shadow);
				if (gw->deco != None)
					XRaiseWindow (wm_disp, gw->deco);
				else
					XRaiseWindow (wm_disp, gw->win);
			} else {
				if (gw->deco != None)
					XLowerWindow (wm_disp, gw->deco);
				else
					XLowerWindow (wm_disp, gw->win);
				if (gw->shadow != None)
					XLowerWindow (wm_disp, gw->shadow);
			}
		}
		break;
	case MapNotify:
		w = ev->xmap.window;
		gw = find_window (w, FALSE);
		if (gw != NULL) {
			if (gw->ignore_next_map > 0) {
				gw->ignore_next_map --;
				break;
			}
			if ( ! ev->xmap.override_redirect &&
			     focus_new_windows) {
				mdm_wm_focus_window (w);
			}
		}
		break;
	case UnmapNotify:
		w = ev->xunmap.window;
		gw = find_window (w, FALSE);
		if (gw != NULL) {
			if (gw->ignore_next_unmap > 0) {
				gw->ignore_next_unmap --;
				break;
			}
			XGrabServer (wm_disp);
			if (gw->deco != None)
				XUnmapWindow (wm_disp, gw->deco);
			if (gw->shadow != None)
				XUnmapWindow (wm_disp, gw->shadow);
			reparent_to_root (gw);
			remove_window (w);
			XDeleteProperty (wm_disp, w, XA_WM_STATE);
			if (w != wm_login_window)
				revert_focus_to_login ();
			XUngrabServer (wm_disp);
		}
		break;
	case DestroyNotify:
		w = ev->xdestroywindow.window;
		gw = find_window (w, FALSE);
		if (gw != NULL) {
			XGrabServer (wm_disp);
			remove_window (w);
			if (w != wm_login_window)
				revert_focus_to_login ();
			XUngrabServer (wm_disp);
		}
		break;
	case EnterNotify:
		w = ev->xcrossing.window;
		gw = find_window (w, TRUE);
		if (gw != NULL)
			mdm_wm_focus_window (gw->win);
		break;
	case PropertyNotify:
		if (ev->xproperty.atom == XA_NET_WM_STRUT)
		{
			mdm_wm_update_struts (ev->xproperty.display, 
					      ev->xproperty.window);
			constrain_all_windows ();
		}
		break;
	default:
		break;
	}

	trap_pop ();
}

/* following partly stolen from gdk */
static GPollFD event_poll_fd;

static gboolean  
event_prepare (GSource *source,
	       gint     *timeout)
{
	*timeout = -1;
	return XPending (wm_disp) > 0;
}

static gboolean  
event_check (GSource *source)
{
	if (event_poll_fd.revents & G_IO_IN) {
		return XPending (wm_disp) > 0;
	} else {
		return FALSE;
	}
}

static void
process_events (void)
{
	while (XPending (wm_disp) > 0) {
		XEvent ev;
		XNextEvent (wm_disp, &ev);
		event_process (&ev);
	}
}

static gboolean  
event_dispatch (GSource     *source,
		GSourceFunc  callback,
		gpointer     user_data)
{
	process_events ();

	return TRUE;
}

static GSourceFuncs event_funcs = {
	event_prepare,
	event_check,
	event_dispatch
};

void
mdm_wm_init (Window login_window)
{
	XWindowAttributes attribs = { 0, };
	GSource *source;
	gchar *display;

	wm_login_window = login_window;

	if (wm_disp != NULL) {
		return;
	}

	display = gdk_get_display ();
	wm_disp = XOpenDisplay (display);
	g_free (display);
	if (wm_disp == NULL) {
		/* EEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEK! */
		wm_disp = GDK_DISPLAY ();
		return;
	}

	trap_push ();

	XA_WM_PROTOCOLS = XInternAtom (wm_disp, "WM_PROTOCOLS", False);
	XA_WM_STATE = XInternAtom (wm_disp, "WM_STATE", False);
	XA_WM_TAKE_FOCUS = XInternAtom (wm_disp, "WM_TAKE_FOCUS", False);

	XA_COMPOUND_TEXT = XInternAtom (wm_disp, "COMPOUND_TEXT", False);
	XA_NET_WM_STRUT = XInternAtom (wm_disp, "_NET_WM_STRUT", False);

	wm_root = DefaultRootWindow (wm_disp);

	/* set event mask for events on root window */
	XGetWindowAttributes (wm_disp, wm_root, &attribs);
	XSelectInput (wm_disp, wm_root,
		      attribs.your_event_mask |
		      SubstructureNotifyMask |
		      SubstructureRedirectMask);

	if (trap_pop () != 0)
		return;

	trap_push ();

	add_all_current_windows ();

	source = g_source_new (&event_funcs, sizeof (GSource));

	event_poll_fd.fd = ConnectionNumber (wm_disp);
	event_poll_fd.events = G_IO_IN;

	g_source_add_poll (source, &event_poll_fd);
	g_source_set_priority (source, GDK_PRIORITY_EVENTS);
	g_source_set_can_recurse (source, FALSE);
	g_source_attach (source, NULL);

	trap_pop ();
}

void
mdm_wm_focus_new_windows (gboolean focus)
{
	focus_new_windows = focus;
}

void
mdm_wm_no_login_focus_push (void)
{
	/* it makes not sense for this to be false then */
	focus_new_windows = TRUE;
	no_focus_login++;
}

void
mdm_wm_no_login_focus_pop (void)
{
	no_focus_login --;

	if (no_focus_login == 0 &&
	    wm_focus_window == None &&
	    wm_login_window != None)
		mdm_wm_focus_window (wm_login_window);
}

void
mdm_wm_get_window_pos (Window window, int *xp, int *yp)
{
	int x, y;
	Window root;
	unsigned int width, height, border, depth;
	MdmWindow *gw;

	trap_push ();

	gw = find_window (window, TRUE);

	if (gw == NULL) {
		XGetGeometry (wm_disp, window,
			      &root, &x, &y, &width, &height, &border, &depth);

		*xp = x;
		*yp = y;

		trap_pop ();

		return;
	}

	if (gw->deco != None) {
		XGetGeometry (wm_disp, gw->deco,
			      &root, &x, &y, &width, &height, &border, &depth);
		*xp = x + 1;
		*yp = y + 1;
	} else {
		XGetGeometry (wm_disp, gw->win,
			      &root, &x, &y, &width, &height, &border, &depth);
		*xp = x;
		*yp = y;
	}

	trap_pop ();
}

void
mdm_wm_move_window_now (Window window, int x, int y)
{
	MdmWindow *gw;

	trap_push ();

	gw = find_window (window, TRUE);

	if (gw == NULL) {
		XMoveWindow (wm_disp, window, x, y);

		XSync (wm_disp, False);
		trap_pop ();
		return;
	}

	if (gw->deco != None)
		XMoveWindow (wm_disp, gw->deco, x - 1, y - 1);
	else
		XMoveWindow (wm_disp, gw->win, x, y);
	if (gw->shadow != None)
		XMoveWindow (wm_disp, gw->deco, x + 4, y + 4);

	XSync (wm_disp, False);
	trap_pop ();
}

void
mdm_wm_save_wm_order (void)
{
	Window *children = NULL;
	Window xparent, xroot;
	guint size = 0;
	int dlen = 0;
	unsigned long *data;

	gdk_flush ();
	XSync (wm_disp, False);
	trap_push ();

	XGrabServer (wm_disp);

	if (XQueryTree (wm_disp, 
			wm_root,
			&xroot,
			&xparent,
			&children,
			&size)) {
		int i;
		Atom atom;
		data = g_new0 (unsigned long, size);

		for (i = 0; i < size; i++) {
			MdmWindow *gw = find_window (children[i], TRUE);

			/* Ignore unknowns and shadows */
			if (gw == NULL ||
			    gw->shadow == children[i])
				continue;

			if (gw->win == wm_login_window) {
				/* Empty spot in the list signifies the
				 * login window */
				data [dlen++] = None;
			} else {
				data [dlen++] = gw->win;
			}
		}

		atom = XInternAtom (wm_disp, "MDMWM_WINDOW_ORDER", False);

		XChangeProperty (wm_disp, wm_root,
				 atom,
				 XA_CARDINAL,
				 32,
				 PropModeReplace,
				 (unsigned char *)data,
				 dlen);

		if (children != NULL)
			XFree (children);
		g_free (data);
	}

	XUngrabServer (wm_disp);

	trap_pop ();
}

static gboolean
focus_win (gpointer data)
{
	Window focus = (Window)data;
	focus_new_windows = TRUE;
	mdm_wm_focus_window (focus);
	return FALSE;
}

void
mdm_wm_restore_wm_order (void)
{
	guint32 *data;
	Window focus = None;
	int size;
	int i;
	Atom atom;

	gdk_flush ();
	XSync (wm_disp, False);

	process_events ();

	gdk_flush ();
	XSync (wm_disp, False);
	trap_push ();

	XGrabServer (wm_disp);

	atom = XInternAtom (wm_disp, "MDMWM_WINDOW_ORDER", False);

	data = get_typed_property_data (wm_disp, wm_root,
					atom, XA_CARDINAL,
					&size, 32);

	if (data != NULL) {
		for (i = 0; i < size/4; i++) {
			MdmWindow *gw;
			if (data[i] == None)
				gw = find_window (wm_login_window, TRUE);
			else
				gw = find_window (data[i], TRUE);

			if (gw != NULL) {
				focus = gw->win;
				if (gw->shadow != None)
					XRaiseWindow (wm_disp, gw->shadow);
				if (gw->deco != None)
					XRaiseWindow (wm_disp, gw->deco);
				else
					XRaiseWindow (wm_disp, gw->win);
			}
		}

		g_free (data);
	}

	XUngrabServer (wm_disp);

	trap_pop ();

	process_events ();

	if (focus != None) {
		/* let us hit the main loop first */ 
		g_idle_add (focus_win, (gpointer)focus);
	}
}

void
mdm_wm_show_info_msg_dialog (const gchar *msg_file,
			     const gchar *msg_font)
{
	GtkWidget *dialog, *label;
	gchar *InfoMsg;
	gsize InfoMsgLength;

	if (ve_string_empty (msg_file) ||
	    ! g_file_test (msg_file, G_FILE_TEST_EXISTS) ||
	    ! g_file_get_contents (msg_file, &InfoMsg, &InfoMsgLength, NULL))
		return;

	if (InfoMsgLength <= 0) {
		g_free (InfoMsg);
		return;
	}

	mdm_wm_focus_new_windows (TRUE);
	dialog = gtk_dialog_new_with_buttons (NULL /* Message */,
					      NULL /* parent */, GTK_DIALOG_MODAL |
					      GTK_DIALOG_DESTROY_WITH_PARENT,
					      GTK_STOCK_OK, GTK_RESPONSE_OK,
					      NULL);
	label = gtk_label_new (InfoMsg);

	if (msg_font && strlen (msg_font) > 0) {
		PangoFontDescription *MdmInfoMsgFontDesc = pango_font_description_from_string (msg_font);
		if (MdmInfoMsgFontDesc) {
			gtk_widget_modify_font (label, MdmInfoMsgFontDesc);
			pango_font_description_free (MdmInfoMsgFontDesc);
		}
	}

	gtk_container_add (GTK_CONTAINER (GTK_DIALOG (dialog)->vbox), label);
	gtk_widget_show_all (dialog);
	mdm_wm_center_window (GTK_WINDOW (dialog));

	mdm_common_setup_cursor (GDK_LEFT_PTR);

	mdm_wm_no_login_focus_push ();
	gtk_dialog_run (GTK_DIALOG (dialog));
	gtk_widget_destroy (dialog);
	mdm_wm_no_login_focus_pop ();

	g_free (InfoMsg);
}

static GtkWidget *
hig_dialog_new (GtkWindow      *parent,
		GtkDialogFlags flags,
		GtkMessageType type,
		GtkButtonsType buttons,
		const gchar    *primary_message,
		const gchar    *secondary_message)
{
	GtkWidget *dialog;

	dialog = gtk_message_dialog_new (GTK_WINDOW (parent),
		                         GTK_DIALOG_DESTROY_WITH_PARENT,
		                         type,
		                         buttons,
		                         "%s", primary_message);

	gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
		                                  "%s", secondary_message);

	gtk_window_set_title (GTK_WINDOW (dialog), "");
	gtk_container_set_border_width (GTK_CONTAINER (dialog), 5);
	gtk_box_set_spacing (GTK_BOX (GTK_DIALOG (dialog)->vbox), 14);

  	return dialog;
}

void
mdm_wm_message_dialog (const gchar *primary_message, 
		       const gchar *secondary_message)
{
	GtkWidget *req = NULL;

	/* we should be now fine for focusing new windows */
	mdm_wm_focus_new_windows (TRUE);

	req = hig_dialog_new (NULL /* parent */,
                              GTK_DIALOG_MODAL /* flags */,
                              GTK_MESSAGE_INFO,
                              GTK_BUTTONS_OK,
                              primary_message,
                              secondary_message);

	mdm_wm_center_window (GTK_WINDOW (req));

	mdm_wm_no_login_focus_push ();
	gtk_dialog_run (GTK_DIALOG (req));
	gtk_widget_destroy (req);
	mdm_wm_no_login_focus_pop ();
}

gint
mdm_wm_query_dialog (const gchar *primary_message,
		     const gchar *secondary_message,
		     const char *posbutton,
		     const char *negbutton,
		     gboolean has_cancel)
{
	int ret;
	GtkWidget *req;
	GtkWidget *button;

	/* we should be now fine for focusing new windows */
	mdm_wm_focus_new_windows (TRUE);

	req = hig_dialog_new (NULL /* parent */,
                              GTK_DIALOG_MODAL /* flags */,
                              GTK_MESSAGE_QUESTION,
                              GTK_BUTTONS_NONE,
                              primary_message,
                              secondary_message);

	if (negbutton != NULL) {
		button = gtk_button_new_from_stock (negbutton);
		gtk_dialog_add_action_widget (GTK_DIALOG (req), button, GTK_RESPONSE_NO);
		GTK_WIDGET_SET_FLAGS (button, GTK_CAN_DEFAULT);
		gtk_widget_show (button);
	}

	if (has_cancel == TRUE) {
		button = gtk_button_new_from_stock (GTK_STOCK_CANCEL);
		gtk_dialog_add_action_widget (GTK_DIALOG (req), button, GTK_RESPONSE_CANCEL);
		GTK_WIDGET_SET_FLAGS (button, GTK_CAN_DEFAULT);
		gtk_widget_show (button);
	}

	if (posbutton != NULL) {
		button = gtk_button_new_from_stock (posbutton);
		gtk_dialog_add_action_widget (GTK_DIALOG (req), button, GTK_RESPONSE_YES);
		GTK_WIDGET_SET_FLAGS (button, GTK_CAN_DEFAULT);
		gtk_widget_show (button);
	}

	if (posbutton != NULL)
		gtk_dialog_set_default_response (GTK_DIALOG (req), GTK_RESPONSE_YES);
	else if (negbutton != NULL)
		gtk_dialog_set_default_response (GTK_DIALOG (req), GTK_RESPONSE_NO);
	else if (has_cancel)
		gtk_dialog_set_default_response (GTK_DIALOG (req), GTK_RESPONSE_CANCEL);

	mdm_wm_center_window (GTK_WINDOW (req));

	mdm_wm_no_login_focus_push ();
	ret = gtk_dialog_run (GTK_DIALOG (req));
	mdm_wm_no_login_focus_pop ();
	gtk_widget_destroy (req);

	return ret;
}

gint
mdm_wm_warn_dialog (const gchar *primary_message,
                    const gchar *secondary_message,
                    const char *posbutton,
                    const char *negbutton,
                    gboolean has_cancel)
{
	int ret;
	GtkWidget *req;
	GtkWidget *button;

	/* we should be now fine for focusing new windows */
	mdm_wm_focus_new_windows (TRUE);

	req = hig_dialog_new (NULL /* parent */,
                              GTK_DIALOG_MODAL /* flags */,
                              GTK_MESSAGE_WARNING,
                              GTK_BUTTONS_NONE,
                              primary_message,
                              secondary_message);

	if (negbutton != NULL) {
		button = gtk_button_new_from_stock (negbutton);
		gtk_dialog_add_action_widget (GTK_DIALOG (req), button, GTK_RESPONSE_NO);
		GTK_WIDGET_SET_FLAGS (button, GTK_CAN_DEFAULT);
		gtk_widget_show (button);
	}

	if (has_cancel == TRUE) {
		button = gtk_button_new_from_stock (GTK_STOCK_CANCEL);
		gtk_dialog_add_action_widget (GTK_DIALOG (req), button, GTK_RESPONSE_CANCEL);
		GTK_WIDGET_SET_FLAGS (button, GTK_CAN_DEFAULT);
		gtk_widget_show (button);
	}

	if (posbutton != NULL) {
		button = gtk_button_new_from_stock (posbutton);
		gtk_dialog_add_action_widget (GTK_DIALOG (req), button, GTK_RESPONSE_YES);
		GTK_WIDGET_SET_FLAGS (button, GTK_CAN_DEFAULT);
		gtk_widget_show (button);
	}

	if (posbutton != NULL)
		gtk_dialog_set_default_response (GTK_DIALOG (req), GTK_RESPONSE_YES);
	else if (negbutton != NULL)
		gtk_dialog_set_default_response (GTK_DIALOG (req), GTK_RESPONSE_NO);
	else if (has_cancel)
		gtk_dialog_set_default_response (GTK_DIALOG (req), GTK_RESPONSE_CANCEL);

	mdm_wm_center_window (GTK_WINDOW (req));

	mdm_wm_no_login_focus_push ();
	ret = gtk_dialog_run (GTK_DIALOG (req));
	mdm_wm_no_login_focus_pop ();
	gtk_widget_destroy (req);

	return ret;
}

/* EOF */
