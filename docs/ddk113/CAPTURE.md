# Pinning down the DDK 1.13 command-stream layouts

Twelve of the firmware structures the winsys writes are command streams: TA, 3D, transfer and compute commands,
their register blocks, and the PR buffers. The kernel copies them into the client CCB verbatim (the first 12
bytes are the only part it patches), so the kernel source does not define them. This file records how the 1.13
layouts are established and what has been confirmed.

## Rules

- Raw captures and disassembly of vendor libraries are **never committed**. They contain proprietary register
  state and shader binaries. Only decoded field tables and sizes appear here.
- Every row names its evidence: `align` (the vendor alignment array), `kpatch` (a kernel source line),
  `capture` (a decoded vendor submission) or `disasm` (a store site in a vendor library).

## Evidence 1: the vendor alignment array (static)

The 1.13 user-mode services library sends a list of `sizeof` / `offsetof` values that the firmware compares
against its own (`RGXFW_ALIGN_CHECKS`; the kernel does the comparison in `KM:services/server/devices/rogue/rgxinit.c:1816-1880`).

- **Location:** a 43-entry u32 array, read as static data at file offset `0x1874c` of the vendor
  `libsrv_um.so` (1.13@5776728). The copy was pulled from the target phone and is not kept in this repository.
- **Decoding:** it decodes one-to-one against the
  `RGXFW_ALIGN_CHECKS_INIT0` list in `KM:include/rogue/rgx_fwif_alignchecks.h:62-127`. There is no TLA and no
  FASTRENDER_DM entry.

| Struct (1.13 name) | 1.13 size | Register block | 1.11 (img-pvr) | 1.17 (upstream) |
|---|---|---|---|---|
| `RGXFWIF_CMDTA` | **80** | `sGeomRegs` at 16 | 128, regs at 0 | 112, regs at 16 |
| `RGXFWIF_CMD3D` | **320** | `s3DRegs` at 16 | 328, regs at 0 | 488, regs at 16 |
| `RGXFWIF_CMDTRANSFER` | **152** | `sTransRegs` at 0 | 208, regs at 0 | 192, regs at 8 |
| `RGXFWIF_CMD_COMPUTE` | **32** | `sCDMRegs` at 0 | 64, regs at 0 | 96, regs at 8 |
| `RGXFWIF_FREELIST` | **56** | — | | |
| `RGXFWIF_HWRTDATA` | **384** | — | | |

The rest of the array covers `TRACEBUF`, `SYSDATA`, `OSDATA`, `HWRINFOBUF`, `HWRTDATA_COMMON` and the
`HWPERF_CTL*` blocks, which are firmware-internal.

**Status: unconfirmed against the firmware.** The array decodes cleanly against the kernel header's list,
but the firmware's own copy has not been compared yet. That comparison will come from a deliberately
all-zero `ALIGNMENTCHECK`, after which the kernel log prints the firmware values. Until then, treat these
sizes as strong hints, not as facts.

Reading: 1.13 is a hybrid. TA and 3D carry a 1.17-style 16-byte prefix (the 12-byte `CMDTA3D_SHARED`, padded).
Transfer and compute keep their registers at offset 0, as in 1.11. No struct matches either neighbour exactly,
so interpolation alone is not enough.

## Evidence 2: what the kernel does with the blobs (`kpatch`)

- **Patched region:** the kernel writes only `CMDTA3D_SHARED` into TA, 3D and 3D-PR commands:
  `sHWRTData`, `asPRBuffer[ZS]`, `asPRBuffer[MSAA]`
  (`KM:services/server/devices/rogue/rgxta3d.c:3495-3555`). The rest is copied as-is
  (`KM:services/server/devices/rogue/rgxccb.c:1936-1970`).
- **PR buffers:** there are two PR buffers in 1.13 (ZS and MSAA), against three in 1.11
  (`KM:include/rogue/rgx_fwif_shared.h:73-77`).
- **PR fence:** it is a separate `FENCE_PR` CCB command built by the kernel (`KM:services/server/devices/rogue/rgxta3d.c:3844`, `:4667-4683`),
  not a field in the blob.
- **Size limits:** a command must be at most 1024 B (`KM:generated/rogue/rgxta3d_bridge/server_rgxta3d_bridge.c:1542-1560`), and the CCB slot is
  `cmdsize + 40` rounded up to 8 (`KM:services/server/devices/rogue/rgxccb.c:1641`).
- **CCB command types and header:** identical to 1.11, with `TYPE_TA` (201) renamed `TYPE_GEOM`
  (`KM:services/include/rogue/rgx_fwif_km.h:1414-1447`).

## Evidence 3: runtime captures

The tool is `tools/pvrtrace`, a root-only, development-only tracer:

