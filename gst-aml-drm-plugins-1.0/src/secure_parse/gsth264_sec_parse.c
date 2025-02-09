/* GStreamer H.264 Secure Parser
 *
 * Copyright (C) <2019> Amlogic Inc.
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
#  include "config.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <gst/base/base.h>
#include <gst/pbutils/pbutils.h>
#include <gst/video/video.h>
#include "gsth264_sec_parse.h"
#include "gstsecmemallocator.h"
#include <string.h>

GST_DEBUG_CATEGORY (h264_sec_parse_debug);
#define GST_CAT_DEFAULT h264_sec_parse_debug

#define DEFAULT_CONFIG_INTERVAL      (0)

enum
{
  PROP_0,
  PROP_CONFIG_INTERVAL
};

enum
{
  GST_H264_SEC_PARSE_FORMAT_NONE,
  GST_H264_SEC_PARSE_FORMAT_AVC,
  GST_H264_SEC_PARSE_FORMAT_BYTE,
  GST_H264_SEC_PARSE_FORMAT_AVC3
};

enum
{
  GST_H264_SEC_PARSE_ALIGN_NONE = 0,
  GST_H264_SEC_PARSE_ALIGN_NAL,
  GST_H264_SEC_PARSE_ALIGN_AU
};

enum
{
  GST_H264_SEC_PARSE_STATE_GOT_SPS = 1 << 0,
  GST_H264_SEC_PARSE_STATE_GOT_PPS = 1 << 1,
  GST_H264_SEC_PARSE_STATE_GOT_SLICE = 1 << 2,

  GST_H264_SEC_PARSE_STATE_VALID_PICTURE_HEADERS = (GST_H264_SEC_PARSE_STATE_GOT_SPS |
      GST_H264_SEC_PARSE_STATE_GOT_PPS),
  GST_H264_SEC_PARSE_STATE_VALID_PICTURE =
      (GST_H264_SEC_PARSE_STATE_VALID_PICTURE_HEADERS |
      GST_H264_SEC_PARSE_STATE_GOT_SLICE)
};

#define GST_H264_SEC_PARSE_STATE_VALID(parse, expected_state) \
  (((parse)->state & (expected_state)) == (expected_state))

static GstStaticPadTemplate sinktemplate = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-h264(" GST_CAPS_FEATURE_MEMORY_SECMEM_MEMORY ")"));

static GstStaticPadTemplate srctemplate = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-h264(" GST_CAPS_FEATURE_MEMORY_DMABUF "),"
        "parsed = (boolean) true, "
        "stream-format=(string)byte-stream, "
        "alignment=(string)au"));

#define parent_class gst_h264_sec_parse_parent_class
G_DEFINE_TYPE (GstH264SecParse, gst_h264_sec_parse, GST_TYPE_BASE_PARSE);

static void gst_h264_sec_parse_finalize (GObject * object);

static gboolean gst_h264_sec_parse_start (GstBaseParse * parse);
static gboolean gst_h264_sec_parse_stop (GstBaseParse * parse);
static GstFlowReturn gst_h264_sec_parse_handle_frame (GstBaseParse * parse,
    GstBaseParseFrame * frame, gint * skipsize);
static GstFlowReturn gst_h264_sec_parse_parse_frame (GstBaseParse * parse,
    GstBaseParseFrame * frame);
static GstFlowReturn gst_h264_sec_parse_pre_push_frame (GstBaseParse * parse,
    GstBaseParseFrame * frame);

static void gst_h264_sec_parse_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_h264_sec_parse_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static gboolean gst_h264_sec_parse_set_caps (GstBaseParse * parse, GstCaps * caps);
static GstCaps *gst_h264_sec_parse_get_caps (GstBaseParse * parse,
    GstCaps * filter);
static gboolean gst_h264_sec_parse_event (GstBaseParse * parse, GstEvent * event);
static gboolean gst_h264_sec_parse_src_event (GstBaseParse * parse,
    GstEvent * event);
static void gst_h264_sec_parse_update_src_caps (GstH264SecParse * h264parse,
    GstCaps * caps);

static void
gst_h264_sec_parse_class_init (GstH264SecParseClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstBaseParseClass *parse_class = GST_BASE_PARSE_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);

  GST_DEBUG_CATEGORY_INIT (h264_sec_parse_debug, "h264secparse", 0, "amlogic h264 secure parser");

  gobject_class->finalize = gst_h264_sec_parse_finalize;
  gobject_class->set_property = gst_h264_sec_parse_set_property;
  gobject_class->get_property = gst_h264_sec_parse_get_property;

  g_object_class_install_property (gobject_class, PROP_CONFIG_INTERVAL,
      g_param_spec_int ("config-interval",
          "SPS PPS Send Interval",
          "Send SPS and PPS Insertion Interval in seconds (sprop parameter sets "
          "will be multiplexed in the data stream when detected.) "
          "(0 = disabled, -1 = send with every IDR frame)",
          -1, 3600, DEFAULT_CONFIG_INTERVAL,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));

  /* Override BaseParse vfuncs */
  parse_class->start = GST_DEBUG_FUNCPTR (gst_h264_sec_parse_start);
  parse_class->stop = GST_DEBUG_FUNCPTR (gst_h264_sec_parse_stop);
  parse_class->handle_frame = GST_DEBUG_FUNCPTR (gst_h264_sec_parse_handle_frame);
  parse_class->pre_push_frame =
      GST_DEBUG_FUNCPTR (gst_h264_sec_parse_pre_push_frame);
  parse_class->set_sink_caps = GST_DEBUG_FUNCPTR (gst_h264_sec_parse_set_caps);
  parse_class->get_sink_caps = GST_DEBUG_FUNCPTR (gst_h264_sec_parse_get_caps);
  parse_class->sink_event = GST_DEBUG_FUNCPTR (gst_h264_sec_parse_event);
  parse_class->src_event = GST_DEBUG_FUNCPTR (gst_h264_sec_parse_src_event);

  gst_element_class_add_static_pad_template (gstelement_class, &srctemplate);
  gst_element_class_add_static_pad_template (gstelement_class, &sinktemplate);

  gst_element_class_set_static_metadata (gstelement_class, "H.264 parser",
      "Codec/Parser/Converter/Video",
      "Parses H.264 streams",
      "Mark Nauwelaerts <mark.nauwelaerts@collabora.co.uk>");
}

static void
gst_h264_sec_parse_init (GstH264SecParse * h264parse)
{
  h264parse->frame_out = gst_adapter_new ();
  gst_base_parse_set_pts_interpolation (GST_BASE_PARSE (h264parse), FALSE);
  GST_PAD_SET_ACCEPT_INTERSECT (GST_BASE_PARSE_SINK_PAD (h264parse));
  GST_PAD_SET_ACCEPT_TEMPLATE (GST_BASE_PARSE_SINK_PAD (h264parse));

  h264parse->aud_needed = TRUE;
  h264parse->aud_insert = TRUE;
}


