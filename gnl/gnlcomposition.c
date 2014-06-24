/* GStreamer
 * Copyright (C) 2001 Wim Taymans <wim.taymans@gmail.com>
 *               2004-2008 Edward Hervey <bilboed@bilboed.com>
 *               2014 Mathieu Duponchelle <mathieu.dupnchelle@opencreed.com>
 *               2014 Thibault Saunier <tsaunier@gnome.org>
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
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>

#include "gnl.h"

/**
 * SECTION:element-gnlcomposition
 *
 * A GnlComposition contains GnlObjects such as GnlSources and GnlOperations,
 * it connects them dynamically to create a composition timeline.
 */

static GstStaticPadTemplate gnl_composition_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

#define GNL_TYPE_COMPOSITION           (gnl_composition_get_type())
#define GNL_COMPOSITION(obj)           (G_TYPE_CHECK_INSTANCE_CAST((obj),GNL_TYPE_COMPOSITION,GnlComposition))
#define GNL_COMPOSITION_CLASS(klass)   (G_TYPE_CHECK_CLASS_CAST((klass),GNL_TYPE_COMPOSITION,GnlCompositionClass))
#define GNL_COMPOSITION_GET_CLASS(obj) (GNL_COMPOSITION_CLASS (G_OBJECT_GET_CLASS (obj)))
#define GNL_IS_COMPOSITION(obj)        (G_TYPE_CHECK_INSTANCE_TYPE((obj),GNL_TYPE_COMPOSITION))
#define GNL_IS_COMPOSITION_CLASS(obj)  (G_TYPE_CHECK_CLASS_TYPE((klass),GNL_TYPE_COMPOSITION))


/**********************************
 *   GnlComposition structures   *
 **********************************/
typedef struct _GnlCompositionEntry
{
  GnlObject *object;
  GnlComposition *comp;

  /* handler id for block probe */
  gulong probeid;
  gulong dataprobeid;

  gboolean seeked;
} GnlCompositionEntry;

typedef struct _GnlComposition
{
  GnlObject parent;

  gboolean dispose_has_run;

  /**
   * Sorted List of GnlObjects , ThreadSafe
   * objects_start : sorted by start-time then priority
   * objects_stop  : sorted by stop-time then priority
   * objects_hash  : contains signal handlers id for controlled objects
   * objects_lock  : mutex to acces/modify any of those lists/hashtable
   * pending_io    : The objects that should be removed or added on the next
   *                 commit
   */
  GList *objects_start;
  GList *objects_stop;
  GHashTable *objects_hash;
  GMutex objects_lock;
  GList *pending_io;

  /* List of GnlObject whose start/duration will be the same as the composition */
  GList *expandables;

  /* current stack, root of the tree of GnlObject currently used
   * in the pipeline  */
  GNode *current_stack;

  /* source top-level ghostpad, probe and entry */
  gulong ghosteventprobe;
  GnlCompositionEntry *toplevelentry;

  GstSegment current_segment;   /* The segment we are currently working on */
  GstSegment pending_segment;   /* The segment that has been requested */

  /* Next running base_time to set on outgoing segment */
  guint64 next_base_time;
  gboolean send_stream_start;

  GMainContext *mcontext;
  gboolean running;

  /* Ensure that when we remove all sources from the maincontext
   * we can not add any source, avoiding:
   * "g_source_attach: assertion '!SOURCE_DESTROYED (source)' failed" */
  GMutex mcontext_lock;

  gboolean reset_time;
  GstState deactivated_elements_state;
} GnlComposition;

typedef struct _GnlCompositionClass
{
  GnlObjectClass parent_class;

  /* Signal method handler */
    gboolean (*remove_object_signal_handler) (GnlComposition * self,
      GnlObject * child);
} GnlCompositionClass;

enum
{
  PROP_0,
  PROP_DEACTIVATED_ELEMENTS_STATE,
  PROP_LAST,
};

/* Properties from GnlObject */
enum
{
  GNLOBJECT_PROP_START,
  GNLOBJECT_PROP_STOP,
  GNLOBJECT_PROP_DURATION,
  GNLOBJECT_PROP_LAST
};

enum
{
  COMMIT_SIGNAL,
  REMOVE_OBJECT_SIGNAL,
  LAST_SIGNAL
};

static guint _signals[LAST_SIGNAL] = { 0 };

static GParamSpec *_properties[PROP_LAST];
static GParamSpec *gnlobject_properties[GNLOBJECT_PROP_LAST];

/**************************
 * GnlComposition macros  *
 **************************/

#define COMP_OBJECTS_LOCK(self) G_STMT_START {                                 \
  GST_LOG_OBJECT (self, "locking objects_lock from thread %p",               \
      g_thread_self());                                                      \
  g_mutex_lock (&self->objects_lock);                                  \
  GST_LOG_OBJECT (self, "locked objects_lock from thread %p",                \
      g_thread_self());                                                      \
} G_STMT_END

#define COMP_OBJECTS_UNLOCK(self) G_STMT_START {                               \
  GST_LOG_OBJECT (self, "unlocking objects_lock from thread %p",             \
      g_thread_self());                                                      \
  g_mutex_unlock (&self->objects_lock);                                \
} G_STMT_END

/* FIXME: We should not use GnlCompositionEntry, instead just use the
 * GnlObject themselves.
 */
#define COMP_ENTRY(self, object)                                               \
  (g_hash_table_lookup (self->objects_hash, (gconstpointer) object))

#define MAIN_CONTEXT_LOCK(self) G_STMT_START {                       \
  GST_LOG_OBJECT (self, "Getting MAIN_CONTEXT_LOCK in thread %p",    \
        g_thread_self());                                            \
  g_mutex_lock(&((GnlComposition*)self)->mcontext_lock);    \
  GST_LOG_OBJECT (self, "Got MAIN_CONTEXT_LOCK in thread %p",        \
        g_thread_self());                                            \
} G_STMT_END

#define MAIN_CONTEXT_UNLOCK(self) G_STMT_START {                     \
  g_mutex_unlock(&((GnlComposition*)self)->mcontext_lock);  \
  GST_LOG_OBJECT (self, "Unlocked MAIN_CONTEXT_LOCK in thread %p",   \
        g_thread_self());                                            \
} G_STMT_END

GST_DEBUG_CATEGORY_STATIC (gnlcomposition_debug);
#define GST_CAT_DEFAULT gnlcomposition_debug
#define gnl_composition_parent_class parent_class
G_DEFINE_TYPE (GnlComposition, gnl_composition, GNL_TYPE_OBJECT)

