#!/usr/bin/env python3
"""Condense cycles:u perf-script samples by process/thread and self symbol."""
import argparse
import collections
import json
import pathlib
import re

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('directory')
p.add_argument('--json-output')
a=p.parse_args()
root=pathlib.Path(a.directory)
header=re.compile(r'^\S+\s+(\d+)/(\d+)\s+([\d.]+):\s+(\d+)\s+cycles:u:')

def parse(path):
    samples=[]
    current=None
    for line in path.read_text().splitlines():
        m=header.match(line)
        if m:
            if current is not None: samples.append(current)
            current={'pid':int(m[1]),'tid':int(m[2]),'time':float(m[3]),'period':int(m[4]),'frames':[]}
        elif current is not None and line[:1].isspace() and ' (' in line:
            left,dso=line.rsplit(' (',1)
            parts=left.strip().split(None,1)
            if len(parts)==2:
                current['frames'].append({'symbol':parts[1],'dso':dso.rstrip(')')})
    if current is not None: samples.append(current)
    if not samples: raise ValueError(f'no samples parsed: {path}')
    return samples

def short_symbol(symbol):
    out=[]
    depth=0
    for char in symbol:
        if char=='<':
            if depth==0: out.append('<…>')
            depth+=1
        elif char=='>':
            depth=max(0,depth-1)
        elif depth==0:
            out.append(char)
    return ''.join(out)

summaries={}
paths=sorted(root.glob('*-script.txt'))
if not paths:
    raise ValueError(f'no perf script files in {root}')
for path in paths:
    samples=parse(path)
    name=path.name.removesuffix('-script.txt')
    summary={'sample_count':len(samples),'threads':{}}
    for role in ('producer','consumer'):
        subset=[s for s in samples if (s['pid']==s['tid']) == (role=='producer')]
        total=sum(s['period'] for s in subset)
        self_counts=collections.Counter()
        atomic_callers=collections.Counter()
        categories=collections.Counter()
        for s in subset:
            if not s['frames']:
                symbol='[no callchain]'
            else:
                symbol=s['frames'][0]['symbol']
            self_counts[symbol]+=s['period']
            if symbol.startswith('__aarch64_'):
                categories['outlined atomic helpers']+=s['period']
                if len(s['frames'])>1:
                    atomic_callers[s['frames'][1]['symbol']]+=s['period']
            if symbol.startswith('flux_foundry::static_mem_pool<'):
                categories['explicit static pool functions']+=s['period']
            if any(x in symbol for x in ('_int_malloc','_int_free','malloc_consolidate','__libc_malloc','__libc_free','malloc@','free@')):
                categories['libc allocation/free functions']+=s['period']
            if symbol=='__kernel_clock_gettime' or symbol in ('clock_gettime','__clock_gettime'):
                categories['timestamp functions']+=s['period']
        def top(counter):
            return [{'symbol':key,'cycles_weight':value,'percent':100*value/total if total else 0} for key,value in counter.most_common(12)]
        summary['threads'][role]={'samples':len(subset),'cycles_weight':total,'top_self':top(self_counts),'atomic_callers':top(atomic_callers),'categories':{k:100*v/total for k,v in categories.items()} if total else {}}
    summaries[name]=summary
    print('\n===',name,'samples=',len(samples),'===')
    for role,data in summary['threads'].items():
        print(role,'samples',data['samples'],'categories',{k:round(v,2) for k,v in data['categories'].items()})
        for item in data['top_self'][:8]: print(f"  {item['percent']:6.2f}% {short_symbol(item['symbol'])[:500]}")
        if data['atomic_callers']:
            print('  Atomic caller stacks:')
            for item in data['atomic_callers'][:5]: print(f"    {item['percent']:6.2f}% {short_symbol(item['symbol'])[:500]}")
if a.json_output:
    pathlib.Path(a.json_output).write_text(json.dumps(summaries,ensure_ascii=False,indent=2)+'\n')