static void
gst_h264_sec_parse_finalize (GObject * object)
{
  GstH264SecParse *h264parse = GST_H264_SEC_PARSE (object);

  g_object_unref (h264parse->frame_out);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_h264_sec_parse_reset_frame (GstH264SecParse * h264parse)
{
  GST_LOG_OBJECT (h264parse, "reset frame");

  /* done parsing; reset state */
  h264parse->current_off = -1;

  h264parse->picture_start = FALSE;
  h264parse->update_caps = FALSE;
  h264parse->idr_pos = -1;
  h264parse->sei_pos = -1;
  h264parse->keyframe = FALSE;
  h264parse->header = FALSE;
  h264parse->frame_start = FALSE;
  h264parse->aud_insert = TRUE;
  h264parse->have_sps_in_frame = FALSE;
  h264parse->have_pps_in_frame = FALSE;
  gst_adapter_clear (h264parse->frame_out);
}

static void
gst_h264_sec_parse_reset_stream_info (GstH264SecParse * h264parse)
{
  gint i;

  h264parse->width = 0;
  h264parse->height = 0;
  h264parse->fps_num = 0;
  h264parse->fps_den = 0;
  h264parse->upstream_par_n = -1;
  h264parse->upstream_par_d = -1;
  h264parse->parsed_par_n = 0;
  h264parse->parsed_par_d = 0;
  h264parse->have_pps = FALSE;
  h264parse->have_sps = FALSE;

  h264parse->multiview_mode = GST_VIDEO_MULTIVIEW_MODE_NONE;
  h264parse->multiview_flags = GST_VIDEO_MULTIVIEW_FLAGS_NONE;
  h264parse->first_in_bundle = TRUE;

  h264parse->align = GST_H264_SEC_PARSE_ALIGN_NONE;
  h264parse->format = GST_H264_SEC_PARSE_FORMAT_NONE;

  h264parse->transform = FALSE;
  h264parse->nal_length_size = 4;
  h264parse->packetized = FALSE;
  h264parse->push_codec = FALSE;

  gst_buffer_replace (&h264parse->codec_data, NULL);
  gst_buffer_replace (&h264parse->codec_data_in, NULL);

  gst_h264_sec_parse_reset_frame (h264parse);

  for (i = 0; i < GST_H264_MAX_SPS_COUNT; i++)
    gst_buffer_replace (&h264parse->sps_nals[i], NULL);
  for (i = 0; i < GST_H264_MAX_PPS_COUNT; i++)
    gst_buffer_replace (&h264parse->pps_nals[i], NULL);
}

static void
gst_h264_sec_parse_reset (GstH264SecParse * h264parse)
{
  h264parse->last_report = GST_CLOCK_TIME_NONE;

  h264parse->dts = GST_CLOCK_TIME_NONE;
  h264parse->ts_trn_nb = GST_CLOCK_TIME_NONE;
  h264parse->do_ts = TRUE;

  h264parse->sent_codec_tag = FALSE;

  h264parse->pending_key_unit_ts = GST_CLOCK_TIME_NONE;
  gst_event_replace (&h264parse->force_key_unit_event, NULL);

  h264parse->discont = FALSE;

  gst_h264_sec_parse_reset_stream_info (h264parse);
}

static gboolean
gst_h264_sec_parse_start (GstBaseParse * parse)
{
  GstH264SecParse *h264parse = GST_H264_SEC_PARSE (parse);

  GST_DEBUG_OBJECT (parse, "start");
  gst_h264_sec_parse_reset (h264parse);

  h264parse->nalparser = gst_h264_nal_parser_new ();

  h264parse->state = 0;
  h264parse->dts = GST_CLOCK_TIME_NONE;
  h264parse->ts_trn_nb = GST_CLOCK_TIME_NONE;
  h264parse->sei_pic_struct_pres_flag = FALSE;
  h264parse->sei_pic_struct = 0;
  h264parse->field_pic_flag = 0;

  gst_base_parse_set_min_frame_size (parse, 6);

  return TRUE;
}

static gboolean
gst_h264_sec_parse_stop (GstBaseParse * parse)
{
  GstH264SecParse *h264parse = GST_H264_SEC_PARSE (parse);

  GST_DEBUG_OBJECT (parse, "stop");
  gst_h264_sec_parse_reset (h264parse);

  gst_h264_nal_parser_free (h264parse->nalparser);

  return TRUE;
}

static const gchar *
gst_h264_sec_parse_get_string (GstH264SecParse * parse, gboolean format, gint code)
{
  if (format) {
    switch (code) {
      case GST_H264_SEC_PARSE_FORMAT_AVC:
        return "avc";
      case GST_H264_SEC_PARSE_FORMAT_BYTE:
        return "byte-stream";
      case GST_H264_SEC_PARSE_FORMAT_AVC3:
        return "avc3";
      default:
        return "none";
    }
  } else {
    switch (code) {
      case GST_H264_SEC_PARSE_ALIGN_NAL:
        return "nal";
      case GST_H264_SEC_PARSE_ALIGN_AU:
        return "au";
      default:
        return "none";
    }
  }
}

static void
gst_h264_sec_parse_format_from_caps (GstCaps * caps, guint * format, guint * align)
{
  if (format)
    *format = GST_H264_SEC_PARSE_FORMAT_NONE;

  if (align)
    *align = GST_H264_SEC_PARSE_ALIGN_NONE;

  g_return_if_fail (gst_caps_is_fixed (caps));

  GST_DEBUG ("parsing caps: %" GST_PTR_FORMAT, caps);

  if (caps && gst_caps_get_size (caps) > 0) {
    GstStructure *s = gst_caps_get_structure (caps, 0);
    const gchar *str = NULL;

    if (format) {
      if ((str = gst_structure_get_string (s, "stream-format"))) {
        if (strcmp (str, "avc") == 0)
          *format = GST_H264_SEC_PARSE_FORMAT_AVC;
        else if (strcmp (str, "byte-stream") == 0)
          *format = GST_H264_SEC_PARSE_FORMAT_BYTE;
        else if (strcmp (str, "avc3") == 0)
          *format = GST_H264_SEC_PARSE_FORMAT_AVC3;
      }
    }

    if (align) {
      if ((str = gst_structure_get_string (s, "alignment"))) {
        if (strcmp (str, "au") == 0)
          *align = GST_H264_SEC_PARSE_ALIGN_AU;
        else if (strcmp (str, "nal") == 0)
          *align = GST_H264_SEC_PARSE_ALIGN_NAL;
      }
    }
  }
}

/* check downstream caps to configure format and alignment */
static void
gst_h264_sec_parse_negotiate (GstH264SecParse * h264parse, gint in_format,
    GstCaps * in_caps)
{
  GstCaps *caps;
  guint format = h264parse->format;
  guint align = h264parse->align;

  g_return_if_fail ((in_caps == NULL) || gst_caps_is_fixed (in_caps));

  caps = gst_pad_get_allowed_caps (GST_BASE_PARSE_SRC_PAD (h264parse));
  GST_DEBUG_OBJECT (h264parse, "allowed caps: %" GST_PTR_FORMAT, caps);

  /* concentrate on leading structure, since decodebin parser
   * capsfilter always includes parser template caps */
  if (caps) {
    caps = gst_caps_truncate (caps);
    GST_DEBUG_OBJECT (h264parse, "negotiating with caps: %" GST_PTR_FORMAT,
        caps);
  }

  h264parse->can_passthrough = FALSE;

  if (in_caps && caps) {
    if (gst_caps_can_intersect (in_caps, caps)) {
      GST_DEBUG_OBJECT (h264parse, "downstream accepts upstream caps");
      gst_h264_sec_parse_format_from_caps (in_caps, &format, &align);
      gst_caps_unref (caps);
      caps = NULL;
      h264parse->can_passthrough = TRUE;
    }
  }

  /* FIXME We could fail the negotiation immediatly if caps are empty */
  if (caps && !gst_caps_is_empty (caps)) {
    /* fixate to avoid ambiguity with lists when parsing */
    caps = gst_caps_fixate (caps);
    gst_h264_sec_parse_format_from_caps (caps, &format, &align);
  }

  /* default */
  if (!format)
    format = GST_H264_SEC_PARSE_FORMAT_BYTE;
  if (!align)
    align = GST_H264_SEC_PARSE_ALIGN_AU;

  GST_DEBUG_OBJECT (h264parse, "selected format %s, alignment %s",
      gst_h264_sec_parse_get_string (h264parse, TRUE, format),
      gst_h264_sec_parse_get_string (h264parse, FALSE, align));

  h264parse->format = format;
  h264parse->align = align;

  h264parse->transform = in_format != h264parse->format ||
      align == GST_H264_SEC_PARSE_ALIGN_AU;

  if (caps)
    gst_caps_unref (caps);
}

static GstBuffer *
gst_h264_sec_parse_wrap_nal (GstH264SecParse * h264parse, guint format, guint8 * data,
    guint size)
{
  GstBuffer *buf;
  guint nl = h264parse->nal_length_size;
  guint32 tmp;

  GST_DEBUG_OBJECT (h264parse, "nal length %d", size);

  buf = gst_buffer_new_allocate (NULL, 4 + size, NULL);
  if (format == GST_H264_SEC_PARSE_FORMAT_AVC
      || format == GST_H264_SEC_PARSE_FORMAT_AVC3) {
    tmp = GUINT32_TO_BE (size << (32 - 8 * nl));
  } else {
    /* HACK: nl should always be 4 here, otherwise this won't work.
     * There are legit cases where nl in avc stream is 2, but byte-stream
     * SC is still always 4 bytes. */
    nl = 4;
    tmp = GUINT32_TO_BE (1);
  }

  gst_buffer_fill (buf, 0, &tmp, sizeof (guint32));
  gst_buffer_fill (buf, nl, data, size);
  gst_buffer_set_size (buf, size + nl);

  return buf;
}

static void
gst_h264_sec_parser_store_nal (GstH264SecParse * h264parse, guint id,
    GstH264NalUnitType naltype, GstH264NalUnit * nalu)
{
  GstBuffer *buf, **store;
  guint size = nalu->size, store_size;

  if (naltype == GST_H264_NAL_SPS || naltype == GST_H264_NAL_SUBSET_SPS) {
    store_size = GST_H264_MAX_SPS_COUNT;
    store = h264parse->sps_nals;
    GST_DEBUG_OBJECT (h264parse, "storing sps %u", id);
  } else if (naltype == GST_H264_NAL_PPS) {
    store_size = GST_H264_MAX_PPS_COUNT;
    store = h264parse->pps_nals;
    GST_DEBUG_OBJECT (h264parse, "storing pps %u", id);
  } else
    return;

  if (id >= store_size) {
    GST_DEBUG_OBJECT (h264parse, "unable to store nal, id out-of-range %d", id);
    return;
  }

  buf = gst_buffer_new_allocate (NULL, size, NULL);
  gst_buffer_fill (buf, 0, nalu->data + nalu->offset, size);

  /* Indicate that buffer contain a header needed for decoding */
  if (naltype == GST_H264_NAL_SPS || naltype == GST_H264_NAL_PPS)
    GST_BUFFER_FLAG_SET (buf, GST_BUFFER_FLAG_HEADER);

  if (store[id])
    gst_buffer_unref (store[id]);

  store[id] = buf;
}

#ifndef GST_DISABLE_GST_DEBUG
static const gchar *nal_names[] = {
  "Unknown",
  "Slice",
  "Slice DPA",
  "Slice DPB",
  "Slice DPC",
  "Slice IDR",
  "SEI",
  "SPS",
  "PPS",
  "AU delimiter",
  "Sequence End",
  "Stream End",
  "Filler Data",
  "SPS extension",
  "Prefix",
  "SPS Subset",
  "Depth Parameter Set",
  "Reserved", "Reserved",
  "Slice Aux Unpartitioned",
  "Slice Extension",
  "Slice Depth/3D-AVC Extension"
};

static const gchar *
_nal_name (GstH264NalUnitType nal_type)
{
  if (nal_type <= GST_H264_NAL_SLICE_DEPTH)
    return nal_names[nal_type];
  return "Invalid";
}
#endif

static void
gst_h264_sec_parse_process_sei (GstH264SecParse * h264parse, GstH264NalUnit * nalu)
{
  GstH264SEIMessage sei;
  GstH264NalParser *nalparser = h264parse->nalparser;
  GstH264ParserResult pres;
  GArray *messages;
  guint i;

  pres = gst_h264_parser_parse_sei (nalparser, nalu, &messages);
  if (pres != GST_H264_PARSER_OK)
    GST_WARNING_OBJECT (h264parse, "failed to parse one or more SEI message");

  /* Even if pres != GST_H264_PARSER_OK, some message could have been parsed and
   * stored in messages.
   */
  for (i = 0; i < messages->len; i++) {
    sei = g_array_index (messages, GstH264SEIMessage, i);
    switch (sei.payloadType) {
      case GST_H264_SEI_PIC_TIMING:
      {
        guint j;
        h264parse->sei_pic_struct_pres_flag =
            sei.payload.pic_timing.pic_struct_present_flag;
        h264parse->sei_cpb_removal_delay =
            sei.payload.pic_timing.cpb_removal_delay;
        if (h264parse->sei_pic_struct_pres_flag) {
          h264parse->sei_pic_struct = sei.payload.pic_timing.pic_struct;
        }

        h264parse->num_clock_timestamp = 0;

        for (j = 0; j < 3; j++) {
          if (sei.payload.pic_timing.clock_timestamp_flag[j]) {
            memcpy (&h264parse->clock_timestamp[h264parse->
                    num_clock_timestamp++],
                &sei.payload.pic_timing.clock_timestamp[j],
                sizeof (GstH264ClockTimestamp));
          }
        }

        GST_LOG_OBJECT (h264parse, "pic timing updated");
        break;
      }
      case GST_H264_SEI_BUF_PERIOD:
        if (h264parse->ts_trn_nb == GST_CLOCK_TIME_NONE ||
            h264parse->dts == GST_CLOCK_TIME_NONE)
          h264parse->ts_trn_nb = 0;
        else
          h264parse->ts_trn_nb = h264parse->dts;

        GST_LOG_OBJECT (h264parse,
            "new buffering period; ts_trn_nb updated: %" GST_TIME_FORMAT,
            GST_TIME_ARGS (h264parse->ts_trn_nb));
        break;

        /* Additional messages that are not innerly useful to the
         * element but for debugging purposes */
      case GST_H264_SEI_RECOVERY_POINT:
        GST_LOG_OBJECT (h264parse, "recovery point found: %u %u %u %u",
            sei.payload.recovery_point.recovery_frame_cnt,
            sei.payload.recovery_point.exact_match_flag,
            sei.payload.recovery_point.broken_link_flag,
            sei.payload.recovery_point.changing_slice_group_idc);
        h264parse->keyframe = TRUE;
        break;

        /* Additional messages that are not innerly useful to the
         * element but for debugging purposes */
      case GST_H264_SEI_STEREO_VIDEO_INFO:{
        GstVideoMultiviewMode mview_mode = GST_VIDEO_MULTIVIEW_MODE_NONE;
        GstVideoMultiviewFlags mview_flags = GST_VIDEO_MULTIVIEW_FLAGS_NONE;

        GST_LOG_OBJECT (h264parse, "Stereo video information %u %u %u %u %u %u",
            sei.payload.stereo_video_info.field_views_flag,
            sei.payload.stereo_video_info.top_field_is_left_view_flag,
            sei.payload.stereo_video_info.current_frame_is_left_view_flag,
            sei.payload.stereo_video_info.next_frame_is_second_view_flag,
            sei.payload.stereo_video_info.left_view_self_contained_flag,
            sei.payload.stereo_video_info.right_view_self_contained_flag);

        if (sei.payload.stereo_video_info.field_views_flag) {
          mview_mode = GST_VIDEO_MULTIVIEW_MODE_ROW_INTERLEAVED;
          if (!sei.payload.stereo_video_info.top_field_is_left_view_flag)
            mview_flags |= GST_VIDEO_MULTIVIEW_FLAGS_RIGHT_VIEW_FIRST;
        } else {
          mview_mode = GST_VIDEO_MULTIVIEW_MODE_FRAME_BY_FRAME;
          if (sei.payload.stereo_video_info.next_frame_is_second_view_flag) {
            /* Mark current frame as first in bundle */
            h264parse->first_in_bundle = TRUE;
            if (!sei.payload.stereo_video_info.current_frame_is_left_view_flag)
              mview_flags |= GST_VIDEO_MULTIVIEW_FLAGS_RIGHT_VIEW_FIRST;
          }
        }
        if (mview_mode != h264parse->multiview_mode ||
            mview_flags != h264parse->multiview_flags) {
          h264parse->multiview_mode = mview_mode;
          h264parse->multiview_flags = mview_flags;
          /* output caps need to be changed */
          gst_h264_sec_parse_update_src_caps (h264parse, NULL);
        }
        break;
      }
      case GST_H264_SEI_FRAME_PACKING:{
        GstVideoMultiviewMode mview_mode = GST_VIDEO_MULTIVIEW_MODE_NONE;
        GstVideoMultiviewFlags mview_flags = GST_VIDEO_MULTIVIEW_FLAGS_NONE;

        GST_LOG_OBJECT (h264parse,
            "frame packing arrangement message: id %u cancelled %u "
            "type %u quincunx %u content_interpretation %d flip %u "
            "right_first %u field_views %u is_frame0 %u",
            sei.payload.frame_packing.frame_packing_id,
            sei.payload.frame_packing.frame_packing_cancel_flag,
            sei.payload.frame_packing.frame_packing_type,
            sei.payload.frame_packing.quincunx_sampling_flag,
            sei.payload.frame_packing.content_interpretation_type,
            sei.payload.frame_packing.spatial_flipping_flag,
            sei.payload.frame_packing.frame0_flipped_flag,
            sei.payload.frame_packing.field_views_flag,
            sei.payload.frame_packing.current_frame_is_frame0_flag);

        /* Only IDs from 0->255 and 512->2^31-1 are valid. Ignore others */
        if ((sei.payload.frame_packing.frame_packing_id >= 256 &&
                sei.payload.frame_packing.frame_packing_id < 512) ||
            (sei.payload.frame_packing.frame_packing_id >= (1U << 31)))
          break;                /* ignore */

        if (!sei.payload.frame_packing.frame_packing_cancel_flag) {
          /* Cancel flag sets things back to no-info */

          if (sei.payload.frame_packing.content_interpretation_type == 2)
            mview_flags |= GST_VIDEO_MULTIVIEW_FLAGS_RIGHT_VIEW_FIRST;

          switch (sei.payload.frame_packing.frame_packing_type) {
            case 0:
              mview_mode = GST_VIDEO_MULTIVIEW_MODE_CHECKERBOARD;
              break;
            case 1:
              mview_mode = GST_VIDEO_MULTIVIEW_MODE_COLUMN_INTERLEAVED;
              break;
            case 2:
              mview_mode = GST_VIDEO_MULTIVIEW_MODE_ROW_INTERLEAVED;
              break;
            case 3:
              if (sei.payload.frame_packing.quincunx_sampling_flag)
                mview_mode = GST_VIDEO_MULTIVIEW_MODE_SIDE_BY_SIDE_QUINCUNX;
              else
                mview_mode = GST_VIDEO_MULTIVIEW_MODE_SIDE_BY_SIDE;
              if (sei.payload.frame_packing.spatial_flipping_flag) {
                /* One of the views is flopped. */
                if (sei.payload.frame_packing.frame0_flipped_flag !=
                    ! !(mview_flags &
                        GST_VIDEO_MULTIVIEW_FLAGS_RIGHT_VIEW_FIRST))
                  /* the left view is flopped */
                  mview_flags |= GST_VIDEO_MULTIVIEW_FLAGS_LEFT_FLOPPED;
                else
                  mview_flags |= GST_VIDEO_MULTIVIEW_FLAGS_RIGHT_FLOPPED;
              }
              break;
            case 4:
              mview_mode = GST_VIDEO_MULTIVIEW_MODE_TOP_BOTTOM;
              if (sei.payload.frame_packing.spatial_flipping_flag) {
                /* One of the views is flipped, */
                if (sei.payload.frame_packing.frame0_flipped_flag !=
                    ! !(mview_flags &
                        GST_VIDEO_MULTIVIEW_FLAGS_RIGHT_VIEW_FIRST))
                  /* the left view is flipped */
                  mview_flags |= GST_VIDEO_MULTIVIEW_FLAGS_LEFT_FLIPPED;
                else
                  mview_flags |= GST_VIDEO_MULTIVIEW_FLAGS_RIGHT_FLIPPED;
              }
              break;
            case 5:
              if (sei.payload.frame_packing.content_interpretation_type == 0)
                mview_mode = GST_VIDEO_MULTIVIEW_MODE_MULTIVIEW_FRAME_BY_FRAME;
              else
                mview_mode = GST_VIDEO_MULTIVIEW_MODE_FRAME_BY_FRAME;
              break;
            default:
              GST_DEBUG_OBJECT (h264parse, "Invalid frame packing type %u",
                  sei.payload.frame_packing.frame_packing_type);
              break;
          }
        }

        if (mview_mode != h264parse->multiview_mode ||
            mview_flags != h264parse->multiview_flags) {
          h264parse->multiview_mode = mview_mode;
          h264parse->multiview_flags = mview_flags;
          /* output caps need to be changed */
          gst_h264_sec_parse_update_src_caps (h264parse, NULL);
        }
        break;
      }
    }
  }
  g_array_free (messages, TRUE);
}

/* caller guarantees 2 bytes of nal payload */
static gboolean
gst_h264_sec_parse_process_nal (GstH264SecParse * h264parse, GstH264NalUnit * nalu)
{
  guint nal_type;
  GstH264PPS pps = { 0, };
  GstH264SPS sps = { 0, };
  GstH264NalParser *nalparser = h264parse->nalparser;
  GstH264ParserResult pres;

  /* nothing to do for broken input */
  if (G_UNLIKELY (nalu->size < 2)) {
    GST_DEBUG_OBJECT (h264parse, "not processing nal size %u", nalu->size);
    return TRUE;
  }

  /* we have a peek as well */
  nal_type = nalu->type;

  GST_DEBUG_OBJECT (h264parse, "processing nal of type %u %s, size %u",
      nal_type, _nal_name (nal_type), nalu->size);

  switch (nal_type) {
    case GST_H264_NAL_SUBSET_SPS:
      if (!GST_H264_SEC_PARSE_STATE_VALID (h264parse, GST_H264_SEC_PARSE_STATE_GOT_SPS))
        return FALSE;
#if GST_CHECK_VERSION(1,18,0)
      pres = gst_h264_parser_parse_subset_sps (nalparser, nalu, &sps);
#else
      pres = gst_h264_parser_parse_subset_sps (nalparser, nalu, &sps, TRUE);
#endif
      goto process_sps;

    case GST_H264_NAL_SPS:
      /* reset state, everything else is obsolete */
      h264parse->state = 0;
#if GST_CHECK_VERSION(1,18,0)
      pres = gst_h264_parser_parse_sps (nalparser, nalu, &sps);
#else
      pres = gst_h264_parser_parse_sps (nalparser, nalu, &sps, TRUE);
#endif

    process_sps:
      /* arranged for a fallback sps.id, so use that one and only warn */
      if (pres != GST_H264_PARSER_OK) {
        GST_WARNING_OBJECT (h264parse, "failed to parse SPS:");
        return FALSE;
      }

      GST_DEBUG_OBJECT (h264parse, "triggering src caps check");
      h264parse->update_caps = TRUE;
      h264parse->have_sps = TRUE;
      h264parse->have_sps_in_frame = TRUE;
      if (h264parse->push_codec && h264parse->have_pps) {
        /* SPS and PPS found in stream before the first pre_push_frame, no need
         * to forcibly push at start */
        GST_INFO_OBJECT (h264parse, "have SPS/PPS in stream");
        h264parse->push_codec = FALSE;
        h264parse->have_sps = FALSE;
        h264parse->have_pps = FALSE;
      }

      gst_h264_sec_parser_store_nal (h264parse, sps.id, nal_type, nalu);
      gst_h264_sps_clear (&sps);
      h264parse->state |= GST_H264_SEC_PARSE_STATE_GOT_SPS;
      h264parse->header |= TRUE;
      break;
    case GST_H264_NAL_PPS:
      /* expected state: got-sps */
      h264parse->state &= GST_H264_SEC_PARSE_STATE_GOT_SPS;
      if (!GST_H264_SEC_PARSE_STATE_VALID (h264parse, GST_H264_SEC_PARSE_STATE_GOT_SPS))
        return FALSE;

      pres = gst_h264_parser_parse_pps (nalparser, nalu, &pps);
      /* arranged for a fallback pps.id, so use that one and only warn */
      if (pres != GST_H264_PARSER_OK) {
        GST_WARNING_OBJECT (h264parse, "failed to parse PPS:");
        if (pres != GST_H264_PARSER_BROKEN_LINK)
          return FALSE;
      }

      /* parameters might have changed, force caps check */
      if (!h264parse->have_pps) {
        GST_DEBUG_OBJECT (h264parse, "triggering src caps check");
        h264parse->update_caps = TRUE;
      }
      h264parse->have_pps = TRUE;
      h264parse->have_pps_in_frame = TRUE;
      if (h264parse->push_codec && h264parse->have_sps) {
        /* SPS and PPS found in stream before the first pre_push_frame, no need
         * to forcibly push at start */
        GST_INFO_OBJECT (h264parse, "have SPS/PPS in stream");
        h264parse->push_codec = FALSE;
        h264parse->have_sps = FALSE;
        h264parse->have_pps = FALSE;
      }

      gst_h264_sec_parser_store_nal (h264parse, pps.id, nal_type, nalu);
      gst_h264_pps_clear (&pps);
      h264parse->state |= GST_H264_SEC_PARSE_STATE_GOT_PPS;
      h264parse->header |= TRUE;
      break;
    case GST_H264_NAL_SEI:
      /* expected state: got-sps */
      if (!GST_H264_SEC_PARSE_STATE_VALID (h264parse, GST_H264_SEC_PARSE_STATE_GOT_SPS))
        return FALSE;

      h264parse->header |= TRUE;
      gst_h264_sec_parse_process_sei (h264parse, nalu);
      /* mark SEI pos */
      if (h264parse->sei_pos == -1) {
        if (h264parse->transform)
          h264parse->sei_pos = gst_adapter_available (h264parse->frame_out);
        else
          h264parse->sei_pos = nalu->sc_offset;
        GST_DEBUG_OBJECT (h264parse, "marking SEI in frame at offset %d",
            h264parse->sei_pos);
      }
      break;

    case GST_H264_NAL_SLICE:
    case GST_H264_NAL_SLICE_DPA:
    case GST_H264_NAL_SLICE_DPB:
    case GST_H264_NAL_SLICE_DPC:
    case GST_H264_NAL_SLICE_IDR:
    case GST_H264_NAL_SLICE_EXT:
      /* expected state: got-sps|got-pps (valid picture headers) */
      h264parse->state &= GST_H264_SEC_PARSE_STATE_VALID_PICTURE_HEADERS;
      if (!GST_H264_SEC_PARSE_STATE_VALID (h264parse,
              GST_H264_SEC_PARSE_STATE_VALID_PICTURE_HEADERS))
        return FALSE;

      /* don't need to parse the whole slice (header) here */
      if (*(nalu->data + nalu->offset + nalu->header_bytes) & 0x80) {
        /* means first_mb_in_slice == 0 */
        /* real frame data */
        GST_DEBUG_OBJECT (h264parse, "first_mb_in_slice = 0");
        h264parse->frame_start = TRUE;
      }
      GST_DEBUG_OBJECT (h264parse, "frame start: %i", h264parse->frame_start);
      if (nal_type == GST_H264_NAL_SLICE_EXT && !GST_H264_IS_MVC_NALU (nalu))
        break;
      {
        GstH264SliceHdr slice;

        pres = gst_h264_parser_parse_slice_hdr (nalparser, nalu, &slice,
            FALSE, FALSE);
        GST_DEBUG_OBJECT (h264parse,
            "parse result %d, first MB: %u, slice type: %u",
            pres, slice.first_mb_in_slice, slice.type);
        if (pres == GST_H264_PARSER_OK) {
          if (GST_H264_IS_I_SLICE (&slice) || GST_H264_IS_SI_SLICE (&slice))
            h264parse->keyframe |= TRUE;

          h264parse->state |= GST_H264_SEC_PARSE_STATE_GOT_SLICE;
          h264parse->field_pic_flag = slice.field_pic_flag;
        }
      }
      if (G_LIKELY (nal_type != GST_H264_NAL_SLICE_IDR &&
              !h264parse->push_codec))
        break;
      /* if we need to sneak codec NALs into the stream,
       * this is a good place, so fake it as IDR
       * (which should be at start anyway) */
      /* mark where config needs to go if interval expired */
      /* mind replacement buffer if applicable */
      if (h264parse->idr_pos == -1) {
        if (h264parse->transform)
          h264parse->idr_pos = gst_adapter_available (h264parse->frame_out);
        else
          h264parse->idr_pos = nalu->sc_offset;
        GST_DEBUG_OBJECT (h264parse, "marking IDR in frame at offset %d",
            h264parse->idr_pos);
      }
      /* if SEI preceeds (faked) IDR, then we have to insert config there */
      if (h264parse->sei_pos >= 0 && h264parse->idr_pos > h264parse->sei_pos) {
        h264parse->idr_pos = h264parse->sei_pos;
        GST_DEBUG_OBJECT (h264parse, "moved IDR mark to SEI position %d",
            h264parse->idr_pos);
      }
      break;
    case GST_H264_NAL_AU_DELIMITER:
      /* Just accumulate AU Delimiter, whether it's before SPS or not */
      pres = gst_h264_parser_parse_nal (nalparser, nalu);
      if (pres != GST_H264_PARSER_OK)
        return FALSE;
      h264parse->aud_insert = FALSE;
      break;
    default:
      /* drop anything before the initial SPS */
      if (!GST_H264_SEC_PARSE_STATE_VALID (h264parse, GST_H264_SEC_PARSE_STATE_GOT_SPS))
        return FALSE;

      pres = gst_h264_parser_parse_nal (nalparser, nalu);
      if (pres != GST_H264_PARSER_OK)
        return FALSE;
      break;
  }

  /* if AVC output needed, collect properly prefixed nal in adapter,
   * and use that to replace outgoing buffer data later on */
  if (h264parse->transform) {
    GstBuffer *buf;

    GST_LOG_OBJECT (h264parse, "collecting NAL in AVC frame");
    buf = gst_h264_sec_parse_wrap_nal (h264parse, h264parse->format,
        nalu->data + nalu->offset, nalu->size);
    gst_adapter_push (h264parse->frame_out, buf);
  }
  return TRUE;
}

/* caller guarantees at least 2 bytes of nal payload for each nal
 * returns TRUE if next_nal indicates that nal terminates an AU */
static inline gboolean
gst_h264_sec_parse_collect_nal (GstH264SecParse * h264parse, const guint8 * data,
    guint size, GstH264NalUnit * nalu)
{
  gboolean complete;
  GstH264ParserResult parse_res;
  GstH264NalUnitType nal_type = nalu->type;
  GstH264NalUnit nnalu;

  GST_DEBUG_OBJECT (h264parse, "parsing collected nal");
  parse_res = gst_h264_parser_identify_nalu_unchecked (h264parse->nalparser,
      data, nalu->offset + nalu->size, size, &nnalu);

  if (parse_res != GST_H264_PARSER_OK)
    return FALSE;

  /* determine if AU complete */
  GST_LOG_OBJECT (h264parse, "nal type: %d %s", nal_type, _nal_name (nal_type));
  /* coded slice NAL starts a picture,
   * i.e. other types become aggregated in front of it */
  h264parse->picture_start |= (nal_type == GST_H264_NAL_SLICE ||
      nal_type == GST_H264_NAL_SLICE_DPA || nal_type == GST_H264_NAL_SLICE_IDR);

  /* consider a coded slices (IDR or not) to start a picture,
   * (so ending the previous one) if first_mb_in_slice == 0
   * (non-0 is part of previous one) */
  /* NOTE this is not entirely according to Access Unit specs in 7.4.1.2.4,
   * but in practice it works in sane cases, needs not much parsing,
   * and also works with broken frame_num in NAL
   * (where spec-wise would fail) */
  nal_type = nnalu.type;
  complete = h264parse->picture_start && ((nal_type >= GST_H264_NAL_SEI &&
          nal_type <= GST_H264_NAL_AU_DELIMITER) ||
      (nal_type >= 14 && nal_type <= 18));

  GST_LOG_OBJECT (h264parse, "next nal type: %d %s", nal_type,
      _nal_name (nal_type));
  complete |= h264parse->picture_start && (nal_type == GST_H264_NAL_SLICE
      || nal_type == GST_H264_NAL_SLICE_DPA
      || nal_type == GST_H264_NAL_SLICE_IDR) &&
      /* first_mb_in_slice == 0 considered start of frame */
      (nnalu.data[nnalu.offset + nnalu.header_bytes] & 0x80);

  GST_LOG_OBJECT (h264parse, "au complete: %d", complete);

  return complete;
}

static GstFlowReturn
gst_h264_sec_parse_handle_frame_packetized (GstBaseParse * parse,
    GstBaseParseFrame * frame)
{
  GstH264SecParse *h264parse = GST_H264_SEC_PARSE (parse);
  GstBuffer *buffer = frame->buffer;
  GstFlowReturn ret = GST_FLOW_OK;
  const guint nl = h264parse->nal_length_size;
  gint left = 0;
  gsize size = gst_buffer_get_size(buffer);

  if (nl < 1 || nl > 4) {
    GST_DEBUG_OBJECT (h264parse, "insufficient data to split input");
    return GST_FLOW_NOT_NEGOTIATED;
  }

  /* need to save buffer from invalidation upon _finish_frame */
  if (h264parse->split_packetized)
    buffer = gst_buffer_copy (frame->buffer);

  if (!h264parse->split_packetized) {
    gst_h264_sec_parse_parse_frame (parse, frame);
    ret = gst_base_parse_finish_frame (parse, frame, size);
  } else {
    gst_buffer_unref (buffer);
    if (G_UNLIKELY (left)) {
      /* should not be happening for nice AVC */
      GST_WARNING_OBJECT (parse, "skipping leftover AVC data %d", left);
      frame->flags |= GST_BASE_PARSE_FRAME_FLAG_DROP;
      ret = gst_base_parse_finish_frame (parse, frame, size);
    }
  }

  return ret;
}

static GstFlowReturn
gst_h264_sec_parse_handle_frame (GstBaseParse * parse,
    GstBaseParseFrame * frame, gint * skipsize)
{
  GstH264SecParse *h264parse = GST_H264_SEC_PARSE (parse);
  GstBuffer *buffer = frame->buffer;
  gsize size;

  if (G_UNLIKELY (GST_BUFFER_FLAG_IS_SET (frame->buffer,
              GST_BUFFER_FLAG_DISCONT))) {
    h264parse->discont = TRUE;
  }

  /* delegate in packetized case, no skipping should be needed */
  if (h264parse->packetized)
    return gst_h264_sec_parse_handle_frame_packetized (parse, frame);

  size = gst_buffer_get_size(buffer);

  /* expect at least 3 bytes startcode == sc, and 2 bytes NALU payload */
  if (G_UNLIKELY (size < 5)) {
    *skipsize = 1;
    return GST_FLOW_OK;
  }

  /* need to configure aggregation */
  if (G_UNLIKELY (h264parse->format == GST_H264_SEC_PARSE_FORMAT_NONE))
    gst_h264_sec_parse_negotiate (h264parse, GST_H264_SEC_PARSE_FORMAT_BYTE, NULL);

  /* avoid stale cached parsing state */
  if (frame->flags & GST_BASE_PARSE_FRAME_FLAG_NEW_FRAME) {
    GST_LOG_OBJECT (h264parse, "parsing new frame");
    gst_h264_sec_parse_reset_frame (h264parse);
  } else {
    GST_LOG_OBJECT (h264parse, "resuming frame parsing");
  }

  gst_h264_sec_parse_parse_frame (parse, frame);
  *skipsize = 0;

  return gst_base_parse_finish_frame (parse, frame, size);
}

/* byte together avc codec data based on collected pps and sps so far */
static GstBuffer *
gst_h264_sec_parse_make_codec_data (GstH264SecParse * h264parse)
{
  GstBuffer *buf, *nal;
  gint i, sps_size = 0, pps_size = 0, num_sps = 0, num_pps = 0;
  guint8 profile_idc = 0, profile_comp = 0, level_idc = 0;
  gboolean found = FALSE;
  GstMapInfo map;
  guint8 *data;
  gint nl;

  /* only nal payload in stored nals */

  for (i = 0; i < GST_H264_MAX_SPS_COUNT; i++) {
    if ((nal = h264parse->sps_nals[i])) {
      gsize size = gst_buffer_get_size (nal);
      num_sps++;
      /* size bytes also count */
      sps_size += size + 2;
      if (size >= 4) {
        guint8 tmp[3];
        found = TRUE;
        gst_buffer_extract (nal, 1, tmp, 3);
        profile_idc = tmp[0];
        profile_comp = tmp[1];
        level_idc = tmp[2];
      }
    }
  }
  for (i = 0; i < GST_H264_MAX_PPS_COUNT; i++) {
    if ((nal = h264parse->pps_nals[i])) {
      num_pps++;
      /* size bytes also count */
      pps_size += gst_buffer_get_size (nal) + 2;
    }
  }

  /* AVC3 has SPS/PPS inside the stream, not in the codec_data */
  if (h264parse->format == GST_H264_SEC_PARSE_FORMAT_AVC3) {
    num_sps = sps_size = 0;
    num_pps = pps_size = 0;
  }

  GST_DEBUG_OBJECT (h264parse,
      "constructing codec_data: num_sps=%d, num_pps=%d", num_sps, num_pps);

  if (!found || (0 == num_pps
          && GST_H264_SEC_PARSE_FORMAT_AVC3 != h264parse->format))
    return NULL;

  buf = gst_buffer_new_allocate (NULL, 5 + 1 + sps_size + 1 + pps_size, NULL);
  gst_buffer_map (buf, &map, GST_MAP_WRITE);
  data = map.data;
  nl = h264parse->nal_length_size;

  data[0] = 1;                  /* AVC Decoder Configuration Record ver. 1 */
  data[1] = profile_idc;        /* profile_idc                             */
  data[2] = profile_comp;       /* profile_compability                     */
  data[3] = level_idc;          /* level_idc                               */
  data[4] = 0xfc | (nl - 1);    /* nal_length_size_minus1                  */
  data[5] = 0xe0 | num_sps;     /* number of SPSs */

  data += 6;
  if (h264parse->format != GST_H264_SEC_PARSE_FORMAT_AVC3) {
    for (i = 0; i < GST_H264_MAX_SPS_COUNT; i++) {
      if ((nal = h264parse->sps_nals[i])) {
        gsize nal_size = gst_buffer_get_size (nal);
        GST_WRITE_UINT16_BE (data, nal_size);
        gst_buffer_extract (nal, 0, data + 2, nal_size);
        data += 2 + nal_size;
      }
    }
  }

  data[0] = num_pps;
  data++;
  if (h264parse->format != GST_H264_SEC_PARSE_FORMAT_AVC3) {
    for (i = 0; i < GST_H264_MAX_PPS_COUNT; i++) {
      if ((nal = h264parse->pps_nals[i])) {
        gsize nal_size = gst_buffer_get_size (nal);
        GST_WRITE_UINT16_BE (data, nal_size);
        gst_buffer_extract (nal, 0, data + 2, nal_size);
        data += 2 + nal_size;
      }
    }
  }

  gst_buffer_unmap (buf, &map);

  return buf;
}

static void
gst_h264_sec_parse_get_par (GstH264SecParse * h264parse, gint * num, gint * den)
{
  if (h264parse->upstream_par_n != -1 && h264parse->upstream_par_d != -1) {
    *num = h264parse->upstream_par_n;
    *den = h264parse->upstream_par_d;
  } else {
    *num = h264parse->parsed_par_n;
    *den = h264parse->parsed_par_d;
  }
}

static GstCaps *
get_compatible_profile_caps (GstH264SPS * sps)
{
  GstCaps *caps = NULL;
  const gchar **profiles = NULL;
  gint i;
  GValue compat_profiles = G_VALUE_INIT;
  g_value_init (&compat_profiles, GST_TYPE_LIST);

  switch (sps->profile_idc) {
    case GST_H264_PROFILE_EXTENDED:
      if (sps->constraint_set0_flag) {  /* A.2.1 */
        if (sps->constraint_set1_flag) {
          static const gchar *profile_array[] =
              { "constrained-baseline", "baseline", "main", "high",
            "high-10", "high-4:2:2", "high-4:4:4", NULL
          };
          profiles = profile_array;
        } else {
          static const gchar *profile_array[] = { "baseline", NULL };
          profiles = profile_array;
        }
      } else if (sps->constraint_set1_flag) {   /* A.2.2 */
        static const gchar *profile_array[] =
            { "main", "high", "high-10", "high-4:2:2", "high-4:4:4", NULL };
        profiles = profile_array;
      }
      break;
    case GST_H264_PROFILE_BASELINE:
      if (sps->constraint_set1_flag) {  /* A.2.1 */
        static const gchar *profile_array[] =
            { "baseline", "main", "high", "high-10", "high-4:2:2",
          "high-4:4:4", NULL
        };
        profiles = profile_array;
      } else {
        static const gchar *profile_array[] = { "extended", NULL };
        profiles = profile_array;
      }
      break;
    case GST_H264_PROFILE_MAIN:
    {
      static const gchar *profile_array[] =
          { "high", "high-10", "high-4:2:2", "high-4:4:4", NULL };
      profiles = profile_array;
    }
      break;
    case GST_H264_PROFILE_HIGH:
      if (sps->constraint_set1_flag) {
        static const gchar *profile_array[] =
            { "main", "high-10", "high-4:2:2", "high-4:4:4", NULL };
        profiles = profile_array;
      } else {
        static const gchar *profile_array[] =
            { "high-10", "high-4:2:2", "high-4:4:4", NULL };
        profiles = profile_array;
      }
      break;
    case GST_H264_PROFILE_HIGH10:
      if (sps->constraint_set1_flag) {
        static const gchar *profile_array[] =
            { "main", "high", "high-4:2:2", "high-4:4:4", NULL };
        profiles = profile_array;
      } else {
        if (sps->constraint_set3_flag) {        /* A.2.8 */
          static const gchar *profile_array[] =
              { "high-10", "high-4:2:2", "high-4:4:4", "high-4:2:2-intra",
            "high-4:4:4-intra", NULL
          };
          profiles = profile_array;
        } else {
          static const gchar *profile_array[] =
              { "high-4:2:2", "high-4:4:4", NULL };
          profiles = profile_array;
        }
      }
      break;
    case GST_H264_PROFILE_HIGH_422:
      if (sps->constraint_set1_flag) {
        static const gchar *profile_array[] =
            { "main", "high", "high-10", "high-4:4:4", NULL };
        profiles = profile_array;
      } else {
        if (sps->constraint_set3_flag) {        /* A.2.9 */
          static const gchar *profile_array[] =
              { "high-4:2:2", "high-4:4:4", "high-4:4:4-intra", NULL };
          profiles = profile_array;
        } else {
          static const gchar *profile_array[] = { "high-4:4:4", NULL };
          profiles = profile_array;
        }
      }
      break;
    case GST_H264_PROFILE_HIGH_444:
      if (sps->constraint_set1_flag) {
        static const gchar *profile_array[] =
            { "main", "high", "high-10", "high-4:2:2", NULL };
        profiles = profile_array;
      } else if (sps->constraint_set3_flag) {   /* A.2.10 */
        static const gchar *profile_array[] = { "high-4:4:4", NULL };
        profiles = profile_array;
      }
      break;
    case GST_H264_PROFILE_MULTIVIEW_HIGH:
      if (sps->extension_type == GST_H264_NAL_EXTENSION_MVC
          && sps->extension.mvc.num_views_minus1 == 1) {
        static const gchar *profile_array[] =
            { "stereo-high", "multiview-high", NULL };
        profiles = profile_array;
      } else {
        static const gchar *profile_array[] = { "multiview-high", NULL };
        profiles = profile_array;
      }
      break;
    default:
      break;
  }

  if (profiles) {
    GValue value = G_VALUE_INIT;
    caps = gst_caps_new_empty_simple ("video/x-h264");
    for (i = 0; profiles[i]; i++) {
      g_value_init (&value, G_TYPE_STRING);
      g_value_set_string (&value, profiles[i]);
      gst_value_list_append_value (&compat_profiles, &value);
      g_value_unset (&value);
    }
    gst_caps_set_value (caps, "profile", &compat_profiles);
    g_value_unset (&compat_profiles);
  }

  return caps;
}

/* if downstream didn't support the exact profile indicated in sps header,
 * check for the compatible profiles also */
static void
ensure_caps_profile (GstH264SecParse * h264parse, GstCaps * caps, GstH264SPS * sps)
{
  GstCaps *peer_caps, *compat_caps;

  peer_caps = gst_pad_get_current_caps (GST_BASE_PARSE_SRC_PAD (h264parse));
  if (!peer_caps || !gst_caps_can_intersect (caps, peer_caps)) {
    GstCaps *filter_caps = gst_caps_new_empty_simple ("video/x-h264");

    if (peer_caps)
      gst_caps_unref (peer_caps);
    peer_caps =
        gst_pad_peer_query_caps (GST_BASE_PARSE_SRC_PAD (h264parse),
        filter_caps);

    gst_caps_unref (filter_caps);
  }

  if (peer_caps && !gst_caps_can_intersect (caps, peer_caps)) {
    GstStructure *structure;

    compat_caps = get_compatible_profile_caps (sps);
    if (compat_caps != NULL) {
      GstCaps *res_caps = NULL;

      res_caps = gst_caps_intersect (peer_caps, compat_caps);

      if (res_caps && !gst_caps_is_empty (res_caps)) {
        const gchar *profile_str = NULL;

        res_caps = gst_caps_fixate (res_caps);
        structure = gst_caps_get_structure (res_caps, 0);
        profile_str = gst_structure_get_string (structure, "profile");
        if (profile_str) {
          gst_caps_set_simple (caps, "profile", G_TYPE_STRING, profile_str,
              NULL);
          GST_DEBUG_OBJECT (h264parse,
              "Setting compatible profile %s to the caps", profile_str);
        }
      }
      if (res_caps)
        gst_caps_unref (res_caps);
      gst_caps_unref (compat_caps);
    }
  }
  if (peer_caps)
    gst_caps_unref (peer_caps);
}

static const gchar *
digit_to_string (guint digit)
{
  static const char itoa[][2] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9"
  };

  if (G_LIKELY (digit < 10))
    return itoa[digit];
  else
    return NULL;
}

