# Windows PowerShell integration tests for the safe ISO-WCX wincmd.ini migrator.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "TEST FAILED: $Message" }
}
function Assert-Bytes([byte[]]$Expected, [byte[]]$Actual, [string]$Message) {
    Assert-True ($Expected.Length -eq $Actual.Length) "$Message (size differs)"
    for($i=0;$i -lt $Expected.Length;$i++) {
        if($Expected[$i] -ne $Actual[$i]) { throw "TEST FAILED: $Message at byte $i" }
    }
}
$repair = Join-Path (Split-Path -Parent $PSScriptRoot) 'ISO-WCX-Repair.ps1'
$root = Join-Path $env:TEMP ("iso-wcx-repair-test-" + [guid]::NewGuid().ToString('N'))
$legacyDir = Join-Path $root 'plugins\wcx\ISO'
$plugin = Join-Path $root 'plugins\wcx\ISO-WCX'
$ini = Join-Path $root 'wincmd.ini'
New-Item -ItemType Directory -Path $legacyDir -Force | Out-Null
New-Item -ItemType Directory -Path $plugin -Force | Out-Null
$plug32 = Join-Path $plugin 'ISO-WCX.wcx'
$plug64 = Join-Path $plugin 'ISO-WCX.wcx64'
[IO.File]::WriteAllBytes($plug32, [byte[]](1,2,3))
[IO.File]::WriteAllBytes($plug64, [byte[]](4,5,6))
try {
    $old = Join-Path $legacyDir 'iso.wcx'
    $legacy = "192,$old"
    $crlf = [Environment]::NewLine
    $beforeText = (@(
        '[Configuration]'
        'LanguageIni=Russian'
        '[PackerPlugins]'
        '7z=735,C:\Unrelated\7z.wcx'
        "iso=$legacy"
        'nrg=64,C:\Unrelated\nrg.wcx'
        '[PackerPlugins64]'
        'exts=7z*iso'
        '# comment that must remain unchanged'
    ) -join $crlf) + $crlf
    $u8 = [Text.UTF8Encoding]::new($true, $true)
    $payload = $u8.GetBytes($beforeText)
    $bom = $u8.GetPreamble()
    $original = [byte[]]::new($bom.Length + $payload.Length)
    [Array]::Copy($bom,0,$original,0,$bom.Length)
    [Array]::Copy($payload,0,$original,$bom.Length,$payload.Length)
    [IO.File]::WriteAllBytes($ini,$original)

    # Read-only pass must not create a backup or touch the INI.
    & $repair -IniPath $ini -PluginDirectory $plugin | Out-Null
    Assert-Bytes $original ([IO.File]::ReadAllBytes($ini)) 'dry run modified INI'
    Assert-True (@(Get-ChildItem -LiteralPath $root -Filter '*.isowcx-backup-*').Count -eq 0) 'dry run wrote a backup'

    # Apply: change only the obsolete iso= path and preserve BOM/other rows.
    & $repair -IniPath $ini -PluginDirectory $plugin -Apply | Out-Null
    $modified = [IO.File]::ReadAllBytes($ini)
    Assert-True ($modified[0] -eq 239 -and $modified[1] -eq 187 -and $modified[2] -eq 191) 'UTF-8 BOM lost'
    $updatedText = $u8.GetString($modified,3,$modified.Length-3)
    $expectedText = $beforeText.Replace("iso=$legacy", "iso=158,$plug32")
    Assert-True ($updatedText -ceq $expectedText) 'changed another association, section, encoding, or line ending'
    $backups = @(Get-ChildItem -LiteralPath $root -Filter '*.isowcx-backup-*')
    Assert-True ($backups.Count -eq 1) 'missing/duplicate recovery backup'
    Assert-Bytes $original ([IO.File]::ReadAllBytes($backups[0].FullName)) 'backup differs from input'

    # Re-running a repair must be idempotent and preserve the other handlers.
    & $repair -IniPath $ini -PluginDirectory $plugin -Apply | Out-Null
    Assert-Bytes $modified ([IO.File]::ReadAllBytes($ini)) 'second run changed correct INI'
    Assert-True (@(Get-ChildItem -LiteralPath $root -Filter '*.isowcx-backup-*').Count -eq 1) 'idempotent run wrote another backup'

    # A previous ISO-WCX install can carry the stale 192 flags even when
    # its path is correct. Update only its own ISO row to 158, preserving
    # the language, buttons and all unrelated plugin entries.
    $stale = $beforeText.Replace("iso=$legacy", "iso=192,$plug32")
    [IO.File]::WriteAllText($ini,$stale,$u8)
    & $repair -IniPath $ini -PluginDirectory $plugin -Apply | Out-Null
    $refreshed = [IO.File]::ReadAllBytes($ini)
    Assert-True ($u8.GetString($refreshed,3,$refreshed.Length-3) -ceq $expectedText) 'stale 192 flags were not updated to 158'
    & $repair -IniPath $ini -PluginDirectory $plugin -Apply | Out-Null
    Assert-Bytes $refreshed ([IO.File]::ReadAllBytes($ini)) 'already valid 158 association changed on repeat'

    # User's unrelated working ISO handler must never be overwritten.
    $third = $beforeText.Replace("iso=$legacy", 'iso=192,C:\OtherPlugin\custom.wcx')
    [IO.File]::WriteAllText($ini,$third,$u8)
    $unrelated = [IO.File]::ReadAllBytes($ini)
    & $repair -IniPath $ini -PluginDirectory $plugin -Apply | Out-Null
    Assert-Bytes $unrelated ([IO.File]::ReadAllBytes($ini)) 'replaced an unrelated ISO handler'

    # Missing counterpart binaries are always fatal, never silently selected.
    [IO.File]::WriteAllBytes($ini,$original)
    Remove-Item -LiteralPath $plug64 -Force
    $caught = $false
    try { & $repair -IniPath $ini -PluginDirectory $plugin -Apply | Out-Null }
    catch { $caught = $true }
    Assert-True $caught 'missing WCX64 should fail safely'
    Assert-Bytes $original ([IO.File]::ReadAllBytes($ini)) 'missing plugin changed INI'
    [IO.File]::WriteAllBytes($plug64,[byte[]](4,5,6))

    # An existing legacy plugin may be working; no automatic takeover.
    [IO.File]::WriteAllBytes($old,[byte[]](9))
    $caught = $false
    try { & $repair -IniPath $ini -PluginDirectory $plugin -Apply | Out-Null }
    catch { $caught = $true }
    Assert-True $caught 'existing legacy ISO plugin must not be overwritten'
    Assert-Bytes $original ([IO.File]::ReadAllBytes($ini)) 'working handler was modified'

    Write-Host 'PASS: WCX association migration, safe dry-run/backup, BOM+CRLF preservation, idempotency and refusal cases'
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
