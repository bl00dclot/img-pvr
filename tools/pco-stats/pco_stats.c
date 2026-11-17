/*
 * XCLIPSE: host compiler statistics harness (see README.md). SPIR-V in, PCO statistics out.
 *
 * Mirrors the compile path of pvr_graphics_pipeline_compile() (src/imagination/vulkan/pvr_arch_pipeline.c)
 * for one fixed pipeline state: render pass path, one colour attachment, one sample, no blending,
 * no dynamic state. The allocation helpers below are copied from that file (marked), the rest of the
 * pco_data setup is re-created for that state. The GE8320 device info is the driver's.
 *
 * SPDX-License-Identifier: MIT
 */

#include "common/pvr_device_info.h"
#include "common/pvr_iface.h"
#include "common/pvr_limits.h"
#include "compiler/glsl_types.h"
#include "compiler/shader_enums.h"
#include "compiler/spirv/nir_spirv.h"
#include "hwdef/rogue_hw_defs.h"
#include "nir/nir.h"
#include "pco/pco.h"
#include "pco/pco_data.h"
#include "pco_uscgen_programs.h"
#include "util/bitscan.h"
#include "util/macros.h"
#include "util/ralloc.h"
#include "util/shader_stats.h"
#include "compiler/spirv/spirv_info.h"
#include "nir/nir_xfb_info.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- copied from pvr_arch_pipeline.c (the allocate_ and pvr_alloc_ helpers); keep in sync, the phone check catches drift ---- */
static void set_var(pco_range *allocation_list,
                    unsigned to,
                    nir_variable *var,
                    unsigned dwords_each)
{
   unsigned slots = glsl_count_dword_slots(var->type, false);

   allocation_list[var->data.location] = (pco_range){
      .start = to,
      .count = slots * dwords_each,
   };
}

static void allocate_var(pco_range *allocation_list,
                         unsigned *counter,
                         nir_variable *var,
                         unsigned dwords_each)
{
   unsigned slots = glsl_count_dword_slots(var->type, false);

   allocation_list[var->data.location] = (pco_range){
      .start = *counter,
      .count = slots * dwords_each,
   };

   *counter += slots * dwords_each;
}

static void try_allocate_var(pco_range *allocation_list,
                             unsigned *counter,
                             nir_shader *nir,
                             uint64_t bitset,
                             nir_variable_mode mode,
                             int location,
                             unsigned dwords_each)
{
   if (!(bitset & BITFIELD64_BIT(location)))
      return;

   nir_variable *var = nir_find_variable_with_location(nir, mode, location);
   assert(var);

   allocate_var(allocation_list, counter, var, dwords_each);
}

static void allocate_val(pco_range *allocation_list,
                         unsigned *counter,
                         unsigned location,
                         unsigned dwords_each)
{
   allocation_list[location] = (pco_range){
      .start = *counter,
      .count = dwords_each,
   };

   *counter += dwords_each;
}

static void try_allocate_vars(pco_range *allocation_list,
                              unsigned *counter,
                              nir_shader *nir,
                              uint64_t *bitset,
                              nir_variable_mode mode,
                              bool any,
                              bool f16,
                              enum glsl_interp_mode interp_mode,
                              unsigned dwords_each)
{
   uint64_t skipped = 0;

   while (*bitset) {
      int location = u_bit_scan64(bitset);
      unsigned accum = 0;

      nir_foreach_variable_with_modes (var, nir, mode) {
         if (var->data.location != location)
            continue;

         if (!any && (glsl_type_is_16bit(
                         glsl_without_array_or_matrix(var->type)) != f16 ||
                      var->data.interpolation != interp_mode)) {
            skipped |= BITFIELD64_BIT(location);
            break;
         }

         accum += glsl_count_dword_slots(var->type, false);
      }

      if (skipped & BITFIELD64_BIT(location))
         continue;

      accum *= dwords_each;
      allocate_val(allocation_list, counter, location, accum);
   }

   *bitset |= skipped;
}

static void check_unused_sysvals(
   BITSET_WORD system_values_read[static BITSET_WORDS(SYSTEM_VALUE_MAX)])
{
#if MESA_DEBUG
   if (!__bitset_is_empty(system_values_read, BITSET_WORDS(SYSTEM_VALUE_MAX))) {
      gl_system_value sysval;
      BITSET_FOREACH_SET (sysval, system_values_read, SYSTEM_VALUE_MAX) {
         debug_printf("unhandled sysval: %s\n", gl_system_value_name(sysval));
      }
   }
#endif
}