static const gchar *
get_profile_string (GstH264SPS * sps)
{
  const gchar *profile = NULL;

  switch (sps->profile_idc) {
    case 66:
      if (sps->constraint_set1_flag)
        profile = "constrained-baseline";
      else
        profile = "baseline";
      break;
    case 77:
      profile = "main";
      break;
    case 88:
      profile = "extended";
      break;
    case 100:
      if (sps->constraint_set4_flag) {
        if (sps->constraint_set5_flag)
          profile = "constrained-high";
        else
          profile = "progressive-high";
      } else
        profile = "high";
      break;
    case 110:
      if (sps->constraint_set3_flag)
        profile = "high-10-intra";
      else if (sps->constraint_set4_flag)
        profile = "progressive-high-10";
      else
        profile = "high-10";
      break;
    case 122:
      if (sps->constraint_set3_flag)
        profile = "high-4:2:2-intra";
      else
        profile = "high-4:2:2";
      break;
    case 244:
      if (sps->constraint_set3_flag)
        profile = "high-4:4:4-intra";
      else
        profile = "high-4:4:4";
      break;
    case 44:
      profile = "cavlc-4:4:4-intra";
      break;
    case 118:
      profile = "multiview-high";
      break;
    case 128:
      profile = "stereo-high";
      break;
    case 83:
      if (sps->constraint_set5_flag)
        profile = "scalable-constrained-baseline";
      else
        profile = "scalable-baseline";
      break;
    case 86:
      if (sps->constraint_set3_flag)
        profile = "scalable-high-intra";
      else if (sps->constraint_set5_flag)
        profile = "scalable-constrained-high";
      else
        profile = "scalable-high";
      break;
    default:
      return NULL;
  }

  return profile;
}

