/*
 * $Id$
 *
 * Copyright (c) 2003, Michael Gray
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
 * @ingroup gtk
 * @file
 *
 * Icon management.
 *
 * @author Michael Gray
 * @date 2003
 */

#include "gui.h"

RCSID("$Id$")

#include "icon.h"

#ifdef USE_GTK2

#include "gtk2/interface-glade.h"
#include "if/gnet_property.h"
#include "lib/override.h"		/* Must be the last header included */

static GtkWidget *icon;
static GtkWidget *canvas;
static GdkPixbuf *con_pixbuf, *up_pixbuf, *down_pixbuf;
static gboolean icon_visible_fg, icon_close_fg, icon_just_mapped_fg;
static guint32 leaf_cnt, norm_cnt, ultra_cnt, con_max;
static guint32 up_cnt, up_max;
static guint32 down_cnt, down_max;

#if GTK_CHECK_VERSION(2, 0, 0) && !GTK_CHECK_VERSION(2, 2, 0)
/** gdk_pixbuf_render_to_drawable is deprecated since GTK+ 2.2.0 */
static void gdk_draw_pixbuf(GdkDrawable *drawable, GdkGC *gc, GdkPixbuf *pixbuf,
	gint src_x, gint src_y, gint dest_x, gint dest_y, gint width, gint height,
	GdkRgbDither dither, gint x_dither, gint y_dither)
{
	gdk_pixbuf_render_to_drawable(pixbuf, drawable, gc, src_x, src_y,
		dest_x, dest_y, width, height, dither, x_dither, y_dither);
}
#endif

/*
 * These macros set the default icon window dimensions.  The
 * ICON_INSET should be the size of the borders drawn on the
 * window.  The XPM_WIDTH is the width between the icons and
 * the bar.
 */
#define ICON_WIDTH   64
#define ICON_HEIGHT  64
#define ICON_INSET   2
#define ICON_INSET2  4
#define ICON_INSET4  8
#define XPM_WIDTH    20

/**
 * Callback when icon recieves a map event.
 *
 * This function and on_icon_unmap_event are needed to keep
 * track of when main_window is iconified.
 */
gboolean
on_icon_map_event(GtkWidget *unused_widget, GdkEvent *unused_event,
	gpointer unused_udata)
{
	(void) unused_widget;
	(void) unused_event;
	(void) unused_udata;
    icon_just_mapped_fg = icon_visible_fg = TRUE;
    return FALSE;
}

/**
 * Callback when icon recieves an unmap event.
 */
gboolean
on_icon_unmap_event(GtkWidget *unused_widget, GdkEvent *unused_event,
	gpointer unused_udata)
{
	(void) unused_widget;
	(void) unused_event;
	(void) unused_udata;
    icon_visible_fg = FALSE;
    return FALSE;
}

/**
 * Calculates the width of rect will that will represent the
 * value of cnt in rect.
 */
static guint
get_width(const GdkRectangle *rect, const guint cnt, const guint mx)
{
    guint r;

    r = (guint) ((gfloat)rect->width * ((gfloat) cnt / (gfloat) mx));
    return (r < (guint) rect->width) ? r : (guint) rect->width;
}

/**
 * Sets width and height of rect to that of image and calculates
 * x and y such that the centers of rect and base are the same point.
 */
static void
center_image(GdkRectangle * rect,
	const GdkRectangle * base, const GdkPixbuf * image)
{
    rect->width = gdk_pixbuf_get_width(image);
    rect->height = gdk_pixbuf_get_height(image);
    rect->x = base->x + (base->width - rect->width) / 2;
    rect->y = base->y + (base->height - rect->height) / 2;
}

/**
 * Callback when canvas recieves an expose event.  The icon is entirely
 * redrawn for every expose event instead of checking and redrawing
 * just the dirty regions.  Since the icon is so small, the gain
 * probably isn't worth the extra overhead.
 */