static void pvr_alloc_vs_sysvals(pco_data *data, nir_shader *nir)
{
   BITSET_DECLARE(system_values_read, SYSTEM_VALUE_MAX);
   BITSET_COPY(system_values_read, nir->info.system_values_read);

   gl_system_value sys_vals[] = {
      SYSTEM_VALUE_VERTEX_ID,     SYSTEM_VALUE_INSTANCE_ID,
      SYSTEM_VALUE_BASE_INSTANCE, SYSTEM_VALUE_BASE_VERTEX,
      SYSTEM_VALUE_DRAW_ID,       SYSTEM_VALUE_VIEW_INDEX,
   };

   for (unsigned u = 0; u < ARRAY_SIZE(sys_vals); ++u) {
      if (BITSET_TEST(system_values_read, sys_vals[u])) {
         nir_intrinsic_op op = nir_intrinsic_from_system_value(sys_vals[u]);
         unsigned dwords = nir_intrinsic_infos[op].dest_components;
         assert(dwords > 0);

         allocate_val(data->common.sys_vals,
                      &data->common.vtxins,
                      sys_vals[u],
                      dwords);

         BITSET_CLEAR(system_values_read, sys_vals[u]);
      }
   }

   check_unused_sysvals(system_values_read);
   assert(BITSET_IS_EMPTY(system_values_read));
}


static void pvr_alloc_vs_attribs(pco_data *data, nir_shader *nir)
{
   nir_foreach_shader_in_variable (var, nir) {
      allocate_var(data->vs.attribs, &data->common.vtxins, var, 1);
   }
}

static void pvr_alloc_vs_varyings(pco_data *data, nir_shader *nir)
{
   uint64_t vars_mask = nir->info.outputs_written &
                        BITFIELD64_RANGE(VARYING_SLOT_VAR0, MAX_VARYING);

   /* Varying ordering is specific. */
   try_allocate_var(data->vs.varyings,
                    &data->vs.vtxouts,
                    nir,
                    nir->info.outputs_written,
                    nir_var_shader_out,
                    VARYING_SLOT_POS,
                    1);

   /* Save varying counts. */
   nir_foreach_variable_with_modes (var, nir, nir_var_shader_out) {
      if (var->data.location < VARYING_SLOT_VAR0 ||
          var->data.location >= VARYING_SLOT_VAR0 + MAX_VARYING) {
         continue;
      }

      /* TODO: f16 support. */
      bool f16 = glsl_type_is_16bit(glsl_without_array_or_matrix(var->type));
      assert(!f16);
      unsigned components = glsl_get_components(var->type);

      switch (var->data.interpolation) {
      case INTERP_MODE_NONE:
         /* pco_rev_link_nir didn't run, probably xfb; override here. */
         var->data.interpolation = INTERP_MODE_SMOOTH;
         FALLTHROUGH;

      case INTERP_MODE_SMOOTH:
         if (f16)
            data->vs.f16_smooth += components;
         else
            data->vs.f32_smooth += components;

         break;

      case INTERP_MODE_FLAT:
         if (f16)
            data->vs.f16_flat += components;
         else
            data->vs.f32_flat += components;

         break;

      case INTERP_MODE_NOPERSPECTIVE:
         if (f16)
            data->vs.f16_npc += components;
         else
            data->vs.f32_npc += components;

         break;

      default:
         UNREACHABLE("");
      }
   }

   for (unsigned f16 = 0; f16 <= 1; ++f16) {
      for (enum glsl_interp_mode interp_mode = INTERP_MODE_SMOOTH;
           interp_mode <= INTERP_MODE_NOPERSPECTIVE;
           ++interp_mode) {
         try_allocate_vars(data->vs.varyings,
                           &data->vs.vtxouts,
                           nir,
                           &vars_mask,
                           nir_var_shader_out,
                           false,
                           f16,
                           interp_mode,
                           1);
      }
   }

   assert(!vars_mask);

   const gl_varying_slot last_slots[] = {
      VARYING_SLOT_PSIZ,       VARYING_SLOT_VIEWPORT,   VARYING_SLOT_LAYER,
      VARYING_SLOT_CLIP_DIST0, VARYING_SLOT_CLIP_DIST1,
   };

   for (unsigned u = 0; u < ARRAY_SIZE(last_slots); ++u) {
      try_allocate_var(data->vs.varyings,
                       &data->vs.vtxouts,
                       nir,
                       nir->info.outputs_written,
                       nir_var_shader_out,
                       last_slots[u],
                       1);
   }
}

