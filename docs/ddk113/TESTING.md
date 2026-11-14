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
| L4 | compute: a trivial dispatch writes a buffer | not run |
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
- **SELinux:** the log has no `avc: denied` lines at all, so `/dev/pvr_sync` is usable from the app sandbox.
- **L2:** memory has 1 heap of 1400 MiB and memtype 0 with flags `0x7` (device-local, host-visible,
  host-coherent). Image and buffer allocations succeed.
- **L3:** the first submission (a transfer) stops at the gate, as intended:
  `the transfer command layout is not confirmed yet, nothing submitted (docs/ddk113/CAPTURE.md)`.
- **GPU state:** the `/proc/pvr/debug_dump` HWR counters were identical before and after (TA 164, 3D 101; the
  phone's own history). The firmware state was `OK`.
