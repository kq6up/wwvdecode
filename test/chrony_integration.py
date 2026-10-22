#!/usr/bin/env python3
"""Isolated Linux integration: real chronyd consumes both outputs, never adjusts time.
Requires chronyd, chronyc, capsh, numpy and a built ALSA-enabled decoder.
Run as root ONLY inside a disposable container/VM. Uses no network time servers.
"""
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import time

root=Path(__file__).resolve().parents[1]
if not sys.platform.startswith('linux') or os.geteuid()!=0:
    raise SystemExit('Run inside a disposable Linux VM/container as root')
with tempfile.TemporaryDirectory(prefix='wwv-integration-',dir='/tmp') as folder:
    d=Path(folder);d.chmod(0o700)
    conf=d/'chrony.conf';sock=d/'wwv.sock';cmd=d/'command.sock'
    conf.write_text(f'''port 0
cmdport 0
bindcmdaddress {cmd}
pidfile {d}/chronyd.pid
logdir {d}
log refclocks tracking
refclock SOCK {sock} refid WVSK poll 0 filter 1 precision 0.03125 delay 0.05
refclock SHM 10:perm=0600 refid WVSH poll 0 filter 1 precision 0.03125 delay 0.05 noselect
''')
    with (d/'chronyd.log').open('w') as log:
        # Enforce -x AND remove CAP_SYS_TIME from the capability bounding set.
        daemon=subprocess.Popen(['capsh','--shell=/bin/sh','--drop=cap_sys_time','--','-c',
                                 f'exec chronyd -x -d -u root -f {conf}'],stdout=log,stderr=log)
        try:
            for _ in range(100):
                if sock.exists():break
                if daemon.poll() is not None:raise RuntimeError((d/'chronyd.log').read_text())
                time.sleep(.05)
            else:raise RuntimeError('chronyd did not create socket')
            status=Path(f'/proc/{daemon.pid}/status').read_text()
            for name in ('CapEff','CapBnd'):
                bits=int(re.search(rf'^{name}:\s*(\w+)',status,re.M)[1],16)
                assert not bits & (1<<25),f'{name}: CAP_SYS_TIME unexpectedly present'
            # Both signs test reference-minus-local offset and minute association.
            for offset in (0.25,-0.375):
                subprocess.run([sys.executable,str(root/'test/live_synthetic.py'),
                                '--decoder',str(root/'wwvdecode'),'--sock',str(sock),'--shm-unit','10',
                                '--offset',str(offset),'--log',str(d/f'decoder-{offset}.log')],check=True)
                text=(d/'refclocks.log').read_text()
                for refid in ('WVSK','WVSH'):
                    values=[]
                    for line in text.splitlines():
                        fields=line.split()
                        if len(fields)>6 and fields[2]==refid and fields[3]=='0':
                            values.append(float(fields[6]))
                    good=[v for v in values if abs(v-offset)<.025]
                    assert len(good)>=8,(refid,offset,values)
                    print(f'{refid}: {len(good)} accepted raw samples; median near {offset:+.3f}s')
            assert 'Disabled control of system clock' in (d/'chronyd.log').read_text()
            print(subprocess.check_output(['chronyc','-h',str(cmd),'sources','-v'],text=True))
            print('PASS: both real chrony refclocks accepted synthetic audio; clock control disabled')
        finally:
            daemon.terminate();daemon.wait(timeout=5)
            # Preserve test evidence outside temporary files when requested.
            dest=os.environ.get('WWV_TEST_ARTIFACTS')
            if dest:
                import shutil
                shutil.copytree(d,dest,dirs_exist_ok=True,ignore=shutil.ignore_patterns('*.sock','*.pid'))
