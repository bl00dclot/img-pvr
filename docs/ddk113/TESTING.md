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
| L7 | a dEQP-VK subset passes under `deqp-runner`, with a committed baseline of known failures, flakes and skips (Mesa CI practice), image and shader logging off | **run, triaged** (2026-11-15/16): 7 subsets, 229 baseline failures at the first run; after the correctness work 166 pass, 60 sit in `ge8320-ddk113-skips.txt` because they reset the GPU, 3 are expected failures; no GPU recovery in a run with the skips file; `src/imagination/ci/ge8320-ddk113-{fails,flakes,skips}.txt` |
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

### 2026-11-16: blending and the attachment load (the blend work)

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

### 2026-11-16: correctness (229 baseline failures, GPU recoveries)

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
| `renderpass` D16/D24 depth, `x8_d24`, `d24_s8` clears | 16 | the clear depth of the render pass was written as an unorm integer; the core reads the float | fixed, `a9e3c31` (also 9 `fragment_operations` depth tests found by the pixel-merging work) |
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

### 2026-11-16: the GPU clock, a kernel panic, and how GPU time is measured now (clock and measurement)

Why: the probe runs saw Vulkan job times flip between two values from run to run, and some single runs had
Vulkan faster than GLES. This entry finds the cause and replaces the measuring method. No driver change; the
driver measured is the branch head of the time, `fc6a3aa`.

**Clock levels.** MediaTek's GPU DVFS (GED) runs the GE8320 at three levels on this phone: 0 = 680 MHz,
1 = 500 MHz, 2 = 400 MHz. A sampler read GED's current level (`/sys/kernel/ged/hal/current_freqency`) about
3,000 times a second (about 0.3 ms resolution) during 64 benchmark runs (8 `vkbench`/`glwork` modes, Vulkan and
GLES, 4 rounds, 400 passes per run), and each 3D job was tagged with the level it ran at; jobs during which the
level changed were left out. The file holds GED's last decision, not a hardware reading; a second, independent
tag from the firmware-to-host timestamp rate agreed on 9,424 of 9,435 Vulkan jobs.

- GED raises the clock to 680 MHz about 33 ms after a load starts and steps it down again in steps of about
  33 ms after the load ends.
- Vulkan: 30 of 32 runs started at 400 or 500 MHz; the modes with short jobs spent 4–53 % of a 400-pass run at
  680 MHz (`alu` and `td`, with long jobs, 95–99 %). At 20 passes, as the probe runs had them, `fill`, `blend`,
  `tv`, `tf` and `disc` reached 680 MHz in 0 of 4 runs, `fetch` in 1 of 4.
- Vendor GLES: GED switches between 680 and 500/400 MHz about every millisecond; GLES spent 72–98 % of each run
  at 680 MHz.

**Firmware timestamps change unit with the clock.** Slope of the firmware end timestamps against the host end
times, over blocks of at least 15 consecutive jobs at one level spanning at least 12 ms:

| Level | Blocks | Jobs | Firmware/host slope, median (min–max) |
|---|---|---|---|
| 680 MHz, Vulkan | 59 | 6,365 | 1.700 (1.488–1.703) |
| 680 MHz, GLES | 8 | 2,425 | 1.700 (1.700–1.700) |
| 500 MHz, Vulkan | 46 | 1,155 | 0.735 (0.732–1.252) |
| 400 MHz, Vulkan | 62 | 4,968 | 0.588 (0.587–0.917) |
| fixed at 680 MHz (GED floor pinned, below) | 30 Vulkan + 30 GLES | 12,000 + 12,000 | 1.000 (1.000–1.000) |

1.700 = 680/400, 0.735 = 500/680, 0.588 = 400/680. It is a change of unit, not of speed: `fill` is fixed work
and shows 0.406 firmware ms at 680 MHz and 0.237 at 400 MHz, both about 161,000–162,000 GPU cycles.