static void pvr_alloc_fs_sysvals(pco_data *data, nir_shader *nir)
{
   BITSET_DECLARE(system_values_read, SYSTEM_VALUE_MAX);
   BITSET_COPY(system_values_read, nir->info.system_values_read);

   gl_system_value sys_vals[] = {
      SYSTEM_VALUE_FRONT_FACE,
   };

   for (unsigned u = 0; u < ARRAY_SIZE(sys_vals); ++u) {
      if (BITSET_TEST(system_values_read, sys_vals[u])) {
         nir_intrinsic_op op = nir_intrinsic_from_system_value(sys_vals[u]);
         unsigned dwords = nir_intrinsic_infos[op].dest_components;
         assert(dwords > 0);

         allocate_val(data->common.sys_vals,
                      &data->common.shareds,
                      sys_vals[u],
                      dwords);

         BITSET_CLEAR(system_values_read, sys_vals[u]);
      }
   }

   /* Clear built-in sysvals. */
   gl_system_value builtin_sys_vals[] = {
      SYSTEM_VALUE_SAMPLE_ID,
      SYSTEM_VALUE_LAYER_ID,
      SYSTEM_VALUE_SAMPLE_POS,
      SYSTEM_VALUE_BARYCENTRIC_LINEAR_CENTROID,
      SYSTEM_VALUE_BARYCENTRIC_LINEAR_COORD,
      SYSTEM_VALUE_BARYCENTRIC_LINEAR_PIXEL,
      SYSTEM_VALUE_BARYCENTRIC_LINEAR_SAMPLE,
      SYSTEM_VALUE_BARYCENTRIC_PERSP_CENTROID,
      SYSTEM_VALUE_BARYCENTRIC_PERSP_COORD,
      SYSTEM_VALUE_BARYCENTRIC_PERSP_PIXEL,
      SYSTEM_VALUE_BARYCENTRIC_PERSP_SAMPLE,
      SYSTEM_VALUE_BARYCENTRIC_PULL_MODEL,
   };

   for (unsigned u = 0; u < ARRAY_SIZE(builtin_sys_vals); ++u)
      BITSET_CLEAR(system_values_read, builtin_sys_vals[u]);

   check_unused_sysvals(system_values_read);
   assert(BITSET_IS_EMPTY(system_values_read));

   data->fs.meta = (pco_range){
      .start = data->common.shareds,
      .count = 1,
   };

   ++data->common.shareds;
}

static void pvr_alloc_fs_varyings(pco_data *data, nir_shader *nir)
{
   uint64_t vars_mask = nir->info.inputs_read &
                        BITFIELD64_RANGE(VARYING_SLOT_VAR0, MAX_VARYING);

   assert(!data->common.coeffs);

   /* Save the z/w locations. */
   unsigned zw_count = !!data->fs.uses.z + !!data->fs.uses.w;
   allocate_val(data->fs.varyings,
                &data->common.coeffs,
                VARYING_SLOT_POS,
                zw_count * ROGUE_USC_COEFFICIENT_SET_SIZE);

   /* If point coords are used, they come after z/w (if present). */
   nir_variable *var = nir_find_variable_with_location(nir,
                                                       nir_var_shader_in,
                                                       VARYING_SLOT_PNTC);
   if (var) {
      assert(!var->data.location_frac);
      unsigned count = glsl_get_components(var->type);
      assert(count == 2);

      allocate_var(data->fs.varyings,
                   &data->common.coeffs,
                   var,
                   ROGUE_USC_COEFFICIENT_SET_SIZE);

      data->fs.uses.pntc = true;
   }

   /* Allocate the rest of the input varyings. */
   try_allocate_vars(data->fs.varyings,
                     &data->common.coeffs,
                     nir,
                     &vars_mask,
                     nir_var_shader_in,
                     true,
                     false,
                     0,
                     ROGUE_USC_COEFFICIENT_SET_SIZE);
}

/* ---- end of copied code ---- */

#define MAX_SETS 4
#define MAX_BINDINGS 16

/* -i: every UBO is an inline uniform block (VK_EXT_inline_uniform_block): shared registers via PDS DMA. */
static bool opt_inline_ubo;

/* Descriptor layout, derived from the shaders' variables (the driver gets it from the set layouts). */
struct bind_info {
   bool present, img_smp, inline_ubo;
   unsigned stage_mask;
   unsigned stride_dw;
   unsigned offset_dw;
};
struct layout {
   struct bind_info b[MAX_SETS][MAX_BINDINGS];
   unsigned set_count, set_size_dw[MAX_SETS], set_stages[MAX_SETS];
};

