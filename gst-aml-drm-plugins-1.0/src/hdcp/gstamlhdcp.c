/*
 * gstamlhdcp.c
 *
 *  Created on: Feb 5, 2020
 *      Author: tao
 */



#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif
#include <stdbool.h>
#include "gstamlhdcp.h"
#include "secmem_ca.h"
#include "gstsecmemallocator.h"

GST_DEBUG_CATEGORY_STATIC (gst_aml_hdcp_debug);
#define GST_CAT_DEFAULT gst_aml_hdcp_debug

#define GST_STATIC_CAPS_SINK GST_STATIC_CAPS("ANY")
static GstStaticPadTemplate sinktemplate = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-hdcp"));

static GstStaticPadTemplate srctemplate = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);


#define gst_aml_hdcp_parent_class parent_class
G_DEFINE_TYPE(GstAmlHDCP, gst_aml_hdcp, GST_TYPE_BASE_TRANSFORM);

static void             gst_aml_hdcp_set_property(GObject * object, guint prop_id, const GValue * value, GParamSpec * pspec);
static void             gst_aml_hdcp_get_property(GObject * object, guint prop_id, GValue * value, GParamSpec * pspec);
static gboolean         gst_aml_hdcp_start(GstBaseTransform *trans);
static gboolean         gst_aml_hdcp_stop(GstBaseTransform *trans);
static GstCaps*         gst_aml_hdcp_transform_caps(GstBaseTransform *trans, GstPadDirection direction, GstCaps *caps, GstCaps *filter);
static gboolean         gst_aml_hdcp_propose_allocation(GstBaseTransform *trans, GstQuery *decide_query, GstQuery *query);
static GstFlowReturn    gst_aml_prepare_output_buffer(GstBaseTransform * trans, GstBuffer *input, GstBuffer **outbuf);
static GstFlowReturn    gst_aml_hdcp_transform(GstBaseTransform *trans, GstBuffer *inbuf, GstBuffer *outbuf);
static GstFlowReturn    drm_hdcp_decrypt_all(GstAmlHDCP *plugin, GstBuffer *inbuf, GstBuffer *outbuf, unsigned char *iv, unsigned int iv_len);


static void
gst_aml_hdcp_class_init (GstAmlHDCPClass * klass)
{
    GObjectClass *gobject_class = (GObjectClass *) klass;
    GstElementClass *element_class = (GstElementClass *) klass;
    GstBaseTransformClass *base_class = GST_BASE_TRANSFORM_CLASS(klass);

    gobject_class->set_property = gst_aml_hdcp_set_property;
    gobject_class->get_property = gst_aml_hdcp_get_property;
    base_class->start = GST_DEBUG_FUNCPTR(gst_aml_hdcp_start);
    base_class->stop = GST_DEBUG_FUNCPTR(gst_aml_hdcp_stop);
    base_class->transform_caps = GST_DEBUG_FUNCPTR(gst_aml_hdcp_transform_caps);
    base_class->transform = GST_DEBUG_FUNCPTR(gst_aml_hdcp_transform);
    base_class->propose_allocation = GST_DEBUG_FUNCPTR(gst_aml_hdcp_propose_allocation);
    base_class->prepare_output_buffer = GST_DEBUG_FUNCPTR(gst_aml_prepare_output_buffer);
    base_class->passthrough_on_same_caps = FALSE;
    base_class->transform_ip_on_passthrough = FALSE;

    gst_element_class_add_pad_template (element_class,
            gst_static_pad_template_get (&sinktemplate));
    gst_element_class_add_pad_template (element_class,
            gst_static_pad_template_get (&srctemplate));

    g_object_class_install_property(gobject_class, PROP_DRM_STATIC_PIPELINE,
                                    g_param_spec_boolean("static-pipeline", "static pipeline",
                                                         "whether static pipeline", FALSE, G_PARAM_READWRITE));
    g_object_class_install_property(gobject_class, PROP_DRM_HDCP_CONTEXT,
                                    g_param_spec_pointer("set-hdcp-context", "set-hdcp-context",
                                                      "set the hdcp-context", G_PARAM_READWRITE));

    gst_element_class_set_details_simple(element_class,
            "Amlogic HDCP Plugin",
            "Decryptor/Converter/Video/DRM/HDCP",
            "HDCP Decryption Plugin",
            "mm@amlogic.com");
}

