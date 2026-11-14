# nx2-precompile

Builds Vulkan pipelines for a Switch 2 game's shaders before they are first drawn, so a new area does
not stall while the driver compiles them.

## How it works

1. `nx2-precompile extract` finds the game's shader programs in its own files, by content
   (`core/file_sys/nx2_shader_extract`): every RomFS file of the `.nup` (base and update, as the game
   sees them) is read, zstd (with the game's dictionaries) and Yaz0 are undone, SARC archives are
   expanded, and BNSH (shader archives, effect bundles, layouts), agl BAHS and NDSR packs, nn::ndve
   movie files and sead's primitive drawer are parsed. For Ocarina of Time it finds, in about 30
   seconds, every program nxnative's build of the game holds (42,848 of 42,854; the others are
   entries nxnative's current `shader_split.py` drops too) and every shader recorded sessions drew,
   and its variation tables reproduce nxnative's vertex/pixel pair tables. The cutting of programs
   and the fingerprints follow nxnative's `shader_split.py` (github.com/seedds/nxnative, MIT).
   `nx2-precompile scan` then captures and lowers each program as `GenericEnvironment::LowerNativeCode` does, and computes the shader hash
   the emulator will compute at runtime. It then translates each program against a static fact oracle
   and writes the facts the translator asked for, in `GenericEnvironment::Serialize` layout.
2. The Python tools in `py/` predict the render states each shader combination is drawn with, from
   the pipelines the recorded caches hold, and write the game's precompiled file (`NX2PREP1`).
   Vertex/pixel pairs and mesh, task and pixel combinations come from the variations the extractor
   found (`game_tables.py`; older versions of a shader archive that the game ships but does not load
   are left out), with their mesh state
   (`SET_MESH_SHADER_A/B`, `SET_MESH_INIT_SHADER` and the pass-through geometry header) derived from
   the mesh program's control section by `nx2-precompile mesh-envs`.
3. At boot, after the shader cache's boot builds, `PipelineCache::PreparePrecompiled` builds every
   pipeline of that file once and drops it, as jobs of the shader job scheduler. The driver keeps
   the compiled code in its own cache, and Lichii keeps each pipeline's shader infos and SPIR-V
   (`precompiled_kept.fork-v<N>.bin` and its index), as each one finishes. The setting "Prepare
   precompiled shaders" (`precompiled_prepare`) picks when: **While playing** (default) starts the
   game at once and prepares on a few shader jobs at a time behind the game's own (a quarter of the
   CPU threads, 1 to 4; `EDEN_NX2_PRECOMPILED_THREADS=<n>`), compute programs first, then each
   vertex/pixel pair's most likely render state before its others, and the pipelines the disk
   cache already holds last; each pipeline becomes drawable from kept infos as soon as it is done.
   **Before the game starts** prepares on the loading screen with every CPU thread; its Skip
   button continues in the background. Either way a later launch continues where one stopped
   (closed, skipped or crashed). Ocarina of Time on an RX 9070 XT (RADV, Ryzen 7 9800X3D): about
   21 pipelines a second per job with a cold driver cache (~26 min for 133,822 with 4 jobs), about
   1,000 a second with 4 jobs when Mesa's cache already holds them (a new emulator build), 50 s on
   the loading screen then.
4. When a draw first needs a pipeline the file predicted, the GPU thread checks that the game's state
   answers the recorded environments' questions as they were answered
   (`FileEnvironment::AnswersHoldFor`: a constant buffer value only used as a texture handle stands
   for the type and the pixel format traits of the texture it names, a storage buffer pointer word for
   its classification, any other value for itself). Then the translation of the recorded environments
   is the one the game's state gives, so the pipeline is created with the kept infos and a worker
   builds it from the kept SPIR-V, which the driver finds compiled; the draw waits for it on the
   scheduler thread, not on the GPU thread. A pipeline that is not predicted, or whose answers
   differ, is translated as before, while the GPU thread waits. In Ocarina of Time this took the GPU
   thread's time for the 628 pipelines of entering an area from 2.5 s to 0.25 s; translated live
   with `EDEN_NX2_PRECOMPILED_AUDIT=1`, all 1,719 of two sessions gave the kept SPIR-V and infos.

