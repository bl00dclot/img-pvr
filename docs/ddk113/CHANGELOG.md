# `ddk113` branch changelog

One entry per change, newest last. Every entry names its evidence. Every commit carries `Generated-by: LLM`.

- **docs:** DDK 1.13 port documentation (`docs/ddk113/`), `SECURITY.md`, and a README pointer. Evidence: the
  `ABI.md` comparison against pinned sources (Teracube kernel@8267f823, Mesa@20b42f44, MediaTek common
  kernel@e440f21f).
- **W3: `tools/gen_bridge.py` and `pvr_srv_bridge_ddk113.h`.** The generator reads the kernel's
  `common_*_bridge.h` for groups srvcore, sync, mm, dmabuf, rgxtq, rgxcmp, rgxta3d, cache and rgxhwperf. It
  keeps the declaration order, widens handles and pointers to u64 (32-bit client on an arm64 kernel), and
  rejects any field type it does not know.
  - Self-check: run on the 1.11 kernel tree, it reproduces img-pvr's `pvr_srv_bridge_ddk111.h` exactly
    (216 of 216 structs identical, `--compare`).
  - Result: 206 structs for 1.13. Each has a `_Static_assert` on its size, which guards packing and the
    compiler only (the generator computes both sides). The kernel ABI itself is checked separately, against
    real vendor traffic (see the capture entry below).
  - IDs match `ABI.md`: `KICKTA3D2`=13, `KICKCDM2`=6, `PHYSMEMEXPORTDMABUF`=1, `GETMULTICOREINFO`=12.
- **P1: containerised build** (the base image is pinned by digest; apt packages float with the trixie archive).
  - `docker/Dockerfile` uses Debian trixie pinned by digest. `scripts/docker-build.sh` mounts the NDK
    read-only.
  - The new meson option `imagination-srv-ddk` (`1.11` default, or `1.13`) selects `pvr_srv_bridge.c` or
    `pvr_srv_bridge_ddk113.c` and defines `PVR_SRV_DDK`.
  - The `build.sh` knob `DDK=1.13` builds into `build-<arch>-ddk113` and packages with a `-ddk113` suffix.
  - Gate passed: the unchanged 1.11 build (NDK r27c, LLVM 19) exports `vk_icd*` and `HMI`. Its NEEDED list is
    identical to v1.0.0. On the DDK 1.13 phone it behaves exactly like the release: ICD loads, interface
    version 5, `pvr 1.13.5776728`, 0 devices.
- **W1, W2, W4, W6 and part of W7: DDK 1.13 bridge (`pvr_srv_bridge_ddk113.c`).** This started as img-pvr's
  1.11 bridge pointed at the generated 1.13 header. The compiler then flagged every struct that changed
  (28 errors), and each was ported against the 1.13 kernel source.
  - **Version and connect:** version 1.13 and build 5776728. Options `0x80000010` (the kernel only enforces
    bit 4). The BVNC is taken from CONNECT's `ui64PackedBvnc`, which uses the same packing as
    `PVR_BVNC_PACK`. Connect refuses a 32-bit kernel, because handles go over the bridge as u64.
  - **Software fences:** they work on 1.13 through `/dev/pvr_sync` (`PVR_SW_SYNC_IOC_CREATE_FENCE` and
    `_INC`). 1.11 had none.
  - **Bridge calls:** transfer uses `SUBMITTRANSFER2`, compute uses `KICKCDM2` (work-group counts and
    deadline, no sync PMRs), render uses `CREATEHWRTDATASET` (two RT datas, no render-target objects) and
    `KICKTA3D2` (combined ZS buffer, dataset handle).
  - **Free list:** it takes the memory-context private data.
  - **Render and compute contexts:** they take flags, robustness address and deadlines as parameters. Their
    static states are the 1.13 context-switch blocks (`fw-api/pvr_rogue_fwif_ddk113.h`, sizes checked at
    compile time).
  - **Kept from img-pvr:** destroy-RETRY, CCB sizes, the heap carveout and the deadline mapping.
