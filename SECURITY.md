# Security policy

## Reporting a vulnerability

Report it privately through GitHub: the repository's **Security** tab,
**Report a vulnerability**
(https://github.com/tomdionysus/SoapyHackRFDuplex/security/advisories/new).
Please do not open a public issue for it.

Say what you found, which version or commit, and how to reproduce it. You
will hear back within seven days. A confirmed vulnerability is fixed in a
release as soon as it can be, with a GitHub security advisory crediting
you unless you would rather not be named, and you will be told before the
advisory is published.

## Supported versions

Fixes go into the latest release only. Versions before 1.0.0 are not
maintained once a newer one is out.

| Version | Supported |
|---|---|
| latest 0.2.x | yes |
| older | no |

## Scope

In scope: the SoapySDR module (`soapysdr0.8-module-hackrfduplex`), the
check tool (`hackrfduplex-tools`), the build and packaging in this
repository, and the release artifacts and their provenance. The module
runs in the application that loads it, takes its device arguments and
settings from that application, and drives two USB devices through
libhackrf; a way for those inputs to crash it, read or write memory it
should not, or key a transmitter it was not asked to is a vulnerability.

Out of scope: the HackRF firmware, libhackrf and SoapySDR themselves
(report those to their projects), and what a transmitter does once an
application has asked it to transmit.

## Verifying a release

Each GitHub Release from 0.2.2 on carries SLSA build provenance
(`*.intoto.jsonl`) for its packages, made by the Release workflow from
the tag. Check a package against it with
[slsa-verifier](https://github.com/slsa-framework/slsa-verifier):

```
slsa-verifier verify-artifact soapysdr0.8-module-hackrfduplex_0.2.2_arm64.deb \
  --provenance-path hackrfduplex-0.2.2.intoto.jsonl \
  --source-uri github.com/tomdionysus/SoapyHackRFDuplex --source-tag 0.2.2
```

The packages of 0.2.0 and 0.2.1, released before the provenance, carry
Sigstore signatures instead (`*.deb.sigstore.json`), made afterwards by
the Sign releases workflow: they say these are the files the project
published, not how they were built. Check one with
[cosign](https://github.com/sigstore/cosign):

```
cosign verify-blob soapysdr0.8-module-hackrfduplex_0.2.1_arm64.deb \
  --bundle soapysdr0.8-module-hackrfduplex_0.2.1_arm64.deb.sigstore.json \
  --certificate-identity-regexp '^https://github.com/tomdionysus/SoapyHackRFDuplex/\.github/workflows/sign-release\.yml@' \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com
```

The apt repository at tomcully.com is signed with its own archive key
(`tomcully.sources` carries it).
