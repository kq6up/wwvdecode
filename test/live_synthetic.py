#!/usr/bin/env python3
"""Real-time synthetic WWV -> timestamped PCM -> decoder. Never sets a clock.
Run only against isolated test consumers: the synthetic clock has an intentional offset.
"""
import argparse
import datetime as dt
from pathlib import Path
import subprocess
import sys
import tempfile
import time

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--decoder',default='./wwvdecode')
p.add_argument('--sock',required=True)
p.add_argument('--shm-unit',type=int,default=10)
p.add_argument('--offset',type=float,default=.25)
p.add_argument('--seconds',type=float,default=205)
p.add_argument('--log',default='live-decoder.log')
a=p.parse_args()
root=Path(__file__).resolve().parents[1]
now=time.time();epoch=int((now+a.offset)//60)*60
stamp=dt.datetime.fromtimestamp(epoch,dt.timezone.utc)
with tempfile.TemporaryDirectory() as folder:
    raw=Path(folder)/'synth.raw'
    subprocess.run([sys.executable,str(root/'mkwwv.py'),'-o',str(raw),'--minutes','5','--snr','40',
                    '--yy',str(stamp.year%100),'--doy',str(stamp.timetuple().tm_yday),
                    '--hh',str(stamp.hour),'--mm',str(stamp.minute)],check=True)
    pcm=raw.read_bytes();rate=8000
    # Virtual radio UTC = host UTC + offset. Crop into the appropriate minute.
    real0=time.time()+.2
    start=round((real0+a.offset-epoch)*rate);real0=epoch+start/rate-a.offset
    mono0=time.monotonic()+(real0-time.time())
    print(f'synthetic UTC minus host = {a.offset:+.6f} s; duration={a.seconds}s',flush=True)
    with open(a.log,'w') as log:
        proc=subprocess.Popen([a.decoder,'--timed-raw','--sock',a.sock,'--shm-unit',str(a.shm_unit)],
                              stdin=subprocess.PIPE,stdout=log,stderr=log)
        try:
            for i in range(0,int(a.seconds*rate),800):
                block=pcm[2*(start+i):2*(start+i+800)]
                if len(block)!=1600:raise RuntimeError('synthetic audio exhausted')
                target=mono0+(i+800)/rate
                time.sleep(max(0,target-time.monotonic()))
                proc.stdin.write(f'WWV1 {real0+i/rate:.9f} {mono0+i/rate:.9f} 800\n'.encode()+block)
                proc.stdin.flush()
        finally:
            proc.stdin.close()
        rc=proc.wait(timeout=10)
        if rc:raise SystemExit(rc)
