/**************************************************************************
 *
 * Copyright 2019 Red Hat.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 **************************************************************************/

/*
 * NIR lowering passes to handle the draw stages for
 * - pstipple
 * - aaline
 * - aapoint.
 *
 * These are all ported from the equivalent TGSI transforms.
 */

#include "nir.h"
#include "tgsi/tgsi_from_mesa.h"
#include "nir_builder.h"

#include "nir_draw_helpers.h"

typedef struct {
   nir_builder b;
   nir_shader *shader;
   bool fs_pos_is_sysval;
   nir_variable *stip_tex;
   nir_ssa_def *fragcoord;
   nir_alu_type bool_type;
} lower_pstipple;

static nir_ssa_def *
load_frag_coord(nir_builder *b)
{
   nir_foreach_shader_in_variable(var, b->shader) {
      if (var->data.location == VARYING_SLOT_POS)
         return nir_load_var(b, var);
   }

   nir_variable *pos = nir_variable_create(b->shader, nir_var_shader_in,
                                           glsl_vec4_type(), NULL);
   pos->data.location = VARYING_SLOT_POS;
   pos->data.interpolation = INTERP_MODE_NOPERSPECTIVE;
   pos->data.driver_location = b->shader->num_inputs++;
   return nir_load_var(b, pos);
}

static void
nir_lower_pstipple_block(nir_block *block,
                         lower_pstipple *state)
{
   nir_builder *b = &state->b;
   nir_ssa_def *texcoord;

   b->cursor = nir_before_block(block);

   nir_ssa_def *frag_coord = state->fs_pos_is_sysval ? nir_load_frag_coord(b) : load_frag_coord(b);

   texcoord = nir_fmul(b, nir_channels(b, frag_coord, 0x3),
                       nir_imm_vec2(b, 1.0/32.0, 1.0/32.0));

   nir_tex_instr *tex = nir_tex_instr_create(b->shader, 1);
   tex->op = nir_texop_tex;
   tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
   tex->coord_components = 2;
   tex->dest_type = nir_type_float32;
   tex->texture_index = state->stip_tex->data.binding;
   tex->sampler_index = state->stip_tex->data.binding;
   tex->src[0].src_type = nir_tex_src_coord;
   tex->src[0].src = nir_src_for_ssa(texcoord);
   nir_ssa_dest_init(&tex->instr, &tex->dest, 4, 32, NULL);

   nir_builder_instr_insert(b, &tex->instr);

   nir_ssa_def *condition = nir_f2b32(b, nir_channel(b, &tex->dest.ssa, 3));

   switch (state->bool_type) {
   case nir_type_bool1:
      condition = nir_f2b(b, nir_channel(b, &tex->dest.ssa, 3));
      break;
   case nir_type_bool32:
      condition = nir_f2b32(b, nir_channel(b, &tex->dest.ssa, 3));
      break;
   default:
      unreachable("Invalid Boolean type.");
   }

   nir_discard_if(b, condition);
   b->shader->info.fs.uses_discard = true;
}

static void
nir_lower_pstipple_impl(nir_function_impl *impl,
                        lower_pstipple *state)
{
   nir_builder *b = &state->b;

   nir_builder_init(b, impl);

   nir_block *start = nir_start_block(impl);
   nir_lower_pstipple_block(start, state);
}

