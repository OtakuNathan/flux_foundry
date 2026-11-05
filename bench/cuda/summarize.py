#!/usr/bin/env python3
import argparse, csv, statistics, collections, pathlib
parser = argparse.ArgumentParser(description='Summarize matched CUDA end-to-end trials and the separate kernel reference.')
parser.add_argument('--results-dir', type=pathlib.Path, default=pathlib.Path(__file__).parent / 'results/rtx2080ti-20261105')
base = parser.parse_args().results_dir
results = list(csv.reader((base / 'results.csv').open()))
data = collections.defaultdict(dict)
for r in results:
    if r and r[0] == 'RUN':
        mode,w,h,window,rep,seconds,fps,mpix = r[1],int(r[2]),int(r[3]),int(r[4]),int(r[5]),float(r[6]),float(r[7]),float(r[8])
        data[(w,window,mode)][rep] = (fps,seconds,mpix)
    elif r and r[0] == 'VERIFY':
        print('Verification:', ', '.join(r[1:]))
for w,window in sorted({(k[0],k[1]) for k in data}):
    print(f'\n{w}x{w} window={window}:')
    baseline=data[(w,window,'native')]
    for mode in ('native','concrete','erased'):
        records=data[(w,window,mode)]
        fps=[x[0] for x in records.values()]
        common=sorted(baseline.keys() & records.keys())
        ratios=[100.0*(records[i][0]/baseline[i][0]-1) for i in common]
        print(f'  {mode:8} median {statistics.median(fps):9.2f} fps range [{min(fps):.2f}, {max(fps):.2f}] MPix/s {statistics.median(x[2] for x in records.values()):.2f}; median paired FPS vs native {statistics.median(ratios):+.2f}% range [{min(ratios):+.2f}%,{max(ratios):+.2f}%]')
kernel = collections.defaultdict(list)
for r in csv.reader((base/'kernel-results.csv').open()):
    if r and r[0] == 'GPU_ONLY':
        kernel[int(r[1])].append(float(r[6]))
print('\nDevice-only CUDA event reference:')
for size, fps in sorted(kernel.items()):
    print(f'  {size}x{size}: median {statistics.median(fps):,.2f} fps, range [{min(fps):,.2f}, {max(fps):,.2f}] (n={len(fps)})')
print('\nTelemetry:')
rows=list(csv.reader((base/'telemetry.csv').open())) if (base/'telemetry.csv').exists() else []
if rows:
    for index,name in ((1,'temperature C'),(2,'GPU utilisation %'),(3,'SM clock MHz'),(5,'power W')):
        vals=[float(r[index]) for r in rows]
        print(f'  {name}: min={min(vals):.1f} median={statistics.median(vals):.1f} max={max(vals):.1f} (n={len(vals)})')

if not rows:
    print('  not supplied')