/* Sizes in dwords: pvr_{image,sampler,combined_image_sampler,buffer}_descriptor in pvr_common.h. */
#define IMG_DW ((ROGUE_NUM_TEXSTATE_IMAGE_WORDS * 8 + PCO_IMAGE_META_COUNT * 4) / 4)
#define SMP_DW ((ROGUE_NUM_TEXSTATE_SAMPLER_WORDS * 16 + PCO_SAMPLER_META_COUNT * 4) / 4)

static void layout_add(struct layout *l, nir_shader *nir)
{
   nir_foreach_variable_in_shader (var, nir) {
      unsigned stride;
      bool smp = false;
      const struct glsl_type *t = glsl_without_array(var->type);
      bool inl = false;
      if (var->data.mode == nir_var_mem_ubo && opt_inline_ubo) {
         inl = true;
         stride = glsl_get_explicit_size(t, false) / 4;
      } else if (var->data.mode == nir_var_mem_ubo || var->data.mode == nir_var_mem_ssbo) {
         stride = 4;
      } else if (var->data.mode == nir_var_uniform && glsl_type_is_sampler(t)) {
         stride = IMG_DW + SMP_DW;
         smp = true;
      } else if (var->data.mode == nir_var_image || (var->data.mode == nir_var_uniform && glsl_type_is_texture(t))) {
         if (glsl_get_sampler_dim(t) == GLSL_SAMPLER_DIM_SUBPASS) {
            fprintf(stderr, "input attachments are not modelled\n");
            exit(2);
         }
         stride = IMG_DW;
      } else {
         continue;
      }
      assert(var->data.descriptor_set < MAX_SETS && var->data.binding < MAX_BINDINGS);
      struct bind_info *bi = &l->b[var->data.descriptor_set][var->data.binding];
      bi->present = true;
      bi->img_smp = smp;
      bi->inline_ubo = inl;
      bi->stride_dw = stride * MAX2(1, glsl_type_is_array(var->type) ? glsl_get_aoa_size(var->type) : 1);
      bi->stage_mask |= BITFIELD_BIT(nir->info.stage);
      l->set_stages[var->data.descriptor_set] |= BITFIELD_BIT(nir->info.stage);
      l->set_count = MAX2(l->set_count, var->data.descriptor_set + 1);
   }
}

static void layout_finish(struct layout *l)
{
   for (unsigned s = 0; s < l->set_count; s++)
      for (unsigned b = 0; b < MAX_BINDINGS; b++)
         if (l->b[s][b].present) {
            l->b[s][b].offset_dw = l->set_size_dw[s];
            l->set_size_dw[s] += l->b[s][b].stride_dw;
         }
}

static void init_descriptors(void *mem_ctx, pco_data *data, nir_shader *nir, const struct layout *l)
{
   for (unsigned s = 0; s < l->set_count; s++) {
      if (!(BITFIELD_BIT(nir->info.stage) & l->set_stages[s]))
         continue;
      pco_descriptor_set_data *d = &data->common.desc_sets[s];
      d->binding_count = MAX_BINDINGS;
      d->bindings = rzalloc_array_size(mem_ctx, sizeof(*d->bindings), MAX_BINDINGS);
      for (unsigned b = 0; b < MAX_BINDINGS; b++) {
         d->bindings[b].is_img_smp = l->b[s][b].img_smp;
         d->bindings[b].is_inline_ubo = l->b[s][b].inline_ubo;
      }
   }
}

