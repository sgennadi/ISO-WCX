# Total Commander WCX association audit (RC2)

This implements the field-reported suspicion that ISO-WCX F5 stopped working
because of `[PackerPlugins]` entries or a stale 64-bit registration cache.

## What the capability numbers actually mean

| Mask | MOD (2) | DELETE (8) | NEW (1) | Comment |
|---|---|---|---|---|
| 158 | yes | yes | no | F5 and Delete registration enabled |
| 159 | yes | yes | yes | F5/Delete enabled, but NEW is not implemented |
| 192 | **no** | **no** | no | BY_CONTENT+SEARCHTEXT only: F5/Delete blocked by TC |
| 222 | yes | yes | no | **ISO-WCX advertised GetPackerCaps** |

158 vs. 159 cannot, by itself, explain losing F5 into an existing WIM.
Mask 192, a different plugin path, missing x64 DLL, duplicate entries, an
unexpected redirected INI or a stale `PackerPlugins64` cache can.

## Built-in, read-only diagnostic (no EXE)

Open Total Commander > Files > Pack > Configure... > ISO-WCX Settings >
**Check WCX associations / F5 (158, 159, 222)...**.

The plugin exports

`%LOCALAPPDATA%\ISO-WCX\Reports\ISO-WCX-TC-Associations.txt`

with the active `wincmd.ini`, `[PackerPlugins]` and
`[PackerPlugins64]` file locations (including redirects), all five
optical/WIM associations, masks, native x86 WCX paths, companion x64 WCX
path existence and a warning for an invalid or missing 64-bit extension
cache entry. It does **not** modify the INI, reload the DLL, clear the
cache or change F5 behavior. It may include account-specific paths:
**review it before sharing**.

For an existing ISO-WCX WIM association, `wim=158,<path>`,
`wim=159,<path>` and `wim=222,<path>` all contain the needed modify
and delete bits. `wim=192,<path>` does **not**.

## Optional offline repair utility — development tests only

The older `tools/tc_setup.cpp` originally wrote the incorrect mask
192. Its repair logic now writes 222 for supported writable formats and
also repairs existing ISO-WCX paths with a stale read-only mask.
Legitimate 158/159 entries remain unchanged; ESD and SWM stay read-only.
Its Windows integration test checks stale 192 correction, redirected
configuration and idempotence.

The setup helper is a **development/test-only executable** and is
**never packaged** in the single user-installable x86/x64 ZIP. To prevent
conflicts, any future offline repair must only run when Total Commander
is closed, after a backup. The RC2 production plugin only offers the
read-only audit; it does not modify another process's `wincmd.ini`.

## What this does not prove

Without the user's actual audit report, we cannot establish whether the
reported regression came from the registration, WCX argument routing
or WIMGAPI itself. Green synthetic tests do not prove the user's real
WIM can be modified. Do not issue a 'fixed' claim until checked on a
disposable `boot.wim` and `install.wim` from the user's environment.