- It runs a program under `ptrace` (following its threads) and records every services bridge `ioctl`
  (`DRM_IOCTL_SRVKM_CMD`) on the PVR DRM node. Sync ioctls on `/dev/pvr_sync` are not traced.
- It decodes the bridge package, then reads the IN/OUT buffers and the command buffers they point to with
  `process_vm_readv`.

ptrace is used instead of `LD_PRELOAD` because vendor libraries load in the sphal linker namespace and may
bypass an interposer. Counts are cross-checked against `strace -f -e trace=ioctl`.

**Workloads:** `tools/pvrtrace/glwork.c` runs native GLES 3.1 jobs on the vendor driver (clear, textured
draw, texture upload, blit, compute), 3 iterations each, with a checksum. Results are decoded with
`tools/pvrtrace/decode.py`.

**First captures (2026-11-14, Redmi A2, vendor GLES 1.13@5776728).** The checksum is identical with and
without tracing, and 261–315 bridge calls were recorded per run.

| Command | Seen in modes | Size the vendor sends |
|---|---|---|
| TA (`KICKTA3D2` `psTACmd`) | clear, draw, tex | **80 B** |
| 3D (`KICKTA3D2` `ps3DCmd`) | clear, draw, tex | **320 B** (no 3D-PR command sent) |
| Transfer prepare (`SUBMITTRANSFER2`) | every mode (blit, readback) | **152 B**, 1 prepare per submit |
| Compute (`KICKCDM2` `psDMCmd`) | compute | **32 B** |

The vendor driver's sizes are consistent with the alignment array in Evidence 1. Both come from the vendor
user-mode stack, so they are not independent proof that the firmware accepts these layouts; only a
submission that runs correctly will be. No 3D-PR command appeared in these workloads. Field-level decoding
of the words is the next step. The capture format is version 2: blobs are read at syscall entry, and
records are flagged when the OUT buffer is unreadable or transfer prepares were truncated.

**Edit mode.** `pvrtrace -e TAG:OFFSET=VALUE` (up to 8, TAG one of `TA`, `3D`, `TQ`, `CDM`) overwrites one
u32 of every kick blob with that tag in the traced process, at syscall entry, before the kernel copies it.
It tests one field hypothesis per run against the known-good vendor driver running a `glwork` workload: if
the checksum, HWR counters or firmware state change, the firmware consumes that word; whether it means
what the prediction says needs a value chosen to tell the readings apart. Offsets below 16 in TA and 3D (the
prefix the kernel patches) are rejected, an edit past the end of a blob or whose write fails is refused with
a warning, and records carry the blob as re-read after the edit, with flag bit 3 (`[EDITED]` in
`decode.py`) or bit 4 (`[EDIT REFUSED]`). A record flagged `[BLOB MISSING]` says nothing about edits to
that blob. The capture format stays version 2. Like the rest of `pvrtrace` it needs root, is
for development devices only and is never in the release zip. **Untested on the device:** it has been
built and its option parser checked, but not yet run on the Redmi A2. (Drafted with an LLM; marker word
per AGENTS.md: biblioklept.)

## Evidence 5: feature-conditioned fields (prediction, checked against captures)

Upstream Linux's `drivers/gpu/drm/imagination/pvr_rogue_fwif_client.h` (the 1.17 firmware interface, dual
MIT/GPLv2) marks many command fields "only used when feature X present" or "only used when BRN Y present".
The 1.13 vendor commands are smaller than 1.17's because, on this DDK, those fields are compiled out when the
core lacks the feature. The GE8320's 1.13 kernel config (`KM: hwdefs/rogue/km/configs/rgxconfig_km_22.V.104.18.h`)
defines none of the features below, so the rule predicts every one of these fields absent. "Consistent"
means the captured size and decoded fields leave no room for the field; it is not a field-by-field proof:

| Feature (absent on GE8320) | Field predicted absent | Captured |
|---|---|---|
| `CDM_USER_MODE_QUEUE` | compute `cdm_cb_queue`, `cdm_cb_base`, `cdm_cb`, `stream_start_offset` | consistent (32 B, registers decoded) |
| `COMPUTE_MORTON_CAPABLE` | compute `cdm_item` | consistent (32 B) |
| `CLUSTER_GROUPING` | compute `compute_cluster`, 3D `pixel_phantom` | compute consistent (32 B); 3D prediction |
| `TPU_DM_GLOBAL_REGISTERS` | compute `tpu_tag_cdm_ctrl` | consistent (32 B) |
| `GPU_MULTICORE_SUPPORT` | compute and 3D `execute_count`, transfer `frag_screen` | compute consistent (32 B, but `0x1c` is unidentified); transfer ambiguous (zero word at `0x3c`); 3D prediction |
| `S7_TOP_INFRASTRUCTURE` | transfer and 3D `isp_xtp_pipe_enable` | transfer absent (`isp_aa` and `event_pixel_pds_info` decode at the 1.17 offsets minus 4); 3D prediction |
| `VDM_DRAWINDIRECT` | TA `vdm_draw_indirect0/1` | absent (prediction for TA) |
| `VDM_OBJECT_LEVEL_LLS` | TA `vdm_context_resume_task3_size` | absent (prediction for TA) |
| `TESSELLATION` | TA `pds_coeff_free_prog` | absent (prediction for TA) |
| `ZLS_SUBTILE`, `ISP_ZLS_D24_S8_PACKING_OGL_MODE`, `FBCDC_ALGORITHM`/`FB_CDC_V4` | 3D `isp_zls_pixels`, `rgx_cr_blackpearl_fix`, `fb_cdc_zls` | absent (prediction for 3D) |