A native compute program's translation leaves out the accesses through a pointer slot that is null
at the time, and the program is translated again once the game fills that slot
(`PipelineCache::StaleNullPointerSlot`). Which slots are filled depends on the area: in Ocarina of
Time, the foliage culling programs. Each such state is SPIR-V the driver compiles while the game
waits, about 4 seconds for those programs. A state's translation depends only on its storage pointer
answers, so the precompiled file holds every compute program in each pointer state the recorded
caches show it in, and in the states a rebuild reaches from those (`build_precompiled.py`,
`compute_states`): each combination of the slot groups the program left null that the recordings
show filled elsewhere (slots filled together are one group), and the unions of those states. Learned
from the original three caches and one more session, the file held all six states (their SPIR-V)
that a held-out session translated those programs in, three of them rebuilds that had stalled for
about 4 seconds each, with 20 compute pipelines more.

The oracle never decides what a draw runs: a wrong guess only means the driver compiles that pipeline
when it is first drawn, as it does without this tool.

Native keys are canonical (`FixedPipelineState::CanonicalizeNative`): attribute types of inputs the
vertex program does not read, blend factors of attachments without blending, attachments past the
last colour target, and the depth bounds are cleared. None of them changes the pipeline, and the
precompiled keys can only match draws if both sides clear them. The attribute mask comes from the
vertex program's header; a program found to load an attribute its header does not list is logged,
its pipeline is dropped once, and its keys keep every attribute type from then on.

The preparation step needs a driver that keeps its own pipeline cache (NVIDIA's and AMD's
proprietary drivers, and RADV while Mesa's shader cache is on, as `Device::DriverCachesPipelines`
says); elsewhere it runs only with `EDEN_NX2_PRECOMPILED=1`.
Records carry the driver of the machine that recorded them; they are prepared with this machine's,
as draws key them, so a driver update only means preparing again.

The driver's cache must be large enough to hold the whole file. **On NVIDIA, set Shader Cache Size to
Unlimited** (NVIDIA Control Panel, Manage 3D settings). With "Driver default" the driver kept about
1 GiB for the emulator: of the 132,312 pipelines prepared for Ocarina of Time it stored the first
~33,000 and silently dropped the rest, so a new area compiled almost everything again. Unlimited
held all of them (about 2.6 GB on an RTX 3080). The preparation step cannot detect this; a session's
log shows it as prepared pipelines that still take hundreds of milliseconds to build.

## Commands

```
nx2-precompile build    --game GAME.nup|ROMFS_DIR --out precompiled.fork-v<N>.nx2prep --cache CACHE [--cache CACHE ...]
nx2-precompile extract  --game GAME.nup|ROMFS_DIR --out PREFIX [--packages merged|union] [--threads N]
nx2-precompile scan     --pkg PREFIX.programs --out PREFIX [--priors FILE] [--threads N]
nx2-precompile validate --pkg PREFIX.programs --cache CACHE.bin --out PREFIX [--priors FILE]
nx2-precompile cache-export --cache lichii-cache.v2.bin --out CACHE.bin
nx2-precompile mesh-envs --pkg PREFIX.programs --cache DRAWS.tsv --out PREFIX [--priors FILE]
nx2-precompile check    --file precompiled.fork-v<N>.nx2prep
nx2-precompile cache-version
```

`build` is `run_pipeline.sh` up to the precompiled file (extract, priors, scan, game tables, mesh
draws and environments, `build_precompiled.py` with `--k 4 --mesh-k 2`) in one process, without
Python: the Python steps are ported to C++ in `video_core/nx2_precompile_build.cpp`, and the file
is the scripts' byte for byte (checked on Ocarina of Time with one cache and with two, one of them
of the format before the shader cache v2). It leaves out the checks (`validate`, the held-out
check, `check`, the SPIR-V emit check). Ocarina of Time: 37 s on 16 threads, against
about 50 s for the script.

## In the emulator

The same pipeline runs in Lichii from the player's own play (`VideoCommon::Nx2Precompile::Build`,
the tool's commands live in `video_core/nx2_precompile.cpp`):

- The game list's "Build Precompiled Shaders from My Play..." reads the game's files and the
  title's `lichii-cache.v2.bin`, and writes `precompiled.fork-v<N>.nx2prep` next to it, with a
  progress window. A precompiled file from elsewhere (the tool) is replaced only after asking.
