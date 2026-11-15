# DDK 1.13 bring-up: test ladder and results

Climb the ladder in order and stop at the first failure. At every failure, record three things here before
changing any code:

- the `/proc/pvr/debug_dump` lines (FW state, HWR counts)
- the tail of `/proc/pvr/firmware_trace`
- the logcat lines

Tests run first as a native binary from `/data/local/tmp` (as root, with no app sandbox). They are then
repeated inside an ordinary app: the ICD loaded with `dlopen` plus a stub `libhardware.so`.

| Step | Pass criterion | Status |
|---|---|---|
| L0 | `vkEnumeratePhysicalDevices` returns 1 device | **pass** (2026-11-14) |
| L1 | `vkCreateDevice` succeeds | **pass** (2026-11-14), render, compute and transfer contexts created |
| L2 | memory can be allocated, mapped and freed, and the heaps look sane | **pass**: allocation in the first run (2026-11-14); mapping, flushing and freeing by dEQP-VK `memory.*` (4,883 passes, L7) |
| L3 | transfer queue: clear and copy work, and the readback checksum matches | **pass for fill, copy and clear + readback** (2026-11-14, native root run): byte-exact, 50/50 runs, no HWR; image-to-image blits and copies run only under dEQP-VK `api.copy_and_blit` (L7: 174 failures there, mostly filtering precision) |
| L4 | compute: a trivial dispatch writes a buffer | **pass** (2026-11-14, native root run): 200/200 dispatches correct, no HWR |
| L5 | render: a clear, then a triangle, then probe tests T1–T4, with image checksums matching the GLES run | **pass** (2026-11-15): clear and triangle byte-exact 100/100, a 2-million-triangle scene with partial renders equal to the vendor's checksum; probe T1–T4 run inside the app, T2 and T4 sums equal GLES, T1 within 0.93 %, T3 differs (see the log) |
| L6 | 200 frames with no HWR increase in `/proc/pvr/debug_dump` | **pass with one exception** (2026-11-15): 12 x 200 probe frames per run, counters unchanged on 4 of 5 in-app runs after the deadline fix; one TA guilty lockup in one run (see the L5 log). `firmware_trace` is off on this kernel ("Debug log type: none") |
| L7 | a dEQP-VK subset passes under `deqp-runner`, with a committed baseline of known failures, flakes and skips (Mesa CI practice), image and shader logging off | **run, triaged** (2026-11-15/16): 7 subsets, 229 baseline failures at the first run; after the correctness lane 166 pass, 60 sit in `ge8320-ddk113-skips.txt` because they reset the GPU, 3 are expected failures; no GPU recovery in a run with the skips file; `src/imagination/ci/ge8320-ddk113-{fails,flakes,skips}.txt` |
| L8 | numbers against the GLES baseline | **done** (2026-11-15): after the speed work Vulkan is 2.3–3.9x slower than GLES on passes, draws and chains (3.7–6x before it), at parity on a plain fill, and 1.2–1.4x faster on uploads (last log entry) |

## Baseline before the port

The released img-pvr v1.0.0 (DDK 1.11) on a DDK 1.13 phone behaves as follows:

- the instance is created
- `vk_icdNegotiateLoaderICDInterfaceVersion` returns interface version 5
- **0 physical devices**

The render node opens and `DRM_IOCTL_VERSION` reports `pvr 1.13.5776728`. The device is then rejected by the
1.11-only version gate (`pvr_srv.c:695`).

## Results log

New entries go below, newest last.

### 2026-11-14: first run of the 1.13 build (L0–L2)

Setup: the Redmi A2 (kernel driver DDK 1.13@5776728). The driver was built from the `ddk113` branch with
`DDK=1.13 ARCHES=armv7 scripts/docker-build.sh` and loaded by `dlopen` inside an ordinary app
(targetSdk 35, `untrusted_app`), with a stub `libhardware.so`. The game was closed.

Results:

- **L0:** `VK device=PowerVR Rogue GE8320 api=1.3.363 driverVersion=0x6863063 vendor=0x1010 deviceID=0x22104018`.
  - CONNECT was accepted: DDK version 1.13, build 5776728, options `0x80000010`.
  - The kernel reported itself as 64-bit (`ui8KernelArch`, checked).
  - The BVNC was taken from CONNECT.
