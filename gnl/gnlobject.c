/* Gnonlin
 * Copyright (C) <2001> Wim Taymans <wim.taymans@chello.be>
 *               <2004> Edward Hervey <bilboed@bilboed.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include "gnl.h"
#include "gnlmarshal.h"

typedef struct _GnlPadPrivate GnlPadPrivate;

struct _GnlPadPrivate {
  GnlObject		*object;
  GstPadDirection	dir;
  GstPadEventFunction	eventfunc;
  GstPadQueryFunction	queryfunc;
  GstPadLinkFunction	linkfunc;
  GstPadUnlinkFunction	unlinkfunc;
};

GST_BOILERPLATE (GnlObject, gnl_object, GstBin, GST_TYPE_BIN);

GST_DEBUG_CATEGORY_STATIC (gnlobject);
#define GST_CAT_DEFAULT gnlobject

enum {
  ARG_0,
  ARG_START,
  ARG_STOP,
  ARG_MEDIA_START,
  ARG_MEDIA_STOP,
  ARG_RATE,
  ARG_PRIORITY,
  ARG_ACTIVE,
  ARG_CAPS,
};

static void
gnl_object_set_property 	(GObject *object, guint prop_id,
				 const GValue *value, GParamSpec *pspec);

static void
gnl_object_get_property 	(GObject *object, guint prop_id, GValue *value,
				 GParamSpec *pspec);

static void
gnl_object_release_pad		(GstElement *element, GstPad *pad);

static gboolean
gnl_object_covers_func		(GnlObject *object,
				 GstClockTime start,
				 GstClockTime stop,
				 GnlCoverType type);


static void
gnl_object_base_init (gpointer g_class)
{
};

static void
gnl_object_class_init (GnlObjectClass *klass)
{
  GObjectClass 		*gobject_class;
  GstElementClass 	*gstelement_class;
  GnlObjectClass 	*gnlobject_class;

  gobject_class = 	(GObjectClass*)klass;
  gstelement_class = 	(GstElementClass*)klass;
  gnlobject_class = 	(GnlObjectClass*)klass;

  GST_DEBUG_CATEGORY_INIT (gnlobject, "gnlobject", 0, "GNonLin Base class");

  gobject_class->set_property = GST_DEBUG_FUNCPTR (gnl_object_set_property);
  gobject_class->get_property = GST_DEBUG_FUNCPTR (gnl_object_get_property);

  gstelement_class->release_pad = GST_DEBUG_FUNCPTR (gnl_object_release_pad);

  gnlobject_class->covers = GST_DEBUG_FUNCPTR (gnl_object_covers_func);

  g_object_class_install_property (gobject_class, ARG_START,
    g_param_spec_uint64 ("start", "Start", "The start position relative to the parent",
                         0, G_MAXINT64, 0, G_PARAM_READWRITE));
  g_object_class_install_property (gobject_class, ARG_STOP,
    g_param_spec_uint64 ("stop", "Stop", "The stop position relative to the parent",
                         0, G_MAXINT64, 0, G_PARAM_READWRITE));
  g_object_class_install_property (gobject_class, ARG_MEDIA_START,
    g_param_spec_uint64 ("media_start", "Media start", "The media start position",
                         0, G_MAXINT64, 0, G_PARAM_READWRITE));
  g_object_class_install_property (gobject_class, ARG_MEDIA_STOP,
    g_param_spec_uint64 ("media_stop", "Media stop", "The media stop position",
                         0, G_MAXINT64, 0, G_PARAM_READWRITE));
  g_object_class_install_property (gobject_class, ARG_RATE,
    g_param_spec_double ("rate", "Rate", "Playback rate of the media",
			 G_MINDOUBLE, G_MAXDOUBLE, 1.0, G_PARAM_READABLE));
  g_object_class_install_property (gobject_class, ARG_PRIORITY,
    g_param_spec_int ("priority", "Priority", "The priority of the object",
                       0, G_MAXINT, 0, G_PARAM_READWRITE));
  g_object_class_install_property (gobject_class, ARG_ACTIVE,
    g_param_spec_boolean ("active", "Active", "Render this object",
                          TRUE, G_PARAM_READWRITE));
  g_object_class_install_property (gobject_class, ARG_CAPS,
    g_param_spec_boxed ("caps", "Caps", "Caps used to filter/choose the output stream",
			GST_TYPE_CAPS, G_PARAM_READWRITE));
				   
/*   gstelement_class->change_state 	= gnl_object_change_state; */

}

