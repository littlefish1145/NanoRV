"""Boot the Debian disk and run commands over the native VM's serial console."""
import argparse, pathlib, subprocess, threading, time, re, os
from host_affinity import apply_affinity
p = argparse.ArgumentParser()
p.add_argument('--exe', default='build/performance-final/bin/Release/rvvm.exe')
p.add_argument('--timeout', type=int, default=1200)
p.add_argument('--log', default='build/debian-boot.log')
p.add_argument('--initrd', default='assets/debian-initrd.cpio')
p.add_argument('--disk', default='assets/disk.raw')
p.add_argument('--qemu', action='store_true')
p.add_argument('--qemu-wsl', action='store_true')
p.add_argument('--network', choices=['user','none'], help='Windows xPack QEMU has no user-network backend; use none for a matched CPU benchmark')
p.add_argument('--engine', default='jit')
p.add_argument('--password', default='rvvm')
p.add_argument('--smp', type=int, default=1)
p.add_argument('--smp-mode', choices=['parallel','cooperative'], default='parallel')
p.add_argument('--forward',action='append',default=[],help='native loopback TCP host:guest forward')
p.add_argument('--affinity',type=lambda value:int(value,0),help='Windows test process CPU affinity mask, e.g. 0xc03c03 on this host')
p.add_argument('--memory',default='1G')
p.add_argument('--display', choices=['headless','window'])
p.add_argument('--gpu',choices=['2d','virgl'])
p.add_argument('--display-backend',choices=['auto','gdi','vulkan'])
p.add_argument('--host-gpu',choices=['auto','nvidia','intel','amd'])
p.add_argument('--screenshot')
p.add_argument('--shell',action='store_true',help='diagnostic Debian shell without systemd')
p.add_argument('--append-extra',default='',help='additional native guest kernel arguments')
p.add_argument('--command', action='append')
p.add_argument('--reboot-check', action='store_true')
p.add_argument('--after-reboot-command', action='append')
p.add_argument('--precise-timing',action='store_true',help='time guest START/DONE UART markers, excluding serial command typing')
a = p.parse_args()
if a.affinity and a.qemu_wsl:p.error('--affinity cannot pin the QEMU server through the WSL client')
root = pathlib.Path(__file__).resolve().parent.parent
cmd = [str(root/a.exe), '--bios','assets/fw_jump.bin','--kernel','assets/linux',
       '--initrd',a.initrd,'--disk',a.disk,'--net',a.network or 'user','-m',a.memory,
       '--append','console=ttyS0 earlycon=sbi root=/dev/vda1 rw loglevel=6 systemd.firstboot=no', '--no-reg-dump','--engine',a.engine,'--smp',str(a.smp),'--smp-mode',a.smp_mode]
if a.network == 'none':
    del cmd[cmd.index('--net'):cmd.index('--net')+2]
if a.display: cmd+=['--display',a.display]
if a.gpu: cmd+=['--gpu',a.gpu]
if a.screenshot: cmd+=['--screenshot',a.screenshot]
if a.display_backend:cmd+=['--display-backend',a.display_backend]
if a.host_gpu:cmd+=['--host-gpu',a.host_gpu]
if a.shell: cmd[cmd.index('--append')+1]+=' rvvm.shell=1'
if a.append_extra: cmd[cmd.index('--append')+1]+=' '+a.append_extra
for forward in a.forward:cmd+=['--forward',forward]
if a.qemu or a.qemu_wsl:
    if a.display_backend or a.host_gpu:p.error('host presentation/GPU selection options are native-only')
    if a.forward:p.error('--forward is currently supported only by the native test runner')
    cmd = [str(root/'xpack-qemu-riscv-9.2.4-1/bin/qemu-system-riscv64.exe'),
           '-machine','virt','-m','1G','-smp',str(a.smp),'-nographic','-monitor','none',
           '-bios','assets/fw_jump.bin','-kernel','assets/linux','-initrd',a.initrd,
           '-drive','file='+a.disk+',format=raw,if=none,id=disk',
           '-device','virtio-blk-device,drive=disk','-nic','none',
           '-append','console=ttyS0 earlycon=sbi root=/dev/vda1 rw loglevel=6 systemd.firstboot=no']
    if a.qemu_wsl:
        cmd[0] = 'qemu-system-riscv64'
    if (a.network or ('user' if a.qemu_wsl else 'none')) == 'user':
        cmd[cmd.index('-nic'):cmd.index('-nic')+2] = ['-netdev','user,id=net','-device','virtio-net-device,netdev=net']
    if a.qemu_wsl:
        cmd = ['wsl','-d','Ubuntu-24.04','--'] + cmd
    if a.display: cmd+=['-global','virtio-mmio.force-legacy=false','-device','virtio-gpu-device','-device','virtio-keyboard-device','-display','none']
    if a.shell: cmd[cmd.index('-append')+1]+=' rvvm.shell=1'
    if a.append_extra: cmd[cmd.index('-append')+1]+=' '+a.append_extra
