# Kick versus poll — desktop, 2026-11-19

How should a pooled CUDA pipeline learn that an image is ready? Two candidates, same
kernel, same pooled slots, same 257-sample verification per image, window swept
1/2/4/8:

- **poll** — one disable-timing event per slot, `cudaEventQuery` in the loop that
  issues work.
- **hostfunc** — `cudaLaunchHostFunc` after each image; the driver thread runs a
  callback that pushes the slot into a queue, and the issuing thread blocks on a
  condition variable until an image is ready.

Environment: RTX 2080 Ti, driver 620.70.06, toolkit 13.4.3, `sm_75`, Ryzen 7 3700X,
GCC 16.2.1, Release. FF `7af9c8a` (the probe needs no FF at all). Medians of five
samples; `cpu` is issuing-thread CPU seconds per wall second, so 1.0 means one
saturated core. `lat` is issue-to-visible time, which at window 8 is dominated by
queueing and only near window 1 approximates the cost of detection itself.

## 128x128 (64 KiB per image, per-operation overhead dominates)

| Window | poll fps | hostfunc fps | throughput | poll cpu | hostfunc cpu | poll lat | hostfunc lat |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 46,282 | 23,757 | **-48.7%** | 1.00 | 0.27 | 19.7 us | 37.3 us |
| 2 | 88,166 | 52,517 | **-40.4%** | 1.00 | 0.54 | 20.7 us | 32.0 us |
| 4 | 141,679 | 115,127 | **-18.7%** | 1.00 | 0.99 | 25.4 us | 26.5 us |
| 8 | 152,325 | 117,706 | **-22.7%** | 1.00 | 0.99 | 40.6 us | 26.7 us |

The callback path costs 1.9 us per image here (6.6 us becomes 8.5 us), and by window 4
the consumer thread is saturated anyway, so the core that was supposed to be freed is
spent on the callback instead. Polling wins outright.

## 512x512 (1 MiB per image, PCIe-bound)

| Window | poll fps | hostfunc fps | throughput | poll cpu | hostfunc cpu | poll lat | hostfunc lat |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 7,945 | 6,802 | -14.4% | 1.00 | **0.08** | 123.0 us | 139.8 us |
| 2 | 12,747 | 12,466 | -2.2% | 1.00 | **0.15** | 153.5 us | 153.3 us |
| 4 | 12,736 | 12,470 | -2.1% | 1.00 | **0.15** | 310.1 us | 313.7 us |
| 8 | 12,746 | 12,487 | -2.0% | 1.00 | **0.15** | 621.9 us | 632.0 us |

Once the transfer dominates, the spinning thread has nothing to do but burn a core:
the callback path gives up 2% throughput and returns **85% of a core**. The window-1
row pays 14% because detection latency is a larger share of a 123 us image.

Verified 288,000 sampled 128x128 images and 72,000 sampled 512x512 images across both
modes, byte-identical between them — the callback path does not change the output.

## The rule this gives

- Small image, high rate, latency-sensitive, or window 1-2: **poll**. The callback
  costs more CPU than the spin it replaces.
- Large image, transfer-bound, or a core that has other work: **kick**. 2% throughput
  buys back a whole core.
- Window 4-8 with a small image: poll, and do not expect a callback to save CPU —
  the consumer becomes the bottleneck either way.

For FF this is a scheduling question, not a throughput one: the same conclusion as the
completion-batching probe. `cuda_awaitable.h` already has the hook
(`await_cuda(executor_to_resume)`), so the kick path resumes on a chosen executor
instead of the driver thread; what this snapshot adds is the price list.

## Reproduce

```sh
CUDA=/path/to/cuda
$CUDA/bin/nvcc -std=c++14 -O3 -DNDEBUG --allow-unsupported-compiler -arch=sm_75 \
  -I. -I$CUDA/include kick_probe.cu -o kick-probe
./kick-probe > kick.csv
```

`kick.csv` is
`kind,mode,width,window,rep,fps,wall_s,thread_cpu_s,proc_cpu_s,cpu_ratio_thread,mean_latency_us`.

## Caveats

- One GPU, `sm_75`, clocks unlocked, compositor active.
- `cpu_ratio_thread` is the issuing thread only; the driver callback thread is not
  counted there, so the hostfunc CPU figures are a lower bound on the CPU actually
  consumed by the callback.
- Latency at large windows is queueing time, not detection overhead.
