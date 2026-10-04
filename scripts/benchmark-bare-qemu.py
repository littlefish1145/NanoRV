"""Identical finite RV64 programs on native JIT and QEMU TCG.

Contrasts fusible arithmetic with dependent mixed operations and memory. Host
START/DONE UART timestamps exclude process startup; final register checksums
must agree. No guest-clock scores or QEMU icount timing are used.
"""
import argparse,hashlib,json,pathlib,re,statistics,struct,subprocess,threading,time
from host_affinity import apply_affinity
p=argparse.ArgumentParser()
p.add_argument('--exe',default='build/vulkan17/bin/Release/nanorv.exe')
p.add_argument('--baseline-exe',help='include the previous NanoRV executable in the alternating runs')
p.add_argument('--iterations',type=int,default=8000000)
p.add_argument('--rounds',type=int,default=3)
p.add_argument('--affinity',type=lambda v:int(v,0))
p.add_argument('--prefix',default='build/bare-qemu-vulkan17')
p.add_argument('--min-seconds',type=float,default=0.02,help='reject unresolvable or buffered UART timings')
p.add_argument('--require-qemu-fraction',type=float,help='fail if any tested workload misses this throughput fraction')
a=p.parse_args()
if not 1<=a.iterations<1<<30:p.error('iterations must be between 1 and 2^30-1')
if a.rounds<1:p.error('--rounds must be positive')
if a.min_seconds<=0:p.error('--min-seconds must be positive')
if a.require_qemu_fraction is not None and not 0<a.require_qemu_fraction<=1:p.error('--require-qemu-fraction must be in (0,1]')
root=pathlib.Path(__file__).resolve().parent.parent
(root/a.prefix).parent.mkdir(parents=True,exist_ok=True)
def i(imm,rs,funct,rd,opcode=0x13):return ((imm&4095)<<20)|(rs<<15)|(funct<<12)|(rd<<7)|opcode
def r(rs2,rs1,funct,rd,fn=0):return (fn<<25)|(rs2<<20)|(rs1<<15)|(funct<<12)|(rd<<7)|0x33
def s(rs2,rs1,funct=0):return (rs2<<20)|(rs1<<15)|(funct<<12)|0x23
def b(offset,rs2,rs1,funct):
    u=offset&8191
    return ((u>>12)<<31)|(((u>>5)&63)<<25)|(rs2<<20)|(rs1<<15)|(funct<<12)|(((u>>1)&15)<<8)|(((u>>11)&1)<<7)|0x63
def uart(words,text):
    for byte in text.encode():words.extend([i(byte,0,0,29),s(29,28)])
programs={'fusible-add':[i(1,1,0,1)]*30,
          'mixed-dependent':[i(17,1,0,1),r(2,1,4,1),i(3,2,1,2),r(1,2,0,2),i(1,1,5,1),i(-17,2,7,2)]*5,
          'memory-dependent':[i(0,10,3,1,3),i(1,1,0,1),s(1,10,3)]*10}
