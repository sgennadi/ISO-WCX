# UniversalDiagnostics embedded integration — ISO-WCX 0.21.0

The native shared queue module is **compiled into the WCX DLL itself**
(\`.wcx\` on x86, \`.wcx64\` on x64). It does not load a separate DLL,
spawn a collector EXE, use a shell, or contact GitHub during capture.

Source of truth:
[UniversalDiagnocticsModule/embedded/cpp](https://github.com/sgennadi/UniversalDiagnocticsModule/tree/main/embedded/cpp)

Vendored sources:
\`src/vendor/universal_diagnostics_embedded.hpp\` and
\`src/vendor/universal_diagnostics_embedded.cpp\`.

## Using it in Total Commander

1. Open ISO-WCX Settings (ConfigurePacker).
2. Under **Failed area**, select ISO F5, WIM F5, UDF edit, WIM mount/commit,
   read/extract or other.
3. Click **Save diagnostic locally (built-in)**, and confirm **Yes**.
4. The plugin immediately writes an atomic UTF-8 JSON event and shows its ID.

Only the selected category and the fixed application name/version go into
the event. ISO filenames, WIM paths, file contents, machine and account
identity, diagnostic logs, credentials and tokens are **not gathered**.
Nothing is uploaded. The operation is never triggered by WCX archive
callbacks, unsuccessful F5, or updater background tasks.

The local queue is:

\`%LOCALAPPDATA%\UniversalDiagnostics\<project-hash>\<report-id>.json\`

It uses the exact \`universal-diagnostics/v1\` event + delivery shape as
DISM GUI and the central Python analyzer. A distinct project hash keeps
ISO-WCX records separate from DISM GUI's records.

The optional standalone Python/EXE analyzer may **later** open these records
for user-approved GitHub submission. It is not a runtime dependency.

## Build, version, compatibility

- Source is included by CMake into the same \`ISO-WCX\` module.
- No extra binaries are included with the WCX plugin.
- Uses Windows BCrypt for SHA-256, native random UUIDs, and
  same-directory temp-file + atomic rename with file flush.
- Contract tests run under Linux; the Windows test loads the local embedded
  module, checks the resulting JSON and that no tokens or \`excerpt\` appear.
- \`ISO-WCX\` version remains **0.21.0**. Current integration PR #70 is a
  test PR and must pass Windows ABI/Total Commander smoke tests before merging.

To update the module in the future, change the canonical shared source
first, copy its exact contents to both \`src/vendor/\` files, and rebuild.
