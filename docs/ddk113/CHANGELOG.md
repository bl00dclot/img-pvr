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
  - Result: 206 structs for 1.13, each with a `_Static_assert` on its size. These hold with NDK r27c armv7
    clang.
  - IDs match `ABI.md`: `KICKTA3D2`=13, `KICKCDM2`=6, `PHYSMEMEXPORTDMABUF`=1, `GETMULTICOREINFO`=12.
  - Not yet used by any code.
- **P1: reproducible build.**
  - `docker/Dockerfile` uses Debian trixie pinned by digest. `scripts/docker-build.sh` mounts the NDK
    read-only.
  - The new meson option `imagination-srv-ddk` (`1.11` default, or `1.13`) selects `pvr_srv_bridge.c` or
    `pvr_srv_bridge_ddk113.c` and defines `PVR_SRV_DDK`.
  - The `build.sh` knob `DDK=1.13` builds into `build-<arch>-ddk113` and packages with a `-ddk113` suffix.
  - Gate passed: the unchanged 1.11 build (NDK r27c, LLVM 19) exports `vk_icd*` and `HMI`. Its NEEDED list is
    identical to v1.0.0. On the DDK 1.13 phone it behaves exactly like the release: ICD loads, interface
    version 5, `pvr 1.13.5776728`, 0 devices.
