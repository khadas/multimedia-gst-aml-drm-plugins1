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

#include <string.h>
#include <gst/base/gstbitreader.h>
#include <gst/base/gstbytewriter.h>
#include <gst/pbutils/pbutils.h>
#include "gst_aml_audio_parse.h"

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-aes128-cbc,"
        "original-media-type=(string) audio/mpeg,"
        "parsed = (boolean) false;"
        "application/x-aes128-cbc,"
        "original-media-type=(string) audio/x-ac3,"
        "parsed = (boolean) false;"
        "application/x-aes128-cbc,"
        "original-media-type=(string) audio/x-eac3,"
        "parsed = (boolean) false;"));

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-aes128-cbc,"
        "original-media-type=(string) audio/mpeg,"
        "parsed = (boolean) true;"
        "application/x-aes128-cbc,"
        "original-media-type=(string) audio/x-ac3,"
        "parsed = (boolean) true;"
        "application/x-aes128-cbc,"
        "original-media-type=(string) audio/x-eac3,"
        "parsed = (boolean) true;"));

GST_DEBUG_CATEGORY_STATIC (audio_parse_debug);
#define GST_CAT_DEFAULT audio_parse_debug

#define NEED_MIN_SIZE 10

static const struct
{
  const guint bit_rate;         /* nominal bit rate */
  const guint frame_size[3];    /* frame size for 32kHz, 44kHz, and 48kHz */
} frmsizcod_table[38] = {
  {
    32, {
  64, 69, 96}}, {
    32, {
  64, 70, 96}}, {
    40, {
  80, 87, 120}}, {
    40, {
  80, 88, 120}}, {
    48, {
  96, 104, 144}}, {
    48, {
  96, 105, 144}}, {
    56, {
  112, 121, 168}}, {
    56, {
  112, 122, 168}}, {
    64, {
  128, 139, 192}}, {
    64, {
  128, 140, 192}}, {
    80, {
  160, 174, 240}}, {
    80, {
  160, 175, 240}}, {
    96, {
  192, 208, 288}}, {
    96, {
  192, 209, 288}}, {
    112, {
  224, 243, 336}}, {
    112, {
  224, 244, 336}}, {
    128, {
  256, 278, 384}}, {
    128, {
  256, 279, 384}}, {
    160, {
  320, 348, 480}}, {
    160, {
  320, 349, 480}}, {
    192, {
  384, 417, 576}}, {
    192, {
  384, 418, 576}}, {
    224, {
  448, 487, 672}}, {
    224, {
  448, 488, 672}}, {
    256, {
  512, 557, 768}}, {
    256, {
  512, 558, 768}}, {
    320, {
  640, 696, 960}}, {
    320, {
  640, 697, 960}}, {
    384, {
  768, 835, 1152}}, {
    384, {
  768, 836, 1152}}, {
    448, {
  896, 975, 1344}}, {
    448, {
  896, 976, 1344}}, {
    512, {
  1024, 1114, 1536}}, {
    512, {
  1024, 1115, 1536}}, {
    576, {
  1152, 1253, 1728}}, {
    576, {
  1152, 1254, 1728}}, {
    640, {
  1280, 1393, 1920}}, {
    640, {
  1280, 1394, 1920}}
};


static gboolean gst_aml_audio_parse_start (GstBaseParse * parse);
static gboolean gst_aml_audio_parse_stop (GstBaseParse * parse);
static gboolean gst_aml_audio_parse_set_sink_caps (GstBaseParse * parse,
    GstCaps * caps);
static GstCaps *gst_aml_audio_parse_get_sink_caps (GstBaseParse * parse,
    GstCaps * filter);
static GstFlowReturn gst_aml_audio_parse_handle_frame (GstBaseParse * parse,
    GstBaseParseFrame * frame, gint * skip_size);
static GstFlowReturn gst_aml_audio_parse_pre_push_frame (GstBaseParse * parse,
    GstBaseParseFrame * frame);
