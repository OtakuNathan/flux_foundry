import csv,json,subprocess,os,statistics
from pathlib import Path
root=Path('/tmp/ff-io-perf-vFZ6HVwW')
source=root/'build/identity-after'
out=root/'build/identity-single-core-default';out.mkdir(exist_ok=True)
env=os.environ.copy();env['LD_LIBRARY_PATH']=str(root/'deps/usr/lib')
commands=[];rows=[]
for compiler in ['clang++','g++']:
 for variant in [0,1]:
  for backend in ['uring','dpdk']:
   cmd=json.loads((root/f'build/diag-{backend}/environment.json').read_text())['commands'][0]
   cmd[0]=compiler
   cmd[1:1]=['-I'+str(source),f'-DFLUX_FOUNDRY_AWAITABLE_IDENTITY_POOL={variant}',f'-DFLUX_FOUNDRY_AWAITABLE_POOL_SLOT_COUNT={256 if variant else 512}']
   cmd[cmd.index('-o')+1]=str(out/f'{compiler}-{variant}-{backend}')
   commands.append(cmd);subprocess.run(cmd,check=True,env=env)
for trial in range(5):
 for compiler in ['clang++','g++']:
  for variant in ([0,1] if trial%2==0 else [1,0]):
   for backend in ['uring','dpdk']:
    for path in (['staged','kernel'] if backend=='uring' else ['net_ring']):
     cmd=[str(out/f'{compiler}-{variant}-{backend}')]
     cmd+=([path,'fast','1048576','32','3'] if backend=='uring' else ['1048576','1','3'])
     commands.append(cmd)
     r=subprocess.run(cmd,check=True,text=True,capture_output=True,env=env)
     (out/f'{compiler}-{variant}-{backend}-{path}-{trial}.log').write_text(r.stdout+r.stderr)
     for line in r.stdout.splitlines():
      f=line.split(',')
      if backend=='uring' and line.startswith('uring,'):
       rows.append(dict(compiler=compiler,variant=variant,backend=backend,path=path,batch=32,trial=trial,ns=float(f[-1])))
      elif backend=='dpdk' and ',dpdk,net_ring,' in line:
       rows.append(dict(compiler=compiler,variant=variant,backend=backend,path=path,batch=int(f[5]),trial=trial,ns=float(f[6])))
with (out/'results.csv').open('w') as f:
 w=csv.DictWriter(f,fieldnames=rows[0].keys(),lineterminator='\n');w.writeheader();w.writerows(rows)
(out/'environment.json').write_text(json.dumps(dict(commands=commands),indent=2)+'\n')
s=['| Compiler | Path | Batch | ns baseline → identity | Change |','|---|---|---:|---:|---:|']
for compiler in ['clang++','g++']:
 for backend,path,batch in [('uring','staged',32),('uring','kernel',32),('dpdk','net_ring',1),('dpdk','net_ring',32)]:
  v=[statistics.median(r['ns'] for r in rows if r['compiler']==compiler and r['variant']==var and r['backend']==backend and r['path']==path and r['batch']==batch) for var in [0,1]]
  s.append(f'| {compiler} | {backend}/{path} | {batch} | {v[0]:.3f} → {v[1]:.3f} | {(v[1]/v[0]-1)*100:+.2f}% |')
(out/'summary.md').write_text('\n'.join(s)+'\n');print('\n'.join(s))
