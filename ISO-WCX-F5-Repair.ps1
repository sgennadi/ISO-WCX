# ISO-WCX RC3: offline Total Commander F5 association migration.
# Default is read-only preview; -Apply requires Total Commander to be closed.
# Only changes iso, udf, wim entries belonging to ISO-WCX.wcx; no other WCX
# registration, plugin binaries, or actual ISO/WIM data is modified.
[CmdletBinding()]
param(
    [string]$IniPath = '',
    [switch]$Apply
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-ActiveIni {
    if ($IniPath) { return [Environment]::ExpandEnvironmentVariables($IniPath) }
    if ($env:COMMANDER_INI) {
        return [Environment]::ExpandEnvironmentVariables($env:COMMANDER_INI)
    }
    foreach ($registryPath in @('HKCU:\Software\Ghisler\Total Commander',
                               'HKLM:\Software\Ghisler\Total Commander')) {
        $reg = Get-ItemProperty -Path $registryPath -Name IniFileName -ErrorAction SilentlyContinue
        if ($reg -and $reg.IniFileName) {
            return [Environment]::ExpandEnvironmentVariables([string]$reg.IniFileName)
        }
    }
    if ($env:APPDATA) { return (Join-Path $env:APPDATA 'GHISLER\wincmd.ini') }
    throw 'Specify the active Total Commander wincmd.ini with -IniPath.'
}
function Read-OriginalText([byte[]]$bytes) {
    $offset = 0
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 239 -and $bytes[1] -eq 187 -and $bytes[2] -eq 191) {
        $encoding = [Text.UTF8Encoding]::new($true, $true)
        $offset = 3
    } elseif ($bytes.Length -ge 2 -and $bytes[0] -eq 255 -and $bytes[1] -eq 254) {
        $encoding = [Text.UnicodeEncoding]::new($false, $true, $true)
        $offset = 2
    } elseif ($bytes.Length -ge 2 -and $bytes[0] -eq 254 -and $bytes[1] -eq 255) {
        $encoding = [Text.UnicodeEncoding]::new($true, $true, $true)
        $offset = 2
    } else {
        # Keep an ANSI INI's original Windows code page, not UTF-8 conversion.
        $encoding = [Text.Encoding]::Default
    }
    $text = $encoding.GetString($bytes, $offset, $bytes.Length - $offset)
    if ($text.Contains([char]0)) { throw 'wincmd.ini contains NUL bytes; refusing repair.' }
    return @{ Encoding = $encoding; Text = $text }
}
function Get-IniLines([string]$text) {
    # Include each original line ending in the corresponding element.
    return [regex]::Matches($text, '[^\r\n]*(?:\r\n|\r|\n|$)') |
        ForEach-Object { $_.Value } |
        Where-Object { $_.Length -gt 0 }
}
$ini = Get-ActiveIni
if (-not (Test-Path -LiteralPath $ini -PathType Leaf)) { throw "Missing INI: $ini" }
$ini = (Resolve-Path -LiteralPath $ini).ProviderPath
$file = Get-Item -LiteralPath $ini
if (($file.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw 'Refusing to edit a symbolic link / reparse point.'
}
if ($file.Length -gt 16777216) { throw 'INI unexpectedly exceeds 16 MiB.' }
$original = [IO.File]::ReadAllBytes($ini)
$decoded = Read-OriginalText $original
$lines = @(Get-IniLines $decoded.Text)
$section = ''
$changes = New-Object 'System.Collections.Generic.List[string]'
$seen = @{}
for ($i=0; $i -lt $lines.Count; ++$i) {
    $line = [string]$lines[$i]
    $body = $line.TrimEnd([char[]]@([char]13,[char]10))
    $ending = $line.Substring($body.Length)
    $header = [regex]::Match($body, '^\s*\[([^\]]+)\]\s*$')
    if ($header.Success) {
        $section = $header.Groups[1].Value.Trim().ToLowerInvariant()
        continue
    }
    if ($section -ne 'packerplugins') { continue }
    if ($body -match '^\s*RedirectSection\s*=') {
        throw 'PackerPlugins is redirected: run against the redirected INI explicitly after reviewing its location.'
    }
    $entry = [regex]::Match($body,
        '^(?<prefix>\s*(?<ext>wim|iso|udf)\s*=\s*)(?<caps>\d+)\s*,\s*(?<path>.*?)(?<suffix>\s*)$',
        [Text.RegularExpressions.RegexOptions]::IgnoreCase)
    if (-not $entry.Success) { continue }
    $extension = $entry.Groups['ext'].Value.ToLowerInvariant()
    if ($seen.ContainsKey($extension)) { throw "Duplicate $extension entries: refusing to edit INI." }
    $seen[$extension] = $true
    $wcx = $entry.Groups['path'].Value.Trim().Trim('"')
    if ($wcx -notmatch '(?i)(^|[\\/])ISO-WCX[\\/]ISO-WCX\.wcx$') {
        Write-Host "SKIP $extension (different plugin): $wcx"
        continue
    }
    $cap = $entry.Groups['caps'].Value
    if ($cap -eq '159') {
        Write-Host "OK $extension=159"
        continue
    }
    $changes.Add("$extension : $cap -> 159")
    # Only change the decimal mask. Keep the original path, capitalization,
    # whitespace, BOM, comments, other extensions and line endings untouched.
    $start = $entry.Groups['caps'].Index
    $length = $entry.Groups['caps'].Length
    $lines[$i] = $body.Substring(0,$start) + '159' +
                 $body.Substring($start+$length) + $ending
}
if ($changes.Count -eq 0) {
    Write-Host 'No ISO-WCX WIM/ISO/UDF association changes required.'
    return
}
Write-Host "Total Commander INI: $ini"
$changes | ForEach-Object { Write-Host "PLAN $_" }
if (-not $Apply) {
    Write-Host 'DRY RUN ONLY. Nothing changed. Close all TC windows, then rerun with -Apply.'
    return
}
$running = @(Get-Process -Name TOTALCMD, TOTALCMD64 -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) { throw 'Close ALL Total Commander windows before applying this repair.' }

# Invalidate only the derived Total Commander x64 extension cache
# checksum, never a $checksum$ in an unrelated INI section. Fail closed on
# a redirected 64-bit section rather than leaving a stale hidden cache.
$section = ''
for ($i=0; $i -lt $lines.Count; ++$i) {
    $body = [string]$lines[$i]
    $head = [regex]::Match($body.Trim(), '^\[([^\]]+)\]
$bom = $decoded.Encoding.GetPreamble()
$bytes = [byte[]]::new($bom.Length + $payload.Length)
[Array]::Copy($bom, 0, $bytes, 0, $bom.Length)
[Array]::Copy($payload, 0, $bytes, $bom.Length, $payload.Length)
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$backup = "$ini.iso-wcx-f5-$stamp-$([guid]::NewGuid().ToString('N').Substring(0,6)).bak"
$temporary = "$ini.iso-wcx-f5-$([guid]::NewGuid().ToString('N')).tmp"
try {
    [IO.File]::WriteAllBytes($temporary, $bytes)
    # Atomic replacement on the same volume, with original backup bytes.
    [IO.File]::Replace($temporary, $ini, $backup, $true)
    Write-Host "SUCCESS: F5 capability associations are now 159."
    Write-Host "Backup: $backup"
    Write-Host 'Restart Total Commander to rebuild its x64 WCX cache.'
} finally {
    if (Test-Path -LiteralPath $temporary) {
        Remove-Item -LiteralPath $temporary -Force
    }
}
)
    if ($head.Success) { $section = $head.Groups[1].Value.Trim().ToLowerInvariant(); continue }
    if ($section -ne 'packerplugins64') { continue }
    if ($body -match '^\s*RedirectSection\s*=') {
        throw 'PackerPlugins64 is redirected: review its cache location first.'
    }
    if ($body -match '^\s*\$checksum\$\s*=') { $lines[$i] = '' }
}
$updatedText = [string]::Concat($lines)
$payload = $decoded.Encoding.GetBytes($updatedText)
$bom = $decoded.Encoding.GetPreamble()
$bytes = [byte[]]::new($bom.Length + $payload.Length)
[Array]::Copy($bom, 0, $bytes, 0, $bom.Length)
[Array]::Copy($payload, 0, $bytes, $bom.Length, $payload.Length)
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$backup = "$ini.iso-wcx-f5-$stamp-$([guid]::NewGuid().ToString('N').Substring(0,6)).bak"
$temporary = "$ini.iso-wcx-f5-$([guid]::NewGuid().ToString('N')).tmp"
try {
    [IO.File]::WriteAllBytes($temporary, $bytes)
    # Atomic replacement on the same volume, with original backup bytes.
    [IO.File]::Replace($temporary, $ini, $backup, $true)
    Write-Host "SUCCESS: F5 capability associations are now 159."
    Write-Host "Backup: $backup"
    Write-Host 'Restart Total Commander to rebuild its x64 WCX cache.'
} finally {
    if (Test-Path -LiteralPath $temporary) {
        Remove-Item -LiteralPath $temporary -Force
    }
}
