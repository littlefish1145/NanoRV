"""Pin only an independently spawned Windows benchmark process."""
import ctypes,os
def apply_affinity(process,mask):
    if os.name!='nt':raise ValueError('Windows affinity is supported only on the Windows runner')
    if not 0<mask<1<<(ctypes.sizeof(ctypes.c_size_t)*8):raise ValueError('invalid affinity mask')
    kernel=ctypes.WinDLL('kernel32',use_last_error=True)
    kernel.SetProcessAffinityMask.argtypes=[ctypes.c_void_p,ctypes.c_size_t]
    kernel.SetProcessAffinityMask.restype=ctypes.c_int
    if not kernel.SetProcessAffinityMask(int(process._handle),mask):raise ctypes.WinError(ctypes.get_last_error())
