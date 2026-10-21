"""End-to-end tests use independently specified expected UTC/status values."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
import wave
import numpy as np

ROOT=Path(__file__).resolve().parents[1]
DECODER=Path(os.environ.get('WWV_DECODER', ROOT/'wwvdecode'))
UTC=re.compile(r'UTC   YY=(\d+) DOY=(\d+) (\d+):(\d+):00')

def run(args, expected=0):
    p=subprocess.run([str(x) for x in args],cwd=ROOT,text=True,capture_output=True,timeout=90)
    if p.returncode!=expected:
        raise AssertionError(f'{args}: expected exit {expected}, got {p.returncode}\n{p.stdout}\n{p.stderr}')
    return p.stdout+p.stderr

class DecodeTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.folder=Path(self.temp.name)
    def tearDown(self): self.temp.cleanup()
    def generate(self,name='audio.raw',*options):
        p=self.folder/name
        run([sys.executable,ROOT/'mkwwv.py','-o',p,'--minutes','3',*options])
        return p
    def decode(self,p,*opts,expected=0):
        return run([DECODER,'-i',p,'--no-analyze',*opts],expected)
    def times(self,out): return {tuple(map(int,m)) for m in UTC.findall(out)}
    def test_clean_blind_raw(self):
        out=self.decode(self.generate())
        self.assertEqual(self.times(out),{(26,224,7,m) for m in (13,14,15)})
    def test_exact_one_minute(self):
        out=self.decode(self.generate('minute.raw','--minutes','1'))
        self.assertEqual(self.times(out),{(26,224,7,13)})
    def test_wav_cropped_and_status(self):
        out=self.decode(self.generate('crop.wav','--start-offset','17.321','--dut1','-.7','--leap-warning','--dst','3'))
        self.assertEqual(self.times(out),{(26,224,7,m) for m in (14,15)})
        self.assertIn('DUT1  -0.7 s',out);self.assertIn('LEAP  warning set',out);self.assertIn('DST   bits=11',out)
    def test_station_and_tuning(self):
        for station,offset in [('wwv',-9.6),('wwvh',12.3)]:
            with self.subTest(station=station):
                out=self.decode(self.generate('tune.raw','--station',station,'--offset',str(offset)))
                self.assertEqual(self.times(out),{(26,224,7,m) for m in (13,14,15)})
    def test_rates_and_bcd_extremes(self):
        for rate in (4000,11025,44100):
            with self.subTest(rate=rate):
                out=self.decode(self.generate('rate.wav','--rate',str(rate),'--yy','99','--doy','365','--hh','23','--mm','57'))
                self.assertEqual(self.times(out),{(99,365,23,m) for m in (57,58,59)})
    def test_calendar_rollovers(self):
        for yy,day,following in [(26,365,27),(28,366,29),(99,365,0)]:
            with self.subTest(yy=yy):
                out=self.decode(self.generate('year.wav','--yy',str(yy),'--doy',str(day),'--hh','23','--mm','59'))
                self.assertEqual(self.times(out),{(yy,day,23,59),(following,1,0,0),(following,1,0,1)})
    def test_dropped_audio_and_fading(self):
        for drop in (10,22,50):
            with self.subTest(drop=drop):
                out=self.decode(self.generate('gap.raw','--snr','30','--drop-ms',str(drop),'--qsb','.5','--offset','-9.6'),'-o','-9.6','--track')
                self.assertEqual(self.times(out),{(26,224,7,m) for m in (13,14,15)})
    def test_unconfirmed_tracking_is_not_reported_as_utc(self):
        # At this lower SNR a plausible BCD hour can be wrong. Require a
        # second consistent frame instead of accepting that isolated time.
        p=self.generate('weak.raw','--snr','20','--drop-ms','22','--qsb','.5','--offset','-9.6')
        out=self.decode(p,'-o','-9.6','--track',expected=3)
        self.assertFalse(self.times(out))
    def test_streaming_and_raw_stdin(self):
        p=self.generate()
        out=self.decode(p,'--no-search','--stream')
        self.assertTrue({(26,224,7,m) for m in (14,15)} <= self.times(out))
        self.assertFalse(self.times(out)-{(26,224,7,m) for m in (13,14,15)})
        with p.open('rb') as f:
            q=subprocess.run([str(DECODER),'--raw','-o','0'],stdin=f,text=True,capture_output=True,timeout=90)
        self.assertEqual(q.returncode,0,q.stderr)
        self.assertTrue({(26,224,7,m) for m in (14,15)} <= self.times(q.stdout))
        self.assertFalse(self.times(q.stdout)-{(26,224,7,m) for m in (13,14,15)})
    def test_wrong_boundary_is_not_a_time(self):
        out=self.decode(self.generate(),'--no-search','--at','10',expected=3)
        self.assertFalse(self.times(out))
    def test_noise_and_silence_are_not_times(self):
        for seed in range(4):
            with self.subTest(seed=seed):
                p=self.folder/'noise.raw'
                samples=(np.random.default_rng(seed).normal(0,3000,8000*180).astype('<i2') if seed else np.zeros(8000*180,dtype='<i2'))
                samples.tofile(p)
                out=self.decode(p,'-o','0',expected=3)
                self.assertFalse(self.times(out))
    def test_short_recording(self):
        p=self.folder/'short.raw';np.zeros(8000*59,dtype='<i2').tofile(p)
        self.assertFalse(self.times(self.decode(p,'-o','0',expected=3)))
    @unittest.skipUnless(os.environ.get('WWV_REAL_CAPTURE'),'local receiver recording not supplied')
    def test_captured_audio_is_not_claimed_as_a_full_decode(self):
        p=Path(os.environ['WWV_REAL_CAPTURE'])
        out=self.decode(p,'--track',expected=3)
        self.assertFalse(self.times(out))
    def test_analyze_only(self):
        p=self.folder/'short.raw';np.zeros(8000*2,dtype='<i2').tofile(p)
        self.decode(p,'-o','0','--no-search')

if __name__=='__main__':unittest.main(verbosity=2)
