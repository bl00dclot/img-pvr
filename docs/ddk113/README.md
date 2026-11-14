# img-pvr on DDK 1.13 (`ddk113` branch)

This fork ports img-pvr (Mesa's Imagination PowerVR Vulkan driver, on Mesa 27.0.0-devel) from the DDK 1.11
kernel interface to **DDK 1.13ED5776728**. The target is the Redmi A2. Other MT6765 phones whose kernel driver is DDK 1.13@5776728 may work too, but
only the A2 is being tested.

**Status: work in progress.** On the Redmi A2 the driver enumerates the GPU, creates a Vulkan 1.3 device and allocates memory (`TESTING.md` L0, L1 and the allocation part of L2). Compute dispatches and transfers (fill, copy, clear, image readback) run correctly in native root runs (`TESTING.md` L3, L4; not yet repeated inside an app). Render (TA/3D) submission is still deliberately refused until its DDK 1.13 command layouts are confirmed (`CAPTURE.md`), so nothing renders yet.

## Target

| | |
|---|---|
| GPU | PowerVR Rogue GE8320, BVNC 22.87.104.18 (the same GPU img-pvr supports on DDK 1.11) |
| Kernel driver | Imagination DDK 1.13@5776728, DRM node name `pvr`, plus `/dev/pvr_sync` |
| Userspace | 32-bit armeabi-v7a, with the app on an arm64 kernel (compat bridge) |
| Requirements | no root, no custom kernel, nothing flashed. On the test device (2026-11-14), an ordinary app reached `/dev/dri/renderD128` and `/dev/pvr_sync` with no SELinux denial in the system log. See `TESTING.md` for how this was checked |

## Why 1.13 needs its own port

The three DDK versions differ in their bridge IDs, bridge structs and firmware command layouts. The bridge IDs were extracted from the 1.13 kernel's `common_*_bridge.h` and upstream's
`pvr_srv_bridge.h`:

- **Upstream 1.17 vs 1.13:** of the 35 bridge functions upstream Mesa's DDK 1.17 winsys calls, 32 have the same
  ID in 1.13. Only 12 have the same ID in 1.11.
- **Same object model as 1.17:** 1.13 already uses HWRTDataSet, KickTA3D2, KickCDM2 and has no server syncs.
- **Approach:** start from upstream's 1.17 winsys, apply the measured 1.13 differences, and keep img-pvr's
  GE8320 and Android fixes.

Details and citations are in [`ABI.md`](ABI.md).

## Documents

| File | Contents |
|---|---|
| [`ABI.md`](ABI.md) | field-by-field comparison of DDK 1.13 and 1.17 for bridges, connect, firmware structs and sync, with `file:line` on both sides |
| [`CAPTURE.md`](CAPTURE.md) | how the firmware command-stream layouts the kernel never sees are pinned down, and what was found |
| [`TESTING.md`](TESTING.md) | the bring-up ladder and its results |
| [`CHANGELOG.md`](CHANGELOG.md) | every change on this branch, each with its evidence |

## Building

Not yet: Phase 1 adds a reproducible Docker build with the meson option `-Dpvr-ddk=1.13`. The existing
`build.sh` still builds the DDK 1.11 driver unchanged.

## Authorship and upstream policy

The changes on this branch are written with an AI assistant (an LLM), and every such commit carries the
trailer `Generated-by: LLM`. Mesa's `AGENTS.md` and the Imagination
[contribution policy](../../src/imagination/README.md#contribution-policy) decline LLM-generated contributions
upstream. For that reason **nothing from this branch is, or will be, submitted to Mesa**. It is a personal
fork, used under the project's MIT licence.

## Security

See [`SECURITY.md`](../../SECURITY.md). Planned: compile-time size checks on every kernel-facing structure. Planned: capture tooling under
`tools/` (it does not exist yet), which will need root, is meant for development only, and will never be
part of a release.
