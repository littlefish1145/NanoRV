"""Fetch pinned VcXsrv from its official SourceForge mirrors, without installing it."""
import concurrent.futures, hashlib, pathlib, subprocess, time, urllib.request
root=pathlib.Path(__file__).resolve().parent.parent
target=root/'third_party/vcxsrv'
target.mkdir(parents=True,exist_ok=True)
parts=root/'build/vcxsrv-parts'
parts.mkdir(exist_ok=True)
size=42865185
chunk=262144
filename='vcxsrv-64.1.20.14.0.installer.exe'
def download(index):
    start=index*chunk;end=min(size,start+chunk)-1
    part=parts/str(index)
    if part.exists() and part.stat().st_size==end-start+1:return part
    for attempt in range(5):
        mirror=('netix','phoenixnap')[attempt%2]
        url=f'https://{mirror}.dl.sourceforge.net/project/vcxsrv/vcxsrv/1.20.14.0/{filename}'
        try:
            request=urllib.request.Request(url,headers={'Range':f'bytes={start}-{end}'})
            with urllib.request.urlopen(request,timeout=60) as response:
                if response.status!=206 or response.headers['Content-Range']!=f'bytes {start}-{end}/{size}':
                    raise RuntimeError('unexpected HTTP range')
                data=response.read()
            if len(data)!=end-start+1:raise RuntimeError('short HTTP range')
            part.write_bytes(data);return part
        except Exception:
            if attempt==4:raise
            time.sleep(1)
installer=target/'installer.exe'
with concurrent.futures.ThreadPoolExecutor(max_workers=24) as pool:
    files=list(pool.map(download,range((size+chunk-1)//chunk)))
with installer.open('wb') as output:
    for part in files:output.write(part.read_bytes())
digest=hashlib.sha256(installer.read_bytes()).hexdigest()
if digest!='7a5cd7d9d0ac982f09c8a93051fc48ab072fcc4b52237d1b91494363b9dad27e':raise RuntimeError('VcXsrv SHA-256 mismatch')
print('Downloaded official VcXsrv 1.20.14.0 SHA-256:',digest,flush=True)
subprocess.run([r'C:\Program Files\7-Zip\7z.exe','x','-y',f'-o{target}',str(installer)],check=True,stdout=subprocess.DEVNULL)
print('Extracted portable runtime:',target,flush=True)