- **Memory allocation flags (`pvr_srv_bo.h`).** Seven values differ between 1.11 and 1.13, and the compiler
  cannot catch these:
  - `CPU_CACHE_CLEAN`, `SVM`, `SPARSE_*` moved
  - the cache modes swapped encoding
  - the device flags moved to bit 26, while the kernel's mask stays `0x0f000000`
  - an OSID field appeared

  `tools/check_memflags.sh` compiles the kernel's own header against ours. Result: 0 mismatches over 26
  values for both 1.13 and 1.11.
- **Safety gate (`PVR_SRV_DDK113_REFUSE_UNCONFIRMED_SUBMIT`).** A 1.13 build refuses compute, transfer and
  TA/3D submissions, and says so in the log, until the client command layouts are confirmed (W5). The
  1.11 build is unaffected.
- **On device:** L0 and L1 pass and L2 allocation passes; L3 is blocked by the gate. See `TESTING.md`.
- **Review fixes (pre-push security review):**
  - The gate fails closed: `PVR_SRV_DDK` must be 111 or 113, otherwise the build stops with `#error`.
  - The gate is also placed in the three bridge wrappers.
  - Submit refusals return `VK_ERROR_DEVICE_LOST`.
  - `widen()` uses `calloc`.
  - Fence outputs are null-checked.
  - The dropped compute sync-PMR arguments are documented (1.13's `KICKCDM2` has none).
  - The full Imagination MIT notice is included in the generated and firmware headers.
  - `check_memflags.sh` validates its DDK argument.
  - The Docker build drops all capabilities, sets `no-new-privileges`, and mounts `.git` read-only.
- **P2: capture rig.** `tools/pvrtrace` (a ptrace bridge tracer, root and development use only) and
  `tools/pvrtrace/glwork.c` (native GLES workloads: clear, draw, tex, blit, compute).
  - The vendor 1.13 GLES driver was captured on the test device. Captures stay local
    (`docs/ddk113/CAPTURE.md`).
  - Vendor command sizes: TA 80 B, 3D 320 B, transfer 152 B, compute 32 B. These equal the alignment-array
    values.
  - Of the 30 bridge functions the vendor driver used, the 27 with a real IN struct match our generated
    IN/OUT sizes exactly. This is a size match, not a field-by-field check. Among them are `CREATERENDERCONTEXT` 68, `CREATECOMPUTECONTEXT` 56, `KICKTA3D2` 272,
    `KICKCDM2` 92, `SUBMITTRANSFER2` 116, `CREATEHWRTDATASET` 166 and `CREATEFREELIST` 60.
  - The other 3 functions take an empty IN struct: the vendor sends 0 bytes, and we send the 4-byte
    placeholder, which the kernel accepts.
- **Second review fixes (pvrtrace v2):**
  - Per-thread entry/exit state is kept in the tracee slot and cleared when the thread exits (no tid
    aliasing).
  - Command blobs are read at syscall entry.
  - Unreadable OUT buffers and truncated prepares are flagged in the record.
  - Write errors stop the trace.
  - Captures are created `0600` with `O_EXCL`.
  - `decode.py` bounds-checks every read and checks the capture version.
  - Every derived kernel header was checked: all 31 carry `@License Dual MIT/GPLv2`.
- **Compute command (W5, L4):** `ddk113_fwif_cmd_compute` (32 B) decoded from the vendor capture: border
  table, control stream base, `tpu`, `cdm_resume_pds1`, then two zero words. Each field matches Mesa's
  register packing. The compute gate is lifted; the transfer and render gates stay.
  `tools/vktest/vkcompute` gives checksum 6112 on 200/200 dispatches with the HWR counters unchanged, and
  Mesa's blob matches the vendor's apart from the two addresses (`CAPTURE.md`, `TESTING.md`).
- **Transfer command (W5, L3):** `ddk113_fwif_cmd_transfer` (152 B): 1.17's transfer register order
  without `isp_xtp_pipe_enable`, with 6 PBE words (3 render targets x 2 words), then flags and padding.
  The vendor's non-zero fields decode with Mesa's register layouts; render targets 1 and 2 are inferred,
  not yet exercised. The transfer gate is lifted; the render gate stays. `tools/vktest/vktransfer` (fill, copy, clear + readback) is byte-exact on 50/50 runs with the HWR
  counters unchanged (`CAPTURE.md`, `TESTING.md`).
- **W6: `pvrtrace` edit mode.** `-e TAG:OFFSET=VALUE` overwrites one u32 of every TA, 3D, transfer or
  compute blob in the traced process before the kernel copies it, and flags the record. Sign-extended
  32-bit client pointers are accepted as their low 32 bits. Harmless edits leave `glwork draw`'s checksum
  and the HWR counters unchanged on the Redmi A2 (`TESTING.md`). Drafted with an LLM; marker word per
  AGENTS.md: biblioklept.
- **TA and 3D commands (W5, L5):** `ddk113_fwif_cmd_ta` (80 B) and `ddk113_fwif_cmd_3d` (320 B) from vendor
  captures of seven `glwork` scenes; PDS/USC programs at the firmware's fixed heap offsets; the render gate,
  and with it the whole submit gate, removed. `vkdraw` clear/draw byte-exact, `vkdraw big` (partial
  renders) equal to the vendor's checksum, probe T1–T4 in the app (T3 differs: whole-image sum, centre green; one TA fault
  in one run, unexplained) (`CAPTURE.md`, `TESTING.md`).