/**********************************
 * GnlComposition implementation  *
 **********************************/
     static GstPadProbeReturn
         pad_blocked (GstPad * pad, GstPadProbeInfo * info,
    GnlComposition * self)
{
  GST_DEBUG_OBJECT (self, "Pad : %s:%s", GST_DEBUG_PAD_NAME (pad));

  return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn
drop_data (GstPad * pad, GstPadProbeInfo * info, GnlCompositionEntry * entry)
{
  /* When updating the pipeline, do not let data flowing */
  if (!GST_IS_EVENT (info->data)) {
    GST_LOG_OBJECT (pad, "Dropping data while updating pipeline");
    return GST_PAD_PROBE_DROP;
  } else {
    GstEvent *event = GST_EVENT (info->data);

    if (GST_EVENT_TYPE (event) == GST_EVENT_SEEK) {
      entry->seeked = TRUE;
      GST_DEBUG_OBJECT (pad, "Got SEEK event");
    } else if (entry->seeked == TRUE &&
        GST_EVENT_TYPE (event) == GST_EVENT_SEGMENT) {
      entry->seeked = FALSE;
      entry->dataprobeid = 0;

      GST_DEBUG_OBJECT (pad, "Already seeked and got segment,"
          " removing probe");
      return GST_PAD_PROBE_REMOVE;
    }
  }

  return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn
ghost_event_probe_handler (GstPad * ghostpad G_GNUC_UNUSED,
    GstPadProbeInfo * info, GnlComposition * self)
{
  GstPadProbeReturn retval = GST_PAD_PROBE_OK;
  GstEvent *event = GST_PAD_PROBE_INFO_EVENT (info);

  GST_DEBUG_OBJECT (self, "event: %s", GST_EVENT_TYPE_NAME (event));

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_FLUSH_STOP:
      GST_DEBUG_OBJECT (self,
          "replacing flush stop event with a flush stop event with 'reset_time' = %d",
          self->reset_time);
      GST_PAD_PROBE_INFO_DATA (info) =
          gst_event_new_flush_stop (self->reset_time);
      gst_event_unref (event);
      break;
    case GST_EVENT_STREAM_START:
      if (g_atomic_int_compare_and_exchange (&self->send_stream_start, TRUE,
              FALSE)) {
        /* FIXME: Do we want to create a new stream ID here? */
        GST_DEBUG_OBJECT (self, "forward stream-start %p", event);
      } else {
        GST_DEBUG_OBJECT (self, "dropping stream-start %p", event);
        retval = GST_PAD_PROBE_DROP;
      }
      break;
    case GST_EVENT_SEGMENT:
    {
      guint64 rstart, rstop;
      const GstSegment *segment;
      GstSegment copy;
      GstEvent *event2;
      /* next_base_time */

      gst_event_parse_segment (event, &segment);
      gst_segment_copy_into (segment, &copy);

      rstart =
          gst_segment_to_running_time (segment, GST_FORMAT_TIME,
          segment->start);
      rstop =
          gst_segment_to_running_time (segment, GST_FORMAT_TIME, segment->stop);
      copy.base = self->next_base_time;
      GST_DEBUG_OBJECT (self,
          "Updating base time to %" GST_TIME_FORMAT ", next:%" GST_TIME_FORMAT,
          GST_TIME_ARGS (self->next_base_time),
          GST_TIME_ARGS (self->next_base_time + rstop - rstart));
      self->next_base_time += rstop - rstart;

      event2 = gst_event_new_segment (&copy);
      GST_EVENT_SEQNUM (event2) = GST_EVENT_SEQNUM (event);
      GST_PAD_PROBE_INFO_DATA (info) = event2;
      gst_event_unref (event);
    }
      break;
    case GST_EVENT_EOS:
      break;
    default:
      break;
  }

  return retval;
}

static void
_reset (GnlComposition * self)
{
  GST_DEBUG_OBJECT (self, "resetting");

  self->next_base_time = 0;

  if (self->current_stack)
    g_node_destroy (self->current_stack);
  self->current_stack = NULL;

  self->reset_time = FALSE;
  self->send_stream_start = TRUE;

  gst_segment_init (&self->current_segment, GST_FORMAT_TIME);
  gst_segment_init (&self->pending_segment, GST_FORMAT_TIME);

  GST_DEBUG_OBJECT (self, "Composition now resetted");
}

static void
hash_value_destroy (GnlCompositionEntry * entry)
{

  if (entry->probeid) {
    gst_pad_remove_probe (entry->object->srcpad, entry->probeid);
    entry->probeid = 0;
  }

  if (entry->dataprobeid) {
    gst_pad_remove_probe (entry->object->srcpad, entry->dataprobeid);
    entry->dataprobeid = 0;
  }

  g_slice_free (GnlCompositionEntry, entry);
}

static inline GstClockTime
_get_composition_start (GnlComposition * self)
{
  return MAX (self->current_segment.start, GNL_OBJECT_START (self));
}

static inline GstClockTime
_get_composition_stop (GnlComposition * self)
{
  return (GST_CLOCK_TIME_IS_VALID (self->current_segment.stop) ?
      (MIN (self->current_segment.stop, GNL_OBJECT_STOP (self))) :
      GNL_OBJECT_STOP (self));
}

static gboolean
lock_child_state (GValue * item, GValue * ret G_GNUC_UNUSED,
    gpointer udata G_GNUC_UNUSED)
{
  GstElement *child = g_value_get_object (item);

  GST_DEBUG_OBJECT (child, "locking state");
  gst_element_set_locked_state (child, TRUE);

  return TRUE;
}

static gint
objects_start_compare (GnlObject * a, GnlObject * b)
{
  if (a->start == b->start) {
    if (a->priority < b->priority)
      return -1;
    if (a->priority > b->priority)
      return 1;
    return 0;
  }
  if (a->start < b->start)
    return -1;
  if (a->start > b->start)
    return 1;
  return 0;
}

static gint
objects_stop_compare (GnlObject * a, GnlObject * b)
{
  if (a->stop == b->stop) {
    if (a->priority < b->priority)
      return -1;
    if (a->priority > b->priority)
      return 1;
    return 0;
  }
  if (b->stop < a->stop)
    return -1;
  if (b->stop > a->stop)
    return 1;
  return 0;
}

  static gint
priority_comp (GnlObject * a, GnlObject * b)
{
  if (a->priority < b->priority)
    return -1;

  if (a->priority > b->priority)
    return 1;

  return 0;
}

/* signal_duration_change
 * Creates a new GST_MESSAGE_DURATION_CHANGED with the currently configured
 * composition duration and sends that on the bus.
 */

static inline void
signal_duration_change (GnlComposition * comp)
{
  gst_element_post_message (GST_ELEMENT_CAST (comp),
      gst_message_new_duration_changed (GST_OBJECT_CAST (comp)));
}

static void
update_start_stop_duration (GnlComposition * comp)
{
  GnlObject *obj;
  GnlObject *cobj = (GnlObject *) comp;

  if (!comp->objects_start) {
    GST_LOG ("no objects, resetting everything to 0");

    if (cobj->start) {
      cobj->start = cobj->pending_start = 0;
      g_object_notify_by_pspec (G_OBJECT (cobj),
          gnlobject_properties[GNLOBJECT_PROP_START]);
    }

    if (cobj->duration) {
      cobj->pending_duration = cobj->duration = 0;
      g_object_notify_by_pspec (G_OBJECT (cobj),
          gnlobject_properties[GNLOBJECT_PROP_DURATION]);
      signal_duration_change (comp);
    }

    if (cobj->stop) {
      cobj->stop = 0;
      g_object_notify_by_pspec (G_OBJECT (cobj),
          gnlobject_properties[GNLOBJECT_PROP_STOP]);
    }

    return;
  }

  /* If we have a default object, the start position is 0 */
  if (comp->expandables) {
    GST_LOG_OBJECT (cobj,
        "Setting start to 0 because we have a default object");

    if (cobj->start != 0) {
      cobj->pending_start = cobj->start = 0;
      g_object_notify_by_pspec (G_OBJECT (cobj),
          gnlobject_properties[GNLOBJECT_PROP_START]);
    }

  } else {

    /* Else it's the first object's start value */
    obj = (GnlObject *) comp->objects_start->data;

    if (obj->start != cobj->start) {
      GST_LOG_OBJECT (obj, "setting start from %s to %" GST_TIME_FORMAT,
          GST_OBJECT_NAME (obj), GST_TIME_ARGS (obj->start));
      cobj->pending_start = cobj->start = obj->start;
      g_object_notify_by_pspec (G_OBJECT (cobj),
          gnlobject_properties[GNLOBJECT_PROP_START]);
    }

  }

  obj = (GnlObject *) comp->objects_stop->data;

  if (obj->stop != cobj->stop) {
    GST_LOG_OBJECT (obj, "setting stop from %s to %" GST_TIME_FORMAT,
        GST_OBJECT_NAME (obj), GST_TIME_ARGS (obj->stop));

    if (comp->expandables) {
      GList *tmp;

      GST_INFO_OBJECT (comp, "RE-setting all expandables duration and commit");
      for (tmp = comp->expandables; tmp; tmp = tmp->next) {
        g_object_set (tmp->data, "duration", obj->stop, NULL);
        gnl_object_commit (GNL_OBJECT (tmp->data), FALSE);
      }
    }

    comp->current_segment.stop = obj->stop;
    cobj->stop = obj->stop;
    g_object_notify_by_pspec (G_OBJECT (cobj),
        gnlobject_properties[GNLOBJECT_PROP_STOP]);
  }

  if ((cobj->stop - cobj->start) != cobj->duration) {
    cobj->pending_duration = cobj->duration = cobj->stop - cobj->start;
    g_object_notify_by_pspec (G_OBJECT (cobj),
        gnlobject_properties[GNLOBJECT_PROP_DURATION]);
    signal_duration_change (comp);
  }

  GST_LOG_OBJECT (comp,
      "start:%" GST_TIME_FORMAT
      " stop:%" GST_TIME_FORMAT
      " duration:%" GST_TIME_FORMAT,
      GST_TIME_ARGS (cobj->start),
      GST_TIME_ARGS (cobj->stop), GST_TIME_ARGS (cobj->duration));
}
/*
 * Converts a sorted list to a tree
 * Recursive
 *
 * stack will be set to the next item to use in the parent.
 * If operations number of sinks is limited, it will only use that number.
 */

static GNode *
convert_list_to_tree (GList ** stack, GstClockTime * start,
    GstClockTime * stop, guint32 * highprio)
{
  GNode *ret;
  guint nbsinks;
  gboolean limit;
  GList *tmp;
  GnlObject *object;

  if (!stack || !*stack)
    return NULL;

  object = (GnlObject *) (*stack)->data;

  GST_DEBUG ("object:%s , *start:%" GST_TIME_FORMAT ", *stop:%"
      GST_TIME_FORMAT " highprio:%d",
      GST_ELEMENT_NAME (object), GST_TIME_ARGS (*start),
      GST_TIME_ARGS (*stop), *highprio);

  /* update earliest stop */
  if (GST_CLOCK_TIME_IS_VALID (*stop)) {
    if (GST_CLOCK_TIME_IS_VALID (object->stop) && (*stop > object->stop))
      *stop = object->stop;
  } else {
    *stop = object->stop;
  }

  if (GST_CLOCK_TIME_IS_VALID (*start)) {
    if (GST_CLOCK_TIME_IS_VALID (object->start) && (*start < object->start))
      *start = object->start;
  } else {
    *start = object->start;
  }

  if (GNL_OBJECT_IS_SOURCE (object)) {
    *stack = g_list_next (*stack);

    /* update highest priority.
     * We do this here, since it's only used with sources (leafs of the tree) */
    if (object->priority > *highprio)
      *highprio = object->priority;

    ret = g_node_new (object);

    goto beach;
  } else {
    /* GnlOperation */
    GnlOperation *oper = (GnlOperation *) object;

    GST_LOG_OBJECT (oper, "operation, num_sinks:%d", oper->num_sinks);

    ret = g_node_new (object);
    limit = (oper->dynamicsinks == FALSE);
    nbsinks = oper->num_sinks;

    /* FIXME : if num_sinks == -1 : request the proper number of pads */
    for (tmp = g_list_next (*stack); tmp && (!limit || nbsinks);) {
      g_node_append (ret, convert_list_to_tree (&tmp, start, stop, highprio));
      if (limit)
        nbsinks--;
    }

    *stack = tmp;
  }

beach:
  GST_DEBUG_OBJECT (object,
      "*start:%" GST_TIME_FORMAT " *stop:%" GST_TIME_FORMAT
      " priority:%u", GST_TIME_ARGS (*start), GST_TIME_ARGS (*stop), *highprio);

  return ret;
}

/*
 * get_stack_list:
 * @self: The #GnlComposition
 * @timestamp: The #GstClockTime to look at
 * @priority: The priority level to start looking from
 * @activeonly: Only look for active elements if TRUE
 * @start: The biggest start time of the objects in the stack
 * @stop: The smallest stop time of the objects in the stack
 * @highprio: The highest priority in the stack
 *
 * Not MT-safe, you should take the objects lock before calling it.
 * Returns: A tree of #GNode sorted in priority order, corresponding
 * to the given search arguments. The returned value can be #NULL.
 *
 * WITH OBJECTS LOCK TAKEN
 */
static GNode *
get_stack_list (GnlComposition * self, GstClockTime timestamp,
    guint32 priority, gboolean activeonly, GstClockTime * start,
    GstClockTime * stop, guint * highprio)
{
  GList *tmp;
  GList *stack = NULL;
  GNode *ret = NULL;
  GstClockTime nstart = GST_CLOCK_TIME_NONE;
  GstClockTime nstop = GST_CLOCK_TIME_NONE;
  GstClockTime first_out_of_stack = GST_CLOCK_TIME_NONE;
  guint32 highest = 0;
  gboolean reverse = (self->pending_segment.rate < 0.0);

  GST_DEBUG_OBJECT (self,
      "timestamp:%" GST_TIME_FORMAT ", priority:%u, activeonly:%d",
      GST_TIME_ARGS (timestamp), priority, activeonly);

  GST_LOG ("objects_start:%p objects_stop:%p", self->objects_start,
      self->objects_stop);

  if (reverse) {
    for (tmp = self->objects_stop; tmp; tmp = g_list_next (tmp)) {
      GnlObject *object = (GnlObject *) tmp->data;

      GST_LOG_OBJECT (object,
          "start: %" GST_TIME_FORMAT ", stop:%" GST_TIME_FORMAT " , duration:%"
          GST_TIME_FORMAT ", priority:%u, active:%d",
          GST_TIME_ARGS (object->start), GST_TIME_ARGS (object->stop),
          GST_TIME_ARGS (object->duration), object->priority, object->active);

      if (object->stop >= timestamp) {
        if ((object->start < timestamp) &&
            (object->priority >= priority) &&
            ((!activeonly) || (object->active))) {
          GST_LOG_OBJECT (self, "adding %s: sorted to the stack",
              GST_OBJECT_NAME (object));
          stack = g_list_insert_sorted (stack, object,
              (GCompareFunc) priority_comp);
          if (GNL_IS_OPERATION (object))
            gnl_operation_update_base_time (GNL_OPERATION (object), timestamp);
        }
      } else {
        GST_LOG_OBJECT (self, "too far, stopping iteration");
        first_out_of_stack = object->stop;
        break;
      }
    }
  } else {
    for (tmp = self->objects_start; tmp; tmp = g_list_next (tmp)) {
      GnlObject *object = (GnlObject *) tmp->data;

      GST_LOG_OBJECT (object,
          "start: %" GST_TIME_FORMAT " , stop:%" GST_TIME_FORMAT " , duration:%"
          GST_TIME_FORMAT ", priority:%u", GST_TIME_ARGS (object->start),
          GST_TIME_ARGS (object->stop), GST_TIME_ARGS (object->duration),
          object->priority);

      if (object->start <= timestamp) {
        if ((object->stop > timestamp) &&
            (object->priority >= priority) &&
            ((!activeonly) || (object->active))) {
          GST_LOG_OBJECT (self, "adding %s: sorted to the stack",
              GST_OBJECT_NAME (object));
          stack = g_list_insert_sorted (stack, object,
              (GCompareFunc) priority_comp);
          if (GNL_IS_OPERATION (object))
            gnl_operation_update_base_time (GNL_OPERATION (object), timestamp);
        }
      } else {
        GST_LOG_OBJECT (self, "too far, stopping iteration");
        first_out_of_stack = object->start;
        break;
      }
    }
  }

  /* Insert the expandables */
  if (G_LIKELY (timestamp < GNL_OBJECT_STOP (self)))
    for (tmp = self->expandables; tmp; tmp = tmp->next) {
      GST_DEBUG_OBJECT (self, "Adding expandable %s sorted to the list",
          GST_OBJECT_NAME (tmp->data));
      stack = g_list_insert_sorted (stack, tmp->data,
          (GCompareFunc) priority_comp);
      if (GNL_IS_OPERATION (tmp->data))
        gnl_operation_update_base_time (GNL_OPERATION (tmp->data), timestamp);
    }

  /* convert that list to a stack */
  tmp = stack;
  ret = convert_list_to_tree (&tmp, &nstart, &nstop, &highest);
  if (GST_CLOCK_TIME_IS_VALID (first_out_of_stack)) {
    if (reverse && nstart < first_out_of_stack)
      nstart = first_out_of_stack;
    else if (!reverse && nstop > first_out_of_stack)
      nstop = first_out_of_stack;
  }

  GST_DEBUG ("nstart:%" GST_TIME_FORMAT ", nstop:%" GST_TIME_FORMAT,
      GST_TIME_ARGS (nstart), GST_TIME_ARGS (nstop));

  if (*stop)
    *stop = nstop;
  if (*start)
    *start = nstart;
  if (highprio)
    *highprio = highest;

  g_list_free (stack);

  return ret;
}

static void
refine_start_stop_in_region_above_priority (GnlComposition * self,
    GstClockTime timestamp, GstClockTime start,
    GstClockTime stop,
    GstClockTime * rstart, GstClockTime * rstop, guint32 priority)
{
  GList *tmp;
  GnlObject *object;
  GstClockTime nstart = start, nstop = stop;

  GST_DEBUG_OBJECT (self,
      "timestamp:%" GST_TIME_FORMAT " start: %" GST_TIME_FORMAT " stop: %"
      GST_TIME_FORMAT " priority:%u", GST_TIME_ARGS (timestamp),
      GST_TIME_ARGS (start), GST_TIME_ARGS (stop), priority);

  for (tmp = self->objects_start; tmp; tmp = tmp->next) {
    object = (GnlObject *) tmp->data;

    GST_LOG_OBJECT (object, "START %" GST_TIME_FORMAT "--%" GST_TIME_FORMAT,
        GST_TIME_ARGS (object->start), GST_TIME_ARGS (object->stop));

    if ((object->priority >= priority) || (!object->active))
      continue;

    if (object->start <= timestamp)
      continue;

    if (object->start >= nstop)
      continue;

    nstop = object->start;

    GST_DEBUG_OBJECT (self,
        "START Found %s [prio:%u] at %" GST_TIME_FORMAT,
        GST_OBJECT_NAME (object), object->priority,
        GST_TIME_ARGS (object->start));

    break;
  }

  for (tmp = self->objects_stop; tmp; tmp = tmp->next) {
    object = (GnlObject *) tmp->data;

    GST_LOG_OBJECT (object, "STOP %" GST_TIME_FORMAT "--%" GST_TIME_FORMAT,
        GST_TIME_ARGS (object->start), GST_TIME_ARGS (object->stop));

    if ((object->priority >= priority) || (!object->active))
      continue;

    if (object->stop >= timestamp)
      continue;

    if (object->stop <= nstart)
      continue;

    nstart = object->stop;

    GST_DEBUG_OBJECT (self,
        "STOP Found %s [prio:%u] at %" GST_TIME_FORMAT,
        GST_OBJECT_NAME (object), object->priority,
        GST_TIME_ARGS (object->start));

    break;
  }

  if (*rstart)
    *rstart = nstart;

  if (*rstop)
    *rstop = nstop;
}

/*
 * get_clean_toplevel_stack:
 * @self: The #GnlComposition
 * @timestamp: The #GstClockTime to look at
 * @stop_time: Pointer to a #GstClockTime for min stop time of returned stack
 * @start_time: Pointer to a #GstClockTime for greatest start time of returned stack
 *
 * Returns: The new current stack for the given #GnlComposition and @timestamp.
 *
 * WITH OBJECTS LOCK TAKEN
 */
static GNode *
get_clean_toplevel_stack (GnlComposition * self, GstClockTime * timestamp,
    GstClockTime * start_time, GstClockTime * stop_time)
{
  GNode *stack = NULL;
  GstClockTime start = G_MAXUINT64;
  GstClockTime stop = G_MAXUINT64;
  guint highprio;
  gboolean reverse = (self->pending_segment.rate < 0.0);

  GST_DEBUG_OBJECT (self, "timestamp:%" GST_TIME_FORMAT,
      GST_TIME_ARGS (*timestamp));
  GST_DEBUG ("start:%" GST_TIME_FORMAT ", stop:%" GST_TIME_FORMAT,
      GST_TIME_ARGS (start), GST_TIME_ARGS (stop));

  stack = get_stack_list (self, *timestamp, 0, TRUE, &start, &stop, &highprio);

  if (!stack &&
      ((reverse && (*timestamp > _get_composition_start (self))) ||
          (!reverse && (*timestamp < _get_composition_stop (self))))) {
    GST_ELEMENT_ERROR (self, STREAM, WRONG_TYPE,
        ("Gaps ( at %" GST_TIME_FORMAT
            ") in the stream is not supported, the application is responsible"
            " for filling them", GST_TIME_ARGS (*timestamp)),
        ("Gap in the composition this should never"
            "append, make sure to fill them"));

    return NULL;
  }

  GST_DEBUG ("start:%" GST_TIME_FORMAT ", stop:%" GST_TIME_FORMAT,
      GST_TIME_ARGS (start), GST_TIME_ARGS (stop));

  if (stack) {
    guint32 top_priority = GNL_OBJECT_PRIORITY (stack->data);

    /* Figure out if there's anything blocking us with smaller priority */
    refine_start_stop_in_region_above_priority (self, *timestamp, start,
        stop, &start, &stop, (highprio == 0) ? top_priority : highprio);
  }

  if (*stop_time) {
    if (stack)
      *stop_time = stop;
    else
      *stop_time = 0;
  }

  if (*start_time) {
    if (stack)
      *start_time = start;
    else
      *start_time = 0;
  }

  GST_DEBUG_OBJECT (self,
      "Returning timestamp:%" GST_TIME_FORMAT " , start_time:%"
      GST_TIME_FORMAT " , stop_time:%" GST_TIME_FORMAT,
      GST_TIME_ARGS (*timestamp), GST_TIME_ARGS (*start_time),
      GST_TIME_ARGS (*stop_time));

  return stack;
}

static gboolean
are_same_stacks (GNode * stack1, GNode * stack2)
{
  gboolean res = FALSE;

  /* TODO : FIXME : we should also compare start/inpoint */
  /* stacks are not equal if one of them is NULL but not the other */
  if ((!stack1 && stack2) || (stack1 && !stack2))
    goto beach;

  if (stack1 && stack2) {
    GNode *child1, *child2;

    /* if they don't contain the same source, not equal */
    if (!(stack1->data == stack2->data))
      goto beach;

    /* if they don't have the same number of children, not equal */
    if (!(g_node_n_children (stack1) == g_node_n_children (stack2)))
      goto beach;

    child1 = stack1->children;
    child2 = stack2->children;
    while (child1 && child2) {
      if (!(are_same_stacks (child1, child2)))
        goto beach;
      child1 = g_node_next_sibling (child1);
      child2 = g_node_next_sibling (child2);
    }

    /* if there's a difference in child number, stacks are not equal */
    if (child1 || child2)
      goto beach;
  }

  /* if stack1 AND stack2 are NULL, then they're equal (both empty) */
  res = TRUE;

beach:
  GST_LOG ("Stacks are equal : %d", res);

  return res;
}

/* gnl_composition_ghost_pad_set_target:
 * target: The target #GstPad. The refcount will be decremented (given to the ghostpad).
 * entry: The GnlCompositionEntry to which the pad belongs
 */
static void
gnl_composition_ghost_pad_set_target (GnlComposition * self, GstPad * target,
    GnlCompositionEntry * entry)
{
  GnlObject *gnlobject = (GnlObject *) self;

  GstPad *ptarget =
      gst_ghost_pad_get_target (GST_GHOST_PAD (gnlobject->srcpad));

  if (target)
    GST_DEBUG_OBJECT (self, "target:%s:%s", GST_DEBUG_PAD_NAME (target));
  else
    GST_DEBUG_OBJECT (self, "Removing target");


  if (ptarget && ptarget == target) {
    GST_DEBUG_OBJECT (self,
        "Target of ghostpad is the same as existing one, not changing");
    gst_object_unref (ptarget);
    return;
  }

  /* Unset previous target */
  if (ptarget) {
    GST_DEBUG_OBJECT (self, "Previous target was %s:%s",
        GST_DEBUG_PAD_NAME (ptarget));

    if (!self->toplevelentry->probeid) {
      /* If it's not blocked, block it */
      self->toplevelentry->probeid =
          gst_pad_add_probe (ptarget,
          GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM | GST_PAD_PROBE_TYPE_IDLE,
          (GstPadProbeCallback) pad_blocked, self, NULL);
    }

    if (!self->toplevelentry->dataprobeid) {
      self->toplevelentry->dataprobeid = gst_pad_add_probe (ptarget,
          GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST |
          GST_PAD_PROBE_TYPE_EVENT_BOTH, (GstPadProbeCallback) drop_data,
          self->toplevelentry, NULL);
    }

    /* remove event probe */
    if (self->ghosteventprobe) {
      gst_pad_remove_probe (ptarget, self->ghosteventprobe);
      self->ghosteventprobe = 0;
    }
    gst_object_unref (ptarget);
  }

  /* Actually set the target */
  gnl_object_ghost_pad_set_target ((GnlObject *) self, gnlobject->srcpad,
      target);

  /* Set top-level entry (will be NULL if unsetting) */
  self->toplevelentry = entry;

  if (target && (self->ghosteventprobe == 0)) {
    self->ghosteventprobe =
        gst_pad_add_probe (target,
        GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM | GST_PAD_PROBE_TYPE_EVENT_FLUSH,
        (GstPadProbeCallback) ghost_event_probe_handler, self, NULL);
    GST_DEBUG_OBJECT (self, "added event probe %lu", self->ghosteventprobe);
  }

  GST_DEBUG_OBJECT (self, "END");
}


/*
 * recursive depth-first compare stack function on old stack
 *
 * _ Add no-longer used objects to the deactivate list
 * _ unlink child-parent relations that have changed (not same parent, or not same order)
 * _ blocks available source pads
 *
 * FIXME : flush_downstream is only used for the root element.
 *    It is TRUE all the time except when the update is done from a seek
 *
 * WITH OBJECTS LOCK TAKEN
 */
static GList *
compare_deactivate_single_node (GnlComposition * self, GNode * node,
    GNode * newstack, gboolean flush_downstream)
{
  GNode *child;
  GNode *newnode = NULL;        /* Same node in newstack */
  GnlObject *oldparent;
  GList *deactivate = NULL;
  GnlObject *oldobj = NULL;
  GstPad *srcpad = NULL;

  if (G_UNLIKELY (!node))
    return NULL;

  /* The former parent GnlObject (i.e. downstream) of the given node */
  oldparent = G_NODE_IS_ROOT (node) ? NULL : (GnlObject *) node->parent->data;

  /* The former GnlObject */
  oldobj = (GnlObject *) node->data;

  /* The node corresponding to oldobj in the new stack */
  if (newstack)
    newnode = g_node_find (newstack, G_IN_ORDER, G_TRAVERSE_ALL, oldobj);

  GST_DEBUG_OBJECT (self, "oldobj:%s",
      GST_ELEMENT_NAME ((GstElement *) oldobj));

  if (G_LIKELY (oldobj->srcpad)) {
    GstPad *peerpad = NULL;
    GnlCompositionEntry *entry = COMP_ENTRY (self, oldobj);

    /* 1. Block source pad
     *   This makes sure that no data/event flow will come out of this element after this
     *   point.
     *
     * If entry is NULL, this means the element is in the process of being removed.
     */
    if (entry && !entry->probeid) {
      GST_LOG_OBJECT (self, "Setting BLOCKING probe on %s:%s",
          GST_DEBUG_PAD_NAME (srcpad));
      entry->probeid =
          gst_pad_add_probe (srcpad,
          GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM | GST_PAD_PROBE_TYPE_IDLE,
          (GstPadProbeCallback) pad_blocked, self, NULL);
    }
    if (entry && !entry->dataprobeid) {
      entry->dataprobeid = gst_pad_add_probe (srcpad,
          GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST |
          GST_PAD_PROBE_TYPE_EVENT_BOTH, (GstPadProbeCallback) drop_data, entry,
          NULL);
    }

    /* 2. If we have to flush_downstream or we have a parent, flush downstream
     *   This ensures the streaming thread going through the current object has
     *   either stopped or is blocking against the source pad. */
    if ((flush_downstream || oldparent)
        && (peerpad = gst_pad_get_peer (srcpad))) {
      GST_LOG_OBJECT (self, "Sending flush start/stop downstream ");
      gst_pad_send_event (peerpad, gst_event_new_flush_start ());
      gst_pad_send_event (peerpad, gst_event_new_flush_stop (TRUE));
      GST_DEBUG_OBJECT (self, "DONE Sending flush events downstream");
      gst_object_unref (peerpad);
    }

  }

  /* 3. Unlink from the parent if we've changed position */
  GST_LOG_OBJECT (self,
      "Checking if we need to unlink from downstream element");
  if (G_UNLIKELY (!oldparent)) {
    GST_LOG_OBJECT (self, "Top-level object");
    /* for top-level objects we just set the ghostpad target to NULL */
    GST_LOG_OBJECT (self, "Setting ghostpad target to NULL");
    gnl_composition_ghost_pad_set_target (self, NULL, NULL);
  } else {
    GnlObject *newparent = NULL;

    GST_LOG_OBJECT (self, "non-toplevel object");

    if (newnode)
      newparent =
          G_NODE_IS_ROOT (newnode) ? NULL : (GnlObject *) newnode->parent->data;

    if ((!newnode) || (oldparent != newparent) ||
        (newparent &&
            (g_node_child_index (node,
                    oldobj) != g_node_child_index (newnode, oldobj)))) {
      GstPad *peerpad = NULL;

      GST_LOG_OBJECT (self, "Topology changed, unlinking from downstream");

      if (srcpad && (peerpad = gst_pad_get_peer (srcpad))) {
        GST_LOG_OBJECT (peerpad, "Sending flush start/stop");
        gst_pad_send_event (peerpad, gst_event_new_flush_start ());
        gst_pad_send_event (peerpad, gst_event_new_flush_stop (TRUE));
        gst_pad_unlink (srcpad, peerpad);
        gst_object_unref (peerpad);
      }
    } else
      GST_LOG_OBJECT (self, "Topology unchanged");
  }

  /* 4. If we're dealing with an operation, call this method recursively on it */
  if (G_UNLIKELY (GNL_IS_OPERATION (oldobj))) {
    GST_LOG_OBJECT (self,
        "Object is an operation, recursively calling on children");
    for (child = node->children; child; child = child->next) {
      GList *newdeac = compare_deactivate_single_node (self, child, newstack,
          flush_downstream);

      if (newdeac)
        deactivate = g_list_concat (deactivate, newdeac);
    }
  }

  /* 5. If object isn't used anymore, add it to the list of objects to deactivate */
  if (G_LIKELY (!newnode)) {
    GST_LOG_OBJECT (self, "Object doesn't exist in new stack");
    deactivate = g_list_prepend (deactivate, oldobj);
  }

  GST_LOG_OBJECT (self, "done with object %s",
      GST_ELEMENT_NAME (GST_ELEMENT (oldobj)));

  return deactivate;
}

/*
 * recursive depth-first relink stack function on new stack
 *
 * _ relink nodes with changed parent/order
 * _ links new nodes with parents
 * _ unblocks available source pads (except for toplevel)
 *
 * WITH OBJECTS LOCK TAKEN
 */
static void
compare_relink_single_node (GnlComposition * self, GNode * node,
    GNode * oldstack)
{
  GNode *child;
  GNode *oldnode = NULL;
  GnlObject *newobj;
  GnlObject *newparent;
  GnlObject *oldparent = NULL;
  GstPad *sinkpad = NULL;
  GnlCompositionEntry *entry;

  if (G_UNLIKELY (!node))
    return;

  newparent = G_NODE_IS_ROOT (node) ? NULL : (GnlObject *) node->parent->data;
  newobj = (GnlObject *) node->data;
  if (oldstack) {
    oldnode = g_node_find (oldstack, G_IN_ORDER, G_TRAVERSE_ALL, newobj);
    if (oldnode)
      oldparent =
          G_NODE_IS_ROOT (oldnode) ? NULL : (GnlObject *) oldnode->parent->data;
  }

  GST_DEBUG_OBJECT (self, "newobj:%s",
      GST_ELEMENT_NAME ((GstElement *) newobj));

  /* 1. Make sure the source pad is blocked for new objects */
  if (G_UNLIKELY (!oldnode)) {
    GnlCompositionEntry *oldentry = COMP_ENTRY (self, newobj);
    if (!oldentry->probeid) {
      GST_LOG_OBJECT (self, "block_async(%s:%s, TRUE)",
          GST_DEBUG_PAD_NAME (newobj->srcpad));
      oldentry->probeid =
          gst_pad_add_probe (newobj->srcpad,
          GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM | GST_PAD_PROBE_TYPE_IDLE,
          (GstPadProbeCallback) pad_blocked, self, NULL);
    }
    if (!oldentry->dataprobeid) {
      oldentry->dataprobeid = gst_pad_add_probe (newobj->srcpad,
          GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST |
          GST_PAD_PROBE_TYPE_EVENT_BOTH, (GstPadProbeCallback) drop_data,
          oldentry, NULL);
    }
  }

  entry = COMP_ENTRY (self, newobj);
  /* 2. link to parent if needed. */
  GST_LOG_OBJECT (self, "has a valid source pad");

  /* Post processing */
  if ((oldparent != newparent) ||
      (oldparent && newparent &&
          (g_node_child_index (node,
                  newobj) != g_node_child_index (oldnode, newobj)))) {
    GST_LOG_OBJECT (self,
        "not same parent, or same parent but in different order");
    /* relink to new parent in required order */
    if (newparent) {
      GstPad *sinkpad;
      GST_LOG_OBJECT (self, "Linking %s and %s",
          GST_ELEMENT_NAME (GST_ELEMENT (newobj)),
          GST_ELEMENT_NAME (GST_ELEMENT (newparent)));
      sinkpad = get_unlinked_sink_ghost_pad ((GnlOperation *) newparent);
      if (G_UNLIKELY (sinkpad == NULL)) {
        GST_WARNING_OBJECT (self,
            "Couldn't find an unlinked sinkpad from %s",
            GST_ELEMENT_NAME (newparent));
      } else {
        if (G_UNLIKELY (gst_pad_link_full (newobj->srcpad, sinkpad,
                    GST_PAD_LINK_CHECK_NOTHING) != GST_PAD_LINK_OK)) {
          GST_WARNING_OBJECT (self, "Failed to link pads %s:%s - %s:%s",
              GST_DEBUG_PAD_NAME (newobj->srcpad),
              GST_DEBUG_PAD_NAME (sinkpad));
        }
        gst_object_unref (sinkpad);
      }
    }
  } else
    GST_LOG_OBJECT (newobj, "Same parent and same position in the new stack");
  /* If there's an operation, inform it about priority changes */
  if (newparent) {
    sinkpad = gst_pad_get_peer (newobj->srcpad);
    gnl_operation_signal_input_priority_changed ((GnlOperation *)
        newparent, sinkpad, newobj->priority);
    gst_object_unref (sinkpad);
  }

  /* 3. Handle children */
  if (GNL_IS_OPERATION (newobj)) {
    guint nbchildren = g_node_n_children (node);
    GnlOperation *oper = (GnlOperation *) newobj;
    GST_LOG_OBJECT (newobj, "is a %s operation, analyzing the %d children",
        oper->dynamicsinks ? "dynamic" : "regular", nbchildren);
    /* Update the operation's number of sinks, that will make it have the proper
     * number of sink pads to connect the children to. */
    if (oper->dynamicsinks)
      g_object_set (G_OBJECT (newobj), "sinks", nbchildren, NULL);
    for (child = node->children; child; child = child->next)
      compare_relink_single_node (self, child, oldstack);
    if (G_UNLIKELY (nbchildren < oper->num_sinks))
      GST_ERROR
          ("Not enough sinkpads to link all objects to the operation ! %d / %d",
          oper->num_sinks, nbchildren);
    if (G_UNLIKELY (nbchildren == 0))
      GST_ERROR ("Operation has no child objects to be connected to !!!");
    /* Make sure we have enough sinkpads */
  }

  /* 4. Unblock source pad */
  if (!G_NODE_IS_ROOT (node) && entry->probeid) {
    GST_LOG_OBJECT (self, "Unblocking pad %s:%s",
        GST_DEBUG_PAD_NAME (newobj->srcpad));
    gst_pad_remove_probe (newobj->srcpad, entry->probeid);
    entry->probeid = 0;
  }

  GST_LOG_OBJECT (self, "done with object %s",
      GST_ELEMENT_NAME (GST_ELEMENT (newobj)));
}

/*
 * compare_relink_stack:
 * @self: The #GnlComposition
 * @stack: The new stack
 * @flush_downstream: TRUE if the timeline has changed and needs downstream flushes.
 *
 * Compares the given stack to the current one and relinks it if needed.
 *
 * WITH OBJECTS LOCK TAKEN
 *
 * Returns: The #GList of #GnlObject no longer used
 */
static GList *
compare_relink_stack (GnlComposition * self, GNode * stack,
    gboolean flush_downstream)
{
  GList *deactivate = NULL;

  /* 1. Traverse old stack to deactivate no longer used objects */
  deactivate =
      compare_deactivate_single_node (self, self->current_stack, stack,
      flush_downstream);

  /* 2. Traverse new stack to do needed (re)links */
  compare_relink_single_node (self, stack, self->current_stack);

  return deactivate;
}

static void
unlock_activate_stack (GnlComposition * self, GNode * node, GstState state)
{
  GNode *child;

  GST_LOG_OBJECT (self, "object:%s",
      GST_ELEMENT_NAME ((GstElement *) (node->data)));

  gst_element_set_locked_state ((GstElement *) (node->data), FALSE);
  gst_element_set_state (GST_ELEMENT (node->data), state);

  for (child = node->children; child; child = child->next)
    unlock_activate_stack (self, child, state);
}

/*
 * get_new_seek_event:
 *
 * Returns a seek event for the currently configured segment
 * and start/stop values
 *
 * The GstSegment and segment_start|stop must have been configured
 * before calling this function.
 */
static GstEvent *
get_new_seek_event (GnlComposition * self, gboolean initial,
    gboolean updatestoponly)
{
  GstSeekFlags flags = GST_SEEK_FLAG_ACCURATE | GST_SEEK_FLAG_FLUSH;
  gint64 start, stop;
  GstSeekType starttype = GST_SEEK_TYPE_SET;

  GST_DEBUG_OBJECT (self, "initial:%d", initial);
  /* remove the seek flag */
  if (!initial)
    flags |= (GstSeekFlags) self->pending_segment.flags;

  GST_DEBUG_OBJECT (self,
      "private->pending_segment.start:%" GST_TIME_FORMAT " segment_start%"
      GST_TIME_FORMAT, GST_TIME_ARGS (self->pending_segment.start),
      GST_TIME_ARGS (self->current_segment.start));

  GST_DEBUG_OBJECT (self,
      "private->pending_segment.stop:%" GST_TIME_FORMAT
      "self->current_segment.stop %" GST_TIME_FORMAT,
      GST_TIME_ARGS (self->pending_segment.stop),
      GST_TIME_ARGS (self->current_segment.stop));

  start = MAX (self->pending_segment.start, self->current_segment.start);
  stop = GST_CLOCK_TIME_IS_VALID (self->pending_segment.stop)
      ? MIN (self->pending_segment.stop, self->current_segment.stop)
      : self->current_segment.stop;

  if (updatestoponly) {
    starttype = GST_SEEK_TYPE_NONE;
    start = GST_CLOCK_TIME_NONE;
  }

  GST_DEBUG_OBJECT (self,
      "Created new seek event. Flags:%d, start:%" GST_TIME_FORMAT ", stop:%"
      GST_TIME_FORMAT ", rate:%lf", flags, GST_TIME_ARGS (start),
      GST_TIME_ARGS (stop), self->pending_segment.rate);

  return gst_event_new_seek (self->pending_segment.rate,
      self->pending_segment.format, flags, starttype, start, GST_SEEK_TYPE_SET,
      stop);
}

/*
 * update_pipeline:
 * @self: The #GnlComposition
 * @currenttime: The #GstClockTime to update at, can be GST_CLOCK_TIME_NONE.
 * @flush_downstream: Flush downstream if TRUE. Needed for modified timelines.
 *
 * Updates the internal pipeline and properties. If @currenttime is
 * GST_CLOCK_TIME_NONE, it will not modify the current pipeline
 *
 * Returns: FALSE if there was an error updating the pipeline.
 *
 * WITH OBJECTS LOCK TAKEN
 */
static gboolean
update_pipeline (GnlComposition * self, GstClockTime currenttime,
    gboolean flush_downstream)
{
  gboolean startchanged, stopchanged;

  GNode *stack = NULL;
  gboolean ret = TRUE;
  GList *todeactivate = NULL;
  gboolean samestack = FALSE;
  GstState state = GST_STATE (self);
  GstClockTime new_stop = GST_CLOCK_TIME_NONE;
  GstClockTime new_start = GST_CLOCK_TIME_NONE;
  GstState nextstate = (GST_STATE_NEXT (self) == GST_STATE_VOID_PENDING) ?
      GST_STATE (self) : GST_STATE_NEXT (self);

  GST_DEBUG_OBJECT (self,
      "currenttime:%" GST_TIME_FORMAT
      " flush_downstream:%d", GST_TIME_ARGS (currenttime), flush_downstream);

  if (!GST_CLOCK_TIME_IS_VALID (currenttime))
    return FALSE;

  if (state == GST_STATE_NULL && nextstate == GST_STATE_NULL) {
    GST_DEBUG_OBJECT (self, "STATE_NULL: not updating pipeline");
    return FALSE;
  }


  GST_DEBUG_OBJECT (self,
      "now really updating the pipeline, current-state:%s",
      gst_element_state_get_name (state));

  /* 1. Get new stack and compare it to current one */
  stack = get_clean_toplevel_stack (self, &currenttime, &new_start, &new_stop);
  samestack = are_same_stacks (self->current_stack, stack);

  /* 2. If stacks are different, unlink/relink objects */
  if (!samestack)
    todeactivate = compare_relink_stack (self, stack, flush_downstream);

  if (self->pending_segment.rate >= 0.0) {
    startchanged = self->pending_segment.start != currenttime;
    stopchanged = self->pending_segment.stop != new_stop;
  } else {
    startchanged = self->pending_segment.start != new_start;
    stopchanged = self->pending_segment.stop != currenttime;
  }

  /* 3. set new segment_start/stop (the current zone over which the new stack
   *    is valid) */
  if (self->pending_segment.rate >= 0.0) {
    self->pending_segment.start = currenttime;
    self->pending_segment.stop = new_stop;
  } else {
    self->pending_segment.start = new_start;
    self->pending_segment.stop = currenttime;
  }

  /* Invalidate current stack */
  if (self->current_stack)
    g_node_destroy (self->current_stack);
  self->current_stack = NULL;

  /* 5. deactivate unused elements */
  if (todeactivate) {
    GList *tmp;
    GstElement *element;

    GST_DEBUG_OBJECT (self, "De-activating objects no longer used");

    /* state-lock elements no more used */
    for (tmp = todeactivate; tmp; tmp = tmp->next) {
      element = GST_ELEMENT_CAST (tmp->data);

      gst_element_set_state (element, self->deactivated_elements_state);
      gst_element_set_locked_state (element, TRUE);
    }

    g_list_free (todeactivate);

    GST_DEBUG_OBJECT (self, "Finished de-activating objects no longer used");
  }

  /* 6. Unlock all elements in new stack */
  GST_DEBUG_OBJECT (self, "Setting current stack");
  self->current_stack = stack;

  if (!samestack && stack) {
    GST_DEBUG_OBJECT (self, "activating objects in new stack to %s",
        gst_element_state_get_name (nextstate));
    unlock_activate_stack (self, stack, nextstate);
    GST_DEBUG_OBJECT (self, "Finished activating objects in new stack");
  }

  /* 7. Activate stack */
  if (self->current_stack) {
    GstEvent *event;
    GnlObject *topelement = GNL_OBJECT (self->current_stack->data);
    GnlCompositionEntry *topentry = COMP_ENTRY (self, topelement);

    /* 7.1. Create new seek event for newly configured timeline stack */
    if (samestack && (startchanged || stopchanged))
      event =
          get_new_seek_event (self,
          (state == GST_STATE_PLAYING) ? FALSE : TRUE, !startchanged);
    else
      event = get_new_seek_event (self, TRUE, FALSE);

    GST_DEBUG_OBJECT (self,
        "We have a valid toplevel element pad %s:%s",
        GST_DEBUG_PAD_NAME (topelement->srcpad));

    /* Send seek event */
    GST_LOG_OBJECT (self, "sending seek event");
    if (gst_pad_send_event (topelement->srcpad, event)) {
      /* Unconditionnaly set the ghostpad target to pad */
      GST_LOG_OBJECT (self,
          "Setting the composition's ghostpad target to %s:%s",
          GST_DEBUG_PAD_NAME (topelement->srcpad));

      gnl_composition_ghost_pad_set_target (self, topelement->srcpad, topentry);

      if (topentry->probeid) {

        /* unblock top-level pad */
        GST_LOG_OBJECT (self, "About to unblock top-level srcpad");
        gst_pad_remove_probe (topelement->srcpad, topentry->probeid);
        topentry->probeid = 0;
      }
    } else {
      ret = FALSE;
    }
  } else {
    if ((!self->objects_start)) {
      GST_DEBUG_OBJECT (self, "composition is now empty, removing ghostpad");
      self->pending_segment.start = 0;
      self->pending_segment.stop = GST_CLOCK_TIME_NONE;
    }
  }

  self->current_segment = self->pending_segment;
  GST_DEBUG_OBJECT (self, "Returning %d", ret);
  return ret;
}

static inline gboolean
have_to_update_pipeline (GnlComposition * self)
{
  GST_DEBUG_OBJECT (self,
      "segment[%" GST_TIME_FORMAT "--%" GST_TIME_FORMAT "] current[%"
      GST_TIME_FORMAT "--%" GST_TIME_FORMAT "]",
      GST_TIME_ARGS (self->pending_segment.start),
      GST_TIME_ARGS (self->pending_segment.stop),
      GST_TIME_ARGS (self->current_segment.start),
      GST_TIME_ARGS (self->current_segment.start));

  if (self->pending_segment.start < self->current_segment.start)
    return TRUE;

  if (self->pending_segment.start < self->current_segment.stop)
    return TRUE;

  return FALSE;
}

static gboolean
update_base_time (GNode * node, GstClockTime * timestamp)
{
  if (GNL_IS_OPERATION (node->data))
    gnl_operation_update_base_time (GNL_OPERATION (node->data), *timestamp);

  return FALSE;
}

static void
update_operations_base_time (GnlComposition * self, gboolean reverse)
{
  GstClockTime timestamp;

  if (reverse)
    timestamp = self->pending_segment.stop;
  else
    timestamp = self->pending_segment.start;

  g_node_traverse (self->current_stack, G_IN_ORDER, G_TRAVERSE_ALL, -1,
      (GNodeTraverseFunc) update_base_time, &timestamp);
}

static gboolean
seek_handling (GnlComposition * self, gboolean update)
{
  GST_DEBUG_OBJECT (self, "update:%d", update);

  COMP_OBJECTS_LOCK (self);
  if (update || have_to_update_pipeline (self)) {
    if (self->pending_segment.rate >= 0.0)
      update_pipeline (self, self->pending_segment.start, !update);
    else
      update_pipeline (self, self->pending_segment.stop, !update);
  } else {
    update_operations_base_time (self, !(self->pending_segment.rate >= 0.0));
  }
  COMP_OBJECTS_UNLOCK (self);

  return TRUE;
}

/*
 * Child modification updates
 */

static gboolean
_update_pipeline_gsource (GnlComposition * self)
{
  gboolean reverse;
  /* Set up a non-initial seek on segment_stop */

  reverse = (self->pending_segment.rate < 0.0);
  if (!reverse) {
    GST_DEBUG_OBJECT (self,
        "Setting segment.start to segment_stop:%" GST_TIME_FORMAT,
        GST_TIME_ARGS (self->pending_segment.stop));
    self->current_segment.start = self->pending_segment.stop;
  } else {
    GST_DEBUG_OBJECT (self,
        "Setting segment.stop to segment_start:%" GST_TIME_FORMAT,
        GST_TIME_ARGS (self->pending_segment.start));
    self->current_segment.stop = self->pending_segment.start;
  }

  seek_handling (self, TRUE);

  if (!self->current_stack) {
    /* If we're at the end, post SEGMENT_DONE, or push EOS */
    GST_DEBUG_OBJECT (self, "Nothing else to play");

    if (!(self->pending_segment.flags & GST_SEEK_FLAG_SEGMENT)) {
      GST_DEBUG_OBJECT (self, "Real EOS should be sent now");
    } else if (self->pending_segment.flags & GST_SEEK_FLAG_SEGMENT) {
      gint64 epos;

      if (GST_CLOCK_TIME_IS_VALID (self->pending_segment.stop))
        epos = (MIN (self->pending_segment.stop, GNL_OBJECT_STOP (self)));
      else
        epos = GNL_OBJECT_STOP (self);

      GST_LOG_OBJECT (self, "Emitting segment done pos %" GST_TIME_FORMAT,
          GST_TIME_ARGS (epos));
      gst_element_post_message (GST_ELEMENT_CAST (self),
          gst_message_new_segment_done (GST_OBJECT (self),
              self->pending_segment.format, epos));
      gst_pad_push_event (GNL_OBJECT (self)->srcpad,
          gst_event_new_segment_done (self->pending_segment.format, epos));
    }
  }

  return G_SOURCE_REMOVE;
}

static void
_remove_all_sources (GnlComposition * self)
{
  GSource *source;

  MAIN_CONTEXT_LOCK (self);
  while ((source =
          g_main_context_find_source_by_user_data (self->mcontext, self))) {
    g_source_destroy (source);
  }
  MAIN_CONTEXT_UNLOCK (self);
}

static inline void
_add_update_pipeline_gsource (GnlComposition * self)
{
  MAIN_CONTEXT_LOCK (self);
  g_main_context_invoke (self->mcontext,
      (GSourceFunc) _update_pipeline_gsource, self);
  MAIN_CONTEXT_UNLOCK (self);
}

static gboolean
_initialize_pipeline_func (GnlComposition * self)
{
  update_pipeline (self, _get_composition_start (self), TRUE);

  return G_SOURCE_REMOVE;
}

static gboolean
set_child_caps (GValue * item, GValue * ret G_GNUC_UNUSED, GnlObject * self)
{
  GstElement *child = g_value_get_object (item);

  gnl_object_set_caps ((GnlObject *) child, self->caps);

  return TRUE;
}

static gboolean
unblock_child_pads (GValue * item, GValue * ret G_GNUC_UNUSED,
    GnlComposition * comp)
{
  GstElement *child = g_value_get_object (item);
  GnlCompositionEntry *entry = COMP_ENTRY (comp, child);

  GST_DEBUG_OBJECT (child, "unblocking pads");

  if (entry->probeid) {
    gst_pad_remove_probe (GNL_OBJECT (child)->srcpad, entry->probeid);
    entry->probeid = 0;
  }
  return TRUE;
}

static void
unblock_children (GnlComposition * comp)
{
  GstIterator *children;

  children = gst_bin_iterate_elements (GST_BIN (comp));

retry:
  if (G_UNLIKELY (gst_iterator_fold (children,
              (GstIteratorFoldFunction) unblock_child_pads, NULL,
              comp) == GST_ITERATOR_RESYNC)) {
    gst_iterator_resync (children);
    goto retry;
  }
  gst_iterator_free (children);
}

static void
_start (GnlComposition * self)
{
  GstIterator *children;

  _reset (self);

  /* state-lock all elements */
  GST_DEBUG_OBJECT (self,
      "Setting all children to READY and locking their state");

  children = gst_bin_iterate_elements (GST_BIN (self));
  while (G_UNLIKELY (gst_iterator_fold (children,
              (GstIteratorFoldFunction) lock_child_state, NULL,
              NULL) == GST_ITERATOR_RESYNC)) {

    GST_INFO_OBJECT (self, "Resyncing to lock children state");
    gst_iterator_resync (children);
  }
  gst_iterator_free (children);

  /* Set caps on all objects */
  if (G_UNLIKELY (!gst_caps_is_any (GNL_OBJECT (self)->caps))) {
    children = gst_bin_iterate_elements (GST_BIN (self));

    while (G_UNLIKELY (gst_iterator_fold (children,
                (GstIteratorFoldFunction) set_child_caps, NULL,
                self) == GST_ITERATOR_RESYNC)) {
      GST_INFO_OBJECT (self, "Resyncing to set children caps");
      gst_iterator_resync (children);
    }
    gst_iterator_free (children);
  }

  MAIN_CONTEXT_LOCK (self);
  g_main_context_invoke (self->mcontext,
      (GSourceFunc) _initialize_pipeline_func, self);
  MAIN_CONTEXT_UNLOCK (self);
}

static void
iterate_main_context_func (GnlComposition * self)
{
  if (self->running == FALSE) {
    GST_DEBUG_OBJECT (self, "Not running anymore");

    return;
  }

  g_main_context_iteration (self->mcontext, TRUE);
}

/*****************************************
 *  GstElement vmethods implementation   *
 *****************************************/
static GstStateChangeReturn
_change_state (GstElement * element, GstStateChange transition)
{
  GnlComposition *self = (GnlComposition *) element;
  GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;

  GST_DEBUG_OBJECT (self, "%s => %s",
      gst_element_state_get_name (GST_STATE_TRANSITION_CURRENT (transition)),
      gst_element_state_get_name (GST_STATE_TRANSITION_NEXT (transition)));

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      _start (self);
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      _reset (self);
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      _reset (self);
      self->running = FALSE;
      _remove_all_sources (self);
      _add_update_pipeline_gsource (self);
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  if (ret == GST_STATE_CHANGE_FAILURE)
    return ret;

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
    case GST_STATE_CHANGE_READY_TO_NULL:
      unblock_children (self);
      break;
    default:
      break;
  }

  return ret;
}

