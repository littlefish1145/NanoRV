"""Build a newc desktop initrd using Python alone; no WSL needed at launch."""
import argparse, json, pathlib, stat, struct, re
p=argparse.ArgumentParser();p.add_argument('--session');p.add_argument('--output',required=True)
p.add_argument('--base',default='assets/debian-performance-initrd.cpio');p.add_argument('--serial-x11',action='store_true');a=p.parse_args()
root=pathlib.Path(__file__).resolve().parent.parent
data=(root/a.base).read_bytes()
records=[];offset=0
while offset+110<=len(data):
    header=data[offset:offset+110]
    if header[:6]!=b'070701':raise ValueError('expected newc archive')
    fields=[int(header[i:i+8],16) for i in range(6,110,8)]
    size,namesize=fields[6],fields[11]
    name=data[offset+110:offset+110+namesize-1].decode()
    start=(offset+110+namesize+3)&~3;payload=data[start:start+size]
    offset=(start+size+3)&~3
    if name=='TRAILER!!!':break
    records.append([name,fields,payload])
else:raise ValueError('missing cpio trailer')
inode=max(f[0] for _,f,_ in records)+1
def add(name,payload,mode):
    global inode
    fields=[inode,mode,0,0,1,0,len(payload),0,0,0,0,len(name.encode())+1,0]
    records.append([name,fields,payload]);inode+=1
commands='' if a.serial_x11 else '''
/bin/busybox cp /bin/rvvm-desktop-agent /newroot/usr/local/bin/rvvm-desktop-agent
/bin/busybox cp /bin/rvvm-setup-desktop /newroot/usr/local/bin/rvvm-setup-desktop
/bin/busybox chmod 755 /newroot/usr/local/bin/rvvm-desktop-agent /newroot/usr/local/bin/rvvm-setup-desktop
'''
add('./bin/rvvm-gui',(root/'scripts/gui-launch.sh').read_bytes(),stat.S_IFREG|0o755)
add('./bin/rvvm-gimp',b'#!/bin/sh\nexec /usr/local/bin/rvvm-gui /usr/bin/gimp "$@"\n',stat.S_IFREG|0o755)
commands+='''
/bin/busybox mkdir -p /newroot/usr/local/bin
/bin/busybox cp /bin/rvvm-gui /newroot/usr/local/bin/rvvm-gui
/bin/busybox chmod 755 /newroot/usr/local/bin/rvvm-gui
# Manage only our own wrapper; preserve a locally installed command.
if [ ! -e /newroot/usr/local/bin/gimp ] || /bin/busybox grep -q '/usr/local/bin/rvvm-gui /usr/bin/gimp' /newroot/usr/local/bin/gimp; then
  /bin/busybox cp /bin/rvvm-gimp /newroot/usr/local/bin/gimp
  /bin/busybox chmod 755 /newroot/usr/local/bin/gimp
fi
'''
for file,name in ([] if a.serial_x11 else [('desktop-agent.py','rvvm-desktop-agent'),('setup-desktop.sh','rvvm-setup-desktop')]):
    add('./bin/'+name,(root/'scripts'/file).read_bytes(),stat.S_IFREG|0o755)
if a.serial_x11:
    if not a.session:p.error('--serial-x11 requires --session')
    session=json.loads(pathlib.Path(a.session).read_text(encoding='utf-8-sig'))
    display=int(session['display_number']);cookie=session['cookie']
    if not 1<=display<=200 or not re.fullmatch('[0-9a-f]{32}',cookie):raise ValueError('invalid X11 session')
    fields=(b'',str(display).encode(),b'MIT-MAGIC-COOKIE-1',bytes.fromhex(cookie))
    authority=struct.pack('>H',65535)+b''.join(struct.pack('>H',len(f))+f for f in fields)
    add('./bin/rvvm-x11-authority',authority,stat.S_IFREG|0o600)
    profile='''# Managed RVVM serial-shell X11 session; applies only to this boot mode.
case " $(cat /proc/cmdline) " in
  *" rvvm.x11=1 "*)
    if [ -z "${DISPLAY:-}" ]; then
      rvvm_auth="$HOME/.Xauthority-rvvm"
      [ "$(id -u)" != 0 ] || rvvm_auth=/etc/rvvm-x11/Xauthority
      if [ -r "$rvvm_auth" ]; then
        export DISPLAY=10.0.2.2:DISPLAY_NUMBER XAUTHORITY="$rvvm_auth"
        export GDK_BACKEND=x11 QT_QPA_PLATFORM=xcb QT_X11_NO_MITSHM=1
      fi
      unset rvvm_auth
    fi
    ;;
esac
'''.replace('DISPLAY_NUMBER',str(display))
    add('./bin/rvvm-x11-profile',profile.encode(),stat.S_IFREG|0o644)
    commands+='''
/bin/busybox mkdir -p /newroot/etc/rvvm-x11 /newroot/etc/profile.d
/bin/busybox chmod 700 /newroot/etc/rvvm-x11
/bin/busybox cp /bin/rvvm-x11-authority /newroot/etc/rvvm-x11/Xauthority
/bin/busybox chmod 600 /newroot/etc/rvvm-x11/Xauthority
/bin/busybox cp /bin/rvvm-x11-profile /newroot/etc/profile.d/rvvm-x11.sh
/bin/busybox chmod 644 /newroot/etc/profile.d/rvvm-x11.sh
while IFS=: read -r user password uid gid gecos home shell; do
  case "$uid" in ''|*[!0-9]*) continue;; esac
  case "$home" in /*) ;; *) continue;; esac
  if [ "$uid" -ge 1000 ] && [ "$uid" -lt 65534 ] && [ -d "/newroot$home" ]; then
    /bin/busybox cp /bin/rvvm-x11-authority "/newroot$home/.Xauthority-rvvm"
    /bin/busybox chown "$uid:$gid" "/newroot$home/.Xauthority-rvvm"
    /bin/busybox chmod 600 "/newroot$home/.Xauthority-rvvm"
  fi
done < /newroot/etc/passwd
'''
elif a.session:
    session=json.loads(pathlib.Path(a.session).read_text(encoding='utf-8-sig'))
    add('./bin/rvvm-desktop-session.json',json.dumps(session).encode(),stat.S_IFREG|0o600)
    commands+='''/bin/busybox mkdir -p /newroot/etc/rvvm-desktop
/bin/busybox chmod 700 /newroot/etc/rvvm-desktop
/bin/busybox cp /bin/rvvm-desktop-session.json /newroot/etc/rvvm-desktop/session.json
/bin/busybox chmod 600 /newroot/etc/rvvm-desktop/session.json
'''
for record in records:
    if record[0].removeprefix('./')=='init':
        text=record[2].decode()
        marker='mount -o move /dev /newroot/dev'
        if marker not in text:raise ValueError('init insertion marker missing')
        record[2]=text.replace(marker,commands+'\n'+marker).encode();record[1][6]=len(record[2]);break
else:raise ValueError('missing init')
add('TRAILER!!!',b'',0)
output=root/a.output;output.parent.mkdir(parents=True,exist_ok=True)
with output.open('wb') as file:
    for name,fields,payload in records:
        file.write(b'070701'+b''.join(f'{v:08x}'.encode() for v in fields))
        file.write(name.encode()+b'\0');file.write(b'\0'*((-file.tell())%4))
        file.write(payload);file.write(b'\0'*((-file.tell())%4))
    file.write(b'\0'*((-file.tell())%512))
print('Desktop initrd:',output)
