#requires -Version 5.1
<#
.SYNOPSIS
Safely repairs stale legacy ISO.WCX registrations in Total Commander.
.DESCRIPTION
Read-only preview by default. -Apply creates a timestamped backup and replaces
only associations targeting legacy ISO\iso.wcx (or current ISO-WCX). All other
plugins and sections of wincmd.ini are preserved byte-for-byte.
Run AFTER closing Total Commander. No administrative rights are required when
the current user can write their own wincmd.ini.
.EXAMPLE
.\Repair-ISO-WCX.ps1 -IniPath "$env:APPDATA\GHISLER\wincmd.ini"
.EXAMPLE
.\Repair-ISO-WCX.ps1 -IniPath "$env:APPDATA\GHISLER\wincmd.ini" -Apply
#>
[CmdletBinding()]
param(
    [string]$IniPath,
    [string]$PluginDirectory = $PSScriptRoot,
    [switch]$Apply,
    [switch]$IncludeOtherExtensions
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($IniPath)) {
    $candidatePaths = New-Object 'System.Collections.Generic.List[string]'
    if ($env:COMMANDER_INI) { $candidatePaths.Add($env:COMMANDER_INI) }
    try {
        $reg = Get-ItemProperty 'HKCU:\Software\Ghisler\Total Commander' -Name IniFileName -ErrorAction Stop
        if ($reg.IniFileName) { $candidatePaths.Add([Environment]::ExpandEnvironmentVariables($reg.IniFileName)) }
    } catch { }
    if ($env:APPDATA) { $candidatePaths.Add((Join-Path $env:APPDATA 'GHISLER\wincmd.ini')) }
    foreach ($candidate in $candidatePaths) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { $IniPath = $candidate; break }
    }
}
if ([string]::IsNullOrWhiteSpace($IniPath) -or -not (Test-Path -LiteralPath $IniPath -PathType Leaf)) {
    throw 'Cannot locate wincmd.ini. Specify -IniPath (see Total Commander Help > About).'
}
$IniPath = (Resolve-Path -LiteralPath $IniPath).ProviderPath
$PluginDirectory = (Resolve-Path -LiteralPath $PluginDirectory).ProviderPath
$Wcx32 = Join-Path $PluginDirectory 'ISO-WCX.wcx'
$Wcx64 = Join-Path $PluginDirectory 'ISO-WCX.wcx64'

function Assert-PluginArchitecture([string]$Path, [int]$Machine) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Plugin file missing: $Path" }
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 0x100 -or [BitConverter]::ToUInt16($bytes, 0) -ne 0x5A4D) {
        throw "Not a Windows DLL: $Path"
    }
    $offset = [BitConverter]::ToInt32($bytes, 0x3C)
    if ($offset -lt 0x40 -or $offset -gt $bytes.Length-6 -or
        [BitConverter]::ToUInt32($bytes, $offset) -ne 0x00004550 -or
        [BitConverter]::ToUInt16($bytes, $offset+4) -ne $Machine) {
        throw "Invalid PE machine type: $Path"
    }
}
Assert-PluginArchitecture $Wcx32 0x014c
Assert-PluginArchitecture $Wcx64 0x8664

$rawBytes = [IO.File]::ReadAllBytes($IniPath)
if ($rawBytes.Length -gt 16MB) { throw 'wincmd.ini is unexpectedly large; refusing to edit.' }
if ($rawBytes.Length -gt 1 -and (($rawBytes[0] -eq 255 -and $rawBytes[1] -eq 254) -or
    ($rawBytes[0] -eq 254 -and $rawBytes[1] -eq 255))) {
    throw 'UTF-16 INI detected; use the Total Commander GUI to change the association.'
}
# Latin-1 is a lossless 1:1 byte mapping. It preserves legacy Windows ACP, UTF-8,
# unknown comments, and all other sections without changing their encoding.
$latin1 = [Text.Encoding]::GetEncoding(28591)
$text = $latin1.GetString($rawBytes)
$lines = New-Object 'System.Collections.Generic.List[string]'
foreach ($piece in [regex]::Split($text, "(?<=`n)")) { if ($piece.Length) { $lines.Add($piece) } }
$newline = if ($text.Contains("`r`n")) { "`r`n" } else { "`n" }
$exts = @('iso')
if ($IncludeOtherExtensions) { $exts = @('iso', 'cue', 'ccd', 'nrg', 'mds', 'cso', 'zso') }
$seen = @{}
$updates = New-Object 'System.Collections.Generic.List[string]'
$section = ''
$packerStart = -1
$packerCount = 0
$newValue = '192,' + $Wcx32

