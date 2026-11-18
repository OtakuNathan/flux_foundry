# Pooled-context CUDA throughput — desktop, 2026-11-18

## Environment and revisions

- RTX 2080 Ti, driver 620.70.06, CUDA toolkit 13.4.3, `sm_75`.
- Ryzen 7 3700X, GCC 16.2.1, Release `-O3 -DNDEBUG`, C++14.
- FF `030bd9e` plus this directory's `benchmark_pooled.cpp` and `resource_probe.cu`; Portal `7c0ac2f`.
- Desktop compositor active, GPU clocks not locked. The machine was rebooted immediately before the retained trials.
- The same FF and Portal revisions pass the CUDA typed demos on this device: ten of ten `ctest -L cuda` tests, including the injected event-record failures and both no-exception variants.

## What is being compared

`benchmark.cpp` gives every image its own stream, event, `cudaMalloc` and pageable host buffer. This snapshot keeps the kernel, the window, the polling queue and the 257-sample verification and moves only ownership: each in-flight slot has a context that owns its stream, disable-timing event, device buffer and pinned host buffer, and every invocation is dispatched against a fresh context. The blueprint stays static, so the pooled path applies no change to FF or Portal.

`native` therefore means "per-operation resources through the same completion path", `native_pool` the same path over a pooled context, and the two FF modes the concrete and erased typed invocations over pooled contexts.

Five samples per mode, image order rotated every sample, 6000 images per 128x128 sample and 1500 per 512x512 sample. Busy polling, no sleep between polls. Medians in completed images/s; the parenthesised figure is the paired deviation from `native_pool`, i.e. the cost of FF and Portal.

| Image | Window | native | native_pool | concrete_pool | erased_pool |
| --- | ---: | ---: | ---: | ---: | ---: |
| 128x128 | 1 | 28,689 | 41,445 | 40,936 (-1.2%) | 41,389 (-0.1%) |
| 128x128 | 8 | 28,502 | 114,743 | 112,529 (-1.9%) | 110,399 (-3.8%) |
| 512x512 | 1 | 4,517 | 6,121 | 6,108 (-0.2%) | 6,071 (-0.8%) |
| 512x512 | 8 | 1,642 | 12,638 | 12,711 (+0.6%) | 12,693 (+0.4%) |

Verification: 240,064 sampled 128x128 images and 60,064 sampled 512x512 images across the four modes, all matching the first result byte for byte. Pooling does not change the output.

`results.csv` is the raw `benchmark_pooled.cpp` stream (`kind,mode,width,height,window,rep,seconds,fps`). It is not the same column layout as `../rtx2080ti-20261105/results.csv`, so `summarize.py` does not parse it.

## Reading the `native` column

Per-operation allocation is not a constant. In this binary the `native` row reads 28,689 at 128x128 window 1, whereas `benchmark.cpp` and `resource_probe.cu` with `PROBE_ONLY_NATIVE=1` read about 6,200-6,800 in a process that never pools: the live pooled allocations keep the driver allocator warm, so per-operation `cudaMalloc` is served from cache. Both are real conditions but only one can be the baseline, and the pooled one is the stricter test — every paired comparison above is same-process, same-binary. For the per-operation baseline use `../rtx2080ti-20261105/` or a `PROBE_ONLY_NATIVE=1` run.

## The two levers, separated

`resource-probe.csv` is one `resource_probe.cu` run without `PROBE_COPY`; `resource-probe-materialized.csv` is one run with it, which copies each image out of the pinned buffer into a fresh `std::vector` exactly as the by-value `image_result` does. Medians of the retained run:

| Image | Window | native | pool | graph | pool + materialise |
| --- | ---: | ---: | ---: | ---: | ---: |
| 128x128 | 1 | 6,124 | 39,645 | 41,483 | 36,171 |
| 128x128 | 8 | 17,036 | 122,397 | 128,994 | 88,975 |
| 512x512 | 1 | 2,752 | 6,855 | 7,298 | 5,282 |
| 512x512 | 8 | 1,386 | 12,020 | 12,138 | 11,569 |

- Pooling is the whole story: 6-9x at window 8.
- A captured kernel+D2H graph replayed per operation adds 5-9% at 128x128 and nothing at 512x512. Replaying with `cudaGraphExecKernelNodeSetParams` on every operation costs about 0.5 us, which is affordable but does not pay for itself here.
- The materialisation column is the by-value result type, not the framework: `native_pool` pays it as well. It costs 5-27% depending on image size.
- 512x512 window 8 is capped near 12,700 images/s in every mode. That is 1 MiB per image at roughly 13.4 GB/s, i.e. PCIe 3.0 x16 line rate on this board. Nothing above the transfer helps until fewer bytes come back.

## Caveats

- One GPU, `sm_75` only; no Ampere-or-newer run.
- Run-to-run spread at 128x128 window 8 is roughly +/-10% on this desktop, so single-digit mode differences are noise. Repeated runs of the same binary moved between 108k and 129k there.
- Clocks are not locked and the compositor was running, so absolute throughput is a desktop condition, not a hardware limit.
- Both binaries keep every pooled buffer alive for the whole trial set. That is the intended usage, but it also means the `native` control is measured under a warm allocator, as described above.
