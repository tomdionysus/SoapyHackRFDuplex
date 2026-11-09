# Licensing

This repository is licensed under the GNU General Public License, version
3 or (at your option) any later version; see [LICENSE](../LICENSE). Every
source file carries an SPDX header:

```
// SPDX-License-Identifier: GPL-3.0-or-later
```

The project began in 2023 as a fork of SoapyHackRF, which is MIT
licensed; its notice is kept in [NOTICE](../NOTICE). Code taken from a
third-party work keeps that work's licence header and copyright line
beneath the SPDX line.

## Third-party works

| Work | Licence | Use here | Reference |
|---|---|---|---|
| SoapyHackRF | MIT | The 2023 starting point; the gain element names and the stream argument it established are kept for compatibility | https://github.com/pothosware/SoapyHackRF |
| SoapySDR | Boost Software License 1.0 | The API this module implements; linked | https://github.com/pothosware/SoapySDR |
| libhackrf | BSD 3-clause | The board library; linked | https://github.com/greatscottgadgets/hackrf |
| Catch2 | Boost Software License 1.0 | The test framework; linked by the tests only | https://github.com/catchorg/Catch2 |

"HackRF" is the name of Great Scott Gadgets' product.