records=[]
backends={'nanorv':root/a.exe,'qemu':root/'xpack-qemu-riscv-9.2.4-1/bin/qemu-system-riscv64.exe'}
if a.baseline_exe:backends={'baseline':root/a.baseline_exe,**backends}
for name,body in programs.items():
    iterations=a.iterations*(16 if name=='fusible-add' else 4 if name=='mixed-dependent' else 1)
    if iterations>=1<<30:raise RuntimeError('scaled program iteration count exceeds signed immediate range')
    # AUIPC avoids RV64 LUI sign extension for the physical 0x80010000 page.
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
    # End the CPU batch at WFI after the poweroff request, so the native device
    # loop observes shutdown before fetching beyond the generated program.
    words.extend([(0x100<<12)|(28<<7)|0x37,(5<<12)|(29<<7)|0x37,i(0x555,29,0,29),s(29,28,2),0x10500073])
    file=root/f'{a.prefix}-{name}.bin';file.write_bytes(struct.pack('<'+'I'*len(words),*words))
    expected=None
    for round_index in range(a.rounds):
        for backend in (list(backends) if round_index%2==0 else list(reversed(backends))):
            cmd=[str(backends[backend]),'--raw',str(file),'-m','1G','--engine','jit','--no-reg-dump'] if backend!='qemu' else [
                str(backends[backend]),'-machine','virt','-bios','none',
                '-m','1G','-smp','1','-nographic','-monitor','none','-nic','none',
                '-device',f'loader,file={file},addr=0x80000000,cpu-num=0,force-raw=on']
            proc=subprocess.Popen(cmd,cwd=root,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            data=bytearray();marks={};error=[]
            def read_output():
                import os
                tail=b''
                # Windows CRT pipe reads can wait for the requested byte count
                # (or EOF). Byte reads keep START and DONE in separate timing
                # observations even when a fast JIT produces very little UART.
                while chunk:=os.read(proc.stdout.fileno(),1):
                    observed=time.perf_counter();data.extend(chunk);combined=tail+chunk
                    for marker in [b'NanoRV_BARE_START\r\n',b'NanoRV_BARE_DONE\r\n',b'NanoRV_BARE_START\n',b'NanoRV_BARE_DONE\n']:
                        if marker in combined:marks.setdefault('start' if b'START' in marker else 'done',observed)
                    tail=combined[-80:]
            def read_error():error.append(proc.stderr.read())
            if a.affinity:apply_affinity(proc,a.affinity)
            output_thread=threading.Thread(target=read_output);error_thread=threading.Thread(target=read_error)
            output_thread.start();error_thread.start()
            try:proc.wait(timeout=180)
            finally:
                if proc.poll() is None:proc.terminate();proc.wait()
                output_thread.join();error_thread.join()
            transcript=bytes(data).decode(errors='replace')+b''.join(error).decode(errors='replace')
            (root/f'{a.prefix}-{name}-{backend}-{round_index+1}.log').write_text(transcript,encoding='utf-8')
            if proc.returncode or set(marks)!={'start','done'}:raise RuntimeError(f'{name}/{backend} failed: '+transcript[-3000:])
            checksum=re.search(r'CHECKSUM=([0-9a-f]{16}) ([0-9a-f]{16}) ([0-9a-f]{16})',transcript)
            if not checksum:raise RuntimeError('checksum missing')
            values=checksum.groups()
            if expected is None:expected=values
            if values!=expected:raise RuntimeError('native/QEMU checksum differs')
            if name=='fusible-add' and int(values[0],16)!=1+30*iterations:raise RuntimeError('arithmetic result mismatch')
            if name=='memory-dependent' and int(values[0],16)!=10*iterations:raise RuntimeError('memory result mismatch')
            seconds=marks['done']-marks['start']
            if seconds<a.min_seconds:raise RuntimeError('workload timing is too short or UART output is buffered; increase iterations or use an NanoRV build with line flushing')
            instructions=iterations*(len(body)+2)
            record={'program':name,'backend':backend,'round':round_index+1,'seconds':seconds,
                    'guest_work_instructions':instructions,'ips':instructions/seconds,'checksum':values}
            records.append(record);print(json.dumps(record),flush=True)
summary={name:{backend:statistics.median(r['seconds'] for r in records if r['program']==name and r['backend']==backend)
               for backend in backends} for name in programs}
fractions={name:values['qemu']/values['nanorv'] for name,values in summary.items()}
passed=a.require_qemu_fraction is None or all(value>=a.require_qemu_fraction for value in fractions.values())
(root/(a.prefix+'.json')).write_text(json.dumps({'iterations':a.iterations,'rounds':a.rounds,
    'timing':'host START/DONE UART arrival','affinity':hex(a.affinity) if a.affinity else None,
    'executables':{key:{'path':str(path),'sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
       for key,path in backends.items()},
    'records':records,'median_seconds':summary,'throughput_fraction_of_qemu':fractions,
    'required_fraction':a.require_qemu_fraction,'target_passed':passed},indent=2),encoding='utf-8')
print('Median seconds: '+json.dumps(summary),flush=True)
print('Throughput fraction of QEMU: '+json.dumps(fractions),flush=True)
if not passed:raise SystemExit(2)