static gboolean gst_aml_audio_parse_src_event (GstBaseParse * parse,
    GstEvent * event);

#define gst_aml_audio_parse_parent_class parent_class
G_DEFINE_TYPE (GstAmlAudioParse, gst_aml_audio_parse, GST_TYPE_BASE_PARSE);


static void
gst_aml_audio_parse_class_init (GstAmlAudioParseClass * klass)
{
    GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
    GstBaseParseClass *parse_class = GST_BASE_PARSE_CLASS (klass);

    GST_DEBUG_CATEGORY_INIT (audio_parse_debug, "amlaudioparse", 0, "Aml audio stream parser");

    gst_element_class_add_static_pad_template (element_class, &sink_template);
    gst_element_class_add_static_pad_template (element_class, &src_template);

    gst_element_class_set_static_metadata (element_class,
        "aml audio parse",
        "Codec/Parser/Audio",
        "aml audio parse",
        "http://amlogic.com");

    parse_class->start = GST_DEBUG_FUNCPTR (gst_aml_audio_parse_start);
    parse_class->stop = GST_DEBUG_FUNCPTR (gst_aml_audio_parse_stop);
    parse_class->set_sink_caps = GST_DEBUG_FUNCPTR (gst_aml_audio_parse_set_sink_caps);
    parse_class->get_sink_caps = GST_DEBUG_FUNCPTR (gst_aml_audio_parse_get_sink_caps);
    parse_class->handle_frame = GST_DEBUG_FUNCPTR (gst_aml_audio_parse_handle_frame);
    parse_class->pre_push_frame = GST_DEBUG_FUNCPTR (gst_aml_audio_parse_pre_push_frame);
    parse_class->src_event = GST_DEBUG_FUNCPTR (gst_aml_audio_parse_src_event);
}


static void
gst_aml_audio_parse_init (GstAmlAudioParse * audioparse)
{
    GST_PAD_SET_ACCEPT_INTERSECT (GST_BASE_PARSE_SINK_PAD (audioparse));
    GST_PAD_SET_ACCEPT_TEMPLATE (GST_BASE_PARSE_SINK_PAD (audioparse));
}


static gboolean
gst_aml_audio_parse_set_src_caps (GstAmlAudioParse * audioparse, GstCaps * sink_caps)
{
    GstStructure *s;
    GstCaps *src_caps = NULL, *peercaps = NULL;
    gboolean res = FALSE;
    const gchar *stream_format;

    GST_DEBUG_OBJECT (audioparse, "sink caps: %" GST_PTR_FORMAT, sink_caps);

    switch (audioparse->media_type) {
        case MEDIA_TYPE_AAC_ADTS:
            src_caps = gst_caps_new_simple ("application/x-aes128-cbc",
                "mpegversion", G_TYPE_INT, 4,
                "stream-format", G_TYPE_STRING, "adts",
                "original-media-type", G_TYPE_STRING, "audio/mpeg",
                "encryption-algorithm", G_TYPE_STRING, "AES",
                "cipher-mode", G_TYPE_STRING, "cbcs", NULL);
            break;
        case MEDIA_TYPE_AC3:
            src_caps = gst_caps_new_simple ("application/x-aes128-cbc",
                "original-media-type", G_TYPE_STRING, "audio/x-ac3",
                "encryption-algorithm", G_TYPE_STRING, "AES",
                "cipher-mode", G_TYPE_STRING, "cbcs", NULL);
            break;
        case MEDIA_TYPE_EAC3:
            src_caps = gst_caps_new_simple ("application/x-aes128-cbc",
                "original-media-type", G_TYPE_STRING, "audio/x-eac3",
                "encryption-algorithm", G_TYPE_STRING, "AES",
                "cipher-mode", G_TYPE_STRING, "cbcs", NULL);
            break;
        default:
            break;
    }

    peercaps = gst_pad_peer_query_caps (GST_BASE_PARSE_SRC_PAD (audioparse), NULL);
    GST_DEBUG_OBJECT (audioparse, "peer caps: %" GST_PTR_FORMAT, peercaps);
    if (peercaps && !gst_caps_can_intersect (src_caps, peercaps)) {
        GST_DEBUG_OBJECT (GST_BASE_PARSE (audioparse)->srcpad,
            "Caps can not intersect");
    }
    if (peercaps)
        gst_caps_unref (peercaps);

    GST_DEBUG_OBJECT (audioparse, "setting src caps: %" GST_PTR_FORMAT, src_caps);

    res = gst_pad_set_caps (GST_BASE_PARSE (audioparse)->srcpad, src_caps);

    gst_caps_unref (src_caps);

    return res;
}


