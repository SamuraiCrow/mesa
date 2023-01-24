/*
 * Copyright © 2021 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "vk_accelerant_syncobj.h"

#include <sched.h>
#include <xf86drm.h>
#include <AccelerantDrm.h>

#include "drm-uapi/drm.h"

#include "util/os_time.h"

#include "vk_device.h"
#include "vk_log.h"
#include "vk_util.h"

static struct vk_accelerant_syncobj *
to_accelerant_syncobj(struct vk_sync *sync)
{
   assert(vk_sync_type_is_accelerant_syncobj(sync->type));
   return container_of(sync, struct vk_accelerant_syncobj, base);
}

static VkResult
vk_accelerant_syncobj_init(struct vk_device *device,
                    struct vk_sync *sync,
                    uint64_t initial_value)
{
   struct vk_accelerant_syncobj *sobj = to_accelerant_syncobj(sync);

   uint32_t flags = 0;
   if (!(sync->flags & VK_SYNC_IS_TIMELINE) && initial_value)
      flags |= DRM_SYNCOBJ_CREATE_SIGNALED;

   assert(device->acc_drm != NULL);
   int err = device->acc_drm->vt->DrmSyncobjCreate(device->acc_drm, flags, &sobj->syncobj);
   if (err < 0) {
      return vk_errorf(device, VK_ERROR_OUT_OF_HOST_MEMORY,
                       "DrmSyncobjCreate failed: %m");
   }

   if ((sync->flags & VK_SYNC_IS_TIMELINE) && initial_value) {
      err = device->acc_drm->vt->DrmSyncobjTimelineSignal(device->acc_drm, &sobj->syncobj,
                                     &initial_value, 1);
      if (err < 0) {
         vk_accelerant_syncobj_finish(device, sync);
         return vk_errorf(device, VK_ERROR_OUT_OF_HOST_MEMORY,
                          "DrmSyncobjTimelineSignal failed: %m");
      }
   }

   return VK_SUCCESS;
}

void
vk_accelerant_syncobj_finish(struct vk_device *device,
                      struct vk_sync *sync)
{
   struct vk_accelerant_syncobj *sobj = to_accelerant_syncobj(sync);

   assert(device->acc_drm != NULL);
   ASSERTED int err = device->acc_drm->vt->DrmSyncobjDestroy(device->acc_drm, sobj->syncobj);
   assert(err == 0);
}

static VkResult
vk_accelerant_syncobj_signal(struct vk_device *device,
                      struct vk_sync *sync,
                      uint64_t value)
{
   struct vk_accelerant_syncobj *sobj = to_accelerant_syncobj(sync);

   assert(device->acc_drm != NULL);
   int err;
   if (sync->flags & VK_SYNC_IS_TIMELINE)
      err = device->acc_drm->vt->DrmSyncobjTimelineSignal(device->acc_drm, &sobj->syncobj, &value, 1);
   else
      err = device->acc_drm->vt->DrmSyncobjSignal(device->acc_drm, &sobj->syncobj, 1);
   if (err) {
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "DrmSyncobjSignal failed: %m");
   }

   return VK_SUCCESS;
}

static VkResult
vk_accelerant_syncobj_get_value(struct vk_device *device,
                         struct vk_sync *sync,
                         uint64_t *value)
{
   struct vk_accelerant_syncobj *sobj = to_accelerant_syncobj(sync);

   assert(device->acc_drm != NULL);
   int err = device->acc_drm->vt->DrmSyncobjQuery(device->acc_drm, &sobj->syncobj, value, 1, 0);
   if (err) {
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "DrmSyncobjQuery failed: %m");
   }

   return VK_SUCCESS;
}

static VkResult
vk_accelerant_syncobj_reset(struct vk_device *device,
                     struct vk_sync *sync)
{
   struct vk_accelerant_syncobj *sobj = to_accelerant_syncobj(sync);

   assert(device->acc_drm != NULL);
   int err = device->acc_drm->vt->DrmSyncobjReset(device->acc_drm, &sobj->syncobj, 1);
   if (err) {
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "DrmSyncobjReset failed: %m");
   }

   return VK_SUCCESS;
}

static VkResult
vk_accelerant_syncobj_wait_many(struct vk_device *device,
                         uint32_t wait_count,
                         const struct vk_sync_wait *waits,
                         enum vk_sync_wait_flags wait_flags,
                         uint64_t abs_timeout_ns)
{
   /* Syncobj timeouts are signed */
   abs_timeout_ns = MIN2(abs_timeout_ns, (uint64_t)INT64_MAX);

   STACK_ARRAY(uint32_t, handles, wait_count);
   STACK_ARRAY(uint64_t, wait_values, wait_count);

   uint32_t j = 0;
   bool has_timeline = false;
   for (uint32_t i = 0; i < wait_count; i++) {
      /* The syncobj API doesn't like wait values of 0 but it's safe to skip
       * them because a wait for 0 is a no-op.
       */
      if (waits[i].sync->flags & VK_SYNC_IS_TIMELINE) {
         if (waits[i].wait_value == 0)
            continue;

         has_timeline = true;
      }

      handles[j] = to_accelerant_syncobj(waits[i].sync)->syncobj;
      wait_values[j] = waits[i].wait_value;
      j++;
   }
   assert(j <= wait_count);
   wait_count = j;

   uint32_t syncobj_wait_flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT;
   if (!(wait_flags & VK_SYNC_WAIT_ANY))
      syncobj_wait_flags |= DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL;

   assert(device->acc_drm != NULL);
   int err;
   if (wait_count == 0) {
      err = 0;
   } else if (wait_flags & VK_SYNC_WAIT_PENDING) {
      /* We always use a timeline wait for WAIT_PENDING, even for binary
       * syncobjs because the non-timeline wait doesn't support
       * DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE.
       */
      err = device->acc_drm->vt->DrmSyncobjTimelineWait(device->acc_drm, handles, wait_values,
                                   wait_count, abs_timeout_ns,
                                   syncobj_wait_flags |
                                   DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE,
                                   NULL /* first_signaled */);
   } else if (has_timeline) {
      err = device->acc_drm->vt->DrmSyncobjTimelineWait(device->acc_drm, handles, wait_values,
                                   wait_count, abs_timeout_ns,
                                   syncobj_wait_flags,
                                   NULL /* first_signaled */);
   } else {
      err = device->acc_drm->vt->DrmSyncobjWait(device->acc_drm, handles,
                           wait_count, abs_timeout_ns,
                           syncobj_wait_flags,
                           NULL /* first_signaled */);
   }

   STACK_ARRAY_FINISH(handles);
   STACK_ARRAY_FINISH(wait_values);

   if (err && errno == ETIME) {
      return VK_TIMEOUT;
   } else if (err) {
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "DrmSyncobjWait failed: %m");
   }

   return VK_SUCCESS;
}

