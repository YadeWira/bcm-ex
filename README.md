# bcm-ex

**bcm-ex** is the extended line of [BCM]: Ilya Muravyov's BWT-based compressor,
kept alive and made to build and run on current systems.

- **Two new models that beat BCM 2.03 on every Silesia file.** `-m1` (the
  default) is 1.7% smaller than BCM 2.03 over Silesia; `-m2` is 2.1% smaller
  and about twice as slow. BCM's own model is still there as `-m0`.
- **Reads everything BCM 1.x wrote.** The `BCM!` streams of BCM 1.65 and of
  bcm-ex 1.x decode byte for byte. BCM 2.03 does *not* do this: it rejects them
  with `BCM1 is not supported`.
- **Writes a new container, `BCM2`**, so the format can carry a preprocessing
  stage and a method field. This is a deliberate break with 1.x — see
  [compatibility][wCompat].
- **An x86 filter that decides for itself.** On machine code it shrinks the
  output by **15.8%**; on everything else it costs a fraction of a percent, so
  it is applied only when the data says it will pay (measured: 9.4% of a
  Windows DLL is rewritten, 0.24% of a tar of mixed files).
- **Builds where BCM 1.65 does not.** Linux x86, x86_64 and ARM64; Windows x86
  and x64 (Windows 7 SP1 or newer); macOS arm64. `libsais` is vendored and
  linked statically, so `make` produces one self-contained executable.
- **Safer on damaged input.** A corrupted or truncated stream is reported
  instead of trusted; BCM 1.65 crashed on Windows (`ptr[-1]` in the inverse
  BWT) and is fixed here.

| Component | Status |
|---|---|
| CI | [![CI][ciBadge]][ciLink] |
| Latest release | [![Release][relBadge]][relLink] |
| Reads BCM 1.x (`BCM!`) | Yes, byte for byte, [checked on every push][ciLink] |
| Writes `BCM2` | Yes; not readable by BCM 1.x or by BCM 2.03 |
| x86 filter | Automatic, −15.8% on executables |
| Linux x86_64 | Built and tested |
| Linux i386 | Built and tested |
| Linux ARM64 | Built and tested |
| Windows x64 (Windows 7 SP1+) | Built and tested, also on a real Windows 7 SP1 |
| Windows x86 (Windows 7 SP1+) | Built and tested, also on a real Windows 7 SP1 |
| macOS arm64 | Built and passed the test suite in [CI][ciLink]; not run on a Mac by the author yet |

**Documentation is in the [wiki]**: [getting started][wGS], [building][wBuild],
[compatibility][wCompat], [format][wFormat], [command line][wCL],
[platform support][wPlat], [safety fixes][wSafe], [testing][wTest],
[benchmarks][wBench] and [FAQ][wFAQ].

[BCM]: https://github.com/FS-make-simple/bcm
[wiki]: https://github.com/YadeWira/bcm-ex/wiki
[wGS]: https://github.com/YadeWira/bcm-ex/wiki/Getting-Started
[wBuild]: https://github.com/YadeWira/bcm-ex/wiki/Building
[wCompat]: https://github.com/YadeWira/bcm-ex/wiki/Compatibility
[wFormat]: https://github.com/YadeWira/bcm-ex/wiki/Format
[wCL]: https://github.com/YadeWira/bcm-ex/wiki/Command-Line
[wPlat]: https://github.com/YadeWira/bcm-ex/wiki/Platform-Support
[wSafe]: https://github.com/YadeWira/bcm-ex/wiki/Safety-Fixes
[wTest]: https://github.com/YadeWira/bcm-ex/wiki/Testing
[wBench]: https://github.com/YadeWira/bcm-ex/wiki/Benchmarks
[wFAQ]: https://github.com/YadeWira/bcm-ex/wiki/FAQ
[ciBadge]: https://github.com/YadeWira/bcm-ex/actions/workflows/ci.yml/badge.svg "Continuous Integration"
[ciLink]: https://github.com/YadeWira/bcm-ex/actions/workflows/ci.yml
[relBadge]: https://img.shields.io/github/v/release/YadeWira/bcm-ex "Latest release"
[relLink]: https://github.com/YadeWira/bcm-ex/releases/latest


