"""実ゲームの再戦・保存状態を読み取り専用で検査する。引数は対象PID。"""
import ctypes as c,json,sys
k=c.WinDLL('kernel32',use_last_error=True)
k.OpenProcess.argtypes=[c.c_uint32,c.c_int,c.c_uint32];k.OpenProcess.restype=c.c_void_p
k.ReadProcessMemory.argtypes=[c.c_void_p,c.c_void_p,c.c_void_p,c.c_size_t,c.c_void_p]
k.CloseHandle.argtypes=[c.c_void_p]
for pid in sys.argv[1:]:
 h=k.OpenProcess(0x1010,False,int(pid))
 if not h:raise c.WinError(c.get_last_error())
 try:
  values={'pid':int(pid)}
  for name,address in [('auto_save',0x553FE8),('replay_created',0x774C30),('menu_depth',0x767440)]:
   v=c.c_uint32()
   if not k.ReadProcessMemory(h,address,c.byref(v),4,None):raise c.WinError(c.get_last_error())
   values[name]=v.value
  print(json.dumps(values))
 finally:k.CloseHandle(h)
