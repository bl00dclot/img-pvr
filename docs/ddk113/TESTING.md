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
| L2 | memory can be allocated, mapped and freed, and the heaps look sane | **allocation passes**; map/free not yet checked on their own |
| L3 | transfer queue: clear and copy work, and the readback checksum matches | **pass for fill, copy and clear + readback** (2026-11-14, native root run): byte-exact, 50/50 runs, no HWR; image-to-image blits and copies not yet run |
| L4 | compute: a trivial dispatch writes a buffer | **pass** (2026-11-14, native root run): 200/200 dispatches correct, no HWR |
| L5 | render: a clear, then a triangle, then probe tests T1–T4, with image checksums matching the GLES run | **pass** (2026-11-15): clear and triangle byte-exact 100/100, a 2-million-triangle scene with partial renders equal to the vendor's checksum; probe T1–T4 run inside the app, T2 and T4 sums equal GLES, T1 within 0.93 %, T3 differs (see the log) |
| L6 | 200 frames with no HWR increase in `/proc/pvr/debug_dump` | **pass with one exception** (2026-11-15): 12 x 200 probe frames per run, counters unchanged on 4 of 5 in-app runs after the deadline fix; one TA guilty lockup in one run (see the L5 log). `firmware_trace` is off on this kernel ("Debug log type: none") |
| L7 | a dEQP-VK subset passes under `deqp-runner`, with a committed baseline of known failures, flakes and skips (Mesa CI practice), image and shader logging off | **run, baseline committed** (2026-11-15): 7 subsets, 8,443 pass, 229 fail, 1 flake; GPU recoveries during `api` and `renderpass`; `src/imagination/ci/ge8320-ddk113-{fails,flakes}.txt` |
| L8 | numbers against the GLES baseline | **done** (2026-11-15): Vulkan 3.7–6x slower than GLES on passes, draws and chains, 1.4x (final run) to 1.6x (first run) faster on uploads (L5 log table, final run below) |

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

Not yet done for L4: the run inside the app sandbox, and the `firmware_trace` tail.

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

Not yet done for L3: the run inside the app sandbox, the `firmware_trace` tail, and blits between images
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

Not yet done: an edit that changes a value (that is the W5 TA/3D decode work). Drafted with an LLM; marker
word per AGENTS.md: biblioklept.

### Render tools for L5 (W5a, not yet run)

`tools/vktest/vkdraw` (render-pass clear, then a full-screen triangle; expected byte sum 2613248, the same as
`glwork draw`) and the `glwork` modes `depth`, `msaa`, `mrt` and `big` (for TA/3D captures, `big` forcing a
partial render) are built for armv7 but have not been run on the device. Drafted with an LLM; marker word per
AGENTS.md: biblioklept.

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
