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

/**
 * SECTION:element-gnlcomposition
 *
 * A GnlComposition contains GnlObjects such as GnlSources and GnlOperations,
 * it connects them dynamically to create a composition timeline.
 */

static GstStaticPadTemplate gnl_composition_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS_ANY);

#define GNL_TYPE_COMPOSITION           (gnl_composition_get_type())
#define GNL_COMPOSITION(obj)           (G_TYPE_CHECK_INSTANCE_CAST((obj),GNL_TYPE_COMPOSITION,GnlComposition))
#define GNL_COMPOSITION_CLASS(klass)   (G_TYPE_CHECK_CLASS_CAST((klass),GNL_TYPE_COMPOSITION,GnlCompositionClass))
#define GNL_COMPOSITION_GET_CLASS(obj) (GNL_COMPOSITION_CLASS (G_OBJECT_GET_CLASS (obj)))
#define GNL_IS_COMPOSITION(obj)        (G_TYPE_CHECK_INSTANCE_TYPE((obj),GNL_TYPE_COMPOSITION))
#define GNL_IS_COMPOSITION_CLASS(obj)  (G_TYPE_CHECK_CLASS_TYPE((klass),GNL_TYPE_COMPOSITION))


/***************************************
 *   GnlComposition class structures   *
 ***************************************/
typedef struct _GnlComposition
{
  GstBin parent;

} GnlComposition;

typedef struct _GnlCompositionClass
{
  GstBinClass parent_class;

} GnlCompositionClass;

GType gnl_composition_get_type (void);

GST_DEBUG_CATEGORY_STATIC (gnlcomposition_debug);
#define GST_CAT_DEFAULT gnlcomposition_debug

#define gnl_composition_parent_class parent_class
G_DEFINE_TYPE (GnlComposition, gnl_composition, GNL_TYPE_OBJECT)


/*****************************************
 *     GObject vmethods implementation   *
 *****************************************/
     static void gnl_composition_class_init (GnlCompositionClass * klass)
{
  GST_DEBUG_CATEGORY_INIT (gnlcomposition_debug, "gnlcomposition",
      GST_DEBUG_FG_BLUE | GST_DEBUG_BOLD, "GNonLin Composition");
}
