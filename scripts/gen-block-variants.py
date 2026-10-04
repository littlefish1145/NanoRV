"""Generate bare RV64 loop variants with fixed total instructions but varying
block length, isolating per-iteration bookkeeping and dispatch overhead.

fus: K repeated `addi x1,x1,1` per loop body, block length K+2, total 4.096e9.
mem: N repeats of [lw,addi,sw] per loop body, block length 3N+2, total 2.56e8.
"""
import argparse,pathlib,struct
p=argparse.ArgumentParser()
p.add_argument('--out',default='build/blockvar')
a=p.parse_args()
def i(imm,rs,funct,rd,opcode=0x13):return ((imm&4095)<<20)|(rs<<15)|(funct<<12)|(rd<<7)|opcode
def r(rs2,rs1,funct,rd,fn=0):return (fn<<25)|(rs2<<20)|(rs1<<15)|(funct<<12)|(rd<<7)|0x33
def s(rs2,rs1,funct=0):return (rs2<<20)|(rs1<<15)|(funct<<12)|0x23
def b(offset,rs2,rs1,funct):
    u=offset&8191
    return ((u>>12)<<31)|(((u>>5)&63)<<25)|(rs2<<20)|(rs1<<15)|(funct<<12)|(((u>>1)&15)<<8)|(((u>>11)&1)<<7)|0x63
def uart(words,text):
    for byte in text.encode():words.extend([i(byte,0,0,29),s(29,28)])
def build(body,iterations):
    words=[(16<<12)|(10<<7)|0x17,i(1,0,0,1),i(7,0,0,2),i(13,0,0,3),(0x10000<<12)|(28<<7)|0x37]
    hi=(iterations+2048)>>12;lo=iterations-(hi<<12)
    words.extend([(hi<<12)|(30<<7)|0x37,i(lo,30,0,30)])
    uart(words,'\nNanoRV_BARE_START\n')
    start=len(words)
    words.extend(body);words.append(i(-1,30,0,30));words.append(b((start-len(words))*4,0,30,1))
    uart(words,'\nNanoRV_BARE_DONE\n')
    uart(words,'CHECKSUM=')
    for reg in (1,2,3):
        for shift in range(60,-1,-4):
            words.extend([i(shift,reg,5,29),i(15,29,7,29),i(10,29,3,26),
                          r(26,0,0,26,32),i(-39,26,7,26),i(87,29,0,29),r(26,29,0,29),s(29,28)])
        uart(words,' ')
    uart(words,'\n')
    words.extend([(0x100<<12)|(28<<7)|0x37,(5<<12)|(29<<7)|0x37,i(0x555,29,0,29),s(29,28,2),0x10500073])
    return words
def emit(name,words):
    path=pathlib.Path(__file__).resolve().parent.parent/a.out
    path.mkdir(parents=True,exist_ok=True)
    (path/(name+'.bin')).write_bytes(struct.pack('<'+'I'*len(words),*words))
out=pathlib.Path(__file__).resolve().parent.parent/a.out
out.mkdir(parents=True,exist_ok=True)
manifest={}
for k in (6,14,30,62,126):
    total=4_096_000_000;block=k+2
    if total%block:raise RuntimeError('fus block does not divide total')
    iterations=total//block
    words=build([i(1,1,0,1)]*k,iterations)
    emit(f'fus-k{k:03d}-b{block:03d}',words)
    manifest[f'fus-k{k:03d}-b{block:03d}']={'block':block,'iterations':iterations,'total_insns':block*iterations,'x1_expected':hex(1+k*iterations)}
for n in (2,6,10,42):
    total=256_000_000;block=3*n+2
    if total%block:raise RuntimeError('mem block does not divide total')
    iterations=total//block
    words=build([i(0,10,3,1,3),i(1,1,0,1),s(1,10,3)]*n,iterations)
    emit(f'mem-n{n:03d}-b{block:03d}',words)
    manifest[f'mem-n{n:03d}-b{block:03d}']={'block':block,'iterations':iterations,'total_insns':block*iterations,'x1_expected':hex(n*iterations)}
mixchain=[i(17,1,0,1),r(2,1,4,1),i(3,2,1,2),r(1,2,0,2),i(1,1,5,1),i(-17,2,7,2)]
for k in (1,3,5,21):
    total=1_024_000_000;block=6*k+2
    if total%block:raise RuntimeError('mix block does not divide total')
    iterations=total//block
    words=build(mixchain*k,iterations)
    emit(f'mix-k{k:03d}-b{block:03d}',words)
    manifest[f'mix-k{k:03d}-b{block:03d}']={'block':block,'iterations':iterations,'total_insns':block*iterations}
(out/'manifest.json').write_text(__import__('json').dumps(manifest,indent=2),encoding='utf-8')
print(__import__('json').dumps(manifest,indent=2))