static const gchar *
get_level_string (GstH264SPS * sps)
{
  if (sps->level_idc == 0)
    return NULL;
  else if ((sps->level_idc == 11 && sps->constraint_set3_flag)
      || sps->level_idc == 9)
    return "1b";
  else if (sps->level_idc % 10 == 0)
    return digit_to_string (sps->level_idc / 10);
  else {
    switch (sps->level_idc) {
      case 11:
        return "1.1";
      case 12:
        return "1.2";
      case 13:
        return "1.3";
      case 21:
        return "2.1";
      case 22:
        return "2.2";
      case 31:
        return "3.1";
      case 32:
        return "3.2";
      case 41:
        return "4.1";
      case 42:
        return "4.2";
      case 51:
        return "5.1";
      case 52:
        return "5.2";
      default:
        return NULL;
    }
  }
}

static void
gst_h264_sec_parse_update_src_caps (GstH264SecParse * h264parse, GstCaps * caps)
{
  GstH264SPS *sps;
  GstCaps *sink_caps, *src_caps;
  gboolean modified = FALSE;
  GstBuffer *buf = NULL;
  GstStructure *s = NULL;

  if (G_UNLIKELY (!gst_pad_has_current_caps (GST_BASE_PARSE_SRC_PAD
              (h264parse))))
    modified = TRUE;
  else if (G_UNLIKELY (!h264parse->update_caps))
    return;

  /* if this is being called from the first _setcaps call, caps on the sinkpad
   * aren't set yet and so they need to be passed as an argument */
  if (caps)
    sink_caps = gst_caps_ref (caps);
  else
    sink_caps = gst_pad_get_current_caps (GST_BASE_PARSE_SINK_PAD (h264parse));

  /* carry over input caps as much as possible; override with our own stuff */
  if (!sink_caps)
    sink_caps = gst_caps_new_empty_simple ("video/x-h264");
  else
    s = gst_caps_get_structure (sink_caps, 0);

  sps = h264parse->nalparser->last_sps;
  GST_DEBUG_OBJECT (h264parse, "sps: %p", sps);

  /* only codec-data for nice-and-clean au aligned packetized avc format */
  if ((h264parse->format == GST_H264_SEC_PARSE_FORMAT_AVC
          || h264parse->format == GST_H264_SEC_PARSE_FORMAT_AVC3)
      && h264parse->align == GST_H264_SEC_PARSE_ALIGN_AU) {
    buf = gst_h264_sec_parse_make_codec_data (h264parse);
    if (buf && h264parse->codec_data) {
      GstMapInfo map;

      gst_buffer_map (buf, &map, GST_MAP_READ);
      if (map.size != gst_buffer_get_size (h264parse->codec_data) ||
          gst_buffer_memcmp (h264parse->codec_data, 0, map.data, map.size))
        modified = TRUE;

      gst_buffer_unmap (buf, &map);
    } else {
      if (!buf && h264parse->codec_data_in)
        buf = gst_buffer_ref (h264parse->codec_data_in);
      modified = TRUE;
    }
  }

  caps = NULL;
  if (G_UNLIKELY (!sps)) {
    caps = gst_caps_copy (sink_caps);
    gst_caps_set_simple (caps, "width", G_TYPE_INT, 1920,
              "height", G_TYPE_INT, 1080, NULL);
  } else {
    gint crop_width, crop_height;
    gint fps_num, fps_den;
    gint par_n, par_d;

    if (sps->frame_cropping_flag) {
      crop_width = sps->crop_rect_width;
      crop_height = sps->crop_rect_height;
    } else {
      crop_width = sps->width;
      crop_height = sps->height;
    }

    if (G_UNLIKELY (h264parse->width != crop_width ||
            h264parse->height != crop_height)) {
      GST_INFO_OBJECT (h264parse, "resolution changed %dx%d",
          crop_width, crop_height);
      h264parse->width = crop_width;
      h264parse->height = crop_height;
      modified = TRUE;
    }

    /* 0/1 is set as the default in the codec parser, we will set
     * it in case we have no info */
    gst_h264_video_calculate_framerate (sps, h264parse->field_pic_flag,
        h264parse->sei_pic_struct, &fps_num, &fps_den);
    if (G_UNLIKELY (h264parse->fps_num != fps_num
            || h264parse->fps_den != fps_den)) {
      GST_DEBUG_OBJECT (h264parse, "framerate changed %d/%d", fps_num, fps_den);
      h264parse->fps_num = fps_num;
      h264parse->fps_den = fps_den;
      modified = TRUE;
    }

    if (sps->vui_parameters.aspect_ratio_info_present_flag) {
      if (G_UNLIKELY ((h264parse->parsed_par_n != sps->vui_parameters.par_n)
              || (h264parse->parsed_par_d != sps->vui_parameters.par_d))) {
        h264parse->parsed_par_n = sps->vui_parameters.par_n;
        h264parse->parsed_par_d = sps->vui_parameters.par_d;
        GST_INFO_OBJECT (h264parse, "pixel aspect ratio has been changed %d/%d",
            h264parse->parsed_par_n, h264parse->parsed_par_d);
      }
    }

    if (G_UNLIKELY (modified || h264parse->update_caps)) {
      GstVideoInterlaceMode imode = GST_VIDEO_INTERLACE_MODE_PROGRESSIVE;
      gint width, height;
      GstClockTime latency;

      const gchar *caps_mview_mode = NULL;
      GstVideoMultiviewMode mview_mode = h264parse->multiview_mode;
      GstVideoMultiviewFlags mview_flags = h264parse->multiview_flags;
      const gchar *chroma_format = NULL;
      guint bit_depth_chroma;

      fps_num = h264parse->fps_num;
      fps_den = h264parse->fps_den;

      caps = gst_caps_copy (sink_caps);

      /* sps should give this but upstream overrides */
      if (s && gst_structure_has_field (s, "width"))
        gst_structure_get_int (s, "width", &width);
      else
        width = h264parse->width;

      if (s && gst_structure_has_field (s, "height"))
        gst_structure_get_int (s, "height", &height);
      else
        height = h264parse->height;

      if (s == NULL ||
          !gst_structure_get_fraction (s, "pixel-aspect-ratio", &par_n,
              &par_d)) {
        gst_h264_sec_parse_get_par (h264parse, &par_n, &par_d);
        if (par_n != 0 && par_d != 0) {
          GST_INFO_OBJECT (h264parse, "PAR %d/%d", par_n, par_d);
          gst_caps_set_simple (caps, "pixel-aspect-ratio", GST_TYPE_FRACTION,
              par_n, par_d, NULL);
        } else {
          /* Assume par_n/par_d of 1/1 for calcs below, but don't set into caps */
          par_n = par_d = 1;
        }
      }

      /* Pass through or set output stereo/multiview config */
      if (s && gst_structure_has_field (s, "multiview-mode")) {
        caps_mview_mode = gst_structure_get_string (s, "multiview-mode");
        gst_structure_get_flagset (s, "multiview-flags",
            (guint *) & mview_flags, NULL);
      } else if (mview_mode != GST_VIDEO_MULTIVIEW_MODE_NONE) {
        if (gst_video_multiview_guess_half_aspect (mview_mode,
                width, height, par_n, par_d)) {
          mview_flags |= GST_VIDEO_MULTIVIEW_FLAGS_HALF_ASPECT;
        }

        caps_mview_mode = gst_video_multiview_mode_to_caps_string (mview_mode);
        gst_caps_set_simple (caps, "multiview-mode", G_TYPE_STRING,
            caps_mview_mode, "multiview-flags",
            GST_TYPE_VIDEO_MULTIVIEW_FLAGSET, mview_flags,
            GST_FLAG_SET_MASK_EXACT, NULL);
      }

      gst_caps_set_simple (caps, "width", G_TYPE_INT, width,
          "height", G_TYPE_INT, height, NULL);

      /* upstream overrides */
      if (s && gst_structure_has_field (s, "framerate")) {
        gst_structure_get_fraction (s, "framerate", &fps_num, &fps_den);
      }

      /* but not necessarily or reliably this */
      if (fps_den > 0) {
        GstStructure *s2;
        gst_caps_set_simple (caps, "framerate",
            GST_TYPE_FRACTION, fps_num, fps_den, NULL);
        s2 = gst_caps_get_structure (caps, 0);
        gst_structure_get_fraction (s2, "framerate", &h264parse->parsed_fps_n,
            &h264parse->parsed_fps_d);
        gst_base_parse_set_frame_rate (GST_BASE_PARSE (h264parse), fps_num,
            fps_den, 0, 0);
        if (fps_num > 0) {
          latency = gst_util_uint64_scale (GST_SECOND, fps_den, fps_num);
          gst_base_parse_set_latency (GST_BASE_PARSE (h264parse), latency,
              latency);
        }

      }
      if (sps->frame_mbs_only_flag == 0)
        imode = GST_VIDEO_INTERLACE_MODE_MIXED;

      if (s && !gst_structure_has_field (s, "interlace-mode"))
        gst_caps_set_simple (caps, "interlace-mode", G_TYPE_STRING,
            gst_video_interlace_mode_to_string (imode), NULL);

      bit_depth_chroma = sps->bit_depth_chroma_minus8 + 8;

      switch (sps->chroma_format_idc) {
        case 0:
          chroma_format = "4:0:0";
          bit_depth_chroma = 0;
          break;
        case 1:
          chroma_format = "4:2:0";
          break;
        case 2:
          chroma_format = "4:2:2";
          break;
        case 3:
          chroma_format = "4:4:4";
          break;
        default:
          break;
      }

      if (chroma_format)
        gst_caps_set_simple (caps,
            "chroma-format", G_TYPE_STRING, chroma_format,
            "bit-depth-luma", G_TYPE_UINT, sps->bit_depth_luma_minus8 + 8,
            "bit-depth-chroma", G_TYPE_UINT, bit_depth_chroma, NULL);
    }
  }

  if (caps) {
    gint par_n = 0, par_d = 0;
    guint size = gst_caps_get_size(caps);
    for (unsigned i = 0; i < size; ++i) {
      gst_caps_set_features(caps, i, gst_caps_features_from_string(GST_CAPS_FEATURE_MEMORY_DMABUF));
    }

    gst_caps_set_simple (caps, "parsed", G_TYPE_BOOLEAN, TRUE,
        "stream-format", G_TYPE_STRING,
        gst_h264_sec_parse_get_string (h264parse, TRUE, h264parse->format),
        "alignment", G_TYPE_STRING,
        gst_h264_sec_parse_get_string (h264parse, FALSE, h264parse->align), NULL);

    /* set profile and level in caps */
    if (sps) {
      const gchar *profile, *level;

      profile = get_profile_string (sps);
      if (profile != NULL)
        gst_caps_set_simple (caps, "profile", G_TYPE_STRING, profile, NULL);

      level = get_level_string (sps);
      if (level != NULL)
        gst_caps_set_simple (caps, "level", G_TYPE_STRING, level, NULL);

      /* relax the profile constraint to find a suitable decoder */
      ensure_caps_profile (h264parse, caps, sps);
    }

    if (s)
      gst_structure_get_fraction (s, "pixel-aspect-ratio", &par_n, &par_d);
    if (par_n != 0 && par_d != 0) {
      GST_INFO_OBJECT (h264parse, "PAR %d/%d", par_n, par_d);
      gst_caps_set_simple (caps, "pixel-aspect-ratio", GST_TYPE_FRACTION,
          par_n, par_d, NULL);
    }

    src_caps = gst_pad_get_current_caps (GST_BASE_PARSE_SRC_PAD (h264parse));

    if (src_caps) {
      /* use codec data from old caps for comparison; we don't want to resend caps
         if everything is same except codec data; */
      if (gst_structure_has_field (gst_caps_get_structure (src_caps, 0),
              "codec_data")) {
        gst_caps_set_value (caps, "codec_data",
            gst_structure_get_value (gst_caps_get_structure (src_caps, 0),
                "codec_data"));
      } else if (!buf) {
        GstStructure *s;
        /* remove any left-over codec-data hanging around */
        s = gst_caps_get_structure (caps, 0);
        gst_structure_remove_field (s, "codec_data");
      }
    }

    if (!(src_caps && gst_caps_is_strictly_equal (src_caps, caps))) {
      /* update codec data to new value */
      if (buf) {
        gst_caps_set_simple (caps, "codec_data", GST_TYPE_BUFFER, buf, NULL);
        gst_buffer_replace (&h264parse->codec_data, buf);
        gst_buffer_unref (buf);
        buf = NULL;
      } else {
        GstStructure *s;
        /* remove any left-over codec-data hanging around */
        s = gst_caps_get_structure (caps, 0);
        gst_structure_remove_field (s, "codec_data");
        gst_buffer_replace (&h264parse->codec_data, NULL);
      }

      gst_pad_set_caps (GST_BASE_PARSE_SRC_PAD (h264parse), caps);
    }

    if (src_caps)
      gst_caps_unref (src_caps);
    gst_caps_unref (caps);
  }

  gst_caps_unref (sink_caps);
  if (buf)
    gst_buffer_unref (buf);
}

