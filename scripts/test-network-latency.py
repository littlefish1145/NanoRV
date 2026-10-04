"""Fixed persistent TCP echo workload through native SLIRP and VirtIO.

Use a free test disk; never point this runner at an image in use by another VM.
The serial runner logs in and powers off normally after the guest echo finishes.
"""
import argparse,pathlib,shlex,socket,statistics,subprocess,time
p=argparse.ArgumentParser()
p.add_argument('--exe',required=True)
p.add_argument('--disk',default='build/apt-fetch-fix.raw')
p.add_argument('--port',type=int,default=46002)
p.add_argument('--name',default='network-latency')
a=p.parse_args();root=pathlib.Path(__file__).resolve().parent.parent
log=root/'build'/f'{a.name}.log';result=root/'build'/f'{a.name}-guest-result.txt'
loop="""for i in range(100):
 data=b''
 while len(data)<64:
  part=c.recv(64-len(data))
  if not part: raise RuntimeError('early EOF')
  data+=part
 c.sendall(data)
"""
code=('import socket; s=socket.socket(); s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1); '
      's.bind(("0.0.0.0",46000)); s.listen(1); print("NanoRV_LATENCY_READY",flush=True); '
      'c,_=s.accept(); c.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1); '
      'exec('+repr(loop)+'); c.close(); s.close()')
command='python3 -u -c '+shlex.quote(code)
with result.open('w',encoding='utf-8') as output:
    process=subprocess.Popen(['python','scripts/debian_check.py','--exe',a.exe,'--disk',a.disk,
        '--initrd','assets/debian-performance-initrd.cpio','--smp','4','--shell','--timeout','900',
        '--log',str(log),'--forward',f'{a.port}:46000','--command',command],cwd=root,stdout=output,stderr=subprocess.STDOUT)
    deadline=time.monotonic()+900
    try:
        while not (log.exists() and b'\rNanoRV_LATENCY_READY\r\n' in log.read_bytes()):
            if process.poll() is not None:raise RuntimeError('guest exited before readiness; see '+str(result))
            if time.monotonic()>deadline:raise TimeoutError('guest echo readiness timeout')
            time.sleep(.2)
        samples=[];payload=bytes(range(64))
        with socket.create_connection(('127.0.0.1',a.port),timeout=10) as client:
            client.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1)
            for n in range(100):
                started=time.perf_counter();client.sendall(payload);response=b''
                while len(response)<len(payload):
                    part=client.recv(len(payload)-len(response))
                    if not part:raise RuntimeError('unexpected echo EOF')
                    response+=part
                if response!=payload:raise RuntimeError('echo payload mismatch')
                samples.append((time.perf_counter()-started)*1000)
        print(f'100 TCP echoes: median_ms={statistics.median(samples):.3f} p95_ms={sorted(samples)[94]:.3f}',flush=True)
        process.wait(timeout=180)
        if process.returncode:raise RuntimeError('guest runner failed; see '+str(result))
        print('Guest normal shutdown: PASS',flush=True)
    finally:
        # Only our independently launched test runner, never a user's VM.
        if process.poll() is None:process.terminate();process.wait(timeout=10)
