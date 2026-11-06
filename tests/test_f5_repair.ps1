# Tests for explicit, offline ISO-WCX WIM/ISO/UDF host-F5 cache repair.
# Uses ONLY synthetic Total Commander INI files under TEMP.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repair = Join-Path (Split-Path -Parent $PSScriptRoot) 'ISO-WCX-F5-Repair.ps1'
$root = Join-Path $env:TEMP ('iso-wcx-f5-repair-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root -Force | Out-Null

function Assert-Equal([object]$expected, [object]$actual, [string]$msg) {
    if ($expected -cne $actual) { throw "$msg expected='$expected' got='$actual'" }
}
function Assert-Bytes([byte[]]$expected, [byte[]]$actual, [string]$message) {
    if ($expected.Length -ne $actual.Length) { throw "$message (size differs)" }
    for ($i=0; $i -lt $expected.Length; ++$i) {
        if ($expected[$i] -ne $actual[$i]) { throw "$message at byte $i" }
    }
}
function Make-Fixture {
    param([string]$path)
    $dll = Join-Path $root 'ISO-WCX\ISO-WCX.wcx'
    $iniText = @(
        '[Configuration]'
        'ExplorerForCopy=0'
        '[PackerPlugins]'
        "wim=222,$dll"
        "iso=158,$dll"
        "udf=192,$dll"
        'rar=222,C:\Other\Unrelated.wcx'
        '[PackerPlugins64]'
        '$checksum$=01234567'
        'exts=rar*wim*iso*udf*'
        '[Other]'
        '$checksum$=must-not-touch'
    ) -join "`r`n"
    $encoding = [Text.UTF8Encoding]::new($true, $true)
    [IO.File]::WriteAllText($path, $iniText+"`r`n", $encoding)
    return $dll
}
try {
    $ini = Join-Path $root 'wincmd.ini'
    $dll = Make-Fixture -path $ini
    $original = [IO.File]::ReadAllBytes($ini)
    & $repair -IniPath $ini | Out-Null
    Assert-Bytes $original ([IO.File]::ReadAllBytes($ini)) 'Dry run changed wincmd.ini bytes'
    & $repair -IniPath $ini -Apply | Out-Null
    $now = [IO.File]::ReadAllBytes($ini)
    $text = [Text.Encoding]::UTF8.GetString($now, 3, $now.Length-3)
    foreach ($e in @('wim','iso','udf')) {
        if ($text -notmatch "(?m)^$e=159,") { throw "Registration $e not migrated to 159" }
    }
    if ($text -notmatch [regex]::Escape('rar=222,C:\Other\Unrelated.wcx')) {
        throw 'Unrelated WCX registration was changed.'
    }
    if ($text -notmatch [regex]::Escape('$checksum$=must-not-touch')) {
        throw 'Checksum in unrelated section was modified.'
    }
    if ($text -match '(?m)^\$checksum\$=01234567') {
        throw '64-bit TC cached checksum was not invalidated.'
    }
    if ($now[0] -ne 239 -or $now[1] -ne 187 -or $now[2] -ne 191) {
        throw 'UTF-8 BOM was not preserved.'
    }
    $bak = @(Get-ChildItem -Path $root -Filter '*.iso-wcx-f5-*.bak')
    Assert-Equal 1 $bak.Count 'Expected one backup'
    Assert-Bytes $original ([IO.File]::ReadAllBytes($bak[0].FullName)) 'Backup did not preserve original bytes'
    & $repair -IniPath $ini -Apply | Out-Null
    $backupCount = @(Get-ChildItem -Path $root -Filter '*.iso-wcx-f5-*.bak').Count
    Assert-Equal 1 $backupCount 'Repeated repair wrote backup'
    Assert-Bytes $now ([IO.File]::ReadAllBytes($ini)) 'Repeated repair changed already normalized INI'
    # A different plugin must never be reassigned.
    $other = Join-Path $root 'external.ini'
    [IO.File]::WriteAllText($other, "[PackerPlugins]`r`nwim=222,C:\Other\Custom.wcx`r`n")
    $untouched = [IO.File]::ReadAllBytes($other)
    & $repair -IniPath $other -Apply | Out-Null
    Assert-Bytes $untouched ([IO.File]::ReadAllBytes($other)) 'Other plugin was modified'
    # Redirects are not auto-followed by this repair; refuse silently
    # creating a second registration in the wrong file.
    $redirect = Join-Path $root 'redirect.ini'
    [IO.File]::WriteAllText($redirect,
        "[PackerPlugins]`r`nRedirectSection=other.ini`r`nwim=222,$dll`r`n")
    $before = [IO.File]::ReadAllBytes($redirect)
    $blocked = $false
    try { & $repair -IniPath $redirect -Apply | Out-Null } catch { $blocked = $true }
    if (-not $blocked) { throw 'Redirect was not refused.' }
    Assert-Bytes $before ([IO.File]::ReadAllBytes($redirect)) 'Redirected INI changed'
    Write-Host 'PASS: 222/158/192 -> 159, x64 cache, backup, dry run, redirection and privacy'
} finally {
    Remove-Item -LiteralPath $root -Force -Recurse -ErrorAction SilentlyContinue
}