static GstFlowReturn
gst_h264_sec_parse_parse_frame (GstBaseParse * parse, GstBaseParseFrame * frame)
{
  GstH264SecParse *h264parse;
  GstBuffer *buffer;
  //guint av;

  h264parse = GST_H264_SEC_PARSE (parse);
  buffer = frame->buffer;

  gst_h264_sec_parse_update_src_caps (h264parse, NULL);

  if (h264parse->keyframe)
    GST_BUFFER_FLAG_UNSET (buffer, GST_BUFFER_FLAG_DELTA_UNIT);
  else
    GST_BUFFER_FLAG_SET (buffer, GST_BUFFER_FLAG_DELTA_UNIT);

  if (h264parse->header)
    GST_BUFFER_FLAG_SET (buffer, GST_BUFFER_FLAG_HEADER);
  else
    GST_BUFFER_FLAG_UNSET (buffer, GST_BUFFER_FLAG_HEADER);

  if (h264parse->discont) {
    GST_BUFFER_FLAG_SET (buffer, GST_BUFFER_FLAG_DISCONT);
    h264parse->discont = FALSE;
  }

  return GST_FLOW_OK;
}

static GstEvent *
check_pending_key_unit_event (GstEvent * pending_event,
    GstSegment * segment, GstClockTime timestamp, guint flags,
    GstClockTime pending_key_unit_ts)
{
  GstClockTime running_time, stream_time;
  gboolean all_headers;
  guint count;
  GstEvent *event = NULL;

  g_return_val_if_fail (segment != NULL, NULL);

  if (pending_event == NULL)
    goto out;

  if (GST_CLOCK_TIME_IS_VALID (pending_key_unit_ts) &&
      timestamp == GST_CLOCK_TIME_NONE)
    goto out;

  running_time = gst_segment_to_running_time (segment,
      GST_FORMAT_TIME, timestamp);

  GST_INFO ("now %" GST_TIME_FORMAT " wanted %" GST_TIME_FORMAT,
      GST_TIME_ARGS (running_time), GST_TIME_ARGS (pending_key_unit_ts));
  if (GST_CLOCK_TIME_IS_VALID (pending_key_unit_ts) &&
      running_time < pending_key_unit_ts)
    goto out;

  if (flags & GST_BUFFER_FLAG_DELTA_UNIT) {
    GST_DEBUG ("pending force key unit, waiting for keyframe");
    goto out;
  }

  stream_time = gst_segment_to_stream_time (segment,
      GST_FORMAT_TIME, timestamp);

  if (!gst_video_event_parse_upstream_force_key_unit (pending_event,
          NULL, &all_headers, &count)) {
    gst_video_event_parse_downstream_force_key_unit (pending_event, NULL,
        NULL, NULL, &all_headers, &count);
  }

  event =
      gst_video_event_new_downstream_force_key_unit (timestamp, stream_time,
      running_time, all_headers, count);
  gst_event_set_seqnum (event, gst_event_get_seqnum (pending_event));

out:
  return event;
}