/* Mirrors pvr_setup_descriptors() for sets without dynamic buffers. */
static void setup_descriptors(pco_data *data, nir_shader *nir, const struct layout *l)
{
   data->common.shareds = ALIGN_POT(data->common.shareds, 4);

   for (unsigned s = 0; s < l->set_count; s++) {
      pco_descriptor_set_data *d = &data->common.desc_sets[s];
      if (!(BITFIELD_BIT(nir->info.stage) & l->set_stages[s]) || !d->used)
         continue;
      d->range = (pco_range){ .start = data->common.shareds, .count = l->set_size_dw[s] };
      data->common.shareds += l->set_size_dw[s];
      for (unsigned b = 0; b < MAX_BINDINGS; b++) {
         if (!l->b[s][b].present)
            continue;
         d->bindings[b].range = (pco_range){
            .start = d->range.start + l->b[s][b].offset_dw,
            .count = l->b[s][b].stride_dw,
            .stride = l->b[s][b].stride_dw,
         };
      }
   }

   if (data->common.push_consts.used > 0) {
      /* The driver sizes this from the layout's push ranges; a full 128 byte range here. */
      unsigned count = data->common.push_consts.used == ~0U ? 32 : data->common.push_consts.used;
      data->common.push_consts.range = (pco_range){ .start = data->common.shareds, .count = count };
      data->common.shareds += count;
   }
   if (data->common.uses.point_sampler) {
      data->common.shareds = ALIGN_POT(data->common.shareds, 4);
      data->common.point_sampler = (pco_range){ .start = data->common.shareds, .count = ROGUE_NUM_TEXSTATE_DWORDS };
      data->common.shareds += ROGUE_NUM_TEXSTATE_DWORDS;
   }
   if (data->common.uses.ia_sampler) {
      data->common.shareds = ALIGN_POT(data->common.shareds, 4);
      data->common.ia_sampler = (pco_range){ .start = data->common.shareds, .count = ROGUE_NUM_TEXSTATE_DWORDS };
      data->common.shareds += ROGUE_NUM_TEXSTATE_DWORDS;
   }
   data->common.spill_info = (pco_range){ .start = data->common.shareds, .count = 3 };
   data->common.shareds += 3;
   if (data->common.scratch) {
      data->common.scratch_info = (pco_range){ .start = data->common.shareds, .count = 3 };
      data->common.shareds += 3;
   }
   /* The promoted UBO ranges, last (pvr_setup_descriptors). */
   if (data->common.ubo_range_count)
      data->common.shareds = ALIGN_POT(data->common.shareds, 4);
   for (unsigned u = 0; u < data->common.ubo_range_count; u++) {
      data->common.ubo_ranges[u].start = data->common.shareds;
      data->common.shareds += data->common.ubo_ranges[u].count;
   }
}

static void spirv_debug(void *priv, enum nir_spirv_debug_level level, size_t off, const char *msg)
{
   if (level >= NIR_SPIRV_DEBUG_LEVEL_WARNING)
      fprintf(stderr, "SPIR-V offset %zu: %s\n", off, msg);
}

/* vk_spirv_to_nir() (src/vulkan/runtime/vk_nir.c) without the vk_device: same options and passes.
 * The capabilities are the Vulkan 1.0 core ones the driver's features turn on; a shader using another
 * one fails to translate instead of silently compiling differently. */
static nir_shader *load(const char *path, mesa_shader_stage stage, void *mem_ctx)
{
   FILE *f = fopen(path, "rb");
   if (!f) {
      perror(path);
      exit(1);
   }
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   uint32_t *buf = malloc(n);
   if (fread(buf, 1, n, f) != (size_t)n)
      exit(1);
   fclose(f);

   struct spirv_capabilities caps = {
      .Matrix = true, .Shader = true, .InputAttachment = true, .Sampled1D = true, .Image1D = true,
      .SampledBuffer = true, .ImageBuffer = true, .ImageQuery = true, .DerivativeControl = true,
      .ImageGatherExtended = true, .StorageImageExtendedFormats = true, .ClipDistance = true,
      .CullDistance = true, .MinLod = true, .Linkage = true,
   };
   struct spirv_to_nir_options opts = *pco_spirv_options();
   opts.capabilities = &caps;
   opts.debug.func = spirv_debug;

   nir_shader *nir = spirv_to_nir(buf, n / 4, NULL, stage, "main", &opts, pco_nir_options());
   free(buf);
   if (!nir) {
      fprintf(stderr, "%s: spirv_to_nir failed\n", path);
      exit(1);
   }
   ralloc_steal(mem_ctx, nir);
   NIR_PASS(_, nir, nir_lower_variable_initializers, nir_var_function_temp);
   NIR_PASS(_, nir, nir_lower_returns);
   NIR_PASS(_, nir, nir_inline_functions);
   NIR_PASS(_, nir, nir_opt_copy_prop);
   NIR_PASS(_, nir, nir_opt_constant_folding);
   NIR_PASS(_, nir, nir_opt_deref);
   nir_remove_non_cmat_call_entrypoints(nir);
   NIR_PASS(_, nir, nir_lower_variable_initializers, ~0);
   NIR_PASS(_, nir, nir_split_var_copies);
   NIR_PASS(_, nir, nir_split_per_member_structs);
   /* ponytail: the runtime keeps xfb outputs (nir_vk_is_not_xfb_output); no xfb in the corpus. */
   nir_remove_dead_variables_options dvo = { 0 };
   NIR_PASS(_, nir, nir_remove_dead_variables,
            nir_var_shader_in | nir_var_shader_out | nir_var_system_value | nir_var_shader_call_data |
               nir_var_ray_hit_attrib, &dvo);
   nir_gather_clip_cull_distance_sizes_from_vars(nir);
   NIR_PASS(_, nir, nir_merge_clip_cull_distance_vars);
   if (nir->info.stage == MESA_SHADER_VERTEX)
      nir_shader_gather_xfb_info(nir);
   NIR_PASS(_, nir, nir_propagate_invariant, false);
   return nir;
}

