"""Short Windows thread-context sample of an existing NanoRV process.

This samples wall-time locations, not hardware CPU events. Threads are suspended
only for GetThreadContext and always resumed immediately, including on errors.
No guest RAM is written and no application or VM is started or stopped.
"""
import argparse
import bisect
import collections
import ctypes as c
from ctypes import wintypes as w
import json
import pathlib
import re
import struct
import sys
import time

p = argparse.ArgumentParser()
p.add_argument('pid', type=int)
p.add_argument('--seconds', type=float, default=12)
p.add_argument('--map', type=pathlib.Path)
p.add_argument('--mapped-image', type=pathlib.Path)
p.add_argument('--ips', nargs='*', type=lambda text: int(text, 0))
p.add_argument('--memory', action='store_true', help='count private executable regions without pausing threads')
a = p.parse_args()
if not 0 < a.seconds <= 30:
    p.error('--seconds must be in (0, 30]')
k = c.WinDLL('kernel32', use_last_error=True)
ps = c.WinDLL('psapi', use_last_error=True)

class ThreadEntry(c.Structure):
    _fields_ = [('size', w.DWORD), ('usage', w.DWORD), ('tid', w.DWORD),
                ('pid', w.DWORD), ('priority', w.LONG), ('delta', w.LONG), ('flags', w.DWORD)]

class MemoryInfo(c.Structure):
    _fields_ = [('base', c.c_void_p), ('allocation', c.c_void_p), ('allocation_protect', w.DWORD),
                ('partition', w.WORD), ('size', c.c_size_t), ('state', w.DWORD),
                ('protect', w.DWORD), ('type', w.DWORD)]

def api(dll, name, result, args):
    fn = getattr(dll, name); fn.restype = result; fn.argtypes = args; return fn

open_process = api(k, 'OpenProcess', w.HANDLE, [w.DWORD, w.BOOL, w.DWORD])
open_thread = api(k, 'OpenThread', w.HANDLE, [w.DWORD, w.BOOL, w.DWORD])
close = api(k, 'CloseHandle', w.BOOL, [w.HANDLE])
suspend = api(k, 'SuspendThread', w.DWORD, [w.HANDLE])
resume = api(k, 'ResumeThread', w.DWORD, [w.HANDLE])
context = api(k, 'GetThreadContext', w.BOOL, [w.HANDLE, c.c_void_p])
snapshot = api(k, 'CreateToolhelp32Snapshot', w.HANDLE, [w.DWORD, w.DWORD])
first = api(k, 'Thread32First', w.BOOL, [w.HANDLE, c.POINTER(ThreadEntry)])
next_thread = api(k, 'Thread32Next', w.BOOL, [w.HANDLE, c.POINTER(ThreadEntry)])
query = api(k, 'VirtualQueryEx', c.c_size_t, [w.HANDLE, c.c_void_p, c.POINTER(MemoryInfo), c.c_size_t])
mapped = api(ps, 'GetMappedFileNameW', w.DWORD, [w.HANDLE, c.c_void_p, w.LPWSTR, w.DWORD])
image_name = api(k, 'QueryFullProcessImageNameW', w.BOOL, [w.HANDLE, w.DWORD, w.LPWSTR, c.POINTER(w.DWORD)])
times = api(k, 'GetThreadTimes', w.BOOL, [w.HANDLE] + [c.POINTER(w.FILETIME)] * 4)

def cpu_time(handle):
    values = [w.FILETIME() for _ in range(4)]
    if not times(handle, *(c.byref(v) for v in values)): return 0
    return sum((v.dwHighDateTime << 32) | v.dwLowDateTime for v in values[2:]) / 1e7

