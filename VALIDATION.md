# Validation and captured-audio investigation

Investigation date: October 21, 2026. Tests were run locally on macOS/clang.

## New receiver capture: two UTC times recovered

`MONO-026.wav` is a real, user-supplied receiver recording, not generated audio.
It is 184.997732 s, 44,100 Hz, 16-bit stereo with identical channels.
SHA-256: `7e8a495a6e02cf03467c18f8e9eb866a731ef46a4ad07b6f5c6b1aeea0ed0cea`.

The unmodified strict decoder rejects all full frames. The supplied WIP patch
produces both the plausible 2026/day 294/18:12 time and a false
2042/day 234/15:42 alias, so its relaxed validation alone is not acceptable.

The added opt-in recovery mode produces:

```text
SYNC  recovered minute boundary at 51.015 s into the recording
FRAME R01001100P100001000P0001010001001001001P0100000101101001000P  markers=4/6
UTC   YY=26 DOY=294 18:11:00 (recovered; adjacent minute and timing tone agree)
FLAGS not validated in recovery mode; inspect the measured FRAME symbols
SYNC  recovered minute boundary at 111.031 s into the recording
FRAME R01001100P010001000P000101000P001001001P010000010P101001000P  markers=6/6
UTC   YY=26 DOY=294 18:12:00 (recovered; adjacent minute and timing tone agree)
FLAGS not validated in recovery mode; inspect the measured FRAME symbols
```

Day 294 of 2026 is October 21. These are complete **UTC date/time fields**,
confirmed by a second independently measured minute and separate long timing
tones. They are not intact, fully validated 60-symbol frames. In particular,
marker/reserved-bit errors remain visible, and status flags are not certified.
No date/time hints, the patch's expected value, file metadata, or computer
clock are inputs to decoding. The supplied patch was tested separately; it
was not applied to the main decoder.

Run from the project directory:

```sh
make
./wwvdecode -i "$HOME/Downloads/MONO-026.wav" --recover --min-snr 1 --no-analyze
```

The algorithm is documented in README.md. The decision ratio and lower SNR
threshold were chosen while investigating this capture; confirmation reduces
false detections but does not establish a general false-alarm rate. The
boundary estimates are not precision PPS measurements. This result does not
validate the earlier recording, arbitrary missing audio, or an ESP32 build.

The suite now has 21 integration test methods plus C unit checks. With both
local recordings supplied, all 21 run. Public CI skips three tests requiring
local audio. Recovery tests cover both stations, hour markers, century
rollover, one-minute rejection, inconsistent-minute rejection, missing long
tones, noise, and the new capture's exact pair of UTC times. Unit checks also
reject invalid BCD, time-bit erasures, excess framing damage, and ambiguous
supported time chains. All original strict-mode regression tests remain.

```sh
WWV_REAL_CAPTURE="$HOME/wwvdecode/recordings/real-wwv.raw" \
WWV_NEW_CAPTURE="$HOME/Downloads/MONO-026.wav" make test
```

Receiver recordings remain local and are not included in the public source
repository. Logs from the initial baseline, WIP-patch experiment and recovery
run are preserved in the chat workspace's `outputs/mono026-investigation`.


## What changed

The previous decoder rejected even the generator's clean default signal.
Independent 5 ms power correlations admitted roughly 200 Hz of noise around
the 100 Hz signal. The offline path now mixes to complex baseband, maintains
an 80 ms Hann-smoothed envelope for timing, and integrates complex voltage
across each symbol window before measuring power. Prefix sums also make
window measurements constant time.

Fixed-spacing search now checks the minute reference gap, individual BCD
digits, reserved bits, and calendar ranges instead of ranking marker-pattern
aliases as if they were detections. A minute gap is tested against the frame's
pulse levels, rather than an unreliable noise/noise ratio. The search handles
an exactly 60-second input.

An adaptive tracker follows nonuniform local second spacing. Its timing model
never supplies output bits: independent measurements still have to pass all
frame checks. It also requires a second compatible minute before reporting
tracked UTC. This matters: a lower-SNR generated fixture produced an isolated
plausible but incorrect hour during development. That case is now explicitly
tested and remains an unconfirmed `UTC?`, with exit status 3.

The generator now preserves carrier phase, puts ticks on the second boundary,
produces actual WAV headers, includes a low code level, exposes DUT1/DST/leap
flags, and advances dates correctly through New Year. Synthetic impairments
remain simplified and should not be mistaken for an off-air validation.

## Synthetic test coverage

The regression suite checks expected values, not merely the presence of a UTC
line. It covers blind raw/WAV synchronization, an exactly one-minute file,
cropped starts, WWV/WWVH, additive tuning errors, 4/11.025/44.1 kHz rates,
BCD extremes, leap/common-year and two-digit year rollovers, nonzero status
flags, streaming/stdin acquisition, fading with 10/22/50 ms removed per second,
and rejection of noise, silence, a wrong frame boundary, short inputs, and
unconfirmed tracked time hypotheses. Clean fixtures use the generator's
20 dB setting; combined fading/gap fixtures use 30 dB. The lower-SNR fading/gap
case is intentionally a rejection test, not a successful-decoding claim.