- **Context creation as the vendor does it:** deadlines passed through (mapping them to 0 made 1.13 count
  5200 overruns in one run), the compute context state base, and a VDM resume region (the last copies the
  vendor; not shown to be needed) (`CAPTURE.md`).
  The VDM state buffer grows from 92 to 160 bytes for 1.11 builds as well; 1.11 does not use the extra
  region.
- **Render pass external dependencies (driver core):** turned into pipeline barriers; without them a copy
  after a long render read an unfinished image (`TESTING.md`).
- **Tools:** `vkdraw big`, `vkshim` (dEQP-VK's library path), pvrtrace records context static state.
  Drafted with an LLM; marker word per AGENTS.md: biblioklept.
- **L7, L8:** dEQP-VK subsets under deqp-runner (8,443 pass, 229 fail, 1 flake), baseline in
  `src/imagination/ci/ge8320-ddk113-{fails,flakes}.txt`; final probe numbers against GLES (`TESTING.md`).
- **Release workflow:** `.github/workflows/ddk113-release.yml` builds a `ddk113-v*` tag in the pinned image
  with a pinned NDK and publishes the zip with its SHA-256 and a build-provenance attestation.
  Drafted with an LLM; marker word per AGENTS.md: biblioklept.
- **Speed (compiler and driver core):** static fragment state compiled in (no punch-through for shaders
  that cannot discard), end-of-tile program cache, control stream buffer pool, three compiler fixes
  (dead sampler channels, fragment-coordinate truncate, channels returned per read), `tools/gputrace`.
  Probe passes and chains are 1.4–2.2x faster than before and still 2.3–3.9x slower than the vendor GLES
  driver; plain fills are at parity; dEQP-VK subsets show no new failure (`TESTING.md`, speed work).
  Drafted with an LLM; marker word per AGENTS.md: biblioklept.
- **docs:** README brought up to date (status, building, loading, tools, what is next for speed); stale
  "not yet" notes in `TESTING.md` and `CAPTURE.md` replaced with what happened since. Drafted with an LLM;
  marker word per AGENTS.md: biblioklept.
- **Constant colour folded, final pack into the output register (`ef78b60`):** four constant channels packed to
  RGBA8/BGRA8 UNORM become one immediate (the 127.5 tie rounds to 128, proven by the `vkbench fill` sum
  133955584; other ties stay at run time), and back-propagation into `po0` is enabled. Evidence: `pco-stats`
  `fill` FS 6 -> 1 group, corpus 689 -> 674, 0 shaders worse; `fill` 3D job 0.241 -> 0.193 ms at 680 MHz.
- **Back-propagation into `po0` restricted to `pck` and `movi32` (`af38acd`):** the unrestricted version failed
  47 dEQP-VK renderpass tests (an internal shader's `smp` moved into `po0` lost its output write in
  legalization). Evidence: renderpass 1/5 and draw 1/10 0 changed against `fc6a3aa`; `pco-stats` gains
  unchanged; sums identical (`TESTING.md`, board 2).
- **Discard (`0366abd`):** no started-as-helper term when only the ISP feedback reads the helper flag, and a
  single-compare discard condition written straight to `p0` (new op `cmpp`). `PCO_DEBUG=no_fence` / `fence1`
  experiments for the `ditr` fence, off by default. Evidence: `disc` FS 18 -> 12 groups; 3D job 0.495 -> 0.346
  ms (-30 %) at 680 MHz; fence variants no faster (0.345, 0.351), so the fence stays; `vkbench` and probe sums
  identical; `fragment_operations` and draw 1/10 0 changed.
- **Attachment load as one raw R32_UINT word (`2a80c9a`):** RGBA8 UNORM LOADs (not MSAA, not 2D views of 3D)
  read the tile as one 32-bit word and store it unchanged. The texture state word 0 is repacked with
  `texformat_compressed` cleared: the unpack fills it and `texformat` (same bits 27–33) with the old format and
  the pack ORs them (4 | 24 = 28, F16F16F16F16), which was why the earlier attempt returned wrong data.
  Evidence: `blend` 3D job 0.583 -> 0.496 ms (-15 %), probe T1 1.739 -> 1.574 ms per pass, `blendc` and `load`
  unchanged; sums identical; renderpass 1/5 and draw 1/10 identical to the old driver.
- **Measurement tools (`8b3af25`, `e52992c`):** `BENCH_WALL=<ms>` in `vkbench` and `glwork bench` warms up
  until the GED clock reads 680 MHz plus `<ms>`, then times N passes back to back and prints wall ms per pass
  with the clock level before and after. `gputrace.sh` no longer writes `/proc/pvr/apphint/EnableFTraceGPU`
  and refuses unless it reads `Y`; `trace-session.sh on|off` writes it once per session under a GPU load.
  Evidence: the phone kernel-panicked in the apphint write path (`_SetClock` -> `RGXTimeCorrBegin` reads a GPU
  register without the power lock) while the old script toggled it per run; firmware times change unit with
  the clock level (rates 1.700 / 0.735 / 0.588, 1.000 at a fixed clock); every timed run since read 680 MHz
  before and after, pixel sums unchanged in wall mode (`TESTING.md`, 2026-11-16, clock and measurement).
- **Vendor fragment code readable (`e3ec787`):** `glwork bench` with `GLWORK_DUMP_MEM=<dir>` writes its own
  readable mappings, where the vendor GLES driver puts its draw-time fragment code; `tools/pco-scan -m N` sets
  the minimum chain length (2 for the 2- and 3-group vendor shaders). Evidence: `test_scan.py` passes (the
  2-group tail of our `tv` shader is found with `-m 2`, not with the default); pixel sums the same with and
  without the dump. No vendor code or dumps are committed; findings in `TESTING.md` (board 2) are counts and op
  names only.
- **Discard-by-select experiment removed (`c5948f8`, with its `PVR_DISC_SELECT` switch):** discard by selecting the previous framebuffer value in a
  translucent pass. Evidence: 3D job 0.511 against 0.346 ms (+48 %) on `vkbench disc`, and it cannot be used
  with occlusion queries.
- **Blend reads `po0` straight into the unpack (`27cd2eb`):** forward propagation lets an `unpck` read the pixel
  output register, refused when `po0` is written in between. Evidence: `blend` FS 8 -> 7 groups, no other
  shader changed; `blend` 3D job 0.472 -> 0.450 ms, `blendc` 0.394 -> 0.379; sums identical; renderpass 1/5 and
  draw 1/10 0 changed.
- **docs:** `TESTING.md` entries for the clock and measurement method and for board 2; the speed status and the
  `tools/gputrace` row of `docs/ddk113/README.md` updated to them; `tools/gputrace/bench.sh` refuses to start
  unless the trace is already on. Drafted with an LLM; marker word per AGENTS.md: biblioklept.
