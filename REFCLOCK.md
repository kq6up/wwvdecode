# Feeding chrony or ntpd

The decoder can supply **chrony SOCK** samples and **NTP SHM** samples,
separately or together. It never changes the system clock. The receiving time
service decides whether to use the source and whether to adjust its host clock.
Outputs are disabled unless `--sock` or `--shm-unit` is configured.

## Live operation

On Linux, build with `make` after installing ALSA development headers. Start
chronyd before the SOCK producer, then use a configuration based on
[examples/wwvdecode.conf](examples/wwvdecode.conf):

```sh
./wwvdecode --config /absolute/path/wwvdecode.conf --check-config
./wwvdecode --config /absolute/path/wwvdecode.conf --foreground
```

Configuration is loaded **only** with `-c FILE` / `--config FILE`. Each line is
`key = value`; blank lines and whole-line `#` comments are allowed. Values are
literal: no quoting, shell expansion, includes, or inline comments. Unknown
keys and malformed values cause an error. CLI settings override file settings
regardless of option order. Multiple config files are applied in order. There
is no runtime reload; restart to change settings. All supported settings are
shown in the example file. `--check-config` does not open audio, create shared
memory, fork, write PID files, or connect to chrony.

The default process stays in the foreground for a supervisor. Traditional
background operation is also available:

```sh
./wwvdecode --config /absolute/path/wwvdecode.conf --daemon
```

Background mode requires absolute `pidfile` and `logfile` paths with writable
parent directories. It double-forks, detaches from the terminal, changes its
working directory to `/`, and redirects stdin to `/dev/null` and stdout/stderr
to the log. The original parent waits for successful ALSA setup; startup
failures return an error. The PID file remains locked for the process lifetime.
Stop with `kill -TERM PID`; the process removes its PID file on orderly exit.
A stale unlocked PID file can be reused. Log rotation requires a restart.
No user/group privilege switching or service installation is performed.

### Choosing the output

- **SOCK:** `sock=/run/chrony/wwv.sock`. chronyd creates the Unix datagram socket.
  Give the producer directory traversal and socket write permissions. Start the
  consumer first; the producer also retries when it is absent or restarts.
  Samples have absolute UTC (`pulse=0`), not an unlabeled PPS pulse.
- **SHM:** `shm-unit=2` (default `-1` disables it). Choose an unused unit,
  especially when gpsd is also installed. Key is `0x4e545030 + unit`.
  New segments use mode 0600. Existing segments must have the same owner,
  compatible ABI size, and no group/world write permission. Run the producer
  and the consumer's initial attachment under the matching UID. The producer
  uses the standard mode-1 count/valid protocol, with microsecond and nanosecond
  fields. A unit lock prevents multiple wwvdecode producers under that UID.
  It cannot detect a different program writing the same unit. Stop other
  producers before choosing that unit. The segment persists across restarts;
  shutdown invalidates its sample. No world-writable segment is created.

See [chrony configuration](examples/chrony-wwv.conf) and
[NTPsec/legacy ntpd configuration](examples/ntpsec-wwv.conf). These are snippets,
not automatically installed configurations. Start with `noselect` for field
measurements. Use one of the two interfaces for a real source; using both does
not provide independent time references.

## Minute association and capture timing

A code frame describes UTC at its **starting** minute boundary. Its final bit
arrives nearly a minute later. Exporting that decoded minute as the current
clock would introduce a minute error, and sending the original timestamp pair
would be too old for SHM consumers.

The live decoder therefore requires two consecutive, fully validated frames,
including independently observed long minute tones. It then labels **newly
observed** second ticks with UTC. No minute is added to a historical sample and
presented as if it were just captured. Markers, reserved bits, BCD/calendar
fields and status symbols must pass the strict decoder. Recovery-mode results
never feed a clock service. The two-digit year is interpreted as 2000–2099;
there is no automatic century inference.

