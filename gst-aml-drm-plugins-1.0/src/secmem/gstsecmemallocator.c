/*
 * gstsecmemallocator.c
 *
 *  Created on: Feb 8, 2020
 *      Author: tao
 */

#include <sys/mman.h>
#include <errno.h>
#include <unistd.h>

#include "gstsecmemallocator.h"
#include "secmem_ca.h"

GST_DEBUG_CATEGORY_STATIC (gst_secmem_allocator_debug);
#define GST_CAT_DEFAULT gst_secmem_allocator_debug

#define gst_secmem_allocator_parent_class parent_class
G_DEFINE_TYPE (GstSecmemAllocator, gst_secmem_allocator, GST_TYPE_DMABUF_ALLOCATOR);

static void         gst_secmem_allocator_finalize(GObject *object);
static GstMemory*   gst_secmem_mem_alloc (GstAllocator * allocator, gsize size, GstAllocationParams * params);
static void         gst_secmem_mem_free (GstAllocator *allocator, GstMemory *gmem);
static gpointer     gst_secmem_mem_map (GstMemory * gmem, gsize maxsize, GstMapFlags flags);
static void         gst_secmem_mem_unmap (GstMemory * gmem);
static GstMemory *  gst_secmem_mem_copy(GstMemory *mem, gssize offset, gssize size);
static GstMemory*   gst_secmem_mem_share (GstMemory * gmem, gssize offset, gssize size);

static void
gst_secmem_allocator_class_init (GstSecmemAllocatorClass * klass)
{
    GObjectClass *gobject_class = (GObjectClass *) klass;
    GstAllocatorClass *alloc_class = (GstAllocatorClass *) klass;

    gobject_class->finalize = GST_DEBUG_FUNCPTR (gst_secmem_allocator_finalize);
    alloc_class->alloc = GST_DEBUG_FUNCPTR (gst_secmem_mem_alloc);
    alloc_class->free = GST_DEBUG_FUNCPTR (gst_secmem_mem_free);

    GST_DEBUG_CATEGORY_INIT(gst_secmem_allocator_debug, "secmem", 0, "SECMEM Allocator");
}


static void
gst_secmem_allocator_init (GstSecmemAllocator * self)
{
    GstAllocator *allocator = GST_ALLOCATOR_CAST(self);
    g_mutex_init (&self->mutex);
    g_cond_init (&self->cond);
    allocator->mem_type = GST_ALLOCATOR_SECMEM;
    allocator->mem_map = gst_secmem_mem_map;
    allocator->mem_unmap = gst_secmem_mem_unmap;
    allocator->mem_copy = gst_secmem_mem_copy;
    allocator->mem_share = gst_secmem_mem_share;

    GST_OBJECT_FLAG_SET (self, GST_ALLOCATOR_FLAG_CUSTOM_ALLOC);
}

void gst_secmem_allocator_finalize(GObject *object)
{
    GstSecmemAllocator *self = GST_SECMEM_ALLOCATOR(object);
    if (self->sess) {
        Secure_V2_SessionDestroy(&self->sess);
    }
    g_mutex_clear (&self->mutex);
    g_cond_clear (&self->cond);
    G_OBJECT_CLASS(parent_class)->finalize(object);
}

GstMemory *
gst_secmem_mem_alloc (GstAllocator * allocator, gsize size,
    GstAllocationParams * params)
{
    GstSecmemAllocator *self = GST_SECMEM_ALLOCATOR (allocator);
    GstMemory *mem = NULL;
    int fd;
    secmem_handle_t handle;
    unsigned int ret;
    uint32_t maxsize = 0;
    uint32_t mem_available = 0;
    uint32_t handle_available = 0;

    g_return_val_if_fail (GST_IS_SECMEM_ALLOCATOR (allocator), NULL);
    g_return_val_if_fail(self->sess != NULL, NULL);

    g_mutex_lock (&self->mutex);

    do {
        if (Secure_V2_GetSecmemSize(self->sess, NULL, &mem_available, NULL, &handle_available))
            goto error;
        if (handle_available > 0 && size < mem_available)
            break;
        g_cond_wait (&self->cond, &self->mutex);
    } while (1);

    ret = Secure_V2_MemAllocDMA(self->sess, size, &fd, &handle, &maxsize);
    if (ret) {
        GST_ERROR("MemAllocDMA failed");
        goto error;
    }
    mem = gst_fd_allocator_alloc(allocator, fd, maxsize, GST_FD_MEMORY_FLAG_NONE);
    if (!mem) {
        GST_ERROR("gst_fd_allocator_alloc failed");
        goto error;
    }
    mem->size = size;
    GST_INFO("alloc dma %d maxsize %d", fd, maxsize);
    g_mutex_unlock (&self->mutex);
    return mem;

error:
    g_mutex_unlock (&self->mutex);
    return NULL;
}

