# bcm-ex

**bcm-ex** is the extended line of [BCM]: Ilya Muravyov's BWT-based compressor,
kept alive and made to build and run on current systems, with **the compressed
stream unchanged**.

- **Byte-compatible with BCM 1.65.** Same `.bcm` files in both directions: a
  stream written here decodes with `bcm` 1.65 and the other way round. For the
  same input and block size, both write the same bytes — checked against the
  official `bcm.exe` 1.65 on every push.
- **Builds where BCM 1.65 does not.** Linux x86, x86_64 and ARM64; Windows x86
  and x64 (Windows 7 SP1 or newer); macOS arm64.
- **No external dependencies.** `libsais` 2.12.2 is vendored and linked
  statically, so `make` produces a single self-contained executable.
- **Safer on damaged input.** A corrupted or truncated stream is reported
  instead of trusted; BCM 1.65 crashed on Windows (`ptr[-1]` in the inverse
  BWT) and is fixed here.
- **No POSIX-only APIs**, and files past 2 GB work on 32-bit builds too,
  including the Windows ones.

| Component | Status |
|---|---|
| CI | [![CI][ciBadge]][ciLink] |
| Latest release | [![Release][relBadge]][relLink] |
| Stream compatibility with BCM 1.65 | Byte-identical, [checked on every push][ciLink] |
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
make check                    # round-trip, robustness and command line tests
make windows                  # bcm.exe for Windows x64 with mingw-w64
make windows32                # bcm.exe for Windows x86 with mingw-w64

./bcm file                    # compress to file.bcm
./bcm -b100 file              # 100 MB blocks: smaller output, more memory
./bcm -d file.bcm             # decompress
./bcm -t file.bcm             # verify a compressed file, write nothing
cat file | ./bcm -b16 - out.bcm   # standard input and output with '-'
```

`-9` and the other level numbers are accepted and ignored, as in BCM 1.65, so
existing scripts keep working.


How small is it?
-------------------------

10,000,000 bytes of synthetic text, one core:

| | Size |
| --- | ---: |
| gzip -9 | 5,979,159 (59.79%) |
| bzip2 -9 | 4,138,099 (41.38%) |
| zstd -19 | 3,509,668 (35.10%) |
| xz -9 | 3,524,780 (35.25%) |
| **bcm-ex -b16** | **2,844,813 (28.45%)** |

On the upstream project's [enwik8](http://mattmahoney.net/dc/text.html) figure
(100,000,000 bytes): GZIP -9 36,445,248, BZIP2 -9 29,008,758, 7-Zip Ultra
24,864,804, **BCM -b100 20,789,667**.

Speed is BCM 1.65's: this line does not touch the compressor, and both were
measured at the same times on the same files. Details in the wiki's
[benchmarks][wBench] page.


Testing
-------------------------

```sh
make check                                   # behaviour + compatibility
sh tests/check-cross.sh                      # every cross build writes the same bytes
sh tests/fuzz-decoder.sh ./bcm 500           # damaged streams must not crash
```

`tests/check-compat.sh` compares against output recorded from the official
`bcm.exe` 1.65 and must never be updated: a failure there means the format
changed. `tests/fuzz-decoder.sh` covers the class of bug that made BCM 1.65
crash on Windows. See [testing][wTest].


License
-------------------------

bcm-ex keeps BCM's Apache License 2.0; see [LICENSE](LICENSE). `libsais` is
Ilya Grebnov's, Apache License 2.0 with LLVM Exceptions; see
[third_party/libsais/LICENSE](third_party/libsais/LICENSE). See
[AUTHORS](AUTHORS) and [NOTICE](NOTICE) for the copyright holders.

BCM was created by Ilya Muravyov. This line only updates the build and the
command line around his compressor and does not change what it produces.

**bcm-ex** is maintained by Yade Wira Bravo ([@YadeWira](https://github.com/YadeWira)).
