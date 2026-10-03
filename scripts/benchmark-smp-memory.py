"""Fixed four-hart store loops with distinct pages and 256 KiB-spaced pages.

Every hart uses mhartid to select its own data address. This is a contention
diagnostic, not a substitute for real Linux benchmarks.
"""
import argparse,pathlib,struct,subprocess
from host_affinity import apply_affinity
p=argparse.ArgumentParser();p.add_argument('--exe',required=True)
p.add_argument('--instructions',type=int,default=100000000)
p.add_argument('--affinity',type=lambda value:int(value,0));a=p.parse_args()
root=pathlib.Path(__file__).resolve().parent.parent
def jal(offset):
    u=offset&0x1fffff
    return ((u>>20)<<31)|(((u>>1)&1023)<<21)|(((u>>11)&1)<<20)|(((u>>12)&255)<<12)|0x6f
for shift in (12,18):
    words=[0xf14022f3,(shift<<20)|(5<<15)|(1<<12)|(5<<7)|0x13,0x00020317,
           (5<<20)|(6<<15)|(6<<7)|0x33,(6<<15)|(3<<12)|(7<<7)|3,
           (1<<20)|(7<<15)|(7<<7)|0x13,(7<<20)|(6<<15)|(3<<12)|0x23,jal(-12)]
    file=root/'build'/f'benchmark-smp-store-{shift}.bin';file.write_bytes(struct.pack('<8I',*words))
    process=subprocess.Popen([str(root/a.exe),'--raw',str(file),'-m','2M','--smp','4',
        '--engine','jit','--max-steps',str(a.instructions),'--no-reg-dump'],cwd=root,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    try:
        if a.affinity:apply_affinity(process,a.affinity)
        output,error=process.communicate()
        if process.returncode:raise RuntimeError(error)
    finally:
        if process.poll() is None:process.terminate();process.wait()
    print(f'hart data stride={1<<shift} bytes',next(line for line in error.splitlines() if ' ins/s)' in line),flush=True)
