/*
 * Copyright (C) 2019 Rob Clark <robclark@freedesktop.org>
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 */

#define FD_BO_NO_HARDPIN 1

#include "pipe/p_state.h"
#include "util/u_dump.h"
#include "u_tracepoints.h"

#include "freedreno_resource.h"
#include "freedreno_tracepoints.h"

#include "fd6_barrier.h"
#include "fd6_compute.h"
#include "fd6_const.h"
#include "fd6_context.h"
#include "fd6_emit.h"
#include "fd6_pack.h"

/* maybe move to fd6_program? */
static void
cs_program_emit(struct fd_context *ctx, struct fd_ringbuffer *ring,
                struct ir3_shader_variant *v)
   assert_dt
{
   const struct ir3_info *i = &v->info;
   enum a6xx_threadsize thrsz = i->double_threadsize ? THREAD128 : THREAD64;

   OUT_REG(ring, A6XX_HLSQ_INVALIDATE_CMD(.vs_state = true, .hs_state = true,
                                          .ds_state = true, .gs_state = true,
                                          .fs_state = true, .cs_state = true,
                                          .gfx_ibo = true, .cs_ibo = true, ));

   OUT_PKT4(ring, REG_A6XX_HLSQ_CS_CNTL, 1);
   OUT_RING(ring, A6XX_HLSQ_CS_CNTL_CONSTLEN(v->constlen) |
                     A6XX_HLSQ_CS_CNTL_ENABLED);

   OUT_PKT4(ring, REG_A6XX_SP_CS_CONFIG, 1);
   OUT_RING(ring, A6XX_SP_CS_CONFIG_ENABLED |
                     COND(v->bindless_tex, A6XX_SP_CS_CONFIG_BINDLESS_TEX) |
                     COND(v->bindless_samp, A6XX_SP_CS_CONFIG_BINDLESS_SAMP) |
                     COND(v->bindless_ibo, A6XX_SP_CS_CONFIG_BINDLESS_IBO) |
                     COND(v->bindless_ubo, A6XX_SP_CS_CONFIG_BINDLESS_UBO) |
                     A6XX_SP_CS_CONFIG_NIBO(ir3_shader_nibo(v)) |
                     A6XX_SP_CS_CONFIG_NTEX(v->num_samp) |
                     A6XX_SP_CS_CONFIG_NSAMP(v->num_samp)); /* SP_CS_CONFIG */

   OUT_PKT4(ring, REG_A6XX_SP_CS_CTRL_REG0, 1);
   OUT_RING(ring,
            A6XX_SP_CS_CTRL_REG0_THREADSIZE(thrsz) |
               A6XX_SP_CS_CTRL_REG0_FULLREGFOOTPRINT(i->max_reg + 1) |
               A6XX_SP_CS_CTRL_REG0_HALFREGFOOTPRINT(i->max_half_reg + 1) |
               COND(v->mergedregs, A6XX_SP_CS_CTRL_REG0_MERGEDREGS) |
               A6XX_SP_CS_CTRL_REG0_BRANCHSTACK(ir3_shader_branchstack_hw(v)));

   uint32_t local_invocation_id, work_group_id;
   local_invocation_id =
      ir3_find_sysval_regid(v, SYSTEM_VALUE_LOCAL_INVOCATION_ID);
   work_group_id = ir3_find_sysval_regid(v, SYSTEM_VALUE_WORKGROUP_ID);

   OUT_PKT4(ring, REG_A6XX_HLSQ_CS_CNTL_0, 2);
   OUT_RING(ring, A6XX_HLSQ_CS_CNTL_0_WGIDCONSTID(work_group_id) |
                     A6XX_HLSQ_CS_CNTL_0_WGSIZECONSTID(regid(63, 0)) |
                     A6XX_HLSQ_CS_CNTL_0_WGOFFSETCONSTID(regid(63, 0)) |
                     A6XX_HLSQ_CS_CNTL_0_LOCALIDREGID(local_invocation_id));
   OUT_RING(ring, A6XX_HLSQ_CS_CNTL_1_LINEARLOCALIDREGID(regid(63, 0)) |
                     A6XX_HLSQ_CS_CNTL_1_THREADSIZE(thrsz));

   if (ctx->screen->info->a6xx.has_lpac) {
      OUT_PKT4(ring, REG_A6XX_SP_CS_CNTL_0, 2);
      OUT_RING(ring, A6XX_SP_CS_CNTL_0_WGIDCONSTID(work_group_id) |
                        A6XX_SP_CS_CNTL_0_WGSIZECONSTID(regid(63, 0)) |
                        A6XX_SP_CS_CNTL_0_WGOFFSETCONSTID(regid(63, 0)) |
                        A6XX_SP_CS_CNTL_0_LOCALIDREGID(local_invocation_id));
      OUT_RING(ring, A6XX_SP_CS_CNTL_1_LINEARLOCALIDREGID(regid(63, 0)) |
                        A6XX_SP_CS_CNTL_1_THREADSIZE(thrsz));
   }

   fd6_emit_shader(ctx, ring, v);
}

