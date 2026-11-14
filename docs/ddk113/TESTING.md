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
| L3 | transfer queue: clear and copy work, and the readback checksum matches | **blocked on purpose**: the 1.13 build refuses to submit until the command layouts are confirmed |
| L4 | compute: a trivial dispatch writes a buffer | **pass** (2026-11-14, native root run): 200/200 dispatches correct, no HWR |
| L5 | render: a clear, then a triangle, then probe tests T1–T4, with image checksums matching the GLES run | not run |
| L6 | 200 frames with no HWR increase in `/proc/pvr/debug_dump` | not run |
| L7 | a dEQP-VK smoke subset passes | not run |
| L8 | numbers against the GLES baseline | not run |

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