void
nir_lower_pstipple_fs(struct nir_shader *shader,
                      unsigned *samplerUnitOut,
                      unsigned fixedUnit,
                      bool fs_pos_is_sysval,
                      nir_alu_type bool_type)
{
   lower_pstipple state = {
      .shader = shader,
      .fs_pos_is_sysval = fs_pos_is_sysval,
      .bool_type = bool_type,
   };

   assert(bool_type == nir_type_bool1 ||
          bool_type == nir_type_bool32);

   if (shader->info.stage != MESA_SHADER_FRAGMENT)
      return;

   int binding = 0;
   nir_foreach_uniform_variable(var, shader) {
      if (glsl_type_is_sampler(var->type)) {
         if (var->data.binding >= binding)
            binding = var->data.binding + 1;
      }
   }
   const struct glsl_type *sampler2D =
      glsl_sampler_type(GLSL_SAMPLER_DIM_2D, false, false, GLSL_TYPE_FLOAT);

   nir_variable *tex_var = nir_variable_create(shader, nir_var_uniform, sampler2D, "stipple_tex");
   tex_var->data.binding = binding;
   tex_var->data.explicit_binding = true;
   tex_var->data.how_declared = nir_var_hidden;

   BITSET_SET(shader->info.textures_used, binding);
   BITSET_SET(shader->info.samplers_used, binding);
   state.stip_tex = tex_var;

   nir_foreach_function(function, shader) {
      if (function->impl) {
         nir_lower_pstipple_impl(function->impl, &state);
      }
   }
   *samplerUnitOut = binding;
}

typedef struct {
   nir_variable *line_width_input;
   nir_variable *stipple_counter;
   nir_variable *stipple_pattern;
} lower_aaline;

static bool
lower_aaline_instr(nir_builder *b, nir_instr *instr, void *data)
{
   lower_aaline *state = data;

   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
   if (intrin->intrinsic != nir_intrinsic_store_deref)
      return false;

   nir_variable *var = nir_intrinsic_get_var(intrin, 0);
   if (var->data.mode != nir_var_shader_out)
      return false;
   if (var->data.location < FRAG_RESULT_DATA0 && var->data.location != FRAG_RESULT_COLOR)
      return false;

   nir_ssa_def *out_input = intrin->src[1].ssa;
   b->cursor = nir_before_instr(instr);
   nir_ssa_def *lw = nir_load_var(b, state->line_width_input);
   nir_ssa_def *len = nir_channel(b, lw, 3);
   len = nir_fadd_imm(b, nir_fmul_imm(b, len, 2.0), -1.0);
   nir_ssa_def *tmp = nir_fsat(b, nir_fadd(b, nir_channels(b, lw, 0xa),
                                             nir_fneg(b, nir_fabs(b, nir_channels(b, lw, 0x5)))));

   nir_ssa_def *max = len;
   if (state->stipple_counter) {
      assert(state->stipple_pattern);

      nir_ssa_def *counter = nir_load_var(b, state->stipple_counter);
      nir_ssa_def *pattern = nir_load_var(b, state->stipple_pattern);
      nir_ssa_def *factor = nir_i2f32(b, nir_ishr_imm(b, pattern, 16));
      pattern = nir_iand_imm(b, pattern, 0xffff);

      nir_ssa_def *stipple_pos = nir_vec2(b, nir_fadd_imm(b, counter, -0.5),
                                             nir_fadd_imm(b, counter, 0.5));

      stipple_pos = nir_frem(b, nir_fdiv(b, stipple_pos, factor),
                                 nir_imm_float(b, 16.0));

      nir_ssa_def *p = nir_f2i32(b, stipple_pos);
      nir_ssa_def *one = nir_imm_float(b, 1.0);

      // float t = 1.0 - min((1.0 - fract(stipple_pos.x)) * factor, 1.0);
      nir_ssa_def *t = nir_ffract(b, nir_channel(b, stipple_pos, 0));
      t = nir_fsub(b, one,
                     nir_fmin(b, nir_fmul(b, factor,
                                          nir_fsub(b, one, t)), one));

      // vec2 a = vec2((uvec2(pattern) >> p) & uvec2(1u));
      nir_ssa_def *a = nir_i2f32(b,
         nir_iand(b, nir_ishr(b, nir_vec2(b, pattern, pattern), p),
                  nir_imm_ivec2(b, 1, 1)));

      // float cov = mix(a.x, a.y, t);
      nir_ssa_def *cov = nir_flrp(b, nir_channel(b, a, 0), nir_channel(b, a, 1), t);

      max = nir_fmin(b, len, cov);
   }

   tmp = nir_fmul(b, nir_channel(b, tmp, 0),
                  nir_fmin(b, nir_channel(b, tmp, 1), max));
   tmp = nir_fmul(b, nir_channel(b, out_input, 3), tmp);

   nir_ssa_def *out = nir_vec4(b, nir_channel(b, out_input, 0),
                                 nir_channel(b, out_input, 1),
                                 nir_channel(b, out_input, 2),
                                 tmp);
   nir_instr_rewrite_src(instr, &intrin->src[1], nir_src_for_ssa(out));
   return true;
}