static void
fd6_launch_grid(struct fd_context *ctx, const struct pipe_grid_info *info) in_dt
{
   struct ir3_shader_key key = {};
   struct ir3_shader_variant *v;
   struct fd_ringbuffer *ring = ctx->batch->draw;
   unsigned nglobal = 0;

   trace_start_compute(&ctx->batch->trace, ring, !!info->indirect, info->work_dim,
                       info->block[0], info->block[1], info->block[2],
                       info->grid[0],  info->grid[1],  info->grid[2]);

   v = ir3_shader_variant(ir3_get_shader(ctx->compute), key, false, &ctx->debug);
   if (!v)
      return;

   if (ctx->batch->barrier)
      fd6_barrier_flush(ctx->batch);

   if (ctx->dirty_shader[PIPE_SHADER_COMPUTE] & FD_DIRTY_SHADER_PROG)
      cs_program_emit(ctx, ring, v);

   bool emit_instrlen_workaround =
      v->instrlen > ctx->screen->info->a6xx.instr_cache_size;

   /* There appears to be a HW bug where in some rare circumstances it appears
    * to accidentally use the FS instrlen instead of the CS instrlen, which
    * affects all known gens. Based on various experiments it appears that the
    * issue is that when prefetching a branch destination and there is a cache
    * miss, when fetching from memory the HW bounds-checks the fetch against
    * SP_CS_INSTRLEN, except when one of the two register contexts is active
    * it accidentally fetches SP_FS_INSTRLEN from the other (inactive)
    * context. To workaround it we set the FS instrlen here and do a dummy
    * event to roll the context (because it fetches SP_FS_INSTRLEN from the
    * "wrong" context). Because the bug seems to involve cache misses, we
    * don't emit this if the entire CS program fits in cache, which will
    * hopefully be the majority of cases.
    *
    * See https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/19023
    */
   if (emit_instrlen_workaround) {
      OUT_REG(ring, A6XX_SP_FS_INSTRLEN(v->instrlen));
      fd6_event_write(ctx->batch, ring, LABEL, false);
   }

   fd6_emit_cs_state(ctx, ring, v);
   fd6_emit_cs_consts(v, ring, ctx, info);

   u_foreach_bit (i, ctx->global_bindings.enabled_mask)
      nglobal++;

   if (nglobal > 0) {
      /* global resources don't otherwise get an OUT_RELOC(), since
       * the raw ptr address is emitted in ir3_emit_cs_consts().
       * So to make the kernel aware that these buffers are referenced
       * by the batch, emit dummy reloc's as part of a no-op packet
       * payload:
       */
      OUT_PKT7(ring, CP_NOP, 2 * nglobal);
      u_foreach_bit (i, ctx->global_bindings.enabled_mask) {
         struct pipe_resource *prsc = ctx->global_bindings.buf[i];
         OUT_RELOC(ring, fd_resource(prsc)->bo, 0, 0, 0);
      }
   }

   OUT_PKT7(ring, CP_SET_MARKER, 1);
   OUT_RING(ring, A6XX_CP_SET_MARKER_0_MODE(RM6_COMPUTE));

   uint32_t shared_size =
      MAX2(((int)(v->cs.req_local_mem + info->variable_shared_mem) - 1) / 1024, 1);
   OUT_PKT4(ring, REG_A6XX_SP_CS_UNKNOWN_A9B1, 1);
   OUT_RING(ring, A6XX_SP_CS_UNKNOWN_A9B1_SHARED_SIZE(shared_size) |
                     A6XX_SP_CS_UNKNOWN_A9B1_UNK6);

   if (ctx->screen->info->a6xx.has_lpac) {
      OUT_PKT4(ring, REG_A6XX_HLSQ_CS_UNKNOWN_B9D0, 1);
      OUT_RING(ring, A6XX_HLSQ_CS_UNKNOWN_B9D0_SHARED_SIZE(shared_size) |
                        A6XX_HLSQ_CS_UNKNOWN_B9D0_UNK6);
   }

   const unsigned *local_size =
      info->block; // v->shader->nir->info->workgroup_size;
   const unsigned *num_groups = info->grid;
   /* for some reason, mesa/st doesn't set info->work_dim, so just assume 3: */
   const unsigned work_dim = info->work_dim ? info->work_dim : 3;
   OUT_PKT4(ring, REG_A6XX_HLSQ_CS_NDRANGE_0, 7);
   OUT_RING(ring, A6XX_HLSQ_CS_NDRANGE_0_KERNELDIM(work_dim) |
                  A6XX_HLSQ_CS_NDRANGE_0_LOCALSIZEX(local_size[0] - 1) |
                  A6XX_HLSQ_CS_NDRANGE_0_LOCALSIZEY(local_size[1] - 1) |
                  A6XX_HLSQ_CS_NDRANGE_0_LOCALSIZEZ(local_size[2] - 1));
   OUT_RING(ring,
            A6XX_HLSQ_CS_NDRANGE_1_GLOBALSIZE_X(local_size[0] * num_groups[0]));
   OUT_RING(ring, 0); /* HLSQ_CS_NDRANGE_2_GLOBALOFF_X */
   OUT_RING(ring,
            A6XX_HLSQ_CS_NDRANGE_3_GLOBALSIZE_Y(local_size[1] * num_groups[1]));
   OUT_RING(ring, 0); /* HLSQ_CS_NDRANGE_4_GLOBALOFF_Y */
   OUT_RING(ring,
            A6XX_HLSQ_CS_NDRANGE_5_GLOBALSIZE_Z(local_size[2] * num_groups[2]));
   OUT_RING(ring, 0); /* HLSQ_CS_NDRANGE_6_GLOBALOFF_Z */

   OUT_PKT4(ring, REG_A6XX_HLSQ_CS_KERNEL_GROUP_X, 3);
   OUT_RING(ring, 1); /* HLSQ_CS_KERNEL_GROUP_X */
   OUT_RING(ring, 1); /* HLSQ_CS_KERNEL_GROUP_Y */
   OUT_RING(ring, 1); /* HLSQ_CS_KERNEL_GROUP_Z */

   if (info->indirect) {
      struct fd_resource *rsc = fd_resource(info->indirect);

      OUT_PKT7(ring, CP_EXEC_CS_INDIRECT, 4);
      OUT_RING(ring, 0x00000000);
      OUT_RELOC(ring, rsc->bo, info->indirect_offset, 0, 0); /* ADDR_LO/HI */
      OUT_RING(ring,
               A5XX_CP_EXEC_CS_INDIRECT_3_LOCALSIZEX(local_size[0] - 1) |
                  A5XX_CP_EXEC_CS_INDIRECT_3_LOCALSIZEY(local_size[1] - 1) |
                  A5XX_CP_EXEC_CS_INDIRECT_3_LOCALSIZEZ(local_size[2] - 1));
   } else {
      OUT_PKT7(ring, CP_EXEC_CS, 4);
      OUT_RING(ring, 0x00000000);
      OUT_RING(ring, CP_EXEC_CS_1_NGROUPS_X(info->grid[0]));
      OUT_RING(ring, CP_EXEC_CS_2_NGROUPS_Y(info->grid[1]));
      OUT_RING(ring, CP_EXEC_CS_3_NGROUPS_Z(info->grid[2]));
   }

   trace_end_compute(&ctx->batch->trace, ring);

   fd_context_all_clean(ctx);
}

void
fd6_compute_init(struct pipe_context *pctx) disable_thread_safety_analysis
{
   struct fd_context *ctx = fd_context(pctx);
   ctx->launch_grid = fd6_launch_grid;
   pctx->create_compute_state = ir3_shader_compute_state_create;
   pctx->delete_compute_state = ir3_shader_state_delete;
}
