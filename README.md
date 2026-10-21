# wwvdecode

Experimental C decoder for the WWV / WWVH 100 Hz BCD time code in receiver
audio, with a Python signal generator and regression tests.

**Full UTC decoding is verified on synthetic signals. The supplied receiver
capture, `real-wwv.raw`, has NOT produced a trustworthy full UTC decode.**
See [VALIDATION.md](VALIDATION.md) for the measured results and remaining work.

## Build and test

The decoder needs a C compiler and the math library. Linux builds use ALSA
when its development package is available; macOS builds use file/stdin input.
The generator and integration tests also need Python 3 and NumPy.

```sh
make                         # compiler detects ALSA availability
# or: make noalsa
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r requirements.txt
make test PYTHON=python
```

Tests generate their own audio in temporary directories. Receiver recordings
and generated audio are ignored by Git. No microphone, radio, or network
service is needed for the tests.

## Decode a recording

```sh
./wwvdecode -i capture.wav
./wwvdecode -i recordings/capture.raw -r 8000
./wwvdecode -i capture.wav --track
./wwvdecode -i capture.wav --no-search       # signal analysis only
./wwvdecode -i capture.wav --at 34.005 -v    # explicitly supplied frame boundary
```

WAV input supports PCM 8/16/24/32-bit and IEEE float32; channels are averaged.
Headerless input is signed 16-bit little-endian mono (`-r` supplies its rate).

File mode estimates the additive audio tuning offset, measures the signal,
and searches for complete frames. A valid frame requires the reference gap,
all six position markers, reserved zeros, legal individual BCD digits, date
ranges, and the status bits. Window measurements integrate complex voltage
before measuring power; a smoothed 5 ms grid is used for timing searches.

If fixed-spacing search finds no complete frame, an adaptive timing tracker
runs automatically. `--track` selects that tracker directly. It follows local
second lengths between about 0.87 and 1.03 seconds of recorded audio. The
known code layout guides timing, but **every output bit is measured again**;
expected markers and zeros are never inserted into the decoded data.
Tracked UTC is reported only when another nearby complete frame agrees with
the expected minute progression. An isolated plausible frame is labeled
`UTC?` and does not count as a successful decode.

The format has no checksum. Structural checks and agreement between minutes
reduce errors; they cannot prove a noisy result is correct. Explicit `--at`
and the original streaming mode report structurally valid single frames
without the offline tracker's extra minute-consistency check.

File decoding exits **0** when a full frame is accepted, **3** when no full
frame is accepted, **1** on input/building errors, and **2** on rejected command
arguments. Analysis-only mode exits 0 when analysis completes. A `SYNC` or
`FRAME` line by itself is a hypothesis, not a decoded timestamp. File boundary
estimates are not precision PPS measurements or a hardware clock-setting API.

Useful options:

- `-o HZ`: known additive tuning error; otherwise files estimate it automatically.
- `--min-snr DB`: coherent symbol-window pulse/floor threshold, default 3 dB.
- `--candidates N`: fixed-search output limit, default 5.
- `--no-analyze`: suppress the signal report.
- `-v`: per-second measurements.

## Generate an independent test signal

```sh
python mkwwv.py -o test/clean.wav --minutes 3
./wwvdecode -i test/clean.wav

python mkwwv.py -o test/gaps.raw --minutes 3 --snr 30 \
  --offset -9.6 --qsb 0.5 --drop-ms 22
./wwvdecode -i test/gaps.raw -r 8000 -o -9.6 --track

python mkwwv.py -o test/wwvh.wav --station wwvh --start-offset 17.321 \
  --dut1 -0.7 --leap-warning --dst 3
./wwvdecode -i test/wwvh.wav
```

Defaults encode YY=26, day 224, 07:13 UTC, advancing each minute. The decoder
is not given those values. Flags select other dates, rates, seeds, tuning,
fading, and periodic dropped audio. Calendar rollover includes leap years and
New Year. Ticks begin at the second boundary; minute markers last 800 ms and
hour markers use 1500 Hz. Quiet code amplitude defaults to 1/3.3 of the high
level (`--low-level 0` reproduces the earlier ideal on/off fixture). Carrier
phase remains continuous before capture gaps are applied. `.wav` outputs have
headers; other extensions contain raw PCM.

This is a deliberately simplified test transmitter: program tones use an
alternating schedule, voice announcements are absent, and there is one
station. It does not reproduce every broadcast segment, interference source,
or capture failure. `--drop-ms` deletes the end of each generated second;
`--start-offset` crops seconds from the resulting recording.

## Streaming

```sh
./wwvdecode -D plughw:1,0 -a -v                # Linux/ALSA only
arecord -D plughw:1,0 -f S16_LE -c1 -r8000 -t raw | ./wwvdecode --raw -a
./wwvdecode -i capture.wav --no-search --stream
```

The streaming path currently acquires WWV 1000/1500 Hz timing tones, gates
500/600 Hz harmonics, and accumulates symbols while running. Acquisition can
lose the first frame. WWVH 1200 Hz live acquisition, a robust PPS flywheel,
seeded-minute verification, and actual ALSA-device validation remain future
work. Offline 100 Hz decoding supports both stations.

## Format reference

The bit positions and pulse lengths follow [NIST SP 250-67, section 2.3](https://www.nist.gov/sites/default/files/documents/calibrations/sp250-67.pdf)
and the [NIST WWV/WWVH broadcast format](https://www.nist.gov/pml/time-and-frequency-division/time-distribution/radio-station-wwv/wwv-and-wwvh-digital-time-code).
Code pulses start 30 ms after each second boundary and last 170, 470, or
770 ms. Second 0 has no code pulse; seconds 9,19,29,39,49,59 are markers.
Information refers to the UTC at the **start** of that minute. The year is only
two digits; this program uses the 2000–2099 leap-year interpretation.
