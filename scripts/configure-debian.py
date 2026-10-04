"""Run as root under WSL, with the VM stopped, to initialize the local disk."""
import pathlib, subprocess, tempfile, sys
root = pathlib.Path(__file__).resolve().parent.parent
disk = root/(sys.argv[1] if len(sys.argv)>1 else 'assets/disk.raw')
with tempfile.TemporaryDirectory(prefix='nanorv-disk-config-') as directory:
    subprocess.run(['mount','-o','loop,offset=134217728',str(disk),directory],check=True)
    try:
        guest = pathlib.Path(directory)
        shadow = guest/'etc/shadow'
        lines = shadow.read_text().splitlines()
        password = subprocess.check_output(['openssl','passwd','-6','nanorv'],text=True).strip()
        for index,line in enumerate(lines):
            if line.startswith('root:'):
                fields = line.split(':'); fields[1]=password; lines[index]=':'.join(fields)
        shadow.write_text('\n'.join(lines)+'\n')
        (guest/'etc/hostname').write_text('nanorv\n')
        (guest/'etc/locale.conf').write_text('LANG=C.UTF-8\n')
        (guest/'etc/timezone').write_text('Etc/UTC\n')
        # DHCP works regardless of the predictable interface name assigned by udev.
        network = guest/'etc/systemd/network/20-nanorv.network'
        network.parent.mkdir(parents=True,exist_ok=True)
        network.write_text('[Match]\nMACAddress=52:54:00:12:34:56\n\n[Network]\nDHCP=ipv4\n')
        subprocess.run(['sync'],check=True)
    finally: subprocess.run(['umount',directory],check=True)
print('Configured local Debian image:',disk,'console login root / nanorv')
