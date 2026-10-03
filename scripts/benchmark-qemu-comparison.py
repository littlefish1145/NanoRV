"""Matched fixed-workload comparison using host UART timestamps, not guest scores.

VMs run sequentially on separate inactive test disks. Each workload verifies its
iterations, CRC or decompressed data. CPU affinity applies only to child VMs.
"""
import argparse,hashlib,json,pathlib,re,statistics,subprocess,sys,time

p=argparse.ArgumentParser()
p.add_argument('--exe',default='build/perf16/bin/Release/rvvm.exe')
p.add_argument('--baseline-exe')
p.add_argument('--rounds',type=int,default=3)
p.add_argument('--affinity',type=lambda value:int(value,0))
p.add_argument('--prefix',default='build/qemu-comparison-16')
p.add_argument('--native-disk',default='build/debian-benchmark-native-final.raw')
p.add_argument('--qemu-disk',default='build/debian-benchmark-qemu.raw')
p.add_argument('--native-only',action='store_true')
p.add_argument('--raw-only',action='store_true',help='write raw JSON/validation logs, without a Markdown report')
p.add_argument('--require-qemu-fraction',type=float,help='fail unless every RVVM workload reaches this QEMU throughput fraction')
a=p.parse_args()
if a.rounds<1:p.error('--rounds must be positive')
if a.native_only and not a.raw_only:p.error('--native-only requires --raw-only')
if a.require_qemu_fraction is not None and (a.native_only or not 0<a.require_qemu_fraction<=1):p.error('--require-qemu-fraction needs QEMU and a fraction in (0,1]')
root=pathlib.Path(__file__).resolve().parent.parent
prefix=root/a.prefix
workloads=['coremark-1','coremark-4','dhrystone','compression']
backends={'rvvm':a.exe,'qemu':None}
if a.baseline_exe:backends={'baseline':a.baseline_exe,**backends}
if a.native_only:backends.pop('qemu')
records=[]
metadata={'created':time.strftime('%Y-%m-%d %H:%M:%S'),'rounds':a.rounds,
          'affinity':hex(a.affinity) if a.affinity else None,'vcpus':4,'memory':'1G',
          'network':'none','boot':'diagnostic shell, not full systemd',
          'timing':'host UART START/DONE arrival; serial typing excluded',
          'host_load':'Host is not isolated. No preexisting user processes are stopped by this benchmark.',
          'native_executables':{backend:{'path':path,'sha256':hashlib.sha256((root/path).read_bytes()).hexdigest()}
                                for backend,path in backends.items() if path},
          'assets':{name:hashlib.sha256((root/name).read_bytes()).hexdigest()
                    for name in ['assets/fw_jump.bin','assets/linux','build/debian-benchmark-initrd.cpio']}}
