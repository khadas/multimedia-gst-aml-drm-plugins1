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

//TODO:
//Implement a real dma buffer in future.
#define SECMEM_DMA 1

GST_DEBUG_CATEGORY_STATIC (gst_secmem_allocator_debug);
#define GST_CAT_DEFAULT gst_secmem_allocator_debug

typedef struct
{
    GstMemory mem;
    secmem_handle_t handle;
    secmem_paddr_t phyaddr;
} GstSecmemMemory;


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
    unsigned int ret;

    allocator->mem_type = GST_ALLOCATOR_SECMEM;
    allocator->mem_map = gst_secmem_mem_map;
    allocator->mem_unmap = gst_secmem_mem_unmap;
    allocator->mem_copy = gst_secmem_mem_copy;
    allocator->mem_share = gst_secmem_mem_share;

    GST_OBJECT_FLAG_SET (self, GST_ALLOCATOR_FLAG_CUSTOM_ALLOC);

    ret = Secure_V2_SessionCreate(&self->sess);
    g_return_if_fail(ret == 0);
    ret = Secure_V2_Init(self->sess, 1, 1, 0, 0);
    g_return_if_fail(ret == 0);
    GST_INFO("init success");
}

void gst_secmem_allocator_finalize(GObject *object)
{
    GstSecmemAllocator *self = GST_SECMEM_ALLOCATOR(object);
    if (self->sess) {
        Secure_V2_SessionDestroy(&self->sess);
    }
}

#if SECMEM_DMA
GstMemory *
gst_secmem_mem_alloc (GstAllocator * allocator, gsize size,
    GstAllocationParams * params)
{
    GstSecmemAllocator *self = GST_SECMEM_ALLOCATOR (allocator);
    GstMemory *mem = NULL;
    int fd;
    secmem_handle_t handle;
    unsigned int ret;

    g_return_val_if_fail (GST_IS_SECMEM_ALLOCATOR (allocator), NULL);
    g_return_val_if_fail(self->sess != NULL, NULL);

    ret = Secure_V2_MemCreate(self->sess, &handle);
    if (ret) {
        GST_ERROR("MemCreate failed");
        goto error_create;
    }

    ret = Secure_V2_MemAlloc(self->sess, handle, size, NULL);
    if (ret) {
        GST_ERROR("MemAlloc failed");
        goto error_alloc;
    }

    ret = Secure_V2_MemExport(self->sess, handle, &fd);
    if (ret) {
        GST_ERROR("MemExport failed");
        goto error_export;
    }

    mem = gst_fd_allocator_alloc(allocator, fd, size, GST_FD_MEMORY_FLAG_NONE);
    if (!mem) {
        GST_ERROR("gst_fd_allocator_alloc failed");
        goto error_export;
    }
    GST_INFO("alloc dma %d", fd);
    return mem;

error_export:
    Secure_V2_MemFree(self->sess, handle);
error_alloc:
    Secure_V2_MemRelease(self->sess, handle);
error_create:
    return NULL;
}
#else
GstMemory *
gst_secmem_mem_alloc (GstAllocator * allocator, gsize size,
    GstAllocationParams * params)
{
    GstSecmemAllocator *self = GST_SECMEM_ALLOCATOR (allocator);
    GstSecmemMemory *mem = NULL;
    unsigned int ret;

    g_return_val_if_fail (GST_IS_SECMEM_ALLOCATOR (allocator), NULL);
    g_return_val_if_fail(self->sess != NULL, NULL);

    mem = g_slice_new0 (GstSecmemMemory);
    gst_memory_init (GST_MEMORY_CAST (mem), 0, GST_ALLOCATOR_CAST (allocator),
          NULL, size, 0, 0, size);

    ret = Secure_V2_MemCreate(self->sess, &mem->handle);
    if (ret) {
        GST_ERROR("MemCreate failed");
        goto error_create;
    }
    ret = Secure_V2_MemAlloc(self->sess, mem->handle, size, &mem->phyaddr);
    if (ret) {
        GST_ERROR("MemAlloc failed");
        goto error_alloc;
    }
    GST_INFO("alloc %x", mem->handle);
    return (GstMemory *)mem;

error_alloc:
    Secure_V2_MemRelease(self->sess, mem->handle);
error_create:
    g_slice_free(GstSecmemMemory, mem);
    return NULL;
}
#endif