gint gst_secmem_check_free_buf_size(GstAllocator * allocator)
{
    uint32_t available = 0;
    g_return_val_if_fail(GST_IS_SECMEM_ALLOCATOR (allocator), -1);
    GstSecmemAllocator *self = GST_SECMEM_ALLOCATOR (allocator);
    g_return_val_if_fail(self->sess != NULL, -1);
    g_mutex_lock (&self->mutex);
    if (Secure_V2_GetSecmemSize(self->sess, NULL, &available, NULL, NULL))
        available = -1;
    g_mutex_unlock (&self->mutex);
    return available;
}


void
gst_secmem_mem_free(GstAllocator *allocator, GstMemory *memory)
{
    gsize maxsize = 0, size;
    GstSecmemAllocator *self = GST_SECMEM_ALLOCATOR (allocator);
    g_mutex_lock (&self->mutex);
    int fd = gst_fd_memory_get_fd(memory);
    secmem_handle_t handle = Secure_V2_FdToHandle(self->sess, fd);
    if (handle == -1)
        GST_ERROR("handle or fd invalid");
    size = gst_memory_get_sizes (memory, NULL, &maxsize);

    GST_INFO("free dma %d size %d max size %d", fd, size, maxsize);
    GST_ALLOCATOR_CLASS (parent_class)->free(allocator, memory);
    Secure_V2_MemRelease(self->sess, handle); //Auto free when release
    g_cond_broadcast (&self->cond);
    g_mutex_unlock (&self->mutex);
}

void
gst_secmem_free_handle(GstAllocator *allocator, secmem_handle_t handle)
{
    GstSecmemAllocator *self = GST_SECMEM_ALLOCATOR (allocator);
    g_mutex_lock (&self->mutex);

    Secure_V2_MemFree(self->sess, handle);
    Secure_V2_MemRelease(self->sess, handle);
    g_cond_broadcast (&self->cond);
    g_mutex_unlock (&self->mutex);
}


gpointer
gst_secmem_mem_map (GstMemory * gmem, gsize maxsize, GstMapFlags flags)
{
    GST_ERROR("Not support map");
    return NULL;
}

void
gst_secmem_mem_unmap (GstMemory * gmem)
{
    GST_ERROR("Not support map");
}

GstMemory *
gst_secmem_mem_copy (GstMemory *mem, gssize offset, gssize size)
{
    GST_ERROR("Not support copy");
    return NULL;
}

GstMemory *
gst_secmem_mem_share (GstMemory * gmem, gssize offset, gssize size)
{
    GST_ERROR("Not support share");
    return NULL;
}

GstAllocator *
gst_secmem_allocator_new_ex(uint8_t decoder_format, uint32_t reserved)
{
    unsigned int ret;
    uint32_t flag;
    GstAllocator *alloc;
    uint32_t capacity = 0;
    uint32_t setsize = 0;

    alloc = g_object_new(GST_TYPE_SECMEM_ALLOCATOR, NULL);
    gst_object_ref_sink(alloc);

    GstSecmemAllocator *self = GST_SECMEM_ALLOCATOR (alloc);
    self->is_vp9 = decoder_format == SECMEM_DECODER_VP9 ? TRUE: FALSE;
    self->is_av1 = decoder_format == SECMEM_DECODER_AV1 ? TRUE : FALSE;

    ret = Secure_V2_SessionCreate(&self->sess);
    g_return_val_if_fail(ret == 0, alloc);

    capacity = Secure_GetSecmemSize();
    g_return_val_if_fail(capacity > 0, alloc);

    //custom set sec size
    flag = 3;
    if (self->is_vp9) {
        flag |= 0x09 << 4;
    } else if (self->is_av1) {
        flag |= 0x0A << 4;
    }
    setsize = CEIL_POS(((uint32_t)(capacity >> 20) + 1) * 0.75);
    if(setsize > 12)
        setsize = 12;
    flag |= reserved;
    ret = Secure_V2_Init(self->sess, 1, flag, 0, setsize);
    g_return_val_if_fail(ret == 0, alloc);
    GST_INFO("secmem init return %d, flag 0x%x", ret, flag);

    return alloc;
}

