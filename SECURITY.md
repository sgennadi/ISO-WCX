# Security policy

## Reporting a vulnerability

Please do not disclose a vulnerability in a public GitHub issue.
Use the repository's **Security** tab and **Report a vulnerability**
(private vulnerability reporting) when available. If that option is not
enabled, contact the maintainer privately before sharing a proof of concept.
Do not upload private ISO/WIM images, credentials, logs containing paths,
or customer data.

## Scope

ISO-WCX parses untrusted image metadata inside the Total Commander process.
Malformed image handling, out-of-bounds access, unsafe filesystem writes,
update authenticity and Windows privilege handling are security-sensitive.
Do not assume a checksum downloaded beside a ZIP authenticates its publisher.

Security fixes should be tested on both Win32 and x64 before release.
Unsupported and older releases may not receive backported fixes.