Bad frames, inconsistent minutes, timestamp discontinuities, or a system clock
step clear UTC lock. Two new valid minutes are required. Publication expires
if the anchor is not refreshed. Duplicate ticks and samples older than two
seconds are rejected. No ticks means no new samples: it does not manufacture
PPS ticks from a free-running timer. Second 29 and 59 ticks are intentionally
absent from WWV broadcasts. Any leap warning suspends publication because the
WWV bit does not communicate leap direction; explicit leap-event handling is
not implemented. Live timing acquisition currently supports WWV 1000/1500 Hz,
not WWVH's 1200 Hz minute tone. Offline WWVH decoding remains available.

ALSA uses enabled monotonic status timestamps and capture-queue delay to map
audio samples to host time. The real/monotonic mapping is refreshed for every
block. Capture overruns discard lock; they cannot silently compress the time
axis. Hardware, resamplers and plugins can introduce additional latency, so
this is not a hardware-timestamped GPS PPS accuracy claim. Real ALSA hardware
capture timing has not yet been calibrated or validated.

`correction` / `--correction` adds seconds to the transmitted reference time.
For example, a measured net path/receiver/audio/detection delay of 0.012 s
requires a **positive** correction of 0.012 s. Leave it zero until measured.
`precision` is the SHM base-two exponent (default -5, about 31 ms); SOCK's
precision/dispersion must be configured in chrony. Neither value compensates
an unknown systematic receiver delay. DUT1 is reported separately and is not
applied to UTC.

Ordinary `-i` recordings and `--raw` pipes are forbidden with clock outputs:
they do not contain capture timestamps and may run faster than real time.

### Timestamped live PCM input

A capture helper or test generator can instead use `--timed-raw`. Each block
is an ASCII header followed immediately by exactly `frames * 2` binary bytes:

```text
WWV1 first_realtime_seconds first_monotonic_seconds frames\n
<S16_LE mono PCM bytes>
```

`frames` must be 1..1024. Times identify the **first sample** in the block,
using the receiving host's CLOCK_REALTIME/CLOCK_MONOTONIC domains, and must be
fresh. Headers have a real newline; do not include the literal characters
`\n`. Rate is selected with `--rate` (default 8000). Blocks must be continuous;
time jumps reset the decoder. The helper must measure capture time rather
than use pipe write/arrival time. This interface is foreground-only, does not
support automatic tone-offset acquisition, and is not a timestamp file replay
mode. Use a manual `--offset` if needed. The supplied generator uses this
protocol to exercise the same live decoder and output drivers.

## Isolated synthetic integration test

`make test` runs C validation and Python decoder/service tests. Optional real
recordings remain local; see VALIDATION.md. The deeper Linux integration test
requires a disposable VM or container, root **inside that guest only**, ALSA
headers, chrony, capsh, Python and NumPy:

```sh
make test
WWV_TEST_ARTIFACTS=/tmp/wwv-evidence python3 test/chrony_integration.py
```

It feeds two real-time synthetic recordings with known +0.250 s and -0.375 s
reference offsets through the decoder. An actual chronyd consumes SOCK and SHM.
It verifies at least eight raw measurements of each sign from each driver,
checks that chronyd reports clock control disabled, and checks that
CAP_SYS_TIME is absent from its effective and bounding capability sets. No
network time servers or network listening NTP port are configured. The run
takes about seven minutes because acquisition and audio run in real time.
The synthetic offset is deliberate: do not point this test at a production
clock service.

A reproducible container definition is supplied:

```sh
docker build -f test/Dockerfile -t wwvdecode-test .
docker run --rm --cap-drop=SYS_TIME --network=none wwvdecode-test
```

The implementation was exercised in a separate QEMU Alpine Linux VM; the
Docker recipe is supplied for reuse, but has not been run on this Mac. No
chronyd/ntpd was launched on the Mac and no Mac clock settings were changed.

Protocols: [chrony SOCK source](https://github.com/mlichvar/chrony/blob/master/refclock_sock.c),
[NTPsec SHM driver](https://docs.ntpsec.org/latest/driver_shm.html),
[chronyd -x](https://chrony-project.org/doc/4.8/chronyd.html), and
[ALSA capture status](https://www.alsa-project.org/alsa-doc/alsa-lib/group___p_c_m___status.html).