GstAllocator *
gst_secmem_allocator_new (gboolean is_4k, uint8_t decoder_format)
{
    unsigned int ret;
    uint32_t flag;
    GstAllocator *alloc;

    //is_4k = TRUE; //Force 4K
    alloc = g_object_new(GST_TYPE_SECMEM_ALLOCATOR, NULL);
    gst_object_ref_sink(alloc);

    GstSecmemAllocator *self = GST_SECMEM_ALLOCATOR (alloc);
    self->is_4k = is_4k;
    self->is_vp9 = decoder_format == SECMEM_DECODER_VP9 ? TRUE: FALSE;
    self->is_av1 = decoder_format == SECMEM_DECODER_AV1 ? TRUE : FALSE;

    ret = Secure_V2_SessionCreate(&self->sess);
    g_return_val_if_fail(ret == 0, alloc);
    flag = is_4k ? SECMEM_V2_FLAGS_TVP(SECMEM_TVP_TYPE_UHD) : SECMEM_V2_FLAGS_TVP(SECMEM_TVP_TYPE_FHD);
    if (self->is_vp9) {
        flag |= SECMEM_V2_FLAGS_CODEC(SECMEM_CODEC_VP9);
    } else if (self->is_av1) {
        flag |= SECMEM_V2_FLAGS_CODEC(SECMEM_CODEC_AV1);
    }
    if (decoder_format == SECMEM_DECODER_MPEGTS) {
        flag |= 1 << 8;
    }

    ret = Secure_V2_Init(self->sess, 1, flag, 0, 0);
    g_return_val_if_fail(ret == 0, alloc);
    GST_INFO("secmem init return %d, flag %x", ret, flag);

    return alloc;
}

gboolean
gst_is_secmem_memory (GstMemory *mem)
{
    return gst_memory_is_type(mem, GST_ALLOCATOR_SECMEM);
}

gboolean
gst_secmem_fill(GstMemory *mem, uint32_t offset, uint8_t *buffer, uint32_t length)
{
    uint32_t ret;
    uint32_t handle;
    handle = gst_secmem_memory_get_handle(mem);
    g_return_val_if_fail(handle != 0, FALSE);

    GstSecmemAllocator *self = GST_SECMEM_ALLOCATOR (mem->allocator);
    ret = Secure_V2_MemFill(self->sess, handle, offset, buffer, length);
    g_return_val_if_fail(ret == 0, FALSE);
    GST_INFO("secmem fill return %d", ret);
    return TRUE;
}

gboolean
gst_secmem_fillbypaddr(GstMemory *mem, uint32_t src_paddr)
{
    uint32_t ret;
    uint32_t handle;
    handle = gst_secmem_memory_get_handle(mem);
    g_return_val_if_fail(handle != 0, FALSE);
    GST_INFO("mem->size %d", mem->size);
    GstSecmemAllocator *self = GST_SECMEM_ALLOCATOR (mem->allocator);
    ret = Secure_V2_MemFillByPaddr(self->sess, handle, src_paddr, mem->size);
    g_return_val_if_fail(ret == 0, FALSE);
    return TRUE;
}

gboolean
gst_secmem_copybyhandle(GstMemory *mem, uint32_t src_handle, uint32_t range, uint32_t dst_offset[], uint32_t src_offset[], uint32_t size[])
{
    uint32_t ret;
    uint32_t handle;
    handle = gst_secmem_memory_get_handle(mem);
    g_return_val_if_fail(handle != 0, FALSE);
    //GST_INFO("mem->size %d, size %d",  mem->size, size);
    GstSecmemAllocator *self = GST_SECMEM_ALLOCATOR (mem->allocator);
    ret = Secure_V2_MemCopy(self->sess, handle, src_handle, range, dst_offset, src_offset, size);
    g_return_val_if_fail(ret == 0, FALSE);
    return TRUE;
}

gboolean
gst_secmem_check_free_buf_and_handles_size(GstAllocator * allocator, guint *availableSize, guint *handle_available)
{
    g_return_val_if_fail(GST_IS_SECMEM_ALLOCATOR (allocator), FALSE);
    GstSecmemAllocator *self = GST_SECMEM_ALLOCATOR (allocator);
    g_return_val_if_fail(self->sess != NULL, FALSE);
    g_mutex_lock (&self->mutex);
    uint32_t ret = Secure_V2_GetSecmemSize(self->sess, NULL, availableSize, NULL, handle_available);
    g_mutex_unlock (&self->mutex);
    g_return_val_if_fail(ret == 0, FALSE);
    return TRUE;
}

