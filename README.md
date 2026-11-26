# bcm-ex

**bcm-ex** is the extended line of [BCM]: Ilya Muravyov's BWT-based compressor,
kept alive and made to build and run on current systems.

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

./bcm file                    # compress to file.bcm (BCM2)
./bcm -b100 file              # 100 MB blocks: smaller output, more memory
./bcm -d file.bcm             # decompress; BCM! files from 1.x work too
./bcm -t file.bcm             # verify a compressed file, write nothing
./bcm -x- file                # never apply the x86 filter
cat file | ./bcm -b16 - out.bcm   # standard input and output with '-'
```


How it compares
-------------------------

Silesia (211,938,580 bytes) and enwik8 (100,000,000 bytes), 100 MB blocks:

| | bcm-ex 1.66 | **bcm-ex 2.0** | BCM 2.03 | 
|---|---:|---:|---:|
| Silesia total | 46,506,716 | **46,105,798** | 45,848,916 |
| ooffice (Windows DLL) | 2,543,053 | **2,142,069** | 2,122,685 |
| enwik8 | 20,789,667 | **20,789,673** | 20,722,105 |

bcm-ex 2.0 is **0.56% larger** than BCM 2.03 across Silesia and 0.32% larger on
enwik8, and it **wins on 4 of the 12 Silesia files**. What it has that 2.03 does
not is the ability to read the 1.x archive. The measured details, including
where each compressor wins, are in [benchmarks][wBench].


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
build, the container and the command line around the compressor; the
compression model itself is unchanged.

**bcm-ex** is maintained by Yade Wira Bravo ([@YadeWira](https://github.com/YadeWira)).
