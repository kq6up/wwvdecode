#!/usr/bin/env python3
"""Synthesize WWV / WWVH audio with a known time, for testing wwvdecode.

The point of this is to separate "the decoder is broken" from "the recording
is broken".  Every impairment we have seen off the air is reproducible here:
an SSB tuning offset, a weak 100 Hz subcarrier, QSB, and a capture chain that
drops buffers.

Examples:
    # clean reference
    python3 mkwwv.py -o test/clean.raw --minutes 3 --snr 20

    # what the first real recording looked like: SSB offset, weak code,
    # fading, and ~22 ms of audio lost per second
    python3 mkwwv.py -o test/nasty.raw --snr 4 --offset -9.6 \
                     --qsb 0.7 --drop-ms 22

    # WWVH instead of WWV (1200 Hz tick and minute marker)
    python3 mkwwv.py -o test/wwvh.raw --station wwvh

Output is raw S16_LE mono, so decode it with  -r <rate>.
"""
import argparse
from datetime import datetime, timedelta, timezone
from pathlib import Path
import math
import wave
import numpy as np

TICK = {"wwv": 1000.0, "wwvh": 1200.0}


def parse_args():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("-o", "--output", default="synth.raw")
    p.add_argument("--rate", type=int, default=8000)
    p.add_argument("--yy", type=int, default=26, help="year, 2 digits")
    p.add_argument("--doy", type=int, default=224, help="day of year")
    p.add_argument("--hh", type=int, default=7, help="UTC hour at the start")
    p.add_argument("--mm", type=int, default=13, help="UTC minute at the start")
    p.add_argument("--minutes", type=int, default=3)
    p.add_argument("--station", choices=sorted(TICK), default="wwv")
    p.add_argument("--snr", type=float, default=20.0,
                   help="100 Hz code SNR in a 4 Hz bandwidth, dB")
    p.add_argument("--offset", type=float, default=0.0,
                   help="additive audio tone offset in Hz, as SSB tuning error gives")
    p.add_argument("--qsb", type=float, default=0.0,
                   help="fade depth, 0 to 1")
    p.add_argument("--qsb-period", type=float, default=14.0,
                   help="fade period in seconds")
    p.add_argument("--drop-ms", type=float, default=0.0,
                   help="milliseconds of audio dropped per second, as a capture "
                        "chain losing buffers does")
    p.add_argument("--dut1", type=float, default=0.0, help="UT1 correction in 0.1 s steps, -0.7..0.7")
    p.add_argument("--leap-warning", action="store_true")
    p.add_argument("--dst", type=int, choices=range(4), default=0, help="two DST bits, 0..3")
    p.add_argument("--low-level", type=float, default=1/3.3,
                   help="quiet/high code amplitude ratio (default 1/3.3; 0 for legacy fixtures)")
    p.add_argument("--start-offset", type=float, default=0.0,
                   help="seconds to crop from the start, for blind synchronization tests")
    p.add_argument("--seed", type=int, default=7)
    a = p.parse_args()
    finite = (a.snr,a.offset,a.qsb,a.qsb_period,a.drop_ms,a.low_level,a.start_offset,a.dut1)
    if not all(math.isfinite(x) for x in finite): p.error("parameters must be finite")
    if not 4000 <= a.rate <= 192000: p.error("rate must be 4000..192000")
    if not 1 <= a.minutes <= 1440: p.error("minutes must be 1..1440")
    if not 0 <= a.yy <= 99 or not 0 <= a.hh <= 23 or not 0 <= a.mm <= 59:
        p.error("invalid year/hour/minute")
    if abs(a.dut1) > .7 or abs(a.dut1*10-round(a.dut1*10)) > 1e-6:
        p.error("dut1 must be -0.7..0.7 in 0.1 s steps")
    days = 366 if a.yy % 4 == 0 else 365
    if not 1 <= a.doy <= days: p.error("invalid day of year")
    if not 0 <= a.qsb < 1 or a.qsb_period <= 0: p.error("invalid fading parameters")
    if not 0 <= a.drop_ms < 130: p.error("drop-ms must be 0..<130")
    if not 0 <= a.low_level < 1: p.error("low-level must be 0..<1")
    if not 0 <= a.start_offset < 60*a.minutes: p.error("invalid start-offset")
    if not 0 < 100+a.offset or 1500+a.offset >= a.rate/2: p.error("tones outside sample bandwidth")
    return a