static void
gst_h264_sec_parse_prepare_key_unit (GstH264SecParse * parse, GstEvent * event)
{
  GstClockTime running_time;
  guint count;
#ifndef GST_DISABLE_GST_DEBUG
  gboolean have_sps, have_pps;
  gint i;
#endif

  parse->pending_key_unit_ts = GST_CLOCK_TIME_NONE;
  gst_event_replace (&parse->force_key_unit_event, NULL);

  gst_video_event_parse_downstream_force_key_unit (event,
      NULL, NULL, &running_time, NULL, &count);

  GST_INFO_OBJECT (parse, "pushing downstream force-key-unit event %d "
      "%" GST_TIME_FORMAT " count %d", gst_event_get_seqnum (event),
      GST_TIME_ARGS (running_time), count);
  gst_pad_push_event (GST_BASE_PARSE_SRC_PAD (parse), event);

#ifndef GST_DISABLE_GST_DEBUG
  have_sps = have_pps = FALSE;
  for (i = 0; i < GST_H264_MAX_SPS_COUNT; i++) {
    if (parse->sps_nals[i] != NULL) {
      have_sps = TRUE;
      break;
    }
  }
  for (i = 0; i < GST_H264_MAX_PPS_COUNT; i++) {
    if (parse->pps_nals[i] != NULL) {
      have_pps = TRUE;
      break;
    }
  }

  GST_INFO_OBJECT (parse, "preparing key unit, have sps %d have pps %d",
      have_sps, have_pps);
#endif

  /* set push_codec to TRUE so that pre_push_frame sends SPS/PPS again */
  parse->push_codec = TRUE;
}

