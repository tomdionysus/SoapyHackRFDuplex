# Assurance case

Why the module, `soapysdr0.8-module-hackrfduplex`, can be relied on not
to be the way into or out of the process that loads it, and not to
transmit what it was not asked to. Each claim below rests on the
evidence named with it; [SECURITY.md](../SECURITY.md) says how to report
anything that breaks one.

## What it is and where its boundaries lie

The module is a shared library that SoapySDR loads into an application's
process. It has no privileges, files, network connections or
configuration of its own; it runs as the application, which chose to
load it.

| Boundary | What crosses it | Trusted? |
|---|---|---|
| The application, through SoapySDR's API | device arguments and settings (text), stream formats and flags, sample buffers and their lengths, timestamps | No: every value is checked |
| The boards, through libhackrf over USB | sample transfers, firmware and board identification, the streaming engine's state, CLKIN status | No: a board can be a clone, misbehave or vanish mid-stream |
| libhackrf and SoapySDR | the libraries themselves | Yes: from the distribution, at the versions packaged |
| The radio | what the transmitter sends | Only what the application wrote, and the documented calibration burst |

The check tool, `hackrfduplex-check`, is a SoapySDR application like any
other and is run by a person at a terminal; the FT8 example is not
packaged.

## Threats considered

1. **Memory corruption from what the application passes**: a format
   name, a setting, a buffer shorter than claimed, a float that is NaN.
2. **Memory corruption from what a board sends**: a transfer of an
   unexpected size, a serial shared by two boards, an engine state that
   never changes.
3. **A transmitter that keeps transmitting** after the application has
   stopped or died, or transmits something it was not given.
4. **A host that hangs** on a board that stops answering.
5. **A tampered release**: packages that are not what the tag built.

## Claims, and the argument for each

### 1. Input from the application cannot corrupt memory

- Every SoapySDR call checks its direction, channel and stream handle
  before acting, and refuses (throws `std::invalid_argument` or returns
  `SOAPY_SDR_NOT_SUPPORTED`) what it cannot honour rather than ignoring
  it ([design](design.md), "The SoapySDR API against the module").
- Names (formats, clock sources, time origins), counts
  (`tx_underrun_limit`, `buffers`) and sample rates are parsed against a
  closed set or range; anything else is refused by name.
- Samples are converted with lengths the caller gives and the ring's
  block size bounds; a NaN becomes zero; zero samples touch no buffer.
- Evidence: the unit tests against fake boards (100% of lines, 94% of
  branches); the fuzz targets over names, board selection and
  conversions (`fuzz/`), run on every push; the whole suite under
  AddressSanitizer and UndefinedBehaviorSanitizer (with
  float-cast-overflow) in CI.

### 2. Input from the boards cannot corrupt memory

- A transfer is copied by the length libhackrf reports, into blocks of a
  fixed size; a short or empty transfer is handled, not assumed full.
- Board selection refuses boards sharing a serial, and a selection that
  names a board that is not there.
- The binding never hands libhackrf a null device: a failed reopen is a
  `BoardError` naming the board.
- Evidence: the libhackrf binding's tests against a fake libhackrf
  (`module/tests/fake_libhackrf.*`), which makes every call fail in turn;
  the selection fuzzer; the burst search fuzzer over arbitrary captures.

### 3. The transmitter cannot outlive its host or send what it was not given

- The transmitting board stops by itself when it goes
  `tx_underrun_limit` samples without data (on by default, one
  transfer), so a killed application leaves no carrier; a board is
  stopped when its stream is deactivated or closed and when the device is
  destroyed ([design](design.md), "Failure handling").
- It sends only what the application wrote, silence where nothing was
  written, and the short calibration burst, which `time_origin=none`
  turns off.
- Evidence: unit tests; `tools/tx-kill-test.sh`, which kills a
  transmitting check tool and listens for the board stopping, run on the
  bench.

### 4. A board that stops answering cannot hang the host

- Every wait has a timeout; nothing sleeps in the stream paths. A lost
  board, a stopped engine, lost samples and a lost time origin each reach
  the application as a stream status within a bounded time.
- Locking is proved by Clang's thread-safety analysis and exercised under
  ThreadSanitizer.
- Evidence: unit tests of each fault; `tools/unplug-test.sh` on the
  bench (a board pulled mid-stream is a stream error within 0.1 s).

### 5. A release is what its tag built

- Releases are built by GitHub Actions from the tag, in a pinned Debian
  image, with every action pinned to a commit; the packages carry SLSA
  build provenance, signed through Sigstore, which `slsa-verifier`
  checks (SECURITY.md).
- The apt repository is signed with its own archive key, which
  `tomcully.sources` carries; apt refuses anything it did not sign.
- `main` is protected: it changes only through a pull request that CI,
  CodeQL and fuzzing have passed.

## Secure design principles applied

- **Least privilege**: no privileges, files, network or configuration;
  discovery never opens a board.
- **Fail-safe defaults**: the transmit underrun limit is on; an unknown
  or unsupported request is refused, never silently ignored.
- **Complete mediation**: every call checks its stream and channel.
- **Economy of mechanism**: the core is plain C++ without libhackrf or
  SoapySDR; one file calls each library and only translates.
- **Separation**: receive and transmit run on separate boards, rings and
  threads; tuning one never disturbs the other.

## Common weaknesses countered

| Weakness | Countered by |
|---|---|
| Out-of-bounds write or read (CWE-787, CWE-125) | standard containers and spans, lengths checked against block sizes; AddressSanitizer; fuzzing |
| Use after free (CWE-416) | RAII ownership; libhackrf's thread joined before a handler is cleared; AddressSanitizer |
| NULL dereference (CWE-476) | the binding's handle check; zero-length conversions return first |
| Integer overflow or wraparound (CWE-190) | `-Wconversion -Werror`; counts parsed against their range; UBSan |
| Undefined float conversion | NaN made zero; UBSan float-cast-overflow |
| Race condition (CWE-362) | annotated locks proved by Clang's thread-safety analysis; ThreadSanitizer |
| Uncontrolled resource use (CWE-400) | fixed-size rings; a timeout on every wait |
| Improper input validation (CWE-20) | closed sets and ranges for every name, count and rate; refusal by name |

Static analysis backs these on every push: CodeQL's security and quality
queries, clang-tidy with a checked-in configuration, and the compiler's
warnings as errors.
