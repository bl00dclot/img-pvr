# pco-stats: host compiler statistics for PCO

Other Mesa drivers judge a compiler change with shader-db or fossil-db: compile a large set of shaders before and
after, compare instruction counts, registers and spills. PowerVR has no `drm-shim`, so this is a host tool: it links
the PCO compiler, takes SPIR-V, runs it through the same steps as `pvr_graphics_pipeline_compile()` for the GE8320
(BVNC 22.87.104.18, the driver's own device info) and prints one line per shader.

## Use

```sh
# 1. build the tool (host build dir, build image; serialise builds when several jobs share the machine)
flock <repo>/.locks/build docker run --rm --user $(id -u):$(id -g) -e HOME=/tmp -v $PWD:/src img-pvr-build:trixie \
    bash -c 'ninja -C build-host src/imagination/pco/pco_stats'      # build-host from ./build.sh stage 1 (or its meson setup line)
# 2. compile the corpus and collect stats
tools/pco-stats/run.sh [-b build-host] [-p <probe shaders dir> -g <glslc>] [-c <extra corpus dir>] before.txt
# ... change the compiler, rebuild pco_stats ...
tools/pco-stats/run.sh after.txt
# 3. compare
tools/pco-stats/compare.py before.txt after.txt [-s igrps,temps,spills,code_size,smp] [-a]
```

`pco_stats` itself: `pco_stats [-f rgba8|rgba16f|rgba32f] [-b] [-n rts] [-r] [-i] [-d] <name> <vs.spv|-> <fs.spv>`, two lines out
(`vs`, `fs`), `-d` prints the grouped IR to stderr, `-n` is the number of identical colour attachments (1 to 4, outputs
at registers 0, 1, ...; `-b` blends all of them). Needs the vertex shader too, because `pco_link_nir`
changes the fragment shader's inputs.

Result line, keys are what `PCO_DEBUG_PRINT=stats` prints plus the sample counts from the grouped IR:

```
bench/td fs code_size=168 igrps=17 main=11 bitwise=0 control=6 temps=8 vtxins=0 spills=0 scratch=0 loops=0 smp=2 smp_pplod=0 smp_f16=0 smp_raw=0 smp_bias=0 smp_grad=0 smp_replace=0 itrsmp=0 itr=1
```

`smp*` count `smp` instructions and their modifiers (`pplod`, `f16`, raw return = `rawdata`/`both`, `bias`, `gradients`,
`replace`), `itrsmp` the fused iterate-and-sample (zero until the compiler emits it), `itr` the `fitr*`/`ditr*` iterators.
`compare.py` prints per-shader changes, totals and helped/hurt counts for the judged stats (igrps, temps, spills,
scratch, loops, code_size).

## What it models

One fixed pipeline state, the one `tools/vktest/vkbench.c` and the probe use: render pass path, one
`R8G8B8A8_UNORM` colour attachment in output register 0 (`-f` for the 16/32-bit float formats), one sample, no dynamic
state, no sample mask or colour-write-enable state (so the fs meta word is static, as in the driver), blending off or
`-b` for SRC_ALPHA / ONE_MINUS_SRC_ALPHA on colour and alpha, `-r` robustBufferAccess. The descriptor layout is derived
from the shaders' variables (combined sampler = image + sampler descriptor, no dynamic buffers, push constants as a
full 128 byte range). SPIR-V to NIR uses the runtime's `vk_spirv_to_nir` steps without a `vk_device` and with the
Vulkan 1.0 core capabilities.

Not modelled: input attachments (the probe's `t3sub` is skipped), tile buffers and attachments of differing formats (`-n` gives N identical ones), multisampling,
dynamic state, multiview, ycbcr, transform feedback, compute (`c1.comp`), other stages. A shader that needs
one of these either fails loudly or compiles with different pipeline state than the device would use; that is the first
thing to look at when a host line disagrees with the device.

The allocation helpers in `pco_stats.c` are copied from `src/imagination/vulkan/pvr_arch_pipeline.c` (marked); if the
driver changes them, update the copy. The device check below is what notices drift.

## Extra corpus (`run.sh -c DIR`)

`-c DIR` runs `DIR/corpus.txt` after the built-in `corpus.txt`; the same line format, and `extra:name` names `DIR/spv/name.spv`
(SPIR-V built by whoever owns the directory, e.g. from an application's own shaders that cannot live in this repository).
A pipeline that fails (pco_stats exits non-zero) is reported as `FAIL name` on stderr and left out of the result file.
`ASAN_OPTIONS`, `UBSAN_OPTIONS` and `NIR_DEBUG` of the caller are passed through, so a sanitizer/validation build
(`meson setup build-dbg ... -Dbuildtype=debug -Db_sanitize=address,undefined -Db_lundef=false`, the host-tools options of
build.sh stage 1, then `ninja -C build-dbg src/imagination/pco/pco_stats`) runs the same corpus with `-b build-dbg`.
Vertex inputs are modelled as 32 bits per component of the declared type (float, uint or int, 1 to 4 components).

## Corpus

`corpus.txt`: the vkbench shaders (`tools/vktest/bench/*.frag` with `vert.vert`, SPIR-V from `glslangValidator -V` as in
`tools/vktest/bench/gen.sh`) and the probe's (the probe app's `shaders/` directory, kept outside this repository, compiled with
`glslc -O` as its CMake does): `alu alum c1 c2 disc fetch fill blend td tf tv tvm`, probe `t1` (blended), `t1nb`, `t2`,
`t3sep`. No SPIR-V is committed; `run.sh` generates it into a temporary directory. Add the port's shaders as lines of
`corpus.txt` when they exist.

## Checking against the device

`phone2stats.py name < log` converts the device log of one pipeline (`PCO_DEBUG_PRINT=vs,fs,stats PCO_COLOR=off
vkbench <driver.so> <mode> 1`; the `stats` flag alone prints nothing without `vs`/`fs`) into the same line format, then
`compare.py device.txt host.txt -s igrps,main,bitwise,control,temps,vtxins,spills,scratch,loops,code_size,smp,...` must report 0
changed. 15 of the 16 corpus pipelines (all but `probe/t1nb`) matched on the device when the tool was added (`results/device-bf89cfc.txt`).

## Reference results

`results/device-bf89cfc.txt` is the same corpus as the device reports it (vkbench modes and the probe's t1, t2, t3sep
under `PCO_DEBUG_PRINT=vs,fs,stats`, converted with `phone2stats.py`); `results/bf89cfc.txt` and `results/d1913f6.txt` are the corpus stats of those two commits (host tool built from each);
`compare.py results/d1913f6.txt results/bf89cfc.txt` is the before/after of the three S0 compiler fixes. Static
counts do not see loop trip counts: the ALU shader's loop is unrolled by the second fix, so its static instruction
groups go from 43 to 203 while the executed ones fall.