gboolean
on_canvas_expose_event(GtkWidget *widget, GdkEventExpose *event,
	gpointer unused_udata)
{
    GdkRectangle panel, rect, bar;

	(void) unused_udata;

    /*   just draw once for all expose events   */
    if (event->count)
        return FALSE;

    /*   setup image column   */
    panel.x = ICON_INSET;
    panel.y = ICON_INSET;
    panel.height = (widget->allocation.height - ICON_INSET2) / 3;
    panel.width = XPM_WIDTH;

    /*   draw connection icon   */
    center_image(&rect, &panel, con_pixbuf);
    gdk_draw_pixbuf(canvas->window, NULL, con_pixbuf, 0, 0,
		rect.x, rect.y, rect.width, rect.height, GDK_RGB_DITHER_NONE, 0, 0);

    panel.y += panel.height;

    /*   paint download icon   */
    center_image(&rect, &panel, down_pixbuf);
    gdk_draw_pixbuf(canvas->window, NULL, down_pixbuf, 0, 0,
		rect.x, rect.y, rect.width, rect.height, GDK_RGB_DITHER_NONE, 0, 0);

    panel.y += panel.height;

    /*   paint upload icon   */
    center_image(&rect, &panel, up_pixbuf);
    gdk_draw_pixbuf(canvas->window, NULL, up_pixbuf, 0, 0,
		rect.x, rect.y, rect.width, rect.height, GDK_RGB_DITHER_NONE, 0, 0);

    /*   setup bar column   */
    panel.x = XPM_WIDTH + ICON_INSET;
    panel.y = ICON_INSET;
    panel.width = (ICON_WIDTH - ICON_INSET2) - XPM_WIDTH;
    panel.height = (widget->allocation.height - ICON_INSET2);

    /*   draw bar panel   */
    gtk_paint_box(widget->style, widget->window,
                  GTK_STATE_INSENSITIVE, GTK_SHADOW_OUT,
                  NULL, widget, NULL,
                  panel.x, panel.y, panel.width, panel.height);

    panel.height /= 3;
    rect.x = panel.x + ICON_INSET2;
    rect.y = panel.y + ICON_INSET2;
    rect.width = panel.width - ICON_INSET4;
    rect.height = panel.height - ICON_INSET4;
    bar.x = rect.x + ICON_INSET;
    bar.y = rect.y + ICON_INSET;
    bar.width = rect.width - ICON_INSET2;
    bar.height = rect.height - ICON_INSET2;

    /*   paint connection bar   */
    gtk_paint_shadow(widget->style, widget->window,
                     GTK_STATE_NORMAL, GTK_SHADOW_IN,
                     NULL, widget, NULL,
                     rect.x, rect.y, rect.width, rect.height);
    gdk_draw_rectangle(widget->window, widget->style->black_gc, TRUE,
                       bar.x, bar.y, bar.width, bar.height);
    bar.width = get_width(&bar, leaf_cnt + norm_cnt + ultra_cnt, con_max);
    gdk_draw_rectangle(widget->window, widget->style->white_gc, TRUE,
                       bar.x, bar.y, bar.width, bar.height);

    panel.y += panel.height;
    rect.y += panel.height;
    bar.y += panel.height;
    bar.width = rect.width - ICON_INSET2;

    /*   paint download bar   */
    gtk_paint_shadow(widget->style, widget->window,
                     GTK_STATE_NORMAL, GTK_SHADOW_IN,
                     NULL, widget, NULL,
                     rect.x, rect.y, rect.width, rect.height);
    gdk_draw_rectangle(widget->window, widget->style->black_gc, TRUE,
                       bar.x, bar.y, bar.width, bar.height);
    bar.width = get_width(&bar, down_cnt, down_max);
    gdk_draw_rectangle(widget->window, widget->style->white_gc, TRUE,
                       bar.x, bar.y, bar.width, bar.height);

    panel.y += panel.height;
    rect.y += panel.height;
    bar.y += panel.height;
    bar.width = rect.width - ICON_INSET2;

    /*   paint upload bar   */
    gtk_paint_shadow(widget->style, widget->window,
                     GTK_STATE_NORMAL, GTK_SHADOW_IN,
                     NULL, widget, NULL,
                     rect.x, rect.y, rect.width, rect.height);
    gdk_draw_rectangle(widget->window, widget->style->black_gc, TRUE,
                       bar.x, bar.y, bar.width, bar.height);
    bar.width = get_width(&bar, up_cnt, up_max);
    gdk_draw_rectangle(widget->window, widget->style->white_gc, TRUE,
                       bar.x, bar.y, bar.width, bar.height);

    /*   paint border   */
    gtk_paint_shadow(widget->style, widget->window,
                     GTK_STATE_NORMAL, GTK_SHADOW_OUT,
                     NULL, widget, NULL, 0, 0, -1, -1);

    return FALSE;
}

