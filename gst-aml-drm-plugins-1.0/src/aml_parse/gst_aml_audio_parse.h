/* GStreamer Aml Audio Parser
 * Copyright (C) <2023> Amlogic Inc.
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

#ifndef __GST_AML_AUDIO_PARSE_H__
#define __GST_AML_AUDIO_PARSE_H__

#include <gst/gst.h>
#include <gst/base/gstbaseparse.h>

G_BEGIN_DECLS

#define GST_TYPE_AML_AUDIO_PARSE \
  (gst_aml_audio_parse_get_type())
#define GST_AML_AUDIO_PARSE(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_AML_AUDIO_PARSE, GstAmlAudioParse))
#define GST_AML_AUDIO_PARSE_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_AML_AUDIO_PARSE, GstAmlAudioParseClass))
#define GST_IS_AML_AUDIO_PARSE(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_AML_AUDIO_PARSE))
#define GST_IS_AML_AUDIO_PARSE_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_AML_AUDIO_PARSE))


typedef enum {
  MEDIA_TYPE_UNKNOWN,
  MEDIA_TYPE_AAC_ADTS,
  MEDIA_TYPE_AC3,
  MEDIA_TYPE_EAC3,
} GstAmlParseMediaType;


typedef struct _GstAmlAudioParse GstAmlAudioParse;
typedef struct _GstAmlAudioParseClass GstAmlAudioParseClass;

/**
 * GstAmlAudioParse:
 *
 * The opaque GstAmlAudioParse data structure.
 */
struct _GstAmlAudioParse {
  GstBaseParse element;
  gint frame_samples;
  GstAmlParseMediaType media_type;
  GstAmlParseMediaType output_media_type;
};

/**
 * GstAmlAudioParseClass:
 * @parent_class: Element parent class.
 *
 * The opaque GstAmlAudioParseClass data structure.
 */
struct _GstAmlAudioParseClass {
  GstBaseParseClass parent_class;
};

GType gst_aml_audio_parse_get_type (void);

G_END_DECLS

#endif /* __GST_AML_AUDIO_PARSE_H__ */
