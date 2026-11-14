# Frame-time benchmarking

`frame_time_metrics.py` computes throughput from interval count divided by total elapsed time. It reports interpolated median, p95 and p99, maximum, the share of intervals at or below 1000/60 ms, and strict counts above 33.3, 50 and 100 ms. Percentiles describe the recorded event stream. They are not display latency measurements.

Every report requires `--domain`. Label guest frame submission, host presentation and physical display scanout separately. Disable frame generation when assessing real rendering throughput. A frame-generation counter or a stationary scene cannot establish sustained gameplay performance.

For complete JSON interval arrays, supply `intervals_ms` and an explicit interval count in `interval_count`, `frames`, or `summary.intervals`. All supplied counts must match. The debug server's historical `frame_times` response includes only the last 120 intervals even when it summarizes a longer window: this tool rejects that mismatch rather than calculating full-window percentiles from the tail. A `truncated` timing window is also rejected.

For a tab-separated recorder file with `frame` and `interval_ms` columns, select both bounds to exclude menus and loading and detect missing endpoints. The ending frame is exclusive. Comment lines beginning with `#` are supported.

Native present-call traces in `.csv` files may instead supply `frame` and `interval_ns`; nanoseconds are converted to milliseconds. Exclude the first record when it has no preceding interval. These return timestamps do not measure GPU completion or scanout.

An `--output` path that identifies the input file is rejected, including relative/absolute aliases, Windows filename case aliases and hard links. Keep the raw capture separate from its derived report.

```powershell
python tools/nx2/frame_time_metrics.py gameplay.tsv --domain "game present, frame generation off" --first-frame 3000 --end-frame 4200 --output run-1.json
python -m unittest discover -s tools/nx2 -p test_frame_time_metrics.py
```

Use an isolated profile copied from a hash-verified save checkpoint. Record executable/source identities, actual game build, effective runtime settings, resolution, aspect ratio and input script. Separate first visits from warmed runs. Repeat the same warmed route at least three times for each final candidate, resetting to the same checkpoint and camera pose. Include movement, camera rotation, jumping, water and effects where the save permits them. Record which sequences were visibly verified and reject dialogue or loading windows.

A copied portable `user` folder can retain absolute storage paths in `qt-config.ini`. Rebase the NAND, SDMC, load, dump, TAS and screenshot paths into the copied profile before launch. Then verify the runtime log's `NANDDir`, `SaveDir`, `LoadDir` and `CacheDir`; the executable path and the location of its INI do not establish save isolation. If gameplay has already written through an old path, preserve and hash the newer saves before proceeding. Restoring the initial backup over newer user progress is not rollback.

Run only one game runtime at a time. Stop builds and shader-preparation jobs before collecting gameplay measurements. Check for surviving child processes, available physical memory and commit headroom. A background preparation job can contaminate timings after its first worker exits by starting another preparation stage.

Collect thread CPU time as a percentage of one logical processor; total process CPU can conceal a saturated thread. Record GPU execution time separately from GPU utilization, and capture memory, clocks and pipeline/upload/wait events where available. Do not infer a causal optimization benefit when routes, cache warmth, measurement domains or timing methods differ. Report unavailable counters explicitly.

Verify populated internal scene color and depth targets after gameplay has settled. Window size, final output size, a resolution percentage and an enabled fixed-resolution variable can all disagree with the scene's actual dimensions. Inspect the complete frame-dump manifest or render-pass capture, including the passes containing scene geometry; the first clear target or a limited debug-response preview is insufficient. Keep capture overhead outside timed comparison windows.

For example, a local Ocarina 1.1.1 evaluation on 2026-11-08 observed Hookshot 2.0.3 at 2560x1440 output with fixed resolution enabled and scale 1.0, while its populated scene color/depth targets were 1536x864 and later composition was 2048x1152. Three warmed active routes pooled to 52.39 native game presents/s, but that did not establish matched-quality improvement over the reference's captured 2560x1440 scene or sustained real 60 FPS. Increasing the scale and enabling the game's dynamic-resolution path did not establish the required scene dimensions in those tests. Such results belong in an explicitly unmatched-quality row, not a successful replacement recommendation.

A later local Hookshot v2.0.4 integration of the public fixed-scale/base-scale proposals verified main color/depth, later composition and output at 2560x1440. Three warmed 1200-interval movement routes measured 45.75, 44.31 and 44.65 native present-call returns/s, pooling to 44.89. The pooled p95/p99 were 28.40/32.32 ms, maximum 423.68 ms, and only 5.53% of intervals met 16.67 ms. Correcting scene dimensions did not establish the performance target or complete visual equivalence. The reference and native capture points remain different, so this is not a same-instrument causal speedup claim.

## Physical input during automated routes

Keep normal launchers free of inherited `HOS_INPUT_*` test variables. An exclusive-input benchmark launch can deliberately suppress a correctly connected physical controller; that launch policy must not become the user's normal play setup. Controller recognition in a log alone does not prove that input routing allows it through.

An instrumented native producer may supply `input_interference_count` and `input_audit_enabled` beside each present timestamp. These columns are an evaluation extension, not a feature claimed for the stock Hookshot v2.0.4 release. The opt-in producer observes manual input before merging it with the script, so a physical button identical to the script is still detected. Direct native mouse camera mode must be disabled or conservatively excluded because its camera writes bypass Npad input.

`native_input_audit.validate_input_window()` requires every selected frame plus its preceding counter boundary, enabled auditing throughout and nondecreasing counters. Any counter increase excludes the window. Activity before or after those boundaries does not alter its timing statistics; route reset/starting-state validation remains a separate requirement. The route reader may use a wider quiet guard including the prelude while retaining only gameplay intervals for FPS.

Native CSV inputs with either audit column automatically undergo this validation before `read_intervals()` returns comparison samples. JSON with `valid_for_comparison: false` is rejected as excluded evidence. Keep excluded raw records for diagnosis; do not remove their audit metadata to turn them into successful comparisons. Legacy files without audit columns remain readable and do not acquire a claim of verified input isolation.

```powershell
python -m unittest discover -s tools/nx2 -p test_native_input_audit.py
python -m unittest discover -s tools/nx2 -p test_frame_time_metrics.py
```