static void
gnl_object_init (GnlObject *object, GnlObjectClass *klass)
{
  object->start = 0;
  object->stop = 0;
  object->media_start = GST_CLOCK_TIME_NONE;
  object->media_stop = GST_CLOCK_TIME_NONE;
  object->rate = 1.0;
  object->priority = 0;
  object->active = TRUE;
  object->caps = gst_caps_new_any();
}

/**
 * gnl_object_to_media_time:
 * @object:
 * @objecttime: The #GstClockTime we want to convert
 * @mediatime: A pointer on a #GstClockTime to fill
 *
 * Converts a #GstClockTime from the object (container) context to the media context
 *
 * Returns: TRUE if @objecttime was within the limits of the @object start/stop time,
 * FALSE otherwise
 */

static gboolean
gnl_object_to_media_time (GnlObject *object, GstClockTime otime, GstClockTime *mtime)
{
  g_return_val_if_fail (mtime, FALSE);

  GST_DEBUG_OBJECT (object, "ObjectTime : %" GST_TIME_FORMAT,
		    GST_TIME_ARGS(otime));

  /* limit check */
  if ((otime < object->start) || (otime >= object->stop)) {
    GST_DEBUG_OBJECT (object, "ObjectTime is outside object start/stop times");
    if (otime < object->start)
      *mtime = (object->media_start == GST_CLOCK_TIME_NONE) ? object->start : object->media_start;
    else
      *mtime = (object->media_stop == GST_CLOCK_TIME_NONE) ? object->stop : object->media_stop;
    return FALSE;
  }

  if (object->media_start == GST_CLOCK_TIME_NONE) {
    /* no time shifting, for live sources ? */
    *mtime = otime;
  } else {
    *mtime = (otime - object->start) * object->rate + object->media_start;
  }

  GST_DEBUG_OBJECT (object, "Returning MediaTime : %" GST_TIME_FORMAT,
		    GST_TIME_ARGS(*mtime));
  return TRUE;
}

/**
 * gnl_media_to_object_time:
 * @object:
 * @mediatime: The #GstClockTime we want to convert
 * @objecttime: A pointer on a #GstClockTime to fill
 *
 * Converts a #GstClockTime from the media context to the object (container) context
 *
 * Returns: TRUE if @objecttime was within the limits of the @object media start/stop time,
 * FALSE otherwise
 */

static gboolean
gnl_media_to_object_time (GnlObject *object, GstClockTime mtime, GstClockTime *otime)
{
  g_return_val_if_fail (otime, FALSE);

  GST_DEBUG_OBJECT (object, "MediaTime : %" GST_TIME_FORMAT,
		    GST_TIME_ARGS(mtime));
 
  /* limit check */
  if (object->media_start == GST_CLOCK_TIME_NONE)
    return gnl_object_to_media_time (object, mtime, otime);

  if (mtime < object->media_start) {
    GST_DEBUG_OBJECT (object, "media time is before media_start, forcing to start");
    *otime = object->start;
    return FALSE;
  } else if ((object->media_stop != GST_CLOCK_TIME_NONE) && (mtime >= object->media_stop)) {
    GST_DEBUG_OBJECT (object, "media time is at or after media_stop, forcing to stop");
    *otime = object->stop;
  }

  *otime = (mtime - object->media_start) / object->rate + object->start;

  GST_DEBUG_OBJECT (object, "Returning ObjectTime : %" GST_TIME_FORMAT,
		    GST_TIME_ARGS(*otime));
  return TRUE;
}

static gboolean
gnl_object_covers_func		(GnlObject *object,
				 GstClockTime start,
				 GstClockTime stop,
				 GnlCoverType type)
{
  gboolean	ret = FALSE;

  GST_DEBUG_OBJECT (object, "start:%lld, stop:%lld, type:%d",
		    start, stop, type);
  
 /* FIXME: BOGUS, REMOVE */
  gnl_media_to_object_time (object, 0, NULL);

  switch (type) {
  case GNL_COVER_ALL:
  case GNL_COVER_SOME:
    if ((start <= object->start) && (stop >= object->stop))
      ret = TRUE;
    break;
  case GNL_COVER_START:
    if ((start >= object->start) && (start < object->stop))
      ret = TRUE;
    break;
  case GNL_COVER_STOP:
    if ((stop >= object->start) && (stop < object->stop))
      ret = TRUE;
    break;
  default:
    break;
  }
  return ret;
};

