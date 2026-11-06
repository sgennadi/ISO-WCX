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
    if (-not ([Linq.Enumerable]::SequenceEqual([byte[]]$original, [byte[]][IO.File]::ReadAllBytes($ini)))) {
        throw 'Dry run changed wincmd.ini bytes.'
    }
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
    if (-not ([Linq.Enumerable]::SequenceEqual([byte[]]$original, [byte[]][IO.File]::ReadAllBytes($bak[0].FullName)))) {
        throw 'Backup did not preserve original bytes.'
    }
    & $repair -IniPath $ini -Apply | Out-Null
    Assert-Equal 1 @(Get-ChildItem -Path $root -Filter '*.iso-wcx-f5-*.bak').Count 'Repeated repair wrote backup'
    if (-not ([Linq.Enumerable]::SequenceEqual([byte[]]$now, [byte[]][IO.File]::ReadAllBytes($ini)))) {
        throw 'Repeated repair changed already normalized INI.'
    }
    # A different plugin must never be reassigned.
    $other = Join-Path $root 'external.ini'
    [IO.File]::WriteAllText($other, "[PackerPlugins]`r`nwim=222,C:\Other\Custom.wcx`r`n")
    $untouched = [IO.File]::ReadAllBytes($other)
    & $repair -IniPath $other -Apply | Out-Null
    if (-not ([Linq.Enumerable]::SequenceEqual([byte[]]$untouched, [byte[]][IO.File]::ReadAllBytes($other)))) {
        throw 'Other plugin was modified.'
    }
    # Redirects are not auto-followed by this repair; refuse silently
    # creating a second registration in the wrong file.
    $redirect = Join-Path $root 'redirect.ini'
    [IO.File]::WriteAllText($redirect,
        "[PackerPlugins]`r`nRedirectSection=other.ini`r`nwim=222,$dll`r`n")
    $before = [IO.File]::ReadAllBytes($redirect)
    $blocked = $false
    try { & $repair -IniPath $redirect -Apply | Out-Null } catch { $blocked = $true }
    if (-not $blocked) { throw 'Redirect was not refused.' }
    if (-not ([Linq.Enumerable]::SequenceEqual([byte[]]$before, [byte[]][IO.File]::ReadAllBytes($redirect)))) {
        throw 'Redirected INI changed.'
    }
    Write-Host 'PASS: 222/158/192 -> 159, x64 cache, backup, dry run, redirection and privacy'
} finally {
    Remove-Item -LiteralPath $root -Force -Recurse -ErrorAction SilentlyContinue
}
