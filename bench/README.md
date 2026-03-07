# Microbenchmark Snapshots

This document keeps the microbenchmark material out of the root `README`.

For the standalone `flux_foundry` vs Asio harness and collection scripts, see [`bench/horizontal_compare/README.md`](horizontal_compare/README.md).

## 📊 Benchmark snapshots
Measured on `2026-03-07` (local machine):

- OS: `Darwin 24.3.0` (`arm64`)
- CPU: `Apple M1 Max` (`10` cores)
- Memory: `64 GB`
- Compiler: `clang++/c++ -std=c++14 -O3 -DNDEBUG`

Commands:

```bash
# flow perf (exception / no-exception)
c++ -std=c++14 -O3 -DNDEBUG -I. test/flow_perf.cpp -o /tmp/flow_perf_exc -pthread
c++ -std=c++14 -O3 -DNDEBUG -fno-exceptions -DFLUX_FOUNDRY_NO_EXCEPTION_STRICT=1 -fno-rtti -march=native -fstrict-aliasing -I. test/flow_perf_noexcept.cpp -o /tmp/flow_perf_noexc -pthread
/tmp/flow_perf_exc
/tmp/flow_perf_noexc

# flux vs asio horizontal compare
bash bench/horizontal_compare/run.sh
bash bench/horizontal_compare/run_noexcept.sh
```

### Flow perf matrix (`ns/op`, median of 3 rounds)

| Case | Exceptions ON | Exceptions OFF |
|---|---:|---:|
| `direct.loop20` | 5.56 | 5.49 |
| `runner.sync.20nodes` | 32.28 | 26.62 |
| `fast_runner.sync.20nodes` | 5.51 | 5.12 |
| `runner.awaitable.async4` | 324.53 | 288.54 |
| `fast_runner.awaitable.async4` | 122.40 | 125.35 |
| `runner.fast_awaitable.async4` | 173.07 | 134.65 |
| `fast_runner.fast_awaitable.async4` | 87.02 | 77.77 |
| `runner.awaitable.async1` | 92.10 | 78.51 |
| `fast_runner.awaitable.async1` | 32.23 | 32.71 |
| `runner.fast_awaitable.async1` | 45.28 | 34.40 |
| `fast_runner.fast_awaitable.async1` | 23.49 | 16.73 |
| `runner.when_all.2` | 155.28 | 138.79 |
| `fast_runner.when_all_fast.2` | 92.78 | 79.39 |
| `runner.when_all_fast.2` | 113.80 | 93.95 |
| `runner.when_any.2` | 141.46 | 120.33 |
| `fast_runner.when_any_fast.2` | 74.96 | 64.95 |
| `runner.when_any_fast.2` | 91.12 | 79.80 |

### Horizontal compare (exceptions ON, core matrix, `ns/op`)

| Case | ns/op |
|---|---:|
| `baseline.direct.loop20` | 5.39 |
| `sched.flux.via20.dispatch` | 221.83 |
| `sched.asio.dispatch20` | 137.18 |
| `sched.flux.via20.post` | 210.87 |
| `sched.asio.post20` | 548.29 |
| `full.flux.runner.async4.post` | 597.28 |
| `full.flux.fast_runner.async4.post` | 401.51 |
| `full.flux.runner.fast_awaitable.async4.post` | 474.11 |
| `full.flux.fast_runner.fast_awaitable.async4.post` | 406.96 |
| `full.asio.post.async4` | 145.72 |
| `full.flux.runner.when_all2.post` | 242.65 |
| `full.flux.fast_runner.when_all2.post` | 187.81 |
| `full.flux.fast_runner.when_all2.fastagg.post` | 181.66 |
| `full.asio.post.when_all2` | 138.96 |
| `full.flux.runner.when_any2.post` | 235.94 |
| `full.flux.fast_runner.when_any2.post` | 191.82 |
| `full.flux.fast_runner.when_any2.fastagg.post` | 168.12 |
| `full.asio.post.when_any2` | 136.17 |

### Horizontal compare (strict no-exception, expanded matrix excerpt, `ns/op`)