void
nir_lower_aaline_fs(struct nir_shader *shader, int *varying,
                    nir_variable *stipple_counter,
                    nir_variable *stipple_pattern)
{
   lower_aaline state = {
      .stipple_counter = stipple_counter,
      .stipple_pattern = stipple_pattern,
   };
   assert(shader->info.stage == MESA_SHADER_FRAGMENT);

   int highest_location = -1, highest_drv_location = -1;
   nir_foreach_shader_in_variable(var, shader) {
     if ((int)var->data.location > highest_location)
         highest_location = var->data.location;
     if ((int)var->data.driver_location > highest_drv_location)
         highest_drv_location = var->data.driver_location;
   }

   nir_variable *line_width = nir_variable_create(shader, nir_var_shader_in,
                                                  glsl_vec4_type(), "aaline");
   if (highest_location == -1 || highest_location < VARYING_SLOT_VAR0) {
     line_width->data.location = VARYING_SLOT_VAR0;
     line_width->data.driver_location = highest_drv_location + 1;
   } else {
     line_width->data.location = highest_location + 1;
     line_width->data.driver_location = highest_drv_location + 1;
   }
   shader->num_inputs++;
   *varying = tgsi_get_generic_gl_varying_index(line_width->data.location, true);
   state.line_width_input = line_width;

   nir_shader_instructions_pass(shader, lower_aaline_instr,
                                nir_metadata_dominance, &state);
}

typedef struct {
   nir_builder b;
   nir_shader *shader;
   nir_variable *input;
} lower_aapoint;

static void
nir_lower_aapoint_block(nir_block *block,
                        lower_aapoint *state, nir_ssa_def *sel)
{
  nir_builder *b = &state->b;
  nir_foreach_instr(instr, block) {
      if (instr->type != nir_instr_type_intrinsic)
         continue;

      nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
      if (intrin->intrinsic != nir_intrinsic_store_deref)
         continue;

      nir_variable *var = nir_intrinsic_get_var(intrin, 0);
      if (var->data.mode != nir_var_shader_out)
         continue;
      if (var->data.location < FRAG_RESULT_DATA0 && var->data.location != FRAG_RESULT_COLOR)
         continue;

      nir_ssa_def *out_input = intrin->src[1].ssa;
      b->cursor = nir_before_instr(instr);

      nir_ssa_def *tmp = nir_fmul(b, nir_channel(b, out_input, 3), sel);
      nir_ssa_def *out = nir_vec4(b, nir_channel(b, out_input, 0),
                                  nir_channel(b, out_input, 1),
                                  nir_channel(b, out_input, 2),
                                  tmp);
      nir_instr_rewrite_src(instr, &intrin->src[1], nir_src_for_ssa(out));
   }

}

