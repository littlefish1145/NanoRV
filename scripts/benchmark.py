"""Reproducible guest instruction throughput for the three execution engines."""
import pathlib,struct,subprocess,argparse
parser=argparse.ArgumentParser();parser.add_argument('--exe',default='build/performance-final/bin/Release/nanorv.exe')
parser.add_argument('--instructions',type=int,default=100000000)
args=parser.parse_args()
root=pathlib.Path(__file__).resolve().parent.parent
def jal(offset):
    u=offset&0x1fffff
    return ((u>>20)<<31)|(((u>>1)&1023)<<21)|(((u>>11)&1)<<20)|(((u>>12)&255)<<12)|0x6f
programs={'arithmetic':[0x00108093]*31+[jal(-124)],
          'memory':[0x00010197,0x0001b103,0x00110113,0x0021b023,jal(-12)]}
for name,words in programs.items():
    path=root/'build'/('benchmark-'+name+'.bin')
    path.write_bytes(struct.pack('<'+'I'*len(words),*words))
    for engine in ['interpreter','block','jit']:
        cmd=[str(root/args.exe),'--raw',str(path),'-m','1M','--max-steps',str(args.instructions),'--no-reg-dump','--engine',engine]
        result=subprocess.run(cmd,cwd=root,capture_output=True,text=True,check=True)
        print(name,engine, next(line for line in result.stderr.splitlines() if ' ins/s)' in line),flush=True)