static void
gst_aml_hdcp_init(GstAmlHDCP * plugin)
{
    GstBaseTransform *base = GST_BASE_TRANSFORM (plugin);
    gst_base_transform_set_passthrough (base, FALSE);

    plugin->allocator = NULL;
    plugin->outcaps = NULL;
    plugin->static_pipeline = FALSE;
    plugin->hdcp_init_done = -1;
    plugin->hdcp_context = NULL;
    plugin->hdcp_handle = NULL;

}

void
gst_aml_hdcp_set_property(GObject * object, guint prop_id, const GValue * value, GParamSpec * pspec)
{
    GstAmlHDCP *plugin = GST_AMLHDCP(object);
    GST_DEBUG_OBJECT (object, "gst_aml_hdcp_set_property property:%d \n", prop_id);

    switch(prop_id) {
    case PROP_DRM_STATIC_PIPELINE:
        plugin->static_pipeline = g_value_get_boolean(value);
        break;
    case PROP_DRM_HDCP_CONTEXT:
        plugin->hdcp_context = (amlWfdHdcpHandle )g_value_get_pointer(value);
        plugin->hdcp_init_done = 0;
        GST_DEBUG_OBJECT(object, "gst_aml_hdcp_set_property set done:%p \n", plugin->hdcp_context);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

void
gst_aml_hdcp_get_property(GObject * object, guint prop_id, GValue * value, GParamSpec * pspec)
{
}

gboolean
gst_aml_hdcp_start(GstBaseTransform *trans)
{

    return TRUE;
}
gboolean
gst_aml_hdcp_stop(GstBaseTransform *trans)
{
    GstAmlHDCP *plugin = GST_AMLHDCP(trans);

    if (plugin->allocator) {
        gst_object_unref(plugin->allocator);
    }
    if (plugin->outcaps) {
        gst_caps_unref(plugin->outcaps);
    }
    plugin->hdcp_init_done = -1;
    return TRUE;
}

GstCaps*
gst_aml_hdcp_transform_caps(GstBaseTransform *trans, GstPadDirection direction,
        GstCaps *caps, GstCaps *filter)
{
    GstAmlHDCP *plugin = GST_AMLHDCP(trans);
    GstCaps *srccaps, *sinkcaps;
    GstCaps *ret = NULL;

    GST_DEBUG_OBJECT(plugin, "transform_caps direction:%d filter:%" GST_PTR_FORMAT, direction, filter);

    srccaps = gst_pad_get_pad_template_caps(GST_BASE_TRANSFORM_SRC_PAD(trans));
    sinkcaps = gst_pad_get_pad_template_caps(GST_BASE_TRANSFORM_SINK_PAD(trans));

  switch (direction) {
    case GST_PAD_SINK:
    {
       if (gst_caps_can_intersect(caps, sinkcaps)) {
            gboolean find = false;
            unsigned size;
            ret = gst_caps_copy(caps);
            size = gst_caps_get_size(ret);
            for (unsigned i = 0; i < size; ++i) {
                GstStructure *structure;
                structure = gst_caps_get_structure(caps, i);
                if (g_str_has_prefix(gst_structure_get_name(structure), "application/")) {
                    find = true;
                    ret = gst_caps_new_simple("video/x-h264",
                        "stream-format", G_TYPE_STRING, "byte-stream",
                        "alignment", G_TYPE_STRING, "nal", NULL);
                    gst_caps_set_features(ret, i,
                                          gst_caps_features_from_string(GST_CAPS_FEATURE_MEMORY_SECMEM_MEMORY));
                }
            }
            if (find) {
                if (!plugin->allocator) {
                    plugin->allocator = gst_secmem_allocator_new(false, false);
                }
                if (plugin->outcaps) {
                    gst_caps_unref(plugin->outcaps);
                }

                plugin->outcaps = gst_caps_ref(ret);
            } else {
                gst_caps_unref(ret);
                ret = gst_caps_copy(sinkcaps);
            }
        } else {
            ret = gst_caps_copy(sinkcaps);
        }
        break;
    }
    case GST_PAD_SRC:
        if (plugin->outcaps != NULL && gst_caps_can_intersect(caps, plugin->outcaps)) {
            ret = gst_caps_copy(caps);
            unsigned size = gst_caps_get_size(ret);
            for (unsigned i = 0; i < size; ++i) {
                gst_caps_set_features(ret, i, NULL);
            }
        } else if (gst_caps_can_intersect(caps, srccaps))
            ret = gst_caps_copy(caps);
        else
            ret = gst_caps_copy(srccaps);
        break;
    default:
        g_assert_not_reached();
    }

    if (filter) {
        GstCaps *intersection;
        intersection = gst_caps_intersect_full(filter, ret, GST_CAPS_INTERSECT_FIRST);
        gst_caps_unref(ret);
        ret = intersection;
    }

    GST_DEBUG_OBJECT (plugin, "transform_caps result: caps:%" GST_PTR_FORMAT, ret);
    gst_caps_unref(srccaps);
    gst_caps_unref(sinkcaps);
    return ret;
}

gboolean
gst_aml_hdcp_propose_allocation(GstBaseTransform *trans, GstQuery *decide_query, GstQuery *query)
{
    return GST_BASE_TRANSFORM_CLASS (parent_class)->propose_allocation(trans, decide_query, query);
}


GstFlowReturn
gst_aml_prepare_output_buffer(GstBaseTransform * trans, GstBuffer *inbuf, GstBuffer **outbuf)
{
    GstAmlHDCP *plugin = GST_AMLHDCP(trans);
    GstFlowReturn ret = GST_FLOW_OK;

    g_return_val_if_fail(plugin->allocator != NULL, GST_FLOW_ERROR);

    *outbuf = gst_buffer_new_allocate(plugin->allocator, gst_buffer_get_size(inbuf), NULL);

    g_return_val_if_fail(*outbuf != NULL, GST_FLOW_ERROR);

    GST_BASE_TRANSFORM_CLASS(parent_class)->copy_metadata (trans, inbuf, *outbuf);

    return ret;
}

GstFlowReturn
gst_aml_hdcp_transform(GstBaseTransform *trans, GstBuffer *inbuf, GstBuffer *outbuf)
{
    GstAmlHDCP *plugin = GST_AMLHDCP(trans);
    GstFlowReturn ret = GST_FLOW_OK;
    bool secure = false;
    GstBuffer *iv_buffer;
    GValue *value;
    GstMapInfo map;

    GstProtectionMeta *meta = gst_buffer_get_protection_meta(inbuf);
    if (meta) {
        GstStructure *drm_info = meta->info;
        if (!gst_structure_get(drm_info, "secure", G_TYPE_BOOLEAN,
                &secure, NULL)) {
            GST_INFO_OBJECT(plugin, "No drm_info, take it as clean!!!");
        } else {
            GST_DEBUG_OBJECT(plugin, "Found drm_info in ProtectionMeta.");
            value = gst_structure_get_value(drm_info, "iv");
            iv_buffer = gst_value_get_buffer (value);

            if (iv_buffer == NULL) {
                GST_ERROR_OBJECT(plugin, "Get iv from drm_info fail!!!");
                return GST_FLOW_ERROR;
            }

            GST_DEBUG_OBJECT(plugin, "Get iv from drm_info successfully");

            GST_INFO_OBJECT(plugin, "IV Length: %d", gst_buffer_get_size(iv_buffer));
        }
    } else {
        GST_INFO_OBJECT(plugin, "No ProtectionMeta, take it as clean data.");
    }

    if (secure) {
        gst_buffer_map(iv_buffer, &map, GST_MAP_READ);
        if (drm_hdcp_decrypt_all(plugin, inbuf, outbuf, map.data,
                map.size) == GST_FLOW_OK) {
            GST_INFO_OBJECT(plugin, "decrypt and push data seccessfully. ");
            ret = GST_FLOW_OK;
        } else {
            GST_ERROR_OBJECT(plugin, "decryption process error !!!");
            ret = GST_FLOW_ERROR;
        }
        gst_buffer_unmap(iv_buffer, &map);
    }else { // for have hdcp protocol but no iv
        ret = gst_buffer_copy_to_secmem(outbuf, inbuf) ? GST_FLOW_OK : GST_FLOW_ERROR;
        GST_INFO_OBJECT(plugin, "for have hdcp protocol but no iv secmem ret %d", ret);
    }
    return ret;
}

unsigned long long BitField(unsigned int val, unsigned char start, unsigned char size)
{
    unsigned char start_bit;
    unsigned char bit_count;
    unsigned int mask;
    unsigned int value;

    start_bit=start;
    bit_count= size;
    // generate mask
    if (bit_count == 32)
    {
        mask = 0xffffffff;
    }
    else
    {
        mask = 1;
        mask = mask << bit_count;
        mask = mask -1;
        mask = mask << start_bit;
    }
    value = val;
    unsigned long long rev = (((unsigned long long)(value & mask)) >> start_bit);
    return rev;
}

unsigned int
drm_hdcp_calc_stream_ctr(unsigned char *iv)
{
    unsigned int stream_ctr = 0;
    stream_ctr = (unsigned int)   (BitField(iv[0 + 1], 1, 2) << 30)
                                | (BitField(iv[0 + 2], 0, 8) << 22)
                                | (BitField(iv[0 + 3], 1, 7) << 15)
                                | (BitField(iv[0 + 4], 0, 8) << 7)
                                | (BitField(iv[0 + 5], 1, 7));

    return stream_ctr;
}


unsigned long long
drm_hdcp_calc_input_ctr(unsigned char *iv)
{
    unsigned long long input_ctr = 0;
    input_ctr = /*(unsigned long long)*/  (BitField(iv[0 + 7], 1, 4) << 60)
                                        | (BitField(iv[0 + 8], 0, 8) << 52)
                                        | (BitField(iv[0 + 9], 1, 7) << 45)
                                        | (BitField(iv[0 + 10], 0, 8) << 37)
                                        | (BitField(iv[0 + 11], 1, 7) << 30)
                                        | (BitField(iv[0 + 12], 0, 8) << 22)
                                        | (BitField(iv[0 + 13], 1, 7) << 15)
                                        | (BitField(iv[0 + 14], 0, 8) << 7)
                                        | (BitField(iv[0 + 15], 1, 7));

    return input_ctr;
}

amlWfdHdcpResultType drm_hdcp_init(GstAmlHDCP* drm)
{
    amlWfdHdcpResultType ret = HDCP_RESULT_SUCCESS;
    if (drm->static_pipeline == TRUE) {
        int hdcp_receiver_port=6789;
        ret = amlWfdHdcpInit((const char *)"127.0.0.1", hdcp_receiver_port, &drm->hdcp_handle);
        if (ret) {
            GST_ERROR("amlWfdHdcpInit failed  %d", ret);
            goto beach;
        }
        if (drm->hdcp_init_done == -1) {
            drm->hdcp_init_done = 0;
        }
    } else {
        GST_DEBUG_OBJECT(drm,
                "integrate_hdcp_log: Just print set-hdcp-context : hdcp_context = %p",
                drm->hdcp_context);
    }

beach:
    GST_DEBUG_OBJECT(drm, "drm_hdcp_init Out with ret=%d", ret);
    return ret;
}

static GstFlowReturn drm_hdcp_decrypt_trust_zone(GstAmlHDCP* drm, amlWfdHdcpHandle *hdcp, GstBuffer *inbuf, GstBuffer *outbuf, unsigned int stream_ctr, unsigned long long input_ctr)
{
    GstFlowReturn ret;
    GstMapInfo map;
    struct amlWfdHdcpDataInfo info = {0};
    GstMemory *mem;
    gsize size;
    amlWfdHdcpResultType err;
    unsigned long paddr;
    mem = gst_buffer_peek_memory(outbuf, 0);
    g_return_val_if_fail(gst_is_secmem_memory(mem), GST_FLOW_ERROR);

    size = gst_memory_get_sizes(mem, NULL, NULL);
    gst_buffer_map(inbuf, &map, GST_MAP_READ);
    if (map.size != size) {
        GST_ERROR("buffer size not match");
        goto beach;
    }
    GST_ERROR("here %d", map.size);
    paddr = (unsigned long)gst_secmem_memory_get_handle(mem);

    info.isAudio = 0;
    info.in = map.data;
    info.out = (uint8_t *)paddr;
    info.inSize = map.size;
    info.outSize = map.size;
    info.streamCtr = stream_ctr;
    info.inputCtr = input_ctr;
    err = amlWfdHdcpDecrypt(*hdcp, &info);
    if (err) {
        GST_ERROR("amlWfdHdcpDecrypt failed %d", err);
        goto beach;
    }
    ret = GST_FLOW_OK;

beach:
    gst_buffer_unmap(inbuf, &map);
    return ret;
}

GstFlowReturn drm_hdcp_decrypt_all(GstAmlHDCP *plugin, GstBuffer *inbuf, GstBuffer *outbuf, unsigned char *iv, unsigned int iv_len)
{
    int retry = 10;
    GstFlowReturn ret = GST_FLOW_ERROR;
    amlWfdHdcpHandle *hdcp;
    if (plugin->hdcp_init_done != 0) {
        if (plugin->static_pipeline) {
            if (drm_hdcp_init(plugin) != HDCP_RESULT_SUCCESS) {
                GST_ERROR_OBJECT(plugin,
                        "static-pipeline drm_hdcp_init() fail!!!!!!");
                return GST_FLOW_ERROR;
            }
        } else {
            while (retry-- >= 0) {
                sleep(1);
                if (plugin->hdcp_init_done == 0) {
                    break;
                }
            }
            if (plugin->hdcp_init_done != 0) {
                GST_ERROR_OBJECT(plugin,
                        "No hdcp context set by application (WFD), fatal error!!!!!!");
                return GST_FLOW_ERROR;
            }
        }
    }
    GST_ERROR("here");
    hdcp = (plugin->static_pipeline) ? &plugin->hdcp_handle : plugin->hdcp_context;
    unsigned int stream_ctr = drm_hdcp_calc_stream_ctr(iv);
    unsigned long long input_ctr = drm_hdcp_calc_input_ctr(iv);

    ret = drm_hdcp_decrypt_trust_zone(plugin, hdcp, inbuf, outbuf, stream_ctr, input_ctr);

    return ret;
}

#ifndef PACKAGE
#define PACKAGE "gst-plugins-amlogic"
#endif

static gboolean
amlhdcp_init (GstPlugin * amlhdcp)
{
    GST_DEBUG_CATEGORY_INIT(gst_aml_hdcp_debug, "amlhdcp", 0, "Amlogic HDCP Plugin");

    return gst_element_register(amlhdcp, "amlhdcp", GST_RANK_PRIMARY, GST_TYPE_AMLHDCP);
}


GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    amlhdcp,
    "Gstreamer HDCP plugin",
    amlhdcp_init,
    VERSION,
    "LGPL",
    "gst-plugins-drmhdcp",
    "http://amlogic.com/"
)