//#define DUMP_ES
#ifdef DUMP_ES
static void dump(GstMemory* mem, gint size, const char* name)
{
  uint8_t *data = (uint8_t *)malloc(size);
  if (data) {
    gst_secmem_dump(mem, data, size);
    FILE* fd = fopen(name, "a+b");
    if (fd) {
      fwrite(data, 1, size, fd);
      fclose(fd);
    }
    free(data);
  }
}
#endif

static GstFlowReturn
gst_h264_sec_parse_pre_push_frame (GstBaseParse * parse, GstBaseParseFrame * frame)
{
  GstH264SecParse *h264parse;
  GstBuffer *buffer;
  GstEvent *event;
  GstFlowReturn ret;
  gboolean csd_from_codec = FALSE;

  h264parse = GST_H264_SEC_PARSE (parse);

  if (!h264parse->sent_codec_tag) {
    GstTagList *taglist;
    GstCaps *caps;

    /* codec tag */
    caps = gst_pad_get_current_caps (GST_BASE_PARSE_SRC_PAD (parse));
    if (caps == NULL) {
      if (GST_PAD_IS_FLUSHING (GST_BASE_PARSE_SRC_PAD (h264parse))) {
        GST_INFO_OBJECT (h264parse, "Src pad is flushing");
        return GST_FLOW_FLUSHING;
      } else {
        GST_INFO_OBJECT (h264parse, "Src pad is not negotiated!");
        return GST_FLOW_NOT_NEGOTIATED;
      }
    }

    taglist = gst_tag_list_new_empty ();
    gst_pb_utils_add_codec_description_to_tag_list (taglist,
        GST_TAG_VIDEO_CODEC, caps);
    gst_caps_unref (caps);

    gst_base_parse_merge_tags (parse, taglist, GST_TAG_MERGE_REPLACE);
    gst_tag_list_unref (taglist);

    /* also signals the end of first-frame processing */
    h264parse->sent_codec_tag = TRUE;
  }

  buffer = frame->buffer;
  GstMemory* mem;

  /* sanity check */
  mem = gst_buffer_peek_memory(buffer, 0);
  if (!gst_is_secmem_memory(mem)) {
    GST_WARNING_OBJECT (parse, "wrong memory type");
    return GST_FLOW_ERROR;
  }

  if (h264parse->format_org != GST_H264_SEC_PARSE_FORMAT_BYTE) {
    gboolean rc = FALSE;
    uint32_t flag;

    if (h264parse->codec_data_in) {
      /* convert avcC format to stream header. */
      GST_DEBUG_OBJECT(h264parse, "parse codec_data");
      GstMapInfo map;
      gst_buffer_map(h264parse->codec_data_in, &map, GST_MAP_READ);
      rc = gst_secmem_parse_avcc(mem, map.data, map.size);
      gst_buffer_unmap(h264parse->codec_data_in, &map);
      if (!rc) {
        GST_ERROR_OBJECT(parse, "gst_secmem_parse_avcc fail");
      } else {
        GST_INFO_OBJECT(parse, "Need update csd from codec");
        csd_from_codec = TRUE;
        gst_buffer_replace(&h264parse->codec_data_in, NULL);
      }
    }

    /* convert avc format to stream format. */
    rc = gst_secmem_parse_avc2nalu(mem, &flag);
    if (!rc) {
      GST_ERROR_OBJECT(parse, "gst_secmem_parse_avc2nalu fail");
#ifdef DUMP_ES
      dump(mem, gst_buffer_get_size(buffer), "/tmp/wrong.dat");
#endif
      return GST_FLOW_ERROR;
    } else
      GST_LOG_OBJECT(parse, "gst_secmem_parse_avc2nalu success, %x buffer:%p flag %x", flag, buffer, flag);

    if ((flag & (PARSER_H264_SPS_SEEN | PARSER_H264_PPS_SEEN))
        && !(flag & (PARSER_H264_IDR_SEEN | PARSER_H264_SLICE_SEEN))) {
      GST_WARNING_OBJECT(h264parse, "frame dropped push_codec:%d flag %x", h264parse->push_codec, flag);
      return GST_BASE_PARSE_FLOW_DROPPED;
    }

    if (h264parse->push_codec) {
      if (!(flag & (PARSER_H264_SPS_SEEN | PARSER_H264_PPS_SEEN))
          && (flag & PARSER_H264_IDR_SEEN)) {
        GST_DEBUG_OBJECT(h264parse, "prepend csd");
        ret = gst_secmem_prepend_csd(mem);
        if (!ret) {
          GST_ERROR_OBJECT(h264parse, "gst_secmem_prepend_csd failed flag %x", flag);
          return FALSE;
        }
        h264parse->push_codec = FALSE;
      } else if (!(flag & PARSER_H264_IDR_SEEN)) {
        GST_WARNING_OBJECT(h264parse, "frame dropped buffer:%p flag %x", buffer, flag);
        return GST_BASE_PARSE_FLOW_DROPPED;
      } else if (flag & PARSER_H264_IDR_SEEN) {
        GST_DEBUG_OBJECT(h264parse, "SPS/PPS in stream flag %x", flag);
        if (csd_from_codec && (flag & (PARSER_H264_SPS_SEEN | PARSER_H264_PPS_SEEN)) !=
          (PARSER_H264_SPS_SEEN | PARSER_H264_PPS_SEEN)) {
          ret = gst_secmem_prepend_csd(mem);
          if (!ret) {
            GST_ERROR_OBJECT(h264parse, "gst_secmem_prepend_csd failed flag %x", flag);
            return FALSE;
          }
        }
        csd_from_codec = FALSE;
        h264parse->push_codec = FALSE;
      }
    }

#ifdef DUMP_ES
    dump(mem, gst_buffer_get_size(buffer), "/tmp/es.dat");
#endif

  } else {
    GST_LOG_OBJECT(h264parse, "bypass frame format: %d", h264parse->format_org);
#ifdef DUMP_ES
    dump(mem, gst_buffer_get_size(buffer), "/tmp/es.dat");
#endif

  }

  if ((event = check_pending_key_unit_event (h264parse->force_key_unit_event,
              &parse->segment, GST_BUFFER_TIMESTAMP (buffer),
              GST_BUFFER_FLAGS (buffer), h264parse->pending_key_unit_ts))) {
    gst_h264_sec_parse_prepare_key_unit (h264parse, event);
  }

  {
    guint i = 0;

    for (i = 0; i < h264parse->num_clock_timestamp; i++) {
      GstH264ClockTimestamp *tim = &h264parse->clock_timestamp[i];
      GstVideoTimeCodeFlags flags = 0;
      gint field_count = -1;
      guint n_frames;

      /* Table D-1 */
      switch (h264parse->sei_pic_struct) {
        case GST_H264_SEI_PIC_STRUCT_FRAME:
        case GST_H264_SEI_PIC_STRUCT_TOP_FIELD:
        case GST_H264_SEI_PIC_STRUCT_BOTTOM_FIELD:
          field_count = h264parse->sei_pic_struct;
          break;
        case GST_H264_SEI_PIC_STRUCT_TOP_BOTTOM:
          field_count = i + 1;
          break;
        case GST_H264_SEI_PIC_STRUCT_BOTTOM_TOP:
          field_count = 2 - i;
          break;
        case GST_H264_SEI_PIC_STRUCT_TOP_BOTTOM_TOP:
          field_count = i % 2 ? 2 : 1;
          break;
        case GST_H264_SEI_PIC_STRUCT_BOTTOM_TOP_BOTTOM:
          field_count = i % 2 ? 1 : 2;
          break;
        case GST_H264_SEI_PIC_STRUCT_FRAME_DOUBLING:
        case GST_H264_SEI_PIC_STRUCT_FRAME_TRIPLING:
          field_count = 0;
          break;
      }

      if (field_count == -1) {
        GST_WARNING_OBJECT (parse,
            "failed to determine field count for timecode");
        field_count = 0;
      }

      /* dropping of the two lowest (value 0 and 1) n_frames
       * counts when seconds_value is equal to 0 and
       * minutes_value is not an integer multiple of 10 */
      if (tim->counting_type == 4)
        flags |= GST_VIDEO_TIME_CODE_FLAGS_DROP_FRAME;

      n_frames =
          gst_util_uint64_scale_int (tim->n_frames, 1,
          2 - tim->nuit_field_based_flag);

      gst_buffer_add_video_time_code_meta_full (buffer,
          h264parse->parsed_fps_n,
          h264parse->parsed_fps_d,
          NULL,
          flags,
          tim->hours_flag ? tim->hours_value : 0,
          tim->minutes_flag ? tim->minutes_value : 0,
          tim->seconds_flag ? tim->seconds_value : 0, n_frames, field_count);
    }

    h264parse->num_clock_timestamp = 0;
  }

  gst_h264_sec_parse_reset_frame (h264parse);

  return GST_FLOW_OK;
}