static gboolean
gst_aml_audio_parse_set_sink_caps (GstBaseParse * parse, GstCaps * caps)
{
    GstAmlAudioParse *audioparse = GST_AML_AUDIO_PARSE (parse);
    GST_DEBUG_OBJECT (audioparse, "caps: %" GST_PTR_FORMAT, caps);

    guint size = gst_caps_get_size(caps);
    for (int i = 0; i < size; i++) {
        GstStructure *in_structure = gst_caps_get_structure(caps, i);
        gchar* media_type = (gchar *)gst_structure_get_string(in_structure, "original-media-type");
        if (strcasecmp (media_type, "audio/mpeg") == 0) {
            audioparse->media_type = MEDIA_TYPE_AAC_ADTS;
        } else if (strcasecmp (media_type, "audio/x-ac3") == 0) {
            audioparse->media_type = MEDIA_TYPE_AC3;
        } else if (strcasecmp (media_type, "audio/x-eac3") == 0) {
            audioparse->media_type = MEDIA_TYPE_EAC3;
        }
    }

    if (!gst_aml_audio_parse_set_src_caps (audioparse, caps)) {
        /* If linking fails, we need to return appropriate error */
        return FALSE;
    }

    return TRUE;
}


static GstCaps *
gst_aml_audio_parse_get_sink_caps (GstBaseParse * parse, GstCaps * filter)
{
    GstAmlAudioParse *audioparse = GST_AML_AUDIO_PARSE (parse);
    GstCaps *incaps;
    incaps = gst_pad_get_current_caps (GST_BASE_PARSE_SINK_PAD (audioparse));
    GST_DEBUG_OBJECT (audioparse, "incaps: %" GST_PTR_FORMAT, incaps);

    return incaps;
}


static guint
gst_aml_audio_parse_ac3_get_frame_len (const guint8 * data, const guint avail)
{
    GstBitReader bits;
    guint8 fscod, frmsizcod;
    guint frame_size;
    gst_bit_reader_init (&bits, data, avail);
    gst_bit_reader_skip_unchecked (&bits, 16 + 16);
    fscod = gst_bit_reader_get_bits_uint8_unchecked (&bits, 2);
    frmsizcod = gst_bit_reader_get_bits_uint8_unchecked (&bits, 6);

    if (G_UNLIKELY (fscod == 3 || frmsizcod >= G_N_ELEMENTS (frmsizcod_table))) {
        GST_ERROR ("bad fscod %d frmsizcod %d", fscod, frmsizcod);
        return avail;
    }

    frame_size = frmsizcod_table[frmsizcod].frame_size[fscod] * 2;
    return frame_size;
}


static guint
gst_aml_audio_parse_eac3_get_frame_len (const guint8 * data, const guint avail)
{
    GstBitReader bits;
    guint8 strmtyp, strmid, frmsiz, fscod, fscod2;
    guint frame_size;
    gst_bit_reader_init (&bits, data, avail);
    gst_bit_reader_skip_unchecked (&bits, 16);
    strmtyp = gst_bit_reader_get_bits_uint8_unchecked (&bits, 2);

    if (G_UNLIKELY (strmtyp == 3)) {
        GST_ERROR ("bad strmtyp %d", strmtyp);
        return avail;
    }

    strmid = gst_bit_reader_get_bits_uint8_unchecked (&bits, 3);
    frmsiz = gst_bit_reader_get_bits_uint16_unchecked (&bits, 11);
    fscod = gst_bit_reader_get_bits_uint8_unchecked (&bits, 2);

    if (fscod == 3) {
        fscod2 = gst_bit_reader_get_bits_uint8_unchecked (&bits, 2);
        if (G_UNLIKELY (fscod2 == 3)) {
            GST_ERROR ("invalid fscod2");
            return avail;
        }
    }

    frame_size = (frmsiz + 1) * 2;
    return frame_size;
}