Quick start
-------------------------

```sh
make                          # builds ./bcm
make check                    # round-trip, 1.x reading, robustness, command line
make windows                  # bcm.exe for Windows x64 with mingw-w64
make windows32                # bcm.exe for Windows x86 with mingw-w64
make OPENMP=0                 # without threads (used by default if available)

./bcm file                    # compress to file.bcm (BCM2)
./bcm -b100 file              # 100 MB blocks: smaller output, more memory
./bcm -d file.bcm             # decompress; BCM! files from 1.x work too
./bcm -t file.bcm             # verify a compressed file, write nothing
./bcm -m2 file                # stronger model: smaller, ~2x slower than -m1
./bcm -m0 file                # BCM's model: what bcm-ex 2.0 wrote, ~3x faster
./bcm -x- file                # never apply the x86 filter
./bcm -j8 file                # 8 threads for the BWT (default 4; same output)
cat file | ./bcm -b16 - out.bcm   # standard input and output with '-'
```


How it compares
-------------------------

Silesia (211,938,580 bytes) and enwik8 (100,000,000 bytes), 100 MB blocks:

| | bcm-ex 2.0 | **bcm-ex 2.1 `-m1`** | **bcm-ex 2.1 `-m2`** | BCM 2.03 |
|---|---:|---:|---:|---:|
| Silesia total | 46,105,798 | **45,059,460** | **44,894,070** | 45,848,916 |
| ooffice (Windows DLL) | 2,142,069 | **2,091,636** | **2,083,101** | 2,122,685 |
| enwik8 | 20,789,673 | **20,577,437** | **20,533,317** | 20,722,105 |

Both new models are smaller than BCM 2.03 on all 12 Silesia files and on
enwik8. Time and memory for enwik8 on one machine (Linux x86_64, 4 threads for
the BWT; the models themselves run on one thread):

| | compress | decompress | memory |
|---|---:|---:|---:|
| `-m0 -b100` | 11.4 s | 9.6 s | 493 MB |
| `-m1 -b100` | 29.5 s | 31.7 s | 554 MB |
| `-m2 -b100` | 53.7 s | 54.3 s | 667 MB |
| `-m1 -b16` | 29.7 s | 33.2 s | 167 MB |

The measured details, including where each compressor wins, are in
[benchmarks][wBench].


Testing
-------------------------

```sh
make check                                   # behaviour, 1.x reading, filters
sh tests/check-cross.sh                      # every cross build writes the same bytes
sh tests/fuzz-decoder.sh ./bcm 500           # damaged streams must not crash
```

`tests/compat165/` keeps the recorded BCM 1.65 streams, and `check-compat.sh`
requires all of them to decode correctly. See [testing][wTest].


License
-------------------------

bcm-ex keeps BCM's Apache License 2.0; see [LICENSE](LICENSE). `libsais` is
Ilya Grebnov's, Apache License 2.0 with LLVM Exceptions; see
[third_party/libsais/LICENSE](third_party/libsais/LICENSE). See
[AUTHORS](AUTHORS) and [NOTICE](NOTICE) for the copyright holders.

BCM was created by Ilya Muravyov. The x86 filter follows the BCJ algorithm that
Igor Pavlov published in the LZMA SDK (public domain). This line updates the
build, the container and the command line around the compressor. BCM's model
is kept unchanged as `-m0`; the mixing models of `-m1` and `-m2` (`src/cm1.h`,
`src/cm2.h`) are new in bcm-ex 2.1.

**bcm-ex** is maintained by Yade Wira Bravo ([@YadeWira](https://github.com/YadeWira)).
