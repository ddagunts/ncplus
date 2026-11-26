/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * netcastsink: GStreamer video sink for the LG NetCast TV's video plane.
 *
 * A bin around v4l2sink on the mt5398-vplane device (found by name in sysfs): decoded video
 * goes straight to the panel's video plane, full screen, and the kernel switches the desktop
 * off while it streams. Without the device (another machine, module not loaded) the bin
 * holds ximagesink instead, so players keep working in a window.
 *
 * Registered twice:
 *   netcastsink    rank PRIMARY + 10, class Sink/Video: autovideosink (gst-play-1.0, ...)
 *                  picks it over the X sinks.
 *   autoimagesink  rank NONE: Parole's default "videosink" setting names this element, which
 *                  GStreamer itself doesn't have (Parole then falls back to ximagesink), so
 *                  Parole uses the video plane with no change to it.
 * GstVideoOverlay calls (the player's X window) are passed to ximagesink and ignored for the
 * video plane, which is always full screen.
 */
#include <dirent.h>
#include <stdio.h>
#include <string.h>

#include <gst/gst.h>
#include <gst/video/videooverlay.h>

#define PACKAGE "netcastsink"
#define VERSION "1.0"
#define PLANE_NAME "mt5398-vplane"

#define GST_TYPE_NETCAST_SINK (gst_netcast_sink_get_type())
G_DECLARE_FINAL_TYPE(GstNetcastSink, gst_netcast_sink, GST, NETCAST_SINK, GstBin)

struct _GstNetcastSink {
	GstBin parent;
	GstElement *sink;
	gboolean plane;
};

GST_DEBUG_CATEGORY_STATIC(netcast_debug);
#define GST_CAT_DEFAULT netcast_debug

static void overlay_set_window_handle(GstVideoOverlay *o, guintptr handle)
{
	GstNetcastSink *self = GST_NETCAST_SINK(o);

	if (!self->plane && GST_IS_VIDEO_OVERLAY(self->sink))
		gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(self->sink), handle);
}

static void overlay_expose(GstVideoOverlay *o)
{
	GstNetcastSink *self = GST_NETCAST_SINK(o);

	if (!self->plane && GST_IS_VIDEO_OVERLAY(self->sink))
		gst_video_overlay_expose(GST_VIDEO_OVERLAY(self->sink));
}

static void overlay_handle_events(GstVideoOverlay *o, gboolean on)
{
	GstNetcastSink *self = GST_NETCAST_SINK(o);

	if (!self->plane && GST_IS_VIDEO_OVERLAY(self->sink))
		gst_video_overlay_handle_events(GST_VIDEO_OVERLAY(self->sink), on);
}

static void overlay_set_render_rectangle(GstVideoOverlay *o, gint x, gint y, gint w, gint h)
{
	GstNetcastSink *self = GST_NETCAST_SINK(o);

	if (!self->plane && GST_IS_VIDEO_OVERLAY(self->sink))
		gst_video_overlay_set_render_rectangle(GST_VIDEO_OVERLAY(self->sink), x, y, w, h);
}

static void overlay_init(GstVideoOverlayInterface *iface)
{
	iface->set_window_handle = overlay_set_window_handle;
	iface->expose = overlay_expose;
	iface->handle_events = overlay_handle_events;
	iface->set_render_rectangle = overlay_set_render_rectangle;
}

G_DEFINE_TYPE_WITH_CODE(GstNetcastSink, gst_netcast_sink, GST_TYPE_BIN,
			G_IMPLEMENT_INTERFACE(GST_TYPE_VIDEO_OVERLAY, overlay_init))

/* "/dev/videoN" of the video plane, or NULL */
static gchar *find_plane(void)
{
	DIR *d = opendir("/sys/class/video4linux");
	struct dirent *e;
	gchar *dev = NULL;

	if (!d)
		return NULL;
	while (!dev && (e = readdir(d))) {
		gchar *path, *name = NULL;

		if (strncmp(e->d_name, "video", 5))
			continue;
		path = g_strdup_printf("/sys/class/video4linux/%s/name", e->d_name);
		if (g_file_get_contents(path, &name, NULL, NULL) && !strcmp(g_strchomp(name), PLANE_NAME))
			dev = g_strdup_printf("/dev/%s", e->d_name);
		g_free(name);
		g_free(path);
	}
	closedir(d);
	return dev;
}

static void gst_netcast_sink_init(GstNetcastSink *self)
{
	gchar *dev = find_plane();
	GstPad *pad;

	if (dev) {
		self->sink = gst_element_factory_make("v4l2sink", NULL);
		if (self->sink) {
			/*
			 * io-mode stays auto: v4l2sink copies each frame into the device's buffers
			 * (~2 ms at 720p). dmabuf-import would save that for mt5398-vdec's buffers
			 * but refuses the system-memory ones every software decoder produces.
			 */
			g_object_set(self->sink, "device", dev, NULL);
			self->plane = TRUE;
		}
		GST_INFO_OBJECT(self, "video plane %s%s", dev, self->sink ? "" : ", but no v4l2sink");
		g_free(dev);
	}
	if (!self->sink)
		self->sink = gst_element_factory_make("ximagesink", NULL);
	if (!self->sink) {
		GST_ERROR_OBJECT(self, "neither v4l2sink nor ximagesink");
		return;
	}
	gst_bin_add(GST_BIN(self), self->sink);
	pad = gst_element_get_static_pad(self->sink, "sink");
	if (self->plane) {
		/*
		 * System memory only: v4l2sink also offers DMABuf (DMA_DRM) caps, and a decoder that
		 * picks those inside playbin fails ("DMABuf caps negotiated without the mandatory
		 * support of VideoMeta") because playbin's converters sit in between.
		 */
		GstElement *filter = gst_element_factory_make("capsfilter", NULL);
		GstCaps *caps = gst_caps_from_string("video/x-raw");

		g_object_set(filter, "caps", caps, NULL);
		gst_caps_unref(caps);
		gst_bin_add(GST_BIN(self), filter);
		gst_element_link(filter, self->sink);
		gst_object_unref(pad);
		pad = gst_element_get_static_pad(filter, "sink");
	}
	gst_element_add_pad(GST_ELEMENT(self), gst_ghost_pad_new("sink", pad));
	gst_object_unref(pad);
	GST_OBJECT_FLAG_SET(self, GST_ELEMENT_FLAG_SINK);
}

static GstStaticPadTemplate sink_template =
	GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS("video/x-raw"));

static void gst_netcast_sink_class_init(GstNetcastSinkClass *klass)
{
	GstElementClass *ec = GST_ELEMENT_CLASS(klass);

	gst_element_class_add_static_pad_template(ec, &sink_template);
	gst_element_class_set_static_metadata(ec, "NetCast video plane sink", "Sink/Video",
		"Shows video on the LG NetCast TV's video plane (mt5398-vplane), else in an X window",
		"ncplus");
}

static gboolean plugin_init(GstPlugin *plugin)
{
	GST_DEBUG_CATEGORY_INIT(netcast_debug, "netcastsink", 0, "NetCast video plane sink");
	return gst_element_register(plugin, "netcastsink", GST_RANK_PRIMARY + 10, GST_TYPE_NETCAST_SINK) &&
	       gst_element_register(plugin, "autoimagesink", GST_RANK_NONE, GST_TYPE_NETCAST_SINK);
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, netcastsink,
		  "LG NetCast TV video plane sink", plugin_init, VERSION, "LGPL", PACKAGE,
		  "ncplus")