static gboolean
gst_aml_audio_parse_check_ac3_frame (GstAmlAudioParse * audioparse,
    GstBaseParseFrame * frame,
    const guint8 * data, const guint avail, gboolean drain,
    guint * frame_size, guint * needed_data)
{
    GstBitReader bits;
    guint8 fscod, frmsizcod;
    guint subsample_count;
    GstBuffer *subsamples;
    GstByteWriter *subsample_writer;
    *needed_data = 0;


    if (G_UNLIKELY (avail < 3)) {
        *needed_data = 3;
        return FALSE;
    }

    if ((data[0] == 0x0b) && (data[1] == 0x77)) {

        if (G_UNLIKELY (avail < 16)) {
            *needed_data = 16;
            return FALSE;
        }

        gst_bit_reader_init (&bits, data, avail);
        gst_bit_reader_skip_unchecked (&bits, 16 + 16);
        fscod = gst_bit_reader_get_bits_uint8_unchecked (&bits, 2);
        frmsizcod = gst_bit_reader_get_bits_uint8_unchecked (&bits, 6);

        if (G_UNLIKELY (fscod == 3 || frmsizcod >= G_N_ELEMENTS (frmsizcod_table))) {
            GST_DEBUG_OBJECT (audioparse, "bad fscod %d frmsizcod %d", fscod, frmsizcod);
            return FALSE;
        }

        *frame_size = frmsizcod_table[frmsizcod].frame_size[fscod] * 2;
        if (*frame_size < 16) {
            GST_ERROR_OBJECT (audioparse, "Invalid frame len %d", *frame_size);
            *needed_data = 16;
            return FALSE;
        }

        subsample_writer = gst_byte_writer_new_with_size (sizeof (guint16) + sizeof (guint32), FALSE);
        gst_byte_writer_put_uint16_be (subsample_writer, 16);
        gst_byte_writer_put_uint32_be (subsample_writer, *frame_size - 16);

        subsamples = gst_byte_writer_free_and_get_buffer (subsample_writer);
        subsample_count = gst_buffer_get_size (subsamples) / (sizeof (guint16) + sizeof (guint32));
        GST_DEBUG_OBJECT (audioparse, "Subsamples %d %d", 16, *frame_size - 16);
        GST_DEBUG_OBJECT (audioparse, "SubsampleCount %d ", subsample_count);

        if (*frame_size + NEED_MIN_SIZE > avail) {
            /* We have found a possible frame header candidate, but can't be
                sure since we don't have enough data to check the next frame */
            GST_DEBUG_OBJECT (audioparse, "NEED MORE DATA: we need %d, available %d",
                *frame_size + NEED_MIN_SIZE, avail);
            *needed_data = *frame_size + NEED_MIN_SIZE;
            return FALSE;
        }

        frame->out_buffer = gst_buffer_copy_region (frame->buffer, GST_BUFFER_COPY_ALL, 0, *frame_size);
        GstProtectionMeta *meta = gst_buffer_get_protection_meta (frame->out_buffer);
        if (meta && meta->info) {
            gst_structure_set (meta->info,
                "subsample_count", G_TYPE_UINT, subsample_count,
                "subsamples", GST_TYPE_BUFFER, subsamples, NULL);
        } else {
            GST_ERROR_OBJECT (audioparse, "no protection meta");
        }
        gst_buffer_unref (subsamples);

        if ((data[*frame_size] == 0x0b) && (data[*frame_size + 1] == 0x77)) {
            guint next_len = gst_aml_audio_parse_ac3_get_frame_len (data + (*frame_size), avail - (*frame_size));
            GST_DEBUG_OBJECT (audioparse, "AC3 frame found, len: %d bytes", *frame_size);
            gst_base_parse_set_min_frame_size (GST_BASE_PARSE (audioparse),
                next_len + NEED_MIN_SIZE);
            return TRUE;
        }
    }
    return FALSE;
}