| Case | ns/op |
|---|---:|
| `baseline.direct.loop20` | 6.33 |
| `sched.flux.native.via20.inline` | 78.23 |
| `sched.flux.native.via20.dispatch` | 1332.06 |
| `sched.flux.on_asio.via20.dispatch` | 205.30 |
| `sched.asio.raw.dispatch20` | 130.37 |
| `sched.flux.on_asio.via20.post` | 193.08 |
| `sched.asio.raw.post20` | 533.65 |
| `full.flux.native.fast_runner.fast_awaitable.async4.inline` | 78.64 |
| `full.flux.native.fast_runner.fast_awaitable.async1.inline` | 20.17 |
| `full.flux.on_asio.fast_runner.fast_awaitable.async4.dispatch` | 229.79 |
| `full.asio.raw.dispatch.async4` | 79.82 |
| `full.asio.adapter.dispatch.async4` | 126.56 |
| `full.flux.on_asio.fast_runner.fast_awaitable.async4.post` | 321.33 |
| `full.asio.raw.post.async4` | 144.78 |
| `full.asio.adapter.post.async4` | 193.65 |
| `full.flux.on_asio.fast_runner.fast_awaitable.async1.dispatch` | 104.69 |
| `full.asio.raw.dispatch.async1` | 70.26 |
| `full.asio.adapter.dispatch.async1` | 83.75 |
| `full.flux.on_asio.fast_runner.fast_awaitable.async1.post` | 104.43 |
| `full.asio.raw.post.async1` | 70.60 |
| `full.asio.adapter.post.async1` | 84.45 |
| `full.flux.on_asio.fast_runner.when_all2.fastagg.post` | 162.32 |
| `full.asio.raw.post.when_all2` | 138.22 |
| `full.flux.on_asio.fast_runner.when_any2.fastagg.post` | 141.49 |
| `full.asio.raw.post.when_any2` | 137.60 |

### Real backend async overhead (strict no-exception, same host/toolchain, `ns/op`)

| Case | ns/op |
|---|---:|
| `real.flux.fast_runner.fast_awaitable.async4.backend` | 5497.95 |
| `real.asio.raw.async4.backend` | 4975.41 |
| `real.flux.fast_runner.fast_awaitable.async1.backend` | 5285.36 |
| `real.asio.raw.async1.backend` | 4991.35 |


### Windows snapshot (`2026-03-02`, separate local host)

- OS: `Windows 11 Home` (`10.0.26200`, `x64`)
- CPU: `AMD Ryzen 7 3700X` (`8` cores / `16` threads)
- Memory: `31.91 GB`
- Compiler: `clang++ 21.1.0` (`x86_64-pc-windows-msvc`)

Commands (PowerShell equivalent):

```bash
# flow perf (exception / no-exception)
clang++ -std=c++14 -O3 -DNDEBUG -I. test/flow_perf.cpp -o test/bin/flow_perf_exc.exe
clang++ -std=c++14 -O3 -DNDEBUG -fno-exceptions -DFLUX_FOUNDRY_NO_EXCEPTION_STRICT=1 -fno-rtti -march=native -fstrict-aliasing -I. test/flow_perf_noexcept.cpp -o test/bin/flow_perf_noexc.exe
.\test\bin\flow_perf_exc.exe
.\test\bin\flow_perf_noexc.exe

# flux vs asio horizontal compare
# set ASIO include path first, e.g. $env:ASIO_INCLUDE="C:\path\to\asio\include"
clang++ -std=c++14 -O3 -DNDEBUG -pthread -I. -I"$env:ASIO_INCLUDE" bench/horizontal_compare/compare_flux_asio.cpp -o test/bin/flux_asio_horizontal_compare.exe
# NOTE: Asio path uses RTTI internally (typeid), so do NOT pass -fno-rtti here.
clang++ -std=c++14 -O3 -DNDEBUG -fno-exceptions -DFLUX_FOUNDRY_NO_EXCEPTION_STRICT=1 -DASIO_NO_EXCEPTIONS=1 -DASIO_DISABLE_EXCEPTIONS=1 -march=native -fstrict-aliasing -pthread -I. -I"$env:ASIO_INCLUDE" bench/horizontal_compare/compare_flux_asio_noexcept.cpp -o test/bin/flux_asio_horizontal_compare_noexcept.exe
.\test\bin\flux_asio_horizontal_compare.exe
.\test\bin\flux_asio_horizontal_compare_noexcept.exe
```