gboolean
gnl_object_covers (GnlObject *object, GstClockTime start, GstClockTime stop,
		   GnlCoverType type)
{
  return GNL_OBJECT_GET_CLASS(object)->covers(object, start, stop, type);
}

static void
gnl_object_release_pad		(GstElement *element, GstPad *pad)
{
  GnlPadPrivate	*priv = gst_pad_get_element_private (pad);

  GST_DEBUG_OBJECT (element, "releasing pad %s:%s",
		    GST_DEBUG_PAD_NAME(pad));
  if (priv)
    g_free (priv);

}

static GstEvent *
translate_incoming_seek (GnlObject *object, GstEvent *event)
{
  GstEvent	*event2;
  GstFormat	format;
  gdouble	rate, nrate;
  GstSeekFlags	flags;
  GstSeekType	curtype, stoptype;
  gint64	cur, ncur;
  gint64	stop, nstop;
  GstStructure	*struc;

  /* FILL IN */
  GST_DEBUG_OBJECT (object, "shifting cur/stop/rate of seek event to object time domain");
  gst_event_parse_seek (event, &rate, &format, &flags,
			&curtype, &cur,
			&stoptype, &stop);
  if (format != GST_FORMAT_TIME) {
    GST_WARNING ("GNonLin time shifting only works with GST_FORMAT_TIME");
    return event;
  }

  event2 = GST_EVENT (gst_mini_object_make_writable (GST_MINI_OBJECT (event)));
  struc = gst_event_get_structure(event2);

  /* convert rate */
  nrate = rate * object->rate;
  gst_structure_set (struc, "rate", G_TYPE_DOUBLE, nrate, NULL);

  /* convert cur */
  if ((curtype == GST_SEEK_TYPE_SET) && (gnl_object_to_media_time (object, cur, &ncur)))
      gst_structure_set (struc, "cur", G_TYPE_INT64, ncur, NULL);

  /* convert stop, we also need to limit it to object->stop */
  if ((stoptype == GST_SEEK_TYPE_SET) && (gnl_object_to_media_time (object, stop, &nstop)))
    gst_structure_set (struc, "stop", G_TYPE_INT64, nstop, NULL);

  /* add segment seekflags */
  if (!(flags && GST_SEEK_FLAG_SEGMENT))
    gst_structure_set (struc, "flags", GST_TYPE_SEEK_FLAGS, flags | GST_SEEK_FLAG_SEGMENT);

  return event2;
}

static GstEvent *
translate_outgoing_seek (GnlObject *object, GstEvent *event)
{
  GST_DEBUG_OBJECT (object, "shifting cur/stop/rate of seek event to container time domain");

  return event;
}

static GstEvent *
translate_outgoing_newsegment (GnlObject *object, GstEvent *event)
{
  GstEvent	*event2;
  gboolean	update;
  gdouble	rate;
  GstFormat	format;
  gint64	start, stop, stream;
  gint64	nstream;

  /* only modify the streamtime */
  GST_DEBUG_OBJECT (object, "Modifying stream time for container time domain");

  gst_event_parse_newsegment (event, &update, &rate, &format,
			      &start, &stop, &stream);

  if (format != GST_FORMAT_TIME)
    return event;
  
  gnl_media_to_object_time (object, stream, &nstream);

  event2 = gst_event_new_newsegment (update, rate / object->rate, format,
				     start, stop, nstream);

  return event2;
}

static gboolean
internalpad_event_function	(GstPad *internal, GstEvent *event)
{
  GnlPadPrivate	*priv = gst_pad_get_element_private (internal);
  GnlObject	*object = priv->object;

  GST_DEBUG_OBJECT (internal, "...");

  switch (priv->dir) {
  case GST_PAD_SRC:
    if (GST_EVENT_TYPE (event) == GST_EVENT_NEWSEGMENT) {
      event = translate_outgoing_newsegment (object, event);
    }
    break;
  case GST_PAD_SINK:
    if (GST_EVENT_TYPE (event) == GST_EVENT_SEEK) {
      event = translate_outgoing_seek (object, event);
    }
    break;
  default:
    break;
  }
  return priv->eventfunc (internal, event);
}

