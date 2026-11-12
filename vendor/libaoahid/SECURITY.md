# Security policy

Report vulnerabilities privately through the repository's
[GitHub Security Advisories form](https://github.com/nemarpuc/libaoahid/security/advisories/new).
Do not put
device serial numbers, USB traces containing accessory strings, or undisclosed
hardware identifiers in a public issue.

Input reports can contain keystrokes, credentials, precise touch locations, and
medical or accessibility data. libaoahid never sends report payload bytes to its
log sink. Do not add payloads to application logs, crash reports, fixtures, or
public bug reports; reproduce with synthetic non-sensitive input instead.

Raw descriptors are untrusted input. The raw factory validates structure and
wire lengths but makes no Android-support claim. udev rules are examples and
must be reviewed against the host's user/group policy before installation.
