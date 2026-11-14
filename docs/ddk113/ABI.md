# DDK 1.13 ABI: Mesa pvrsrvkm winsys (DDK 1.17) vs the DDK 1.13ED5776728 kernel module

Target: PowerVR GE8320, BVNC 22.87.104.18, kernel driver DDK 1.13@5776728 (Redmi A2, MT6765). This is a
read-only source comparison: nothing was compiled or run to produce it.

Path roots (every `file:line` below is relative to one of these):

- `KM:` is `drivers/gpu/mediatek/gpu_rgx/m1.13ED5776728/` in
  [Teracube-Inc/kernel_teracube_emernia-4.19@8267f823](https://github.com/Teracube-Inc/kernel_teracube_emernia-4.19/tree/8267f823e1dab1357b9eb201e62b3171aa6fc340/drivers/gpu/mediatek/gpu_rgx/m1.13ED5776728).
  This is the truth for the kernel ABI.
- `UP:` is `src/imagination/vulkan/winsys/pvrsrvkm/` in
  [mesa/mesa@20b42f44](https://gitlab.freedesktop.org/mesa/mesa/-/tree/20b42f446667d01d7861b52f667bcd6b1569b9c2/src/imagination/vulkan/winsys/pvrsrvkm),
  the upstream DDK 1.17 winsys.
- `KM111:` is `drivers/gpu/mediatek/gpu_rgx/m1.11ED5425693/` in
  [MediaTek-Labs/common-kernel-4.19@e440f21f](https://github.com/MediaTek-Labs/common-kernel-4.19/tree/e440f21fa5dca203a027f31c2ed30af50e6b05c7/drivers/gpu/mediatek/gpu_rgx/m1.11ED5425693),
  used for context only.

Method: structs were parsed mechanically and each field was mapped by name and meaning. Sizes assume the packed
layout, since both sides are `PACKED` or `__attribute__((packed))`.

## 0. Headline findings

1. **Bridge IDs**: 32 of 35 identical (extracted from `KM:generated/rogue/*/common_*_bridge.h` and `UP:pvr_srv_bridge.h`). Differing: EXPORTDMABUF 2 vs 1, KICKCDM2 5 vs 6, KICKTA3D2 12 vs 13.
2. **Bridge structs**: 13 IDENTICAL, 22 FIELD-DIFF, 0 MISSING-IN-1.13. The IMG generator in 1.17 sorts fields by size (u64, pointers, u32, u8); 1.13 keeps declaration order. Field order differs in 21 of the 22 FIELD-DIFFs (12 of them differ in order only). On top: `flags` is u32 in 1.13 (4 functions), 5 functions have a different field set, CREATEHWRTDATASET is structurally different.
3. **Transport**: 1.13's DRM driver has exactly one ioctl (`DRM_PVR_SRVKM_CMD`, nr 0). Upstream's `DRM_SRVKM_INIT` (nr 5) and the three SW-sync ioctls (nr 2/3/4) do not exist in 1.13. `DRM_IOCTL_SRVKM_CMD` itself is identical (same nr, same 32-byte struct).
4. **Connect**: version must equal 1.13 exactly (hard fail); build number is only a warning; build options only need bit 4 set. Upstream's field order in the CONNECT in/out structs is wrong for 1.13.
5. **Hard failures upstream would hit on 1.13** (each with citations below): `DRM_IOCTL_SRVKM_INIT` fails; CONNECT in/out misparsed; render context create rejected because the static state blob is 368 B vs the 184 B limit (`KM:generated/rogue/rgxta3d_bridge/server_rgxta3d_bridge.c:960-967`, `KM:services/server/devices/rogue/rgxta3d.c:2883`); timelines opened on the render node are rejected by the sync driver (`KM:kernel/drivers/staging/imgtec/pvr_sync_file.c:145-160,310-322`).
6. **FW interface**: of 27 upstream structs, 6 identical, 9 different, 12 UM-ONLY (no definition in the 1.13 kernel tree). The TA/3D shared block differs (no frame-number word in 1.13), which shifts `hw_rt_data` and the PR buffer pointers that the kernel patches in.
7. **Sync**: payloads identical, transport different (`/dev/pvr_sync` with `'W'` ioctls instead of DRM ioctls on a re-opened render node). Fence fds consumed by `pvr_srv_sync.c` need no change.


## 1. Transport layer (DRM ioctls) and bridge dispatch

| Item | 1.17 upstream | 1.13 kernel | Verdict |
|---|---|---|---|
| Bridge ioctl request | `DRM_IOWR(DRM_COMMAND_BASE+0, struct drm_srvkm_cmd)` `UP:pvr_srv_bridge.h:101,114-115` | `DRM_IOWR(DRM_COMMAND_BASE+0, struct drm_pvr_srvkm_cmd)` `KM:include/drm/pvr_drm.h:76,80-82` | IDENTICAL (same nr, same size 32) |
| ioctl struct | `{u32 bridge_id; u32 bridge_func_id; u64 in_data_ptr; u64 out_data_ptr; u32 in_data_size; u32 out_data_size}` `UP:pvr_srv_bridge.h:834-841` | same six fields, same order `KM:include/drm/pvr_drm.h:63-70` | IDENTICAL |
| Module init ioctl | `DRM_IOCTL_SRVKM_INIT` = `DRM_IOWR(BASE+5, struct drm_srvkm_init_data{u32 init_module})`, called with `PVR_SRVKM_SERVICES_INIT=1` before CONNECT `UP:pvr_srv_bridge.h:111,124-125,844-848`, `UP:pvr_srv_bridge.c:75-88`, `UP:pvr_srv.c:724` | not present. `pvr_drm_ioctls[]` has one entry `DRM_IOCTL_DEF_DRV(PVR_SRVKM_CMD,...)` `KM:kernel/drivers/staging/imgtec/pvr_drm.c:248-250`; `pvr_drm.h` defines only `DRM_PVR_SRVKM_CMD 0` `KM:include/drm/pvr_drm.h:76` | MISSING-IN-1.13. Expect the DRM core to return an error for nr 5 (exact errno unverified, DRM core code not in the tree). The connection is created on `open()` (`pvr_drm_open` -> `PVRSRVDeviceOpen`, `KM:kernel/drivers/staging/imgtec/pvr_drm.c:219-232`), so no init call is needed. |
| SW-sync ioctls on the render fd | `DRM_IOCTL_SRVKM_SYNC_FORCE_SW_ONLY_CMD` (BASE+2), `..._SW_SYNC_CREATE_FENCE_CMD` (BASE+3), `..._SW_SYNC_INC_CMD` (BASE+4) `UP:pvr_srv_bridge.h:104-123` | not present (see section 5) | MISSING-IN-1.13 |
| DRM driver name/version | `drmGetVersion` must be `1.17` `UP:pvr_srv.c:684-710` | `.name = "pvr"`, `.major = PVRVERSION_MAJ (1)`, `.minor = PVRVERSION_MIN (13)`, `.patchlevel = 5776728` `KM:kernel/drivers/staging/imgtec/pvr_drm.c:299-304`, `KM:include/pvrversion.h:47-52` | version gate must be changed to 1.13 |
| Bridge buffer checks | n/a | The dispatcher only checks `in/out_data_size <= PVRSRV_MAX_BRIDGE_IN/OUT_SIZE` and copies exactly `in_data_size` bytes in and `out_data_size` bytes out; it never compares sizes with `sizeof(struct)` `KM:services/server/common/srvcore.c:1238-1270,1306-1315` | A wrong-sized struct is not rejected, it is silently misparsed. Handlers that take arrays do bound-check the counts (e.g. `PVRSRV_MAX_SYNCS = 12`, `KM:include/public/powervr/pvrsrv_sync_ext.h:40-50`; `RGXFWIF_DM_INDEPENDENT_KICK_CMD_SIZE = 1024`, `KM:include/rogue/rgx_fwif_shared.h:71`). Both match upstream (`UP:pvr_srv_bridge.h:154` `PVR_SRV_SYNC_MAX 12`, `UP:fw-api/pvr_rogue_fwif_shared.h:49,59`). |
| `PVRSRV_ERROR` values used by upstream | `OK=0, RETRY=25, BRIDGE_CALL_FAILED=37` `UP:pvr_srv_bridge.h:179-184` | `PVRE()` list index: RETRY=25, BRIDGE_CALL_FAILED=37 (also DDK_VERSION_MISMATCH=26, BUILD_OPTIONS_MISMATCH=28) `KM:include/pvrsrv_errors.h:70,82`, enum `KM:include/pvrsrv_error.h:47-59` | IDENTICAL for the 3 values used |

Bridge function IDs: all 35 were extracted from both sides and compared; the three differences are listed in the table of section 2 (`id` column).

## 2. Bridge structs (task 1)

Verdict counts over the 35 functions upstream calls: **IDENTICAL 13, FIELD-DIFF 22, MISSING-IN-1.13 0.**

IDENTICAL means: same field set, same order, same widths, same offsets, for both the IN and OUT struct. For functions with no input, upstream sends `NULL/0` while 1.13 declares a 4-byte `ui32EmptyStructPlaceholder` (`KM:...srvcore_bridge/common_srvcore_bridge.h:105`, `...sync_bridge/common_sync_bridge.h:79`); `in_data_size = 0` means nothing is copied, so this is not a difference.

Why so many structs differ: 1.13 declares fields in the order of the C prototype (e.g. `KM:generated/rogue/srvcore_bridge/common_srvcore_bridge.h:81-86`), 1.17's generated headers sort by type size (u64/pointers first, then u32, then u8). Because the structs are packed, each reorder moves every later field. The kernel does not validate, so every reordered struct must be re-marshalled.

Classes of FIELD-DIFF (22):

- **Order only (12)**: CONNECT, GETMULTICOREINFO, ALLOCSYNCPRIMITIVEBLOCK (out), DEVMEMINTCTXCREATE (out), DEVMEMINTHEAPCREATE (in), DEVMEMINTRESERVERANGE (in), DEVMEMINTUNMAPPAGES (in), HEAPCFGHEAPCOUNT (out), HEAPCFGHEAPDETAILS, EXPORTDMABUF (out), CREATECOMPUTECONTEXT (in), CREATEFREELIST (in).
- **Order + `flags` is u32 in 1.13 (4)**: PHYSMEMNEWRAMBACKEDLOCKEDPMR, DEVMEMINTMAPPMR, DEVMEMINTMAPPAGES, PHYSMEMIMPORTDMABUF. `PVRSRV_MEMALLOCFLAGS_T` is `IMG_UINT32` unless `SUPPORT_VALIDATION || SUPPORT_SECURITY_VALIDATION` (`KM:include/pvrsrv_memallocflags.h:56-62`); the only `SUPPORT_VALIDATION` reference in the tree is a Kbuild conditional (`KM:services/server/env/linux/Kbuild.mk:451`), so u32 is expected. The MTK build of this driver (`KM:Makefile:29-35` selects `KM:include/config_kernel_user_mt6765.h`, or `config_kernel_eng_mt6765.h`, which differs only by `#define MTK_DEBUG` at line 170) defines neither macro (grep over both headers and `KM:Makefile`), so u32 is confirmed for this source tree (the binary on the phone is assumed to come from it: **unverified**). The bit positions of every flag upstream sets (GPU R/W bits 0/1, CPU R/W bits 4/5, GPU cache mode 8-10, CPU cache mode 11-13, KERNEL_CPU_MAPPABLE 14, ZERO_ON_ALLOC 31, device flag bit 26) match `KM:include/pvrsrv_memallocflags.h:124,154,190,202,261,307,333,408,434,522,777`; the 1.17-only high bits (34/35 VAL_SECURE/SHARED, 60-63 PHYS_HEAP_HINT, `UP:pvr_srv_bo.h:64-68`) do not exist in a u32 and would be truncated. The device flag `PM_FW_PROTECT` (bit 0 of the device field) is `PMMETA_PROTECT` in 1.13 (`KM:include/rgx_memallocflags.h:46`, device mask `0x0f000000` `KM:include/pvrsrv_memallocflags.h:782`).
- **Different field set (5)**: CREATETRANSFERCONTEXT (1.17 has `robustness_address`), SUBMITTRANSFER2 (1.13 has extra `ui32ClientCacheOpSeqNum`), KICKCDM2 (1.17 has `sync_pmr_flags/sync_pmrs/sync_pmr_count`, 1.13 has `ui32ClientCacheOpSeqNum`), CREATERENDERCONTEXT (1.17 has `call_stack_depth`), KICKTA3D2 (1.13 has extra `ui32ClientCacheOpSeqNum`). `ui32ClientCacheOpSeqNum` is passed to `CacheOpFence()` before scheduling (`KM:services/server/devices/rogue/rgxta3d.c:4924,5008`, `rgxcompute.c:357`, `rgxtransfer.c:1246,1294`, `KM:services/server/common/cache_km.c:2191-2221`); 0 passes the first check `CacheOpFenceCheck(completed, 0)` as far as read (semantics of the check function itself **unverified**).
- **Structurally different (1)**: CREATEHWRTDATASET (section 2.3).

### 2.1 Summary table (all 35)

`id` = group, then function id 1.17 (and 1.13 when different). UP line = struct start in `UP:pvr_srv_bridge.h`; KM line = struct start in `KM:generated/rogue/<group>/common_<group>.h`.

| # | Function (group.id 1.17 / 1.13) | 1.17 structs (UP:pvr_srv_bridge.h) | 1.13 structs (KM:generated/rogue/...) | Verdict | Difference (summary) |
|---|---|---|---|---|---|
| 1 | SRVCORE_CONNECT (grp 1, id 0) | pvr_srv_bridge.h:190 / :197 | srvcore_bridge/common_*.h:81 / 90 | **FIELD-DIFF** | IN: field order; OUT: field order |
| 2 | SRVCORE_DISCONNECT (grp 1, id 1) | pvr_srv_bridge.h:208 (ret only) | srvcore_bridge/common_*.h:103 / 109 | **IDENTICAL** | - |
| 3 | SRVCORE_GETMULTICOREINFO (grp 1, id 12) | pvr_srv_bridge.h:216 / :221 | srvcore_bridge/common_*.h:284 / 292 | **FIELD-DIFF** | IN: field order; OUT: field order |
| 4 | SYNC_ALLOCSYNCPRIMITIVEBLOCK (grp 2, id 0) | pvr_srv_bridge.h:231 (ret only) | sync_bridge/common_*.h:77 / 83 | **FIELD-DIFF** | OUT: field order |
| 5 | SYNC_FREESYNCPRIMITIVEBLOCK (grp 2, id 1) | pvr_srv_bridge.h:243 / :247 | sync_bridge/common_*.h:97 / 103 | **IDENTICAL** | - |
| 6 | SYNC_SYNCPRIMSET (grp 2, id 2) | pvr_srv_bridge.h:255 / :261 | sync_bridge/common_*.h:113 / 121 | **IDENTICAL** | - |
| 7 | MM_PMRUNREFUNLOCKPMR (grp 6, id 8) | pvr_srv_bridge.h:409 / :413 | mm_bridge/common_*.h:242 / 248 | **IDENTICAL** | - |
| 8 | MM_PHYSMEMNEWRAMBACKEDLOCKEDPMR (grp 6, id 10) | pvr_srv_bridge.h:385 / :399 | mm_bridge/common_*.h:285 / 301 | **FIELD-DIFF** | IN: width: flags u64->u32; field order; OUT: 1.17-only: out_flags |
| 9 | MM_DEVMEMINTCTXCREATE (grp 6, id 15) | pvr_srv_bridge.h:269 / :273 | mm_bridge/common_*.h:378 / 384 | **FIELD-DIFF** | OUT: field order |
| 10 | MM_DEVMEMINTCTXDESTROY (grp 6, id 16) | pvr_srv_bridge.h:284 / :288 | mm_bridge/common_*.h:397 / 403 | **IDENTICAL** | - |
| 11 | MM_DEVMEMINTHEAPCREATE (grp 6, id 17) | pvr_srv_bridge.h:330 / :337 | mm_bridge/common_*.h:413 / 422 | **FIELD-DIFF** | IN: field order |
| 12 | MM_DEVMEMINTHEAPDESTROY (grp 6, id 18) | pvr_srv_bridge.h:346 / :350 | mm_bridge/common_*.h:433 / 439 | **IDENTICAL** | - |
| 13 | MM_DEVMEMINTMAPPMR (grp 6, id 19) | pvr_srv_bridge.h:452 / :459 | mm_bridge/common_*.h:449 / 458 | **FIELD-DIFF** | IN: width: flags u64->u32; field order |
| 14 | MM_DEVMEMINTUNMAPPMR (grp 6, id 20) | pvr_srv_bridge.h:468 / :472 | mm_bridge/common_*.h:469 / 475 | **IDENTICAL** | - |
| 15 | MM_DEVMEMINTRESERVERANGE (grp 6, id 21) | pvr_srv_bridge.h:358 / :364 | mm_bridge/common_*.h:485 / 493 | **FIELD-DIFF** | IN: field order |
| 16 | MM_DEVMEMINTUNRESERVERANGE (grp 6, id 22) | pvr_srv_bridge.h:373 / :377 | mm_bridge/common_*.h:504 / 510 | **IDENTICAL** | - |
| 17 | MM_DEVMEMINTMAPPAGES (grp 6, id 24) | pvr_srv_bridge.h:421 / :430 | mm_bridge/common_*.h:545 / 556 | **FIELD-DIFF** | IN: width: flags u64->u32; field order |
| 18 | MM_DEVMEMINTUNMAPPAGES (grp 6, id 25) | pvr_srv_bridge.h:438 / :444 | mm_bridge/common_*.h:566 / 574 | **FIELD-DIFF** | IN: field order |
| 19 | MM_HEAPCFGHEAPCOUNT (grp 6, id 30) | pvr_srv_bridge.h:296 / :300 | mm_bridge/common_*.h:654 / 660 | **FIELD-DIFF** | OUT: field order |
| 20 | MM_HEAPCFGHEAPDETAILS (grp 6, id 32) | pvr_srv_bridge.h:309 / :316 | mm_bridge/common_*.h:691 / 701 | **FIELD-DIFF** | IN: field order; OUT: field order |
| 21 | DMABUF_PHYSMEMIMPORTDMABUF (grp 11, id 0) | pvr_srv_bridge.h:480 / :487 | dmabuf_bridge/common_*.h:66 / 75 | **FIELD-DIFF** | IN: width: flags u64->u32; field order; OUT: field order |
| 22 | DMABUF_PHYSMEMEXPORTDMABUF (grp 11, id 2 -> **1**) | pvr_srv_bridge.h:498 / :502 | dmabuf_bridge/common_*.h:88 / 94 | **FIELD-DIFF** | OUT: field order |
| 23 | RGXTQ_CREATETRANSFERCONTEXT (grp 128, id 0) | pvr_srv_bridge.h:511 / :521 | rgxtq_bridge/common_*.h:69 / 80 | **FIELD-DIFF** | IN: 1.17-only: robustness_address; field order; OUT: field order |
| 24 | RGXTQ_DESTROYTRANSFERCONTEXT (grp 128, id 1) | pvr_srv_bridge.h:532 / :536 | rgxtq_bridge/common_*.h:93 / 99 | **IDENTICAL** | - |
| 25 | RGXTQ_SUBMITTRANSFER2 (grp 128, id 3) | pvr_srv_bridge.h:544 / :564 | rgxtq_bridge/common_*.h:126 / 149 | **FIELD-DIFF** | IN: 1.13-only: ui32ClientCacheOpSeqNum; field order; OUT: field order |
| 26 | RGXCMP_CREATECOMPUTECONTEXT (grp 129, id 0) | pvr_srv_bridge.h:574 / :588 | rgxcmp_bridge/common_*.h:72 / 87 | **FIELD-DIFF** | IN: field order |
| 27 | RGXCMP_DESTROYCOMPUTECONTEXT (grp 129, id 1) | pvr_srv_bridge.h:597 / :601 | rgxcmp_bridge/common_*.h:98 / 104 | **IDENTICAL** | - |
| 28 | RGXCMP_KICKCDM2 (grp 129, id 5 -> **6**) | pvr_srv_bridge.h:609 / :630 | rgxcmp_bridge/common_*.h:182 / 203 | **FIELD-DIFF** | IN: 1.17-only: sync_pmr_flags, sync_pmrs, sync_pmr_count; 1.13-only: ui32ClientCacheOpSeqNum; field order; OUT: field order |
| 29 | RGXTA3D_CREATEHWRTDATASET (grp 130, id 0) | pvr_srv_bridge.h:639 / :677 | rgxta3d_bridge/common_*.h:81 / 115 | **FIELD-DIFF** | IN: device-address arrays passed by value (suffix 0/1), no `hwrt_dataset` out pointer, `rgn_header_size` u32 -> u64; OUT: two handles in the struct (see 2.2) |
| 30 | RGXTA3D_DESTROYHWRTDATASET (grp 130, id 1) | pvr_srv_bridge.h:687 / :691 | rgxta3d_bridge/common_*.h:127 / 133 | **IDENTICAL** | - |
| 31 | RGXTA3D_CREATEFREELIST (grp 130, id 6) | pvr_srv_bridge.h:699 / :712 | rgxta3d_bridge/common_*.h:211 / 226 | **FIELD-DIFF** | IN: field order |
| 32 | RGXTA3D_DESTROYFREELIST (grp 130, id 7) | pvr_srv_bridge.h:721 / :725 | rgxta3d_bridge/common_*.h:237 / 243 | **IDENTICAL** | - |
| 33 | RGXTA3D_CREATERENDERCONTEXT (grp 130, id 8) | pvr_srv_bridge.h:733 / :754 | rgxta3d_bridge/common_*.h:253 / 270 | **FIELD-DIFF** | IN: 1.17-only: call_stack_depth; field order |
| 34 | RGXTA3D_DESTROYRENDERCONTEXT (grp 130, id 9) | pvr_srv_bridge.h:763 / :767 | rgxta3d_bridge/common_*.h:281 / 287 | **IDENTICAL** | - |
| 35 | RGXTA3D_KICKTA3D2 (grp 130, id 12 -> **13**) | pvr_srv_bridge.h:775 / :823 | rgxta3d_bridge/common_*.h:348 / 399 | **FIELD-DIFF** | IN: 1.13-only: ui32ClientCacheOpSeqNum; field order; OUT: field order |

### 2.2 Field-by-field tables for the 22 FIELD-DIFF functions

Columns: 1.17 field with its line in `UP:pvr_srv_bridge.h`, its type and packed offset (LP64/ILP32); the matching 1.13 field with its line in the KM header, type and offset; "position a -> b" = index in declaration order (0-based) in 1.17 vs 1.13. Offsets are within the IN or OUT struct. Types: `IMG_UINT32` = u32, `IMG_HANDLE` = pointer, `IMG_DEV_VIRTADDR`/`pvr_dev_addr_t` = u64, `PVRSRV_ERROR`/enums = u32.

#### SRVCORE_CONNECT

IN: UP `pvr_srv_bridge_connect_cmd` (UP:pvr_srv_bridge.h:190, packed, total 16 B LP64 / 16 B ILP32) vs KM `PVRSRV_BRIDGE_IN_CONNECT` (KM:generated/rogue/srvcore_bridge/common_srvcore_bridge.h:81, packed, total 16 B LP64 / 16 B ILP32)

| 1.17 field (line) | type | off LP64/ILP32 | 1.13 field (line) | type | off LP64/ILP32 | note |
|---|---|---|---|---|---|---|
| build_options (L191) | uint32_t | 0/0 | ui32ClientBuildOptions (L84) | IMG_UINT32 | 4/4 | position 0 -> 1 |
| DDK_build (L192) | uint32_t | 4/4 | ui32ClientDDKBuild (L86) | IMG_UINT32 | 12/12 | position 1 -> 3 |
| DDK_version (L193) | uint32_t | 8/8 | ui32ClientDDKVersion (L85) | IMG_UINT32 | 8/8 | same |
| flags (L194) | uint32_t | 12/12 | ui32Flags (L83) | IMG_UINT32 | 0/0 | position 3 -> 0 |

OUT: UP `pvr_srv_bridge_connect_ret` (UP:pvr_srv_bridge.h:197, packed, total 17 B LP64 / 17 B ILP32) vs KM `PVRSRV_BRIDGE_OUT_CONNECT` (KM:generated/rogue/srvcore_bridge/common_srvcore_bridge.h:90, packed, total 17 B LP64 / 17 B ILP32)

| 1.17 field (line) | type | off LP64/ILP32 | 1.13 field (line) | type | off LP64/ILP32 | note |
|---|---|---|---|---|---|---|
| bvnc (L198) | uint64_t | 0/0 | ui64PackedBvnc (L94) | IMG_UINT64 | 5/5 | position 0 -> 2 |
| error (L199) | pvr_srv_error | 8/8 | eError (L95) | PVRSRV_ERROR | 13/13 | position 1 -> 3 |
| capability_flags (L200) | uint32_t | 12/12 | ui32CapabilityFlags (L93) | IMG_UINT32 | 1/1 | position 2 -> 1 |
| kernel_arch (L201) | uint8_t | 16/16 | ui8KernelArch (L92) | IMG_UINT8 | 0/0 | position 3 -> 0 |

#### SRVCORE_GETMULTICOREINFO

IN: UP `pvr_srv_bridge_getmulticoreinfo_cmd` (UP:pvr_srv_bridge.h:216, packed, total 12 B LP64 / 8 B ILP32) vs KM `PVRSRV_BRIDGE_IN_GETMULTICOREINFO` (KM:generated/rogue/srvcore_bridge/common_srvcore_bridge.h:284, packed, total 12 B LP64 / 8 B ILP32)

| 1.17 field (line) | type | off LP64/ILP32 | 1.13 field (line) | type | off LP64/ILP32 | note |
|---|---|---|---|---|---|---|
| caps (L217) | uint64_t * | 0/0 | pui64Caps (L288) | IMG_UINT64 * | 4/4 | position 0 -> 1 |
| caps_size (L218) | uint32_t | 8/4 | ui32CapsSize (L286) | IMG_UINT32 | 0/0 | position 1 -> 0 |

OUT: UP `pvr_srv_bridge_getmulticoreinfo_ret` (UP:pvr_srv_bridge.h:221, packed, total 16 B LP64 / 12 B ILP32) vs KM `PVRSRV_BRIDGE_OUT_GETMULTICOREINFO` (KM:generated/rogue/srvcore_bridge/common_srvcore_bridge.h:292, packed, total 16 B LP64 / 12 B ILP32)

| 1.17 field (line) | type | off LP64/ILP32 | 1.13 field (line) | type | off LP64/ILP32 | note |
|---|---|---|---|---|---|---|
| caps (L222) | uint64_t * | 0/0 | pui64Caps (L295) | IMG_UINT64 * | 4/4 | position 0 -> 1 |
| error (L223) | pvr_srv_error | 8/4 | eError (L296) | PVRSRV_ERROR | 12/8 | position 1 -> 2 |
| num_cores (L224) | uint32_t | 12/8 | ui32NumCores (L294) | IMG_UINT32 | 0/0 | position 2 -> 0 |

#### SYNC_ALLOCSYNCPRIMITIVEBLOCK
- IN: IDENTICAL ((none) vs PVRSRV_BRIDGE_IN_ALLOCSYNCPRIMITIVEBLOCK)

OUT: UP `pvr_srv_bridge_alloc_sync_primitive_block_ret` (UP:pvr_srv_bridge.h:231, packed, total 28 B LP64 / 20 B ILP32) vs KM `PVRSRV_BRIDGE_OUT_ALLOCSYNCPRIMITIVEBLOCK` (KM:generated/rogue/sync_bridge/common_sync_bridge.h:83, packed, total 28 B LP64 / 20 B ILP32)

| 1.17 field (line) | type | off LP64/ILP32 | 1.13 field (line) | type | off LP64/ILP32 | note |
|---|---|---|---|---|---|---|
| handle (L232) | void * | 0/0 | hSyncHandle (L85) | IMG_HANDLE | 0/0 | same |
| pmr (L233) | void * | 8/4 | hhSyncPMR (L88) | IMG_HANDLE | 16/12 | position 1 -> 3 |
| error (L234) | pvr_srv_error | 16/8 | eError (L89) | PVRSRV_ERROR | 24/16 | position 2 -> 4 |
| size (L235) | uint32_t | 20/12 | ui32SyncPrimBlockSize (L87) | IMG_UINT32 | 12/8 | position 3 -> 2 |
| addr (L236) | uint32_t | 24/16 | ui32SyncPrimVAddr (L86) | IMG_UINT32 | 8/4 | position 4 -> 1 |

#### MM_PHYSMEMNEWRAMBACKEDLOCKEDPMR

IN: UP `pvr_srv_physmem_new_ram_backed_locked_pmr_cmd` (UP:pvr_srv_bridge.h:385, packed, total 64 B LP64 / 56 B ILP32) vs KM `PVRSRV_BRIDGE_IN_PHYSMEMNEWRAMBACKEDLOCKEDPMR` (KM:generated/rogue/mm_bridge/common_mm_bridge.h:285, packed, total 60 B LP64 / 52 B ILP32)

| 1.17 field (line) | type | off LP64/ILP32 | 1.13 field (line) | type | off LP64/ILP32 | note |
|---|---|---|---|---|---|---|
| block_size (L386) | uint64_t | 0/0 | uiChunkSize (L288) | IMG_DEVMEM_SIZE_T | 8/8 | position 0 -> 1 |
| size (L387) | uint64_t | 8/8 | uiSize (L287) | IMG_DEVMEM_SIZE_T | 0/0 | position 1 -> 0 |
| mapping_table (L388) | uint32_t * | 16/16 | pui32MappingTable (L291) | IMG_UINT32 * | 24/24 | position 2 -> 4 |
| annotation (L389) | const char * | 24/20 | puiAnnotation (L295) | const IMG_CHAR * | 44/40 | position 3 -> 8 |
| annotation_size (L390) | uint32_t | 32/24 | ui32AnnotationLength (L294) | IMG_UINT32 | 40/36 | position 4 -> 7 |
| log2_page_size (L391) | uint32_t | 36/28 | ui32Log2PageSize (L292) | IMG_UINT32 | 32/28 | offset shifted |
| phy_blocks (L392) | uint32_t | 40/32 | ui32NumPhysChunks (L289) | IMG_UINT32 | 16/16 | position 6 -> 2 |
| virt_blocks (L393) | uint32_t | 44/36 | ui32NumVirtChunks (L290) | IMG_UINT32 | 20/20 | position 7 -> 3 |
| pdump_flags (L394) | uint32_t | 48/40 | ui32PDumpFlags (L297) | IMG_UINT32 | 56/48 | position 8 -> 10 |
| pid (L395) | uint32_t | 52/44 | ui32PID (L296) | IMG_PID | 52/44 | same |
| flags (L396) | uint64_t | 56/48 | uiFlags (L293) | PVRSRV_MEMALLOCFLAGS_T | 36/32 | position 10 -> 6; width 8 vs 4 |

OUT: UP `pvr_srv_physmem_new_ram_backed_locked_pmr_ret` (UP:pvr_srv_bridge.h:399, packed, total 20 B LP64 / 16 B ILP32) vs KM `PVRSRV_BRIDGE_OUT_PHYSMEMNEWRAMBACKEDLOCKEDPMR` (KM:generated/rogue/mm_bridge/common_mm_bridge.h:301, packed, total 12 B LP64 / 8 B ILP32)

| 1.17 field (line) | type | off LP64/ILP32 | 1.13 field (line) | type | off LP64/ILP32 | note |
|---|---|---|---|---|---|---|
| pmr (L400) | void * | 0/0 | hPMRPtr (L303) | IMG_HANDLE | 0/0 | same |
| error (L401) | pvr_srv_error | 8/4 | eError (L304) | PVRSRV_ERROR | 8/4 | same |
| out_flags (L402) | uint64_t | 12/8 | - | - | - | **missing in 1.13** |

#### MM_DEVMEMINTCTXCREATE
- IN: IDENTICAL (pvr_srv_devmem_int_ctx_create_cmd vs PVRSRV_BRIDGE_IN_DEVMEMINTCTXCREATE)

OUT: UP `pvr_srv_devmem_int_ctx_create_ret` (UP:pvr_srv_bridge.h:273, packed, total 24 B LP64 / 16 B ILP32) vs KM `PVRSRV_BRIDGE_OUT_DEVMEMINTCTXCREATE` (KM:generated/rogue/mm_bridge/common_mm_bridge.h:384, packed, total 24 B LP64 / 16 B ILP32)

| 1.17 field (line) | type | off LP64/ILP32 | 1.13 field (line) | type | off LP64/ILP32 | note |
|---|---|---|---|---|---|---|
| server_memctx (L274) | void * | 0/0 | hDevMemServerContext (L386) | IMG_HANDLE | 0/0 | same |
| server_memctx_data (L275) | void * | 8/4 | hPrivData (L387) | IMG_HANDLE | 8/4 | same |
| error (L276) | pvr_srv_error | 16/8 | eError (L389) | PVRSRV_ERROR | 20/12 | position 2 -> 3 |
| cpu_cache_line_size (L277) | uint32_t | 20/12 | ui32CPUCacheLineSize (L388) | IMG_UINT32 | 16/8 | position 3 -> 2 |

#### MM_DEVMEMINTHEAPCREATE

IN: UP `pvr_srv_devmem_int_heap_create_cmd` (UP:pvr_srv_bridge.h:330, packed, total 28 B LP64 / 24 B ILP32) vs KM `PVRSRV_BRIDGE_IN_DEVMEMINTHEAPCREATE` (KM:generated/rogue/mm_bridge/common_mm_bridge.h:413, packed, total 28 B LP64 / 24 B ILP32)

| 1.17 field (line) | type | off LP64/ILP32 | 1.13 field (line) | type | off LP64/ILP32 | note |
|---|---|---|---|---|---|---|
| base_addr (L331) | pvr_dev_addr_t | 0/0 | sHeapBaseAddr (L416) | IMG_DEV_VIRTADDR | 8/4 | position 0 -> 1 |
| size (L332) | uint64_t | 8/8 | uiHeapLength (L417) | IMG_DEVMEM_SIZE_T | 16/12 | position 1 -> 2 |
| server_memctx (L333) | void * | 16/16 | hDevmemCtx (L415) | IMG_HANDLE | 0/0 | position 2 -> 0 |
| log2_page_size (L334) | uint32_t | 24/20 | ui32Log2DataPageSize (L418) | IMG_UINT32 | 24/20 | same |
- OUT: IDENTICAL (pvr_srv_devmem_int_heap_create_ret vs PVRSRV_BRIDGE_OUT_DEVMEMINTHEAPCREATE)

#### MM_DEVMEMINTMAPPMR

IN: UP `pvr_srv_devmem_int_map_pmr_cmd` (UP:pvr_srv_bridge.h:452, packed, total 32 B LP64 / 20 B ILP32) vs KM `PVRSRV_BRIDGE_IN_DEVMEMINTMAPPMR` (KM:generated/rogue/mm_bridge/common_mm_bridge.h:449, packed, total 28 B LP64 / 16 B ILP32)

| 1.17 field (line) | type | off LP64/ILP32 | 1.13 field (line) | type | off LP64/ILP32 | note |
|---|---|---|---|---|---|---|
| server_heap (L453) | void * | 0/0 | hDevmemServerHeap (L451) | IMG_HANDLE | 0/0 | same |
| pmr (L454) | void * | 8/4 | hPMR (L453) | IMG_HANDLE | 16/8 | position 1 -> 2 |
| reservation (L455) | void * | 16/8 | hReservation (L452) | IMG_HANDLE | 8/4 | position 2 -> 1 |
| flags (L456) | uint64_t | 24/12 | uiMapFlags (L454) | PVRSRV_MEMALLOCFLAGS_T | 24/12 | width 8 vs 4 |
- OUT: IDENTICAL (pvr_srv_devmem_int_map_pmr_ret vs PVRSRV_BRIDGE_OUT_DEVMEMINTMAPPMR)

#### MM_DEVMEMINTRESERVERANGE

IN: UP `pvr_srv_devmem_int_reserve_range_cmd` (UP:pvr_srv_bridge.h:358, packed, total 24 B LP64 / 20 B ILP32) vs KM `PVRSRV_BRIDGE_IN_DEVMEMINTRESERVERANGE` (KM:generated/rogue/mm_bridge/common_mm_bridge.h:485, packed, total 24 B LP64 / 20 B ILP32)

| 1.17 field (line) | type | off LP64/ILP32 | 1.13 field (line) | type | off LP64/ILP32 | note |
|---|---|---|---|---|---|---|
| addr (L359) | pvr_dev_addr_t | 0/0 | sAddress (L488) | IMG_DEV_VIRTADDR | 8/4 | position 0 -> 1 |
| size (L360) | uint64_t | 8/8 | uiLength (L489) | IMG_DEVMEM_SIZE_T | 16/12 | position 1 -> 2 |
| server_heap (L361) | void * | 16/16 | hDevmemServerHeap (L487) | IMG_HANDLE | 0/0 | position 2 -> 0 |
- OUT: IDENTICAL (pvr_srv_devmem_int_reserve_range_ret vs PVRSRV_BRIDGE_OUT_DEVMEMINTRESERVERANGE)

#### MM_DEVMEMINTMAPPAGES

IN: UP `pvr_srv_devmem_int_map_pages_cmd` (UP:pvr_srv_bridge.h:421, packed, total 40 B LP64 / 32 B ILP32) vs KM `PVRSRV_BRIDGE_IN_DEVMEMINTMAPPAGES` (KM:generated/rogue/mm_bridge/common_mm_bridge.h:545, packed, total 36 B LP64 / 28 B ILP32)

| 1.17 field (line) | type | off LP64/ILP32 | 1.13 field (line) | type | off LP64/ILP32 | note |
|---|---|---|---|---|---|---|
| addr (L422) | pvr_dev_addr_t | 0/0 | sDevVAddr (L552) | IMG_DEV_VIRTADDR | 28/20 | position 0 -> 5 |
| pmr (L423) | void * | 8/8 | hPMR (L548) | IMG_HANDLE | 8/4 | same |
| reservation (L424) | void * | 16/12 | hReservation (L547) | IMG_HANDLE | 0/0 | position 2 -> 0 |
| page_count (L425) | uint32_t | 24/16 | ui32PageCount (L549) | IMG_UINT32 | 16/8 | position 3 -> 2 |
| page_offset (L426) | uint32_t | 28/20 | ui32PhysicalPgOffset (L550) | IMG_UINT32 | 20/12 | position 4 -> 3 |
| flags (L427) | uint64_t | 32/24 | uiFlags (L551) | PVRSRV_MEMALLOCFLAGS_T | 24/16 | position 5 -> 4; width 8 vs 4 |
- OUT: IDENTICAL (pvr_srv_devmem_int_map_pages_ret vs PVRSRV_BRIDGE_OUT_DEVMEMINTMAPPAGES)

#### MM_DEVMEMINTUNMAPPAGES

IN: UP `pvr_srv_devmem_int_unmap_pages_cmd` (UP:pvr_srv_bridge.h:438, packed, total 20 B LP64 / 16 B ILP32) vs KM `PVRSRV_BRIDGE_IN_DEVMEMINTUNMAPPAGES` (KM:generated/rogue/mm_bridge/common_mm_bridge.h:566, packed, total 20 B LP64 / 16 B ILP32)

| 1.17 field (line) | type | off LP64/ILP32 | 1.13 field (line) | type | off LP64/ILP32 | note |
|---|---|---|---|---|---|---|
| dev_addr (L439) | pvr_dev_addr_t | 0/0 | sDevVAddr (L569) | IMG_DEV_VIRTADDR | 8/4 | position 0 -> 1 |
| reservation (L440) | void * | 8/8 | hReservation (L568) | IMG_HANDLE | 0/0 | position 1 -> 0 |
| page_count (L441) | uint32_t | 16/12 | ui32PageCount (L570) | IMG_UINT32 | 16/12 | same |
- OUT: IDENTICAL (pvr_srv_devmem_int_unmap_pages_ret vs PVRSRV_BRIDGE_OUT_DEVMEMINTUNMAPPAGES)

#### MM_HEAPCFGHEAPCOUNT
- IN: IDENTICAL (pvr_srv_heap_count_cmd vs PVRSRV_BRIDGE_IN_HEAPCFGHEAPCOUNT)

OUT: UP `pvr_srv_heap_count_ret` (UP:pvr_srv_bridge.h:300, packed, total 8 B LP64 / 8 B ILP32) vs KM `PVRSRV_BRIDGE_OUT_HEAPCFGHEAPCOUNT` (KM:generated/rogue/mm_bridge/common_mm_bridge.h:660, packed, total 8 B LP64 / 8 B ILP32)

| 1.17 field (line) | type | off LP64/ILP32 | 1.13 field (line) | type | off LP64/ILP32 | note |
|---|---|---|---|---|---|---|
| error (L301) | pvr_srv_error | 0/0 | eError (L663) | PVRSRV_ERROR | 4/4 | position 0 -> 1 |
| heap_count (L302) | uint32_t | 4/4 | ui32NumHeaps (L662) | IMG_UINT32 | 0/0 | position 1 -> 0 |

#### MM_HEAPCFGHEAPDETAILS

IN: UP `pvr_srv_heap_cfg_details_cmd` (UP:pvr_srv_bridge.h:309, packed, total 20 B LP64 / 16 B ILP32) vs KM `PVRSRV_BRIDGE_IN_HEAPCFGHEAPDETAILS` (KM:generated/rogue/mm_bridge/common_mm_bridge.h:691, packed, total 20 B LP64 / 16 B ILP32)

| 1.17 field (line) | type | off LP64/ILP32 | 1.13 field (line) | type | off LP64/ILP32 | note |
|---|---|---|---|---|---|---|
| buffer (L310) | char * | 0/0 | puiHeapNameOut (L697) | IMG_CHAR * | 12/12 | position 0 -> 3 |
| heap_config_index (L311) | uint32_t | 8/4 | ui32HeapConfigIndex (L693) | IMG_UINT32 | 0/0 | position 1 -> 0 |
| heap_index (L312) | uint32_t | 12/8 | ui32HeapIndex (L694) | IMG_UINT32 | 4/4 | position 2 -> 1 |
| buffer_size (L313) | uint32_t | 16/12 | ui32HeapNameBufSz (L695) | IMG_UINT32 | 8/8 | position 3 -> 2 |

OUT: UP `pvr_srv_heap_cfg_details_ret` (UP:pvr_srv_bridge.h:316, packed, total 44 B LP64 / 40 B ILP32) vs KM `PVRSRV_BRIDGE_OUT_HEAPCFGHEAPDETAILS` (KM:generated/rogue/mm_bridge/common_mm_bridge.h:701, packed, total 44 B LP64 / 40 B ILP32)

| 1.17 field (line) | type | off LP64/ILP32 | 1.13 field (line) | type | off LP64/ILP32 | note |
|---|---|---|---|---|---|---|
| base_addr (L317) | pvr_dev_addr_t | 0/0 | sDevVAddrBase (L704) | IMG_DEV_VIRTADDR | 8/4 | position 0 -> 1 |
| size (L318) | uint64_t | 8/8 | uiHeapLength (L705) | IMG_DEVMEM_SIZE_T | 16/12 | position 1 -> 2 |
| reserved_size (L319) | uint64_t | 16/16 | uiReservedRegionLength (L706) | IMG_DEVMEM_SIZE_T | 24/20 | position 2 -> 3 |
| buffer (L320) | char * | 24/24 | puiHeapNameOut (L703) | IMG_CHAR * | 0/0 | position 3 -> 0 |
| error (L321) | pvr_srv_error | 32/28 | eError (L709) | PVRSRV_ERROR | 40/36 | position 4 -> 6 |
| log2_page_size (L322) | uint32_t | 36/32 | ui32Log2DataPageSizeOut (L707) | IMG_UINT32 | 32/28 | position 5 -> 4 |
| log2_alignment (L323) | uint32_t | 40/36 | ui32Log2ImportAlignmentOut (L708) | IMG_UINT32 | 36/32 | position 6 -> 5 |

#### DMABUF_PHYSMEMIMPORTDMABUF

IN: UP `pvr_srv_phys_mem_import_dmabuf_cmd` (UP:pvr_srv_bridge.h:480, packed, total 24 B LP64 / 20 B ILP32) vs KM `PVRSRV_BRIDGE_IN_PHYSMEMIMPORTDMABUF` (KM:generated/rogue/dmabuf_bridge/common_dmabuf_bridge.h:66, packed, total 20 B LP64 / 16 B ILP32)

| 1.17 field (line) | type | off LP64/ILP32 | 1.13 field (line) | type | off LP64/ILP32 | note |
|---|---|---|---|---|---|---|
| name (L481) | const char * | 0/0 | puiName (L71) | const IMG_CHAR * | 12/12 | position 0 -> 3 |
| buffer_fd (L482) | int | 8/4 | ifd (L68) | IMG_INT | 0/0 | position 1 -> 0 |
| name_size (L483) | uint32_t | 12/8 | ui32NameSize (L70) | IMG_UINT32 | 8/8 | offset shifted |
| flags (L484) | uint64_t | 16/12 | uiFlags (L69) | PVRSRV_MEMALLOCFLAGS_T | 4/4 | position 3 -> 1; width 8 vs 4 |

OUT: UP `pvr_srv_phys_mem_import_dmabuf_ret` (UP:pvr_srv_bridge.h:487, packed, total 28 B LP64 / 24 B ILP32) vs KM `PVRSRV_BRIDGE_OUT_PHYSMEMIMPORTDMABUF` (KM:generated/rogue/dmabuf_bridge/common_dmabuf_bridge.h:75, packed, total 28 B LP64 / 24 B ILP32)

| 1.17 field (line) | type | off LP64/ILP32 | 1.13 field (line) | type | off LP64/ILP32 | note |
|---|---|---|---|---|---|---|
| align (L488) | uint64_t | 0/0 | sAlign (L79) | IMG_DEVMEM_ALIGN_T | 16/12 | position 0 -> 2 |
| size (L489) | uint64_t | 8/8 | uiSize (L78) | IMG_DEVMEM_SIZE_T | 8/4 | same |
| pmr (L490) | void * | 16/16 | hPMRPtr (L77) | IMG_HANDLE | 0/0 | position 2 -> 0 |
| error (L491) | pvr_srv_error | 24/20 | eError (L80) | PVRSRV_ERROR | 24/20 | same |

#### DMABUF_PHYSMEMEXPORTDMABUF
- IN: IDENTICAL (pvr_srv_phys_mem_export_dmabuf_cmd vs PVRSRV_BRIDGE_IN_PHYSMEMEXPORTDMABUF)

OUT: UP `pvr_srv_phys_mem_export_dmabuf_ret` (UP:pvr_srv_bridge.h:502, packed, total 8 B LP64 / 8 B ILP32) vs KM `PVRSRV_BRIDGE_OUT_PHYSMEMEXPORTDMABUF` (KM:generated/rogue/dmabuf_bridge/common_dmabuf_bridge.h:94, packed, total 8 B LP64 / 8 B ILP32)

| 1.17 field (line) | type | off LP64/ILP32 | 1.13 field (line) | type | off LP64/ILP32 | note |
|---|---|---|---|---|---|---|
| error (L503) | pvr_srv_error | 0/0 | eError (L97) | PVRSRV_ERROR | 4/4 | position 0 -> 1 |
| fd (L504) | int | 4/4 | iFd (L96) | IMG_INT | 0/0 | position 1 -> 0 |

#### RGXTQ_CREATETRANSFERCONTEXT

IN: UP `pvr_srv_rgx_create_transfer_context_cmd` (UP:pvr_srv_bridge.h:511, packed, total 40 B LP64 / 32 B ILP32) vs KM `PVRSRV_BRIDGE_IN_RGXCREATETRANSFERCONTEXT` (KM:generated/rogue/rgxtq_bridge/common_rgxtq_bridge.h:69, packed, total 32 B LP64 / 24 B ILP32)

| 1.17 field (line) | type | off LP64/ILP32 | 1.13 field (line) | type | off LP64/ILP32 | note |
|---|---|---|---|---|---|---|
| priv_data (L513) | void * | 8/8 | hPrivData (L74) | IMG_HANDLE | 16/12 | position 1 -> 3 |
| reset_framework_cmd (L514) | uint8_t * | 16/12 | psFrameworkCmd (L73) | IMG_BYTE * | 8/8 | offset shifted |
| context_flags (L515) | uint32_t | 24/16 | ui32ContextFlags (L76) | IMG_UINT32 | 28/20 | position 3 -> 5 |
| reset_framework_cmd_size (L516) | uint32_t | 28/20 | ui32FrameworkCmdize (L72) | IMG_UINT32 | 4/4 | position 4 -> 1 |
| packed_ccb_size_u8888 (L517) | uint32_t | 32/24 | ui32PackedCCBSizeU8888 (L75) | IMG_UINT32 | 24/16 | position 5 -> 4 |
| priority (L518) | uint32_t | 36/28 | ui32Priority (L71) | IMG_UINT32 | 0/0 | position 6 -> 0 |
| robustness_address (L512) | uint64_t | 0/0 | - | - | - | **missing in 1.13** |

OUT: UP `pvr_srv_rgx_create_transfer_context_ret` (UP:pvr_srv_bridge.h:521, packed, total 28 B LP64 / 16 B ILP32) vs KM `PVRSRV_BRIDGE_OUT_RGXCREATETRANSFERCONTEXT` (KM:generated/rogue/rgxtq_bridge/common_rgxtq_bridge.h:80, packed, total 28 B LP64 / 16 B ILP32)

| 1.17 field (line) | type | off LP64/ILP32 | 1.13 field (line) | type | off LP64/ILP32 | note |
|---|---|---|---|---|---|---|
| cli_pmr_mem (L522) | void * | 0/0 | hCLIPMRMem (L83) | IMG_HANDLE | 8/4 | position 0 -> 1 |
| transfer_context (L523) | void * | 8/4 | hTransferContext (L82) | IMG_HANDLE | 0/0 | position 1 -> 0 |
| usc_pmr_mem (L524) | void * | 16/8 | hUSCPMRMem (L84) | IMG_HANDLE | 16/8 | same |
| error (L525) | pvr_srv_error | 24/12 | eError (L85) | PVRSRV_ERROR | 24/12 | same |

#### RGXTQ_SUBMITTRANSFER2

IN: UP `pvr_srv_rgx_submit_transfer2_cmd` (UP:pvr_srv_bridge.h:544, packed, total 112 B LP64 / 68 B ILP32) vs KM `PVRSRV_BRIDGE_IN_RGXSUBMITTRANSFER2` (KM:generated/rogue/rgxtq_bridge/common_rgxtq_bridge.h:126, packed, total 116 B LP64 / 72 B ILP32)

| 1.17 field (line) | type | off LP64/ILP32 | 1.13 field (line) | type | off LP64/ILP32 | note |
|---|---|---|---|---|---|---|
| transfer_context (L545) | void * | 0/0 | hTransferContext (L128) | IMG_HANDLE | 0/0 | same |
| client_update_count (L546) | uint32_t * | 8/4 | pui32ClientUpdateCount (L131) | IMG_UINT32 * | 16/12 | position 1 -> 3 |
| cmd_size (L547) | uint32_t * | 16/8 | pui32CommandSize (L139) | IMG_UINT32 * | 68/44 | position 2 -> 11 |
| sync_pmr_flags (L548) | uint32_t * | 24/12 | pui32SyncPMRFlags (L144) | IMG_UINT32 * | 100/64 | position 3 -> 16 |
| tq_prepare_flags (L549) | uint32_t * | 32/16 | pui32TQPrepareFlags (L141) | IMG_UINT32 * | 84/52 | position 4 -> 13 |
| update_sync_offset (L550) | uint32_t ** | 40/20 | pui32UpdateSyncOffset (L133) | IMG_UINT32 ** | 32/20 | offset shifted |
| update_value (L551) | uint32_t ** | 48/24 | pui32UpdateValue (L134) | IMG_UINT32 ** | 40/24 | offset shifted |
| fw_command (L552) | uint8_t ** | 56/28 | pui8FWCommand (L140) | IMG_UINT8 ** | 76/48 | position 7 -> 12 |
| update_fence_name (L553) | char * | 64/32 | puiUpdateFenceName (L138) | IMG_CHAR * | 60/40 | position 8 -> 10 |
| sync_pmrs (L554) | void ** | 72/36 | phSyncPMRs (L145) | IMG_HANDLE * | 108/68 | position 9 -> 17 |
| update_ufo_sync_prim_block (L555) | void *** | 80/40 | phUpdateUFOSyncPrimBlock (L132) | IMG_HANDLE ** | 24/16 | position 10 -> 4 |
| update_timeline_2d (L556) | int32_t | 88/44 | h2DUpdateTimeline (L136) | PVRSRV_TIMELINE | 52/32 | position 11 -> 8 |
| update_timeline_3d (L557) | int32_t | 92/48 | h3DUpdateTimeline (L137) | PVRSRV_TIMELINE | 56/36 | position 12 -> 9 |
| check_fence (L558) | int32_t | 96/52 | hCheckFenceFD (L135) | PVRSRV_FENCE | 48/28 | position 13 -> 7 |
| ext_job_ref (L559) | uint32_t | 100/56 | ui32ExtJobRef (L142) | IMG_UINT32 | 92/56 | offset shifted |
| prepare_count (L560) | uint32_t | 104/60 | ui32PrepareCount (L130) | IMG_UINT32 | 12/8 | position 15 -> 2 |
| sync_pmr_count (L561) | uint32_t | 108/64 | ui32SyncPMRCount (L143) | IMG_UINT32 | 96/60 | position 16 -> 15 |
| - | - | - | ui32ClientCacheOpSeqNum (L129) | IMG_UINT32 | 8/4 | **added in 1.13** |

OUT: UP `pvr_srv_rgx_submit_transfer2_ret` (UP:pvr_srv_bridge.h:564, packed, total 12 B LP64 / 12 B ILP32) vs KM `PVRSRV_BRIDGE_OUT_RGXSUBMITTRANSFER2` (KM:generated/rogue/rgxtq_bridge/common_rgxtq_bridge.h:149, packed, total 12 B LP64 / 12 B ILP32)

| 1.17 field (line) | type | off LP64/ILP32 | 1.13 field (line) | type | off LP64/ILP32 | note |
|---|---|---|---|---|---|---|
| error (L565) | pvr_srv_error | 0/0 | eError (L153) | PVRSRV_ERROR | 8/8 | position 0 -> 2 |
| update_fence_2d (L566) | int32_t | 4/4 | h2DUpdateFence (L151) | PVRSRV_FENCE | 0/0 | position 1 -> 0 |
| update_fence_3d (L567) | int32_t | 8/8 | h3DUpdateFence (L152) | PVRSRV_FENCE | 4/4 | position 2 -> 1 |

#### RGXCMP_CREATECOMPUTECONTEXT

IN: UP `pvr_srv_rgx_create_compute_context_cmd` (UP:pvr_srv_bridge.h:574, packed, total 56 B LP64 / 44 B ILP32) vs KM `PVRSRV_BRIDGE_IN_RGXCREATECOMPUTECONTEXT` (KM:generated/rogue/rgxcmp_bridge/common_rgxcmp_bridge.h:72, packed, total 56 B LP64 / 44 B ILP32)

| 1.17 field (line) | type | off LP64/ILP32 | 1.13 field (line) | type | off LP64/ILP32 | note |
|---|---|---|---|---|---|---|
| robustness_address (L575) | uint64_t | 0/0 | ui64RobustnessAddress (L82) | IMG_UINT64 | 44/32 | position 0 -> 8 |
| priv_data (L576) | void * | 8/8 | hPrivData (L77) | IMG_HANDLE | 16/12 | position 1 -> 3 |
| reset_framework_cmd (L577) | uint8_t * | 16/12 | psFrameworkCmd (L76) | IMG_BYTE * | 8/8 | offset shifted |
| static_compute_context_state (L578) | uint8_t * | 24/16 | psStaticComputeContextState (L79) | IMG_BYTE * | 28/20 | position 3 -> 5 |
| context_flags (L579) | uint32_t | 32/20 | ui32ContextFlags (L81) | IMG_UINT32 | 40/28 | position 4 -> 7 |
| reset_framework_cmd_size (L580) | uint32_t | 36/24 | ui32FrameworkCmdize (L75) | IMG_UINT32 | 4/4 | position 5 -> 1 |
| max_deadline_ms (L581) | uint32_t | 40/28 | ui32MaxDeadlineMS (L83) | IMG_UINT32 | 52/40 | position 6 -> 9 |
| packed_ccb_size (L582) | uint32_t | 44/32 | ui32PackedCCBSizeU88 (L80) | IMG_UINT32 | 36/24 | position 7 -> 6 |
| priority (L584) | uint32_t | 48/36 | ui32Priority (L74) | IMG_UINT32 | 0/0 | position 8 -> 0 |
| static_compute_context_state_size (L585) | uint32_t | 52/40 | ui32StaticComputeContextStateSize (L78) | IMG_UINT32 | 24/16 | position 9 -> 4 |
- OUT: IDENTICAL (pvr_srv_rgx_create_compute_context_ret vs PVRSRV_BRIDGE_OUT_RGXCREATECOMPUTECONTEXT)

#### RGXCMP_KICKCDM2

IN: UP `pvr_srv_rgx_kick_cdm2_cmd` (UP:pvr_srv_bridge.h:609, packed, total 108 B LP64 / 76 B ILP32) vs KM `PVRSRV_BRIDGE_IN_RGXKICKCDM2` (KM:generated/rogue/rgxcmp_bridge/common_rgxcmp_bridge.h:182, packed, total 92 B LP64 / 68 B ILP32)

| 1.17 field (line) | type | off LP64/ILP32 | 1.13 field (line) | type | off LP64/ILP32 | note |
|---|---|---|---|---|---|---|
| max_deadline_us (L610) | uint64_t | 0/0 | ui64DeadlineInus (L199) | IMG_UINT64 | 84/60 | position 0 -> 15 |
| compute_context (L611) | void * | 8/8 | hComputeContext (L184) | IMG_HANDLE | 0/0 | position 1 -> 0 |
| client_update_offset (L612) | uint32_t * | 16/12 | pui32ClientUpdateOffset (L188) | IMG_UINT32 * | 24/16 | position 2 -> 4 |
| client_update_value (L613) | uint32_t * | 24/16 | pui32ClientUpdateValue (L189) | IMG_UINT32 * | 32/20 | position 3 -> 5 |
| cdm_cmd (L615) | uint8_t * | 40/24 | psDMCmd (L194) | IMG_BYTE * | 60/40 | position 5 -> 10 |
| update_fence_name (L616) | char * | 48/28 | puiUpdateFenceName (L192) | IMG_CHAR * | 48/32 | position 6 -> 8 |
| client_update_ufo_sync_prim_block (L617) | void ** | 56/32 | phClientUpdateUFOSyncPrimBlock (L187) | IMG_HANDLE * | 16/12 | position 7 -> 3 |
| check_fence (L619) | int32_t | 72/40 | hCheckFenceFd (L190) | PVRSRV_FENCE | 40/24 | position 9 -> 6 |
| update_timeline (L620) | int32_t | 76/44 | hUpdateTimeline (L191) | PVRSRV_TIMELINE | 44/28 | position 10 -> 7 |
| client_update_count (L621) | uint32_t | 80/48 | ui32ClientUpdateCount (L186) | IMG_UINT32 | 12/8 | position 11 -> 2 |
| cmd_size (L622) | uint32_t | 84/52 | ui32CmdSize (L193) | IMG_UINT32 | 56/36 | position 12 -> 9 |
| ext_job_ref (L623) | uint32_t | 88/56 | ui32ExtJobRef (L196) | IMG_UINT32 | 72/48 | position 13 -> 12 |
| num_work_groups (L624) | uint32_t | 92/60 | ui32NumOfWorkgroups (L197) | IMG_UINT32 | 76/52 | position 14 -> 13 |
| num_work_items (L625) | uint32_t | 96/64 | ui32NumOfWorkitems (L198) | IMG_UINT32 | 80/56 | position 15 -> 14 |
| pdump_flags (L626) | uint32_t | 100/68 | ui32PDumpFlags (L195) | IMG_UINT32 | 68/44 | position 16 -> 11 |
| sync_pmr_flags (L614) | uint32_t * | 32/20 | - | - | - | **missing in 1.13** |
| sync_pmrs (L618) | void ** | 64/36 | - | - | - | **missing in 1.13** |
| sync_pmr_count (L627) | uint32_t | 104/72 | - | - | - | **missing in 1.13** |
| - | - | - | ui32ClientCacheOpSeqNum (L185) | IMG_UINT32 | 8/4 | **added in 1.13** |

OUT: UP `pvr_srv_rgx_kick_cdm2_ret` (UP:pvr_srv_bridge.h:630, packed, total 8 B LP64 / 8 B ILP32) vs KM `PVRSRV_BRIDGE_OUT_RGXKICKCDM2` (KM:generated/rogue/rgxcmp_bridge/common_rgxcmp_bridge.h:203, packed, total 8 B LP64 / 8 B ILP32)

| 1.17 field (line) | type | off LP64/ILP32 | 1.13 field (line) | type | off LP64/ILP32 | note |
|---|---|---|---|---|---|---|
| error (L631) | pvr_srv_error | 0/0 | eError (L206) | PVRSRV_ERROR | 4/4 | position 0 -> 1 |
| update_fence (L632) | int32_t | 4/4 | hUpdateFence (L205) | PVRSRV_FENCE | 0/0 | position 1 -> 0 |

#### RGXTA3D_CREATEFREELIST

IN: UP `pvr_srv_rgx_create_free_list_cmd` (UP:pvr_srv_bridge.h:699, packed, total 60 B LP64 / 48 B ILP32) vs KM `PVRSRV_BRIDGE_IN_RGXCREATEFREELIST` (KM:generated/rogue/rgxta3d_bridge/common_rgxta3d_bridge.h:211, packed, total 60 B LP64 / 48 B ILP32)

| 1.17 field (line) | type | off LP64/ILP32 | 1.13 field (line) | type | off LP64/ILP32 | note |
|---|---|---|---|---|---|---|
| free_list_dev_addr (L700) | pvr_dev_addr_t | 0/0 | spsFreeListDevVAddr (L220) | IMG_DEV_VIRTADDR | 36/28 | position 0 -> 7 |
| pmr_offset (L701) | uint64_t | 8/8 | uiPMROffset (L222) | IMG_DEVMEM_OFFSET_T | 52/40 | position 1 -> 9 |
| mem_ctx_priv_data (L702) | void * | 16/16 | hMemCtxPrivData (L213) | IMG_HANDLE | 0/0 | position 2 -> 0 |
| free_list_pmr (L703) | void * | 24/20 | hsFreeListPMR (L221) | IMG_HANDLE | 44/36 | position 3 -> 8 |
| global_free_list (L704) | void * | 32/24 | hsGlobalFreeList (L218) | IMG_HANDLE | 24/20 | position 4 -> 5 |
| free_list_check (L705) | pvr_srv_bool | 40/28 | bbFreeListCheck (L219) | IMG_BOOL | 32/24 | position 5 -> 6 |
| grow_free_list_pages (L706) | uint32_t | 44/32 | ui32GrowFLPages (L216) | IMG_UINT32 | 16/12 | position 6 -> 3 |
| grow_param_threshold (L707) | uint32_t | 48/36 | ui32GrowParamThreshold (L217) | IMG_UINT32 | 20/16 | position 7 -> 4 |
| init_free_list_pages (L708) | uint32_t | 52/40 | ui32InitFLPages (L215) | IMG_UINT32 | 12/8 | position 8 -> 2 |
| max_free_list_pages (L709) | uint32_t | 56/44 | ui32MaxFLPages (L214) | IMG_UINT32 | 8/4 | position 9 -> 1 |
- OUT: IDENTICAL (pvr_srv_rgx_create_free_list_ret vs PVRSRV_BRIDGE_OUT_RGXCREATEFREELIST)

#### RGXTA3D_CREATERENDERCONTEXT

IN: UP `pvr_srv_rgx_create_render_context_cmd` (UP:pvr_srv_bridge.h:733, packed, total 72 B LP64 / 60 B ILP32) vs KM `PVRSRV_BRIDGE_IN_RGXCREATERENDERCONTEXT` (KM:generated/rogue/rgxta3d_bridge/common_rgxta3d_bridge.h:253, packed, total 68 B LP64 / 56 B ILP32)

| 1.17 field (line) | type | off LP64/ILP32 | 1.13 field (line) | type | off LP64/ILP32 | note |
|---|---|---|---|---|---|---|
| vdm_callstack_addr (L734) | pvr_dev_addr_t | 0/0 | sVDMCallStackAddr (L256) | IMG_DEV_VIRTADDR | 4/4 | position 0 -> 1 |
| robustness_address (L735) | uint64_t | 8/8 | ui64RobustnessAddress (L264) | IMG_UINT64 | 52/40 | position 1 -> 9 |
| priv_data (L736) | void * | 16/16 | hPrivData (L259) | IMG_HANDLE | 24/20 | position 2 -> 4 |
| reset_framework_cmd (L737) | uint8_t * | 24/20 | psFrameworkCmd (L258) | IMG_BYTE * | 16/16 | offset shifted |
| static_render_context_state (L738) | uint8_t * | 32/24 | psStaticRenderContextState (L261) | IMG_BYTE * | 36/28 | position 4 -> 6 |
| context_flags (L740) | uint32_t | 40/28 | ui32ContextFlags (L263) | IMG_UINT32 | 48/36 | position 5 -> 8 |
| reset_framework_cmd_size (L741) | uint32_t | 44/32 | ui32FrameworkCmdSize (L257) | IMG_UINT32 | 12/12 | position 6 -> 2 |
| max_3d_deadline_ms (L742) | uint32_t | 48/36 | ui32Max3DDeadlineMS (L266) | IMG_UINT32 | 64/52 | position 7 -> 11 |
| max_ta_deadline_ms (L743) | uint32_t | 52/40 | ui32MaxTADeadlineMS (L265) | IMG_UINT32 | 60/48 | position 8 -> 10 |
| packed_ccb_size (L744) | uint32_t | 56/44 | ui32PackedCCBSizeU8888 (L262) | IMG_UINT32 | 44/32 | position 9 -> 7 |
| priority (L749) | uint32_t | 60/48 | ui32Priority (L255) | IMG_UINT32 | 0/0 | position 10 -> 0 |
| static_render_context_state_size (L750) | uint32_t | 64/52 | ui32StaticRenderContextStateSize (L260) | IMG_UINT32 | 32/24 | position 11 -> 5 |
| call_stack_depth (L751) | uint32_t | 68/56 | - | - | - | **missing in 1.13** |
- OUT: IDENTICAL (pvr_srv_rgx_create_render_context_ret vs PVRSRV_BRIDGE_OUT_RGXCREATERENDERCONTEXT)

#### RGXTA3D_KICKTA3D2

IN: UP `pvr_srv_rgx_kick_ta3d2_cmd` (UP:pvr_srv_bridge.h:775, packed, total 268 B LP64 / 184 B ILP32) vs KM `PVRSRV_BRIDGE_IN_RGXKICKTA3D2` (KM:generated/rogue/rgxta3d_bridge/common_rgxta3d_bridge.h:348, packed, total 272 B LP64 / 188 B ILP32)

| 1.17 field (line) | type | off LP64/ILP32 | 1.13 field (line) | type | off LP64/ILP32 | note |
|---|---|---|---|---|---|---|
| deadline (L776) | uint64_t | 0/0 | ui64Deadline (L395) | IMG_UINT64 | 264/180 | position 0 -> 45 |
| hw_rt_dataset (L777) | void * | 8/8 | hKMHWRTDataSet (L385) | IMG_HANDLE | 204/140 | position 1 -> 35 |
| msaa_scratch_buffer (L778) | void * | 16/12 | hMSAAScratchBuffer (L387) | IMG_HANDLE | 220/148 | position 2 -> 37 |
| pr_fence_ufo_sync_prim_block (L779) | void * | 24/16 | hPRFenceUFOSyncPrimBlock (L364) | IMG_HANDLE | 96/56 | position 3 -> 14 |
| render_ctx (L780) | void * | 32/20 | hRenderContext (L350) | IMG_HANDLE | 0/0 | position 4 -> 0 |
| zs_buffer (L781) | void * | 40/24 | hZSBuffer (L386) | IMG_HANDLE | 212/144 | position 5 -> 36 |
| client_3d_update_sync_offset (L782) | uint32_t * | 48/28 | pui32Client3DUpdateSyncOffset (L362) | IMG_UINT32 * | 80/48 | position 6 -> 12 |
| client_3d_update_value (L783) | uint32_t * | 56/32 | pui32Client3DUpdateValue (L363) | IMG_UINT32 * | 88/52 | position 7 -> 13 |
| client_ta_fence_sync_offset (L784) | uint32_t * | 64/36 | pui32ClientTAFenceSyncOffset (L354) | IMG_UINT32 * | 24/16 | position 8 -> 4 |
| client_ta_fence_value (L785) | uint32_t * | 72/40 | pui32ClientTAFenceValue (L355) | IMG_UINT32 * | 32/20 | position 9 -> 5 |
| client_ta_update_sync_offset (L786) | uint32_t * | 80/44 | pui32ClientTAUpdateSyncOffset (L358) | IMG_UINT32 * | 52/32 | position 10 -> 8 |
| client_ta_update_value (L787) | uint32_t * | 88/48 | pui32ClientTAUpdateValue (L359) | IMG_UINT32 * | 60/36 | position 11 -> 9 |
| sync_pmr_flags (L788) | uint32_t * | 96/52 | pui32SyncPMRFlags (L389) | IMG_UINT32 * | 232/156 | position 12 -> 39 |
| cmd_3d (L789) | uint8_t * | 104/56 | ps3DCmd (L378) | IMG_BYTE * | 172/112 | position 13 -> 28 |
| cmd_3d_pr (L790) | uint8_t * | 112/60 | ps3DPRCmd (L376) | IMG_BYTE * | 160/104 | position 14 -> 26 |
| cmd_ta (L791) | uint8_t * | 120/64 | psTACmd (L374) | IMG_BYTE * | 148/96 | position 15 -> 24 |
| update_fence_name (L792) | char * | 128/68 | puiUpdateFenceName (L369) | IMG_CHAR * | 120/76 | position 16 -> 19 |
| update_fence_name_3d (L793) | char * | 136/72 | puiUpdateFenceName3D (L372) | IMG_CHAR * | 136/88 | position 17 -> 22 |
| client_3d_update_sync_prim_block (L794) | void ** | 144/76 | phClient3DUpdateSyncPrimBlock (L361) | IMG_HANDLE * | 72/44 | position 18 -> 11 |
| client_ta_fence_sync_prim_block (L795) | void ** | 152/80 | phClientTAFenceSyncPrimBlock (L353) | IMG_HANDLE * | 16/12 | position 19 -> 3 |
| client_ta_update_sync_prim_block (L796) | void ** | 160/84 | phClientTAUpdateSyncPrimBlock (L357) | IMG_HANDLE * | 44/28 | position 20 -> 7 |
| sync_pmrs (L797) | void ** | 168/88 | phSyncPMRs (L390) | IMG_HANDLE * | 240/160 | position 21 -> 40 |
| abort (L798) | pvr_srv_bool | 176/92 | bbAbort (L383) | IMG_BOOL | 196/132 | position 22 -> 33 |
| kick_3d (L799) | pvr_srv_bool | 180/96 | bbKick3D (L382) | IMG_BOOL | 192/128 | position 23 -> 32 |
| kick_pr (L800) | pvr_srv_bool | 184/100 | bbKickPR (L381) | IMG_BOOL | 188/124 | position 24 -> 31 |
| kick_ta (L801) | pvr_srv_bool | 188/104 | bbKickTA (L380) | IMG_BOOL | 184/120 | position 25 -> 30 |
| check_fence (L802) | int32_t | 192/108 | hCheckFence (L367) | PVRSRV_FENCE | 112/68 | position 26 -> 17 |
| check_fence_3d (L803) | int32_t | 196/112 | hCheckFence3D (L370) | PVRSRV_FENCE | 128/80 | position 27 -> 20 |
| update_timeline (L804) | int32_t | 200/116 | hUpdateTimeline (L368) | PVRSRV_TIMELINE | 116/72 | position 28 -> 18 |
| update_timeline_3d (L805) | int32_t | 204/120 | hUpdateTimeline3D (L371) | PVRSRV_TIMELINE | 132/84 | position 29 -> 21 |
| cmd_3d_size (L806) | uint32_t | 208/124 | ui323DCmdSize (L377) | IMG_UINT32 | 168/108 | position 30 -> 27 |
| cmd_3d_pr_size (L807) | uint32_t | 212/128 | ui323DPRCmdSize (L375) | IMG_UINT32 | 156/100 | position 31 -> 25 |
| client_3d_update_count (L808) | uint32_t | 216/132 | ui32Client3DUpdateCount (L360) | IMG_UINT32 | 68/40 | position 32 -> 10 |
| client_ta_fence_count (L809) | uint32_t | 220/136 | ui32ClientTAFenceCount (L352) | IMG_UINT32 | 12/8 | position 33 -> 2 |
| client_ta_update_count (L810) | uint32_t | 224/140 | ui32ClientTAUpdateCount (L356) | IMG_UINT32 | 40/24 | position 34 -> 6 |
| ext_job_ref (L811) | uint32_t | 228/144 | ui32ExtJobRef (L379) | IMG_UINT32 | 180/116 | position 35 -> 29 |
| num_draw_calls (L812) | uint32_t | 232/148 | ui32NumberOfDrawCalls (L392) | IMG_UINT32 | 252/168 | position 36 -> 42 |
| num_indices (L813) | uint32_t | 236/152 | ui32NumberOfIndices (L393) | IMG_UINT32 | 256/172 | position 37 -> 43 |
| num_mrts (L814) | uint32_t | 240/156 | ui32NumberOfMRTs (L394) | IMG_UINT32 | 260/176 | position 38 -> 44 |
| pdump_flags (L815) | uint32_t | 244/160 | ui32PDumpFlags (L384) | IMG_UINT32 | 200/136 | position 39 -> 34 |
| client_pr_fence_ufo_sync_offset (L816) | uint32_t | 248/164 | ui32FRFenceUFOSyncOffset (L365) | IMG_UINT32 | 104/60 | position 40 -> 15 |
| client_pr_fence_value (L817) | uint32_t | 252/168 | ui32FRFenceValue (L366) | IMG_UINT32 | 108/64 | position 41 -> 16 |
| render_target_size (L818) | uint32_t | 256/172 | ui32RenderTargetSize (L391) | IMG_UINT32 | 248/164 | position 42 -> 41 |
| sync_pmr_count (L819) | uint32_t | 260/176 | ui32SyncPMRCount (L388) | IMG_UINT32 | 228/152 | position 43 -> 38 |
| cmd_ta_size (L820) | uint32_t | 264/180 | ui32TACmdSize (L373) | IMG_UINT32 | 144/92 | position 44 -> 23 |
| - | - | - | ui32ClientCacheOpSeqNum (L351) | IMG_UINT32 | 8/4 | **added in 1.13** |

OUT: UP `pvr_srv_rgx_kick_ta3d2_ret` (UP:pvr_srv_bridge.h:823, packed, total 12 B LP64 / 12 B ILP32) vs KM `PVRSRV_BRIDGE_OUT_RGXKICKTA3D2` (KM:generated/rogue/rgxta3d_bridge/common_rgxta3d_bridge.h:399, packed, total 12 B LP64 / 12 B ILP32)

| 1.17 field (line) | type | off LP64/ILP32 | 1.13 field (line) | type | off LP64/ILP32 | note |
|---|---|---|---|---|---|---|
| error (L824) | pvr_srv_error | 0/0 | eError (L403) | PVRSRV_ERROR | 8/8 | position 0 -> 2 |
| update_fence (L825) | int32_t | 4/4 | hUpdateFence (L401) | PVRSRV_FENCE | 0/0 | position 1 -> 0 |
| update_fence_3d (L826) | int32_t | 8/8 | hUpdateFence3D (L402) | PVRSRV_FENCE | 4/4 | position 2 -> 1 |

#### RGXTA3D_CREATEHWRTDATASET (structurally different)

UP IN `pvr_srv_rgx_create_hwrt_dataset_cmd` `UP:pvr_srv_bridge.h:639-675` vs KM `PVRSRV_BRIDGE_IN_RGXCREATEHWRTDATASET` `KM:generated/rogue/rgxta3d_bridge/common_rgxta3d_bridge.h:81-112`.

- 1.17 passes the device addresses as **pointers to arrays** (`macrotile_array_dev_addrs` / `pm_mlist_dev_addrs` / `rgn_header_dev_addrs` with `ROGUE_FWIF_NUM_RTDATAS = 2` entries; `rtc_dev_addrs` / `tail_ptrs_dev_addrs` / `vheap_table_dev_adds` with `ROGUE_FWIF_NUM_GEOMDATAS = 1`) and gets the two dataset handles written through an IN pointer `hwrt_dataset` (`UP:pvr_srv_bridge.c:1226-1262` comment: "hwrt_dataset_out is passed in the cmd struct which the kernel writes to").
- 1.13 passes the same addresses **by value** (suffix 0/1 for the 2-entry ones) and returns the two handles in the OUT struct as `hKmHwRTDataSet0/1` (`KM:...common_rgxta3d_bridge.h:115-120`).
- `rgn_header_size` is u32 in 1.17 and **u64** (`ui64uiRgnHeaderSize`) in 1.13.
- 1.13 has no `hwrt_dataset` IN pointer; `free_lists` is `phapsFreeLists` (`IMG_HANDLE *`, `RGXFW_MAX_FREELISTS = 2`, `KM:include/rogue/rgx_fwif_shared.h:156-161`; upstream `ROGUE_FW_MAX_FREELISTS = 2`, `UP:fw-api/pvr_rogue_fwif_shared.h:200`).
- Field names carry a `ui32ui32`/`ui64ui` doubled prefix in 1.13 (generator artifact, not a layout difference).

| 1.13 IN field (line) | type | off LP64/ILP32 | 1.17 equivalent |
|---|---|---|---|
| ssVHeapTableDevVAddr (L83) | IMG_DEV_VIRTADDR | 0/0 | vheap_table_dev_adds[0] (by value; 1.17 passes `const pvr_dev_addr_t *` to ROGUE_FWIF_NUM_GEOMDATAS=1 element array) |
| sPMMlistDevVAddr0 (L84) | IMG_DEV_VIRTADDR | 8/8 | pm_mlist_dev_addrs[0] (1.17: pointer to NUM_RTDATAS=2 array) |
| sPMMlistDevVAddr1 (L85) | IMG_DEV_VIRTADDR | 16/16 | pm_mlist_dev_addrs[1] |
| phapsFreeLists (L86) | IMG_HANDLE * | 24/24 | free_lists (void **, ROGUE_FW_MAX_FREELISTS=2 handles) - same meaning, different position |
| ui32PPPScreen (L87) | IMG_UINT32 | 32/28 | ppp_screen |
| ui64MultiSampleCtl (L88) | IMG_UINT64 | 36/32 | multi_sample_ctl |
| ui64FlippedMultiSampleCtl (L89) | IMG_UINT64 | 44/40 | flipped_multi_sample_ctl |
| ui32TPCStride (L90) | IMG_UINT32 | 52/48 | tpc_stride |
| sTailPtrsDevVAddr (L91) | IMG_DEV_VIRTADDR | 56/52 | tail_ptrs_dev_addrs[0] (pointer to 1-element array in 1.17) |
| ui32TPCSize (L92) | IMG_UINT32 | 64/60 | tpc_size |
| ui32TEScreen (L93) | IMG_UINT32 | 68/64 | te_screen |
| ui32TEAA (L94) | IMG_UINT32 | 72/68 | te_aa |
| ui32TEMTILE1 (L95) | IMG_UINT32 | 76/72 | te_mtile1 |
| ui32TEMTILE2 (L96) | IMG_UINT32 | 80/76 | te_mtile2 |
| ui32MTileStride (L97) | IMG_UINT32 | 84/80 | mtile_stride |
| ui32ui32ISPMergeLowerX (L98) | IMG_UINT32 | 88/84 | isp_merge_lower_x |
| ui32ui32ISPMergeLowerY (L99) | IMG_UINT32 | 92/88 | isp_merge_lower_y |
| ui32ui32ISPMergeUpperX (L100) | IMG_UINT32 | 96/92 | isp_merge_upper_x |
| ui32ui32ISPMergeUpperY (L101) | IMG_UINT32 | 100/96 | isp_merge_upper_y |
| ui32ui32ISPMergeScaleX (L102) | IMG_UINT32 | 104/100 | isp_merge_scale_x |
| ui32ui32ISPMergeScaleY (L103) | IMG_UINT32 | 108/104 | isp_merge_scale_y |
| ssMacrotileArrayDevVAddr0 (L104) | IMG_DEV_VIRTADDR | 112/108 | macrotile_array_dev_addrs[0] (pointer to 2-element array in 1.17) |
| ssMacrotileArrayDevVAddr1 (L105) | IMG_DEV_VIRTADDR | 120/116 | macrotile_array_dev_addrs[1] |
| ssRgnHeaderDevVAddr0 (L106) | IMG_DEV_VIRTADDR | 128/124 | rgn_header_dev_addrs[0] (pointer to 2-element array in 1.17) |
| ssRgnHeaderDevVAddr1 (L107) | IMG_DEV_VIRTADDR | 136/132 | rgn_header_dev_addrs[1] |
| ssRTCDevVAddr (L108) | IMG_DEV_VIRTADDR | 144/140 | rtc_dev_addrs[0] (pointer to 1-element array in 1.17) |
| ui64uiRgnHeaderSize (L109) | IMG_UINT64 | 152/148 | rgn_header_size (u32 in 1.17 -> **u64 in 1.13**) |
| ui32ui32ISPMtileSize (L110) | IMG_UINT32 | 160/156 | isp_mtile_size |
| ui16MaxRTs (L111) | IMG_UINT16 | 164/160 | max_rts |

1.13 IN total: 166 B LP64 / 162 B ILP32 (KM:generated/rogue/rgxta3d_bridge/common_rgxta3d_bridge.h:81-112)
1.17 IN total: 146 B LP64 / 114 B ILP32 (UP:pvr_srv_bridge.h:639-675)

OUT: UP `pvr_srv_rgx_create_hwrt_dataset_ret {void **hwrt_dataset; enum error}` (`UP:pvr_srv_bridge.h:677-681`, 12 B LP64 / 8 B ILP32, the `hwrt_dataset` member is unused by upstream) vs KM `{IMG_HANDLE hKmHwRTDataSet0; IMG_HANDLE hKmHwRTDataSet1; PVRSRV_ERROR eError}` (`KM:...common_rgxta3d_bridge.h:115-120`, 20 B LP64 / 12 B ILP32). Both handles are later destroyed one at a time by the identical DESTROYHWRTDATASET (`UP:pvr_srv_bridge.h:687`, `KM:...common_rgxta3d_bridge.h:127`).

### 2.3 IDENTICAL functions (13)

SRVCORE_DISCONNECT, SYNC_FREESYNCPRIMITIVEBLOCK, SYNC_SYNCPRIMSET, MM_PMRUNREFUNLOCKPMR, MM_DEVMEMINTCTXDESTROY, MM_DEVMEMINTHEAPDESTROY, MM_DEVMEMINTUNMAPPMR, MM_DEVMEMINTUNRESERVERANGE, RGXTQ_DESTROYTRANSFERCONTEXT, RGXCMP_DESTROYCOMPUTECONTEXT, RGXTA3D_DESTROYHWRTDATASET, RGXTA3D_DESTROYFREELIST, RGXTA3D_DESTROYRENDERCONTEXT. Struct locations are in the summary table.

Observation: all 13 are 0- or 1-field requests plus `error`; none carries more than one non-error field except SYNCPRIMSET (handle, index, value; same order both sides: `UP:pvr_srv_bridge.h:255-259`, `KM:generated/rogue/sync_bridge/common_sync_bridge.h:113-118`).


## 3. Connect handshake (task 2)

### 3.1 Wire format of CONNECT

| | 1.17 upstream `UP:pvr_srv_bridge.h:190-202` | 1.13 `KM:generated/rogue/srvcore_bridge/common_srvcore_bridge.h:81-96` |
|---|---|---|
| IN (16 B) | `{u32 build_options; u32 DDK_build; u32 DDK_version; u32 flags}` | `{u32 ui32Flags; u32 ui32ClientBuildOptions; u32 ui32ClientDDKVersion; u32 ui32ClientDDKBuild}` |
| OUT (17 B) | `{u64 bvnc; u32 error; u32 capability_flags; u8 kernel_arch}` | `{u8 ui8KernelArch; u32 ui32CapabilityFlags; u64 ui64PackedBvnc; u32 eError}` |
| Effect of using the 1.17 layout on 1.13 | the kernel reads `build_options` as `ui32Flags`, `DDK_build` as `ClientBuildOptions`, `DDK_version` as `ClientDDKVersion`, `flags` as `ClientDDKBuild`; every field is misplaced except `DDK_version`, which happens to sit at offset 8 in both. On the way back `bvnc` would be read from bytes 0-7 (kernel arch + caps + 3 bytes of BVNC) and `error` from bytes 8-11 (middle of the BVNC) | - |

Handler: `PVRSRVBridgeConnect` calls `PVRSRVConnectKM(..., ui32Flags, ui32ClientBuildOptions, ui32ClientDDKVersion, ui32ClientDDKBuild, &ui8KernelArch, &ui32CapabilityFlags, &ui64PackedBvnc)` `KM:generated/rogue/srvcore_bridge/server_srvcore_bridge.c:72-93`; implementation `KM:services/server/common/srvcore.c:299-685`.

### 3.2 How version and build are encoded and checked

| Item | Upstream sends | 1.13 requires | Citation |
|---|---|---|---|
| DDK version | `PVR_SRV_VERSION = (1 << 16) \| 17` = 0x00010011 | `PVRVERSION_PACK(1, 13)` = **0x0001000D**, exact match. Mismatch is a hard failure `PVRSRV_ERROR_DDK_VERSION_MISMATCH` | `UP:pvr_srv_bridge.h:140-145`; `KM:services/server/common/srvcore.c:461,557-567`; `KM:include/pvrversion.h:47-48,64` |
| DDK build | `6256262` (`PVR_SRV_VERSION_BUILD`) | `PVRVERSION_BUILD` = 5776728. Mismatch only logs a warning and sets `bIsNoMatch`; it becomes a hard failure only under `PVRSRV_STRICT_COMPAT_CHECK`. That macro is only tested in `#if` and never defined: not in `KM:include/config_kernel_user_mt6765.h`, `config_kernel_eng_mt6765.h` or `KM:Makefile` (grep), assuming the phone's binary was built from this tree (**unverified**). Safest: send 5776728 | `UP:pvr_srv_bridge.h:147`; `KM:services/server/common/srvcore.c:621-631`; `KM:include/pvrversion.h:52` |
| Flags | `PVR_SRV_FLAGS_CLIENT_64BIT_COMPAT` = bit 5, always set | bit 5 only selects `ui8UMSupportedArch` bookkeeping, no failure path. A 32-bit build should send 0 | `UP:pvr_srv_bridge.h:150`, `UP:pvr_srv_bridge.c:163`; `KM:include/services_km.h:98`; `KM:services/server/common/srvcore.c:464-471` |
| Build options | `RGX_BUILD_OPTIONS` = bit4 (SUPPORT_RGX) \| bit10 (DEBUG) \| bit11 (BUFFER_SYNC) \| bit31 = **0x80000C10** | see 3.3 | `UP:pvr_srv_bridge.h:131-138`; `UP:pvr_srv_bridge.c:164` |
| BVNC | n/a (output) | `rgx_bvnc_pack`: B at bit 48, then V, N, C at 16-bit spacing (`KM:services/include/rgx_compat_bvnc.h:90-129`); upstream's unpack macro was not diffed (**unverified**), phone BVNC 22.87.104.18 is already known | `KM:services/server/common/srvcore.c:321` |
| Kernel arch | ignored by upstream | OUT `ui8KernelArch` = 64 or 32 depending on `sizeof(void *)` of the kernel; tells the UM which pointer width to marshal | `KM:services/server/common/srvcore.c:490-496,646-653` |

### 3.3 Build option check, exactly

1. `ui32ClientBuildOptions &= RGX_BUILD_OPTIONS_MASK_KM` (0x0000FFFF) - bit 31 is dropped before comparison (`KM:services/server/common/srvcore.c:500-501`, `KM:include/rogue/rgx_options.h:63`).
2. `ui32BuildOptions = RGX_BUILD_OPTIONS_KM` (compile-time of the kernel; phone: 0x10, only `SUPPORT_RGX_SET_OFFSET` = bit 4, `KM:include/rogue/rgx_options.h:109-110,228-243`) (`:505-507`).
3. If different: `mismatch = KM ^ client`, then **non-strict** (no `PVRSRV_STRICT_COMPAT_CHECK`): `mismatch &= OPTIONS_STRICT` (`:510-513`), where `OPTIONS_STRICT = RGX_BUILD_OPTIONS & ~(DEBUG | WORKLOAD_ESTIMATION | PDVFS)` (`KM:include/rogue/rgx_options.h:253-256`). With a KM of 0x10, `RGX_BUILD_OPTIONS` = 0x80000010, so `OPTIONS_STRICT` = 0x80000010.
4. Fail (`PVRSRV_ERROR_BUILD_OPTIONS_MISMATCH`) if `client & mismatch` != 0 or `KM & mismatch` != 0 (`:514-530`).

Consequences for this phone: because `OPTIONS_STRICT` contains only bits 4 and 31 and bit 31 was already masked off, **the only requirement is that bit 4 (0x10) is set**. Bits 10 and 11 (what upstream adds) fall outside `OPTIONS_STRICT`, so upstream's 0x80000C10 would pass, logging a "COMPAT_TEST ... options differ" line (`:533-548`). Any other bit set that is not in `OPTIONS_STRICT` is also ignored. Recommended value: **0x80000010**, identical to what the vendor UM sends (`/proc/pvr/version`, measured on the phone). Missing bit 4 is the only failing case. The strict set follows the kernel's own compile options. Computed from the MTK config for this core, `SUPPORT_RGX 1` is the only option that maps to a bit (`KM:include/config_kernel_user_mt6765.h:14`; `RELEASE` at :15 so no DEBUG bit 10; no `PDUMP`, `SUPPORT_BUFFER_SYNC`, `SUPPORT_SECURE_EXPORT`, `SUPPORT_INSECURE_EXPORT`, `SUPPORT_VFP`, `SUPPORT_WORKLOAD_ESTIMATION`, `SUPPORT_PDVFS`), i.e. KM = 0x10, which matches `/proc/pvr/version` (measured on the phone).

### 3.4 ALIGNMENTCHECK

- **Not required.** `PVRSRV_BRIDGE_SRVCORE_ALIGNMENTCHECK` = SRVCORE function 10 (`KM:generated/rogue/srvcore_bridge/common_srvcore_bridge.h:67`) is a pure verification call: `PVRSRVAlignmentCheckKM` -> `RGXAlignmentCheck` copies nothing, sets no connection state, and returns `PVRSRV_ERROR_INVALID_ALIGNMENT` on mismatch (`KM:services/server/common/srvcore.c:808-818`, `KM:services/server/devices/rogue/rgxinit.c:1816-1880`). No other code path reads a "checked" flag (grep for `AlignmentCheck`/`AlignChecks` over `KM:services` finds only these).
- **Upstream sends nothing.** There is no ALIGNMENTCHECK call anywhere in `UP:*.c`/`*.h` (grep `-i align` matches only buffer alignment in `pvr_srv_bo.c`).
- **What the vendor UM would send** (`RGXFW_ALIGN_CHECKS_UM`; the name in 1.13 is `RGXFW_ALIGN_CHECKS_INIT`, `KM:include/rogue/rgx_fwif_alignchecks.h:62-147`): an array of `u32` sizeof/offsetof values, first the array length, validated against the FW copy at `ui32UMChecksOffset = ARRAY_SIZE(RGXFW_ALIGN_CHECKS_INIT_KM) + 1` (`rgxinit.c:1821,1848-1856`). For BVNC 22.87.104.18 neither `RGX_FEATURE_TLA` nor `RGX_FEATURE_FASTRENDER_DM` is defined (`KM:hwdefs/rogue/km/configs/rgxconfig_km_22.V.104.18.h:54-78`), so the list is `RGXFW_ALIGN_CHECKS_INIT0` = **43 u32 entries** (counted `sizeof`/`offsetof`; `rgx_fwif_alignchecks.h:62-129`): TRACEBUF (5), SYSDATA (6), OSDATA (3), HWRINFOBUF (5), CMDTA (2: size, `sGeomRegs`), CMD3D (2: size, `s3DRegs`), CMDTRANSFER (2: size, `sTransRegs`), CMD_COMPUTE (2: size, `sCDMRegs`), FREELIST (4), HWRTDATA (6), HWRTDATA_COMMON (2), HWPERF_CTL_BLK (2), HWPERF_CTL (2). Max UM entries `RGXFW_ALIGN_CHECKS_UM_MAX` = 128.
- **Use for this port (optional):** a mismatch makes the kernel log `size/offset mismatch in RGXFW_ALIGN_CHECKS_INIT[i] between UM(x) and FW(y)` with both values (`rgxinit.c:1865-1869`), which would reveal the real sizeof/offsets of CMDTA/CMD3D/CMDTRANSFER/CMD_COMPUTE in the running FW. Needs `dmesg` access; **unverified** that the log level is visible on the phone.

### 3.5 Everything needed to get past CONNECT on 1.13

1. `drmGetVersion` = pvr 1.13 (not 1.17).
2. Skip `DRM_IOCTL_SRVKM_INIT`.
3. IN = `{flags, build_options(bit4 set), 0x0001000D, 5776728}` in that order; read OUT in the 1.13 order.
4. Match UM pointer width to `ui8KernelArch` (inference above).


## 4. Firmware interface (task 3)

Where the 1.13 source stops: the kernel tree contains the UM/KM/FW **shared** header `KM:include/rogue/rgx_fwif_shared.h` and the KM/FW header `KM:services/include/rogue/rgx_fwif_km.h`, but **not** the per-DM command structs (`RGXFWIF_CMDTA`, `RGXFWIF_CMD3D`, `RGXFWIF_CMDTRANSFER`, `RGXFWIF_CMD_COMPUTE`, the TA/3D/transfer/CDM register blocks): the symbols appear only inside `sizeof()`/`offsetof()` probes in `KM:include/rogue/rgx_fwif_alignchecks.h:86-101` (and 2D/TDM at `:132-143`, not applicable to this core). The kernel treats these command blobs as opaque byte arrays bounded by `RGXFWIF_DM_INDEPENDENT_KICK_CMD_SIZE` = 1024 (`KM:include/rogue/rgx_fwif_shared.h:71`), with one exception: the first 12 bytes of the TA, 3D and 3D-PR blobs.

### 4.1 Upstream structs vs 1.13 counterparts (27 structs, 2 enums)

UP lines are in `UP:fw-api/pvr_rogue_fwif_shared.h` (S), `pvr_rogue_fwif.h` (F), `pvr_rogue_fwif_rf.h` (R). KM lines in `KM:include/rogue/rgx_fwif_shared.h` (KS), `KM:services/include/rogue/rgx_fwif_km.h` (KK), `KM:services/include/rogue/rgx_fwif_resetframework.h` (KR).

| # | Upstream struct (line) | 1.13 counterpart (line) | Verdict | Detail |
|---|---|---|---|---|
| 1 | `rogue_fwif_dev_addr` (S:61) | `RGXFWIF_DEV_VIRTADDR` (KS:78-81) | IDENTICAL | `{u32 addr}` |
| 2 | `rogue_fwif_dma_addr` (S:65) | `RGXFWIF_DMA_ADDR` (KS:83-87) | IDENTICAL | 16 B: u64 @0, fw addr u32 @8, 4 B pad |
| 3 | `rogue_fwif_ufo` (S:74) | `RGXFWIF_UFO` (KS:95-99) | IDENTICAL | `{u32 addr, u32 value}`; the kernel lays an array of these after `RGXFWIF_CCB_CMD_HEADER` for FENCE/UPDATE commands (`KM:services/server/devices/rogue/rgxccb.c:1360-1372`) |
| 4 | `rogue_fwif_cleanup_ctl` (S:81) | `RGXFWIF_CLEANUP_CTL` (KS:101-105) | IDENTICAL | 8 B, 8-aligned |
| 5 | `rogue_fwif_prbuffer` (S:104) | `RGXFWIF_PRBUFFER` (KS:231-238) | IDENTICAL layout | offsets 0/4/8/16/24, 32 B both. `on_demand` is `bool` (1 B) upstream vs `IMG_BOOL` (4 B enum) in 1.13: only padding bytes differ. State enum values 0-3 identical (S:94, KS:223-229). Not used by the winsys. |
| 6 | `rogue_fwif_cmd_common` (S:124) | none | DIFFERENT (absent) | `{u32 frame_num}`. No such word exists in any KM-visible 1.13 UM/FW header (`ui32FrameNum` only in HWPerf records, `KM:include/rogue/rgx_hwperf.h:563,602,1299`). The winsys writes it at `UP:pvr_arch_srv_job_render.c:542,723`, `pvr_arch_srv_job_compute.c:122`, `pvr_arch_srv_job_transfer.c:136`. Whether 1.13's compute/transfer blobs start with it is **unverified** (UM-only layouts). |
| 7 | `rogue_fwif_cmd_ta_3d_shared` (S:139) | `CMDTA3D_SHARED` (KS:117-125) | DIFFERENT | 1.17: `{cmn.frame_num @0, hw_rt_data @4, pr_buffers[2] @8,12}` = 16 B. 1.13: `{sHWRTData @0, asPRBuffer[2] @4,8}` = 12 B. The kernel overwrites the first 12 bytes of the TA, 3D and 3D-PR blobs with `sHWRTData`, `asPRBuffer[ZS]`, `asPRBuffer[MSAA]` (`KM:services/server/devices/rogue/rgxta3d.c:3495-3555`), so a 1.17-layout blob would have `frame_num`, `hw_rt_data` and `pr_buffers[0]` clobbered and `pr_buffers[1]` (offset 12) left unpatched. |
| 8 | `rogue_fwif_cccb_ctl` (S:168) | `RGXFWIF_CCCB_CTL` (KS:141-153) | DIFFERENT | 1.13: `{write, read, dep, wrap}` = 16 B. 1.17 adds `read_offset2/3/4` + padding = 32 B. KM-owned, not touched by the winsys. |
| 9 | `rogue_fwif_ta_regs_cswitch` (S:206) | `RGXFWIF_TAREGISTERS_CSWITCH` (KS:164-188) | IDENTICAL | 3 x u64 + `ta_state[2]` of 10 x u64 = 184 B, same order |
| 10 | `rogue_fwif_cdm_regs_cswitch` (S:240) | `RGXFWIF_CDM_REGISTERS_CSWITCH` (KS:192-205) | DIFFERENT | 1.13 has a leading `uCDMReg_CDM_CONTEXT_STATE_BASE_ADDR`: 8 x u64 = 64 B. 1.17 has 7 x u64 = 56 B (that field moved into `rogue_fwif_cdm_regs`, F:462). Every following field is at +8 in 1.13. |
| 11 | `rogue_fwif_static_rendercontext_state` (S:255) | `RGXFWIF_STATIC_RENDERCONTEXT_STATE` (KS:207-210) | DIFFERENT | 1.17: `ctx_switch_geom_regs[ROGUE_NUM_GEOM_CORES_SIZE = 2]` = 368 B. 1.13: one `RGXFWIF_TAREGISTERS_CSWITCH` = 184 B. **Hard failure**: `ui32StaticRenderContextStateSize > RGXFWIF_STATIC_RENDERCONTEXT_SIZE` is rejected in the bridge (`KM:generated/rogue/rgxta3d_bridge/server_rgxta3d_bridge.c:960-967`, `PVRSRV_ERROR_BRIDGE_ARRAY_SIZE_TOO_BIG`) and again in `KM:services/server/devices/rogue/rgxta3d.c:2883`. Upstream passes `sizeof(static_state)` (`UP:pvr_srv_job_render.c:269`). |
| 12 | `rogue_fwif_static_computecontext_state` (S:264) | `RGXFWIF_STATIC_COMPUTECONTEXT_STATE` (KS:214-217) | DIFFERENT | 56 B vs 64 B (consequence of #10). 56 <= 64, so the bridge accepts it (`KM:generated/rogue/rgxcmp_bridge/server_rgxcmp_bridge.c:128-136`), but the 7 fields land one slot early in the FW's view (`cdm_context_pds0` would be read as the state base address, etc.). Upstream: `UP:pvr_srv_job_compute.c:98` |
| 13 | `rogue_context_reset_reason_data` (S:301) | `RGXFWIF_FWCCB_CMD_CONTEXT_RESET_DATA` (KK:1321-1330) | DIFFERENT | 1.13 has context id, reason, job ref, page fault flag, PC address, fault address. Unused by the winsys. |
| 14 | `rogue_fwif_ta_regs` (F:154) | none | UM-ONLY | not in `KM:` tree |
| 15 | `rogue_fwif_dummy_rgnhdr_init_geom_regs` (F:187) | none | UM-ONLY | |
| 16 | `rogue_fwif_cmd_ta` (F:199) | `RGXFWIF_CMDTA` (symbol only, `KM:include/rogue/rgx_fwif_alignchecks.h:86-88`) | UM-ONLY | only the shared prefix (row 7) is visible; `sizeof` and `offsetof(sGeomRegs)` are known to the FW but not in source |
| 17 | `rogue_fwif_3d_regs` (F:242) | none | UM-ONLY | |
| 18 | `rogue_fwif_cmd_3d` (F:326) | `RGXFWIF_CMD3D` (symbol only, `alignchecks.h:90-92`) | UM-ONLY | same note as row 16 |
| 19 | `rogue_fwif_transfer_regs` (F:358) | none | UM-ONLY | |
| 20 | `rogue_fwif_cmd_transfer` (F:407) | `RGXFWIF_CMDTRANSFER` (symbol only, `alignchecks.h:94-96`) | UM-ONLY | |
| 21 | `rogue_fwif_2d_regs` (F:425) | none (`RGXFWIF_CMD2D` only under `RGX_FEATURE_TLA`, absent on this core) | UM-ONLY | not used on GE8320 |
| 22 | `rogue_fwif_cmd_2d` (F:434) | none | UM-ONLY | |
| 23 | `rogue_fwif_cmd_abort` (F:450) | none | UM-ONLY | `{cmd_shared}` only |
| 24 | `rogue_fwif_cdm_regs` (F:462) | none | UM-ONLY | |
| 25 | `rogue_fwif_cmd_compute` (F:501) | `RGXFWIF_CMD_COMPUTE` (symbol only, `alignchecks.h:99-101`) | UM-ONLY | |
| 26 | `rogue_fwif_rf_regs` (R:32) | `RGXFWIF_RF_REGISTERS` (KR:51-58) | DIFFERENT | 1.13 is compile-time: one u64 `CDM_CTRL_STREAM_BASE` unless `RGX_FEATURE_CDM_CONTROL_STREAM_FORMAT == 2` (then `CB_QUEUE, CB_BASE, CB`; order differs from upstream's union at offset 0). This core defines no `CDM_CONTROL_STREAM_FORMAT` (`rgxconfig_km_22.V.104.18.h:54-78`), so 8 B. |
| 27 | `rogue_fwif_rf_cmd` (R:42) | `RGXFWIF_RF_CMD` (KR:62-69, size macro `:72`) | DIFFERENT | 1.13: `{u32 ui32Flags; (pad) regs @8}` = 16 B on this core, flag `RGXFWIF_RF_FLAG_ENABLE = 1` (`KR:60`). Upstream: `regs` only, 24 B, no flags word. Upstream passes size `sizeof(reset_cmd) - sizeof(reset_cmd.regs)` = **0** (`UP:pvr_srv_job_render.c:266`, `pvr_srv_job_compute.c:95`, `pvr_srv_job_transfer.c:86`). The 1.13 bridges accept sizes `<= RGXFWIF_RF_CMD_SIZE` (`server_rgxcmp_bridge.c:118-126`), but the kernel then allocates a framework buffer of exactly that size (`KM:services/server/devices/rogue/rgxfwutils.c:4426-4452`, `DevmemFwAllocate(size=0)`; acceptance of size 0 by `DevmemAllocateAndMap` **unverified**) and the FW reads `ui32Flags` from it. Pass 16 zero bytes. |
| E1 | `enum rogue_fwif_prbuffer_state` (S:94) | `RGXFWIF_PRBUFFER_STATE` (KS:223-229) | IDENTICAL | values 0..3 |
| E2 | `enum rogue_context_reset_reason` (S:275) | `RGXFWIF_CONTEXT_RESET_REASON` (KK:1312-1320) | DIFFERENT (superset) | values 0-5 identical; 1.17 adds 13, 14, 15, 16 |

**Counts over the 27 structs: IDENTICAL 6 (#1, 2, 3, 4, 5, 9), DIFFERENT 9 (#6, 7, 8, 10, 11, 12, 13, 26, 27), UM-ONLY 12 (#14-25).** Enums: 1 identical, 1 different.

The 12 UM-ONLY structs are exactly the ones the winsys fills with real register values (`UP:pvr_arch_srv_job_render.c` 947 lines, `pvr_arch_srv_job_compute.c` 214, `pvr_arch_srv_job_transfer.c` 245). Their 1.13 layouts must be recovered at runtime. The command sizes themselves are visible without decoding anything: capture the vendor GLES driver's `RGXKICKTA3D2` (`ui32TACmdSize`, `ui323DCmdSize`, `ui323DPRCmdSize`), `RGXKICKCDM2` (`ui32CmdSize`) and `RGXSUBMITTRANSFER2` (`pui32CommandSize[]`) calls (bridge ids 130.13, 129.6, 128.3), compare with upstream's `sizeof` (not computed here: nothing compiled), and diff the blob bytes.

### 4.2 Constants and enums

| Upstream | 1.13 | Verdict |
|---|---|---|
| `ROGUE_FWIF_CCB_CMD_MAX_UFOS` = 64 (S:49) | `RGXFWIF_CCB_CMD_MAX_UFOS` = 32+32 (KS:61) | IDENTICAL |
| `ROGUE_FWIF_DM_INDEPENDENT_KICK_CMD_SIZE` = 1024 (S:59) | KS:71 | IDENTICAL |
| `ROGUE_FWIF_PRBUFFER_ZSBUFFER/MSAABUFFER/MAXSUPPORTED` = 0/1/2 (S:90-92) | KS:74-76 | IDENTICAL |
| `ROGUE_FW_LOCAL/GLOBAL_FREELIST`, `ROGUE_FW_MAX_FREELISTS` = 0/1/2 (S:198-200) | `RGXFW_LOCAL/GLOBAL_FREELIST`, `RGXFW_MAX_FREELISTS` (KS:158-161) | IDENTICAL |
| `ROGUE_FWIF_NUM_RTDATAS` = 2, `ROGUE_FWIF_NUM_GEOMDATAS` = 1 (S:33-34) | not defined in the tree (only a comment, `KM:services/server/devices/rogue/rgxta3d.c:1652`) | consistent: the 1.13 CREATEHWRTDATASET struct has `*0/*1` for the 2-entry arrays and one address for the 1-entry ones, and returns 2 handles |
| `ROGUE_NUM_GEOM_CORES(_SIZE)` = 1 / 2 (S:36-38) | none; the 1.13 render context static state has a single TA register block | DIFFERENT (see #11) |
| `PVR_SRV_SYNC_MAX` = 12 (`UP:pvr_srv_bridge.h:154`) | `PVRSRV_MAX_SYNCS` = 4 + 8 (`KM:include/public/powervr/pvrsrv_sync_ext.h:40-50`) | IDENTICAL |
| TQ prepare flags: upstream passes 0 (`cmd_flags[] = {0}`, `UP:pvr_arch_srv_job_transfer.c:164`) | `TQ_PREP_FLAGS_COMMAND_3D` = 0, `_2D` = 1, `_START` bit 5 / `_END` bit 6 (`KM:services/include/rgx_tq_shared.h:49-55`); START/END are not read by the kernel (grep) | IDENTICAL for the value used |
| DM flag constants (`ROGUE_FWIF_TAFLAGS_*`, `RENDERFLAGS_*`, `CMDTRANSFER_*`, `COMPUTE_FLAG_*`, `UP:fw-api/pvr_rogue_fwif.h:35-150`) | not in the kernel tree | UM-ONLY |

### 4.3 Client CCB command types and headers

Upstream defines **no** CCB command type enum and no CCB command header: the strings `ROGUE_FWIF_CCB_CMD_TYPE_*` occur only in comments (`UP:fw-api/pvr_rogue_fwif.h:193,323,405,497`). The kernel builds the client CCB entries from the bridge arguments, so the numeric values below matter only if the winsys ever has to interpret them. 1.13 values (`KM:services/include/rogue/rgx_fwif_km.h:1412-1434`, magic `RGX_CMD_MAGIC_DWORD_SHIFTED` = 0x2ABC0000 at `:840-843`, `RGX_CCB_TYPE_TASK` = 1<<15 at `:1409`): GEOM 0x2ABC80C9 (201), 3D 0x2ABC80CA, CDM 0x2ABC80CB, TQ_3D 0x2ABC80CC, TQ_2D 0x2ABC80CD, 3D_PR 0x2ABC80CE, NULL 0x2ABC80CF, TQ_TDM 0x2ABC80D4 (212), FENCE 0x2ABC00D5 (213), UPDATE 0x2ABC00D6, FENCE_PR 0x2ABC00D8, PRIORITY 0x2ABC00D9, UNFENCED_UPDATE 0x2ABC00DB, PADDING 0x2ABC00DD (221). Header `RGXFWIF_CCB_CMD_HEADER {eCmdType, ui32CmdSize, ui32ExtJobRef, ui32IntJobRef, sWorkEstKickData}` (`:1447-1454`). Comparison against 1.17 values: **not possible, 1.17 values are not in the inputs**.

Freelist / HWRT FW-side structs (`RGXFWIF_FREELIST` KK:1914-1932, `RGXFWIF_HWRTDATA_COMMON` ends KK:1970, `RGXFWIF_HWRTDATA` ends KK:2027, `RGXFWIF_RTA_CTL` KK:1912) have no upstream counterpart (the winsys only passes parameters through the bridge). KM-internal in both versions, nothing to port.

### 4.4 What the kick bridges take for fences and updates

Same model on both sides, same limits. Per DM: arrays of (count, sync-prim-block handle[], offset[], value[]) for fence and update UFOs; a check fence fd (`PVRSRV_FENCE`, int32); an update timeline fd (`PVRSRV_TIMELINE`, int32) plus a 32-char fence name (`PVRSRV_SYNC_NAME_LENGTH = 32`, `KM:include/public/powervr/pvrsrv_sync_ext.h:59`); out fence fd(s). TA3D additionally takes one PR-fence (block, offset, value). Differences are confined to the struct layout (section 2) and: (a) 1.13 has `ui32ClientCacheOpSeqNum` in SUBMITTRANSFER2/KICKCDM2/KICKTA3D2; (b) 1.13 KICKCDM2 has no SyncPMR arguments; TA3D2 and TQ keep `ui32SyncPMRCount`, `pui32SyncPMRFlags`, `phSyncPMRs` (upstream passes 0/NULL).


## 5. Sync (task 4)

Which sync driver the phone runs: `KM:Makefile:181-193` links `pvr_sync_file.c` (+ `pvr_counting_timeline.c`, `pvr_sw_fence.c`, `pvr_fence.c`) if the Android kernel has `CONFIG_SYNC_FILE=y`, otherwise `pvr_sync2.c` (staging `sync_fence`). `SUPPORT_NATIVE_FENCE_SYNC` is defined (`KM:include/config_kernel_user_mt6765.h:138`), `PVR_ANDROID_SYNC_HEADER "linux/sync.h"` (:167), `PVRSYNC_MODNAME "pvr_sync"` (:13). The phone's `CONFIG_SYNC_FILE` is **unverified**; both back-ends register the same misc device `/dev/pvr_sync` (`pvr_sync_file.c:842-845,922`, `pvr_sync2.c:2405-2408,2559`; matches char 10,45 dynamic minor, mode 0666 on the phone) and implement the same four ioctls, so the difference does not matter for the port.

| Operation | Upstream (1.17) | 1.13 kernel | Verdict |
|---|---|---|---|
| Create an update timeline (one per render ctx x2, compute ctx x1, transfer ctx x1) | `pvr_srv_create_timeline`: `drmGetRenderDeviceNameFromFd()`, `open(<render node>, O_CLOEXEC\|O_RDWR)`, then `DRM_IOCTL_SRVKM_INIT(PVR_SRVKM_SYNC_INIT = 2)` `UP:pvr_srv_job_common.c:36-65`; callers `pvr_srv_job_render.c:246,250`, `pvr_srv_job_compute.c:85`, `pvr_srv_job_transfer.c:76` | `open("/dev/pvr_sync", O_RDWR\|O_CLOEXEC)`: each open creates one timeline (`pvr_sync_file.c:172-192`, `pvr_sync2.c:1481-1540`). The fd handed to a kick as `hUpdateTimeline*` is resolved with `fget()` and must satisfy `f_op == &pvr_sync_fops`, and must **not** be a sw-only timeline (`pvr_sync_file.c:145-161,310-324`, `PVRSRV_ERROR_INVALID_PARAMS` otherwise). A render-node fd is rejected. | DIFFERENT: replace the function body (open a different path, no init ioctl) |
| SW timeline for the pre-signaled sync | three DRM ioctls on the timeline fd: `SYNC_FORCE_SW_ONLY` = `DRM_IO(BASE+2)`, `SW_SYNC_CREATE_FENCE` = `DRM_IOWR(BASE+3, struct{char name[32]; s32 fence; u32 pad; u64 sync_pt_idx})`, `SW_SYNC_INC` = `DRM_IOR(BASE+4, struct{u64 sync_pt_idx})` `UP:pvr_srv_bridge.h:104-123,850-859`; callers `UP:pvr_srv_bridge.c:90-158`, flow `UP:pvr_srv.c:798-854` (force sw, create fence, inc, import the fence as the already-signaled sync) | misc-device ioctls on the `/dev/pvr_sync` fd: `PVR_SYNC_IOC_FORCE_SW_ONLY` = `_IO('W', 5)` (`KM:include/pvr_fd_sync_kernel.h:54-60`), `PVR_SW_SYNC_IOC_CREATE_FENCE` = `_IOWR('W', 0, struct pvr_sw_sync_create_fence_data)`, `PVR_SW_SYNC_IOC_INC` = `_IOR('W', 1, struct pvr_sw_timeline_advance_data)` (`KM:include/linux_sw_sync.h:50-63`); dispatch `pvr_sync_file.c:797-831`, `pvr_sync2.c:2212-2245`. After FORCE_SW_ONLY only CREATE_FENCE/INC are accepted, before it only RENAME/FORCE_SW_ONLY (`pvr_sync_file.c:803-826`). | Payload structs IDENTICAL (`{char name[32]; s32 fence; u32 pad; u64 sync_pt_idx}` = 48 B and `{u64 sync_pt_idx}`; `PVRSRV_SYNC_NAME_LENGTH` = 32 `KM:include/public/powervr/pvrsrv_sync_ext.h:59`). Request codes and the fd differ. Call order upstream already satisfies the 1.13 state machine. INC beyond the last created point returns `-EPERM` (`pvr_sync_file.c:781-787`). |
| Kick-time fences | `check_fence` (in fd), `update_timeline` (fd), out `update_fence` (fd), `-1` = none; name string | `hCheckFence*` / `hUpdateTimeline*` / `hUpdateFence*` as `PVRSRV_FENCE`/`PVRSRV_TIMELINE` (int32), `PVRSRV_NO_FENCE = PVRSRV_NO_TIMELINE = -1` (`KM:include/public/powervr/pvrsrv_sync_ext.h:54-64`) | IDENTICAL semantics; struct positions differ (section 2) |
| Returned fence kind | any pollable sync fd | `sync_file` created by `pvr_sync_create_fence` (`pvr_sync_file.c:277-`) or legacy `sync_fence` (`pvr_sync2.c`) | compatible with upstream's use |
| CPU wait | `sync_wait(fd, timeout_ms)` from libsync (poll) `UP:pvr_srv_sync.c:125-152`; multi-wait busy loop `:156-191` | plain fd poll; no render-node ioctl involved | as-is |
| Import/export/dup of sync files | `os_dupfd_cloexec()` `UP:pvr_srv_sync.c:211-255` | no kernel interaction | as-is |
| Sync prim blocks (UFO) | `SYNC_ALLOCSYNCPRIMITIVEBLOCK`, `FREE...`, `SYNCPRIMSET` (`UP:pvr_srv_sync_prim.c`) | same bridge group 2, ids 0/1/2 (extracted); structs: FREE and SET identical, ALLOC out reordered (section 2) | as-is except ALLOC out marshalling |
| Device node access | n/a | `/dev/pvr_sync` char 10,45 mode 0666 (measured on the phone). SELinux for the app domain **unverified**; the vulkan-redmi2 probe saw `untrusted_app` denied a sysfs `uevent` read (`avc: denied { read } ... tcontext=u:object_r:sysfs:s0`). | unverified |

**Sync verdict**: transport DIFFERENT (device node + 3 ioctl request codes + no init ioctl), payloads and fence semantics IDENTICAL. Four upstream functions change: `pvr_srv_create_timeline`, `pvr_srv_set_timeline_sw_only`, `pvr_srv_create_sw_fence`, `pvr_srv_sw_sync_timeline_increment`. `pvr_srv_sync.c` (wait/import/export, 276 lines) needs no change.


## 6. How much of upstream's winsys is reusable as-is for 1.13

Lines are from `wc -l` of the downloaded files (total 8197 incl. `pvr_srv_bo.h`). "As-is" = no change needed for 1.13 ABI reasons; the percentages for partly-changed files are my estimate from the diffs above, not a measurement.

| Group | Files | Lines | Verdict | What has to change |
|---|---|---|---|---|
| Sync objects | `pvr_srv_sync.c/.h`, `pvr_srv_sync_prim.c/.h` | 549 | as-is | nothing (fd-only; bridge group 2 marshalling lives in `pvr_srv_bridge.c`) |
| Buffer objects | `pvr_srv_bo.c/.h` | 784 | as-is | nothing in these files: flag bit positions used match (section 2); the u64 -> u32 truncation belongs in the bridge marshaller |
| Small glue | `pvr_srv.h`, `pvr_srv_public.h`, `pvr_srv_job_null.c`, `pvr_srv_job_common.h` | 276 | as-is | - |
| Device create | `pvr_srv.c` | 876 | ~95% as-is | version gate `:684-715`, drop `DRM_IOCTL_SRVKM_INIT` `:724`, pre-signaled sync `:798-854` |
| Job contexts | `pvr_srv_job_common.c`, `_compute.c`, `_render.c`, `_transfer.c` | 629 | ~90% as-is | `pvr_srv_create_timeline`; create-context arguments (no `robustness_address` for TQ, no `call_stack_depth` for render); render static state 368 -> 184 B; compute static state layout; reset-framework command 16 B |
| Bridge marshalling | `pvr_srv_bridge.c/.h` | 2766 | 13 of 35 calls unchanged | 22 calls need the 1.13 field order/width/field set (section 2), plus the 3 ID changes; 4 sync helpers move to `/dev/pvr_sync` |
| Command-stream fill | `pvr_arch_srv_job_render.c`, `_compute.c`, `_transfer.c` | 1406 | UNVERIFIED | writes the 12 UM-ONLY FW structs; 1.13 layouts not in the kernel tree |
| FW headers | `fw-api/*.h` | 911 | partly | 6 of 27 structs identical, 9 different, 12 UM-ONLY (section 4) |

Estimate by lines: about **49% reusable as-is** (1609 untouched + ~830 + ~570 in lightly edited files + ~1030 in the 13 unchanged bridge calls = ~4040 of 8197), about **22% needs certain but mechanical rewrites** (22 bridge functions plus the small edits above), about **28% cannot be confirmed from the kernel source** (command-stream code and FW headers: 2317 lines). By risk rather than by lines: everything except the command-stream fill is determinable from the 1.13 source and is mechanical.

Context: 1.11 (`KM111:`) lacks `RGXCREATEHWRTDATASET`, `RGXDESTROYHWRTDATASET` and `SRVCORE_GETMULTICOREINFO` (per the bridge-ID extraction), which is why the img-pvr 1.11 bridge cannot be reused for 1.13 either.

## 7. Unverified items

1. Kernel pointer width (UM and KM must match; no compat shim). Check with the CONNECT `ui8KernelArch` byte or `uname -m` on the phone.
2. 1.13 layouts of `RGXFWIF_CMDTA/CMD3D/CMDTRANSFER/CMD_COMPUTE` and their register blocks, and whether compute/transfer blobs start with a frame-number word. Capture vendor GLES `RGXKICKTA3D2`/`RGXKICKCDM2`/`RGXSUBMITTRANSFER2` calls (sizes first, then bytes). The ALIGNMENTCHECK mismatch log (section 3.4) can cross-check sizes and register offsets.
3. Whether `DevmemFwAllocate(size 0)` for a zero-length framework command succeeds (`rgxfwutils.c:4426-4452`); the safe choice is 16 zero bytes.
4. Whether `CacheOpFenceCheck(completed, 0)` always passes (`cache_km.c:2191-2221` read, check function not read).
5. The phone's `CONFIG_SYNC_FILE` (which sync driver is built), and SELinux access from `untrusted_app` to `/dev/pvr_sync`.
6. The errno the DRM core returns for the nonexistent ioctl nr 5 (DRM core not in the tree).
7. That the binary on the phone was built from this source tree and config (`config_kernel_user_mt6765.h`). The `/proc/pvr/version` option bits 0x10 agree with the tree.
8. 1.17 numeric values of CCB command types (not in the inputs; upstream does not define them).
9. Upstream's BVNC unpack macro against `rgx_bvnc_pack` (only the 1.13 side was read).
10. `sizeof()` of upstream's FW structs: nothing was compiled, sizes quoted for upstream were derived by hand from the field lists.
11. The other 32 bridge IDs and heap-config semantics (names, page sizes) were not re-checked; only struct layouts were compared.