/* pvr_init_vs_attribs(): the vertex input state's formats. Here: every input fetched as 32 bits per component of the
 * type the shader declares (float/uint/int, 1 to 4 components), the usual layout of a port's vertex buffers. */
static enum pipe_format attrib_format(const struct glsl_type *t)
{
   static const enum pipe_format f[3][4] = {
      { PIPE_FORMAT_R32_FLOAT, PIPE_FORMAT_R32G32_FLOAT, PIPE_FORMAT_R32G32B32_FLOAT, PIPE_FORMAT_R32G32B32A32_FLOAT },
      { PIPE_FORMAT_R32_UINT, PIPE_FORMAT_R32G32_UINT, PIPE_FORMAT_R32G32B32_UINT, PIPE_FORMAT_R32G32B32A32_UINT },
      { PIPE_FORMAT_R32_SINT, PIPE_FORMAT_R32G32_SINT, PIPE_FORMAT_R32G32B32_SINT, PIPE_FORMAT_R32G32B32A32_SINT },
   };
   unsigned k = glsl_type_is_integer(t) ? (glsl_get_base_type(t) == GLSL_TYPE_UINT ? 1 : 2) : 0;
   return f[k][glsl_get_vector_elements(t) - 1];
}

/* Output formats the harness knows (colour attachment 0), and the size of its output register block. */
static const struct {
   const char *name;
   enum pipe_format fmt;
   unsigned bytes;
} formats[] = {
   { "rgba8", PIPE_FORMAT_R8G8B8A8_UNORM, 4 },
   { "rgba16f", PIPE_FORMAT_R16G16B16A16_FLOAT, 8 },
   { "rgba32f", PIPE_FORMAT_R32G32B32A32_FLOAT, 16 },
};

struct counts {
   unsigned smp, smp_pplod, smp_f16, smp_raw, smp_bias, smp_grad, smp_replace, itrsmp, itr, ld, wdf;
};

/* Counts from the grouped IR text: one op per mnemonic, mods are the .suffixes. */
static void count_ops(const char *text, struct counts *c)
{
   for (const char *p = text; *p; p++) {
      if (p != text && (isalnum((unsigned char)p[-1]) || p[-1] == '_' || p[-1] == '.'))
         continue;
      const char *e = p;
      while (isalnum((unsigned char)*e) || *e == '.' || *e == '_')
         e++;
      size_t n = e - p;
      if (!n)
         continue;
      if (n >= 3 && !strncmp(p, "smp", 3) && (n == 3 || p[3] == '.')) {
         c->smp++;
         char tok[128] = { 0 };
         memcpy(tok, p, MIN2(n, sizeof(tok) - 1));
         strcat(tok, ".");
         c->smp_pplod += !!strstr(tok, ".pplod.");
         c->smp_f16 += !!strstr(tok, ".f16.");
         c->smp_raw += !!strstr(tok, ".rawdata.") || !!strstr(tok, ".both.");
         c->smp_bias += !!strstr(tok, ".bias.");
         c->smp_grad += !!strstr(tok, ".gradients.");
         c->smp_replace += !!strstr(tok, ".replace.");
      } else if (n >= 6 && !strncmp(p, "itrsmp", 6)) {
         c->itrsmp++;
      } else if (n == 2 && !strncmp(p, "ld", 2)) {
         c->ld++;
      } else if (n == 3 && !strncmp(p, "wdf", 3)) {
         c->wdf++;
      } else if ((n >= 4 && !strncmp(p, "fitr", 4)) || (n >= 4 && !strncmp(p, "ditr", 4))) {
         c->itr++;
      }
      p = e - 1;
   }
}

