# Validation and captured-audio investigation

Investigation date: October 21, 2026. Tests were run locally on macOS/clang.

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
