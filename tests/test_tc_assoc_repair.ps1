# Windows CI regression tests for an explicitly opted-in Total Commander repair.
$ErrorActionPreference = 'Stop'
$repair = Join-Path $PSScriptRoot '..\tools\Repair-ISO-WCX.ps1'
$folder = Join-Path $env:TEMP ('iso-wcx-ini-test-' + [guid]::NewGuid().ToString('N'))
New-Item -Path $folder -ItemType Directory -Force | Out-Null
function Assert([bool]$condition,[string]$message) {
    if (-not $condition) { throw $message }
}
function New-FakePE([string]$path,[int]$machine) {
    $bytes = New-Object byte[] 512
    $bytes[0] = 0x4d; $bytes[1] = 0x5a
    [BitConverter]::GetBytes([int]0x80).CopyTo($bytes,0x3c)
    $bytes[0x80]=0x50; $bytes[0x81]=0x45
    [BitConverter]::GetBytes([uint16]$machine).CopyTo($bytes,0x84)
    [IO.File]::WriteAllBytes($path,$bytes)
}
try {
    $p = Join-Path $folder 'plugin'
    New-Item -Path $p -ItemType Directory | Out-Null
    New-FakePE (Join-Path $p 'ISO-WCX.wcx') 0x14c
    New-FakePE (Join-Path $p 'ISO-WCX.wcx64') 0x8664
    $ini = Join-Path $folder 'wincmd.ini'
    $enc = [Text.Encoding]::GetEncoding(28591)
    $orig = "[Configuration]`r`nRussianByte=" + [char]0xe0 + "`r`n[PackerPlugins]`r`niso=192,C:\Program Files\TotalCmd\plugins\wcx\ISO\iso.wcx`r`ncue=192,C:\Tools\Other.wcx`r`nzip=0,C:\Tools\Archive.wcx`r`n[PackerPlugins64]`r`nchecksum=abc123`r`n"
    [IO.File]::WriteAllBytes($ini,$enc.GetBytes($orig))
    & $repair -IniPath $ini -PluginDirectory $p | Out-Null
    Assert ([IO.File]::ReadAllBytes($ini).Length -eq $enc.GetByteCount($orig)) 'Dry-run unexpectedly changed INI'
    Assert ((Get-ChildItem $folder -Filter '*backup*').Count -eq 0) 'Dry run wrote a backup'
    & $repair -IniPath $ini -PluginDirectory $p -Apply | Out-Null
    $out = $enc.GetString([IO.File]::ReadAllBytes($ini))
    Assert ($out -match '(?im)^iso=192,.+\\ISO-WCX\.wcx\r?$') 'Missing migrated ISO association'
    Assert ($out.Contains('cue=192,C:\Tools\Other.wcx')) 'Other WCX association corrupted'
    Assert ($out.Contains('zip=0,C:\Tools\Archive.wcx')) 'Other archive association corrupted'
    Assert ($out.Contains("RussianByte=" + [char]0xe0)) 'Legacy byte changed'
    Assert ($out.Contains('checksum=abc123')) 'PackerPlugins64 cache unexpectedly modified'
    $backup = @(Get-ChildItem $folder -Filter 'wincmd.ini.iso-wcx-backup-*')
    Assert ($backup.Count -eq 1) 'Expected exactly one backup'
    Assert ([IO.File]::ReadAllBytes($backup[0].FullName).Length -eq $enc.GetByteCount($orig)) 'Backup incomplete'
    & $repair -IniPath $ini -PluginDirectory $p -Apply | Out-Null
    Assert (@(Get-ChildItem $folder -Filter 'wincmd.ini.iso-wcx-backup-*').Count -eq 1) 'Idempotence failed'
    $other = $orig.Replace('plugins\wcx\ISO\iso.wcx','plugins\wcx\Other\other.wcx')
    [IO.File]::WriteAllBytes($ini,$enc.GetBytes($other))
    & $repair -IniPath $ini -PluginDirectory $p -Apply | Out-Null
    Assert ($enc.GetString([IO.File]::ReadAllBytes($ini)) -ceq $other) 'Unrelated ISO plugin was replaced'
    $withoutIso = $orig.Replace("iso=192,C:\Program Files\TotalCmd\plugins\wcx\ISO\iso.wcx`r`n",'')
    [IO.File]::WriteAllBytes($ini,$enc.GetBytes($withoutIso))
    & $repair -IniPath $ini -PluginDirectory $p -Apply | Out-Null
    Assert ($enc.GetString([IO.File]::ReadAllBytes($ini)) -match '(?im)^iso=192,.+\\ISO-WCX\.wcx\r?$') 'Missing ISO key not created'
    Write-Host 'PASS: TC repair dry-run, backup, byte preservation, old ISO, other plugin, idempotence and missing key'
} finally {
    Remove-Item -LiteralPath $folder -Force -Recurse -ErrorAction SilentlyContinue
}