log = root/a.log
proc = subprocess.Popen(cmd, cwd=root, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
if a.affinity:
    try:apply_affinity(proc,a.affinity)
    except Exception:
        proc.terminate();proc.wait();raise
    print('Benchmark host affinity:',hex(a.affinity),flush=True)
data = bytearray()
command_timings={}
marker_pattern=re.compile(rb'\r?\nRVVM_BENCH_(START|DONE)_([0-9]+)(?:=([0-9]+))?\r?\n')
command_ids=iter(range(1000000))
def reader():
    tail=b''
    with log.open('wb') as file:
        while c := os.read(proc.stdout.fileno(),65536):
            data.extend(c); file.write(c); file.flush()
            if a.precise_timing:
                observed=time.perf_counter()
                chunk=tail+c
                for marker in marker_pattern.finditer(chunk):
                    command_timings.setdefault((marker[1].decode(),int(marker[2])),observed)
                tail=chunk[-160:]
t = threading.Thread(target=reader, daemon=True); t.start()
started = time.monotonic()
deadline = started + a.timeout
def send(line):
    for byte in (line+'\n').encode():
        proc.stdin.write(bytes([byte])); proc.stdin.flush(); time.sleep(.01)
def wait_for(marker, after=0):
    while proc.poll() is None and time.monotonic() < deadline:
        if any(failure in data for failure in [b'RVVM: root mount failed',b'RVVM: finit_module failed',b'Kernel panic -']):
            raise RuntimeError('guest boot failed; see UART transcript')
        if marker in data[after:]: return
        time.sleep(.1)
    raise RuntimeError('guest exited or timed out waiting for '+repr(marker))
def login(after=0):
    if a.shell:
        wait_for(b'RVVM_SHELL# ',after)
        print('Debian diagnostic shell:',round(time.monotonic()-started,2),'seconds',flush=True)
        return
    wait_for(b'login:',after)
    send('root')
    wait_for(b'Password:',after)
    send(a.password)
    wait_for(b'root@',after)
    print('Debian console login:',round(time.monotonic()-started,2),'seconds',flush=True)
    time.sleep(1)

def run_commands(commands):
    for index, command in enumerate(commands):
        command_started=time.monotonic()
        before = len(data)
        token = 'RVVM_COMMAND_%d_DONE' % index
        timing_id=next(command_ids)
        if a.precise_timing:
            send('printf "\\nRVVM_BENCH_START_'+str(timing_id)+'\\n"; '+command+
                 '; rc=$?; printf "\\nRVVM_BENCH_DONE_'+str(timing_id)+'=%s\\n'+token+'=%s\\n" "$rc" "$rc"')
        else:
            send(command+'; rc=$?; printf "\\n'+token+'=%s\\n" "$rc"')
        wait_for(('\n'+token+'=').encode(), before)
        expression = re.compile(('\\n'+token+'=([0-9]+)\\r?\\n').encode())
        while not (match := expression.search(bytes(data[before:]))):
            if proc.poll() is not None or time.monotonic() >= deadline: raise RuntimeError('missing command result')
            time.sleep(.05)
        code = int(match.group(1))
        elapsed=time.monotonic()-command_started
        if a.precise_timing:
            start=command_timings.get(('START',timing_id));end=command_timings.get(('DONE',timing_id))
            if start is None or end is None:raise RuntimeError('missing precise UART timestamps')
            elapsed=end-start
        print(command, 'exit',code,'host seconds',round(elapsed,4 if a.precise_timing else 2), flush=True)
        if code: raise RuntimeError('guest command failed: '+command)

try:
    if a.precise_timing:print('Timing: START/DONE UART timestamps; serial command typing excluded',flush=True)
    login()
    if a.shell and a.network != 'none':
        run_commands(['ip link set eth0 up && ip addr add 10.0.2.15/24 dev eth0 && ip route add default via 10.0.2.2 && mkdir -p /run/systemd/resolve && printf "nameserver 10.0.2.3\\n" > /etc/resolv.conf'])
    run_commands(a.command or ['uname -a','cat /etc/debian_version','findmnt /',
                              'ip -brief address','date -u','echo RVVM_DEBIAN_OK'])
    if a.reboot_check:
        before=len(data);send('reboot');login(before)
        run_commands(a.after_reboot_command or ['findmnt /','ip -brief address','echo RVVM_REBOOT_OK'])
    send('sync; poweroff -ff' if a.shell else 'poweroff')
    # A guest second is not a host second on an interpreter or under SMP.
    proc.wait(timeout=max(90,deadline-time.monotonic()))
    if proc.returncode: raise RuntimeError('VM exited with '+str(proc.returncode))
    print('Exit:', proc.returncode, 'Wall seconds:', round(time.monotonic()-started, 2))
finally:
    if proc.poll() is None: proc.terminate()
    t.join(timeout=3)
    print(bytes(data[-8000:]).decode(errors='replace'))
    print('Transcript:', log)