static gboolean
internalpad_query_function	(GstPad *internal, GstQuery *query)
{
  GnlPadPrivate	*priv = gst_pad_get_element_private (internal);
/*   GnlObject	*object = GNL_OBJECT (GST_PAD_PARENT (internal)); */

  GST_DEBUG_OBJECT (internal, "...");

  switch (priv->dir) {
  case GST_PAD_SRC:
    break;
  case GST_PAD_SINK:
    break;
  default:
    break;
  }
  return priv->queryfunc (internal, query);
}

static gboolean
ghostpad_event_function		(GstPad *ghostpad, GstEvent *event)
{
  GnlPadPrivate	*priv = gst_pad_get_element_private (ghostpad);
  GnlObject	*object = priv->object;

  GST_DEBUG_OBJECT (ghostpad, "...");

  switch (priv->dir) {
  case GST_PAD_SRC:
    if (GST_EVENT_TYPE (event) == GST_EVENT_SEEK) {
      event = translate_incoming_seek (object, event);
    }
    break;
  case GST_PAD_SINK:
    /* Unless I'm mistaken, we don't need to modify incoming NEWSEGMENT */
    /*     if (GST_EVENT_TYPE (event) == GST_EVENT_NEWSEGMENT) { */
    /*       translate_incoming_newsegment (object, event); */
    /*     } */
    /*     break; */
  default:
    break;
  }

  return priv->eventfunc (ghostpad, event);
}

static gboolean
ghostpad_query_function		(GstPad *ghostpad, GstQuery *query)
{
  GnlPadPrivate	*priv = gst_pad_get_element_private (ghostpad);
  gboolean	pret;

  GST_DEBUG_OBJECT (ghostpad, "...");

  pret = priv->queryfunc (ghostpad, query);
  if (pret) {
    /* translate result */
  }
  return pret;
}

static void
control_internal_pad (GstPad *ghostpad, GnlObject *object)
{
  GnlPadPrivate	*priv = g_new0(GnlPadPrivate, 1);
  GstPad		*internal = gst_pad_get_peer (gst_ghost_pad_get_target (GST_GHOST_PAD (ghostpad)));

  GST_LOG_OBJECT (ghostpad, "overriding ghostpad's internal pad function");

  priv->object = object;
  priv->dir = GST_PAD_DIRECTION (ghostpad);
  priv->eventfunc = GST_PAD_EVENTFUNC (internal);
  priv->queryfunc = GST_PAD_QUERYFUNC (internal);
  gst_pad_set_element_private (internal, priv);
  
  /* add query/event function overrides on internal pad */
  gst_pad_set_event_function (internal, GST_DEBUG_FUNCPTR (internalpad_event_function));
  gst_pad_set_query_function (internal, GST_DEBUG_FUNCPTR (internalpad_query_function));
}

static GstPadLinkReturn
ghostpad_link_function		(GstPad *ghostpad, GstPad *peer)
{
  GnlPadPrivate	*priv = gst_pad_get_element_private (ghostpad);
  GstPadLinkReturn	ret;
  
  GST_DEBUG_OBJECT (ghostpad, "...");

  ret = priv->linkfunc (ghostpad, peer);

  if (ret == GST_PAD_LINK_OK) {
    GST_DEBUG_OBJECT (ghostpad, "linking went ok, getting internal pad and overriding query/event functions");
    control_internal_pad (ghostpad, GNL_OBJECT (GST_PAD_PARENT (ghostpad)));
  }
  return ret;
}

static void
ghostpad_unlink_function	(GstPad *ghostpad)
{
  GstPad	*internal = gst_pad_get_peer (gst_ghost_pad_get_target (GST_GHOST_PAD (ghostpad)));
  GnlPadPrivate	*priv = gst_pad_get_element_private (internal);

  GST_DEBUG_OBJECT (ghostpad, "...");

  /* remove query/event function overrides on internal pad */
  if (priv)
    g_free (priv);
  
  priv->unlinkfunc (ghostpad);
}

/**
 * gnl_object_ghost_pad:
 * @object: #GnlObject to add the ghostpad to
 * @name: Name for the new pad
 * @target: Target #GstPad to ghost
 *
 * Adds a #GstGhostPad overridding the correct pad [query|event]_function so 
 * that time shifting is done correctly
 * The #GstGhostPad is added to the #GnlObject
 *
 * /!\ This function doesn't check if the existing [src|sink] pad was removed
 * first, so you might end up with more pads than wanted
 *
 * Returns: The #GstPad if everything went correctly, else NULL.
 */