static void
_start_srcpad_task (GnlComposition * self)
{
  GST_INFO_OBJECT (self, "Starting srcpad task");

  self->running = TRUE;
  gst_pad_start_task (GST_PAD (GNL_OBJECT (self)->srcpad),
      (GstTaskFunction) iterate_main_context_func, self, NULL);
}

static gboolean
_stop_srcpad_task (GnlComposition * self, GstEvent * flush_start)
{
  gboolean res = TRUE;

  GST_INFO_OBJECT (self, "%s srcpad task",
      flush_start ? "Pausing" : "Stopping");

  self->running = FALSE;

  /*  Clean the stack of GSource set on the MainContext */
  g_main_context_wakeup (self->mcontext);
  _remove_all_sources (self);
  if (flush_start)
    res = gst_pad_push_event (GNL_OBJECT (self)->srcpad, flush_start);

  gst_pad_stop_task (GNL_OBJECT (self)->srcpad);

  return res;
}

/*****************************************
 *     GstPad functions implementation   *
 *****************************************/
static gboolean
src_activate_mode (GstPad * pad,
    GstObject * parent, GstPadMode mode, gboolean active)
{
  GnlComposition *self = GNL_COMPOSITION (parent);

  if (active == TRUE) {
    switch (mode) {
      case GST_PAD_MODE_PUSH:
      {
        GST_INFO_OBJECT (pad, "Activating pad!");
        _start_srcpad_task (self);
        return TRUE;
      }
      default:
      {
        GST_ERROR_OBJECT (pad, "Only supported mode is PUSH");
        return FALSE;
      }
    }
  }

  /* deactivating */
  GST_INFO_OBJECT (self, "Deactivating srcpad");
  _stop_srcpad_task (self, FALSE);

  return TRUE;
}

