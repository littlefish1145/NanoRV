"""Fetch WSL build packages through Windows networking using APT's hashes.

Keeps build downloads outside guest disks. Does not change WSL repository lists
or foreign architectures; package choices come from its existing signed index.
"""
import concurrent.futures, hashlib, pathlib, re, subprocess, urllib.parse, urllib.request
packages=['g++-riscv64-linux-gnu','python3-ply','bison','flex']
directory=pathlib.Path(r'C:\Users\fish_\AppData\Local\Temp\opencode\vulkan-build-debs')
directory.mkdir(parents=True,exist_ok=True)
def wsl(*args):
    return subprocess.check_output(['wsl','-d','Ubuntu-24.04','-u','root','--',*args],encoding='utf-8',errors='replace')
uris=wsl('apt-get','--print-uris','--yes','--download-only','install',*packages)
entries=re.findall(r"^'([^']+)'\s+(\S+)\s+(\d+)\s+MD5Sum:([0-9a-f]+)$",uris,re.M)
names=sorted({urllib.parse.unquote(name).split('_')[0] for _,name,_,_ in entries})
metadata=wsl('apt-cache','show',*names)
hashes={}
for block in re.split(r'\n\s*\n',metadata):
    fields=dict(line.split(': ',1) for line in block.splitlines() if ': ' in line and not line.startswith(' '))
    if 'Filename' in fields and 'SHA256' in fields:hashes[fields['Filename']]=fields['SHA256']
def fetch(entry):
    url,name,length,_=entry
    key=urllib.parse.unquote(urllib.parse.urlsplit(url).path).removeprefix('/ubuntu/')
    expected=hashes.get(key)
    if not expected:raise RuntimeError(f'APT SHA256 missing: {key}')
    # Keep dpkg's URI-escaped package filename. Debian versions contain ':',
    # which is not legal in a Windows path; '+' and '%3a' are valid filename
    # characters and dpkg selects the package by its control metadata.
    target=directory/name.replace(':','%3a')
    if target.exists() and hashlib.sha256(target.read_bytes()).hexdigest()==expected:return target
    url='https://archive.ubuntu.com/ubuntu/'+urllib.parse.quote(key,safe='/+~')
    with urllib.request.urlopen(url,timeout=90) as response:data=response.read()
    if len(data)!=int(length) or hashlib.sha256(data).hexdigest()!=expected:raise RuntimeError(f'Hash/size mismatch: {name}')
    target.write_bytes(data);print('Verified:',target.name,flush=True);return target
with concurrent.futures.ThreadPoolExecutor(max_workers=4) as executor:files=list(executor.map(fetch,entries))
if files:
    paths=['/mnt/c/'+str(file).replace('\\','/').split(':',1)[1].lstrip('/') for file in files]
    subprocess.run(['wsl','-d','Ubuntu-24.04','-u','root','--','dpkg','-i',*paths],check=True)
print('Vulkan build packages ready',flush=True)
