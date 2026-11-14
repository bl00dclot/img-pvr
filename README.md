# PowerVR Vulkan for Rogue GPUs on DDK 1.11

A port of Mesa's Imagination PowerVR Vulkan driver to Rogue GPUs running Imagination's DDK 1.11
kernel driver on stock Android, based on Mesa 27.0.0-devel. Upstream Mesa only talks to the DDK
1.17 kernel interface; this tree adds the 1.11 bridge calls, firmware command layouts and job
setup, so it runs on the kernel driver the phone already has (`/dev/dri/renderD128`) and needs
no root and no custom kernel.

Tested on the PowerVR GE8320 (Redmi 9C, MediaTek Helio G35), BVNC 22.87.104.18, kernel driver
1.11.5425693.

> **This fork:** the `ddk113` branch ports this driver to the DDK 1.13ED5776728 kernel driver, targeting
> the Redmi A2. It is untested work in progress, written with an AI assistant. Everything below this note
> describes the original DDK 1.11 release on the Redmi 9C, not the `ddk113` branch.
> See [`docs/ddk113/`](docs/ddk113/README.md).

## What works

- Vulkan 1.3: draws, compute, transfers, render passes and dynamic rendering.
- Presentation through Android's gralloc (`VkNativeBufferANDROID`).
- 32-bit (armeabi-v7a) and 64-bit (arm64-v8a) builds. Many phones with this GPU ship 32-bit
  userspace only.
- Minecraft Java through MojoLauncher's Vulkan renderer.

Not done yet: geometry shaders and tessellation.

## Requirements

Tools:

| Tool | Version | Notes |
|---|---|---|
| Linux or WSL | | the host tools need LLVM |
| Android NDK | r29 tested | the API 29 armv7a and aarch64 compilers and `llvm-strip` |
| Meson | 1.4.0 or newer | |
| Ninja | any recent | |
| Python | 3.10 or newer | with `mako`, `packaging` and `pyyaml` |
| flex and bison | any recent | |
| pkg-config | any recent | |
| LLVM, clang, libclc, SPIR-V LLVM translator | one major version, 21 tested | for the host tools |

On Ubuntu:

```sh
sudo apt install meson ninja-build pkg-config flex bison python3-mako python3-packaging \
    python3-yaml llvm-21-dev libclang-21-dev clang-21 libclc-21-dev libllvmspirvlib-21-dev
```

Dependencies are fetched by Meson on the first configure, so that step needs network access:

| Library | Version | Use |
|---|---|---|
| libdrm | 2.4.133 | linked statically |
| zlib | 1.3.1 | linked statically |
| Expat | 2.5.0 | required by the configure step, not linked into the driver |

No Android platform libraries are needed: the build uses Mesa's Android stubs.

## Building

```sh
./build.sh /path/to/android-ndk
```

The NDK path can also come from `ANDROID_NDK_HOME` or `ANDROID_NDK_ROOT`. The script first builds
four host tools into `build-host/` (`mesa_clc`, `vtn_bindgen2`, `pco_clc`, `pdsc_precomp`: the
driver's built-in shaders and PDS programs are compiled with them, so they come from this tree).
It then builds `libvulkan_powervr_mesa.so` for each architecture into `build-armv7/` and
`build-arm64/` (set `ARCHES` to build only one), strips it, and writes one zip per architecture
to `dist/`. Each zip holds `meta.json`, `libvulkan_powervr.so` and `NOTICE.txt`, the layout
Android launchers with custom driver support import.

## Disclaimer

This project was developed with heavy use of AI tools. Every change in the DDK 1.11 release was built and
tested on the device above before it was published. The `ddk113` branch of this fork is untested work in
progress; [`docs/ddk113/TESTING.md`](docs/ddk113/TESTING.md) shows what has run.

## License

The changes are MIT licensed (`LICENSE`). Files from Mesa keep the license in their headers,
mostly MIT; `THIRD_PARTY_NOTICES.md` lists the files under other licenses and `LICENSES/` has the
full texts. The notices for everything compiled into the driver ship in the package as
`NOTICE.txt`.

This project is not affiliated with or endorsed by Imagination Technologies, MediaTek, Xiaomi or
the Mesa project. PowerVR is a trademark of Imagination Technologies Limited.