/********************************
 *     GObject implementation   *
 ********************************/

static void
_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GnlComposition *self = GNL_COMPOSITION (object);

  switch (prop_id) {
    case PROP_DEACTIVATED_ELEMENTS_STATE:
      self->deactivated_elements_state = g_value_get_enum (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GnlComposition *self = GNL_COMPOSITION (object);

  switch (prop_id) {
    case PROP_DEACTIVATED_ELEMENTS_STATE:
      g_value_set_enum (value, self->deactivated_elements_state);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
_dispose (GObject * object)
{
  GnlComposition *self = GNL_COMPOSITION (object);

  if (self->dispose_has_run)
    return;

  self->dispose_has_run = TRUE;

  if (self->expandables) {
    g_list_free (self->expandables);
    self->expandables = NULL;
  }

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
_finalize (GObject * object)
{
  GnlComposition *self = GNL_COMPOSITION (object);

  GST_INFO ("finalize");

  COMP_OBJECTS_LOCK (self);
  g_list_free (self->objects_start);
  g_list_free (self->objects_stop);

  if (self->current_stack)
    g_node_destroy (self->current_stack);

  g_hash_table_destroy (self->objects_hash);
  COMP_OBJECTS_UNLOCK (self);

  g_mutex_clear (&self->objects_lock);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}



static gboolean
gnl_composition_add_object (GstBin * bin, GstElement * element)
{
  gboolean ret;
  GnlCompositionEntry *entry;
  GnlComposition *comp = (GnlComposition *) bin;

  /* we only accept GnlObject */
  g_return_val_if_fail (GNL_IS_OBJECT (element), FALSE);

  GST_DEBUG_OBJECT (bin, "element %s", GST_OBJECT_NAME (element));
  GST_DEBUG_OBJECT (element, "%" GST_TIME_FORMAT "--%" GST_TIME_FORMAT,
      GST_TIME_ARGS (GNL_OBJECT_START (element)),
      GST_TIME_ARGS (GNL_OBJECT_STOP (element)));

  gst_object_ref (element);

  COMP_OBJECTS_LOCK (comp);

  if ((GNL_OBJECT_IS_EXPANDABLE (element)) &&
      g_list_find (comp->expandables, element)) {
    GST_WARNING_OBJECT (comp,
        "We already have an expandable, remove it before adding new one");
    ret = FALSE;

    goto chiringuito;
  }

  /* Call parent class ::add_element() */
  ret = GST_BIN_CLASS (parent_class)->add_element (bin, element);

  gnl_object_set_commit_needed (GNL_OBJECT (comp));

  if (!ret) {
    GST_WARNING_OBJECT (bin, "couldn't add element");
    goto chiringuito;
  }

  /* lock state of child ! */
  GST_LOG_OBJECT (bin, "Locking state of %s", GST_ELEMENT_NAME (element));
  gst_element_set_locked_state (element, TRUE);

  /* wrap new element in a GnlCompositionEntry ... */
  entry = g_slice_new0 (GnlCompositionEntry);
  entry->object = (GnlObject *) element;
  entry->comp = comp;

  if (GNL_OBJECT_IS_EXPANDABLE (element)) {
    /* Only react on non-default objects properties */
    g_object_set (element,
        "start", (GstClockTime) 0,
        "inpoint", (GstClockTime) 0,
        "duration", (GstClockTimeDiff) GNL_OBJECT_STOP (comp), NULL);

    GST_INFO_OBJECT (element, "Used as expandable, commiting now");
    gnl_object_commit (GNL_OBJECT (element), FALSE);
  }

  /* ...and add it to the hash table */
  g_hash_table_insert (comp->objects_hash, element, entry);

  if (!entry->probeid) {
    GST_DEBUG_OBJECT (comp, "pad %s:%s was added, blocking it",
        GST_DEBUG_PAD_NAME (entry->object->srcpad));
    entry->probeid =
        gst_pad_add_probe (entry->object->srcpad,
        GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM | GST_PAD_PROBE_TYPE_IDLE,
        (GstPadProbeCallback) pad_blocked, comp, NULL);
  }

  if (!entry->dataprobeid) {
    entry->dataprobeid = gst_pad_add_probe (entry->object->srcpad,
        GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST |
        GST_PAD_PROBE_TYPE_EVENT_BOTH, (GstPadProbeCallback) drop_data, entry,
        NULL);
  }

  /* Set the caps of the composition */
  if (G_UNLIKELY (!gst_caps_is_any (((GnlObject *) comp)->caps)))
    gnl_object_set_caps ((GnlObject *) element, ((GnlObject *) comp)->caps);

  /* Special case for default source. */
  if (GNL_OBJECT_IS_EXPANDABLE (element)) {
    /* It doesn't get added to objects_start and objects_stop. */
    comp->expandables = g_list_prepend (comp->expandables, element);
    goto beach;
  }

  /* add it sorted to the objects list */
  comp->objects_start = g_list_insert_sorted
      (comp->objects_start, element, (GCompareFunc) objects_start_compare);

  if (comp->objects_start)
    GST_LOG_OBJECT (comp,
        "Head of objects_start is now %s [%" GST_TIME_FORMAT "--%"
        GST_TIME_FORMAT "]",
        GST_OBJECT_NAME (comp->objects_start->data),
        GST_TIME_ARGS (GNL_OBJECT_START (comp->objects_start->data)),
        GST_TIME_ARGS (GNL_OBJECT_STOP (comp->objects_start->data)));

  comp->objects_stop = g_list_insert_sorted
      (comp->objects_stop, element, (GCompareFunc) objects_stop_compare);

  /* Now the object is ready to be commited and then used */

beach:
  COMP_OBJECTS_UNLOCK (comp);

  gst_object_unref (element);
  return ret;

chiringuito:
  {
    update_start_stop_duration (comp);
    goto beach;
  }
}

static void
gnl_composition_class_init (GnlCompositionClass * klass)
{
  GstBinClass *gstbin_class;
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstElementClass *gstelement_class = (GstElementClass *) klass;;

  GST_DEBUG_CATEGORY_INIT (gnlcomposition_debug, "gnlcomposition",
      GST_DEBUG_FG_BLUE | GST_DEBUG_BOLD, "GNonLin Composition");

  gobject_class->set_property = GST_DEBUG_FUNCPTR (_set_property);
  gobject_class->get_property = GST_DEBUG_FUNCPTR (_get_property);
  gobject_class->dispose = GST_DEBUG_FUNCPTR (_dispose);
  gobject_class->finalize = GST_DEBUG_FUNCPTR (_finalize);

  gstbin_class = (GstBinClass *) klass;
  gstbin_class->add_element = GST_DEBUG_FUNCPTR (gnl_composition_add_object);

  gstelement_class->change_state = _change_state;
  gst_element_class_set_static_metadata (gstelement_class,
      "GNonLin Composition", "Filter/Editor/Bin", "Combines GNL objects",
      "Wim Taymans <wim.taymans@gmail.com>\n"
      "Edward Hervey <bilboed@bilboed.com>\n"
      "Mathieu Duponchelle <mathieu.duponchelle@opencreed.com>\n"
      "Thibault Saunier <tsaunier@gnome.org>");
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gnl_composition_src_template));

  /* Get the paramspec of the GnlObject klass so we can do
   * fast notifies */
  gnlobject_properties[GNLOBJECT_PROP_START] =
      g_object_class_find_property (gobject_class, "start");
  gnlobject_properties[GNLOBJECT_PROP_STOP] =
      g_object_class_find_property (gobject_class, "stop");
  gnlobject_properties[GNLOBJECT_PROP_DURATION] =
      g_object_class_find_property (gobject_class, "duration");

  /**
   * GnlComposition:deactivated-elements-state
   *
   * Get or set the #GstState in which elements that are not used
   * in the currently configured pipeline should be set.
   * By default the state is GST_STATE_READY to lower memory usage and avoid
   * using all the avalaible threads from the kernel but that means that in
   * certain case gapless will be more 'complicated' than if the state was set
   * to GST_STATE_PAUSED.
   */
  _properties[PROP_DEACTIVATED_ELEMENTS_STATE] =
      g_param_spec_enum ("deactivated-elements-state",
      "Deactivate elements state", "The state in which elements"
      " not used in the currently configured pipeline should"
      " be set", GST_TYPE_STATE, GST_STATE_READY,
      G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties (gobject_class, PROP_LAST, _properties);

  /**
   * GnlComposition::commit
   * @self: a #GnlComposition
   * @recurse: Whether to commit recursiverly into (GnlComposition) children of
   *           @object. This is used in case we have composition inside
   *           a gnlsource composition, telling it to commit the included
   *           composition state.
   *
   * Action signal to commit all the pending changes of the composition and
   * its children timing properties
   *
   * Returns: %TRUE if changes have been commited, %FALSE if nothing had to
   * be commited
   */
  _signals[COMMIT_SIGNAL] = g_signal_new ("commit", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
      G_STRUCT_OFFSET (GnlObjectClass, commit_signal_handler), NULL, NULL, NULL,
      G_TYPE_BOOLEAN, 1, G_TYPE_BOOLEAN);

  /**
   * GnlComposition::remove-object
   * @self: a #GnlComposition
   * @child: The #GnlObject child to remove from @composition. This should
   */
  _signals[REMOVE_OBJECT_SIGNAL] = g_signal_new ("remove-object",
      G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
      G_STRUCT_OFFSET (GnlCompositionClass, remove_object_signal_handler),
      NULL, NULL, NULL, GNL_TYPE_OBJECT, 1, G_TYPE_BOOLEAN);
}

static void
gnl_composition_init (GnlComposition * self)
{
  GnlObject *gnlobject = (GnlObject *) self;
  GST_OBJECT_FLAG_SET (self, GNL_OBJECT_SOURCE);
  GST_OBJECT_FLAG_SET (self, GNL_OBJECT_COMPOSITION);

  g_mutex_init (&self->objects_lock);
  COMP_OBJECTS_LOCK (self);
  self->objects_start = NULL;
  self->objects_stop = NULL;
  self->objects_hash = g_hash_table_new_full
      (g_direct_hash,
      g_direct_equal, NULL, (GDestroyNotify) hash_value_destroy);
  COMP_OBJECTS_UNLOCK (self);

  self->mcontext = g_main_context_new ();

  gst_pad_set_activatemode_function (gnlobject->srcpad,
      GST_DEBUG_FUNCPTR ((GstPadActivateModeFunction) src_activate_mode));

  _reset (self);
}