The C unit test independently checks symbol decisions, a known frame, invalid
BCD, reserved-bit corruption, missing markers, and unknown status bits.

Run:

```sh
make test
# Optional additional regression against the local receiver capture:
WWV_REAL_CAPTURE="$HOME/wwvdecode/recordings/real-wwv.raw" make test
```

## Captured receiver audio: unresolved

`real-wwv.raw` contains 4,142,000 signed-16-bit mono samples: 517.75 seconds at
8 kHz. Its original UTC start and wall-clock recording duration are unknown.
The audio tuning estimator reports about -9.6 Hz. There is measurable energy
near the shifted 100 Hz code frequency, but energy alone is not a decode.

A separate short-window analysis found long timing-tone candidates at these
file offsets (these are **not decoded UTC times**):

| Candidate start (s) | Interval from previous (s) |
|---:|---:|
| 34.005 | — |
| 91.659 | 57.654 |
| 150.861 | 59.202 |
| 199.659 | 48.798 |
| 245.825 | 46.166 |
| 296.505 | 50.680 |
| 353.167 | 56.662 |
| 405.957 | 52.790 |
| 460.499 | 54.542 |

These irregular intervals are consistent with substantial nonuniform capture
gaps or edits, although interference/false marker detections are alternatives
and the recording has no timing ground truth. It is not justified to reduce
this to a known constant sample-clock error or to claim that all missing bits
can be recovered by changing one sample-rate parameter.

Both fixed-grid and adaptive decoding of this file still produce incomplete
frames. **No trustworthy full UTC decode of this captured audio has been
achieved.** Lowering thresholds or forcing expected markers would conceal
that result. The original recording is preserved locally and is not included
in this public source repository.

Next useful work is a timing tracker anchored to independently validated
second/minute tones that can represent whole missing seconds and retain
explicit erasures, plus a new capture with known start/duration to distinguish
receiver filtering from sample loss. That would permit a fair real-audio
accuracy test. The current tracker handles gradual short gaps, not arbitrary
whole-second deletions or every mixed-station/interference case.

## October 22, 2026: clock outputs and service operation

Added NTP SHM and chrony SOCK producers, explicit configuration-file loading,
foreground supervision and optional double-fork background operation. The
outputs require two consecutive strict minutes with measured minute tones and
fresh capture timestamps. File/recovery results cannot be exported as current
time. See REFCLOCK.md for the capture-time mapping and operational limits.

The streaming harmonic gate now compares the fast timing correlator against
the program-tone source. Its former slow monitor suppressed genuine 5 ms
ticks. A synthetic minute now yielded all 57 scheduled second ticks rather
than only the 14 ticks near the end of the minute.

Validation uses a disposable QEMU Alpine 3.22.7 aarch64 VM with GCC 14.2, ALSA
headers, Python/NumPy and chrony 4.6.1. No existing Slackware VM was changed.
The Linux suite runs 28 Python methods (three local-recording tests skipped),
plus C core, config, SOCK ABI and SHM checks. Background startup/shutdown on
ALSA's null device, failed-start notification, PID locking, configuration
precedence, invalid timestamps and forbidden offline exports are covered.
macOS/clang runs the same portable tests; its ALSA daemon test is skipped.
Address/undefined-behavior sanitizer checks of the refclock and service tests
also passed. No physical ALSA capture hardware was exercised.

The existing real-audio regressions were also run locally: MONO-026 still
requires opt-in recovery, the earlier real-wwv.raw is still rejected as a full
decode, and MONO-027 yields the same three strict UTC frames, 2026/day 294 at
22:45, 22:46 and 22:47. Receiver recordings remain unpublished. These file
checks do not validate live capture timestamps or radio-path accuracy.

The real-time integration harness feeds synthetic audio through the live
decoder and both drivers to an actual chronyd in the guest. It deliberately
uses reference-minus-host offsets of +0.250 s and -0.375 s; this exercises both
signs and detects minute-association errors. chronyd runs with `-x`, with
CAP_SYS_TIME removed from both effective and bounding capability sets. No
chronyd/ntpd runs on the Mac, and no Mac clock configuration was changed.
The supplied Docker recipe and manual GitHub workflow reproduce this test;
the Docker recipe has not been executed locally.

The automated two-sign integration test passed. chronyd selected the SOCK
source (`#* WVSK`); SHM was deliberately configured `noselect`. Raw measurements
were accepted from both sources:

| Injected reference offset | SOCK samples | SHM samples | Raw offset median |
| --- | ---: | ---: | ---: |
| +0.250 s | 54 | 53 | +0.24775 s |
| -0.375 s | 79 | 79 | -0.37737 s |

These are synthetic integration results, not measured off-air accuracy. The
uncalibrated timing detector contributes a few milliseconds of bias. Detailed
consumer and decoder logs are preserved in the chat workspace under
`outputs/linux-refclock-test/evidence`.