/**
 * Sets up the icon and canvas widgets.  This function was
 * generated by glade separatly from the main gui since the icon
 * widgets are independent of the rest of the gui, and there
 * are unresolved issues between GTK and GTK2.
 */
static void
create_icon(void)
{
    icon = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_widget_set_name(icon, "icon");
    gtk_object_set_data(GTK_OBJECT(icon), "icon", icon);
    gtk_widget_set_usize(icon, ICON_WIDTH + 1, ICON_HEIGHT + 1);
    gtk_widget_set_sensitive(icon, FALSE);
    GTK_WIDGET_SET_FLAGS(icon, GTK_CAN_FOCUS);
    GTK_WIDGET_SET_FLAGS(icon, GTK_CAN_DEFAULT);
    gtk_widget_set_events(icon, GDK_VISIBILITY_NOTIFY_MASK);
    gtk_window_set_title(GTK_WINDOW(icon), "icon");
    gtk_window_set_default_size(GTK_WINDOW(icon),
                                ICON_WIDTH + 1, ICON_HEIGHT + 1);
    gtk_window_set_policy(GTK_WINDOW(icon), FALSE, FALSE, FALSE);

    canvas = gtk_drawing_area_new();
    gtk_widget_set_name(canvas, "canvas");
    gtk_widget_ref(canvas);
    gtk_object_set_data_full(GTK_OBJECT(icon), "canvas", canvas,
                             (GtkDestroyNotify) gtk_widget_unref);
    gtk_widget_show(canvas);
    gtk_container_add(GTK_CONTAINER(icon), canvas);
    gtk_widget_set_events(canvas, GDK_EXPOSURE_MASK);

    gtk_signal_connect(GTK_OBJECT(icon), "map_event",
                       GTK_SIGNAL_FUNC(on_icon_map_event), NULL);
    gtk_signal_connect(GTK_OBJECT(icon), "unmap_event",
                       GTK_SIGNAL_FUNC(on_icon_unmap_event), NULL);
    gtk_signal_connect(GTK_OBJECT(canvas), "expose_event",
                       GTK_SIGNAL_FUNC(on_canvas_expose_event), NULL);
}

void
icon_timer(void)
{
    GdkRectangle rect;
    guint con_old, up_old, down_old;

    /*   don't do anything if icon isn't even visible or the
       application is closing   */
    if (!icon_visible_fg || icon_close_fg)
        return;

    /*   this may be useful someday   */
    if (icon_just_mapped_fg) {
        icon_just_mapped_fg = FALSE;
    }

    /*   get current values   */
    con_old = leaf_cnt + norm_cnt + ultra_cnt;
    up_old = up_cnt;
    down_old = down_cnt;
    gnet_prop_get_guint32_val(PROP_NODE_LEAF_COUNT, &leaf_cnt);
    gnet_prop_get_guint32_val(PROP_NODE_NORMAL_COUNT, &norm_cnt);
    gnet_prop_get_guint32_val(PROP_NODE_ULTRA_COUNT, &ultra_cnt);
    gnet_prop_get_guint32_val(PROP_MAX_CONNECTIONS, &con_max);
    gnet_prop_get_guint32_val(PROP_UL_RUNNING, &up_cnt);
    gnet_prop_get_guint32_val(PROP_MAX_UPLOADS, &up_max);
    gnet_prop_get_guint32_val(PROP_DL_RUNNING_COUNT, &down_cnt);
    gnet_prop_get_guint32_val(PROP_MAX_DOWNLOADS, &down_max);

    /*   if nothing has changed, then don't redraw   */
    if (con_old == leaf_cnt + norm_cnt + ultra_cnt)
        if (up_old == up_cnt && down_old == down_cnt)
            return;

    /*
     * For some reason, gtk_widget_queue_draw(canvas) will
     * not work in either GTK 1 or GTK 2, probably an issue
     * with the fact that the icon window is an icon.  Whatever
     * the reason, to get the canvas widget to redraw, it
     * has to be done that hard way.
     */
    rect.x = rect.y = 0;
    rect.width = canvas->allocation.width;
    rect.height = canvas->allocation.height;
    gdk_window_invalidate_rect(canvas->window, &rect, FALSE);
}

