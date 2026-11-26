# bcm-ex

**bcm-ex** is the extended line of [BCM], Ilya Muravyov's BWT-based compressor:
the BCM 1.65 code base, kept alive and made to build and run on current systems,
with **the compressed stream unchanged**.

- **Byte-compatible with BCM 1.65.** Same `.bcm` files in both directions: a
  stream written here decodes with `bcm` 1.65 and the other way round. For the
  same input and block size, both write the same bytes — checked against the
  official `bcm.exe` 1.65 on every push.
- **Builds where BCM 1.65 does not.** Linux x86, x86_64 and ARM64; Windows x86
  and x64 (Windows 7 SP1 or newer); macOS arm64.
- **No external dependencies.** `libsais` is vendored and linked statically, so
  `make` produces a single self-contained executable.
- **Safer on damaged input.** A corrupted or truncated stream is reported
  instead of trusted; BCM 1.65 crashed on Windows (`ptr[-1]` in the inverse
  BWT) and is fixed here.
- **No POSIX-only APIs**, and file sizes past 2 GB are handled on 32-bit builds
  too, including the Windows ones.

| Component | Status |
|---|---|
| CI | [![CI][ciBadge]][ciLink] |
| Latest release | [![Release][relBadge]][relLink] |
| Stream compatibility with BCM 1.65 | Byte-identical, [checked on every push][ciLink] |
| Linux x86_64 | Built and tested |
| Linux i386 | Built and tested |
| Linux ARM64 | Built and tested |
| Windows x64 (Windows 7 SP1+) | Built and tested |
| Windows x86 (Windows 7 SP1+) | Built and tested |
| macOS arm64 | Built by [CI][ciLink]; not run on a Mac by the author yet |

[BCM]: https://github.com/FS-make-simple/bcm
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
./bcm -b100 file              # 100 MB blocks (smaller output, more memory)
./bcm -d file.bcm             # decompress
./bcm -t file.bcm             # verify a compressed file, write nothing
cat file | ./bcm -b1 - out.bcm    # standard input and output with '-'
```

On Windows, `bcm file` and `bcm -d file.bcm` are the same commands; `-` works
for pipes there too.


Compatibility
-------------------------

The stream layout, the context model and the arithmetic coder are BCM 1.65's,
untouched. What changed around them:

| | BCM 1.65 | bcm-ex |
|---|---|---|
| Stream format | — | identical, byte for byte |
| `libsais` | 2.2.0, linked with `-lsais` | 2.12.2, vendored and static |
| Linux x86 / x86_64 / ARM64 | source only | built and tested |
| Windows x86 / x64 | prebuilt `.exe`, MSVC only | mingw-w64, Windows 7 SP1+ |
| macOS arm64 | — | built and tested |
| Files larger than 2 GB on 32-bit | broken | handled (64-bit offsets) |
| Damaged input | undefined behaviour, crashes | reported as an error |
| Truncated input | "CRC error" after writing a partial file | same, and `-t` writes nothing |
| `-b0`, `-b4096`, `-b` | out of range not detected | rejected with a message |
| `-b#` digits | `atoi`, so `-b16x` worked by accident | parsed and validated |
| Standard input / output | — | `-` |
| Verify without writing | — | `-t` |
| `--help`, `-h`, `--version`, `-V` | — | yes |
| Input timestamps copied to the output | yes, and `exit(1)` on failure | yes, and a failure is only a warning |

`-9` and the other level numbers are still accepted and ignored, because BCM
1.65 accepted them and ignored them too: any script written for 1.65 keeps
working unchanged.


The format, in one paragraph
-------------------------

A `.bcm` file starts with the four bytes `BCM!` and then an arithmetic-coded
bit stream. The stream holds, per block, the block length, the BWT index and the
BWT-transformed bytes coded with a binary context model (order-0, order-1 and an
order-2 SSE stage), followed by a zero length and the CRC-32 of the whole input.
The block size is chosen at compression time and recorded in the stream, so a
file compressed with `-b100` decodes without passing any option.


Building
-------------------------

Requirements: a C++11 compiler and a C compiler. `libsais` is in
`third_party/libsais`, so there is nothing to install.

```sh
make                                  # native build
make CC=clang CXX=clang++             # with clang
make CXXFLAGS="-O2 -march=native"     # tuned for this machine
```

Cross-compiling for Windows needs mingw-w64 (`make windows` / `make windows32`);
`make windows` produces a statically linked `.exe` with no DLL requirement.


Testing
-------------------------

```sh
make check                                   # the test suite
sh tests/check-compat.sh ./bcm               # byte-identical to BCM 1.65
sh tests/check-cross.sh                      # every cross build writes the same bytes
sh tests/fuzz-decoder.sh ./bcm 500           # damaged streams must not crash
```

`tests/check-compat.sh` compares against output recorded from the official
`bcm.exe` 1.65 and must never be updated: a failure there means the format
changed. `tests/fuzz-decoder.sh` covers the class of bug that made BCM 1.65
crash on Windows.


License
-------------------------

bcm-ex keeps BCM's Apache License 2.0; see [LICENSE](LICENSE). `libsais` is
Ilya Grebnov's, Apache License 2.0 with LLVM Exceptions; see
[third_party/libsais/LICENSE](third_party/libsais/LICENSE).

BCM was created by Ilya Muravyov. This line only updates the build and the
command line around his compressor and does not change what it produces.