static VkResult
vk_accelerant_syncobj_import_opaque_fd(struct vk_device *device,
                                struct vk_sync *sync,
                                int fd)
{
   struct vk_accelerant_syncobj *sobj = to_accelerant_syncobj(sync);

   assert(device->acc_drm != NULL);
   uint32_t new_handle;
   int err = device->acc_drm->vt->DrmSyncobjImportFd(device->acc_drm, fd, &new_handle);
   if (err) {
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "DrmSyncobjImportFd failed: %m");
   }

   err = device->acc_drm->vt->DrmSyncobjDestroy(device->acc_drm, sobj->syncobj);
   assert(!err);

   sobj->syncobj = new_handle;

   return VK_SUCCESS;
}

static VkResult
vk_accelerant_syncobj_export_opaque_fd(struct vk_device *device,
                                struct vk_sync *sync,
                                int *fd)
{
   struct vk_accelerant_syncobj *sobj = to_accelerant_syncobj(sync);

   assert(device->acc_drm != NULL);
   int err = device->acc_drm->vt->DrmSyncobjExportFd(device->acc_drm, sobj->syncobj, fd);
   if (err) {
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "DrmSyncobjExportFd failed: %m");
   }

   return VK_SUCCESS;
}

static VkResult
vk_accelerant_syncobj_import_sync_file(struct vk_device *device,
                                struct vk_sync *sync,
                                int sync_file)
{
   struct vk_accelerant_syncobj *sobj = to_accelerant_syncobj(sync);

   assert(device->acc_drm != NULL);
   int err = device->acc_drm->vt->DrmSyncobjImportSyncFile(device->acc_drm, sobj->syncobj, sync_file);
   if (err) {
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "DrmSyncobjImportSyncFile failed: %m");
   }

   return VK_SUCCESS;
}