gboolean
gst_secmem_store_csd(GstMemory *mem, uint8_t *buffer, uint32_t length)
{
    uint32_t ret;
    g_return_val_if_fail(mem != NULL, FALSE);
    g_return_val_if_fail(GST_IS_SECMEM_ALLOCATOR (mem->allocator), FALSE);
    GstSecmemAllocator *self = GST_SECMEM_ALLOCATOR (mem->allocator);

    ret = Secure_V2_SetCsdData(self->sess, buffer, length);
    g_return_val_if_fail(ret == 0, FALSE);
    return TRUE;
}

gboolean
gst_secmem_prepend_csd(GstMemory *mem)
{
    uint32_t ret;
    uint32_t handle;
    uint32_t csdlen = 0;
    gsize memsize, offset, maxsize;
    handle = gst_secmem_memory_get_handle(mem);
    g_return_val_if_fail(handle != 0, FALSE);

    GstSecmemAllocator *self = GST_SECMEM_ALLOCATOR (mem->allocator);
    memsize = gst_memory_get_sizes(mem, &offset, &maxsize);
    ret = Secure_V2_MergeCsdData(self->sess, handle, &csdlen);
    g_return_val_if_fail(ret == 0, FALSE);
    g_return_val_if_fail(csdlen > 0, FALSE);
    g_return_val_if_fail(memsize + csdlen < maxsize, FALSE);
    mem->size += csdlen;
    return TRUE;
}

gboolean
gst_secmem_parse_avcc(GstMemory *mem, uint8_t *buffer, uint32_t length)
{
    uint32_t ret;
    g_return_val_if_fail(mem != NULL, -1);
    g_return_val_if_fail(GST_IS_SECMEM_ALLOCATOR (mem->allocator), -1);
    GstSecmemAllocator *self = GST_SECMEM_ALLOCATOR (mem->allocator);
    ret = Secure_V2_Parse(self->sess, STREAM_TYPE_AVCC, 0, buffer, length, NULL);
    g_return_val_if_fail(ret == 0, FALSE);

    return TRUE;
}

gboolean
gst_secmem_parse_avc2nalu(GstMemory *mem, uint32_t *flag)
{
    uint32_t ret;
    uint32_t handle;

    handle = gst_secmem_memory_get_handle(mem);
    g_return_val_if_fail(handle != 0, FALSE);

    GstSecmemAllocator *self = GST_SECMEM_ALLOCATOR (mem->allocator);
    ret = Secure_V2_Parse(self->sess, STREAM_TYPE_AVC2NALU, handle, NULL, 0, flag);
    g_return_val_if_fail(ret == 0, FALSE);
    return TRUE;
}

gboolean
gst_secmem_parse_hvcc(GstMemory *mem, uint8_t *buffer, uint32_t length)
{
    uint32_t ret;
    g_return_val_if_fail(mem != NULL, -1);
    g_return_val_if_fail(GST_IS_SECMEM_ALLOCATOR (mem->allocator), -1);
    GstSecmemAllocator *self = GST_SECMEM_ALLOCATOR (mem->allocator);
    ret = Secure_V2_Parse(self->sess, STREAM_TYPE_HVCC, 0, buffer, length, NULL);
    g_return_val_if_fail(ret == 0, FALSE);

    return TRUE;
}

gboolean
gst_secmem_parse_hvc2nalu(GstMemory *mem, uint32_t *flag)
{
    uint32_t ret;
    uint32_t handle;

    handle = gst_secmem_memory_get_handle(mem);
    g_return_val_if_fail(handle != 0, FALSE);

    GstSecmemAllocator *self = GST_SECMEM_ALLOCATOR (mem->allocator);
    ret = Secure_V2_Parse(self->sess, STREAM_TYPE_HVC2NALU, handle, NULL, 0, flag);
    g_return_val_if_fail(ret == 0, FALSE);
    return TRUE;
}

gboolean
gst_secmem_parse_vp9(GstMemory *mem)
{
    uint32_t ret;
    uint32_t handle;
    uint32_t header_size;

    handle = gst_secmem_memory_get_handle(mem);
    g_return_val_if_fail(handle != 0, FALSE);

    GstSecmemAllocator *self = GST_SECMEM_ALLOCATOR (mem->allocator);
    ret = Secure_V2_Parse(self->sess, STREAM_TYPE_VP9, handle, NULL, 0, &header_size);
    g_return_val_if_fail(ret == 0, FALSE);
    mem->size += header_size;
    return TRUE;
}

