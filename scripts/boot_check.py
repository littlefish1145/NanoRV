"""Run a guest, wait for its shell, and save an auditable UART transcript."""
import argparse, hashlib, pathlib, subprocess, threading, time, os
p = argparse.ArgumentParser()
p.add_argument('--qemu', action='store_true')
p.add_argument('--exe', default='build/performance-final/bin/Release/rvvm.exe')
p.add_argument('--initrd', default='assets/initrd.cpio')
p.add_argument('--engine', default='jit')
p.add_argument('--timeout', type=int, default=600)
p.add_argument('--log', default='build/boot.log')
a = p.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
cmdline = 'console=ttyS0 earlycon=sbi rdinit=/bin/sh loglevel=7 TERM=dumb'
if a.qemu:
    cmd = [str(root/'xpack-qemu-riscv-9.2.4-1/bin/qemu-system-riscv64.exe'), '-machine','virt','-m','512M','-smp','1','-nographic','-monitor','none','-bios','assets/fw_jump.bin','-kernel','assets/linux','-initrd','assets/initrd.gz','-append',cmdline]
else:
    cmd = [str(root/a.exe),'--bios','assets/fw_jump.bin','--kernel','assets/linux','--initrd',a.initrd,'-m','512M','--append',cmdline,'--no-reg-dump','--engine',a.engine]
log = root/a.log
log.parent.mkdir(parents=True, exist_ok=True)
proc = subprocess.Popen(cmd,cwd=root,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
data = bytearray()
def reader():
    with log.open('wb') as f:
        while True:
            c = os.read(proc.stdout.fileno(),65536)
            if not c: break
            data.extend(c); f.write(c); f.flush()
t = threading.Thread(target=reader,daemon=True); t.start()
deadline = time.monotonic()+a.timeout
started = time.monotonic()
sent = False
success = False
payload = b'RVVM guest filesystem check\n'
expected_hash = hashlib.sha256(payload).hexdigest().encode()
commands = [
    b'uname -a\n', b'mount -t proc proc /proc\n', b'cat /proc/cpuinfo\n',
    b'mkdir -p /tmp\n', b"printf 'RVVM guest filesystem check\\n' > /tmp/rvvm-check\n",
    b'sha256sum /tmp/rvvm-check\n',
    b"awk 'BEGIN { printf \"FLOAT=%.3f\\n\", 1.5*2.25+0.625 }'\n",
    b'i=0; while [ "$i" -lt 100 ]; do i=$((i + 1)); done; echo LOOP=$i\n',
    b'sleep 1; echo TIMER_OK\n', b'echo RVVM_BOOT_OK\n',
]
def verified():
    text = bytes(data).replace(b'\r',b'')
    return (b'\nRVVM_BOOT_OK\n' in text and expected_hash in text and b'FLOAT=4.000' in text
            and b'LOOP=100' in text and b'\nTIMER_OK\n' in text and b'6.12.107+deb13-riscv64' in text)
try:
    while proc.poll() is None and time.monotonic()<deadline:
        if not sent and (b'/ #' in data or b'~ #' in data):
            time.sleep(1)
            for line in commands:
                before = len(data)
                for byte in line:
                    proc.stdin.write(bytes([byte])); proc.stdin.flush(); time.sleep(.03)
                command_deadline = min(deadline, time.monotonic()+60)
                while proc.poll() is None and time.monotonic()<command_deadline:
                    tail = bytes(data[before:])
                    if b'~ # ' in tail or b'/ # ' in tail: break
                    time.sleep(.1)
                else: raise RuntimeError('shell did not finish command: '+line.decode())
            sent=True
        if verified():
            for byte in b'poweroff -f\n':
                proc.stdin.write(bytes([byte]));proc.stdin.flush();time.sleep(.03)
            proc.wait(timeout=90)
            if proc.returncode: raise RuntimeError('guest poweroff failed')
            success = True
            print('PASS: Linux shell, procfs, guest file SHA-256, floating-point awk, loop and timer')
            print('Poweroff exit code:',proc.poll())
            break
        time.sleep(.1)
    else:
        print('FAIL: guest exited or timeout before shell verification')
    print(bytes(data[-6000:]).decode(errors='replace'))
finally:
    if proc.poll() is None: proc.terminate()
    proc.wait(); t.join(3)
print('Wall time:',round(time.monotonic()-started,2),'seconds')
print('Transcript:',log)
raise SystemExit(0 if success else 1)
