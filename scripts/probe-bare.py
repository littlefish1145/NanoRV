"""A/B probe for the bare RV64 programs produced by benchmark-bare-qemu.py.

Runs one nanorv.exe configuration per invocation with UART START/DONE timing
(identical to benchmark-bare-qemu.py) so environment toggles such as
NanoRV_INLINE_LOADS=0 or engine choices can be compared against a saved default.
"""
import argparse,json,os,pathlib,re,subprocess,threading,time
p=argparse.ArgumentParser()
p.add_argument('bin',help='bare program .bin from benchmark-bare-qemu.py')
p.add_argument('--exe',default='build/perf22/bin/Release/nanorv.exe')
p.add_argument('--qemu',action='store_true',help='run under qemu-system-riscv64 instead of nanorv')
p.add_argument('--engine',default='jit')
p.add_argument('--env',action='append',default=[],metavar='K=V',help='extra environment variable')
p.add_argument('--arg',action='append',default=[],help='extra nanorv.exe argument')
p.add_argument('--rounds',type=int,default=3)
p.add_argument('--affinity',type=lambda v:int(v,0))
p.add_argument('--profile',action='store_true',help='set NanoRV_PROFILE=1 and keep stderr stats')
p.add_argument('--expect-x1',help='expected x1 checksum (lowercase hex)')
a=p.parse_args()
root=pathlib.Path(__file__).resolve().parent.parent
env=dict(os.environ)
for item in a.env:
    key,_,value=item.partition('=');env[key]=value
if a.profile:env['NanoRV_PROFILE']='1'
if a.qemu:
    cmd=[str(root/'xpack-qemu-riscv-9.2.4-1/bin/qemu-system-riscv64.exe'),'-machine','virt','-bios','none',
         '-m','1G','-smp','1','-nographic','-monitor','none','-nic','none',
         '-device',f'loader,file={root/a.bin},addr=0x80000000,cpu-num=0,force-raw=on']
else:
    cmd=[str(root/a.exe),'--raw',str(root/a.bin),'-m','1G','--engine',a.engine,'--no-reg-dump']+a.arg
times=[];stats=[];checksums=[]
for round_index in range(a.rounds):
    proc=subprocess.Popen(cmd,cwd=root,stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=env)
    data=bytearray();marks={};error=[]
    def read_output():
        tail=b''
        while chunk:=os.read(proc.stdout.fileno(),1):
            observed=time.perf_counter();data.extend(chunk);combined=tail+chunk
            for marker in (b'NanoRV_BARE_START\n',b'NanoRV_BARE_DONE\n',b'NanoRV_BARE_START\r\n',b'NanoRV_BARE_DONE\r\n'):
                if marker in combined:marks.setdefault('start' if b'START' in marker else 'done',observed)
            tail=combined[-80:]
    def read_error():error.append(proc.stderr.read())
    if a.affinity:
        from host_affinity import apply_affinity
        apply_affinity(proc,a.affinity)
    out_t=threading.Thread(target=read_output);err_t=threading.Thread(target=read_error)
    out_t.start();err_t.start()
    try:proc.wait(timeout=300)
    finally:
        if proc.poll() is None:proc.terminate();proc.wait()
        out_t.join();err_t.join()
    transcript=bytes(data).decode(errors='replace')+b''.join(error).decode(errors='replace')
    if proc.returncode or set(marks)!={'start','done'}:raise RuntimeError('run failed: '+transcript[-3000:])
    if a.expect_x1:
        match=re.search(r'CHECKSUM=([0-9a-f]+)',transcript)
        if not match or int(match[1],16)!=int(a.expect_x1,16):raise RuntimeError('checksum mismatch: '+transcript[-600:])
    else:
        match=re.search(r'CHECKSUM=([0-9a-f]+) ([0-9a-f]+) ([0-9a-f]+)',transcript)
    if match:checksums.append(match.groups())
    times.append(marks['done']-marks['start'])
    for line in transcript.splitlines():
        if 'JIT sample hart' in line or 'JIT profile hart' in line or 'JIT detail hart' in line:
            stats.append(line)
times.sort();median=times[len(times)//2]
print(json.dumps({'exe':a.exe,'engine':a.engine,'env':a.env,'arg':a.arg,
                  'seconds':times,'median':median,'checksums':sorted(set(checksums))}),flush=True)
for line in dict.fromkeys(stats):print(line,flush=True)
