# Licensing

This repository is licensed under the GNU General Public License, version
3 or (at your option) any later version; see [LICENSE](../LICENSE). Every
source file carries an SPDX header:

```
// SPDX-License-Identifier: GPL-3.0-or-later
```

The code is this project's own. It follows the SoapySDR conventions that
applications written for a single HackRF expect, so that they work with
the pair unchanged, and takes no code from any other module. Code taken
from a third-party work would keep that work's licence header and
copyright line beneath the SPDX line.

## Third-party works

| Work | Licence | Use here | Reference |
|---|---|---|---|
| SoapySDR | Boost Software License 1.0 | The API this module implements; linked | https://github.com/pothosware/SoapySDR |
| libhackrf | BSD 3-clause | The board library; linked | https://github.com/greatscottgadgets/hackrf |
| Catch2 | Boost Software License 1.0 | The test framework; linked by the tests only | https://github.com/catchorg/Catch2 |
| SoapyHackRF | MIT | The conventions single-board SoapySDR applications expect of a HackRF (gain element names, the `buffers` stream argument); no code | https://github.com/pothosware/SoapyHackRF |

"HackRF" is the name of Great Scott Gadgets' product.