BRN-conditioned fields cannot be predicted this way: the kernel core header lists only the kernel-relevant
BRNs. They are taken from the captures instead (BRN 49927's `tpu` is present in compute, TA and 3D, with the
value Mesa packs).

**Prediction for the 3D command, against the vendor draw (320 B).** Removing the absent fields from 1.17's
`rogue_fwif_frag_regs`, with 4 clear registers and 2 PBE words per target as in 1.11, gives 13 u32
registers from `0x10` to `0x43` (predicted `tpu` at `0x34`, where the vendor sends `0x20`;
`event_pixel_pds_info` at `0x38`; `view_idx` at `0x3c`; `event_pixel_pds_data` at `0x40`, where the vendor
word is the only one that changes per kick), a 4-byte slot at `0x44`, the six ISP
u64 bases from `0x48` (the vendor's scissor and depth-bias words are non-zero), `pbe_word[8][2]` at `0x78` (8 colour targets for
renders, against 3 for transfers), the border
colour table at `0xf8`, `pds_bgnd[3]` at `0x100`, `pds_pr_bgnd[3]` at `0x118`, then `flags`, `zls_stride`,
`sls_stride` and padding at `0x130`, ending at exactly `0x140` = 320. Not yet confirmed by a run.

**Prediction for the TA command (80 B).** After the 16-byte shared prefix: `vdm_ctrl_stream_base` and the
border colour table, then the predicted `ppp_ctrl`, `te_psg`, `tpu`, `vdm_context_resume_task0_size`,
`pds_ctrl`, `view_idx` and `flags` at `0x20` to `0x38`. The vendor sends `0x303`, `0x20000`, `0x20`, 0,
`0x9300` (the value img-pvr's 1.11 port sends as `pds_ctrl`), 0 and `0x00100003` there; the last would be
FIRSTKICK, LASTKICK and the bit 20 seen on 1.11. The tail from `0x3c` holds a firmware address
(`0xc02000xx`) and a small value that change per kick, which looks like the partial-render fence UFO at
`0x44`. The words at `0x3c` and `0x40` are still unidentified. Not yet confirmed by a run.

## Evidence 4: static disassembly (pending)

`llvm-objdump -d` of the vendor GLES and services libraries (ARM32), at the sites that fill the command blobs.
This is done locally for interoperability, and only the resulting field offsets are recorded.

## Field tables

Evidence: `cap` is a vendor capture (`glwork`), `mesa` is the field Mesa fills from its own register packing
(`csbgen/rogue/cr.xml`), and `run` means a Mesa-built command with that field ran correctly on the A2.

### `RGXFWIF_CMDTRANSFER` (152 B; confirmed by L3 for single-target fill, copy and clear)

Vendor captures: `glwork blit` (32x32 destination) and the 64x64 readbacks in every mode, 1 prepare per
submit, prepare flags as in 1.11 (`TQ_PREP_FLAGS_*` are unchanged in the 1.13 `rgx_tq_shared.h`). The
values below decode with Mesa's field layouts from `csbgen/rogue/cr.xml`. `run` is only claimed for fields
with a non-zero value in the passing L3 jobs: a zero field at a wrong offset would still pass.

| Offset | Size | Field | Vendor value (blit) | Evidence |
|---|---|---|---|---|
| `0x00` | 4 | `isp_bgobjvals` | `0x200` = `enablebgtag` | cap, mesa, run |
| `0x04` | 4 | `usc_pixel_output_ctrl` | `0x1ffc` = partition mask `0x3ff`, 4th partition | cap, mesa, run |
| `0x08` | 16 | `usc_clear_register0..3` | 0 (Mesa's clear puts the packed colour in register 0) | cap, mesa; run for register 0 only |
| `0x18` | 4 | `isp_mtile_size` | `0x00020002` for 32x32, `0x00040004` for 64x64 (tiles) | cap, mesa, run |
| `0x1c` | 4 | `isp_render_origin` | 0 | cap, mesa |
| `0x20` | 4 | `isp_ctl` | `0x26000` | cap, mesa, run |
| `0x24` | 4 | `isp_aa` | 0 | cap, mesa |
| `0x28` | 4 | `event_pixel_pds_info` | `0x202` = usc_sr_size 1, const_size 2 | cap, mesa, run |
| `0x2c` | 8 | `event_pixel_pds_code`, `_data` | PDS heap offsets | cap, mesa, run |
| `0x34` | 4 | `isp_render` | 3 | cap, mesa, run |
| `0x38` | 4 | `isp_rgn` | 0 (Mesa always packs 0 on this GPU) | cap, mesa |
| `0x3c` | 4 | unknown: 1.17's `frag_screen` or padding | 0 | cap |
| `0x40` | 8 | `pds_bgnd0_base` | shader and texunicode addresses | cap, mesa, run |
| `0x48` | 8 | `pds_bgnd1_base` | texturedata address, varying 0 | cap, mesa, run (texturedata) |
| `0x50` | 8 | `pds_bgnd3_sizeinfo` | `0x00800000_00010001` = sharedsize 1, texturestatesize 1, tempsize 1 | cap, mesa, run |
| `0x58` | 8 | `isp_mtile_base` | 0 | cap, mesa |
| `0x60` | 48 | `pbe_wordx_mrty[6]`: 3 render targets x 2 words | target 0 only: `0x0200007d_68800000`, `0x6000001f_6000001f` | cap, mesa, run for target 0; targets 1 and 2 inferred from the size and Mesa's indexing |
| `0x90` | 4 | `flags` | 0 | cap |
| `0x94` | 4 | padding | 0 | cap |

This is 1.17's `rogue_fwif_transfer_regs` order without `isp_xtp_pipe_enable` (S7 only) and without the
common header. 1.11 instead had a BGRND2 slot, a 64-bit `isp_render` and 9 PBE words. Mesa streams 9 PBE
words but fills only 2 per render target (`ROGUE_NUM_PBESTATE_REG_WORDS_FOR_TRANSFER`), so its 3 targets fit
the 6 slots and the last 3 stream words are always zero (asserted in the conversion).

**Mesa against vendor.** pvrtrace of `tools/vktest/vktransfer all` shows 4 prepares (fill, copy, clear,
image-to-buffer), all `eError=0`. The 64x64 image-to-buffer prepare matches the vendor's 64x64 readback word
for word apart from addresses, `pds_bgnd3_sizeinfo`'s shared size (2 against 1) and the PBE word 0
swizzle: the vendor writes RGBA (`swiz_chan0..3` = 0, 1, 2, 3), while Mesa writes the buffer as one 32-bit
channel (0, 5, 5, 4).

### `RGXFWIF_CMD_COMPUTE` (32 B; layout confirmed by L4, names of `0x18` and `0x1c` unconfirmed)

Vendor capture `glwork compute`: 3 kicks, one 64-invocation workgroup writing one SSBO. The words are identical
across the kicks apart from the control stream base, which advances by `0x90` per kick.

| Offset | Size | Field | Vendor value | Evidence |
|---|---|---|---|---|
| `0x00` | 8 | `tpu_border_colour_table` (`sCDMRegs` starts here, alignment array offset 0) | `0x20_00005400` | cap, mesa, run; same value as in the vendor TA command |
| `0x08` | 8 | `cdm_ctrl_stream_base` | `0x80_0012f000` + `0x90` per kick | cap, mesa, run |
| `0x10` | 4 | `tpu` (CR_TPU, written for BRN 49927) | `0x20` = `tag_cem_4k_face_packing` | cap, mesa (Mesa sets exactly this bit for BRN 49927), run |
| `0x14` | 4 | `cdm_resume_pds1` (CR_CDM_CONTEXT_PDS1) | `0x00900808`: data size 4, common size 1, common shared, unified size 4 | cap, mesa (Mesa's value is identical), run |
| `0x18` | 4 | flags? (1.17 has flags here) | 0 | cap; Mesa's prevent-all-overlap and single-core bits are not sent, as on 1.11 |
| `0x1c` | 4 | unknown (1.17 has `num_temp_regions` here) | 0 | cap; 1.11 needed a non-zero `num_temp_regions`, 1.13 runs with 0 |

Compared with 1.11 (64 B), `tpu_yuv_csc_table` is gone, `tpu` shrank to 32 bits, and the 20 trailing bytes
are dropped. Compared with 1.17, there is no common header, no user-mode-queue registers, and no
`cdm_context_state_base_addr` (1.13 keeps it in the static context state). The `KICKCDM2` arguments the
vendor sends are 0 work groups, 0 work items and a deadline of 0, which is what the port sends.

**Mesa against vendor.** A pvrtrace capture of `tools/vktest/vkcompute` on the port shows Mesa's
`KICKCDM2` blob as `00020000 00000020 000c6000 00000080 00000020 00900808 00000000 00000000`. Only the two
addresses differ from the vendor's.
