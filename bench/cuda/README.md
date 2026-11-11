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
./kernel_only-rebuilt > new-kernel-results.csv
```

For a new result directory, name the two raw outputs `results.csv` and
`kernel-results.csv`. If system-wide telemetry is collected, retain it as
`telemetry.csv`; its one-second observations span trial boundaries and do not
establish per-trial utilization. Rebuilds do not overwrite the historical CSV.

The eight passing tests in the retained `LastTest.log` are the upstream typed
CUDA demo's device tests, including event-record failure handling and the two
no-exception policies. Throughput trials themselves use exceptions enabled;
the test log does not imply that no-exception throughput was measured.