static gboolean
gst_aml_audio_parse_check_eac3_frame (GstAmlAudioParse * audioparse,
    GstBaseParseFrame * frame,
    const guint8 * data, const guint avail, gboolean drain,
    guint * frame_size, guint * needed_data)
{
    GstBitReader bits;
    guint8 strmtyp, strmid, frmsiz, fscod, fscod2;
    guint subsample_count;
    GstBuffer *subsamples;
    GstByteWriter *subsample_writer;
    *needed_data = 0;

    if (G_UNLIKELY (avail < 3)) {
        *needed_data = 3;
        return FALSE;
    }

    if ((data[0] == 0x0b) && (data[1] == 0x77)) {

        if (G_UNLIKELY (avail < 16)) {
            *needed_data = 16;
            return FALSE;
        }

        gst_bit_reader_init (&bits, data, avail);
        gst_bit_reader_skip_unchecked (&bits, 16);
        strmtyp = gst_bit_reader_get_bits_uint8_unchecked (&bits, 2);

        if (G_UNLIKELY (strmtyp == 3)) {
            GST_DEBUG_OBJECT (audioparse, "bad strmtyp %d", strmtyp);
            return FALSE;
        }

        strmid = gst_bit_reader_get_bits_uint8_unchecked (&bits, 3);
        frmsiz = gst_bit_reader_get_bits_uint16_unchecked (&bits, 11);
        fscod = gst_bit_reader_get_bits_uint8_unchecked (&bits, 2);

        if (fscod == 3) {
            fscod2 = gst_bit_reader_get_bits_uint8_unchecked (&bits, 2);
            if (G_UNLIKELY (fscod2 == 3)) {
                GST_DEBUG_OBJECT (audioparse, "invalid fscod2");
                return FALSE;
            }
        }

        *frame_size = (frmsiz + 1) * 2;
        if (*frame_size < 16) {
            GST_ERROR_OBJECT (audioparse, "Invalid frame len %d", *frame_size);
            *needed_data = 16;
            return FALSE;
        }


        subsample_writer = gst_byte_writer_new_with_size (sizeof (guint16) + sizeof (guint32), FALSE);
        gst_byte_writer_put_uint16_be (subsample_writer, 16);
        gst_byte_writer_put_uint32_be (subsample_writer, *frame_size - 16);

        subsamples = gst_byte_writer_free_and_get_buffer (subsample_writer);
        subsample_count = gst_buffer_get_size (subsamples) / (sizeof (guint16) + sizeof (guint32));
        GST_DEBUG_OBJECT (audioparse, "Subsamples %d %d", 16, *frame_size - 16);
        GST_DEBUG_OBJECT (audioparse, "SubsampleCount %d ", subsample_count);

        if (*frame_size + NEED_MIN_SIZE > avail) {
            /* We have found a possible frame header candidate, but can't be
                sure since we don't have enough data to check the next frame */
            GST_DEBUG_OBJECT (audioparse, "NEED MORE DATA: we need %d, available %d",
                *frame_size + NEED_MIN_SIZE, avail);
            *needed_data = *frame_size + NEED_MIN_SIZE;
            return FALSE;
        }

        frame->out_buffer = gst_buffer_copy_region (frame->buffer, GST_BUFFER_COPY_ALL, 0, *frame_size);
        GstProtectionMeta *meta = gst_buffer_get_protection_meta (frame->out_buffer);
        if (meta && meta->info) {
            gst_structure_set (meta->info,
                "subsample_count", G_TYPE_UINT, subsample_count,
                "subsamples", GST_TYPE_BUFFER, subsamples, NULL);
        } else {
            GST_ERROR_OBJECT (audioparse, "no protection meta");
        }
        gst_buffer_unref (subsamples);

        if ((data[*frame_size] == 0x0b) && (data[*frame_size + 1] == 0x77)) {
            guint next_len = gst_aml_audio_parse_eac3_get_frame_len (data + (*frame_size), avail - (*frame_size));
            GST_DEBUG_OBJECT (audioparse, "EAC3 frame found, len: %d bytes", *frame_size);
            gst_base_parse_set_min_frame_size (GST_BASE_PARSE (audioparse),
                next_len + NEED_MIN_SIZE);
            return TRUE;
        }
    }
    return FALSE;
}


