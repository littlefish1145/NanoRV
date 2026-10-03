"""Integration evidence: capture only visible windows owned by our X server."""
import ctypes, json, pathlib
from ctypes import wintypes as w
from PIL import Image
root=pathlib.Path(__file__).resolve().parent.parent
session=json.loads((root/'build/desktop-session/session.json').read_text(encoding='utf-8-sig'))
user=ctypes.WinDLL('user32',use_last_error=True);gdi=ctypes.WinDLL('gdi32',use_last_error=True)
callback=ctypes.WINFUNCTYPE(w.BOOL,w.HWND,w.LPARAM)
user.EnumWindows.argtypes=[callback,w.LPARAM]
user.GetWindowThreadProcessId.argtypes=[w.HWND,ctypes.POINTER(w.DWORD)]
user.IsWindowVisible.argtypes=[w.HWND]
user.GetWindowTextW.argtypes=[w.HWND,w.LPWSTR,ctypes.c_int]
user.GetWindowRect.argtypes=[w.HWND,ctypes.POINTER(w.RECT)]
user.GetDC.argtypes=[w.HWND];user.GetDC.restype=w.HDC
user.ReleaseDC.argtypes=[w.HWND,w.HDC]
user.PrintWindow.argtypes=[w.HWND,w.HDC,w.UINT]
gdi.CreateCompatibleDC.argtypes=[w.HDC];gdi.CreateCompatibleDC.restype=w.HDC
gdi.CreateCompatibleBitmap.argtypes=[w.HDC,ctypes.c_int,ctypes.c_int];gdi.CreateCompatibleBitmap.restype=w.HBITMAP
gdi.SelectObject.argtypes=[w.HDC,w.HANDLE];gdi.SelectObject.restype=w.HANDLE
gdi.DeleteObject.argtypes=[w.HANDLE];gdi.DeleteDC.argtypes=[w.HDC]
class Header(ctypes.Structure):
    _fields_=[('size',w.DWORD),('width',w.LONG),('height',w.LONG),('planes',w.WORD),('bits',w.WORD),('compression',w.DWORD),('bytes',w.DWORD),('x',w.LONG),('y',w.LONG),('used',w.DWORD),('important',w.DWORD)]
gdi.GetDIBits.argtypes=[w.HDC,w.HBITMAP,w.UINT,w.UINT,ctypes.c_void_p,ctypes.POINTER(Header),w.UINT]
found=[]
@callback
def visit(window,unused):
    pid=w.DWORD();user.GetWindowThreadProcessId(window,ctypes.byref(pid))
    if pid.value!=session['xserver_pid'] or not user.IsWindowVisible(window):return True
    title=ctypes.create_unicode_buffer(512);user.GetWindowTextW(window,title,512)
    rectangle=w.RECT();user.GetWindowRect(window,ctypes.byref(rectangle))
    width,height=rectangle.right-rectangle.left,rectangle.bottom-rectangle.top
    if not title.value or width<100 or height<100:return True
    screen=user.GetDC(window);dc=gdi.CreateCompatibleDC(screen);bitmap=gdi.CreateCompatibleBitmap(screen,width,height)
    previous=gdi.SelectObject(dc,bitmap)
    try:
        if not user.PrintWindow(window,dc,2):raise RuntimeError('PrintWindow failed')
        gdi.SelectObject(dc,previous)
        header=Header(ctypes.sizeof(Header),width,-height,1,32,0,width*height*4,0,0,0,0)
        pixels=ctypes.create_string_buffer(width*height*4)
        if gdi.GetDIBits(dc,bitmap,0,height,pixels,ctypes.byref(header),0)!=height:raise RuntimeError('GetDIBits failed')
        file=root/f'build/desktop-window-{len(found)}.png'
        Image.frombytes('RGB',(width,height),pixels.raw,'raw','BGRX').save(file)
        found.append({'title':title.value,'path':str(file),'width':width,'height':height})
    finally:gdi.DeleteObject(bitmap);gdi.DeleteDC(dc);user.ReleaseDC(window,screen)
    return True
user.EnumWindows(visit,0)
if len(found)<3:raise RuntimeError(f'Expected terminal/editor/file manager windows, found {len(found)}')
(root/'build/desktop-windows.json').write_text(json.dumps(found,ensure_ascii=False,indent=2),encoding='utf-8')
for result in found:print(result['title'],result['path'])
