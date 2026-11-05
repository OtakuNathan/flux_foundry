# FF + Dynabridge CUDA throughput — desktop, 2026-11-05

## Environment and revisions

- RTX 2080 Ti, driver 615.78.08, CUDA toolkit 13.4.92, `sm_75`.
- Ryzen 7 3700X, GCC 16.2.1, Release `-O3 -DNDEBUG`, C++14.
- FF `d1cfad070ed8bf667a0c0aff77977d7741d107fd`; Dynabridge `84725ad350baf56c3cddb69aa934bf9eaabf3123`.
- Desktop compositor and other GUI processes remained active. CPU governor was `powersave`. Clocks and system load were not locked.
- Original upstream CUDA demo: 8/8 tests passed on the device, including injected event-record failures and concrete/erased/no-exception variants. See `LastTest.log`.

## End-to-end comparison

`benchmark.cpp` adapts the current demo's Mandelbrot render kernel and resource/queue strategy into a matched test. Each operation creates a stream/event/device allocation, submits the same kernel, copies its image back to a pageable host vector, polls events, destroys resources, and verifies the image. Every image is checked for dimensions and 257 pixel samples (including alpha); samples must match the first result for that size. Timing includes the sampling. All paths use the same helper and polling queue, and the same number of in-flight operations.

- `native`: custom native C++ callback/queue around the CUDA backend (not a bare CUDA launch; still uses shared ownership and `std::function`).
- `concrete`: schema-generated Dynabridge static invocation, FF `await_static_invocation`, reusable FF Blueprint and a fresh runner per operation.
- `erased`: schema-generated Dynabridge erased invocation through the same FF Blueprint pattern.

Release builds with exceptions enabled. Busy polling; unlike the original demo, there is no 100 us sleep between polls. Window is the maximum number of outstanding operations, not a guarantee they execute concurrently. All paths warm up; five measured samples per combination, with the path order rotated. Each sample is 6,000 images for 128x128 or 1,500 for 512x512. The table is medians in completed images/s, not peak GPU kernel throughput.

| Image | Window | Native | FF + DB concrete | FF + DB erased |
| --- | ---: | ---: | ---: | ---: |
| 128x128 | 1 | 5,918 | 6,091 | 5,871 |
| 128x128 | 8 | 17,282 | 17,708 | 18,114 |
| 512x512 | 1 | 2,716 | 2,687 | 2,706 |
| 512x512 | 8 | 1,485 | 1,465 | 1,459 |

The FF paths being faster in some samples does not establish a speedup; noise and scheduling are visible. The 512x512 window-8 slowdown occurs in the native control as well; this test does not isolate its cause. This is not a pooled/pinned-memory production backend, and no conclusion should be generalized to all GPU workloads or other GPU integration paths.

`results.csv` is a mixed-format raw stream: `RUN` rows have `mode,width,height,window,repeat,seconds,fps,MPixels/s` after their tag. `GPU_ONLY` rows in this file were short preliminary references and are superseded by `kernel-results.csv`. [`summarize.py`](../../summarize.py) parses the actual records and shows the raw range and paired ratios. The initial short-sample pilot was not used in this table and is not included in this snapshot. `telemetry.csv` is one-second system-wide sampling including intervals between trials, not continuous utilisation for any particular trial.

## Separate device-only reference

`kernel_only.cpp` reuses one stream and device buffer and times continuous kernel submissions with CUDA events. Allocation, D2H copy and validation are outside the event interval. Three samples alternate image sizes, each preceded by 100 warm-up launches. The 128x128 measurement uses 30,000 kernels/sample; the 512x512 measurement uses 20,000. Outside the timed interval the entire alpha channel and non-black output are checked.

- 128x128: median 93,351 images/s, range 77,777–96,138.
- 512x512: median 25,864 images/s, range 25,704–25,898.

These are separate device-only references, not equivalent end-to-end workloads or hard theoretical GPU limits. Host submission starvation can still affect event elapsed time. `kernel-results.csv` and `kernel-telemetry.csv` preserve the samples. The latter contains only four one-second observations and should not be treated as per-trial GPU utilisation.

## Rebuild and provenance

The [standalone harness](../../README.md) retains the CPU driver, device-only
reference, build script and copied CUDA helper/kernel/definition sources.
Rebuild with exact FF/Dynabridge revisions as described there; the original
binaries and object files are not published. `SHA256SUMS` fingerprints the
retained measurements, validation log and harness sources.