for ($i=0; $i -lt $lines.Count; ++$i) {
    $line = $lines[$i]
    $core = [regex]::Replace($line, '[\r\n]+$', '')
    $sectionMatch = [regex]::Match($core, '^\s*\[([^\]]+)\]\s*$')
    if ($sectionMatch.Success) {
        $section = $sectionMatch.Groups[1].Value
        if ($section -ieq 'PackerPlugins') {
            ++$packerCount
            $packerStart = $i
        }
        continue
    }
    if ($section -ine 'PackerPlugins') { continue }
    $entry = [regex]::Match($core, '^(?<left>[ \t]*(?<ext>[A-Za-z0-9]+)[ \t]*=[ \t]*)(?<value>.*)$')
    if (-not $entry.Success) { continue }
    $ext = $entry.Groups['ext'].Value.ToLowerInvariant()
    if ($exts -cnotcontains $ext) { continue }
    if ($seen.ContainsKey($ext)) { throw "Duplicate association for .$ext in wincmd.ini" }
    $seen[$ext] = $true
    $oldValue = $entry.Groups['value'].Value
    $pathMatch = [regex]::Match($oldValue, '^\s*\d+\s*,\s*(?<file>.*?)\s*$', 'IgnoreCase')
    if (-not $pathMatch.Success) {
        Write-Warning ".$ext has an unexpected association; leaving it unchanged."
        continue
    }
    $oldPath = $pathMatch.Groups['file'].Value.Trim('"').Replace('/', '\')
    $legacy = $oldPath -match '(?i)(^|\\)ISO\\iso\.wcx(?:64)?$'
    $modern = $oldPath -match '(?i)(^|\\)ISO-WCX\\ISO-WCX\.wcx(?:64)?$'
    if ($legacy -or $modern) {
        if ($oldValue -cne $newValue) {
            $suffix = $line.Substring($core.Length)
            $lines[$i] = $entry.Groups['left'].Value + $newValue + $suffix
            $updates.Add(".$ext : $oldValue  =>  $newValue")
        }
    } else {
        Write-Warning ".$ext belongs to another plugin; leaving it untouched."
    }
}
if ($packerCount -ne 1) { throw 'wincmd.ini must have exactly one [PackerPlugins] section.' }
$missing = @($exts | Where-Object { -not $seen.ContainsKey($_) })
if ($missing.Count) {
    $addition = (($missing | ForEach-Object { "$_=$newValue" }) -join $newline) + $newline
    # Insert at start of section. This changes no existing association.
    $header = $lines[$packerStart]
    if ($header -notmatch "`n$") { $header += $newline }
    $lines[$packerStart] = $header + $addition
    foreach ($ext in $missing) { $updates.Add(".$ext : <missing>  =>  $newValue") }
}
if ($updates.Count -eq 0) {
    Write-Host 'No matching stale ISO-WCX registrations; no changes required.'
    return
}
Write-Host "INI: $IniPath"
Write-Host "Plugin: $Wcx32 (x86), $Wcx64 (x64 auto-selected by TC)"
$updates | ForEach-Object { Write-Host "  $_" }
if (-not $Apply) {
    Write-Host 'DRY RUN: nothing changed. Close Total Commander, then rerun with -Apply.'
    return
}
if (Get-Process -Name TOTALCMD,TOTALCMD64 -ErrorAction SilentlyContinue) {
    throw 'Close ALL Total Commander windows before applying the repair.'
}
$bytes = $latin1.GetBytes([string]::Concat($lines))
$timestamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$backup = "$IniPath.iso-wcx-backup-$timestamp"
$temp = "$IniPath.iso-wcx-temp-$timestamp"
try {
    [IO.File]::WriteAllBytes($temp, $bytes)
    [IO.File]::Replace($temp, $IniPath, $backup)
} finally {
    if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Force }
}
Write-Host 'Updated Total Commander PackerPlugins. Other sections left unchanged.'
Write-Host "Backup: $backup"
Write-Host 'Restart Total Commander. Test an ISO with Ctrl+PgDn.'