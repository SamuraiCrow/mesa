/*
 * Copyright 2017 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "si_pipe.h"
#include "si_query.h"
#include "si_shader_internal.h"
#include "util/u_prim.h"

static LLVMValueRef get_wave_id_in_tg(struct si_shader_context *ctx)
{
   return si_unpack_param(ctx, ctx->args->ac.merged_wave_info, 24, 4);
}

LLVMValueRef gfx10_get_thread_id_in_tg(struct si_shader_context *ctx)
{
   LLVMBuilderRef builder = ctx->ac.builder;
   LLVMValueRef tmp;
   tmp = LLVMBuildMul(builder, get_wave_id_in_tg(ctx),
                      LLVMConstInt(ctx->ac.i32, ctx->ac.wave_size, false), "");
   return LLVMBuildAdd(builder, tmp, ac_get_thread_id(&ctx->ac), "");
}

static LLVMValueRef ngg_get_query_buf(struct si_shader_context *ctx)
{
   return ac_build_load_to_sgpr(&ctx->ac,
                                ac_get_ptr_arg(&ctx->ac, &ctx->args->ac, ctx->args->internal_bindings),
                                LLVMConstInt(ctx->ac.i32, SI_GS_QUERY_BUF, false));
}

static LLVMValueRef ngg_get_emulated_counters_buf(struct si_shader_context *ctx)
{
   return ac_build_load_to_sgpr(&ctx->ac,
                                ac_get_ptr_arg(&ctx->ac, &ctx->args->ac, ctx->args->internal_bindings),
                                LLVMConstInt(ctx->ac.i32, SI_GS_QUERY_EMULATED_COUNTERS_BUF, false));
}

unsigned gfx10_ngg_get_vertices_per_prim(struct si_shader *shader)
{
   const struct si_shader_info *info = &shader->selector->info;

   if (shader->selector->stage == MESA_SHADER_GEOMETRY)
      return u_vertices_per_prim(info->base.gs.output_primitive);
   else if (shader->selector->stage == MESA_SHADER_VERTEX) {
      if (info->base.vs.blit_sgprs_amd) {
         /* Blits always use axis-aligned rectangles with 3 vertices. */
         return 3;
      } else if (shader->key.ge.opt.ngg_culling & SI_NGG_CULL_LINES)
         return 2;
      else {
         /* We always build up all three indices for the prim export
          * independent of the primitive type. The additional garbage
          * data shouldn't hurt. This is used by exports and streamout.
          */
         return 3;
      }
   } else {
      assert(shader->selector->stage == MESA_SHADER_TESS_EVAL);

      if (info->base.tess.point_mode)
         return 1;
      else if (info->base.tess._primitive_mode == TESS_PRIMITIVE_ISOLINES)
         return 2;
      else
         return 3;
   }
}

bool gfx10_ngg_export_prim_early(struct si_shader *shader)
{
   struct si_shader_selector *sel = shader->selector;

   assert(shader->key.ge.as_ngg && !shader->key.ge.as_es);

   return sel->stage != MESA_SHADER_GEOMETRY &&
          !gfx10_ngg_writes_user_edgeflags(shader);
}

static void clamp_gsprims_to_esverts(unsigned *max_gsprims, unsigned max_esverts,
                                     unsigned min_verts_per_prim, bool use_adjacency)
{
   unsigned max_reuse = max_esverts - min_verts_per_prim;
   if (use_adjacency)
      max_reuse /= 2;
   *max_gsprims = MIN2(*max_gsprims, 1 + max_reuse);
}

unsigned gfx10_ngg_get_scratch_dw_size(struct si_shader *shader)
{
   const struct si_shader_selector *sel = shader->selector;

   return ac_ngg_get_scratch_lds_size(sel->stage,
                                      si_get_max_workgroup_size(shader),
                                      shader->wave_size,
                                      si_shader_uses_streamout(shader),
                                      shader->key.ge.opt.ngg_culling) / 4;
}

/**
 * Determine subgroup information like maximum number of vertices and prims.
 *
 * This happens before the shader is uploaded, since LDS relocations during
 * upload depend on the subgroup size.
 */