- At launch, a Switch 2 game (a NUP) whose cache has recorded 1,000 pipelines or more and has no
  precompiled file gets the offer to build one before it starts (Build now, Not now, Don't ask for
  this game), and again once the cache has grown by a quarter (at least 500) since the last build,
  or by 1,000 since the player said not now. `precompiled_from_play.fork-v<N>.txt` in the title's
  shader folder remembers which (`built N`, `declined N`, `never`). A precompiled file without
  that note (one the tool made) is left alone.

The precompiled file is then prepared as any other (see step 3 above). Its shader code comes from
the player's own copy of the game, so it is never shared.

The extractor used to give its programs and variations in whatever order its threads found them,
so two runs listed the copies of a shared program in different orders, and the predictions that
depend on which copy comes first differed. It now puts them in the order they are first found in
file order (the origin rule already was), so the same game and caches always give the same file.

## Making a precompiled file with the tool

The emulator does this itself (see above). The tool's chain is the same, with the checks it adds,
for working on the predictions:

1. Build Lichii and the tool with `-DENABLE_DEBUG_TOOLS=ON` (targets `yuzu` and `nx2-precompile`).
   Check `nx2-precompile cache-version` against the Lichii build that will read the file; they must
   match.
   Install Python 3.11 or newer (no packages needed).
2. Play the game in Lichii for a while, with the NVIDIA cache size set as above. Every pipeline the
   game builds is recorded in the title's shader cache, `cache/shader/<title id>/lichii-cache.v2.bin`
   in Lichii's user folder (`vulkan.fork-v<N>.bin` before the shader cache v2). These recorded pipelines are what the tools learn render states and
   storage-pointer facts from, so the more varied the play (areas, menus, cutscenes, effects), the
   better the predictions for the rest of the game. One long session works; caches from several
   sessions or players, in different folders, work better. Copy each cache out before the next
   session if you want to keep them apart.
3. Run the pipeline from the game's `.nup` (or an unpacked RomFS folder) and the caches, for example
   in Git Bash on Windows:

   ```
   NX2_GAME="D:/Games/Game [title][v131072].nup" NX2_OUT=out NX2_TOOL=path/to/nx2-precompile.exe \
       bash tools/nx2-precompile/run_pipeline.sh session1/lichii-cache.v2.bin session2/lichii-cache.v2.bin
   ```

   A shader cache v2 log is converted first (`nx2-precompile cache-export`): its graphics and
   compute records are the older cache format's records byte for byte, which the other commands
   and the Python scripts read. Caches of the older format are read as they are.

   For an unpacked game, `NX2_GAME` is its `romfs` folder.

   About 8 minutes for Ocarina of Time (`nx2-precompile build` writes the same file in under a
   minute, without the checks). It writes `out/precompiled.fork-v<N>.nx2prep`.
4. Optionally test it: start from an empty shader folder, copy the file into the title's shader
   folder, boot, play somewhere new, then run `analyze_session.sh` on that session's cache and log
   to see what still compiled at runtime.
5. The file holds the game's shader code, lowered: it is for the copy of the game it was made from
   and is not to be shared. A new game update or a Lichii build with another cache version needs
   it built again. For a new cache version, the caches recorded under older ones can still be
   learned from (the Ocarina of Time file for v54 was built from v52 and v53 caches); only the
   precompiled file must be built by a tool of the new version. The scripts read every cache in
   v53's layout, whose task and mesh environments have fields an older layout lacks: a cache whose
   task or mesh records lack them must be converted first.

`run_pipeline.sh CACHE.bin...` runs the whole chain from the game (`NX2_GAME`, its `.nup`) and the
disk caches recorded while playing it; see the script's header for the other variables. It needs
Python 3.11 or newer and nothing else; no keys are needed for a `.nup`. `extract` writes
PREFIX.programs (the programs, for the other commands' `--pkg`), PREFIX.tsv (fingerprint, stage and
origin of each), PREFIX.variations.tsv (the programs each variation draws together) and
PREFIX.errors.txt. `--pkg` also takes a folder of nxnative's `BTCD` packages.

`analyze_session.sh` sorts the pipelines a play session still built at runtime by cause: prepared, state not predicted, mesh, shader not in the game files. A shader cache v2 log is converted with `cache-export` first; its first new record counts the records of that export.

`py/learn_priors.py` learns the storage-pointer classifications the oracle uses from the recorded
caches.

`cache-version` prints the pipeline cache version the tool was built with; the precompiled file
carries it in its name and header, and Lichii reads only a file of its own version, so a build
that changes the version needs the file built again (the log names a file of another version).