static VkResult
vk_accelerant_syncobj_export_sync_file(struct vk_device *device,
                                struct vk_sync *sync,
                                int *sync_file)
{
   struct vk_accelerant_syncobj *sobj = to_accelerant_syncobj(sync);

   assert(device->acc_drm != NULL);
   int err = device->acc_drm->vt->DrmSyncobjExportSyncFile(device->acc_drm, sobj->syncobj, sync_file);
   if (err) {
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "DrmSyncobjExportSyncFile failed: %m");
   }

   return VK_SUCCESS;
}

static VkResult
vk_accelerant_syncobj_move(struct vk_device *device,
                    struct vk_sync *dst,
                    struct vk_sync *src)
{
   struct vk_accelerant_syncobj *dst_sobj = to_accelerant_syncobj(dst);
   struct vk_accelerant_syncobj *src_sobj = to_accelerant_syncobj(src);
   VkResult result;

   if (!(dst->flags & VK_SYNC_IS_SHARED) &&
       !(src->flags & VK_SYNC_IS_SHARED)) {
      result = vk_accelerant_syncobj_reset(device, dst);
      if (unlikely(result != VK_SUCCESS))
         return result;

      uint32_t tmp = dst_sobj->syncobj;
      dst_sobj->syncobj = src_sobj->syncobj;
      src_sobj->syncobj = tmp;

      return VK_SUCCESS;
   } else {
      int fd;
      result = vk_accelerant_syncobj_export_sync_file(device, src, &fd);
      if (result != VK_SUCCESS)
         return result;

      result = vk_accelerant_syncobj_import_sync_file(device, dst, fd);
      if (fd >= 0)
         close(fd);
      if (result != VK_SUCCESS)
         return result;

      return vk_accelerant_syncobj_reset(device, src);
   }
}

struct vk_sync_type
vk_accelerant_syncobj_get_type(struct accelerant_base *acc)
{
   struct accelerant_drm *acc_drm = acc->vt->QueryInterface(acc, B_ACCELERANT_IFACE_DRM, 0);
   if (acc_drm == NULL)
      return (struct vk_sync_type) { .features = 0 };

   struct vk_sync_type type = {
      .size = sizeof(struct vk_accelerant_syncobj),
      .features = VK_SYNC_FEATURE_BINARY |
                  VK_SYNC_FEATURE_GPU_WAIT |
                  VK_SYNC_FEATURE_CPU_RESET |
                  VK_SYNC_FEATURE_CPU_SIGNAL |
                  VK_SYNC_FEATURE_WAIT_PENDING |
                  VK_SYNC_FEATURE_CPU_WAIT |
                  VK_SYNC_FEATURE_WAIT_ANY |
                  VK_SYNC_FEATURE_TIMELINE,
      .init = vk_accelerant_syncobj_init,
      .finish = vk_accelerant_syncobj_finish,
      .signal = vk_accelerant_syncobj_signal,
      .get_value = vk_accelerant_syncobj_get_value,
      .reset = vk_accelerant_syncobj_reset,
      .move = vk_accelerant_syncobj_move,
      .wait_many = vk_accelerant_syncobj_wait_many,
      .import_opaque_fd = vk_accelerant_syncobj_import_opaque_fd,
      .export_opaque_fd = vk_accelerant_syncobj_export_opaque_fd,
      .import_sync_file = vk_accelerant_syncobj_import_sync_file,
      .export_sync_file = vk_accelerant_syncobj_export_sync_file
   };

   return type;
}