Windows notes:

- Keep this as a host-specific snapshot; regenerate locally with the command block above.
- The macOS tables in this README are the currently maintained baseline.

### Flow perf matrix (Windows, `ns/op`, median of 7 rounds)

| Case | Exceptions ON | Exceptions OFF |
|---|---:|---:|
| `direct.loop20` | 6.14 | 5.93 |
| `runner.sync.20nodes` | 31.41 | 21.27 |
| `fast_runner.sync.20nodes` | 10.62 | 5.84 |
| `runner.awaitable.async4` | 351.80 | 313.17 |
| `fast_runner.awaitable.async4` | 160.62 | 118.47 |
| `runner.fast_awaitable.async4` | 201.00 | 155.88 |
| `fast_runner.fast_awaitable.async4` | 117.82 | 81.60 |
| `runner.awaitable.async1` | 90.78 | 79.88 |
| `fast_runner.awaitable.async1` | 41.35 | 27.25 |
| `runner.fast_awaitable.async1` | 51.02 | 37.62 |
| `fast_runner.fast_awaitable.async1` | 33.25 | 18.68 |
| `runner.when_all.2` | 202.62 | 187.85 |
| `fast_runner.when_all_fast.2` | 157.65 | 128.37 |
| `runner.when_all_fast.2` | 173.73 | 148.01 |
| `runner.when_any.2` | 178.62 | 159.00 |
| `fast_runner.when_any_fast.2` | 120.11 | 98.49 |
| `runner.when_any_fast.2` | 140.10 | 117.55 |

### Horizontal compare (Windows, exceptions ON, core matrix, `ns/op`)

| Case | ns/op |
|---|---:|
| `baseline.direct.loop20` | 6.07 |
| `sched.flux.via20.dispatch` | 1520.06 |
| `sched.asio.dispatch20` | 1467.20 |
| `sched.flux.via20.post` | 1490.82 |
| `sched.asio.post20` | 9605.10 |
| `full.flux.runner.async4.post` | 3181.42 |
| `full.flux.fast_runner.async4.post` | 3275.80 |
| `full.flux.runner.fast_awaitable.async4.post` | 3639.41 |
| `full.flux.fast_runner.fast_awaitable.async4.post` | 3424.16 |
| `full.asio.post.async4` | 2725.10 |
| `full.flux.runner.when_all2.post` | 1722.61 |
| `full.flux.fast_runner.when_all2.post` | 1848.91 |
| `full.flux.fast_runner.when_all2.fastagg.post` | 1771.33 |
| `full.asio.post.when_all2` | 1940.99 |
| `full.flux.runner.when_any2.post` | 1636.75 |
| `full.flux.fast_runner.when_any2.post` | 1652.85 |
| `full.flux.fast_runner.when_any2.fastagg.post` | 1551.59 |
| `full.asio.post.when_any2` | 1931.10 |

### Horizontal compare (Windows, strict no-exception, expanded matrix excerpt, `ns/op`)