bool gfx10_ngg_calculate_subgroup_info(struct si_shader *shader)
{
   const struct si_shader_selector *gs_sel = shader->selector;
   const struct si_shader_selector *es_sel =
      shader->previous_stage_sel ? shader->previous_stage_sel : gs_sel;
   const gl_shader_stage gs_stage = gs_sel->stage;
   const unsigned gs_num_invocations = MAX2(gs_sel->info.base.gs.invocations, 1);
   const unsigned input_prim = si_get_input_prim(gs_sel, &shader->key);
   const bool use_adjacency =
      input_prim >= PIPE_PRIM_LINES_ADJACENCY && input_prim <= PIPE_PRIM_TRIANGLE_STRIP_ADJACENCY;
   const unsigned max_verts_per_prim = u_vertices_per_prim(input_prim);
   const unsigned min_verts_per_prim = gs_stage == MESA_SHADER_GEOMETRY ? max_verts_per_prim : 1;

   /* All these are in dwords: */
   /* GE can only use 8K dwords (32KB) of LDS per workgroup.
    */
   const unsigned max_lds_size = 8 * 1024 - gfx10_ngg_get_scratch_dw_size(shader);
   const unsigned target_lds_size = max_lds_size;
   unsigned esvert_lds_size = 0;
   unsigned gsprim_lds_size = 0;

   /* All these are per subgroup: */
   const unsigned min_esverts =
      gs_sel->screen->info.gfx_level >= GFX11 ? 3 : /* gfx11 requires at least 1 primitive per TG */
      gs_sel->screen->info.gfx_level >= GFX10_3 ? 29 : (24 - 1 + max_verts_per_prim);
   bool max_vert_out_per_gs_instance = false;
   unsigned max_gsprims_base = gs_sel->screen->ngg_subgroup_size; /* default prim group size clamp */
   unsigned max_esverts_base = gs_sel->screen->ngg_subgroup_size;

   if (gs_stage == MESA_SHADER_GEOMETRY) {
      bool force_multi_cycling = false;
      unsigned max_out_verts_per_gsprim = gs_sel->info.base.gs.vertices_out * gs_num_invocations;

retry_select_mode:
      if (max_out_verts_per_gsprim <= 256 && !force_multi_cycling) {
         if (max_out_verts_per_gsprim) {
            max_gsprims_base = MIN2(max_gsprims_base, 256 / max_out_verts_per_gsprim);
         }
      } else {
         /* Use special multi-cycling mode in which each GS
          * instance gets its own subgroup. Does not work with
          * tessellation. */
         max_vert_out_per_gs_instance = true;
         max_gsprims_base = 1;
         max_out_verts_per_gsprim = gs_sel->info.base.gs.vertices_out;
      }

      esvert_lds_size = es_sel->info.esgs_itemsize / 4;
      gsprim_lds_size = (gs_sel->info.gsvs_vertex_size / 4 + 1) * max_out_verts_per_gsprim;

      if (gsprim_lds_size > target_lds_size && !force_multi_cycling) {
         if (gs_sel->tess_turns_off_ngg || es_sel->stage != MESA_SHADER_TESS_EVAL) {
            force_multi_cycling = true;
            goto retry_select_mode;
         }
      }
   } else {
      /* VS and TES. */

      bool uses_instance_id = gs_sel->info.uses_instanceid;
      bool uses_primitive_id = gs_sel->info.uses_primid;
      if (gs_stage == MESA_SHADER_VERTEX) {
         uses_instance_id |=
            shader->key.ge.part.vs.prolog.instance_divisor_is_one ||
            shader->key.ge.part.vs.prolog.instance_divisor_is_fetched;
      } else {
         uses_primitive_id |= shader->key.ge.mono.u.vs_export_prim_id;
      }

      esvert_lds_size = ac_ngg_nogs_get_pervertex_lds_size(
         gs_stage, gs_sel->info.num_outputs,
         si_shader_uses_streamout(shader),
         shader->key.ge.mono.u.vs_export_prim_id,
         gfx10_ngg_writes_user_edgeflags(shader),
         shader->key.ge.opt.ngg_culling,
         uses_instance_id,
         uses_primitive_id) / 4;
   }

   unsigned max_gsprims = max_gsprims_base;
   unsigned max_esverts = max_esverts_base;

   if (esvert_lds_size)
      max_esverts = MIN2(max_esverts, target_lds_size / esvert_lds_size);
   if (gsprim_lds_size)
      max_gsprims = MIN2(max_gsprims, target_lds_size / gsprim_lds_size);

   max_esverts = MIN2(max_esverts, max_gsprims * max_verts_per_prim);
   clamp_gsprims_to_esverts(&max_gsprims, max_esverts, min_verts_per_prim, use_adjacency);
   assert(max_esverts >= max_verts_per_prim && max_gsprims >= 1);

   if (esvert_lds_size || gsprim_lds_size) {
      /* Now that we have a rough proportionality between esverts
       * and gsprims based on the primitive type, scale both of them
       * down simultaneously based on required LDS space.
       *
       * We could be smarter about this if we knew how much vertex
       * reuse to expect.
       */
      unsigned lds_total = max_esverts * esvert_lds_size + max_gsprims * gsprim_lds_size;
      if (lds_total > target_lds_size) {
         max_esverts = max_esverts * target_lds_size / lds_total;
         max_gsprims = max_gsprims * target_lds_size / lds_total;

         max_esverts = MIN2(max_esverts, max_gsprims * max_verts_per_prim);
         clamp_gsprims_to_esverts(&max_gsprims, max_esverts, min_verts_per_prim, use_adjacency);
         assert(max_esverts >= max_verts_per_prim && max_gsprims >= 1);
      }
   }

   /* Round up towards full wave sizes for better ALU utilization. */
   if (!max_vert_out_per_gs_instance) {
      unsigned orig_max_esverts;
      unsigned orig_max_gsprims;
      do {
         orig_max_esverts = max_esverts;
         orig_max_gsprims = max_gsprims;

         max_esverts = align(max_esverts, shader->wave_size);
         max_esverts = MIN2(max_esverts, max_esverts_base);
         if (esvert_lds_size)
            max_esverts =
               MIN2(max_esverts, (max_lds_size - max_gsprims * gsprim_lds_size) / esvert_lds_size);
         max_esverts = MIN2(max_esverts, max_gsprims * max_verts_per_prim);

         /* Hardware restriction: minimum value of max_esverts */
         max_esverts = MAX2(max_esverts, min_esverts);

         max_gsprims = align(max_gsprims, shader->wave_size);
         max_gsprims = MIN2(max_gsprims, max_gsprims_base);
         if (gsprim_lds_size) {
            /* Don't count unusable vertices to the LDS size. Those are vertices above
             * the maximum number of vertices that can occur in the workgroup,
             * which is e.g. max_gsprims * 3 for triangles.
             */
            unsigned usable_esverts = MIN2(max_esverts, max_gsprims * max_verts_per_prim);
            max_gsprims =
               MIN2(max_gsprims, (max_lds_size - usable_esverts * esvert_lds_size) / gsprim_lds_size);
         }
         clamp_gsprims_to_esverts(&max_gsprims, max_esverts, min_verts_per_prim, use_adjacency);
         assert(max_esverts >= max_verts_per_prim && max_gsprims >= 1);
      } while (orig_max_esverts != max_esverts || orig_max_gsprims != max_gsprims);

      /* Verify the restriction. */
      assert(max_esverts >= min_esverts);
   } else {
      max_esverts = MAX2(max_esverts, min_esverts);
   }

   unsigned max_out_vertices =
      max_vert_out_per_gs_instance
         ? gs_sel->info.base.gs.vertices_out
         : gs_stage == MESA_SHADER_GEOMETRY
              ? max_gsprims * gs_num_invocations * gs_sel->info.base.gs.vertices_out
              : max_esverts;
   assert(max_out_vertices <= 256);

   unsigned prim_amp_factor = 1;
   if (gs_stage == MESA_SHADER_GEOMETRY) {
      /* Number of output primitives per GS input primitive after
       * GS instancing. */
      prim_amp_factor = gs_sel->info.base.gs.vertices_out;
   }

   shader->ngg.hw_max_esverts = max_esverts;
   shader->ngg.max_gsprims = max_gsprims;
   shader->ngg.max_out_verts = max_out_vertices;
   shader->ngg.prim_amp_factor = prim_amp_factor;
   shader->ngg.max_vert_out_per_gs_instance = max_vert_out_per_gs_instance;

   /* Don't count unusable vertices. */
   shader->gs_info.esgs_ring_size = MIN2(max_esverts, max_gsprims * max_verts_per_prim) *
                                    esvert_lds_size;
   shader->ngg.ngg_emit_size = max_gsprims * gsprim_lds_size;

   assert(shader->ngg.hw_max_esverts >= min_esverts); /* HW limitation */

   /* If asserts are disabled, we use the same conditions to return false */
   return max_esverts >= max_verts_per_prim && max_gsprims >= 1 &&
          max_out_vertices <= 256 &&
          shader->ngg.hw_max_esverts >= min_esverts;
}
