# ISO-WCX operation diagnostics and GitHub pre-build review

## What ISO-WCX collects (RC preview)

Every logical WCX operation produces a bounded, local JSONL record:
- archive open/close
- F5 copy/replace and F7 folder creation (the WCX pack path)
- Delete
- extract and test
- WIM processing stages, with 10% progress throttling

Repeated per-entry `PK_SKIP` enumeration is **not** individually logged:
a huge `install.wim` could contain hundreds of thousands of entries.
The single `CloseArchive` record includes the total enumerated count,
preventing logging itself from slowing down WIM opening.

Directory: `%LOCALAPPDATA%\ISO-WCX\Logs`

- `operations.jsonl`: current journal, max 4 MiB
- `operations.previous.jsonl`: previous rotated journal, max 4 MiB
- `edit-errors.log`: existing, separate, detailed local editor errors
- `../Reports/ISO-WCX-Diagnostics-Summary.md`: a manually exported,
  aggregated and sanitized report

Each operation has a start/completion pair with transient operation ID,
UTC time, RC/build number, action, format, result code, error **category**,
file count, WIM index when known, duration and a limited set of stages.

**Not collected in the operation journal:** archive paths, source/destination
paths, filenames, the contents of ISO/WIM, hostname, username, machine ID,
email, password, BitLocker recovery keys, cookies or GitHub credentials.
The separate old `edit-errors.log` may include Windows exception paths; it
is never attached to, or parsed into, the GitHub public report automatically.

There is no HTTP request when a WCX operation fails. Logging must not
alter return values, delay copy progress or prevent Total Commander from
opening a file if the logging directory is unavailable.

## Operator actions (Settings)

**Export diagnostics...** opens the local Markdown aggregate.
**Review on GitHub...** opens a prefilled public Issue with a *limited,
sanitized aggregate* (up to 2600 characters); the operator must review
and click Submit on GitHub to publish it. The plugin contains no GitHub
token and does not silently transmit diagnostic data.

**Failed area + Save in shared diagnostics queue...** is a second,
independent option. It uses the canonical C++ embedded core from
`sgennadi/UniversalDiagnocticsModule` (vendored byte-for-byte under
`src/vendor/`) and, only after a Yes confirmation, stores one
`universal-diagnostics/v1` event in the common local queue:

`%LOCALAPPDATA%\UniversalDiagnostics\<project-hash>\<UUID>.json`

This atomic event includes a SHA-256 error fingerprint and delivery state
`pending`. It is **not submitted** automatically. The optional central
UniversalDiagnostics CLI can later review and submit pending events with
explicit approval. It is not a runtime dependency of ISO-WCX.

## Pre-build analysis in GitHub Actions

The Preview and Stable workflows have a dedicated `diagnostics` job
which must succeed **before** compiler jobs begin.

`python3 .github/scripts/analyze-user-telemetry.py --fail-confirmed`

The analyzer reads the last 30 days of **user-reviewed and published**
GitHub Issues with title prefix `ISO-WCX Diagnostics:`. Only strict
whitelisted aggregate table rows are parsed; raw issue text never becomes
an executable command. Each run publishes a short Actions Step Summary
and a JSON artifact.

Reports are grouped by RC/build, optical format, operation, WCX error code
and category. Expected cancellation (21) and unsupported inputs (24) do
not count as confirmed critical regressions.

Repeated serious failures stop the build **only** when a project maintainer
adds the `iso-wcx-regression-confirmed` label and the reviewed report
contains at least three occurrences of an integrity, verification or
unexpected-exception failure. Ordinary untrusted user submissions cannot
block CI. If the Issues API is unavailable, Actions reports telemetry as
**unavailable**, not `0 errors`, and still runs code tests.

The shared UniversalDiagnostics service can eventually unify this
GitHub-side analyzer across all projects. Until an authenticated delivery
endpoint or user-consented GitHub OAuth workflow is configured, there is
**no unattended user-to-GitHub upload**.

## Testing

- `tests/test_telemetry_analysis.py`: offline safety / critical gating
- `tests/test_wcx_iso_edit.py`: Windows x86/x64 WCX F5/Delete plus JSONL
  file format, successful/failing operations and path-redaction checks
- `tests/universal_diagnostics_contract.cpp`: shared event schema
- `tests/universal_diagnostics_windows.cpp`: native C++ pending queue,
  atomic receipt and secret-redaction checks
- CTest, ASan/UBSan and VS2026 build lanes still run as before

The real AOMEI WinPE ISO requires separate functional and BIOS/UEFI boot
tests. Green diagnostics tests alone do not authorize Stable release.
