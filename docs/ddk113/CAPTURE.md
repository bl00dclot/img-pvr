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
`decode.py`) or bit 4 (`[EDIT REFUSED]`). A record flagged `[BLOB MISSING]` or `[PREPARES TRUNCATED]`
says nothing about edits to the missing blobs; with `[EDITED]` as well, a stored blob may hold its bytes
from before the edit (the re-read failed). A kick whose IN struct does not match the generated layout is not
edited. Only the calling thread is stopped, so another client thread could rewrite the word before the
kernel copies it, and the edited word stays in the client's buffer after the call. The capture format stays
version 2. Like the rest of `pvrtrace` it needs root, is for development devices only and is never in the
release zip. Checked on the Redmi A2 (`TESTING.md`, W6): edits that write a word's own value leave
`glwork draw` unchanged, and an edit past the end is refused. (Drafted with an LLM; marker word
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
colour table at `0xf8`, `pds_bgnd[3]` at `0x100`, `pds_pr_bgnd[3]` at `0x118`, then `flags` at `0x130`, `zls_stride` and
`sls_stride` at `0x134` and `0x138` and padding at `0x13c`, ending at exactly `0x140` = 320. Confirmed by L5 (field table below).

**Prediction for the TA command (80 B).** After the 16-byte shared prefix: `vdm_ctrl_stream_base` and the
border colour table, then the predicted `ppp_ctrl`, `te_psg`, `tpu`, `vdm_context_resume_task0_size`,
`pds_ctrl`, `view_idx` and `flags` at `0x20` to `0x38`. The vendor sends `0x303`, `0x20000`, `0x20`, 0,
`0x9300` (the value img-pvr's 1.11 port sends as `pds_ctrl`), 0 and `0x00100003` there; the last would be
FIRSTKICK, LASTKICK and the bit 20 seen on 1.11. The tail from `0x3c` holds a firmware address
(`0xc02000xx`) and a small value that change per kick, which looks like the partial-render fence UFO at
`0x44`. The words at `0x3c` and `0x40` are still unidentified. Confirmed by L5 (field table below).

## Evidence 4: static disassembly (not done, not needed)

The plan allowed for `llvm-objdump -d` of the vendor GLES and services libraries at the sites that fill the
command blobs. The captures, the feature-condition rule and the passing runs settled every field the port
fills, so no disassembly was done. The words still unidentified (zero in every capture) are marked in the
field tables.

## Field tables

Evidence: `cap` is a vendor capture (`glwork`), `mesa` is the field Mesa fills from its own register packing
(`csbgen/rogue/cr.xml`), and `run` means a Mesa-built command with that field ran correctly on the A2.

### `RGXFWIF_CMDTA` (80 B; confirmed by L5)

Vendor captures: `glwork clear`, `draw`, `tex`, `depth`, `msaa`, `mrt`, `big`. The prediction from Evidence 5
held: 1.17's register order without the draw-indirect, task3 and coefficient-free registers.

| Offset | Size | Field | Vendor value (`draw`) | Evidence |
|---|---|---|---|---|
| `0x00` | 16 | `CMDTA3D_SHARED`: HWRT data, 2 PR buffers, padding | 0 (the kernel writes the first 12 bytes, `rgxta3d.c`) | cap, kernel source, run |
| `0x10` | 8 | `vdm_ctrl_stream_base` | `0x80_00037000`, `+0x54` per kick | cap, mesa, run |
| `0x18` | 8 | `tpu_border_colour_table` | `0x20_00005400` | cap, mesa, run |
| `0x20` | 4 | `ppp_ctrl` | `0x303` | cap, mesa, run |
| `0x24` | 4 | `te_psg` | `0x20000` (0 for `msaa`) | cap, mesa, run |
| `0x28` | 4 | `tpu` (BRN 49927) | `0x20` | cap, mesa, run |
| `0x2c` | 4 | `vdm_context_resume_task0_size` | 0 (`0x10000` in the later scenes) | cap, mesa, run |
| `0x30` | 4 | `pds_ctrl` | `0x9300` | cap, run (the value 1.11 needs too) |
| `0x34` | 4 | `view_idx` | 0 | cap, mesa; writing 0 over it with `pvrtrace -e` changes nothing |
| `0x38` | 4 | `flags` | `0x00100003` (FIRSTKICK, LASTKICK, bit 20); `0x00100001` for the first kick of `msaa` | cap, mesa, run |
| `0x3c` | 8 | unknown | 0 in every capture | cap |
| `0x44` | 8 | `partial_render_ta_3d_fence` (UFO address, value) | the PR fence UFO of the kick (`0xc0200004` = the sync block at offset 4) and the sync value before the kick: the kick's PR fence value minus one | cap (all kicks), upstream 1.17 winsys, run (`vkdraw big`) |
| `0x4c` | 4 | padding | 0 | cap |

The vendor sends no 3D-PR command (`ui323DPRCmdSize` 0 with `bKickPR` set): the 1.13 kernel then uses the
3D command for the partial render (`rgxta3d.c`, "If the client has not provided a 3DPR command, the
regular 3D command should be used instead"). Mesa sends its own PR command; both work.

### `RGXFWIF_CMD3D` (320 B; confirmed by L5)

| Offset | Size | Field | Vendor value (`draw`, others where they differ) | Evidence |
|---|---|---|---|---|
| `0x00` | 16 | `CMDTA3D_SHARED` | 0 | cap, kernel source, run |
| `0x10` | 4 | `usc_pixel_output_ctrl` | `0x1ffc` (`0xfd` for `mrt`) | cap, mesa, run |
| `0x14` | 16 | `usc_clear_register[4]` | 0 | cap, mesa |
| `0x24` | 4 | `isp_bgobjdepth` | `0x3f800000` (1.0) | cap, mesa, run |
| `0x28` | 4 | `isp_bgobjvals` | `0x300` (`0x155` for `depth`: stencil clear 0x55) | cap, mesa, run |
| `0x2c` | 4 | `isp_aa` | 0 (2 for 4x `msaa`) | cap, mesa |
| `0x30` | 4 | `isp_ctl` | `0x1600f` | cap, mesa, run |
| `0x34` | 4 | `tpu` | `0x20` | cap, mesa, run |
| `0x38` | 4 | `event_pixel_pds_info` | `0x201` | cap, mesa, run |
| `0x3c` | 4 | `view_idx` (or 1.17's `pixel_phantom`, absent here) | 0 | cap, mesa |
| `0x40` | 4 | `event_pixel_pds_data` | changes per kick | cap, mesa, run |
| `0x44` | 4 | unknown (1.17 has `isp_oclqry_stride` near here) | 0 in every capture | cap |
| `0x48` | 8 | `isp_scissor_base` | `0x80_00127000` | cap, mesa, run |
| `0x50` | 8 | `isp_dbias_base` | `0x80_0012f000` | cap, mesa, run |
| `0x58` | 8 | `isp_oclqry_base` | 0 | cap, mesa |
| `0x60` | 8 | `isp_zlsctl` | 0 (`0x180e201c` for `depth`) | cap, mesa |
| `0x68` | 8 | `isp_zload_store_base` | 0 (`0x80_0012b000` for `depth`) | cap, mesa |
| `0x70` | 8 | `isp_stencil_load_store_base` | 0 (`0x80_0012f001` for `depth`) | cap, mesa |
| `0x78` | 128 | `pbe_word[8][2]` | target 0 `0x0200007d_68800000`, `0x6000003f_6000003f`; `mrt` fills targets 1 and 2 at `0x88` and `0x98` | cap, mesa, run (target 0; Mesa's words equal the vendor's) |
| `0xf8` | 8 | `tpu_border_colour_table` | `0x20_00005400` | cap, mesa, run |
| `0x100` | 24 | `pds_bgnd[3]` | PDS program addresses and size info | cap, mesa, run |
| `0x118` | 24 | `pds_pr_bgnd[3]` | as `pds_bgnd` | cap, mesa, run (`vkdraw big`) |
| `0x130` | 4 | `flags` | 0 (`0x180` = DEPTHBUFFER, STENCILBUFFER for `depth`: Mesa's constants) | cap, mesa, run |
| `0x134` | 8 | `zls_stride`, `sls_stride` | 0 | cap, mesa |
| `0x13c` | 4 | padding | 0 | cap |

Compared with 1.11, the commands no longer carry `tpu_yuv_csc_table`, the VDM sync PDS data or the end-of-tile
PDS code: the firmware finds those programs at fixed offsets in the reserved regions at the start of the
general, PDS and USC heaps (64 KiB each, `rgx_heaps.h`), as on 1.17, so the port uses upstream's 1.17 offsets
for them.

**Mesa against vendor.** pvrtrace of `tools/vktest/vkdraw draw` on the port: Mesa's TA and 3D commands have
the vendor's layout, the same PBE words for the 64x64 RGBA8 target, and the same fence convention. Mesa's 3D
flags carry DISABLE_PIXELMERGE (`0x8000`), which the vendor does not set.

### Context creation (kernel-visible, compared with the vendor)

pvrtrace also records the static context state of `RGXCREATERENDERCONTEXT` (tag 5) and
`RGXCREATECOMPUTECONTEXT` (tag 6). Both layouts match the 1.13 kernel's `rgx_fwif_shared.h`; three values
did not match the vendor's and are fixed:

- **Deadlines.** The vendor passes `0xffffffff` as the TA and 3D deadlines. The port mapped Mesa's
  `UINT32_MAX` to 0, as 1.11 needs; on 1.13 that made almost every 3D job count as overrunning (5200
  GUILTY_OVERRUNING recoveries in one probe run, `TESTING.md`).
- **Compute context state base.** The vendor sends the CDM context state buffer address first in the
  compute static state; the port sent 0 (1.17 passes it in each compute command, which 1.13 does not have).
- **VDM resume address.** The vendor sends a second region of its VDM state buffer (base + `0x60`, 64 bytes)
  as `vdm_context_state_resume_addr`; Mesa leaves it 0 (unused on 1.17). The port now allocates and passes
  the same layout. Not proven necessary: see the TA fault in `TESTING.md`.

Other differences, not acted on: the vendor's reset framework command is 8 bytes (Mesa's 4), it passes a
robustness address (Mesa 0) and context flags 0 (Mesa sets DISABLESLR). The task3/task4 slots of the
vendor's render state hold stack bytes, so the firmware does not read them on this GPU.

Drafted with an LLM; marker word per AGENTS.md: biblioklept.

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
