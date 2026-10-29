# ISO-WCX association repair for Total Commander (Windows PowerShell 5.1+).
# By default: read-only diagnostics. -Apply: backup and repair the obsolete
# iso=...\\ISO\\iso.wcx association only. Never alters any other extension.
[CmdletBinding()]
param(
    [string]$IniPath,
    [string]$PluginDirectory,
    [switch]$Apply
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Expand-TcPath([string]$Name) {
    if ([string]::IsNullOrWhiteSpace($Name)) { return $null }
    $path = [Environment]::ExpandEnvironmentVariables($Name.Trim('"'))
    if ($path.StartsWith('.\') -and $env:COMMANDER_PATH) {
        $path = Join-Path $env:COMMANDER_PATH $path.Substring(2)
    }
    if ($path.Contains('%')) { return $null }
    return $path
}

function Find-TcIni {
    if ($IniPath) {
        if (-not (Test-Path -LiteralPath $IniPath -PathType Leaf)) {
            throw "Specified wincmd.ini not found: $IniPath"
        }
        return (Resolve-Path -LiteralPath $IniPath).ProviderPath
    }
    $candidates = New-Object 'System.Collections.Generic.List[string]'
    if ($env:COMMANDER_INI) { $candidates.Add($env:COMMANDER_INI) }
    foreach ($key in @('HKCU:\Software\Ghisler\Total Commander',
                       'HKLM:\Software\Ghisler\Total Commander',
                       'HKLM:\Software\WOW6432Node\Ghisler\Total Commander')) {
        $properties = Get-ItemProperty -Path $key -Name IniFileName -ErrorAction SilentlyContinue
        if ($null -ne $properties -and $properties.IniFileName) {
            $candidates.Add([string]$properties.IniFileName)
        }
    }
    if ($env:APPDATA) { $candidates.Add((Join-Path $env:APPDATA 'GHISLER\wincmd.ini')) }
    if ($env:COMMANDER_PATH) { $candidates.Add((Join-Path $env:COMMANDER_PATH 'wincmd.ini')) }
    foreach ($candidate in $candidates) {
        $expanded = Expand-TcPath $candidate
        if ($expanded -and (Test-Path -LiteralPath $expanded -PathType Leaf)) {
            return (Resolve-Path -LiteralPath $expanded).ProviderPath
        }
    }
    throw 'Cannot locate the active wincmd.ini. Run Configuration > Change Settings Files Directly, close Total Commander, then pass -IniPath "full\path\wincmd.ini".'
}

$ini = Find-TcIni
$file = Get-Item -LiteralPath $ini
if (($file.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw 'Refusing to edit a wincmd.ini symbolic link/reparse point.'
}
if ($file.Length -gt 16MB) { throw 'Configuration file unexpectedly large (>16 MB).' }

$bytes = [IO.File]::ReadAllBytes($ini)
$skip = 0
$encoding = [Text.Encoding]::Default
if ($bytes.Length -ge 3 -and $bytes[0] -eq 239 -and $bytes[1] -eq 187 -and $bytes[2] -eq 191) {
    $encoding = [Text.UTF8Encoding]::new($true, $true)
    $skip = 3
} elseif ($bytes.Length -ge 2 -and $bytes[0] -eq 255 -and $bytes[1] -eq 254) {
    $encoding = [Text.UnicodeEncoding]::new($false, $true, $true)
    $skip = 2
} elseif ($bytes.Length -ge 2 -and $bytes[0] -eq 254 -and $bytes[1] -eq 255) {
    $encoding = [Text.UnicodeEncoding]::new($true, $true, $true)
    $skip = 2
}
$text = $encoding.GetString($bytes, $skip, $bytes.Length - $skip)
if ($text.IndexOf([char]0) -ge 0) { throw 'Invalid NUL byte in Total Commander INI.' }

$lines = [regex]::Split($text, '(?<=\n)')
$inPackers = $false
$isoRows = New-Object 'System.Collections.Generic.List[int]'
$matches = New-Object 'System.Collections.Generic.List[object]'
for ($i = 0; $i -lt $lines.Count; ++$i) {
    $line = [string]$lines[$i]
    $body = $line.TrimEnd([char[]]@([char]13, [char]10))
    $section = [regex]::Match($body, '^\s*\[\s*([^\]]+)\s*\]\s*$')
    if ($section.Success) {
        $inPackers = $section.Groups[1].Value.Trim().Equals(
            'PackerPlugins', [StringComparison]::OrdinalIgnoreCase)
        continue
    }
    if (-not $inPackers) { continue }
    $m = [regex]::Match($body, '^(?<prefix>\s*iso\s*=\s*)(?<value>.*?)\s*$',
                        [Text.RegularExpressions.RegexOptions]::IgnoreCase)
    if ($m.Success) {
        $isoRows.Add($i)
        $matches.Add($m)
    }
}

Write-Host "Total Commander INI: $ini"
if ($isoRows.Count -eq 0) {
    Write-Host "No ISO association found in [PackerPlugins]. Nothing changed."
    return
}
if ($isoRows.Count -gt 1) {
    throw 'Multiple iso= keys exist. Refusing automatic repair: use Total Commander Packer settings.'
}

$index = $isoRows[0]
$m = $matches[0]
$value = $m.Groups['value'].Value.Trim()
$parts = [regex]::Match($value, '^(?<flags>\d+)\s*,\s*(?<path>.+)$')
# Match native GetPackerCaps: old mask 192 lacks modify/delete/options.
# Only obsolete legacy ISO.WCX or ISO-WCX's own association is eligible.
$flags = '158'
$legacy = $value.Trim('"')
if ($parts.Success) {
    $legacy = $parts.Groups['path'].Value.Trim().Trim('"')
}
$alreadyInstalled = $legacy -match '(?i)(^|[\\/])ISO-WCX[\\/]ISO-WCX\.wcx$'
if ($alreadyInstalled) {
    if ($parts.Success -and $parts.Groups['flags'].Value -eq '158') {
        Write-Host 'ISO already references ISO-WCX.wcx with flags 158. No changes needed.'
        return
    }
    if (-not $PluginDirectory) {
        $current = Expand-TcPath $legacy
        if (-not $current) {
            throw 'Cannot resolve existing ISO-WCX directory; specify -PluginDirectory.'
        }
        $PluginDirectory = Split-Path -Parent $current
    }
} elseif ($legacy -notmatch '(?i)(^|[\\/])ISO[\\/]iso\.wcx$') {
    Write-Host "ISO is assigned to another plugin: $legacy"
    Write-Host 'Keeping this association unchanged. Use Packer settings to select ISO-WCX manually.'
    return
}
$expandedOld = if ($alreadyInstalled) { $null } else { Expand-TcPath $legacy }
if ($expandedOld -and (Test-Path -LiteralPath $expandedOld -PathType Leaf)) {
    throw "Old ISO.WCX still exists at $expandedOld. Refusing to replace a working handler."
}

if ($PluginDirectory) {
    $newDir = $PluginDirectory
} elseif ($expandedOld) {
    $oldFolder = Split-Path -Parent $expandedOld
    $parent = Split-Path -Parent $oldFolder
    $newDir = Join-Path $parent 'ISO-WCX'
} elseif ($env:COMMANDER_PATH) {
    $newDir = Join-Path $env:COMMANDER_PATH 'plugins\wcx\ISO-WCX'
} else {
    throw 'Cannot resolve the plugin folder from the old path. Supply -PluginDirectory "...\plugins\wcx\ISO-WCX".'
}
$new32 = Join-Path $newDir 'ISO-WCX.wcx'
$new64 = Join-Path $newDir 'ISO-WCX.wcx64'
if (-not (Test-Path -LiteralPath $new32 -PathType Leaf) -or
    -not (Test-Path -LiteralPath $new64 -PathType Leaf)) {
    throw "Missing ISO-WCX.wcx or ISO-WCX.wcx64 in $newDir. Install both files before repairing the INI."
}
if ((Get-Item -LiteralPath $new32).Length -eq 0 -or
    (Get-Item -LiteralPath $new64).Length -eq 0) {
    throw 'One of the WCX plugin files is empty.'
}

# Preserve unrelated plugin registrations, comments, line endings, whitespace
# and ANSI/UTF-8/UTF-16 BOM; use ISO-WCX's native capability mask 158.
# Never modify other [PackerPlugins] values or [PackerPlugins64] manually.
$new32 = (Resolve-Path -LiteralPath $new32).ProviderPath
$newValue = "$flags,$new32"
$line = [string]$lines[$index]
$body = $line.TrimEnd([char[]]@([char]13, [char]10))
$ending = $line.Substring($body.Length)
$prefix = $m.Groups['prefix'].Value
$lines[$index] = "$prefix$newValue$ending"
Write-Host "Old: $value"
Write-Host "New: $newValue"
if (-not $Apply) {
    Write-Host 'DRY RUN: no file modified. Close Total Commander and run this repair again with -Apply.'
    return
}
$running = @(Get-Process -Name 'TOTALCMD', 'TOTALCMD64' -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    throw 'Close all Total Commander instances first, so their settings do not overwrite the repair.'
}
$backup = "$ini.isowcx-backup-$(Get-Date -Format 'yyyyMMdd_HHmmss_fff')"
# File.Replace performs the swap and creates the original-file backup
# atomically on NTFS. Passing null for the third argument is rejected by
# Windows PowerShell/.NET Framework on some systems.
$tmp = "$ini.isowcx-pending-$([guid]::NewGuid().ToString('N'))"
try {
    $payload = $encoding.GetBytes([string]::Concat($lines))
    $bom = $encoding.GetPreamble()
    $updated = [byte[]]::new($bom.Length + $payload.Length)
    [Array]::Copy($bom, 0, $updated, 0, $bom.Length)
    [Array]::Copy($payload, 0, $updated, $bom.Length, $payload.Length)
    [IO.File]::WriteAllBytes($tmp, $updated)
    [IO.File]::Replace($tmp, $ini, $backup)
} finally {
    if (Test-Path -LiteralPath $tmp) {
        Remove-Item -LiteralPath $tmp -Force
    }
}
Write-Host "SUCCESS: repaired iso= association. Backup: $backup"
Write-Host 'Restart Total Commander. If loading still fails, check plugin dependencies separately.'