expected_hashes=None
for round_index in range(a.rounds):
    order=list(backends)
    if round_index%2:order.reverse()
    for backend in order:
        stem=f'{a.prefix}-{backend}-{round_index+1}'
        log=stem+'.log'
        cmd=[sys.executable,'scripts/debian_check.py','--shell','--precise-timing',
             '--network','none','--smp','4','--initrd','build/debian-benchmark-initrd.cpio',
             '--disk',a.qemu_disk if backend=='qemu' else a.native_disk,'--log',log]
        if backend=='qemu':cmd+=['--qemu']
        else:cmd+=['--exe',backends[backend]]
        if a.affinity:cmd+=['--affinity',hex(a.affinity)]
        cmd+=['--command','nproc; sha256sum /usr/local/bin/rvvm-coremark-1 /usr/local/bin/rvvm-coremark-4 /usr/local/bin/rvvm-dhrystone /usr/bin/bash']
        for workload in workloads:cmd+=['--command','rvvm-benchmark '+workload]
        print(f'Start {backend} round {round_index+1}/{a.rounds}',flush=True)
        with (root/(stem+'-result.txt')).open('w',encoding='utf-8') as result_file:
            result=subprocess.run(cmd,cwd=root,stdout=result_file,stderr=subprocess.STDOUT)
        output=(root/(stem+'-result.txt')).read_text(encoding='utf-8',errors='replace')
        transcript=(root/log).read_text(encoding='utf-8',errors='replace')
        if result.returncode:raise RuntimeError(f'{backend} failed; see {stem}-result.txt')
        hashes=dict((path,digest) for digest,path in re.findall(r'(?m)^([0-9a-f]{64})\s+(/(?:usr/local/bin/rvvm-(?:coremark-[14]|dhrystone)|usr/bin/bash))\r?$',transcript))
        if len(hashes)!=4:raise RuntimeError('guest benchmark hash metadata missing')
        if expected_hashes is None:expected_hashes=hashes
        if hashes!=expected_hashes:raise RuntimeError('guest binaries differ between runs')
        for marker in ['RVVM_COREMARK_CRC_OK threads=1 iterations_per_thread=10000',
                       'RVVM_COREMARK_CRC_OK threads=4 iterations_per_thread=10000',
                       'RVVM_DHRYSTONE_VALID=1 runs=1000000','RVVM_COMPRESSION_OK']:
            if marker not in transcript:raise RuntimeError(f'validation missing: {marker}')
        compression_hashes=re.findall(r'(?m)^([0-9a-f]{64})\s+/tmp/rvvm-compression.input\r?$',transcript)
        if compression_hashes!=['c21af8015213e6393c6b5c3c96f73a4203ab05f734fe75ad7b57ad078a9d0648']:
            raise RuntimeError('compression input differs from the fixed data set')
        measured={name:float(seconds) for name,seconds in re.findall(r'^rvvm-benchmark (\S+) exit 0 host seconds ([0-9.]+)$',output,re.M)}
        if set(measured)!=set(workloads):raise RuntimeError('missing workload timings')
        profile={}
        for line in transcript.splitlines():
            if 'JIT profile hart' in line:
                for name,value in re.findall(r'(\w+)=([0-9]+)',line):profile[name]=profile.get(name,0)+int(value)
        retired=re.search(r'rvvm: ([0-9]+) instructions in ([0-9.]+) s \(([0-9]+) ins/s\)',transcript)
        record={'backend':backend,'round':round_index+1,'seconds':measured,'profile':profile}
        if retired:record.update(vm_instructions=int(retired[1]),vm_seconds=float(retired[2]),aggregate_vm_ips=int(retired[3]))
        records.append(record)
        print(f'Finished {backend} round {round_index+1}: '+json.dumps(measured),flush=True)
        (root/(a.prefix+'-partial.json')).write_text(json.dumps({'metadata':metadata,'records':records},indent=2),encoding='utf-8')

medians={backend:{name:statistics.median(r['seconds'][name] for r in records if r['backend']==backend)
                  for name in workloads} for backend in backends}
ratios={backend:{name:medians[backend][name]/medians['qemu'][name] for name in workloads}
        for backend in backends if backend!='qemu'} if 'qemu' in medians else {}
report={'metadata':metadata,'guest_hashes':expected_hashes,'records':records,'median_seconds':medians,'time_ratio_to_qemu':ratios}
report['throughput_fraction_of_qemu']={name:1/ratio for name,ratio in ratios.get('rvvm',{}).items()}
report['required_fraction']=a.require_qemu_fraction
report['target_passed']=a.require_qemu_fraction is None or all(value>=a.require_qemu_fraction for value in report['throughput_fraction_of_qemu'].values())
(root/(a.prefix+'.json')).write_text(json.dumps(report,indent=2),encoding='utf-8')
if a.raw_only:
    print(json.dumps({'median_seconds':medians,'time_ratio_to_qemu':ratios},indent=2),flush=True)
    sys.exit(0 if report['target_passed'] else 2)
lines=['# Fixed-workload QEMU comparison','',f'{a.rounds} rounds; 4 vCPUs / 1 GiB / network disabled.',
       'Host UART timing excludes serial command typing. Host is not isolated; preexisting user processes are not stopped.','',
       '| Workload | RVVM host s | QEMU host s | RVVM/QEMU time |','|---|---:|---:|---:|']
for name in workloads:lines.append(f"| {name} | {medians['rvvm'][name]:.4f} | {medians['qemu'][name]:.4f} | {ratios['rvvm'][name]:.2f}x |")
lines+=['','These are fixed-workload host timings, not official CoreMark/DMIPS scores.',
        'Diagnostic-shell boot time and aggregate virtual instruction rates are not application performance scores.']
if a.baseline_exe:
    lines+=['','| Workload | Baseline host s | Current host s | Time reduction |','|---|---:|---:|---:|']
    for name in workloads:
        reduction=100*(1-medians['rvvm'][name]/medians['baseline'][name])
        lines.append(f"| {name} | {medians['baseline'][name]:.4f} | {medians['rvvm'][name]:.4f} | {reduction:.1f}% |")
(root/(a.prefix+'.md')).write_text('\n'.join(lines)+'\n',encoding='utf-8')
print(json.dumps({'median_seconds':medians,'time_ratio_to_qemu':ratios},indent=2),flush=True)
sys.exit(0 if report['target_passed'] else 2)