GstPad *
gnl_object_ghost_pad	(GnlObject *object, const gchar *name, GstPad *target)
{
  GstPadDirection	dir = GST_PAD_DIRECTION (target);
  GstPad	*ghost = NULL;
  GnlPadPrivate	*priv;

  g_return_val_if_fail ((dir != GST_PAD_UNKNOWN), FALSE);

  /* Create ghostpad for target */
  ghost = gst_ghost_pad_new (name, target);
  if (!ghost)
    return NULL;

  if (!(gst_element_add_pad (GST_ELEMENT (object), ghost))) {
    GST_WARNING ("couldn't add newly created ghostpad");
    return NULL;
  }
  
  GST_DEBUG ("grabbing existing pad functions");

  /* remember the existing ghostpad event/query/link/unlink functions */
  priv = g_new0(GnlPadPrivate, 1);
  priv->dir = dir;
  priv->object = object;
  priv->eventfunc = GST_PAD_EVENTFUNC (ghost);
  priv->queryfunc = GST_PAD_QUERYFUNC (ghost);
  priv->linkfunc = GST_PAD_LINKFUNC (ghost);
  priv->unlinkfunc = GST_PAD_UNLINKFUNC (ghost);
  gst_pad_set_element_private (GST_PAD (ghost), priv);

  GST_DEBUG ("setting our own functions");

  gst_pad_set_event_function (ghost, GST_DEBUG_FUNCPTR (ghostpad_event_function));
  gst_pad_set_query_function (ghost, GST_DEBUG_FUNCPTR (ghostpad_query_function));
  gst_pad_set_link_function (ghost, GST_DEBUG_FUNCPTR (ghostpad_link_function));
  gst_pad_set_unlink_function (ghost, GST_DEBUG_FUNCPTR (ghostpad_unlink_function));

  /* maybe the ghostpad is already linked */
  if (GST_PAD_IS_LINKED (ghost)) {
    GST_LOG_OBJECT (ghost, "ghostpad was already linked");
    control_internal_pad (ghost, object);
  }

  return ghost;
}

static void
gnl_object_set_caps (GnlObject *object, const GstCaps *caps)
{
  if (object->caps)
    gst_caps_unref (object->caps);

  object->caps = gst_caps_copy (caps);
}

static void
gnl_object_set_property (GObject *object, guint prop_id,
			 const GValue *value, GParamSpec *pspec)
{
  GnlObject *gnlobject;

  g_return_if_fail (GNL_IS_OBJECT (object));

  gnlobject = GNL_OBJECT (object);

  switch (prop_id) {
    case ARG_START:
      gnlobject->start = g_value_get_uint64 (value);
      break;
    case ARG_STOP:
      gnlobject->stop = g_value_get_uint64 (value);
      break;
    case ARG_MEDIA_START:
      gnlobject->media_start = g_value_get_uint64 (value);
      break;
    case ARG_MEDIA_STOP:
      gnlobject->media_stop = g_value_get_uint64 (value);
      break;
    case ARG_PRIORITY:
      gnlobject->priority = g_value_get_int (value);
      break;
    case ARG_ACTIVE:
      gnlobject->active = g_value_get_boolean (value);
      break;
    case ARG_CAPS:
      gnl_object_set_caps (gnlobject, gst_value_get_caps (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gnl_object_get_property (GObject *object, guint prop_id,
			 GValue *value, GParamSpec *pspec)
{
  GnlObject *gnlobject;
  
  g_return_if_fail (GNL_IS_OBJECT (object));

  gnlobject = GNL_OBJECT (object);

  switch (prop_id) {
    case ARG_START:
      g_value_set_uint64 (value, gnlobject->start);
      break;
    case ARG_STOP:
      g_value_set_uint64 (value, gnlobject->stop);
      break;
    case ARG_MEDIA_START:
      g_value_set_uint64 (value, gnlobject->media_start);
      break;
    case ARG_MEDIA_STOP:
      g_value_set_uint64 (value, gnlobject->media_stop);
      break;
    case ARG_RATE:
      g_value_set_double (value, gnlobject->rate);
      break;
    case ARG_PRIORITY:
      g_value_set_int (value, gnlobject->priority);
      break;
    case ARG_ACTIVE:
      g_value_set_boolean (value, gnlobject->active);
      break;
    case ARG_CAPS:
      gst_value_set_caps (value, gnlobject->caps);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}