static inline guint
gst_aml_audio_parse_adts_get_frame_len (const guint8 * data)
{
    return ((data[3] & 0x03) << 11) | (data[4] << 3) | ((data[5] & 0xe0) >> 5);
}


static gboolean
gst_aml_audio_parse_check_adts_frame (GstAmlAudioParse * audioparse,
    GstBaseParseFrame * frame,
    const guint8 * data, const guint avail, gboolean drain,
    guint * frame_size, guint * needed_data)
{
    guint crc_size;
    gint subsample_count;
    GstBuffer *subsamples;
    GstByteWriter *subsample_writer;
    *needed_data = 0;

    if (G_UNLIKELY (avail < 3)) {
        *needed_data = 3;
        return FALSE;
    }

    if ((data[0] == 0xff) && ((data[1] & 0xf6) == 0xf0)) {

        /* Sampling frequency test */
        if (G_UNLIKELY ((data[2] & 0x3C) >> 2 == 15))
            return FALSE;

        if (G_UNLIKELY (avail < 7)) {
            *needed_data = 7;
            return FALSE;
        }

        *frame_size = gst_aml_audio_parse_adts_get_frame_len (data);

        crc_size = (data[1] & 0x01) ? 0 : 2;

        if (*frame_size < 7 + 16 + crc_size) {
            GST_ERROR_OBJECT (audioparse, "Invalid frame len %d", *frame_size);
            *needed_data = 7 + 16 + crc_size;
            return FALSE;
        }

        subsample_writer = gst_byte_writer_new_with_size (sizeof (guint16) + sizeof (guint32), FALSE);
        gst_byte_writer_put_uint16_be (subsample_writer, 7 + crc_size + 16);
        gst_byte_writer_put_uint32_be (subsample_writer, *frame_size - 7 - crc_size - 16);

        subsamples = gst_byte_writer_free_and_get_buffer (subsample_writer);
        subsample_count = gst_buffer_get_size (subsamples) / (sizeof (guint16) + sizeof (guint32));
        GST_DEBUG_OBJECT (audioparse, "Subsamples %d %d", 7 + crc_size + 16, *frame_size - 7 - crc_size - 16);
        GST_DEBUG_OBJECT (audioparse, "SubsampleCount %d ", subsample_count);

        if (*frame_size + NEED_MIN_SIZE > avail) {
            /* We have found a possible frame header candidate, but can't be
                sure since we don't have enough data to check the next frame */
            GST_DEBUG_OBJECT (audioparse, "NEED MORE DATA: we need %d, available %d",
                *frame_size + NEED_MIN_SIZE, avail);
            *needed_data = *frame_size + NEED_MIN_SIZE;
            return FALSE;
        }

        frame->out_buffer = gst_buffer_copy_region (frame->buffer, GST_BUFFER_COPY_ALL, 0, *frame_size);
        GstProtectionMeta *meta = gst_buffer_get_protection_meta (frame->out_buffer);
        if (meta && meta->info) {
            gst_structure_set (meta->info,
                "subsample_count", G_TYPE_UINT, subsample_count,
                "subsamples", GST_TYPE_BUFFER, subsamples, NULL);
        } else {
            GST_ERROR_OBJECT (audioparse, "no protection meta");
        }
        gst_buffer_unref (subsamples);

        if ((data[*frame_size] == 0xff) && ((data[*frame_size + 1] & 0xf6) == 0xf0)) {
            guint next_len = gst_aml_audio_parse_adts_get_frame_len (data + (*frame_size));
            GST_DEBUG_OBJECT (audioparse, "ADTS frame found, len: %d bytes", *frame_size);
            gst_base_parse_set_min_frame_size (GST_BASE_PARSE (audioparse),
                next_len + NEED_MIN_SIZE);
            return TRUE;
        }
    }
    return FALSE;
}