- **Queues and timestamps:** one queue family, `flags=0x7 count=2`, with `timestampValidBits=0`. That means no
  GPU timestamps, so timing has to be wall-clock.
- **L1:** `vkCreateDevice` succeeded. This created render, compute and transfer contexts, so the kernel
  accepted the 184-byte and 64-byte static states, the timelines on `/dev/pvr_sync` and the free-list setup.
- **SELinux:** the full system logcat for the run, read over a root adb shell (where `avc: denied` audit lines
  do appear: an earlier run showed the sysfs `uevent` denial), has no `avc: denied` lines. Context creation
  opened its timelines on `/dev/pvr_sync`, so on this device and ROM (Android 14 GSI on a MIUI 13 vendor,
  app targetSdk 35) the app sandbox can use `/dev/pvr_sync`. Other ROMs are untested.
- **L2:** memory has 1 heap of 1400 MiB and memtype 0 with flags `0x7` (device-local, host-visible,
  host-coherent). Image and buffer allocations succeed.
- **L3:** the first submission (a transfer) stops at the gate, as intended:
  `the transfer command layout is not confirmed yet, nothing submitted (docs/ddk113/CAPTURE.md)`.
- **GPU state:** the `/proc/pvr/debug_dump` HWR counters were identical before and after (TA 164, 3D 101; the
  phone's own history), and the firmware state was `OK`. This is weak evidence of GPU health, because the
  gate stopped the run before any GPU work was submitted. The `firmware_trace` tail was not recorded for
  this run, and the native root run (step 0 of this ladder) was skipped.

### 2026-11-14: compute (L4)

Setup: the compute gate lifted with the 1.13 compute command (`CAPTURE.md`, `RGXFWIF_CMD_COMPUTE`). Built
with `DDK=1.13 scripts/docker-build.sh`. `tools/vktest/vkcompute` (64 invocations, `v[i] = 3i + 1`, the same
job as `glwork compute`) was run as a native root binary from `/data/local/tmp`, with the game closed. The
transfer and render gates stay in place.

- **Before:** `RGX FW State: OK`, `Number of HWR: ... TA(164/164+0), 3D(101/101+0), CDM(0/0+0)`.
- **`vkcompute libvulkan_powervr.so 3`:** checksum 6112 (expected 6112) on all 3 iterations,
  `v[0..3] = 1 4 7 10`, exit 0.
- **`vkcompute libvulkan_powervr.so 200`:** `grep -c "checksum 6112 "` gives 200, exit 0.
- **After both runs:** the HWR line is identical (CDM 0, TA 164, 3D 101) and the FW state is still `OK`.
- **logcat:** only Mesa's startup notes (gralloc fallback, the `tpu_border_colour_enhanced` FINISHME). No
  errors.
- **Trace:** under pvrtrace the run made 170 bridge calls (0 dropped), 3 of them `KICKCDM2`, all `eError=0`.

Not done at the time for L4: the run inside the app sandbox, and the `firmware_trace` tail. Later: the in-app
compute check passes (L5 entry); `firmware_trace` is disabled on this kernel.

### 2026-11-14: transfer (L3)

Setup: the transfer gate lifted with the 1.13 transfer command (`CAPTURE.md`, `RGXFWIF_CMDTRANSFER`); the
render gate stays. Built with `DDK=1.13 scripts/docker-build.sh`. `tools/vktest/vktransfer` was run as a
native root binary from `/data/local/tmp`, with the game closed. Each job is its own submit, and every
word is checked on the host.

- **Before:** `RGX FW State: OK`, `TA(164/164+0), 3D(101/101+0), CDM(0/0+0)`.
- **`vktransfer ... fill`:** `0/4096 words wrong -> ok`.
- **`vktransfer ... copy`:** `0/4096 words wrong -> ok`.
- **`vktransfer ... clear`** (clear a 64x64 RGBA8 image, copy it to a buffer): `0/4096 words wrong -> ok`.
- **Stress:** `vktransfer ... all` 50 times in a row: 50/50 pass. `vkcompute ... 200` afterwards: 200/200.
- **After:** the HWR line is identical and the FW state is `OK`. No Mesa errors in logcat.
- **Trace:** 4 `SUBMITTRANSFER2` per `all` run, all `eError=0`, compared with the vendor in `CAPTURE.md`.

Not done at the time for L3 (later: the in-app probe uses transfers for every clear and readback, L5 entry;
blits ran under dEQP-VK, L7): the run inside the app sandbox, the `firmware_trace` tail, and blits between images
(`vkCmdBlitImage`, `vkCmdCopyImage`).

### 2026-11-15: `pvrtrace` edit mode (W6)

Setup: `pvrtrace` built for armv7 (`-Wall -Wextra -Werror -static`), run as root from `/data/local/tmp`
over the vendor GLES driver running `glwork draw`, with the game closed.

- **Self-test:** `pvrtrace --selftest` passes on the device, including the address checks below.
- **Found on the first run:** the edit-mode build recorded 0 bridge calls where the previous build recorded
  315. The 32-bit vendor driver sign-extends pointers into the u64 bridge fields (a stack buffer arrives as
  `0xffffffffffd09310`), and the new address check rejected every one. Such addresses are now accepted as
  their low 32 bits; anything else above 32 bits is still rejected.
- **No edits:** 315 calls, identical call list to the previous build's capture, checksum `2613248`.
- **Harmless edits** `-e TA:0x34=0 -e 3D:0x13c=0` (each word's own value) plus `-e TA:0x50=1` (past the end
  of the 80-byte TA blob): `3 applied, 0 refused` for both harmless edits, `0 applied, 3 refused` for the
  past-end one; checksum `2613248`; the three `KICKTA3D2` records decode as `[EDITED] [EDIT REFUSED]`.
- **HWR:** `TA(164/164+0), 3D(101/101+0), CDM(0/0+0)` before and after; `RGX FW State: OK`.

Not done at the time: an edit that changes a value. Later: used on Mesa's own kicks to test the
pixel-merge flag (speed work entry). Drafted with an LLM; marker
word per AGENTS.md: biblioklept.

### 2026-11-15: render (L5), partial renders, in-app probe

Setup: the render gate lifted with the 1.13 TA and 3D commands (`CAPTURE.md`), built with
`DDK=1.13 scripts/docker-build.sh`, game closed. Native runs as root from `/data/local/tmp`; the probe
runs inside the `local.vkprobe` app.

- **`tools/vktest/vkdraw clear` and `draw`** (64x64 RGBA8 render pass clear; a full-screen triangle):
  sum `2613248`, the same as `glwork draw` on the vendor driver, every pixel `0xffbf8040`; `all 50`:
  100/100 ok. HWR unchanged.
- **Mesa against vendor:** pvrtrace of `vkdraw draw` shows the vendor's TA and 3D layouts, identical PBE
  words and the same PR fence convention (`CAPTURE.md`).
- **`vkdraw big`** (2^21 triangles into 512x512, enough to run the parameter buffer out): sum `173277184`
  on every iteration, equal to `glwork big` on the vendor driver; 10/10 and 48/48 under a competing vendor
  GLES load. HWR unchanged.
- **Found on the way, fixed:**
  - Deadlines: the first in-app probe run raised the 3D HWR count from 101 to 5301, all overruns
    (`3D(5301/101+5200)`, the third number counts GUILTY_OVERRUNING). The port mapped Mesa's "no limit"
    (`UINT32_MAX`) to 0 as 1.11 needs; the vendor passes `0xffffffff`. Passing it through ended the
    overruns: the next runs left the counters unchanged, and T2's sums went from wrong to equal to GLES.
  - Render pass external dependencies: the first `vkdraw big` of each process read back 0. Mesa's pvr
    dropped every subpass dependency on `VK_SUBPASS_EXTERNAL`, so the image-to-buffer copy (a transfer
    job) did not wait for the render (pvrtrace: `hCheckFenceFD = -1` on the transfer after a 3D kick
    whose update fence was 13). The pass now turns its external dependencies into pipeline barriers;
    an explicit `vkCmdPipelineBarrier` in the test had the same effect. Driver core, not DDK-specific.
- **Probe in the app** (`API=both scripts/run.sh`), 200 frames per test, cold and warm, every number wall time:

| Test | GLES warm median | Vulkan warm median | Pixel sums GLES / Vulkan |
|---|---|---|---|
| T1 64 passes | 0.741 ms/pass | 3.926 ms/pass | 196804608 / 198639616 (centre 58,75,168 / 59,76,170) |
| T2 individual | 10.18 ms/1000 draws | 38.02 ms/1000 draws | 282240902 / 282240902 |
| T2 instanced | 4.97 ms/1000 draws | 30.04 ms/1000 draws | 282240902 / 282240902 |
| T3 separate | 5.00 ms/chain | 29.60 ms/chain | 278003712 / 311951360 |
| T3 fbfetch / subpass | 4.29 ms/chain | 21.48 ms/chain | 278003712 / 311951360 |
| T4 uploads | 100.8 ms/frame | 64.2 ms/frame | 33423360 / 33423360 |

  - T1 iterates a bilinear, blended pass 12,800 times; the small difference is within what different
    filtering and blending precision gives.
  - T3 iterates `fract(c.gbr * 1.37 + k)` with 8-bit storage 1,600 times. Both Vulkan variants agree with
    each other and both GLES variants agree with each other; red and blue at the centre match, green
    does not. The shaders are the same, so this points at arithmetic differences between Mesa's and the
    vendor's shader compilers in a chaotic iteration, not at the command layouts. Not proven.
  - GPU clock: both APIs run at the top GED step (680 MHz) under load, so the speed gap is not DVFS.
- **In-app compute** (probe check C1): sum `6112`, the expected value.
- **A TA fault, once:** one in-app probe run raised the TA count from 164 to 165: `Guilty Lockup`, PID of
  the probe, `BIF0 - FAULT ... USCA_USC ... Writing to 0x00000000C0`, timed (kernel clock against
  `/dev/kmsg`) to the cold T3 test. Three other full probe runs and all native runs left it unchanged, and
  it did not recur under a competing vendor GLES load. A shader-side write near address 0 during the TA
  phase fits a context store through a null base; that is why the context-state values were compared
  with the vendor's and three were fixed (`CAPTURE.md`, context creation). Whether those fixes cover this
  fault is not shown.

Drafted with an LLM; marker word per AGENTS.md: biblioklept.

### 2026-11-15: dEQP-VK subsets (L7) and the final probe run (L8)

Setup: VK-GL-CTS `vulkan-cts-1.3.10.0` (commit `7aa35516`), `deqp-vk` built as a native Android armv7
executable (`-DDEQP_TARGET=android -DDEQP_ANDROID_EXE=ON`, NDK r27c), loaded through `tools/vktest/vkshim`
(`--deqp-vk-library-path`); `deqp-runner` 0.24.0 cross-built for armv7 Android; run as root from
`/data/local/tmp`, 2 jobs, 120 s timeout, `--deqp-log-images=disable --deqp-log-shader-sources=disable`,
game closed. Caselists from `external/vulkancts/mustpass/main/vk-default/`.

| Subset (fraction) | Pass | Fail | Flake | Skip (not supported) |
|---|---|---|---|---|
| info (all) | 16 | 1 | | 2 |
| memory (all) | 4,883 | 2 | | 773 |
| compute (1/10) | 45 | 0 | | 3,482 |
| api (1/100) | 394 | 200 | | 2,403 |
| renderpass (1/5) | 2,242 | 25 | 1 | 905 |
| synchronization (1/20) | 458 | 1 | | 2,558 |
| draw (1/10) | 405 | 0 | | 2,640 |

- **Failures:** 174 in `api.copy_and_blit` (169 of them `blit_image.all_formats`, typically "max difference
  = (8, 8, 8, 8), threshold = (2, 2, 2, 2)": filtering precision, not garbage), 26 in `api.image_clearing`,
  25 in `renderpass` (depth/stencil formats with `d24`, clear attachments), 2 in
  `memory.device_memory_report.external_memory` (dma-buf and opaque-fd import: the reported memory object
  ID differs from the original), 1 in `info.device_extensions` and 1 in
  `synchronization.global_priority_transition.preemption`. Committed as the baseline in
  `src/imagination/ci/ge8320-ddk113-fails.txt` and `-flakes.txt`, in the format upstream Mesa's CI uses for
  other PowerVR cores; not triaged further.
- **GPU recoveries:** HWR `TA 165 / 3D 5301 / CDM 0` before; after `api` TA 211, 3D 6037; after
  `renderpass` 3D 6325; after `synchronization` CDM 2. The other subsets left the counters unchanged. Which
  tests caused them is not recorded per test.

**Final probe run** with the build that includes the context-state fixes (`API=both scripts/run.sh`): every
test completed, pixel sums as in the L5 log, C1 `6112`, HWR counters identical before and after. Warm
medians, GLES / Vulkan: T1 0.736 / 3.908 ms per pass, T2 10.20 / 37.96 and 4.99 / 30.04 ms per 1,000 draws,
T3 5.03 / 29.29 and 4.39 / 21.36 ms per chain, T4 90.9 / 64.8 ms per frame.

Drafted with an LLM; marker word per AGENTS.md: biblioklept.

### 2026-11-15: speed work (why Vulkan was slower, and what was fixed)

Method: the probe built as a native executable and run as root; real GPU job times from the kernel
driver's `rogue` ftrace events (`tools/gputrace`: firmware timestamps at the begin and end of each TA and 3D
job, about 1.45x wall ms on this phone); CPU profiles with simpleperf; Mesa's compiled shaders with
`PCO_DEBUG_PRINT`. Game closed, HWR counters unchanged by every run below.

**Findings, in the order they were measured:**

1. The gap was mostly GPU time in the 3D (fragment) job, not CPU: T1 5.44 against 1.12 for the vendor's
   GLES driver, T2 90.3 against 15.3, T3 2.34 against 0.85; TA times were alike.
2. Every application fragment shader was a punch-through object: the compiler always added a sample-mask
   check that can discard, hence ISP feedback, plus runtime tests for alpha-to-coverage and alpha-to-one.
   A copy of upstream Mesa main read on 2026-11-15 (a public mirror) still adds that check to every shader. Fixed for pipelines with one sample per pixel and no dynamic
   multisample or colour-write state (`pco_fs_data::meta_is_static`). A 512x512 constant-colour pass:
   1.29 before, **0.41 after, vendor 0.47**.
3. Per render, the end-of-tile program was recompiled (24 % of the CPU time of a loop of small renders) and
   a control stream buffer was allocated and freed through the kernel (about 25 % in a pass-heavy frame).
   Both are now cached or pooled per device.
4. Compiler waste: 14 dead register moves per sampler fed by a varying, a 7-group truncate per fragment
   coordinate component, and samplers always returning 4 float channels. Each returned 32-bit channel costs
   3D time per fragment (T3's job: 1.71 with 4 channels, 1.37 with the 3 it reads, 1.18 with 1). The
   vendor driver's texture reads are nearly free (`glwork fill` / `tfill` / `dfill`: 0.49 / 0.49 / 0.53).
5. Not the cause: GPU clock (pinning the governor's floor to 680 MHz changed no Vulkan number), memory
   allocation flags (same GPU cache mode as the vendor), the `DISABLE_PIXELMERGE` render flag (clearing it
   with `pvrtrace -e 3D:0x130=0` helps only T2, by 16 %; left set).

**Result** (native runs, 60 frames, wall time; pixel sums identical before and after every change):

| Test | GLES | Vulkan before | Vulkan after | after / GLES |
|---|---|---|---|---|
| T1 ms per pass | 0.73 | 3.87 | 2.16 | 3.0x slower |
| T2 individual, ms per 1,000 draws | 11.1 | 37.6 | 27.1 | 2.4x slower |
| T2 instanced, ms per 1,000 draws | 4.97 | 30.0 | 19.6 | 3.9x slower |
| T3 separate, ms per chain | 5.0 | 29.9 | 13.3 | 2.7x slower |
| T3 tile-local, ms per chain | 5.65 | 21.6 | 13.1 | 2.3x slower |
| T4 uploads, ms per frame | 77–87 | 62 | 62 | 1.2–1.4x faster |

Plain passes without texture reads are at the vendor's speed; passes dominated by texture reads, blending
on loaded contents (T1) and discard (T2) are not.

**dEQP-VK after the changes** (same subsets and fractions as L7, `--baseline` the committed fails file):
renderpass, draw, api, synchronization and compute all exit 0: no new failure, the same 226 expected
failures in those subsets, the earlier flake passed. GPU recoveries during the run: renderpass 3D +144
(was +288), api TA +23 and 3D +369 (was +46 and +736), synchronization CDM +1.

**Open, in order of expected gain:** the cost of texture reads (the vendor can take one packed word per
texel because GLSL ES samplers default to low precision; here it needs format-specialised or
relaxed-precision sampling), the attachment load shader and float blending in T1, the punch-through path
and pixel merging in T2, and the kernel kick (about 15 % of CPU per render).

Drafted with an LLM; marker word per AGENTS.md: biblioklept.

### 2026-11-16: blending and the attachment load (lane blend)

Method: `vkbench` modes `blend` (LOAD op, alpha blend), `load` (LOAD op, no blend) and `blendc` (blend over a
CLEAR), median 3D job per 512x512 pass over 20 passes, three runs, GPU at the 400 MHz level. Before the fix
(fork `bf89cfc`): `fill` 0.238, `load` 0.238, `blendc` 0.419, `blend` 0.823 ms; vendor GLES `fill` 0.311,
`blend` 0.513.

- The load alone is free when nothing reads the tile (`load` equals `fill`), and blending alone costs
  0.18 ms (`blendc`); the loaded blend costs 0.40 ms more than that, so the cost is in the load program, not in
  the blend shader (9 instruction groups against `fill`'s 6, and nothing to cut).
- The load program (`pvr_uscgen_loadop`) was 16 groups; 12 of them tested the coverage mask (`pck.cov`, shift,
  test, `csel`), twice, to choose between the pixel and the sample x/y. A single-sampled load never runs per
  sample, so the translator now reads the pixel registers directly (`pco_fs_data.single_sampled`). The program
  is 6 groups. `blend` 0.823 -> 0.582 ms (three runs 0.582); `fill`, `load`, `blendc` unchanged; image sums
  unchanged.
- Probe T1 (1024x448, 64 passes, three alternating runs per driver, firmware ms of 3D time per pass): 3.382 ->
  2.666 (wall 2.18 -> 1.76 ms); without the load (`PROBE_VARIANT=dc`) 2.094 and without the blend (`nb`) 1.208,
  both unchanged. The load now costs 0.57 ms per pass instead of 1.29.
- Measured and rejected: a constant uniform LOD on the load read (0.582 -> 0.605); half-float return (no
  change, 0.582); a load program that reads nothing and only stores (0.417, the floor: the load machinery
  itself is free, the remaining 0.165 ms over `blendc` is the texture read plus its conversion); raw return
  (`rawdata`) of the texels, 0.493 ms, but that mode is a gather and returns the four texels around the
  coordinate, so the image is wrong and varies between runs (neighbouring tiles); reading the RGBA8 target
  through an R32_UINT view of the same memory (one word, no conversion) timed 0.49 ms but returned wrong data
  (image sum 84148224 against 133431296) with and without a uniform LOD and with integer coordinates; cause not
  found.

### 2026-11-16: correctness lane (229 baseline failures, GPU recoveries)

Setup: the L7 subsets and caselists, `deqp-runner` 0.24.0 for list runs, `deqp-vk` one process per test for the
recovery search, game closed, driver at `bf89cfc` for the triage. HWR counters before and after every session.
Every recovery in the kernel driver's debug dump comes with the faulting unit and address, which is what
identified the causes below.

**Which tests reset the GPU** (api list 2,997 tests and renderpass list 3,173 tests, one process per test, the
counters read after each test; 69 tests, 555 recoveries; the first 2,400 renderpass tests and all other subsets
raised none):

| Family | Tests | Recoveries | Unit and fault | Reading |
|---|---|---|---|---|
| blit from an ETC2/EAC source (`blit_image.all_formats.color.2d.etc2_*`, `eac_*`) | 20 | 3D +13 or +14 each (264) | texture cache unit, page-table fault in the general heap, guilty, no render target (transfer job) | the source read runs past the image; cause not found |
| copy and blit of D24S8/D32S8, 1D and 2D (`all_formats.depth_stencil`) | 6 | 3D +4 each (24) | ISP index fetch from address 0 of the transfer heap | control stream link or base of the two-pass depth/stencil transfer; cause not found |
| `clear_depth_stencil_image` over several layers of D24S8/D32S8 | 3 | 3D +40 to +42 each (123) | same fetch | same |
| `vkCmdClearAttachments` over several array layers (`multiple_layers`) | 23 | TA +1 each (23) | TA hang of about 0.4 s of GPU time, no MMU fault, polling register 0x3B0 | no render target array support (`gs_rta_support` is off in the GE8320 device info), clears of layers above 0 go through a deferred transfer fill |
| render pass with D32S8 clear attachments no subpass uses (`unused_clear_attachments`) | 16 (6 fail, 10 pass) | 3D +1 each | same index fetch | same |
| `subpass_dependencies.separate_channels.d24_unorm_s8_uint` | 1 | 3D +128 | same index fetch | same |
| `synchronization.global_priority_transition.preemption.compute_low_to_compute_medium` | 1 | CDM +1 per run | compute | preemption of a low priority queue |

Run together with two jobs the same tests give the recoveries seen in L7; the 5,200 "GUILTY_OVERRUNING" in the
3D counter are the deadline mistake of the first in-app run, not dEQP. The first four entries of each data
master's recovery list in the dump are old (page-catalog faults on USC code) and were not raised by dEQP.

**Triage of the 229 failures** (counts at `bf89cfc`; "fixed" means the same test passes on the new driver):

| Group | Tests | Cause | Result |
|---|---|---|---|
| scaled `blit_image` colour, 1D/2D/3D, nearest and linear | 110 | the sample of a scaled blit landed on the corner of the destination pixel | fixed, `87ec23f` |
| `blit_image` `generate_mipmaps` | 37 | same | fixed |
| `blit_image` 3D mirror | 1 | same | fixed |
| `renderpass` D16/D24 depth, `x8_d24`, `d24_s8` clears | 16 | the clear depth of the render pass was written as an unorm integer; the core reads the float | fixed, `a9e3c31` (also 9 `fragment_operations` depth tests found by the merge lane) |
| `renderpass` other (colour+depth+stencil D24 simple tests) | 2 | same | fixed |
| blit from ETC2/EAC | 20 | see the recovery table | skipped |
| copy/blit D24S8/D32S8 | 6 | recovery table | skipped |
| `clear_color_attachment` / `partial_clear_color_attachment` `multiple_layers` | 23 | recovery table | skipped |
| `clear_depth_stencil_image` several layers | 3 | recovery table | skipped |
| `unused_clear_attachments` D32S8 depth only | 6 | recovery table | skipped |
| `separate_channels` D24S8 | 1 | recovery table | skipped |
| `synchronization` preemption | 1 | CDM recovery | skipped |
| `memory.device_memory_report.external_memory` (dma-buf, opaque fd) | 2 | the memory object id of the record differs from the one of the imported memory | expected failure |
| `info.device_extensions` | 1 | the driver lists `VK_KHR_robustness2`, CTS 1.3.10 does not know it | expected failure |
| flake `multisample_resolve.r8g8_uint.max_attachments_4_samples_2` | (1) | failed once with two jobs, 20/20 passes single process | kept in the flakes file |

For a game port: blits (mip generation) and D16/D24 depth buffers are fixed; layered clears
(`vkCmdClearAttachments` over array layers, cube/array shadow maps), D24S8/D32S8 image copies and ETC2 blits are
not usable and, run, reset the GPU.

**Fixes** (each a commit in this branch):
- `87ec23f`: half a step added to the constants of the non-iterated transfer shader. The result image of
  `blit_image.all_formats.color.2d.r8g8b8a8_unorm` matched source texel 4x for destination pixel x (error 0),
  where 4x+2 is right. 182 blit tests: 148 Fail / 34 Pass before, 182 Pass after.
- `a9e3c31`: background object depth as float32 for D16/D24. 185 depth tests: 159 Pass, 25 Fail before; 184 Pass,
  1 skipped after.

**Re-run with the committed expectations** (all seven L7 subsets, `--baseline` fails, `--skips`, `--flakes`,
two jobs, HWR TA 280 / 3D 9609 / CDM 9 before and after): info 16 pass + 1 expected fail; memory 4,883 + 2
expected fails; compute 45; api 540 (2,457 skipped or not supported); renderpass 2,240; synchronization 458;
draw 405; all exit 0; no recovery. Old against new driver, same lists: api 148 Fail->Pass, 392 Pass->Pass, 0
Pass->Fail; renderpass 18 Fail->Pass, 2,222 Pass->Pass, 0 Pass->Fail.

**TA lockup (USC write to 0xC0):** 30 cold native probe runs (`vkprobe-native vk`, 40 frames) with the build at
`bf89cfc`: no recovery. Not reproduced. The TA hangs found above have no MMU fault and a different signature.

Drafted with an LLM; marker word per AGENTS.md: biblioklept.