#if SECMEM_DMA
void
gst_secmem_mem_free(GstAllocator *allocator, GstMemory *memory)
{
    GstSecmemAllocator *self = GST_SECMEM_ALLOCATOR (allocator);
    int fd = gst_fd_memory_get_fd(memory);
    secmem_handle_t handle = Secure_V2_FdToHandle(self->sess, fd);
    GST_INFO("free dma %d", gst_fd_memory_get_fd(memory));
    GST_ALLOCATOR_CLASS (parent_class)->free(allocator, memory);
    Secure_V2_MemFree(self->sess, handle);
    Secure_V2_MemRelease(self->sess, handle);
}
#else
void
gst_secmem_mem_free(GstAllocator *allocator, GstMemory *memory)
{
    GstSecmemAllocator *self = GST_SECMEM_ALLOCATOR (allocator);
    GstSecmemMemory *mem = (GstSecmemMemory *)memory;
    secmem_paddr_t phyaddr;
    unsigned int ret;

    ret = Secure_V2_MemFree(self->sess, mem->handle);
    if (!ret) {
        ret = Secure_V2_MemToPhy(self->sess, mem->handle, &phyaddr);
        if (!ret && phyaddr) {
            GST_ERROR("free failed");
            goto error;
        }
        Secure_V2_MemRelease(self->sess, mem->handle);
    }
    g_slice_free(GstSecmemMemory, mem);
    GST_INFO("freed %x", mem->handle);
error:
    return;
}
#endif


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
gst_secmem_mem_copy(GstMemory *mem, gssize offset, gssize size)
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
gst_secmem_allocator_new (void)
{
    GstAllocator *alloc;

    alloc = g_object_new(GST_TYPE_SECMEM_ALLOCATOR, NULL);
    gst_object_ref_sink(alloc);

    return alloc;
}

gboolean
gst_is_secmem_memory (GstMemory *mem)
{
    g_return_val_if_fail(mem != NULL, FALSE);
    return GST_IS_SECMEM_ALLOCATOR(mem->allocator);
}

gboolean
gst_secmem_fill(GstMemory *mem, uint8_t *buffer, uint32_t offset, uint32_t length)
{
    uint32_t ret;
    uint32_t handle;
    handle = gst_secmem_memory_get_handle(mem);
    g_return_val_if_fail(handle != 0, FALSE);

    GstSecmemAllocator *self = GST_SECMEM_ALLOCATOR (mem->allocator);
    ret = Secure_V2_MemFill(self->sess, handle, buffer, offset, length);
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
    uint32_t csdlen;
    handle = gst_secmem_memory_get_handle(mem);
    g_return_val_if_fail(handle != 0, FALSE);

    GstSecmemAllocator *self = GST_SECMEM_ALLOCATOR (mem->allocator);
    ret = Secure_V2_MergeCsdData(self->sess, handle, &csdlen);
    g_return_val_if_fail(ret == 0, FALSE);
    g_return_val_if_fail(csdlen > 0, FALSE);
    return TRUE;
}

secmem_handle_t gst_secmem_memory_get_handle (GstMemory *mem)
{
    g_return_val_if_fail(mem != NULL, 0);
    g_return_val_if_fail(GST_IS_SECMEM_ALLOCATOR (mem->allocator), 0);
#if SECMEM_DMA
    GstSecmemAllocator *self = GST_SECMEM_ALLOCATOR (mem->allocator);
    int fd = gst_fd_memory_get_fd(mem);
    secmem_handle_t handle = Secure_V2_FdToHandle(self->sess, fd);
    return handle;
#else
    return ((GstSecmemMemory *)mem)->handle;
#endif
}

secmem_paddr_t gst_secmem_memory_get_paddr (GstMemory *mem)
{
    g_return_val_if_fail(mem != NULL, -1);
    g_return_val_if_fail(GST_IS_SECMEM_ALLOCATOR (mem->allocator), -1);
#if SECMEM_DMA
    GstSecmemAllocator *self = GST_SECMEM_ALLOCATOR (mem->allocator);
    int fd = gst_fd_memory_get_fd(mem);
    secmem_paddr_t paddr = Secure_V2_FdToPaddr(self->sess, fd);
    return paddr;
#else
    return ((GstSecmemMemory *)mem)->phyaddr;
#endif
}
