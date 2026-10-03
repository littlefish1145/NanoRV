"""Audit actual generated x64 code; body counts are static, not whole-VM IPC."""
import argparse,collections,json,pathlib,re,subprocess
p=argparse.ArgumentParser()
p.add_argument('--exe',required=True)
p.add_argument('--output',required=True)
p.add_argument('--objdump',default='D:/LLVM/bin/llvm-objdump.exe')
a=p.parse_args()
directory=pathlib.Path(a.output);directory.mkdir(parents=True,exist_ok=True)
metadata=subprocess.check_output([a.exe,str(directory)],text=True)
records=[]
for line in metadata.splitlines():
    record=json.loads(line)
    assembly=subprocess.check_output([a.objdump,'-d','--x86-asm-syntax=intel',str(directory/(record['case']+'.obj'))],text=True)
    (directory/(record['case']+'.asm')).write_text(assembly,encoding='utf-8')
    decoded=[];instructions={}
    for match in re.finditer(r'^\s*([0-9a-f]+):\s+(?:[0-9a-f]{2}\s+)+\s*(\S+)\s*([^\r\n]*)',assembly,re.M):
        address=int(match[1],16)
        instructions[address]=(match[2],match[3])
        if record['body_offset']<=address<record['body_offset']+record['body_size']:
            decoded.append((match[2],match[3]))
    if not decoded or any(op in ('<unknown>','(bad)') for op,_ in decoded):raise RuntimeError('body decoding failed')
    record['body_host_instructions']=len(decoded)
    record['static_body_expansion']=len(decoded)/record['guest_instructions']
    record['body_mnemonics']=dict(collections.Counter(op for op,_ in decoded))
    if record['case']=='self-loop-fusible':
        # Trace this particular BNEZ sample under stated hot-loop assumptions:
        # guest branch taken, a valid chain, enough budget, unchanged code.
        # This is static path analysis, not a measured hardware instruction count.
        addresses=sorted(instructions);following=dict(zip(addresses,addresses[1:]))
        address=record['body_offset'];path=[]
        for _ in range(len(instructions)):
            op,operands=instructions[address];path.append(address)
            taken=op=='jmp' or (op=='jne' and address<record['body_offset']+record['body_size'])
            if taken:
                target=re.match(r'0x([0-9a-f]+)',operands)
                if not target:raise RuntimeError('unexpected indirect jump in sample loop')
                address=int(target[1],16)
                if address==record['body_offset']:break
            else:
                if op in ('call','ret'):raise RuntimeError('sample loop did not reach backedge')
                address=following[address]
        else:raise RuntimeError('sample loop path exceeded code bounds')
        record['hot_loop_host_instructions_with_guards']=len(path)
        record['hot_loop_assumptions']='Guest BNEZ taken, valid chain, enough instruction budget, unchanged code; excludes entry/exit.'
    records.append(record)
report={'scope':'Static emitted body excluding ABI entry/exit and final GPR flush. Memory bodies also include fallback paths; these are not dynamic retired instruction counts or energy measurements.','records':records}
(directory/'report.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print(json.dumps(report,indent=2))