| Case | ns/op |
|---|---:|
| `baseline.direct.loop20` | 5.89 |
| `sched.flux.native.via20.inline` | 72.86 |
| `sched.flux.native.via20.dispatch` | 404.06 |
| `sched.flux.on_asio.via20.dispatch` | 1424.66 |
| `sched.asio.raw.dispatch20` | 1425.52 |
| `sched.flux.on_asio.via20.post` | 1419.61 |
| `sched.asio.raw.post20` | 9533.45 |
| `full.flux.native.fast_runner.fast_awaitable.async4.inline` | 78.12 |
| `full.flux.native.fast_runner.fast_awaitable.async1.inline` | 25.83 |
| `full.flux.on_asio.fast_runner.fast_awaitable.async4.dispatch` | 1715.34 |
| `full.asio.raw.dispatch.async4` | 1474.33 |
| `full.asio.adapter.dispatch.async4` | 1552.46 |
| `full.flux.on_asio.fast_runner.fast_awaitable.async4.post` | 3098.98 |
| `full.asio.raw.post.async4` | 2892.24 |
| `full.asio.adapter.post.async4` | 2895.56 |
| `full.flux.on_asio.fast_runner.fast_awaitable.async1.dispatch` | 1458.18 |
| `full.asio.raw.dispatch.async1` | 1500.19 |
| `full.asio.adapter.dispatch.async1` | 1563.52 |
| `full.flux.on_asio.fast_runner.fast_awaitable.async1.post` | 1500.29 |
| `full.asio.raw.post.async1` | 1424.87 |
| `full.asio.adapter.post.async1` | 1458.67 |
| `full.flux.on_asio.fast_runner.when_all2.fastagg.post` | 1662.84 |
| `full.asio.raw.post.when_all2` | 1936.70 |
| `full.flux.on_asio.fast_runner.when_any2.fastagg.post` | 1511.55 |
| `full.asio.raw.post.when_any2` | 2132.18 |

### Real backend async overhead (Windows, strict no-exception, same host/toolchain, `ns/op`)

| Case | ns/op |
|---|---:|
| `real.flux.fast_runner.fast_awaitable.async4.backend` | 2531.64 |
| `real.asio.raw.async4.backend` | 2111.96 |
| `real.flux.fast_runner.fast_awaitable.async1.backend` | 2169.42 |
| `real.asio.raw.async1.backend` | 2265.26 |

### Linux snapshot (`2026-03-02`, same hardware as Windows snapshot)

- OS: `Ubuntu Linux` (same machine as Windows snapshot, switched OS)
- CPU: `AMD Ryzen 7 3700X` (`8` cores / `16` threads)
- Memory: `31.91 GB`
- Compiler: `clang++ -std=c++14 -O3` (local run log)

Commands (Linux equivalent):

```bash
# flow perf (exception / strict no-exception)
clang++ -std=c++14 -O3 -I. test/flow_perf.cpp -o /tmp/flow_perf_exc -pthread
clang++ -std=c++14 -O3 -fno-exceptions -DFLUX_FOUNDRY_NO_EXCEPTION_STRICT=1 -fno-rtti -march=native -fstrict-aliasing -I. test/flow_perf_noexcept.cpp -o /tmp/flow_perf_noexc -pthread
/tmp/flow_perf_exc
/tmp/flow_perf_noexc

# flux vs asio horizontal compare
bash bench/horizontal_compare/run.sh
bash bench/horizontal_compare/run_noexcept.sh
```

### Flow perf matrix (Linux, `ns/op`, median of 7 rounds)

| Case | Exceptions ON | Exceptions OFF |
|---|---:|---:|
| `direct.loop20` | 5.86 | 5.84 |
| `runner.sync.20nodes` | 26.84 | 23.60 |
| `fast_runner.sync.20nodes` | 5.86 | 5.53 |
| `runner.awaitable.async4` | 375.30 | 324.19 |
| `fast_runner.awaitable.async4` | 124.24 | 109.73 |
| `runner.fast_awaitable.async4` | 221.58 | 169.25 |
| `fast_runner.fast_awaitable.async4` | 93.58 | 81.56 |
| `runner.awaitable.async1` | 94.45 | 82.11 |
| `fast_runner.awaitable.async1` | 30.32 | 28.26 |
| `runner.fast_awaitable.async1` | 55.75 | 43.97 |
| `fast_runner.fast_awaitable.async1` | 18.93 | 17.39 |
| `runner.when_all.2` | 192.97 | 188.08 |
| `fast_runner.when_all_fast.2` | 125.14 | 126.24 |
| `runner.when_all_fast.2` | 156.34 | 154.37 |
| `runner.when_any.2` | 165.42 | 161.51 |
| `fast_runner.when_any_fast.2` | 95.13 | 95.68 |
| `runner.when_any_fast.2` | 129.05 | 125.68 |

### Horizontal compare (Linux, exceptions ON, core matrix, `ns/op`)