def frame_symbols(yy, doy, hh, mm, dut1=0.0, leap=False, dst=0):
    """The 60 symbols of one minute: 0, 1, 2 = marker, 3 = hole."""
    s = [0] * 60
    s[0] = 3
    for k in (9, 19, 29, 39, 49, 59):
        s[k] = 2

    def put(positions, value, weights):
        for pos, w in zip(positions, weights):
            s[pos] = 1 if (value & w) else 0

    put([4, 5, 6, 7], yy % 10, [1, 2, 4, 8])
    put([51, 52, 53, 54], yy // 10, [1, 2, 4, 8])
    put([10, 11, 12, 13], mm % 10, [1, 2, 4, 8])
    put([15, 16, 17], mm // 10, [1, 2, 4])
    put([20, 21, 22, 23], hh % 10, [1, 2, 4, 8])
    put([25, 26], hh // 10, [1, 2])
    put([30, 31, 32, 33], doy % 10, [1, 2, 4, 8])
    put([35, 36, 37, 38], (doy // 10) % 10, [1, 2, 4, 8])
    put([40, 41], doy // 100, [1, 2])
    s[2] = dst & 1
    s[55] = (dst >> 1) & 1
    s[3] = int(leap)
    s[50] = int(dut1 >= 0)
    put([56,57,58], int(round(abs(dut1)*10)), [1,2,4])
    return s


def build(a):
    R = a.rate
    tick_f = TICK[a.station] + a.offset
    pulse_len = {0: 0.170, 1: 0.470, 2: 0.770, 3: 0.0}
    out = []
    stamp = datetime(2000+a.yy,1,1,a.hh,a.mm,tzinfo=timezone.utc) + timedelta(days=a.doy-1)
    second_index = 0

    for _ in range(a.minutes):
        yy, doy, hh, mm = stamp.year % 100, stamp.timetuple().tm_yday, stamp.hour, stamp.minute
        sym = frame_symbols(yy, doy, hh, mm, a.dut1, a.leap_warning, a.dst)
        # 500 Hz on even minutes, 600 on odd (WWVH is the other way round)
        tone = 500.0 if (mm % 2 == 0) else 600.0
        if a.station == "wwvh":
            tone = 600.0 if (mm % 2 == 0) else 500.0
        tone += a.offset

        for k in range(60):
            t = np.arange(R) / R
            carrier_t = t + second_index
            x = np.zeros(R)

            # 100 Hz time code, pulse starts 30 ms into the second
            dur = pulse_len[sym[k]]
            if dur > 0:
                active = (t >= 0.030) & (t < 0.990)
                high = t < 0.030 + dur
                g = active * np.where(high, 1.0, a.low_level)
                x += 0.30 * g * np.sin(2 * np.pi * (100.0 + a.offset) * carrier_t)

            # program tone, seconds 1..45, with the 40 ms hole at each second
            if 1 <= k < 45:
                g = np.ones(R)
                g[: int(0.030 * R)] = 0.0          # tail of this second's hole
                g[int(0.990 * R):] = 0.0           # lead of the next one
                x += 0.50 * g * np.sin(2 * np.pi * tone * carrier_t)

            # Tick begins ON the second boundary; the preceding 10 ms are quiet.
            if k == 0:
                g = (t < 0.800).astype(float)
                marker_f = (1500.0 + a.offset) if mm == 0 else tick_f
                x += 0.70 * g * np.sin(2 * np.pi * marker_f * carrier_t)
            elif k not in (29, 59):
                g = (t < 0.005).astype(float)
                x += 0.70 * g * np.sin(2 * np.pi * tick_f * carrier_t)

            out.append(x)
            second_index += 1

        stamp += timedelta(minutes=1)
    return np.concatenate(out)


def main():
    a = parse_args()
    R = a.rate
    x = build(a)

    if a.qsb > 0:
        n = np.arange(x.size) / R
        x = x * (1.0 - a.qsb * 0.5 * (1 + np.sin(2 * np.pi * n / a.qsb_period)))

    # Noise scaled so the code line sits at the requested SNR in 4 Hz.
    code_power = (0.30 ** 2) / 2
    n0 = code_power / (10 ** (a.snr / 10.0) * 4.0)
    rng = np.random.default_rng(a.seed)
    x = x + rng.normal(0.0, np.sqrt(n0 * R / 2), x.size)

    if a.drop_ms > 0:
        drop = int(a.drop_ms * R / 1000)
        kept = [x[i:i + R - drop] for i in range(0, x.size, R)]
        x = np.concatenate(kept)

    x = x[int(round(a.start_offset*R)):]
    if not x.size: raise SystemExit("no samples remain after cropping")
    peak = np.max(np.abs(x))
    if peak > 0:
        x = x / peak * 0.5
    pcm = (np.clip(x, -1, 1) * 32767).astype("<i2")
    output = Path(a.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.suffix.lower() == ".wav":
        with wave.open(str(output),"wb") as f:
            f.setnchannels(1); f.setsampwidth(2); f.setframerate(R)
            f.writeframes(pcm.tobytes())
    else:
        pcm.tofile(output)

    start_mm = a.mm
    print(f"wrote {a.output}: {x.size / R:.1f} s at {R} Hz, {a.station.upper()}")
    print(f"  starts at YY={a.yy:02d} DOY={a.doy:03d} "
          f"{a.hh:02d}:{start_mm:02d}:00 UTC")
    print(f"  code SNR {a.snr:.1f} dB in 4 Hz, tone offset {a.offset:+.1f} Hz"
          + (f", QSB {a.qsb:.2f}" if a.qsb else "")
          + (f", dropping {a.drop_ms:.0f} ms/s" if a.drop_ms else ""))
    print(f"  decode with: ./wwvdecode -i {a.output} -r {R}"
          + (" --track" if a.drop_ms else ""))


if __name__ == "__main__":
    main()
