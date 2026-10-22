"""Configuration, safe input selection, startup failure, and PID ownership."""
from pathlib import Path
import os
import signal
import sys
import subprocess
import tempfile
import time
import unittest

ROOT=Path(__file__).resolve().parents[1]
DECODER=Path(os.environ.get('WWV_DECODER',ROOT/'wwvdecode'))
class ServiceTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.folder=Path(self.temp.name)
    def tearDown(self):self.temp.cleanup()
    def run_decoder(self,*args,code=0,data=None):
        p=subprocess.run([str(DECODER),*map(str,args)],input=data,capture_output=True,text=True,timeout=8)
        self.assertEqual(p.returncode,code,p.stdout+p.stderr);return p.stdout+p.stderr
    def test_config_and_cli_precedence(self):
        conf=self.folder/'wwv.conf';conf.write_text('# comment\nrate=11025\nsock=/tmp/wwv-test.sock\nshm-unit=-1\n')
        for args in [('--rate','8000','--config',conf),('--config',conf,'--rate','8000')]:
            self.assertIn('rate=8000',self.run_decoder(*args,'--check-config'))
    def test_unknown_key_and_malformed_value(self):
        conf=self.folder/'bad.conf'
        for text in ('sok=/tmp/foo','daemon=yes','rate=nan','sock=relative','shm-unit=1junk','rate=8000.5'):
            conf.write_text(text+'\n');self.run_decoder('-c',conf,'--check-config',code=2)
        self.run_decoder('-c',self.folder/'missing',code=2)
    def test_refuse_unstamped_or_offline_exports(self):
        for args in [('--raw',),('-i','missing.wav'),('--recover',),('--track',)]:
            self.run_decoder(*args,'--sock','/tmp/unused.sock',code=2)
        self.run_decoder('--daemon',code=2)
        self.run_decoder('--daemon','--timed-raw',code=2)
    def test_bad_and_stale_timestamped_blocks(self):
        self.run_decoder('--timed-raw','--sock','/tmp/unused.sock',data='invalid\n',code=2)
        self.run_decoder('--timed-raw','--sock','/tmp/unused.sock',data='WWV1 1 1 1\n00',code=2)
    def test_daemon_startup_failure_reaches_parent(self):
        self.run_decoder('--daemon','--pidfile',self.folder/'wwv.pid','--logfile',self.folder/'wwv.log',
                         '--device','wwv_nonexistent_device',code=1)
    @unittest.skipUnless(sys.platform.startswith('linux'),'Linux ALSA daemon test')
    def test_background_start_and_stop(self):
        pid=self.folder/'daemon.pid';log=self.folder/'daemon.log'
        if 'no ALSA support' in self.run_decoder('--help').lower():self.skipTest('ALSA not built')
        self.run_decoder('--daemon','--pidfile',pid,'--logfile',log,'-D','null')
        daemon=int(pid.read_text());os.kill(daemon,signal.SIGTERM)
        end=time.monotonic()+4
        while pid.exists() and time.monotonic()<end:time.sleep(.02)
        self.assertFalse(pid.exists());self.assertIn('capture: ALSA',log.read_text())
    def test_pid_lock_and_shutdown(self):
        pid=self.folder/'live.pid'
        proc=subprocess.Popen([str(DECODER),'--timed-raw','--pidfile',str(pid)],stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        try:
            end=time.monotonic()+3
            while not pid.exists() and time.monotonic()<end:time.sleep(.02)
            self.assertTrue(pid.exists())
            self.run_decoder('--timed-raw','--pidfile',pid,data='',code=1)
            self.assertEqual(pid.read_text().strip(),str(proc.pid))
            proc.communicate('',timeout=3);self.assertEqual(proc.returncode,0)
            self.assertFalse(pid.exists())
        finally:
            if proc.poll() is None:proc.kill();proc.communicate()
if __name__=='__main__':unittest.main()
