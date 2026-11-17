# img-pvr on DDK 1.13 (`ddk113` branch)

This fork ports img-pvr (Mesa's Imagination PowerVR Vulkan driver, on Mesa 27.0.0-devel) from the DDK 1.11
kernel interface to **DDK 1.13ED5776728**. The target is the Redmi A2. Other MT6765 phones whose kernel driver is DDK 1.13@5776728 may work too, but
only the A2 is being tested.

**Status: work in progress; renders correctly, slower than the vendor driver.** On the Redmi A2 the
driver enumerates the GPU, creates a Vulkan 1.3 device and runs transfers, compute dispatches and render
passes, including partial renders (`TESTING.md` L0–L7). All four DDK 1.13 command layouts are decoded
(`CAPTURE.md`).

- **Correctness:** test images equal the vendor GLES driver's on the native tests and on probe tests T2
  and T4; T1 is within 1 % and T3 differs (whole-image sum; at the centre only green). A dEQP-VK subset
  gives 8,443 passes and 229 known failures, kept as a baseline in `src/imagination/ci/`.
- **Stability:** GPU recovery counters stayed unchanged in the native runs and in four of five in-app
  probe runs (one TA lockup, unexplained), and rose during the dEQP-VK `api` and `renderpass` subsets.
- **Speed:** measured at one GPU clock (680 MHz; `TESTING.md`, 2026-11-16 and 2026-11-17 entries), the GPU time
  of a 512x512 pass against the vendor GLES driver after board 3 is 1.07x for a plain fill, 1.02x with discard,
  1.32x for blending over loaded contents, 1.06x for an ALU-heavy shader and 0.89x for a texture read. A shader
  that reads 28 scalars from a uniform buffer in 12 branches (`vkbench ubo`) was 26x slower than GLES; board 4
  reads such values from shared registers and brings it to 4.2x, still the largest gap (GLES branches where this
  compiler predicates; `TESTING.md`, board 4). On the probe, the render-pass chains are 13-23 % faster than before board 3 (T1 -13 %, T3 -19
  to -23 %, instanced draws -14 %); the probe has not been compared with GLES at one clock since 2026-11-15
  (then 2.3-3.9x slower, mixed clock levels); uploads are 1.2-1.4x faster. "Next for speed" below lists what
  remains.

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

```
DDK=1.13 ARCHES=armv7 scripts/docker-build.sh <android-ndk-r27c-dir>
```

builds in the digest-pinned image of `docker/Dockerfile` (the NDK is mounted read-only) and leaves
`dist/img-pvr-<version>-<commit>-armv7-ddk113.zip` (`meta.json`, `libvulkan_powervr.so`, `NOTICE.txt`).
`DDK=1.11` (the default) builds the original driver; the meson option is `-Dimagination-srv-ddk=1.11|1.13`.
A pushed `ddk113-v*` tag builds the same zip in GitHub Actions and publishes it as a prerelease with its
SHA-256 and a build-provenance attestation (`.github/workflows/ddk113-release.yml`). The only release so
far, `ddk113-v0.1.0`, is from before the speed work.

## Loading

The phone has no Vulkan HAL for this driver, so a program loads the ICD itself: `dlopen` the `.so`, call
`vk_icdNegotiateLoaderICDInterfaceVersion`, then use `vk_icdGetInstanceProcAddr`. `tools/vktest/*.c` do
exactly that; `tools/vktest/vkshim.c` wraps it as a `vkGetInstanceProcAddr` library for programs that
expect a loader (dEQP-VK). Inside an app the ICD also needs a `libhardware.so` next to it.

## Tools (development only, never in a release)

| Path | What it is for |
|---|---|
| `tools/gen_bridge.py`, `tools/check_memflags.sh` | generate the 1.13 bridge header from kernel headers; check memory-flag values against both DDKs |
| `tools/pvrtrace/` | root ptrace tracer of the bridge ioctls with the kick command blobs and context state, an edit mode (`-e`), `decode.py`, and `glwork` (GLES scenes for the vendor driver) |
| `tools/vktest/` | loader-less Vulkan tests: `vkcompute`, `vktransfer`, `vkdraw` (clear, draw, big, fill), `vkshim` |
| `tools/gputrace/` | GPU job times from the kernel driver's ftrace events: `trace-session.sh on` once per phone session (a panic followed one apphint write on 2026-11-16, suspected cause a clock-correlation read without the power lock, not checked against this phone's kernel; the load during the switch is a precaution, not proven; so once per session, never per run), then `gputrace.sh` per run; `gpujobs.py` and `bench_table.py` on the host. `BENCH_WALL=<ms>` in `vkbench`/`glwork` warms the GPU up to 680 MHz first (`TESTING.md`) |

## Next for speed

Measured causes are in `TESTING.md` ("speed work"). In order of expected gain, as of 2026-11-15; the
same-clock measurements of 2026-11-16 put texture reads at parity with GLES and leave fill, blending, discard
and ALU as the gaps (`TESTING.md`, board 2, "Open"); board 3 closed most of those; board 4 cut the uniform-buffer gap from 26x to 4.2x, and the rest of it is
predicated small branches (`TESTING.md`, board 4):

1. **Half-float texture results for relaxed-precision shaders.** Each 32-bit channel a texture read returns
   costs GPU time per fragment. Imagination's precision guide says Rogue stores `lowp` and `mediump` as
   16-bit floats and that samplers default to `lowp`, which is how the vendor GLES driver's reads stay
   cheap. Mesa has the pieces (`nir_lower_mediump_*`, `nir_opt_16bit_tex_image`) and the sampler
   instruction has an `f16` mode that this compiler does not use yet; an application must then mark its
   shaders `mediump` (SPIR-V `RelaxedPrecision`). Not implemented, gain not measured.
2. **The attachment load shader and float blending** (probe T1): a leaner load program for known formats.
3. **Discard shaders** (probe T2): the punch-through path, and the pixel-merge render flag Mesa always
   disables (clearing it gained 16 % on T2; its safety conditions are not known).
4. **The kernel kick:** about 15 % of the CPU time per render.

Application side, from Imagination's render-pass guidance: few render passes, `CLEAR`/`DONT_CARE` load
ops and `DONT_CARE` store ops wherever contents are not needed, subpasses and transient attachments to
keep data on-chip.

## Authorship and upstream policy

The changes on this branch are written with an AI assistant (an LLM), and every such commit carries the
trailer `Generated-by: LLM`. Mesa's `AGENTS.md` and the Imagination
[contribution policy](../../src/imagination/README.md#contribution-policy) decline LLM-generated contributions
upstream. For that reason **nothing from this branch is, or will be, submitted to Mesa**. It is a personal
fork, used under the project's MIT licence.

## Security

See [`SECURITY.md`](../../SECURITY.md). Every bridge structure has a compile-time size check, and every firmware command structure a size or
offset check. The
tools above need root, are for development devices only and are never part of a release; raw captures and
vendor binaries are never committed. Drafted with an LLM; marker word per AGENTS.md: biblioklept.