static gboolean
gst_h264_sec_parse_set_caps (GstBaseParse * parse, GstCaps * caps)
{
  GstH264SecParse *h264parse;
  GstStructure *str;
  const GValue *codec_data_value;
  GstBuffer *codec_data = NULL;
  gsize size;
  guint format, align, off;
  GstH264NalUnit nalu;
  GstH264ParserResult parseres;
  GstCaps *old_caps;

  h264parse = GST_H264_SEC_PARSE (parse);

  /* reset */
  h264parse->push_codec = FALSE;

  old_caps = gst_pad_get_current_caps (GST_BASE_PARSE_SINK_PAD (parse));
  if (old_caps) {
    if (!gst_caps_is_equal (old_caps, caps))
      gst_h264_sec_parse_reset_stream_info (h264parse);
    gst_caps_unref (old_caps);
  }

  str = gst_caps_get_structure (caps, 0);

  /* accept upstream info if provided */
  gst_structure_get_int (str, "width", &h264parse->width);
  gst_structure_get_int (str, "height", &h264parse->height);
  gst_structure_get_fraction (str, "framerate", &h264parse->fps_num,
      &h264parse->fps_den);
  gst_structure_get_fraction (str, "pixel-aspect-ratio",
      &h264parse->upstream_par_n, &h264parse->upstream_par_d);

  /* get upstream format and align from caps */
  gst_h264_sec_parse_format_from_caps (caps, &format, &align);

  codec_data_value = gst_structure_get_value (str, "codec_data");

  h264parse->format_org = format;
  GST_INFO_OBJECT(h264parse, "original format is: %d", format);

  /* fix up caps without stream-format for max. backwards compatibility */
  if (format == GST_H264_SEC_PARSE_FORMAT_NONE) {
    /* codec_data implies avc */
    if (codec_data_value != NULL) {
      GST_ERROR ("video/x-h264 caps with codec_data but no stream-format=avc");
      format = GST_H264_SEC_PARSE_FORMAT_AVC;
    } else {
      /* otherwise assume bytestream input */
      GST_ERROR ("video/x-h264 caps without codec_data or stream-format");
      format = GST_H264_SEC_PARSE_FORMAT_BYTE;
    }
  }

  /* avc caps sanity checks */
  if (format == GST_H264_SEC_PARSE_FORMAT_AVC) {
    /* AVC requires codec_data, AVC3 might have one and/or SPS/PPS inline */
    if (codec_data_value == NULL)
      goto avc_caps_codec_data_missing;

    /* AVC implies alignment=au, everything else is not allowed */
    if (align == GST_H264_SEC_PARSE_ALIGN_NONE)
      align = GST_H264_SEC_PARSE_ALIGN_AU;
    else if (align != GST_H264_SEC_PARSE_ALIGN_AU)
      goto avc_caps_wrong_alignment;
  }

  /* bytestream caps sanity checks */
  if (format == GST_H264_SEC_PARSE_FORMAT_BYTE) {
    /* should have SPS/PSS in-band (and/or oob in streamheader field) */
    if (codec_data_value != NULL)
      goto bytestream_caps_with_codec_data;
  }

  /* packetized video has codec_data (required for AVC, optional for AVC3) */
  if (codec_data_value != NULL) {
    GstMapInfo map;
    guint8 *data;
    guint num_sps, num_pps;
#ifndef GST_DISABLE_GST_DEBUG
    guint profile;
#endif
    gint i;

    GST_DEBUG_OBJECT (h264parse, "have packetized h264");
    /* make note for optional split processing */
    h264parse->packetized = TRUE;

    /* codec_data field should hold a buffer */
    if (!GST_VALUE_HOLDS_BUFFER (codec_data_value))
      goto avc_caps_codec_data_wrong_type;

    codec_data = gst_value_get_buffer (codec_data_value);
    if (!codec_data)
      goto avc_caps_codec_data_missing;
    gst_buffer_map (codec_data, &map, GST_MAP_READ);
    data = map.data;
    size = map.size;

    /* parse the avcC data */
    if (size < 7) {
      /* when numSPS==0 and numPPS==0, length is 7 bytes */
      gst_buffer_unmap (codec_data, &map);
      goto avcc_too_small;
    }
    /* parse the version, this must be 1 */
    if (data[0] != 1) {
      gst_buffer_unmap (codec_data, &map);
      goto wrong_version;
    }
#ifndef GST_DISABLE_GST_DEBUG
    /* AVCProfileIndication */
    /* profile_compat */
    /* AVCLevelIndication */
    profile = (data[1] << 16) | (data[2] << 8) | data[3];
    GST_DEBUG_OBJECT (h264parse, "profile %06x", profile);
#endif

    /* 6 bits reserved | 2 bits lengthSizeMinusOne */
    /* this is the number of bytes in front of the NAL units to mark their
     * length */
    h264parse->nal_length_size = (data[4] & 0x03) + 1;
    GST_DEBUG_OBJECT (h264parse, "nal length size %u",
        h264parse->nal_length_size);

    num_sps = data[5] & 0x1f;
    off = 6;
    for (i = 0; i < num_sps; i++) {
      parseres = gst_h264_parser_identify_nalu_avc (h264parse->nalparser,
          data, off, size, 2, &nalu);
      if (parseres != GST_H264_PARSER_OK) {
        gst_buffer_unmap (codec_data, &map);
        goto avcc_too_small;
      }

      gst_h264_sec_parse_process_nal (h264parse, &nalu);
      off = nalu.offset + nalu.size;
    }

    if (off >= size) {
      gst_buffer_unmap (codec_data, &map);
      goto avcc_too_small;
    }
    num_pps = data[off];
    off++;

    for (i = 0; i < num_pps; i++) {
      parseres = gst_h264_parser_identify_nalu_avc (h264parse->nalparser,
          data, off, size, 2, &nalu);
      if (parseres != GST_H264_PARSER_OK) {
        gst_buffer_unmap (codec_data, &map);
        goto avcc_too_small;
      }

      gst_h264_sec_parse_process_nal (h264parse, &nalu);
      off = nalu.offset + nalu.size;
    }

    gst_buffer_unmap (codec_data, &map);

    gst_buffer_replace (&h264parse->codec_data_in, codec_data);

    /* don't confuse codec_data with inband sps/pps */
    h264parse->have_sps_in_frame = FALSE;
    h264parse->have_pps_in_frame = FALSE;
  } else if (format == GST_H264_SEC_PARSE_FORMAT_BYTE) {
    GST_DEBUG_OBJECT (h264parse, "have bytestream h264");
    /* nothing to pre-process */
    h264parse->packetized = FALSE;
    /* we have 4 sync bytes */
    h264parse->nal_length_size = 4;
  } else {
    /* probably AVC3 without codec_data field, anything to do here? */
  }

  {
    GstCaps *in_caps;

    /* prefer input type determined above */
    in_caps = gst_caps_new_simple ("video/x-h264",
        "parsed", G_TYPE_BOOLEAN, TRUE,
        "stream-format", G_TYPE_STRING,
        gst_h264_sec_parse_get_string (h264parse, TRUE, format),
        "alignment", G_TYPE_STRING,
        gst_h264_sec_parse_get_string (h264parse, FALSE, align), NULL);
    /* negotiate with downstream, sets ->format and ->align */
    gst_h264_sec_parse_negotiate (h264parse, format, in_caps);
    gst_caps_unref (in_caps);
  }

  if (format == h264parse->format && align == h264parse->align) {
    /* we did parse codec-data and might supplement src caps */
    gst_h264_sec_parse_update_src_caps (h264parse, caps);
  } else if (format == GST_H264_SEC_PARSE_FORMAT_AVC
      || format == GST_H264_SEC_PARSE_FORMAT_AVC3) {
    /* if input != output, and input is avc, must split before anything else */
    /* arrange to insert codec-data in-stream if needed.
     * src caps are only arranged for later on */
    h264parse->push_codec = TRUE;
    h264parse->have_sps = FALSE;
    h264parse->have_pps = FALSE;
    if (h264parse->align == GST_H264_SEC_PARSE_ALIGN_NAL)
      h264parse->split_packetized = TRUE;
    h264parse->packetized = TRUE;
  }

  h264parse->in_align = align;

  return TRUE;

  /* ERRORS */
avc_caps_codec_data_wrong_type:
  {
    GST_WARNING_OBJECT (parse, "H.264 AVC caps, codec_data field not a buffer");
    goto refuse_caps;
  }
avc_caps_codec_data_missing:
  {
    GST_WARNING_OBJECT (parse, "H.264 AVC caps, but no codec_data");
    goto refuse_caps;
  }
avc_caps_wrong_alignment:
  {
    GST_WARNING_OBJECT (parse, "H.264 AVC caps with NAL alignment, must be AU");
    goto refuse_caps;
  }
bytestream_caps_with_codec_data:
  {
    GST_WARNING_OBJECT (parse, "H.264 bytestream caps with codec_data is not "
        "expected, send SPS/PPS in-band with data or in streamheader field");
    goto refuse_caps;
  }
avcc_too_small:
  {
    GST_DEBUG_OBJECT (h264parse, "avcC size %" G_GSIZE_FORMAT " < 8", size);
    goto refuse_caps;
  }
wrong_version:
  {
    GST_DEBUG_OBJECT (h264parse, "wrong avcC version");
    goto refuse_caps;
  }
refuse_caps:
  {
    GST_WARNING_OBJECT (h264parse, "refused caps %" GST_PTR_FORMAT, caps);
    return FALSE;
  }
}

static void
remove_fields (GstCaps * caps, gboolean all)
{
  guint i, n;

  n = gst_caps_get_size (caps);
  for (i = 0; i < n; i++) {
    GstStructure *s = gst_caps_get_structure (caps, i);

    if (all) {
      gst_structure_remove_field (s, "alignment");
      gst_structure_remove_field (s, "stream-format");
    }
    gst_structure_remove_field (s, "parsed");
  }
}

static GstCaps *
gst_h264_sec_parse_get_caps (GstBaseParse * parse, GstCaps * filter)
{
  GstCaps *peercaps, *templ;
  GstCaps *res, *tmp, *pcopy;

  templ = gst_pad_get_pad_template_caps (GST_BASE_PARSE_SINK_PAD (parse));
  if (filter) {
    GstCaps *fcopy = gst_caps_copy (filter);
    /* Remove the fields we convert */
    remove_fields (fcopy, TRUE);
    peercaps = gst_pad_peer_query_caps (GST_BASE_PARSE_SRC_PAD (parse), fcopy);
    gst_caps_unref (fcopy);
  } else
    peercaps = gst_pad_peer_query_caps (GST_BASE_PARSE_SRC_PAD (parse), NULL);

  pcopy = gst_caps_copy (peercaps);
  remove_fields (pcopy, TRUE);

  res = gst_caps_intersect_full (pcopy, templ, GST_CAPS_INTERSECT_FIRST);
  gst_caps_unref (pcopy);
  gst_caps_unref (templ);

  if (filter) {
    GstCaps *tmp = gst_caps_intersect_full (res, filter,
        GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (res);
    res = tmp;
  }

  /* Try if we can put the downstream caps first */
  pcopy = gst_caps_copy (peercaps);
  remove_fields (pcopy, FALSE);
  tmp = gst_caps_intersect_full (pcopy, res, GST_CAPS_INTERSECT_FIRST);
  gst_caps_unref (pcopy);
  if (!gst_caps_is_empty (tmp))
    res = gst_caps_merge (tmp, res);
  else
    gst_caps_unref (tmp);

  gst_caps_unref (peercaps);
  return res;
}

static gboolean
gst_h264_sec_parse_event (GstBaseParse * parse, GstEvent * event)
{
  gboolean res;
  GstH264SecParse *h264parse = GST_H264_SEC_PARSE (parse);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:
    {
      GstCaps *caps;
      gst_event_parse_caps (event, &caps);
      GST_DEBUG_OBJECT (h264parse, "caps: %" GST_PTR_FORMAT, caps);
      res = GST_BASE_PARSE_CLASS (parent_class)->sink_event (parse, event);
      break;
    }
    case GST_EVENT_CUSTOM_DOWNSTREAM:
    {
      GstClockTime timestamp, stream_time, running_time;
      gboolean all_headers;
      guint count;

      if (gst_video_event_is_force_key_unit (event)) {
        gst_video_event_parse_downstream_force_key_unit (event,
            &timestamp, &stream_time, &running_time, &all_headers, &count);

        GST_INFO_OBJECT (h264parse,
            "received downstream force key unit event, "
            "seqnum %d running_time %" GST_TIME_FORMAT
            " all_headers %d count %d", gst_event_get_seqnum (event),
            GST_TIME_ARGS (running_time), all_headers, count);
        if (h264parse->force_key_unit_event) {
          GST_INFO_OBJECT (h264parse, "ignoring force key unit event "
              "as one is already queued");
        } else {
          h264parse->pending_key_unit_ts = running_time;
          gst_event_replace (&h264parse->force_key_unit_event, event);
        }
        gst_event_unref (event);
        res = TRUE;
      } else {
        res = GST_BASE_PARSE_CLASS (parent_class)->sink_event (parse, event);
        break;
      }
      break;
    }
    case GST_EVENT_FLUSH_STOP:
      h264parse->dts = GST_CLOCK_TIME_NONE;
      h264parse->ts_trn_nb = GST_CLOCK_TIME_NONE;
      h264parse->push_codec = TRUE;

      res = GST_BASE_PARSE_CLASS (parent_class)->sink_event (parse, event);
      break;
    case GST_EVENT_SEGMENT:
    {
      const GstSegment *segment;

      gst_event_parse_segment (event, &segment);
      /* don't try to mess with more subtle cases (e.g. seek) */
      if (segment->format == GST_FORMAT_TIME &&
          (segment->start != 0 || segment->rate != 1.0
              || segment->applied_rate != 1.0))
        h264parse->do_ts = FALSE;

      h264parse->last_report = GST_CLOCK_TIME_NONE;

      res = GST_BASE_PARSE_CLASS (parent_class)->sink_event (parse, event);
      break;
    }
    default:
      res = GST_BASE_PARSE_CLASS (parent_class)->sink_event (parse, event);
      break;
  }
  return res;
}

static gboolean
gst_h264_sec_parse_src_event (GstBaseParse * parse, GstEvent * event)
{
  gboolean res;
  GstH264SecParse *h264parse = GST_H264_SEC_PARSE (parse);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CUSTOM_UPSTREAM:
    {
      GstClockTime running_time;
      gboolean all_headers;
      guint count;

      if (gst_video_event_is_force_key_unit (event)) {
        gst_video_event_parse_upstream_force_key_unit (event,
            &running_time, &all_headers, &count);

        GST_INFO_OBJECT (h264parse, "received upstream force-key-unit event, "
            "seqnum %d running_time %" GST_TIME_FORMAT
            " all_headers %d count %d", gst_event_get_seqnum (event),
            GST_TIME_ARGS (running_time), all_headers, count);

        if (all_headers) {
          h264parse->pending_key_unit_ts = running_time;
          gst_event_replace (&h264parse->force_key_unit_event, event);
        }
      }
      res = GST_BASE_PARSE_CLASS (parent_class)->src_event (parse, event);
      break;
    }
    default:
      res = GST_BASE_PARSE_CLASS (parent_class)->src_event (parse, event);
      break;
  }

  return res;
}

static void
gst_h264_sec_parse_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstH264SecParse *parse;

  parse = GST_H264_SEC_PARSE (object);

  switch (prop_id) {
    case PROP_CONFIG_INTERVAL:
      parse->interval = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_h264_sec_parse_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstH264SecParse *parse;

  parse = GST_H264_SEC_PARSE (object);

  switch (prop_id) {
    case PROP_CONFIG_INTERVAL:
      g_value_set_int (value, parse->interval);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}