gboolean
gst_secmem_parse_av1(GstMemory *mem)
{
    uint32_t ret;
    uint32_t handle;
    uint32_t header_size;

    handle = gst_secmem_memory_get_handle(mem);
    g_return_val_if_fail(handle != 0, FALSE);

    GstSecmemAllocator *self = GST_SECMEM_ALLOCATOR (mem->allocator);
    ret = Secure_V2_Parse(self->sess, STREAM_TYPE_AV1, handle, NULL, 0, &header_size);
    g_return_val_if_fail(ret == 0, FALSE);
    mem->size += header_size;
    return TRUE;
}

secmem_handle_t gst_secmem_memory_get_handle (GstMemory *mem)
{
    g_return_val_if_fail(mem != NULL, 0);
    g_return_val_if_fail(GST_IS_SECMEM_ALLOCATOR (mem->allocator), 0);

    GstSecmemAllocator *self = GST_SECMEM_ALLOCATOR (mem->allocator);
    int fd = gst_fd_memory_get_fd(mem);
    secmem_handle_t handle = Secure_V2_FdToHandle(self->sess, fd);
    return handle;
}

secmem_paddr_t gst_secmem_memory_get_paddr (GstMemory *mem)
{
    g_return_val_if_fail(mem != NULL, -1);
    g_return_val_if_fail(GST_IS_SECMEM_ALLOCATOR (mem->allocator), -1);

    GstSecmemAllocator *self = GST_SECMEM_ALLOCATOR (mem->allocator);
    int fd = gst_fd_memory_get_fd(mem);
    secmem_paddr_t paddr = Secure_V2_FdToPaddr(self->sess, fd);
    return paddr;
}

gint gst_secmem_get_free_buf_num(GstMemory *mem)
{
    unsigned int handle_available = 0;
    g_return_val_if_fail(mem != NULL, -1);
    g_return_val_if_fail(GST_IS_SECMEM_ALLOCATOR (mem->allocator), -1);

    GstSecmemAllocator *self = GST_SECMEM_ALLOCATOR (mem->allocator);
    g_mutex_lock (&self->mutex);
    Secure_V2_GetSecmemSize(self->sess, NULL, NULL, NULL, &handle_available);
    g_mutex_unlock (&self->mutex);
    return (gint)handle_available;

}

gint gst_secmem_get_free_buf_size(GstMemory *mem)
{
    uint32_t available = 0;

    g_return_val_if_fail(mem != NULL, -1);
    g_return_val_if_fail(GST_IS_SECMEM_ALLOCATOR (mem->allocator), -1);

    GstSecmemAllocator *self = GST_SECMEM_ALLOCATOR (mem->allocator);
    g_mutex_lock (&self->mutex);
    if (Secure_V2_GetSecmemSize(self->sess, NULL, &available, NULL, NULL))
        available = -1;
    g_mutex_unlock (&self->mutex);
    return available;
}

secmem_handle_t gst_buffer_get_secmem_handle(GstBuffer *buffer)
{
    GstMemory *mem = gst_buffer_peek_memory(buffer, 0);
    g_return_val_if_fail(gst_is_secmem_memory(mem), 0);

    secmem_handle_t handle = gst_secmem_memory_get_handle(mem);
    return handle;
}

secmem_paddr_t gst_buffer_get_secmem_paddr(GstBuffer *buffer)
{
    GstMemory *mem = gst_buffer_peek_memory(buffer, 0);
    g_return_val_if_fail(gst_is_secmem_memory(mem), 0);

    secmem_handle_t handle = gst_secmem_memory_get_paddr(mem);
    return handle;
}

gboolean gst_buffer_copy_to_secmem(GstBuffer *dst, GstBuffer *src)
{
    gboolean ret;
    GstMapInfo map;
    GstMemory *mem;

    g_return_val_if_fail(dst != NULL, FALSE);
    mem = gst_buffer_peek_memory(dst, 0);
    g_return_val_if_fail(gst_is_secmem_memory(mem), FALSE);
    g_return_val_if_fail(gst_buffer_map(src, &map, GST_MAP_READ), FALSE);
    ret = gst_secmem_fill(mem, 0, map.data, map.size);
    gst_buffer_unmap(src, &map);
    return ret;
}

gboolean gst_buffer_sideband_secmem(GstBuffer *dst)
{
    unsigned int ret;
    GstMemory *mem;
    uint32_t handle;
    mem = gst_buffer_peek_memory(dst, 0);
    g_return_val_if_fail(gst_is_secmem_memory(mem), FALSE);
    handle = gst_secmem_memory_get_handle(mem);
    g_return_val_if_fail(handle != 0, FALSE);
    ret = Secure_SetHandle(handle);
    g_return_val_if_fail(ret == 0, FALSE);
    return TRUE;
}
