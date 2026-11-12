# Building from source

## Dependencies

Debian or Ubuntu:

```
sudo apt install build-essential cmake pkgconf libhackrf-dev libusb-1.0-0-dev libsoapysdr-dev catch2
```

`libusb-1.0-0-dev` is needed although nothing here calls libusb:
Debian's `libhackrf-dev` names libusb's header directory in its
pkg-config file without depending on the package that provides it.

macOS with Homebrew:

```
brew install cmake hackrf soapysdr catch2
```

## Build, test, install

From the repository root:

```
cmake -S . -B build
cmake --build build -j
ctest --test-dir build
sudo cmake --install build
```

`cmake --install` puts the module in SoapySDR's modules directory under
the install prefix, and `hackrfduplex-check` and its man page under
`bin` and `share/man`.

## Debian packages

On Debian trixie, `debian/` builds two packages: the module,
`soapysdr0.8-module-hackrfduplex`, and the check tool,
`hackrfduplex-tools`.

```
sudo apt install build-essential debhelper cmake pkgconf libhackrf-dev libusb-1.0-0-dev libsoapysdr-dev catch2
dpkg-buildpackage -us -uc -b
sudo apt install ../soapysdr0.8-module-hackrfduplex_*.deb ../hackrfduplex-tools_*.deb
```

## Running from the build tree

Point SoapySDR at the build tree's module to use it without installing:

```
export SOAPY_SDR_PLUGIN_PATH=$PWD/build/module
SoapySDRUtil --find="driver=hackrfduplex"
./build/tools/hackrfduplex-check --clkout rx --seconds 5
```

Do not leave an installed module in place while doing this: both
register the same driver.

## Options

| Option | Effect |
|---|---|
| `-DHACKRFDUPLEX_DSP=ON` | receive DC removal and I/Q balance correction, with liquid-dsp (`sudo apt install libliquid-dev`); without it the module neither links liquid-dsp nor offers the calls |
| `-DHACKRFDUPLEX_BUILD_MODULE=OFF` | skip the SoapySDR module, and the need for SoapySDR |
| `-DHACKRFDUPLEX_BUILD_TESTS=OFF` | skip the unit tests, and the need for Catch2 |
| `-DHACKRFDUPLEX_BUILD_TOOLS=OFF` | skip `hackrfduplex-check` |
| `-DHACKRFDUPLEX_SANITIZE=ON` | AddressSanitizer and UndefinedBehaviorSanitizer (`cmake --preset asan`) |
| `-DHACKRFDUPLEX_COVERAGE=ON` | gcov instrumentation and a `coverage` target (`cmake --preset coverage`) |

The full set of checks a change must pass, `tools/ci.sh`, is described
in [CONTRIBUTING.md](../CONTRIBUTING.md).