#if GTK_CHECK_VERSION(2, 10, 0)
static void
on_status_icon_activate(GtkStatusIcon *status_icon, gpointer unused_udata)
{
	(void) status_icon;
	(void) unused_udata;

	if (GTK_WIDGET_VISIBLE(gui_main_window())) {
		gtk_widget_hide(gui_main_window());
	} else {
		gtk_widget_show(gui_main_window());
	}
}
#endif	/* Gtk+ >= 2.10.0 */

/**
 * For details of what is expected from an icon window and what it
 * should expect.
 *
 * See --
 *    http://tronche.com/gui/x/icccm/sec-4.html#s-4.1.9
 */
void
icon_init(void)
{
    create_icon();

	gtk_widget_realize(icon);

    /*
     * For some reason, when a window is the icon for another
     * window, none of its subwindows get mapped.  This is not
     * because of GTK, but seems to be either the window manager
     * or X itself that does this.
     *
     * Also note the canvas widget is never unmapped, regardless
     * of whether the icon window is visible or not.
     */
    gtk_widget_map(canvas);
	/* FIXME: This causes a crash with twm when iconizing the main window. */
#if 0
    gdk_window_set_icon(gui_main_window()->window, icon->window, NULL, NULL);
#endif
    icon_just_mapped_fg = icon_visible_fg = icon_close_fg = FALSE;

    /*   load images   */
    con_pixbuf = create_pixbuf("smallserver.xpm");
    up_pixbuf = create_pixbuf("upload.xpm");
    down_pixbuf = create_pixbuf("download.xpm");

#if GTK_CHECK_VERSION(2, 10, 0)
	{
		GtkStatusIcon *status_icon;
		GdkPixbuf *icon_pixbuf;
		
		/*
		 * Create an status so that Gtk-Gnutella can be minimized to a
		 * so-called "system tray" if supported by the window manager.
		 */

    	icon_pixbuf = create_pixbuf("icon.xpm");
		status_icon = gtk_status_icon_new_from_pixbuf(icon_pixbuf);
		gtk_status_icon_set_tooltip(status_icon,
			_("Gtk-Gnutella: Click to minimize/restore"));
		gtk_status_icon_set_visible(status_icon, TRUE);
    	g_signal_connect(G_OBJECT(status_icon), "activate",
			G_CALLBACK(on_status_icon_activate), NULL);
	}
#endif	/* Gtk+ >= 2.10.0 */
}

void
icon_close(void)
{
    icon_close_fg = TRUE;

    /*
     * Because the icon window is a top level window, it must be
     * destroyed manually.
     */
    gtk_widget_destroy(icon);
}

#endif /* USE_GTK2  */

#ifdef USE_GTK1
#include "gtk1/interface-glade.h"
#include "lib/override.h"		/* Must be the last header included */

static GdkPixmap *icon_map;
static GdkBitmap *icon_mask;

void
icon_timer(void)
{
    return;
}

void
icon_init(void)
{
    GtkPixmap *pixmap;
    pixmap = (GtkPixmap *) create_pixmap(gui_main_window(), "icon.xpm");
    gtk_pixmap_get(pixmap, &icon_map, &icon_mask);
    gdk_window_set_icon(gui_main_window()->window, NULL, icon_map, icon_mask);
}

void
icon_close(void)
{
    return;
}

#endif /* USE_GTK1 */

/* vi: set ts=4 sw=4 cindent: */