static GstFlowReturn
gst_aml_audio_parse_handle_frame (GstBaseParse * parse,
    GstBaseParseFrame * frame, gint * skip_size)
{
    GstMapInfo map;
    GstAmlAudioParse *audioparse;
    gboolean ret = FALSE;
    GstBuffer *buffer;
    guint buffer_size;
    guint frame_size = 0;
    guint needed_data = 1024;
    *skip_size = 0;

    audioparse = GST_AML_AUDIO_PARSE (parse);

    buffer = frame->buffer;

    gst_buffer_map (buffer, &map, GST_MAP_READ);
    buffer_size = map.size;

    GST_DEBUG_OBJECT (audioparse, "buffer size %d type %d", buffer_size, audioparse->media_type);

    switch (audioparse->media_type) {
        case MEDIA_TYPE_AAC_ADTS:
            ret = gst_aml_audio_parse_check_adts_frame (audioparse, frame, map.data, map.size,
                GST_BASE_PARSE_DRAINING (parse), &frame_size, &needed_data);
            break;
        case MEDIA_TYPE_AC3:
            ret = gst_aml_audio_parse_check_ac3_frame (audioparse, frame, map.data, map.size,
                GST_BASE_PARSE_DRAINING (parse), &frame_size, &needed_data);
            break;
        case MEDIA_TYPE_EAC3:
            ret = gst_aml_audio_parse_check_eac3_frame (audioparse, frame, map.data, map.size,
                GST_BASE_PARSE_DRAINING (parse), &frame_size, &needed_data);
            break;
        default:
            break;
    }

    if (!ret && needed_data) {
        gst_base_parse_set_min_frame_size (GST_BASE_PARSE (audioparse), needed_data);
    }

    if (G_UNLIKELY (!ret))
        goto exit;

exit:
    gst_buffer_unmap (buffer, &map);

    if (ret && frame_size <= buffer_size) {
        return gst_base_parse_finish_frame (parse, frame, frame_size);
    }

    return GST_FLOW_OK;
}


static GstFlowReturn
gst_aml_audio_parse_pre_push_frame (GstBaseParse * parse, GstBaseParseFrame * frame)
{
    return GST_FLOW_OK;
}


static gboolean
gst_aml_audio_parse_start (GstBaseParse * parse)
{
    GstAmlAudioParse *audioparse;

    audioparse = GST_AML_AUDIO_PARSE (parse);
    GST_INFO_OBJECT (audioparse, "%s", __func__);
    audioparse->frame_samples = 1024;
    gst_base_parse_set_min_frame_size (GST_BASE_PARSE (audioparse), NEED_MIN_SIZE);
    audioparse->media_type = MEDIA_TYPE_UNKNOWN;
    return TRUE;
}


static gboolean
gst_aml_audio_parse_stop (GstBaseParse * parse)
{
    GstAmlAudioParse *audioparse;

    audioparse = GST_AML_AUDIO_PARSE (parse);
    GST_INFO_OBJECT (audioparse, "%s", __func__);
    return TRUE;
}


static gboolean
gst_aml_audio_parse_src_event (GstBaseParse * parse, GstEvent * event)
{
    return GST_BASE_PARSE_CLASS (parent_class)->src_event (parse, event);
}
