/*
 * gstdummydrm.c
 *
 *  Created on: 2020年4月1日
 *      Author: tao
 */


#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif
#include <stdbool.h>
#include "gstdummydrm.h"
#include "gstsecmemallocator.h"

#define TS_PKT_SIZE 188
#define TS_BUFFER_SIZE (160 * TS_PKT_SIZE)

GST_DEBUG_CATEGORY_STATIC (gst_dummydrm_debug);
#define GST_CAT_DEFAULT gst_dummydrm_debug

static GstStaticPadTemplate sinktemplate = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

static GstStaticPadTemplate srctemplate = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS_ANY);


#define gst_dummydrm_parent_class parent_class
G_DEFINE_TYPE(GstDummyDrm, gst_dummydrm, GST_TYPE_BASE_TRANSFORM);

static void             gst_dummydrm_set_property(GObject * object, guint prop_id, const GValue * value, GParamSpec * pspec);
static void             gst_dummydrm_get_property(GObject * object, guint prop_id, GValue * value, GParamSpec * pspec);
static gboolean         gst_dummydrm_start(GstBaseTransform *trans);
static gboolean         gst_dummydrm_stop(GstBaseTransform *trans);
static GstCaps*         gst_dummydrm_transform_caps(GstBaseTransform *trans, GstPadDirection direction, GstCaps *caps, GstCaps *filter);
static GstFlowReturn    gst_dummydrm_prepare_output_buffer(GstBaseTransform * trans, GstBuffer *input, GstBuffer **outbuf);
static GstFlowReturn    gst_dummydrm_transform(GstBaseTransform *trans, GstBuffer *inbuf, GstBuffer *outbuf);
static GstFlowReturn    gst_dummydrm_chain (GstPad * pad, GstObject * parent, GstBuffer * buffer);
static gboolean         gst_dummydrm_sink_eventfunc (GstBaseTransform * trans, GstEvent * event);

enum
{
    PROP_0,
    PROP_IS_4K,
    PROP_STREAM_MODE,
};

