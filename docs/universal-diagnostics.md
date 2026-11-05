# Shared UniversalDiagnostics — ISO-WCX pilot (version 0.21.0 unchanged)

The native Total Commander WCX plugin has an **optional, manually invoked**
connection to the same [UniversalDiagnocticsModule](https://github.com/sgennadi/UniversalDiagnocticsModule)
used by DISM GUI 4.3.10. It never sends anything from F5, read, extract,
mount, commit, error callbacks or background updater checks.

## Setup

Install the collector once for the current Windows user with the
[`Install-UniversalDiagnostics.ps1`](https://github.com/sgennadi/UniversalDiagnocticsModule/blob/main/Install-UniversalDiagnostics.ps1)
script from the private central repository. The verified Windows x64
collector is stored at:

`%LOCALAPPDATA%\UniversalDiagnostics\UniversalDiagnostics.exe`

A 32-bit Total Commander process can use the common x64 helper on 64-bit
Windows; a true 32-bit Windows system needs a native 32-bit collector.
Alternatively, specify an absolute path via `UNIVERSAL_DIAGNOSTICS_EXE`.

## In Total Commander

Open the ISO-WCX configuration/settings window (WCX ConfigurePacker).
Select a category under **Failed area** (for example ISO file copy/F5,
WIM file copy/F5, ISO/UDF edit, mount/commit, or read/extract), then select
**Save diagnostic to local queue**. The plugin displays an explicit
confirmation before starting the shared helper.

The event contains fixed project/component/version/category data only.
No usernames, file names, archive paths, drive paths, image contents,
credentials, memory dumps, or raw logs are included. Windows launches only
the local collector, with GitHub tokens removed from its environment.
Only `capture-json` is invoked; there is **no automatic network
submission**. A successful response shows the report ID and that the local
queue was updated.

To inspect the pending event manually:

```powershell
& "$env:LOCALAPPDATA\UniversalDiagnostics\UniversalDiagnostics.exe" list --project sgennadi/ISO-WCX
```

The shared collector's separate review/draft/submission workflow remains
opt-in and belongs to the central repository. The plugin does not receive
GitHub credentials or embed tokens.

## Versioning and tests

The existing ISO-WCX version stays **0.21.0**. Do not replace its stable
release with untested binaries. Build and test this branch in GitHub Actions
before packaging.

`universal_diagnostics_contract` is a portable CTest verifying that all
six categories use the agreed JSON protocol and do not include private
data. Full Windows/Total Commander UI and collector installation tests
are separate from contract tests.
