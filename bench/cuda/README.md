# Typed CUDA benchmark

This standalone experiment compares a native C++ callback path with FF fast
runners consuming concrete or erased Portal invocations. All three paths
use the same Mandelbrot kernel, per-operation CUDA resources, polling queue,
in-flight bound and sampled image verification. Each FF operation creates a
fresh runner from a reusable Blueprint. Kernel code stays behind a native C++
boundary; NVCC does not compile FF or Portal templates.

The [2026-11-05 RTX 2080 Ti snapshot](results/rtx2080ti-20261105/README.md)
contains the end-to-end table, environment, exact revisions, raw CSV files and
the upstream device-test log. `results.csv` mixes record types; use the supplied
parser rather than treating every line as an image result:

```sh
python3 bench/cuda/summarize.py
python3 bench/cuda/summarize.py --results-dir /path/to/another/result-directory
```

The device-only reference is separate: it reuses a stream and device buffer and
times repeated kernel submissions with CUDA events. It excludes allocation,
D2H and verification. Neither reference is a theoretical GPU limit or evidence
that the end-to-end gap is FF overhead. The end-to-end backend creates and
destroys a stream, event and device allocation per image and uses pageable host
storage; it is not a pooled production renderer. The desktop GUI and power
policy were left active during the recorded trials.

## Where the per-operation cost actually goes (2026-11-18)

Everything above measures one execution model: a fresh stream, event, device
allocation and pageable host buffer for every image. `benchmark_pooled.cpp`
measures what that lifecycle costs. The kernel, window, polling queue and
257-sample verification are unchanged; only ownership moves. Every CUDA resource
belongs to the context, one context per in-flight slot, and each invocation is
dispatched against a fresh context. The blueprint stays static: the context is
the only per-invocation input, so nothing about the pooled path needs FF or
Portal to change.

Medians of five samples, image order rotated, RTX 2080 Ti (driver 620.70.06,
toolkit 13.4.3, `sm_75`). The parenthesised column is the paired deviation from
`native_pool`, i.e. what FF and Portal cost:

| Image | Window | native | native_pool | concrete_pool | erased_pool |
| --- | ---: | ---: | ---: | ---: | ---: |
| 128x128 | 1 | 28,689 | 41,445 | 40,936 (-1.2%) | 41,389 (-0.1%) |
| 128x128 | 8 | 28,502 | 114,743 | 112,529 (-1.9%) | 110,399 (-3.8%) |
| 512x512 | 1 | 4,517 | 6,121 | 6,108 (-0.2%) | 6,071 (-0.8%) |
| 512x512 | 8 | 1,642 | 12,638 | 12,711 (+0.6%) | 12,693 (+0.4%) |

All four modes verified 240,064 (128x128) and 60,064 (512x512) sampled images
with byte-identical pixels, so the pooled path is not trading correctness for
throughput. FF and Portal stay within 0-4% of the pooled native control; the gain
comes from not rebuilding CUDA resources per operation, not from the framework.

Two caveats decide how to read the `native` column. First, per-operation
`cudaMalloc` is cheaper in a process that also holds pooled allocations, because
the driver allocator stays warm: this binary reads 28,689 at 128x128 window 1
where `benchmark.cpp` reads about 6,700 in a process that never pools. Both
figures are real conditions, so the pooled comparison above is the paired one and
the per-operation baseline should be taken from `benchmark.cpp` or from
`resource_probe.cu` with `PROBE_ONLY_NATIVE=1`.

Second, the result type still copies every image out of the pinned buffer into a
fresh `std::vector`. `resource_probe.cu` measures that materialisation on its own
(`PROBE_COPY=1`): it costs a further 5-27% depending on image size, and the
pooled native path pays it too. It belongs to this demo's by-value result type,
not to FF.

`resource_probe.cu` isolates the same levers with no FF involved: per-operation
resources, pooled slots, a captured kernel+D2H graph, and a graph replayed with
per-operation parameter updates. On the same hardware, pooling accounts for
6-9x at window 8, while graph replay adds only 5-9% at 128x128 and nothing at
512x512 — there the 1 MiB device-to-host transfer over PCIe 3.0 x16 caps every
mode near 12,700 images/s. Run-to-run spread at 128x128 window 8 is roughly
+/-10%, so single-digit differences between modes are noise.

## Reproduce

The current sources use Portal's renamed API. Use the FF and Portal revisions
checked by `build.sh` for a new run. CUDA helper sources are unchanged; the
build still targets `sm_75` and uses NVCC's `--allow-unsupported-compiler`
for the recorded GCC toolchain.

To reproduce the historical snapshot exactly, check out this benchmark's
original version from FF commit `98d73c280667226f0fbb6f4408626ab875e2bd2a`.
That version's script pins FF `d1cfad070ed8bf667a0c0aff77977d7741d107fd`
and the pre-rename bridge `84725ad350baf56c3cddb69aa934bf9eaabf3123`.
The historical CSV and logs retain their original names and revisions.

From this directory:

```sh
FF=/path/to/pinned/flux_foundry PORTAL=/path/to/pinned/portal \
  CUDA=/path/to/cuda bash build.sh
./benchmark-rebuilt > new-results.csv
./benchmark-pooled-rebuilt > new-pooled-results.csv
./kernel_only-rebuilt > new-kernel-results.csv
```

`build.sh` asserts the pinned FF and Portal revisions; set `FF_ALLOW_UNPINNED=1`
to build against another checkout, which is what a pooled run needs because the
pins describe the historical snapshot. `benchmark_pooled.cpp` and
`resource_probe.cu` do not need the pin at all:

```sh
$CUDA/bin/nvcc -std=c++14 -O3 -DNDEBUG --allow-unsupported-compiler -arch=sm_75 \
  -I$CUDA/include -c cuda_typed_device.cu -o cuda_typed_device.o
c++ -std=c++14 -O3 -DNDEBUG -I$FF -I. -I$PORTAL -I$CUDA/include \
  benchmark_pooled.cpp cuda_typed_device.o -L$CUDA/lib64 \
  -Wl,-rpath,$CUDA/lib64 -lcudart -pthread -o benchmark-pooled-rebuilt
$CUDA/bin/nvcc -std=c++14 -O3 -DNDEBUG --allow-unsupported-compiler -arch=sm_75 \
  -I. -I$CUDA/include resource_probe.cu -o resource-probe
```

For a new result directory, name the two raw outputs `results.csv` and
`kernel-results.csv`. If system-wide telemetry is collected, retain it as
`telemetry.csv`; its one-second observations span trial boundaries and do not
establish per-trial utilization. Rebuilds do not overwrite the historical CSV.

The eight passing tests in the retained `LastTest.log` are the upstream typed
CUDA demo's device tests, including event-record failure handling and the two
no-exception policies. Throughput trials themselves use exceptions enabled;
the test log does not imply that no-exception throughput was measured.