static void
nir_lower_aapoint_impl(nir_function_impl *impl, lower_aapoint *state,
                       nir_alu_type bool_type)
{
   nir_builder *b = &state->b;

   nir_builder_init(b, impl);

   nir_block *block = nir_start_block(impl);
   b->cursor = nir_before_block(block);

   nir_ssa_def *aainput = nir_load_var(b, state->input);

   nir_ssa_def *dist = nir_fadd(b, nir_fmul(b, nir_channel(b, aainput, 0), nir_channel(b, aainput, 0)),
                                nir_fmul(b, nir_channel(b, aainput, 1), nir_channel(b, aainput, 1)));

   nir_ssa_def *k = nir_channel(b, aainput, 2);
   nir_ssa_def *chan_val_one = nir_channel(b, aainput, 3);
   nir_ssa_def *comp;

   switch (bool_type) {
   case nir_type_bool1:
      comp = nir_flt(b, chan_val_one, dist);
      break;
   case nir_type_bool32:
      comp = nir_flt32(b, chan_val_one, dist);
      break;
   case nir_type_float32:
      comp = nir_slt(b, chan_val_one, dist);
      break;
   default:
      unreachable("Invalid Boolean type.");
   }

   nir_discard_if(b, comp);
   b->shader->info.fs.uses_discard = true;

   /* compute coverage factor = (1-d)/(1-k) */
   /* 1 - k */
   nir_ssa_def *tmp = nir_fadd(b, chan_val_one, nir_fneg(b, k));
   /* 1.0 / (1 - k) */
   tmp = nir_frcp(b, tmp);

   /* 1 - d */
   nir_ssa_def *tmp2 = nir_fadd(b, chan_val_one, nir_fneg(b, dist));

   /* (1 - d) / (1 - k) */
   nir_ssa_def *coverage = nir_fmul(b, tmp, tmp2);

   /* if (k >= distance)
    *    sel = coverage;
    * else
    *    sel = 1.0;
    */
   nir_ssa_def *sel;

   switch (bool_type) {
   case nir_type_bool1:
      sel = nir_b32csel(b, nir_fge(b, k, dist), coverage, chan_val_one);
      break;
   case nir_type_bool32:
      sel = nir_b32csel(b, nir_fge32(b, k, dist), coverage, chan_val_one);
      break;
   case nir_type_float32: {
      /* On this path, don't assume that any "fancy" instructions are
       * supported, but also try to emit something decent.
       *
       *    sel = (k >= distance) ? coverage : 1.0;
       *    sel = (k >= distance) * coverage : (1 - (k >= distance)) * 1.0
       *    sel = (k >= distance) * coverage : (1 - (k >= distance))
       *
       * Since (k >= distance) * coverage is zero when (1 - (k >= distance))
       * is not zero,
       *
       *    sel = (k >= distance) * coverage + (1 - (k >= distance))
       *
       * If we assume that coverage == fsat(coverage), this could be further
       * optimized to fsat(coverage + (1 - (k >= distance))), but I don't feel
       * like verifying that right now.
       */
      nir_ssa_def *cmp_result = nir_sge(b, k, dist);
      sel = nir_fadd(b,
                     nir_fmul(b, coverage, cmp_result),
                     nir_fadd(b, chan_val_one, nir_fneg(b, cmp_result)));
      break;
   }
   default:
      unreachable("Invalid Boolean type.");
   }

   nir_foreach_block(block, impl) {
     nir_lower_aapoint_block(block, state, sel);
   }
}

void
nir_lower_aapoint_fs(struct nir_shader *shader, int *varying, const nir_alu_type bool_type)
{
   assert(bool_type == nir_type_bool1 ||
          bool_type == nir_type_bool32 ||
          bool_type == nir_type_float32);

   lower_aapoint state = {
      .shader = shader,
   };
   if (shader->info.stage != MESA_SHADER_FRAGMENT)
      return;

   int highest_location = -1, highest_drv_location = -1;
   nir_foreach_shader_in_variable(var, shader) {
     if ((int)var->data.location > highest_location)
         highest_location = var->data.location;
     if ((int)var->data.driver_location > highest_drv_location)
         highest_drv_location = var->data.driver_location;
   }

   nir_variable *aapoint_input = nir_variable_create(shader, nir_var_shader_in,
                                                     glsl_vec4_type(), "aapoint");
   if (highest_location == -1 || highest_location < VARYING_SLOT_VAR0) {
     aapoint_input->data.location = VARYING_SLOT_VAR0;
   } else {
     aapoint_input->data.location = highest_location + 1;
   }
   aapoint_input->data.driver_location = highest_drv_location + 1;

   shader->num_inputs++;
   *varying = tgsi_get_generic_gl_varying_index(aapoint_input->data.location, true);
   state.input = aapoint_input;

   nir_foreach_function(function, shader) {
      if (function->impl) {
         nir_lower_aapoint_impl(function->impl, &state, bool_type);
      }
   }
}
