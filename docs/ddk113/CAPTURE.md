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

## Evidence 3: runtime captures (rig working, field decoding in progress)

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

## Evidence 4: static disassembly (pending)

`llvm-objdump -d` of the vendor GLES and services libraries (ARM32), at the sites that fill the command blobs.
This is done locally for interoperability, and only the resulting field offsets are recorded.

## Field tables

The decoded tables go here, one per struct, each with its evidence column.