int main(int argc, char **argv)
{
   unsigned fmt = 0;
   bool robust = false, dump = false, blend = false;
   unsigned nrt = 1;
   int i = 1;
   for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
      if (!strcmp(argv[i], "-f") && i + 1 < argc) {
         for (fmt = 0; fmt < ARRAY_SIZE(formats) && strcmp(argv[i + 1], formats[fmt].name); fmt++)
            ;
         if (fmt == ARRAY_SIZE(formats))
            goto usage;
         i++;
      } else if (!strcmp(argv[i], "-r")) {
         robust = true;
      } else if (!strcmp(argv[i], "-b")) {
         blend = true;
      } else if (!strcmp(argv[i], "-n") && i + 1 < argc) {
         nrt = atoi(argv[++i]);
         if (nrt < 1 || nrt > 4)
            goto usage;
      } else if (!strcmp(argv[i], "-i")) {
         opt_inline_ubo = true;
      } else if (!strcmp(argv[i], "-d")) {
         dump = true;
      } else {
         goto usage;
      }
   }
   if (argc - i != 2 && argc - i != 3) {
usage:
      fprintf(stderr,
              "usage: pco_stats [-f rgba8|rgba16f|rgba32f] [-b] [-n rts] [-r] [-i] [-d] <name> <vs.spv|-> <fs.spv>\n"
              "  one line per shader on stdout; -b src-alpha/one-minus-src-alpha blend on every attachment, -n number of identical colour attachments (1..4), -r robustBufferAccess, -i UBOs are inline uniform blocks, -d also print the IR to stderr\n");
      return 1;
   }
   const char *name = argv[i], *vs_path = argv[i + 1], *fs_path = argv[i + 2];
   (void)name;

   void *mem_ctx = ralloc_context(NULL);
   struct pvr_device_info dev_info;
   if (!pvr_device_info_init_public_name(&dev_info, "GE8320")) {
      fprintf(stderr, "no GE8320 device info\n");
      return 1;
   }
   /* Runtime info is only used by compute (cdm_max_local_mem_size_regs). */
   struct pvr_device_runtime_info rt = { .core_count = 1 };
   pco_ctx *ctx = pco_ctx_create(&dev_info, &rt, mem_ctx);
   pco_ctx_setup_usclib(ctx, pco_usclib_0_nir, sizeof(pco_usclib_0_nir));

   nir_shader *nirs[MESA_SHADER_STAGES] = { 0 };
   pco_data data[MESA_SHADER_STAGES] = { 0 };
   if (strcmp(vs_path, "-"))
      nirs[MESA_SHADER_VERTEX] = load(vs_path, MESA_SHADER_VERTEX, mem_ctx);
   nirs[MESA_SHADER_FRAGMENT] = load(fs_path, MESA_SHADER_FRAGMENT, mem_ctx);

   for (unsigned s = 0; s < MESA_SHADER_STAGES; s++) {
      if (!nirs[s])
         continue;
      data[s].common.robust_buffer_access = robust; /* pvr_early_init_shader_data() */
      pco_preprocess_nir(ctx, nirs[s], &data[s]);
   }

   nir_shader *vs = nirs[MESA_SHADER_VERTEX], *fs = nirs[MESA_SHADER_FRAGMENT];
   if (vs) {
      pco_link_nir(ctx, vs, fs, &data[MESA_SHADER_VERTEX], &data[MESA_SHADER_FRAGMENT]);
      pco_rev_link_nir(ctx, vs, fs);
   }

   struct layout layout = { 0 };
   for (unsigned s = 0; s < MESA_SHADER_STAGES; s++)
      if (nirs[s])
         layout_add(&layout, nirs[s]);
   layout_finish(&layout);

   for (unsigned s = 0; s < MESA_SHADER_STAGES; s++) {
      if (!nirs[s])
         continue;
      nir_shader *nir = nirs[s];
      pco_data *d = &data[s];

      /* pvr_preprocess_shader_data(), render pass path, state: 1 sample, 1 colour attachment, no blend,
       * no dynamic state, no pSampleMask, no colour write enables. */
      if (s == MESA_SHADER_FRAGMENT) {
         pco_fs_data *f = &d->fs;
         f->z_replicate = ~0u;
         const nir_lower_blend_channel replace = {
            .func = PIPE_BLEND_ADD,
            .src_factor = PIPE_BLENDFACTOR_ONE,
            .dst_factor = PIPE_BLENDFACTOR_ZERO,
         };
         const nir_lower_blend_channel over = {
            .func = PIPE_BLEND_ADD,
            .src_factor = PIPE_BLENDFACTOR_SRC_ALPHA,
            .dst_factor = PIPE_BLENDFACTOR_INV_SRC_ALPHA,
         };
         /* nrt identical attachments (-n), each with the same blend state. */
         for (unsigned rt = 0; rt < nrt; rt++) {
            f->output_formats[FRAG_RESULT_DATA0 + rt] = formats[fmt].fmt;
            f->blend_opts.rt[rt].format = formats[fmt].fmt;
            f->blend_opts.rt[rt].rgb = blend ? over : replace;
            f->blend_opts.rt[rt].alpha = blend ? over : replace;
            f->blend_opts.rt[rt].colormask = 0xf;
         }
         f->uses.alpha_to_coverage = false;

         f->meta_is_static = true;
         f->meta_static = (BITFIELD_MASK(PVR_FS_META_SAMPLE_MASK_LENGTH) << PVR_FS_META_SAMPLE_MASK_OFFSET) |
                          (BITFIELD_MASK(PVR_FS_META_COLOR_WRITE_ENABLE_LENGTH) << PVR_FS_META_COLOR_WRITE_ENABLE_OFFSET);
      }
      if (s == MESA_SHADER_VERTEX)
         nir_foreach_shader_in_variable (var, nir)
            d->vs.attrib_formats[var->data.location] = attrib_format(var->type);
      init_descriptors(mem_ctx, d, nir, &layout);

      pco_lower_nir(ctx, nir, d);
      pco_postprocess_nir(ctx, nir, d);

      /* pvr_postprocess_shader_data() */
      if (s == MESA_SHADER_VERTEX) {
         pvr_alloc_vs_sysvals(d, nir);
         pvr_alloc_vs_attribs(d, nir);
         pvr_alloc_vs_varyings(d, nir);
      } else {
         pvr_alloc_fs_sysvals(d, nir);
         pvr_alloc_fs_varyings(d, nir);
         /* pvr_setup_fs_outputs(): output register 0 */
         for (unsigned rt = 0; rt < nrt; rt++) {
            nir_variable *var = nir_find_variable_with_location(nir, nir_var_shader_out, FRAG_RESULT_DATA0 + rt);
            if (var)
               set_var(d->fs.outputs, rt * DIV_ROUND_UP(formats[fmt].bytes, 4), var, DIV_ROUND_UP(formats[fmt].bytes, 4));
         }
         /* pvr_setup_fs_blend(), pvr_setup_fs_sample_locations() */
         unsigned nbc = util_bitcount(d->fs.blend_consts_needed);
         if (nbc) {
            d->fs.blend_consts = (pco_range){ .start = d->common.shareds, .count = nbc };
            d->common.shareds += nbc;
         }
         if (d->fs.uses.sample_locations) {
            d->fs.sample_locations = (pco_range){ .start = d->common.shareds, .count = 4 };
            d->common.shareds += 4;
         }
      }
      setup_descriptors(d, nir, &layout);
   }

   for (unsigned s = 0; s < MESA_SHADER_STAGES; s++) {
      if (!nirs[s])
         continue;
      pco_shader *sh = pco_trans_nir(ctx, nirs[s], &data[s], mem_ctx);
      if (!sh) {
         fprintf(stderr, "%s: pco_trans_nir failed\n", s == MESA_SHADER_VERTEX ? vs_path : fs_path);
         return 1;
      }
      pco_process_ir(ctx, sh);
      pco_encode_ir(ctx, sh);

      char *text = NULL;
      size_t tsz = 0;
      FILE *mf = open_memstream(&text, &tsz);
      pco_print_shader(sh, mf, NULL);
      fclose(mf);
      if (dump)
         fputs(text, stderr);
      struct counts c = { 0 };
      count_ops(text, &c);
      free(text);

      struct pvr_stats st = pco_get_pvr_stats(sh);
      const struct rogue_stats *r = &st.rogue;
      printf("%s %s code_size=%u igrps=%u main=%u bitwise=%u control=%u temps=%u vtxins=%u spills=%u scratch=%u loops=%u "
             "smp=%u smp_pplod=%u smp_f16=%u smp_raw=%u smp_bias=%u smp_grad=%u smp_replace=%u itrsmp=%u itr=%u ld=%u wdf=%u shareds=%u\n",
             name,
             s == MESA_SHADER_VERTEX ? "vs" : "fs",
             r->code_size, r->inst_group_count, r->main_inst_group_count, r->bitwise_inst_group_count,
             r->control_inst_group_count, r->temp_count, r->vtxin_count, r->spill_count, r->scratch_size,
             r->loop_count, c.smp, c.smp_pplod, c.smp_f16, c.smp_raw, c.smp_bias, c.smp_grad, c.smp_replace, c.itrsmp,
             c.itr, c.ld, c.wdf, data[s].common.shareds);
   }
   ralloc_free(mem_ctx);
   return 0;
}