`validate` compares, for every environment a disk cache recorded, the SPIR-V from the recorded
facts with the SPIR-V from the oracle's facts, and names the fact class behind each difference.
`check` replays every environment of a precompiled file the way `FileEnvironment` does and checks
that each record's key names the code of its environments; with `NX2_CHECK_EMIT=1` it also emits
SPIR-V, and `py/emit_check.py` runs it one environment at a time so a compiler abort is listed
instead of ending the run.

`--priors` is the JSON file `py/learn_priors.py` writes: storage-pointer classifications
(`storage_pointer_direct`, `storage_pointer_indirect`) by stage, constant buffer and offset.

## Files

`PREFIX.envs` (scan): `NXPENV01`, then per program u32 ordinal, u64 shader hash, u32 size and the
serialized environment.

`PREFIX.mesh` (mesh-envs): `NXPMESH1`, then per draw u32 line, u64 mesh state hash, u64 task and
mesh shader hashes, and the sized task (possibly empty) and mesh environments.

`precompiled.fork-v<N>.nx2prep`, in the game's shader directory next to `lichii-cache.v2.bin`:
`NX2PREP1`, u32 cache version, u32 environment count, u32 record count, u32 reserved; per environment
u64 offset and u64 size; per record u8 kind (0 graphics, 1 compute), u8 environment count, u16
reserved, u32 environment indices in stage order, then the 1056-byte graphics or 24-byte compute key.
Environments are stored once and shared by the records that use them.

`precompiled_kept.fork-v<N>.bin` and `precompiled_kept_index.fork-v<N>.bin`, written by the
preparation step next to it (`video_core/precompiled_store.h`). The data file: `NX2PDAT1`, u32 stamp
size and the stamp (the file, driver, build and shader settings it was written for), then the blobs
(a `Shader::SerializeInfo` info, or one zstd frame of a stage's SPIR-V words). The index:
`NX2PIDX1`, u32 version 1, u32 the precompiled file's record count, u32 stamp size and the stamp,
then entries appended as the step runs: a blob (u8 1, u8 kind 0 info / 1 SPIR-V, u16 0, u32 size,
u64 offset, u64 CityHash64 of the info's bytes or the SPIR-V's words), a prepared record (u8 2, u8
environment count, u8 flags 1 compute / 2 failed, u8 0, u32 record index, u64 key hash, u32
environments[5], u32 info blobs[5], u32 SPIR-V blobs[5], ~0 for none), and the start (u8 3) or
cancel (u8 4) of a record (3 bytes 0, u32 record index). A record's entry is written after the data
it names is flushed; an open drops a cut-off tail and anything after an entry naming data the file
lacks. A record two runs started and never finished or cancelled (crashes) is noted as failed and
not prepared again. Another stamp starts both files again. Ocarina of Time: 730 MiB and 12 MiB.
The stamp holds the build's `scm_rev`: a build that changes the translator without a new revision
needs `EDEN_NX2_PRECOMPILED_FORCE=1`, or its kept SPIR-V is the old one's. Infos read for a draw are
checked against their hash. Files of the format before (`precompiled_infos`, `precompiled_prepared`,
`precompiled_progress`) are deleted.

## Switches

- `EDEN_NX2_PRECOMPILED=0`: skip the preparation step; `1`: run it also on drivers without their
  own pipeline cache.
- `EDEN_NX2_PRECOMPILED_FORCE=1`: prepare again from the first record although the kept files say
  the file was (partly) prepared for this driver and build.
- `EDEN_NX2_PRECOMPILED_THREADS=<n>`: shader jobs the preparation keeps in flight while the game
  runs (default a quarter of the CPU threads, 1 to 4).
- `EDEN_NX2_PIPELINE_LOG_LIMIT=<n>`: how many runtime pipeline builds over 2 ms are logged (default
  400; builds over 100 ms are always logged).
- `EDEN_NX2_PRECOMPILED_FAST=0`: translate precompiled pipelines on the GPU thread like the others
  (the kept infos and SPIR-V unused), to compare.
- `EDEN_NX2_PRECOMPILED_VERIFY=1`: a worker translates the recorded environments instead of using the
  kept SPIR-V, and fails the build when the infos differ from the kept ones.
- `EDEN_NX2_PRECOMPILED_AUDIT=1`: also translate each precompiled pipeline from the game's state on
  the GPU thread and compare its SPIR-V and infos with the kept ones (slow; counted in the log every
  10 s).