static void
gst_dummydrm_class_init (GstDummyDrmClass * klass)
{
    GObjectClass *gobject_class = (GObjectClass *) klass;
    GstElementClass *element_class = (GstElementClass *) klass;
    GstBaseTransformClass *base_class = GST_BASE_TRANSFORM_CLASS(klass);

    gobject_class->set_property = gst_dummydrm_set_property;
    gobject_class->get_property = gst_dummydrm_get_property;

    base_class->sink_event = GST_DEBUG_FUNCPTR(gst_dummydrm_sink_eventfunc);
    base_class->start = GST_DEBUG_FUNCPTR(gst_dummydrm_start);
    base_class->stop = GST_DEBUG_FUNCPTR(gst_dummydrm_stop);
    base_class->transform_caps = GST_DEBUG_FUNCPTR(gst_dummydrm_transform_caps);
    base_class->transform = GST_DEBUG_FUNCPTR(gst_dummydrm_transform);
    base_class->prepare_output_buffer = GST_DEBUG_FUNCPTR(gst_dummydrm_prepare_output_buffer);
    base_class->passthrough_on_same_caps = FALSE;
    base_class->transform_ip_on_passthrough = FALSE;

    g_object_class_install_property(gobject_class, PROP_IS_4K,
                                    g_param_spec_boolean("is-4k", "is-4k",
                                                         "is 4k stream",
                                                         FALSE,
                                                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(gobject_class, PROP_STREAM_MODE,
                                    g_param_spec_boolean("stream-mode", "stream-mode",
                                                         "secmem stream mode",
                                                         FALSE,
                                                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    gst_element_class_add_pad_template (element_class,
            gst_static_pad_template_get (&sinktemplate));
    gst_element_class_add_pad_template (element_class,
            gst_static_pad_template_get (&srctemplate));

    gst_element_class_set_details_simple(element_class,
            "Amlogic Dummy DRM Plugin",
            "Filter/DRM/Dummy",
            "DRM Decryption Plugin",
            "mm@amlogic.com");
}

static void
gst_dummydrm_init(GstDummyDrm * plugin)
{
    GstBaseTransform *base = GST_BASE_TRANSFORM (plugin);
    gst_base_transform_set_passthrough (base, FALSE);

    plugin->allocator = NULL;
    plugin->outcaps = NULL;
    plugin->is_4k = FALSE;
    plugin->stream_mode = FALSE;
    plugin->need_alignment = FALSE;
    plugin->adapter = NULL;
    plugin->base_chain= base->sinkpad->chainfunc;
    gst_pad_set_chain_function(base->sinkpad, gst_dummydrm_chain);
}

static GstFlowReturn gst_dummydrm_chain (GstPad * pad, GstObject * parent, GstBuffer * inbuf)
{
    GstDummyDrm * plugin = NULL;
    GstBuffer * outbuf = NULL;
    guint available = 0;

    plugin = (GstDummyDrm *)gst_pad_get_parent(pad);

    if (!plugin)
        return GST_FLOW_ERROR;
    if (G_UNLIKELY(plugin->need_alignment && !plugin->adapter))
    {
        plugin->adapter = gst_adapter_new();
        if (!plugin->adapter)
            return GST_FLOW_ERROR;
    }
    if (!plugin->base_chain)
        return GST_FLOW_ERROR;

    if (plugin->adapter)
    {
        gst_adapter_push(plugin->adapter, inbuf);
        available = gst_adapter_available(plugin->adapter);
        if (available >= TS_BUFFER_SIZE)
        {
            outbuf = gst_adapter_take_buffer(plugin->adapter, TS_BUFFER_SIZE);
            if (outbuf)
            {
                GST_DEBUG_OBJECT(plugin, "got %d bytes from adaptor. pass to base chain func", TS_BUFFER_SIZE);
                goto base;
            }
            else
            {
                GST_DEBUG_OBJECT(plugin, "get buf from adaptor meet error");
                return GST_FLOW_ERROR;
            }
        }
        else
        {
            GST_DEBUG_OBJECT(plugin, "adaptor available size:%d < needed size:%d. wait more data", available, TS_BUFFER_SIZE);
            return GST_FLOW_OK;
        }
    }

base:
    return plugin->base_chain(pad, parent, outbuf);
}

static gboolean gst_dummydrm_sink_eventfunc (GstBaseTransform * trans, GstEvent * event)
{
    gboolean ret = TRUE;
    GstDummyDrm *plugin = GST_DUMMYDRM(trans);

    GST_DEBUG_OBJECT(plugin, "event: %" GST_PTR_FORMAT, event);

    switch (GST_EVENT_TYPE (event)) {
        case GST_EVENT_FLUSH_START:
        {
            if (plugin && plugin->adapter)
                gst_adapter_clear(plugin->adapter);
            break;
        }
        case GST_EVENT_EOS:
        {
            if (plugin && plugin->adapter)
            {
                guint available;
                guint available_aligned;
                GstBuffer *lastbuf;
                GstBaseTransform *base = GST_BASE_TRANSFORM (plugin);

                available = gst_adapter_available(plugin->adapter);
                available_aligned = available / TS_PKT_SIZE * TS_PKT_SIZE;
                if (available_aligned)
                {
                    lastbuf = gst_adapter_take_buffer(plugin->adapter, available_aligned);
                    if (lastbuf)
                    {
                        GST_DEBUG_OBJECT(plugin, "meet eos. got %d bytes from adaptor. pass to base chain func", available_aligned);
                        plugin->base_chain(base->sinkpad, plugin, lastbuf);
                    }
                    else
                    {
                        GST_DEBUG_OBJECT(plugin, "get buf from adaptor meet error");
                    }
                }
                gst_adapter_clear(plugin->adapter);
            }
            break;
        }
        default:
            break;
    }

    if (GST_BASE_TRANSFORM_CLASS(parent_class)->sink_event)
        ret = GST_BASE_TRANSFORM_CLASS(parent_class)->sink_event(trans, event);
    else
        gst_event_unref (event);

    return ret;
}

void
gst_dummydrm_set_property(GObject * object, guint prop_id, const GValue * value, GParamSpec * pspec)
{
    GstDummyDrm *plugin = GST_DUMMYDRM(object);
    switch (prop_id)
    {
    case PROP_IS_4K:
    {
        plugin->is_4k = g_value_get_boolean(value);
        GST_DEBUG_OBJECT(plugin, "set PROP_IS_4K:%d", plugin->is_4k);
        break;
    }
    case PROP_STREAM_MODE:
    {
        plugin->stream_mode = g_value_get_boolean(value);
        GST_DEBUG_OBJECT(plugin, "set PROP_STREAM_MODE:%d", plugin->stream_mode);
        break;
    }
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

void
gst_dummydrm_get_property(GObject * object, guint prop_id, GValue * value, GParamSpec * pspec)
{
    GstDummyDrm *plugin = GST_DUMMYDRM(object);
    switch (prop_id)
    {
    case PROP_IS_4K:
    {
        g_value_set_boolean(value, plugin->is_4k);
        GST_DEBUG_OBJECT(plugin, "get PROP_IS_4K:%d", plugin->is_4k);
        break;
    }
    case PROP_STREAM_MODE:
    {
        g_value_set_boolean(value, plugin->stream_mode);
        GST_DEBUG_OBJECT(plugin, "get PROP_STREAM_MODE:%d", plugin->stream_mode);
        break;
    }
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

gboolean
gst_dummydrm_start(GstBaseTransform *trans)
{
    GstDummyDrm *plugin = GST_DUMMYDRM(trans);


    return TRUE;
}
gboolean
gst_dummydrm_stop(GstBaseTransform *trans)
{
    GstDummyDrm *plugin = GST_DUMMYDRM(trans);

    if (plugin->allocator) {
        gst_object_unref(plugin->allocator);
    }
    if (plugin->outcaps) {
        gst_caps_unref(plugin->outcaps);
    }
    if (plugin->adapter) {
        gst_adapter_clear(plugin->adapter);
        gst_object_unref(plugin->adapter);
        plugin->adapter = NULL;
    }
    return TRUE;
}

GstCaps*
gst_dummydrm_transform_caps(GstBaseTransform *trans, GstPadDirection direction,
        GstCaps *caps, GstCaps *filter)
{
    GstDummyDrm *plugin = GST_DUMMYDRM(trans);
    GstCaps *srccaps, *sinkcaps;
    GstCaps *ret = NULL;

    GST_DEBUG_OBJECT (plugin, "transform_caps direction:%d caps:%" GST_PTR_FORMAT, direction, caps);

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
                if (g_str_has_prefix (gst_structure_get_name (structure), "video/")) {
                    find = true;
                    gst_caps_set_features(ret, i,
                            gst_caps_features_from_string (GST_CAPS_FEATURE_MEMORY_SECMEM_MEMORY));
                    if (g_str_has_suffix (gst_structure_get_name (structure), "/mpegts")) {
                        plugin->stream_mode = TRUE;
                        plugin->need_alignment = TRUE;
                        GST_DEBUG_OBJECT (plugin, "source suffix is mpegts, config streammode is true, config need_alignment to true.");
                    }
                }
            }
            if (find) {
                if (!plugin->allocator) {
                    uint32_t flag = (plugin->is_4k ? 2 : 1);
                    if (plugin->stream_mode)
                        flag |= (1<<8);
                    plugin->allocator = gst_secmem_allocator_new_ex(false, flag);
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
        if (gst_caps_can_intersect(caps, srccaps)) {
            ret = gst_caps_copy(caps);
            unsigned size = gst_caps_get_size(ret);
            for (unsigned i = 0; i < size; ++i) {
                GstCapsFeatures * feature = gst_caps_get_features(ret, i);
                gst_caps_features_remove(feature, GST_CAPS_FEATURE_MEMORY_SECMEM_MEMORY);
            }
        }  else {
            ret = gst_caps_copy(srccaps);
        }
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

    GST_DEBUG_OBJECT (plugin, "transform_caps result:%" GST_PTR_FORMAT, ret);

    gst_caps_unref(srccaps);
    gst_caps_unref(sinkcaps);
    return ret;
}

GstFlowReturn
gst_dummydrm_prepare_output_buffer(GstBaseTransform * trans, GstBuffer *inbuf, GstBuffer **outbuf)
{
    GstDummyDrm *plugin = GST_DUMMYDRM(trans);
    GstFlowReturn ret = GST_FLOW_OK;

    g_return_val_if_fail(plugin->allocator != NULL, GST_FLOW_ERROR);

    *outbuf = gst_buffer_new_allocate(plugin->allocator, gst_buffer_get_size(inbuf), NULL);

    g_return_val_if_fail(*outbuf != NULL, GST_FLOW_ERROR);
    GST_BASE_TRANSFORM_CLASS(parent_class)->copy_metadata (trans, inbuf, *outbuf);
    return ret;
}

static GstFlowReturn
gst_dummydrm_transform(GstBaseTransform *trans, GstBuffer *inbuf, GstBuffer *outbuf)
{
    GstFlowReturn ret = GST_FLOW_ERROR;
    GstMapInfo map;
    GstMemory *mem;
    gsize size;

    mem = gst_buffer_peek_memory(outbuf, 0);
    g_return_val_if_fail(gst_is_secmem_memory(mem), ret);

    gst_memory_get_sizes(mem, NULL, &size);
    gst_buffer_map(inbuf, &map, GST_MAP_READ);
    if (map.size > size) {
        GST_ERROR("buffer size not match");
        goto beach;
    }
    gst_secmem_fill(mem, 0, map.data, map.size);
    ret = GST_FLOW_OK;
beach:
    gst_buffer_unmap(inbuf, &map);
    return ret;
}

#ifndef PACKAGE
#define PACKAGE "gst-aml-drm-plugins"
#endif

static gboolean
dummydrm_init (GstPlugin * dummydrm)
{
    GST_DEBUG_CATEGORY_INIT(gst_dummydrm_debug, "dummydrm", 0, "Amlogic Dummy DRM Plugin");

    return gst_element_register(dummydrm, "dummydrm", GST_RANK_PRIMARY, GST_TYPE_DUMMYDRM);
}


GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    dummydrm,
    "Gstreamer Dummy Drm plugin",
    dummydrm_init,
    PACKAGE_VERSION,
    GST_LICENSE,
    GST_PACKAGE_NAME,
    GST_PACKAGE_ORIGIN
)
