"""Play a quiet sine (or silence) to the array's stereo UAC2 WASAPI endpoint."""
import argparse,ctypes as C,struct,time,math,json
from pathlib import Path
from uac_wasapi import endpoints,call,release,guid,GUID

def play(seconds,frequency=1000,peak=512,output=None):
 choices=endpoints(0);matching=[(n,d) for n,d in choices if "Pico" in n]
 if len(matching)!=1:raise RuntimeError(f"Expected one Pico playback endpoint: {[n for n,d in choices]}")
 name,device=matching[0];client,render=C.c_void_p(),C.c_void_p()
 call(device,3,[C.POINTER(GUID),C.c_ulong,C.c_void_p,C.POINTER(C.c_void_p)],C.byref(guid("1cb9ad4c-dbfa-4c32-b178-c2f568a703b2")),23,None,C.byref(client))
 fmt=C.create_string_buffer(struct.pack("<HHIIHHHHI",0xfffe,2,48000,48000*4,4,16,22,16,3)+bytes.fromhex("0100000000001000800000aa00389b71"))
 call(client,7,[C.c_int,C.c_void_p,C.c_void_p],1,fmt,None)
 default,minimum=C.c_longlong(),C.c_longlong();call(client,9,[C.POINTER(C.c_longlong),C.POINTER(C.c_longlong)],C.byref(default),C.byref(minimum))
 period=default.value
 call(client,3,[C.c_int,C.c_ulong,C.c_longlong,C.c_longlong,C.c_void_p,C.c_void_p],1,0,max(period*4,1000000),period,fmt,None)
 count=C.c_uint();call(client,4,[C.POINTER(C.c_uint)],C.byref(count));capacity=count.value
 call(client,14,[C.POINTER(GUID),C.POINTER(C.c_void_p)],C.byref(guid("f294acfc-3146-4483-a7bf-addca7c260e2")),C.byref(render))
 sent=0;start=time.monotonic();total=int(seconds*48000)
 def write(frames):
  nonlocal sent
  data=C.c_void_p();call(render,3,[C.c_uint,C.POINTER(C.c_void_p)],frames,C.byref(data))
  pcm=(C.c_int16*(frames*2)).from_address(data.value)
  for n in range(frames):
   at=sent+n;fade=min(1,at/480,max(0,(total-at)/480));value=round(peak*fade*math.sin(2*math.pi*frequency*at/48000))
   pcm[2*n]=pcm[2*n+1]=value
  call(render,4,[C.c_uint,C.c_ulong],frames,0);sent+=frames
 try:
  write(min(capacity,total));call(client,10,[])
  while sent<total:
   padding=C.c_uint();call(client,6,[C.POINTER(C.c_uint)],C.byref(padding));free=capacity-padding.value
   if free:write(min(free,total-sent))
   else:time.sleep(.002)
  while True:
   padding=C.c_uint();call(client,6,[C.POINTER(C.c_uint)],C.byref(padding))
   if not padding.value:break
   time.sleep(.002)
 finally:
  call(client,11,[]);release(render);release(client)
  for _,d in choices:release(d)
 report={'device':name,'rate':48000,'channels':2,'format':'PCM16','frames':sent,'frequency':frequency,'peak':peak,'wall_seconds':time.monotonic()-start}
 if output:Path(output).write_text(json.dumps(report,indent=2),encoding='utf-8')
 print(json.dumps(report,indent=2))
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--seconds',type=int,default=3);p.add_argument('--frequency',type=int,default=1000);p.add_argument('--peak',type=int,default=512);p.add_argument('--output');p.add_argument('--list',action='store_true');a=p.parse_args()
 if a.list:
  for name,device in endpoints(0):print(name);release(device)
 elif 1<=a.seconds<=3600 and 20<=a.frequency<=20000 and 0<=a.peak<=32767:play(a.seconds,a.frequency,a.peak,a.output)
 else:p.error('Invalid playback parameters')
