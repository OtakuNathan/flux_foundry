# Payload and completion costs — desktop, 2026-11-18 (second session)

Two follow-ups to [the pooled-context snapshot](../rtx2080ti-20261118-pooled/README.md).
Both keep the kernel, the window and the pooled resources; each varies exactly one
thing.

- Environment: RTX 2080 Ti, driver 620.70.06, toolkit 13.4.3, `sm_75`, Ryzen 7 3700X,
  GCC 16.2.1, Release `-O3 -DNDEBUG`. FF `030bd9e`, Portal `7c0ac2f`.
- Window 8 in every row, pooled slots, pinned host buffers, busy polling, medians of
  five samples.

## Payload: how much of the 512x512 wall is the byte count

`payload_probe.cu` runs the unchanged RGBA kernel and copies only a prefix of the
image back, so the device work is identical and only the returned bytes vary. Pixel
verification is skipped when the copy is truncated.

| Image | Returned per image | fps | img/ms | GB/s |
| --- | ---: | ---: | ---: | ---: |
| 128x128 | 64 KiB (full RGBA) | 163,839 | 163.84 | 10.74 |
| 128x128 | 32 KiB | 203,977 | 203.98 | 6.68 |
| 128x128 | 16 KiB | 204,965 | 204.97 | 3.36 |
| 128x128 | 4 KiB | 195,368 | 195.37 | 0.80 |
| 512x512 | 1024 KiB (full RGBA) | 12,681 | 12.68 | **13.30** |
| 512x512 | 512 KiB | 24,983 | 24.98 | 13.10 |
| 512x512 | 256 KiB | 42,788 | 42.79 | 11.22 |
| 512x512 | 64 KiB | 42,783 | 42.78 | 2.80 |

- At 512x512 the full payload saturates at 13.3 GB/s, which is PCIe 3.0 x16 line rate.
  Removing bytes is close to free throughput: 512 KiB doubles it, 256 KiB is **3.4x**.
- Below 256 KiB nothing improves: 256 KiB and 64 KiB both sit at 42.8k images/s
  (23.4 us per image), so the kernel and host submission become the limit there. That
  is the floor for this kernel at this window.
- At 128x128 the floor is reached sooner: 32 KiB already gives 204k images/s (4.9 us
  per image) and 16 KiB adds nothing, so returning one byte per pixel at 128x128 buys
  about 25% and then stops.

Practical reading: one byte per pixel instead of RGBA is worth ~3.4x at 512x512 and
~25% at 128x128, and every byte below that budget is wasted. This is a result-type
decision, not a framework one — `image_result` currently owns a 4-byte-per-pixel
`std::vector`.

## Completion: does amortising per-image completion pay

`completion_probe.cu` keeps everything else fixed and changes only how completion is
detected: a disable-timing event per image, `cudaStreamQuery` with no event at all, or
four images per slot per round with one event recorded after the fourth. All three do
the same 257-sample verification per image.

| Image | event (current) | streamq | batch4 |
| --- | ---: | ---: | ---: |
| 128x128 | 149,979 | 153,439 (+2.3%) | 139,503 (-7.0%) |
| 512x512 | 12,743 | 12,743 (-0.0%) | 12,537 (-1.6%) |

Verified 90,024 sampled images at 128x128 and 22,524 at 512x512 across the three modes.

- Dropping the event entirely is worth +2.3% at 128x128, which is inside the +/-10%
  run-to-run spread documented for this desktop. At 512x512 it is exactly zero.
- Batching four images per completion is **slower** in both sizes: the four buffers per
  slot spread the pipeline less evenly, and the amortised bookkeeping was never the
  cost.
- Conclusion: per-image completion detection is not a throughput lever at these rates.
  The per-owner return batching that FF added for other reasons should not be justified
  on GPU throughput, because there is none to collect here.

## Reproduce

```sh
CUDA=/path/to/cuda
$CUDA/bin/nvcc -std=c++14 -O3 -DNDEBUG --allow-unsupported-compiler -arch=sm_75 \
  -I. -I$CUDA/include payload_probe.cu -o payload-probe
$CUDA/bin/nvcc -std=c++14 -O3 -DNDEBUG --allow-unsupported-compiler -arch=sm_75 \
  -I. -I$CUDA/include completion_probe.cu -o completion-probe
./payload-probe > payload.csv
./completion-probe > completion.csv
```

Neither probe needs FF, Portal or the pinned revisions. `payload.csv` is
`kind,width,height,window,copy_bytes,rep,seconds,fps,GBps`; `completion.csv` is
`kind,mode,width,height,window,rep,seconds,fps`.

## Not measured tonight

The heterogeneous ring — one FF flow carrying an io_uring read, a host-to-device copy
and a kernel — is still unpriced. The mechanisms exist (`extension/io_uring_awaitable.h`,
`extension/cuda_awaitable.h`, both aliases over the same external-async contract); what
is missing is the measurement, not the plumbing.