Likely cause, from the kernel source (a MediaTek DDK 1.13 kernel tree of another device, whose function names
match this phone's panic call chain below): the GPU timer ticks once per 256 core cycles, and the kernel
converts ticks to OS time with the clock rate it estimated over the previous calibration window. Under DVFS that
estimate is stale (the rates above); at a fixed clock it is exact (slope 1.000). This fits the measurements; it
was not checked against the phone's own kernel build.

Consequences:

- A firmware job time means something only within one clock level. Compute-bound jobs even show fewer firmware
  ms at 400 MHz than at 680 MHz although they take longer.
- The run-to-run flip was the clock level. Single-run "Vulkan faster than GLES" readings were Vulkan at
  400/500 MHz against GLES at 680 MHz.
- The firmware ms in the earlier entries of this log mix clock levels: the "speed work" entry's "about 1.45x
  wall ms" was taken without knowing the level, and the "blending and the attachment load" entry's Vulkan runs were at 400 MHz while
  GLES, by the shares above, runs mostly at 680 MHz. Before/after pairs taken at one level stand; Vulkan against GLES ratios from them do not.
- Vulkan/GLES ratios from 680 MHz jobs only (median over 4 rounds), against the ratio over all jobs:

| Mode | All jobs | 680 MHz only | Pinned at 680 MHz |
|---|---|---|---|
| `fill` | 0.78 | 1.34 | 1.34 |
| `alu` | 1.57 | 1.57 | 1.57 |
| `blend` | 1.49 (1.11–1.92) | 1.89 | 1.89 |
| `tv` | 0.56 | 1.02 | 1.03 |
| `tf` | 0.61 | 1.06 | 1.01 |
| `td` | 0.99 | 0.99 | 0.99 |
| `fetch` | 0.61 | 1.00 | 1.03 (3 rounds) |
| `disc` | 1.04 (0.98–1.67) | 1.68 | 1.69 (3 rounds) |

  Texture reads are at parity with GLES; fill, blend and discard are the real gaps, with ALU.

**The pin and the kernel panic.** To confirm the table, the GED floor was pinned to 680 MHz
(`custom_boost_gpu_freq`) under a script on the phone with a deadline, temperature and throttling aborts and a
dead-man restore, all tested first. Under the pin every job ran at 680 MHz and firmware time equalled wall time
(slope 1.000, table above). About 6 minutes into the pinned session (2026-11-16, about 12:31 UTC, GLES `fetch`,
round 4) the phone kernel-panicked and rebooted (boot reason `kernel_panic`). The console log kept across the
reboot shows a synchronous external abort at `RGXTimeCorrBegin+0x5c` in the kernel's `apphint_workqueue`, call
chain `apphint_action_worker` > `_GpuTraceSetEnabledCallback` > `_GpuTraceSetEnabled` >
`RGXTimeCorrSetClockSource` > `_SetClock` > `RGXTimeCorrBegin`.

Cause, from the same kernel source. Writing `/proc/pvr/apphint/EnableFTraceGPU` makes the apphint workqueue
switch GPU ftrace on or off. Switching on sets the firmware HWPerf event mask, opens the HWPerf stream, registers
a command-complete notifier that turns HWPerf packets into the `rogue_*` ftrace events, and switches the time
correlation clock source to `sched_clock`; switching off undoes this. Each clock-source switch calls
`_SetClock`, which calls `RGXTimeCorrEnd` and then `RGXTimeCorrBegin`, and each of those reads `RGX_CR_TIMER`
directly (`RGXReadHWTimerReg`) without the power lock and without checking the GPU power state. The periodic
path, `RGXTimeCorrRestartPeriodic`, takes `PVRSRVPowerTryLock` and skips the read when the GPU is not on. A read
of a register of a powered-down GPU is the likely source of the abort. The same unguarded `_SetClock` is in DDK
1.15 and in a newer upstream tree. The board-1 `gputrace.sh` wrote the apphint at the start and the end of every
run, hundreds of times before without a panic; whether the pin made the race more likely is not known.
Temperatures were normal (CPU zone at most 45.6 °C; the watcher's abort was 50 °C). Mesa's driver is not in the
call chain.

After the reboot `adb shell` runs as an unprivileged user; root is through `su`. The pin is not used any more
(no writes under `/sys/kernel/ged/`); the 680 MHz-only ratios above did not need it.

**Method from now on** (tools in `8b3af25` and `e52992c`, run as root, game closed, HWR counters before and
after every session):

1. `tools/gputrace/trace-session.sh on` once at the start of a phone session, `off` once at the end. It writes
   the apphint only while a GLES load runs, so the GPU is powered, polls the readback, and refuses (exit 3) when
   the value is already the one asked for. `gputrace.sh` never writes it and refuses (exit 3) unless it already
   reads `Y`. First test: on, one trace (267 lines, 204 `rogue_sched_switch`, 52 `rogue_job_enqueue`), off;
   HWR unchanged. Every later session in this log used it the same way without a panic.
2. `BENCH_WALL=300` on `vkbench` and `glwork bench`: warm up in batches of 64 passes until GED reads level 0
   (680 MHz) plus 300 ms, at most 5 s (level readings in the first 200 ms are ignored: a 0 there is left from
   the previous process), then time N passes back to back (Vulkan: one command buffer, one submit, one wait;
   GLES: `glFlush` per pass, one `glFinish`) and print wall ms per pass and the GED level before and after. A
   run counts only if both read 680 MHz. Two earlier warm-ups failed: one pass per submit left the GPU mostly
   idle and GED at 400/500 MHz for the whole 5 s; 400 warm-up passes started the timed block at 400/500 MHz.
3. Main number: the 3D job time, the median per run of the trace's 3D jobs (`bench_table.py`, `frag_times`),
   three runs, the median of the three. All jobs are at 680 MHz, so all are in one unit.
4. GLES per-pass 3D time comes from one-pass jobs: `glwork bench` with `BENCH_WALL=300` and N = 1, the timed
   pass being the last 3D job of the trace. With N = 2,000 the vendor driver puts all passes into one 3D job
   (`fill`: 71.5 ms for 2,000 passes, 0.036 ms per pass; the warm-up jobs hold 65 passes each), so its batched
   wall time (about 0.26 ms per pass, set by kicks and CPU) is not a per-pass GPU cost. The one-pass jobs hold
   two TA/3D kicks, one of them not identified; their times (`fill` 0.178, `blend` 0.313, `disc` 0.287,
   `tv` 0.294 ms, medians of 3) match the firmware figures of the pinned session (0.179, 0.307, 0.290,
   0.289).
5. Wall ms per pass only for before/after on the same API. Vulkan/GLES wall-time ratios at 680 MHz (2,000
   passes, 3 runs each): `fill` 1.36 and `blend` 1.80 are within 5 % of the same-level firmware ratios (1.34,
   1.89), `disc` 2.09 (1.68) and `tv` 1.40 (1.02) are not. Vulkan wall time carries a per-job floor (kick, TA/3D
   hand-over) of about 0.34 ms per pass that hides light passes, and the GLES side is batched (item 4).

Drafted with an LLM; marker word per AGENTS.md: biblioklept.

### 2026-11-16: fill, discard, attachment load and blend shaders (board 2)

Setup: the 680 MHz method of the previous entry; `vkbench` 512x512 passes, 2,000 timed passes per run, three
runs, median. "Old" is `fc6a3aa` (or, for the last change, the branch head before it), "new" the change.
Instruction groups come from `tools/pco-stats` on the host: 24 shader stages (12 `vkbench` shaders, vertex and
fragment); the probe's shaders are not in the corpus. Correctness bar for every change: `vkbench` and probe
pixel sums identical, dEQP-VK old against new driver on the same lists with 0 changed results, HWR counters
unchanged.

**Fill: constant colour folded, final pack written straight to the output (`ef78b60`, `af38acd`).**

- `pack_to_format()` folds four constant float channels in [0, 1] packed to R8G8B8A8 or B8G8R8A8 UNORM into one
  immediate. The tie 127.5 is folded to 128: the `vkbench fill` sum 133955584 / 512² = 511 = 64 + 128 + 191 + 128
  proves that one tie. It does not tell round-half-up from round-half-even, so other ties, values within 1e-3
  of a tie, out-of-range and NaN values are left to the run-time `pck`.
- `pco_opt.c` back-propagation into the pixel output register `po0` was switched off in the import; the comment
  said `pck` has no `olchk`, which is wrong (`pck` carries it in the group header, and `xfer_set_op_mods()`
  copies it over). The first version (`ef78b60`) let any producer write `po0` and failed 47 dEQP-VK renderpass
  tests that pass on the old driver (1/5 fraction; for example `dedicated_allocation.attachment.1.24`, input
  attachment and depth/stencil cases). The internal load-op and transfer fragment shaders had their `smp` result
  moved into `po0`; legalization then copied `po0` into a temporary before the `smp` and nothing wrote `po0`
  afterwards. `af38acd` allows only `pck` and `movi32` (the folded colour). The legalization problem itself is
  not fixed, only avoided.
- Groups: `fill` FS 6 -> 1 (`bbyp0bm_imm32` to `po0` with `olchk` and end; temps 8 -> 0, code 64 -> 16 bytes);
  `blend`, `tv`, `tvm`, `tf`, `fetch`, `td`, `c1`, `c2`, `alu`, `alum` FS one group fewer each. Corpus 689 -> 674
  groups, 11 shaders better, 0 worse, temps 107 -> 95, code 7,664 -> 7,544 bytes.
- 3D job ms (old -> new, medians of 3): `fill` 0.241 -> 0.193 (-20 %), `blend` 0.584 -> 0.535 (-8.4 %),
  `tv` 0.259 -> 0.254 (-2 %). Sums identical (`fill` 133955584, `blend` 133431296, `tv` 133693440); probe
  sums and centre pixels identical (T1 198639616, T2 282240902, T3 216989696, T4 33423360). The centre pixels
  and dEQP's colour compares confirm the channel order of the folded colour.
- dEQP-VK `af38acd` against `fc6a3aa`: renderpass 1/5 and draw 1/10, both exit 0, 0 changed.

**Discard (`0366abd`; the select experiment removed in `c5948f8`).**

- When nothing but the ISP feedback that `pco_nir_pfo` adds reads `gl_IsHelperInvocation` (no explicit read, no
  helper-write guard), the invocation no longer starts as a helper
  (`nir_lower_is_helper_invocation_opts.start_as_helper`): the sample-mask arithmetic goes (18 -> 13 groups).
  Argument: the hardware has no kill, every invocation runs to the end, so demote and terminate coincide; the
  ISP ignores feedback for pixels the rasterizer did not cover; for MSAA the sample check is already a demote in
  the condition. The ISP premise is not proven on the device beyond the identical sums below.
- New op `cmpp`: a discard condition that is one scalar compare is written straight to `p0`, and the feedback
  runs on `!p0` (13 -> 12 groups). `disc` FS 18 -> 12 groups, temps 6 -> 5, code 200 -> 120 bytes; no other
  shader changed.
- The "empty if/else" that looked removable is not dead code: `legalize_fence()` puts a 3-group fence after
  every `ditr` (`flush_p0`, then `br.next` on both senses of `p0`; the code says "enforce WDF ordering"). Kept.
  `PCO_DEBUG=no_fence` (10 groups) and `PCO_DEBUG=fence1` (11) stay as experiments, off by default.
- 3D job ms: old 0.495 (0.503 0.495 0.495), new 0.346 (0.344 0.351 0.346), -30 %; wall 0.559 -> 0.412 ms per
  pass. `no_fence` 0.345 and `fence1` 0.351: no measurable gain, so the fence stays.
- Discard by select (translucent pass, colour stores moved to the end, the previous framebuffer value selected
  by the helper flag, no ISP feedback; only without depth/stencil): 16 groups and 13 temps on the host, 3D job
  0.511 ms against 0.346 (+48 %), and unusable with occlusion queries, which count discarded fragments.
  Rejected; the code and its `PVR_DISC_SELECT` switch are removed in `c5948f8`.
- Sums identical old and new for `fill`, `blend`, `disc` (108724224), `tv`, `tvm`, `alu`, `td`, `tf`, `fetch`,
  `c1`, `c2`; the six probe `PIXELS` lines identical. dEQP-VK: `fragment_operations` (all 151 cases: 98 Pass,
  51 Skip, 2 Warn) and draw 1/10 (405 Pass, 2,640 Skip), identical, 0 changed. Coverage is thin: the
  `early_fragment.discard_*` cases are skipped on this driver and 87 % of draw is skipped.

**Attachment load as one raw word (`2a80c9a`).** For a LOAD of an R8G8B8A8_UNORM attachment (not MSAA, not a
2D view of a 3D image) the load program reads the tile through an R32_UINT view with one channel and stores the
word unchanged, instead of a converting RGBA8 read and a `pck`.

- Why the first attempt (the blend work, previous day's entry) returned wrong data: the texture state word 0 was
  edited by unpacking and repacking it. `texformat` and `texformat_compressed` share bits 27–33; the unpack fills
  both with the old format, and the pack ORs all fields, so U8U8U8U8 (4) | U32 (24) gave 28 = F16F16F16F16. The
  one-channel program then read a four-channel half-float texture. Found by printing the packed words of the
  load's descriptor and decoding them by field. Fixed by clearing `texformat_compressed` before setting
  `texformat`.
- 3D job ms (old, new, 3 runs each): `blend` 0.583 -> 0.496 (-15 %); `blendc` (blend over a CLEAR) 0.420 both;
  `load` (LOAD, no blend) 0.241 both. The raw read only matters when the loaded tile is read. Probe T1 wall at
  680 MHz: 1.739 -> 1.574 ms per pass (-9.5 %).
- Sums identical (`blend` 133431296, `blendc` 83886080, `load` 133955584; probe T1–T4 as above). dEQP-VK
  renderpass 1/5 and draw 1/10, old and new: 2,261 Pass / 7 Fail / 905 Skip and 405 Pass / 2,640 Skip, the
  same 7 failures on both. These runs were made without the GPU-reset skips file; the 7 are the D24S8/D32S8
  tests the skips file lists (6 `unused_clear_attachments`, 1 `separate_channels`).

**Blend reads `po0` straight into the unpack (`27cd2eb`).** The destination-colour read (`mov` from `po0` with
`olchk`) had its own group before the unpack. Forward propagation now lets an `unpck` read `po0` directly, the
group carrying the `olchk`; it is refused when a pixel output register is written between the copy and the
unpack.

- Groups: `blend` FS 8 -> 7 (code 96 -> 88 bytes); no other shader changed, temps and spills unchanged.
- 3D job ms against the branch head before it (`c5948f8`): `blend` 0.472 -> 0.450 (-4.7 %), `blendc` 0.394 ->
  0.379 (-3.8 %); wall 0.535 -> 0.513 ms per pass. Sums and probe sums identical.
- dEQP-VK renderpass 1/5 and draw 1/10, old and new (again without the skips file): 2,260 Pass / 7 Fail /
  905 Skip / 1 Flake and 405 Pass / 2,640 Skip on both, the same 7 failures, 0 changed.
- This change was measured on its own; the whole-branch check below was made before it.

**The branch as a whole (`c5948f8`, everything above except `27cd2eb`, against `fc6a3aa`).**

- Host: 12 of 24 shader stages changed, 0 worse; groups 689 -> 668, temps 107 -> 94, code 7,664 -> 7,464 bytes,
  no spills. `fill` 6 -> 1, `disc` 18 -> 12, `blend` 9 -> 8 (7 with `27cd2eb`), the other fragment shaders -1.
- Sums: `vkbench` 20 passes, all 14 modes identical (`fill` and `load` 133955584; `tv`, `tf`, `tvm`, `c2`,
  `fetch` 133693440; `td` 133695656; `c1` 100270080; `disc` 108724224; `alu`, `alum` 132863621; `blend`
  133431296; `blendc` 83886080). Probe `PIXELS` and centre pixels identical over 4 runs per driver.
- dEQP-VK, with the baseline fails file and the GPU-reset skips file, two jobs, per-test results compared old
  against new: 0 changed in every subset, both drivers exit 0. renderpass 1/5 (3,173: 2,240 Pass, 933 Skip),
  draw 1/10 (3,045: 405 / 2,640), api 1/100 (2,997: 540 / 2,457), synchronization 1/20 (3,017: 458 / 2,559),
  compute 1/10 (3,527: 45 / 3,482), `fragment_operations` all (151: 98 Pass, 51 Skip, 2 Warn), `glsl.discard`
  all (25 Pass), `glsl.derivate` all (1,656: 1,368 / 288), rasterization 1/10 (1,508: 18 / 1,490). No
  `pipeline.*` list was run; most draw, api and compute cases are skipped on this driver, so the sums are the
  stronger evidence.
- HWR: no recovery from these sessions (counters unchanged before and after each).
- 3D job ms per 512x512 pass at 680 MHz, three runs; GLES from one-pass jobs:

| Mode | Vulkan old | Vulkan new | GLES | old / GLES | new / GLES | Wall old -> new |
|---|---|---|---|---|---|---|
| `fill` | 0.242 | 0.194 | 0.178 | 1.36 | 1.09 | 0.375 -> 0.340 |
| `blend` | 0.584 | 0.472 (see below) | 0.313 | 1.87 | 1.51 | 0.646 -> 0.535 |
| `disc` | 0.500 | 0.352 | 0.287 | 1.74 | 1.23 | 0.563 -> 0.418 |
| `tv` | 0.267 | 0.258 | 0.294 | 0.91 | 0.88 | 0.376 -> 0.372 |
| `alu` | 4.963 | 4.939 | 3.157 | 1.57 | 1.56 | 5.026 -> 5.002 |

  With `27cd2eb`, `blend` is 0.450 ms (its own session), 1.44x GLES. The new `blend` 3D job times are bimodal
  within and across runs: the run medians were 0.806, 0.472 and 0.801 ms, the old driver never showed it, and
  the wall time is steady (0.534–0.537 ms per pass, against 0.646 old). Wall minus the old driver's per-job gap
  (0.646 - 0.584) gives 0.47, and the earlier blend run measured 0.472 0.472 0.473 on the same code, so 0.472 is taken
  as the real time and 0.80 as a conversion artefact of the firmware timestamps. Not proven.
- Probe at 680 MHz (`vkprobe-native`, wall, three alternating runs per driver, each after a `vkbench` warm-up;
  GED read 680 MHz after every run, but 500 MHz at the start of runs 2 and 3 for both drivers):

| Test | Old | New | Change |
|---|---|---|---|
| T1 ping-pong, ms per pass | 1.7375 | 1.5305 | -11.9 % |
| T2 individual draws, ms per 1,000 | 19.20 | 16.99 | -11.5 % |
| T2 instanced, ms per 1,000 | 11.77 | 9.54 | -19 % |
| T3 8-pass chain, separate, ms | 10.98 | 10.65 | -3.0 % |
| T3 8 subpasses, ms | 11.90 | 11.39 | -4.3 % |
| T4 uploads, ms per frame | 62.07 | 62.39 | +0.5 % (noise) |

  The probe was not run against GLES at a controlled clock, so these are not GLES ratios.

**GPU recoveries this day.** dEQP-VK renderpass run without the skips file resets the GPU: +288 3D and +1 TA
recoveries per renderpass 1/5 run, the same on the old and the new driver (counters 3D 566 -> 854 -> 1,142,
TA 0 -> 1 -> 2 over one old and one new run); draw runs add none. They come from the D24S8/D32S8 tests the
skips file lists (the "correctness" entry). The kernel's recovery list also holds a "guilty lockup" with a
BIF fault reading 0xE400000000 at about 17:22 UTC, while the first such renderpass run was going; possibly from
that run, not proven. Since then
every dEQP run passes the skips file.

**The vendor's fragment code.** The vendor GLES driver compiles fragment shaders at draw time (the vendor readout,
board 1). Its shader cache is not reachable without root and its compiler-log apphint produced no files, but
the code lies in a CPU-mapped device heap of the process: `glwork bench` with `GLWORK_DUMP_MEM=<dir>` (`e3ec787`)
writes its own mappings after the first draw and after the passes, and `tools/pco-scan -m 2` decodes the
instruction groups (pixel sums with the dump equal those without). Kept local; findings only:

| Shader | Vendor groups | Ours before board 2 | Notable |
|---|---|---|---|
| `fill` | 3 | 6 | two `mbyp`, then one output group |
| `blend` | 4 | 9 | the same two `mbyp`, one more output-unit group, the output group; where it reads the destination colour is not known |
| `tv` | 2 | 9 | `itrsmp` (iterate and sample fused), then the output group; no `ditr`, no separate `smp`, no fence |
| `disc` | 5 or 8 | 18 | two variants in the heap, which one the draw uses not established; `itrsmp`, one compare, one `branch` over the output, final `movc` with end; the larger one adds `pck`, `vistest_atst` and `wdf`; no helper arithmetic, no fence |
| `alu` | 130 | 203 | `ditr`, 64 `fmad`, 64 `fadd`: `fract` is one `fadd` with both `s0neg` and `s0flr` (ours: two), and the constants come from preloaded registers |

The last group of every vendor shader is a single-issue output-unit op that `pco_isa` has no encoding for
(`f16sop`), with `olchk` and end; it is identical in `fill`, `tv` and `alu`, so the colour is computed into
fixed registers before it.

**Measured and rejected:** discard by select (+48 %, above); removing the `ditr` fence (no measurable gain, and a
missing ordering barrier would be a timing-dependent corruption); fusing any producer into `po0` (47 renderpass
failures, above).

**Exit targets of the board, not met:** `fill` 1.09x against 1.05x, `disc` 1.23x against 1.2x (the discard work
had 1.19x, from 0.346 against the pinned session's GLES 0.290), `blend` 1.44x against 1.3x.

**Open:**
- The bimodal `blend` 3D job time on the new driver (0.47 / 0.80 ms).
- `smp` into `po0` loses the pixel output write in legalization (latent; avoided by `af38acd`).
- Other places that unpack a texture state word and repack it with a changed format may hit the same OR as the
  attachment load.
- The rounding rule of `pck` for ties other than 127.5; the ISP-ignores-uncovered-pixels premise of the discard
  change on hardware.
- The second TA/3D kick in a one-pass GLES job.
- Gaps against the vendor, by group count: `tv` and `disc` (`itrsmp`, the fused iterate-and-sample step), `blend` (7 against 4:
  constants in registers, co-issue), `alu` (`fract` as one `fadd`, about 64 groups, 1.56x).

Drafted with an LLM; marker word per AGENTS.md: biblioklept.

### 2026-11-17: board 3 (instruction grouping, fused iterate-and-sample, per-render cost, uniform reads)

Base: `41bad10` (the pushed `ddk113`). Head: `b228a10` plus the three uniform commits (19 commits, `CHANGELOG.md`).
Correctness bar as before: `vkbench` and probe pixel sums identical, dEQP-VK old driver against new on the same
lists with per-test results diffed (0 changed), HWR counters unchanged.

**What changed, by effect.**

- Forward propagation to a fixed point: `fract` is one `fadd` with `.neg.flr` (alu 202 -> 136 groups, the vendor's
  shader has 130). The fix `b228a10` stops a consumer's `.flr` from dropping `.abs`/`.neg` of the source
  (`floor(-x)`).
- Co-issue: independent main-ALU ops in phase 0 and phase 1 of one group, after register allocation, so no
  register pressure change. Coefficient registers are kept out of phase 1 (a dEQP interpolation case failed with
  them; how the upper source group encodes that bank is not established).
- `itrsmp`: one instruction for a texture read at an interpolated input, with its own `wdf` in the scheduling
  field; the validator now knows shared-register operands. Backend destinations (`po0` for `smp`) are copied after
  the instruction, which fixes the cause of the board-2 pixel-output fusion failures.
- Per render: a 32x32 dummy object instead of a quarter-framebuffer one, and a 100 us sleep between RETRY answers
  of the kick.
- Uniform reads: no dynamic-offset add for buffers that are not dynamic, one `imadd64` for the address, phi
  registers written by the producer.

**Debug flags** (`PCO_DEBUG=a,b`, default off; each restores the previous code for A/B runs; with all of them set
the host compiler output is byte-identical to `41bad10`, below):

| bit | flag | restores |
|---|---|---|
| 11 | `no_coissue` | one op per group |
| 12 | `no_fwdfix` | one forward-propagation round |
| 13 | `fuse_all` | (test) lifts board 2's `pck`/`movi32` limit on writing `po0` |
| 14 | `no_itrsmp` | `ditr`, fence, `smp` |
| 15 | `no_schedwdf` | `itrsmp` followed by a `wdf` group |
| 16 | `no_imadd_addr` | `add64_32` buffer address |
| 17 | `no_vreg_coalesce` | lone copy into the phi register |
| 18 | (unused here; a later branch uses it) | |
| 19 | `no_dynoff_skip` | the dynamic-offset add for every buffer (`073da0e`) |

`no_dynoff_skip` was added after the board (the uniform commit had none; a later check found 47 shaders
differing without it).

**Method.** As in the previous entry: one GPU clock (680 MHz; every run read 680000 before and after), 3D job ms
from the trace, wall ms per pass as a cross-check, 2,000 passes per run, 5 interleaved repeats (old, new, GLES in turn),
median with min-max and CV, thermal zone 28.9-29.3 C; GLES is the vendor driver, one pass per job. A change is
called real only when the min-max ranges do not overlap. Host: `tools/pco-stats` over 88 pipelines (26 `vkbench`,
8 probe, 54 stages of the application's own shaders, kept outside the repository), compared per shader.

**Host, old against new** (final head, after the review fixes below; the head before them had 100885 groups, -16.4 %).
Instruction groups 120641 -> 101542 (-15.8 %), 67 of 88 changed, 67 helped, 0 hurt; code size -12.1 %; samples 1411 -> 1403; no spills. Application shaders: 54 stages 119382 -> 100611 (-15.7 %), 0 hurt; `vkbench`
shaders -28.5 %; probe -11.2 % (both unchanged by the review fixes). Registers: total temps 2377 -> 2373 (9 shaders better, 4 worse by 1-2: the 4-register
`itrsmp` destination of a 1-channel read, one `+1` from the phi change, one in probe T2). Per flag on the new
build: `no_imadd_addr` 47 changed (-5189 groups), `no_vreg_coalesce` 22 changed (-3210), before the uniform commits, co-issue alone gave -7909 groups, `itrsmp` alone -186, both -7952 (the two overlap by about
140 groups). Sanitizer build (debug,
asserts and NIR validation on, ASan and UBSan): same 88 pipelines, run again on the final head, no ASan error, no leak, no assertion or
validation failure; the only UBSan message is `applying zero offset to null pointer` (empty `util_dynarray`
loops), output byte-identical to the release build; with all six flags set the release and the sanitizer outputs equal `41bad10`'s byte for byte.

**GPU, 3D job ms per pass, old (`41bad10`) against new, GLES for reference** (median [min-max]):

| mode | old | new | change | GLES | old/GLES | new/GLES |
|---|---|---|---|---|---|---|
| fill | 0.195 [0.194-0.327] | 0.196 [0.195-0.197] | +0.5 % (overlap; one old outlier) | 0.183 | 1.07 | 1.07 |
| blend | 0.452 [0.451-0.454] | 0.410 [0.407-0.412] | -9.3 % | 0.311 | 1.45 | 1.32 |
| disc | 0.358 [0.350-0.611] | 0.317 [0.308-0.512] | -11.5 % (overlap; one outlier run each) | 0.310 | 1.15 | 1.02 |
| tv | 0.265 [0.261-0.273] | 0.256 [0.253-0.435] | -3.4 % (overlap; one new outlier, other 4 runs 0.253-0.258) | 0.289 | 0.92 | 0.89 |
| alu | 4.940 [4.940-4.940] | 3.333 [3.332-3.333] | -32.5 % | 3.159 | 1.56 | 1.06 |
| ubo | 24.713 | 24.613 | -0.4 % | 0.952 | 25.96 | 25.85 |

Wall ms per pass agree (fill -6.4 %, blend -8.5 %, disc -11.1 %, tv -2.2 %, alu -32.1 %, ubo -0.4 %). The
vendor's fragment code for the same shaders has fewer groups still (blend 4 against our 5, `tv` 2 = ours, `alu` 130
against 136), so group counts alone do not predict the time: `blend` is 1.32x GLES at 5 groups.

Probe wall time (cold, 5 repeats, old -> new; T1 ms per pass, T2 ms per 1000 draws, T3 ms per chain, T4 ms per
frame): T1 1.494 -> 1.298 (-13.1 %), T2 instanced 9.619 -> 8.291 (-13.8 %), T3 separate 10.745 -> 8.284 (-22.9 %),
T3 subpass 11.232 -> 9.127 (-18.7 %), T4 63.739 -> 63.717 (no change). T2 with individual draws (CPU-bound
submission) moved with the phone's CPU state between sessions (old 16.8-23.4, new 20.8-21.0 in the later one; in the
earlier session 15.7 -> 14.1) and cannot be judged as a before/after without a quiet or pinned CPU; a three-way
check in one session (old, the head before the uniform commits, new) put the last two equal, so the uniform commits
add no draw cost.

**Per-render cost.** At 680 MHz a 512x512 fill render costs: kick 0.061 ms CPU in the ioctl and 0.097 ms per
`vkQueueSubmit` in all; TA job 0.163 ms (GLES 0.039; 0.139 after the smaller dummy object); firmware hand-over
TA to 3D 0.075 ms in both APIs (not ours to cut); 3D 0.195 ms. The vendor driver batches 20-65 passes into one 3D
job, we run one job per pass. A chain of small renders is bound by a firmware loop: the next TA starts about
0.25 ms after the 3D job two renders earlier ends, whatever the CPU does, so one more small render costs about
0.23-0.25 ms and one more big render about 0.06 ms. The port should therefore avoid many small render passes. Our TA time
beyond the dummy object (about 0.10 ms at 512x512, growing with the tile count) is not explained; the render target
dataset parameters are the same as the vendor's.

**dEQP-VK, old driver against new, per test** (`results.csv` diffed, skips file, baseline failures): `glsl.builtin` full
(2131: 821 Pass, 37 Fail, 1273 Skip on both; the 37 are old-driver failures) 0 changed; `ubo` full (9115) 0;
`binding_model` 1/50, `renderpass` 1/5 (3173), `draw` 1/10 (3045), `glsl.operator` 1/3 (1210),
`glsl.texture_functions` full (3672), `api` 1/100, `synchronization` 1/20, `compute` 1/10: 0 changed on the head;
`fragment_operations` and `glsl.discard` full: 0 changed on the board before the uniform commits (not re-run). Before the fix, `glsl.builtin` 1/5 had 4 Pass -> Fail (`precision.modf.highp.vec4`,
`modfstruct.mediump.vec2`, `roundeven.mediump.scalar`, `roundeven.mediump.vec4`); the earlier subset held only the
`common` functions and missed them (the full `builtin` list is run now). A co-issue pair with coefficient
registers also failed `draw.*.linear_interpolation.offset_*` before `026b8c4`. `vkbench` sums (15 modes, 20 passes)
identical for old, new and new with the five debug flags of the uniform and co-issue commits set; probe
PIXELS identical (T1 198639616, T2 282240902, T3 216989696, T4 33423360). HWR counters did not change in any
session. The 20 GPU recoveries on the board came from one release build that encoded invalid ALU operands (see the
`9cd563b` entry), not from the shipped code.

**Review fixes after the board** (an independent read of the 19 commits; `CHANGELOG.md`):

- Co-issue: vertex-input and shared registers are no longer phase 1 (S3-S5) sources. The coefficient-bank bug had
  shown that the upper source group's banks can read wrong data silently, and these two classes had never been
  read there by a shader checked on the device (the checked `vkbench` and probe pairs use constants only, which
  stay allowed). Cost: game shaders +657 groups (+0.7 %, 25 shaders, 0 helped), `vkbench` and probe shaders
  unchanged. Allowing temps only would have cost +1.3 % and undone `blend` 5 -> 7 groups (probe T1 FS 16 -> 21;
  probe T1 1.49 against 1.30 ms per pass on the phone), so the narrower restriction is used. Probe T1 on the
  final build 1.300 against 1.298 ms per pass (old).
- Pipeline cache: a hit is rejected when its stored binding kinds differ from the layout's. On the phone, `vkbench`
  `ubo` and `blend` with a `VkPipelineCache` saved and reloaded in a new process: 25 of 25 pipelines found, sums
  unchanged (the rejection path itself was not exercised).
- The render kick's RETRY loop ends after 10 s (compute and transfer kicks: still unbounded) with `VK_ERROR_DEVICE_LOST` (not triggered on the phone, which never
  reached it); the dummy-object buffers are freed when the static clear state fails to initialise.
- A render with no draw keeps the whole render area, although the 32x32 dummy object touches only the origin
  tile: `vkbench` 512x512, 20 passes, clear and store without a draw: sum 66846720 = every pixel (old and new);
  `load` with a draw in the first pass only (`BENCH_NODRAW=2`) and 19 load-and-store passes without a draw:
  133955584, the same as one draw, and `blend` 83886080 (old and new); HWR 3D 1165 / TA 4 / events 1169 before
  and after.
- Re-run on the final build: `vkbench` sums of 15 modes identical old (`437ccca`) / new / new with the six flags;
  probe PIXELS identical (T1 198639616, T2 282240902, T3 216989696, T4 33423360); dEQP per test, old
  (`437ccca`) against new: `draw` 1/10 (3045, includes 5 `linear_interpolation` cases, 5 Pass 4 Skip), `renderpass`
  1/5 (3173), `api` 1/100 (2997), `glsl.operator` 1/3 (1210), `glsl.builtin` full (2131: 821 Pass, 37 Fail, 1273 Skip
  on both): 0 changed in each. HWR unchanged. No dEQP pipeline-cache subset was run; the cache check above is
  `vkbench`'s.

**Flags against the old compiler.** `PCO_DEBUG=no_coissue,no_fwdfix,no_itrsmp,no_imadd_addr,no_vreg_coalesce,no_dynoff_skip`
on the new build gives the same compiler output as `41bad10` for the whole corpus (byte for byte).

**Vendor code, in words.** For the same shaders the vendor's fragment code puts `fract` in one `fadd` with both
source modifiers, reads an interpolated texture coordinate with one fused instruction that carries its own wait,
and keeps constants in registers; the first two are done now. The vendor's blend shader is one group shorter than
ours.

**Known limits and open.**

- The `itrsmp` instruction runs with the extended execution condition the vendor's code also sets: lanes the
  condition masks write a dead temporary, and an `if` that contains a fused read cannot be turned into predicated
  execution. Not fixed.
- The `imadd64` buffer address zero-extends the offset: offsets from 2^31 up are used as unsigned, which is what
  Vulkan buffer offsets are; a negative offset cannot occur.
- Co-issue allows constants in S3-S5 on the strength of the checked `vkbench` and probe shaders (they read them there
  with unchanged sums), not of a per-register-number test; vertex-input and shared registers there would need
  a pair-counter bisect on the device.
- Validation layers (core, then synchronization) were not run on the probe and `vkbench`: the phone has no
  loader chaining for them.
- The plain debug build without sanitizers was not evaluated separately; the sanitizer build is a debug build with
  asserts on.
- T2 with individual draws is CPU-bound and session dependent (above).
- `ubo`: one 512x512 pass costs 24.6 ms on Vulkan, 26x the vendor's GLES twin (same shader and data), with old and new
  driver alike, so none of the uniform commits is visible in it. This is the largest gap left at the end of the
  board; it is addressed on a later board.
- The `itrsmp` result of a 1-channel read keeps four registers; the copies of a vector's components into phi
  registers are not coalesced (they need the register allocator); the TA time beyond the dummy object is unexplained.

Drafted with an LLM; marker word per AGENTS.md: biblioklept.