process = open_process(0x410, False, a.pid)
if not process: raise c.WinError(c.get_last_error())
threads = {}
try:
    name = c.create_unicode_buffer(32768); length = w.DWORD(len(name))
    if not image_name(process, 0, name, c.byref(length)): raise c.WinError(c.get_last_error())
    if pathlib.Path(name.value).name.lower() != 'nanorv.exe': raise ValueError('PID must identify nanorv.exe')
    if a.memory:
        address = 0; regions = []; allocations = set(); mapped_regions=0
        while True:
            info = MemoryInfo()
            if not query(process, address, c.byref(info), c.sizeof(info)): break
            if info.state == 0x1000 and info.type in (0x20000,0x40000) and info.protect & 0xF0:
                regions.append(info.size); allocations.add(info.allocation)
                mapped_regions+=info.type==0x40000
            following = (info.base or 0) + info.size
            if following <= address: break
            address = following
        print(json.dumps({'pid': a.pid, 'binary': name.value,
                          'non_image_executable_regions': len(regions),
                          'non_image_executable_allocations': len(allocations),
                          'mapped_executable_regions': mapped_regions,
                          'committed_executable_bytes': sum(regions),
                          'region_sizes': dict(collections.Counter(regions))}, indent=2))
        sys.exit(0)
    symbols = []; symbol_addresses = []; whole_text_verified = False
    if a.map or a.mapped_image:
        if not a.map: p.error('--mapped-image requires --map')
        def text_section(path):
            data = path.read_bytes(); pe = struct.unpack_from('<I', data, 0x3c)[0]
            count = struct.unpack_from('<H', data, pe + 6)[0]
            size = struct.unpack_from('<H', data, pe + 20)[0]
            base = struct.unpack_from('<Q', data, pe + 24 + 24)[0]
            for n in range(count):
                section = pe + 24 + size + n * 40
                if data[section:section + 8].rstrip(b'\0') == b'.text':
                    virtual_size, rva, raw_size, raw_offset = struct.unpack_from('<4I', data, section + 8)
                    timestamp = struct.unpack_from('<I', data, pe + 8)[0]
                    return base, rva, data[raw_offset:raw_offset + virtual_size], timestamp
            raise ValueError('PE image has no .text section')
        original = text_section(pathlib.Path(name.value))
        linked = text_section(a.mapped_image or pathlib.Path(name.value))
        if original[1] != linked[1] or len(original[2]) != len(linked[2]):
            raise ValueError('Relinked code layout differs; refusing an inaccurate symbol map')
        whole_text_verified = original[2] == linked[2]
        map_text = a.map.read_text(errors='replace')
        timestamp = re.search(r'Timestamp is ([0-9a-f]+)', map_text, re.I)
        if not timestamp or int(timestamp[1], 16) != linked[3]:
            raise ValueError('Linker map timestamp does not match its image')
        for line in map_text.splitlines():
            match = re.match(r'^\s*[0-9a-f]+:[0-9a-f]+\s+(\S+)\s+([0-9a-f]{16})\s', line, re.I)
            if match:
                symbols.append((int(match[2], 16) - linked[0], match[1]))
        symbols.sort(); symbol_addresses = [address for address, _ in symbols]
    undecorate = api(c.WinDLL('dbghelp'), 'UnDecorateSymbolName', w.DWORD,
                     [c.c_char_p, c.c_char_p, w.DWORD, w.DWORD])
    def resolve(ip):
        info = MemoryInfo()
        if not query(process, ip, c.byref(info), c.sizeof(info)): return 'unmapped'
        if info.type in (0x20000,0x40000) and info.protect & 0xF0: return 'JIT'
        module = c.create_unicode_buffer(1024)
        if not mapped(process, ip, module, len(module)): return 'other'
        module_name = pathlib.PureWindowsPath(module.value).name
        if module_name.lower() == 'nanorv.exe' and symbols:
            rva = ip - info.allocation; index = bisect.bisect_right(symbol_addresses, rva) - 1
            if index >= 0:
                address, symbol = symbols[index]; result = c.create_string_buffer(4096)
                end_index = bisect.bisect_right(symbol_addresses, address)
                start = address - original[1]
                end = min(symbol_addresses[end_index] - original[1] if end_index < len(symbols)
                          else len(original[2]), len(original[2]))
                # Relinking can change a few import/metadata displacements. Do
                # not accept a nearest-symbol guess: verify this entire code
                # range against the live binary, or return no function name.
                if not (0 <= start < end and original[2][start:end] == linked[2][start:end]):
                    return f'{module_name} (unverified function at RVA 0x{rva:x})'
                if undecorate(symbol.encode(), result, len(result), 0x1000): symbol = result.value.decode(errors='replace')
                return f'{module_name}!{symbol}+0x{rva - address:x} [code verified]'
        return module_name
    if a.ips:
        print(json.dumps({'whole_text_verified': whole_text_verified,
                          'locations': {hex(ip): resolve(ip) for ip in a.ips}}, indent=2))
        sys.exit(0)
    snap = snapshot(4, 0)
    if snap == c.c_void_p(-1).value: raise c.WinError(c.get_last_error())
    try:
        entry = ThreadEntry(); entry.size = c.sizeof(entry)
        ok = first(snap, c.byref(entry))
        while ok:
            if entry.pid == a.pid:
                handle = open_thread(0x4A, False, entry.tid)
                if handle: threads[entry.tid] = handle
            ok = next_thread(snap, c.byref(entry))
    finally: close(snap)
    locations = {tid: collections.Counter() for tid in threads}
    addresses = {tid: collections.Counter() for tid in threads}
    initial = {tid: cpu_time(h) for tid, h in threads.items()}
    modules = {}
    def classify(ip):
        page = ip & ~4095
        if page in modules: return modules[page]
        info = MemoryInfo()
        if not query(process, ip, c.byref(info), c.sizeof(info)): return 'unmapped'
        if info.type in (0x20000,0x40000) and info.protect & 0xF0:
            result = 'JIT (non-image executable)'
        elif info.type == 0x1000000:
            module = c.create_unicode_buffer(1024)
            result = pathlib.PureWindowsPath(module.value).name if mapped(process, ip, module, len(module)) else 'image'
            if module.value: result = pathlib.PureWindowsPath(module.value).name
        else: result = 'other'
        modules[page] = result; return result
    buffer = c.create_string_buffer(1232 + 16)
    aligned = (c.addressof(buffer) + 15) & ~15
    started = time.perf_counter(); deadline = started + a.seconds
    while time.perf_counter() < deadline:
        for tid, handle in threads.items():
            if suspend(handle) == 0xFFFFFFFF: continue
            try:
                c.c_uint32.from_address(aligned + 48).value = 0x100003
                ok = context(handle, aligned)
                ip = c.c_uint64.from_address(aligned + 248).value if ok else 0
            finally:
                resume(handle)
            if ip:
                locations[tid][classify(ip)] += 1
                addresses[tid][ip] += 1
        time.sleep(0.01)
    elapsed = time.perf_counter() - started
    rows = []
    for tid, handle in threads.items():
        seconds = cpu_time(handle) - initial[tid]
        rows.append({'tid': tid, 'cpu_seconds': round(seconds, 4),
                     'locations': dict(locations[tid]),
                     'top_ips': [{'ip': hex(ip), 'count': count, 'function': resolve(ip)}
                                 for ip, count in addresses[tid].most_common(8)]})
    print(json.dumps({'pid': a.pid, 'binary': name.value, 'wall_seconds': round(elapsed, 3),
                      'cpu_cores': round(sum(r['cpu_seconds'] for r in rows) / elapsed, 3),
                      'threads': sorted(rows, key=lambda r: r['cpu_seconds'], reverse=True)}, indent=2))
finally:
    for handle in threads.values(): close(handle)
    close(process)