| Case | ns/op |
|---|---:|
| `baseline.direct.loop20` | 6.27 |
| `sched.flux.via20.dispatch` | 246.23 |
| `sched.asio.dispatch20` | 159.26 |
| `sched.flux.via20.post` | 247.76 |
| `sched.asio.post20` | 689.79 |
| `full.flux.runner.async4.post` | 703.32 |
| `full.flux.fast_runner.async4.post` | 481.98 |
| `full.flux.runner.fast_awaitable.async4.post` | 639.63 |
| `full.flux.fast_runner.fast_awaitable.async4.post` | 461.71 |
| `full.asio.post.async4` | 187.36 |
| `full.flux.runner.when_all2.post` | 317.02 |
| `full.flux.fast_runner.when_all2.post` | 258.03 |
| `full.flux.fast_runner.when_all2.fastagg.post` | 262.37 |
| `full.asio.post.when_all2` | 141.28 |
| `full.flux.runner.when_any2.post` | 280.62 |
| `full.flux.fast_runner.when_any2.post` | 237.41 |
| `full.flux.fast_runner.when_any2.fastagg.post` | 215.74 |
| `full.asio.post.when_any2` | 126.85 |

### Horizontal compare (Linux, strict no-exception, expanded matrix excerpt, `ns/op`)

| Case | ns/op |
|---|---:|
| `baseline.direct.loop20` | 6.61 |
| `sched.flux.native.via20.inline` | 101.77 |
| `sched.flux.native.via20.dispatch` | 1459.86 |
| `sched.flux.on_asio.via20.dispatch` | 267.01 |
| `sched.asio.raw.dispatch20` | 166.47 |
| `sched.flux.on_asio.via20.post` | 275.68 |
| `sched.asio.raw.post20` | 812.18 |
| `full.flux.native.fast_runner.fast_awaitable.async4.inline` | 86.30 |
| `full.flux.native.fast_runner.fast_awaitable.async1.inline` | 22.02 |
| `full.flux.on_asio.fast_runner.fast_awaitable.async4.dispatch` | 327.29 |
| `full.asio.raw.dispatch.async4` | 105.31 |
| `full.asio.adapter.dispatch.async4` | 181.02 |
| `full.flux.on_asio.fast_runner.fast_awaitable.async4.post` | 459.56 |
| `full.asio.raw.post.async4` | 203.90 |
| `full.asio.adapter.post.async4` | 284.25 |
| `full.flux.on_asio.fast_runner.fast_awaitable.async1.dispatch` | 150.47 |
| `full.asio.raw.dispatch.async1` | 97.04 |
| `full.asio.adapter.dispatch.async1` | 123.47 |
| `full.flux.on_asio.fast_runner.fast_awaitable.async1.post` | 153.20 |
| `full.asio.raw.post.async1` | 97.38 |
| `full.asio.adapter.post.async1` | 123.45 |
| `full.flux.on_asio.fast_runner.when_all2.fastagg.post` | 265.95 |
| `full.asio.raw.post.when_all2` | 144.90 |
| `full.flux.on_asio.fast_runner.when_any2.fastagg.post` | 212.32 |
| `full.asio.raw.post.when_any2` | 145.91 |

### Real backend async overhead (Linux, strict no-exception, same host/toolchain, `ns/op`)

| Case | ns/op |
|---|---:|
| `real.flux.fast_runner.fast_awaitable.async4.backend` | 5012.02 |
| `real.asio.raw.async4.backend` | 4156.70 |
| `real.flux.fast_runner.fast_awaitable.async1.backend` | 4745.71 |
| `real.asio.raw.async1.backend` | 4255.38 |

Notes:

- Linux snapshot above is sourced from `test/ubuntu_bench.txt` (`flow_perf*`) and `bench/horizontal_compare/linux.txt` (`run*.sh`).
- `run_noexcept.sh` currently covers the full expanded matrix (`57` benchmark cases).
- `run.sh` keeps a compact exception-enabled matrix for quick comparison.
- The current tables are product-realistic: they include framework optimizations such as DSL fusion (`via|via` deduplication).
- If you need strict symmetry against raw scheduler APIs, run a separate benchmark profile with those fusion optimizations intentionally disabled.
- This is a microbenchmark for framework overhead, not an end-to-end workload benchmark.
