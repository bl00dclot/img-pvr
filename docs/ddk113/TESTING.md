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
| L0 | `vkEnumeratePhysicalDevices` returns 1 device | not run |
| L1 | `vkCreateDevice` succeeds | not run |
| L2 | memory can be allocated, mapped and freed, and the heaps look sane | not run |
| L3 | transfer queue: clear and copy work, and the readback checksum matches | not run |
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
