# ISO-WCX bulk Total Commander packer association installer.
# Read-only unless -Apply. Preserves toolbars, user commands and other plugins.
# Run as the same Windows user as Total Commander, with TC fully closed.
[CmdletBinding()]
param(
    [string]$IniPath,
    [string]$PluginDirectory,
    [switch]$Apply,
    [switch]$IncludeOptional
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Only image extensions. ZIP, RAR, 7z and common general archives are excluded.
$Extensions = @(
    'iso','udf','cue','ccd','nrg','mds','mdx','cso','zso','isz',
    'daa','img','raw','vhd','vhdx','wim','esd','swm','ima','vfd',
    'flp','bif','imz'
)
# BIN may be arbitrary data; MDF is often a raw track without a descriptor.
# CHD optical handling is experimental. Register only with explicit consent.
if ($IncludeOptional) { $Extensions += @('bin','mdf','chd') }
$CapabilityMask = '158' # GetPackerCaps: 2+4+8+16+128 (NOT by-content 64)
$EolCRLF = [string][char]13 + [char]10

function Resolve-TcText([string]$Value, [string]$Base) {
    if ([string]::IsNullOrWhiteSpace($Value)) { return $null }
    $value = $Value.Trim().Trim('"')
    if ($value -match '(?i)%COMMANDER_PATH%' -and $env:COMMANDER_PATH) {
        $value = [regex]::Replace($value,'(?i)%COMMANDER_PATH%',
                                 [string]$env:COMMANDER_PATH)
    }
    $value = [Environment]::ExpandEnvironmentVariables($value)
    if ($value.Contains('%')) { return $null }
    if (-not [IO.Path]::IsPathRooted($value)) {
        if (-not $Base) { return $null }
        $value = Join-Path $Base $value
    }
    return [IO.Path]::GetFullPath($value)
}
function Find-TcIni {
    if ($IniPath) {
        $path = Resolve-TcText $IniPath (Get-Location).Path
        if (-not $path -or -not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Specified wincmd.ini was not found: $IniPath"
        }
        return $path
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
    if ($env:APPDATA) {
        $candidates.Add((Join-Path $env:APPDATA 'GHISLER\wincmd.ini'))
    }
    if ($env:COMMANDER_PATH) {
        $candidates.Add((Join-Path $env:COMMANDER_PATH 'wincmd.ini'))
    }
    foreach ($candidate in $candidates) {
        $path = Resolve-TcText $candidate ([string]$env:COMMANDER_PATH)
        if ($path -and (Test-Path -LiteralPath $path -PathType Leaf)) { return $path }
    }
    throw 'Active wincmd.ini not found. Open TC > Configuration > Change Settings Files Directly and pass -IniPath.'
}
function Equal-Bytes([byte[]]$A, [byte[]]$B) {
    if ($A.Length -ne $B.Length) { return $false }
    for ($i=0; $i -lt $A.Length; $i++) {
        if ($A[$i] -ne $B[$i]) { return $false }
    }
    return $true
}
function Read-Doc([string]$Name) {
    $path = (Resolve-Path -LiteralPath $Name -ErrorAction Stop).ProviderPath
    $item = Get-Item -LiteralPath $path -ErrorAction Stop
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Refusing to edit a linked INI: $path"
    }
    if ($item.Length -gt 16MB) { throw "INI exceeds 16 MiB: $path" }
    $bytes = [IO.File]::ReadAllBytes($path)
    $skip = 0
    $enc = [Text.Encoding]::Default
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 239 -and
        $bytes[1] -eq 187 -and $bytes[2] -eq 191) {
        $enc = [Text.UTF8Encoding]::new($true,$true); $skip = 3
    } elseif ($bytes.Length -ge 2 -and $bytes[0] -eq 255 -and $bytes[1] -eq 254) {
        $enc = [Text.UnicodeEncoding]::new($false,$true,$true); $skip = 2
    } elseif ($bytes.Length -ge 2 -and $bytes[0] -eq 254 -and $bytes[1] -eq 255) {
        $enc = [Text.UnicodeEncoding]::new($true,$true,$true); $skip = 2
    }
    $text = $enc.GetString($bytes,$skip,$bytes.Length-$skip)
    if ($text.IndexOf([char]0) -ge 0) { throw "Unexpected NUL byte in INI: $path" }
    $roundtrip = $enc.GetBytes($text)
    $payload = [byte[]]::new($bytes.Length-$skip)
    [Array]::Copy($bytes,$skip,$payload,0,$payload.Length)
    if (-not (Equal-Bytes $payload $roundtrip)) {
        throw "INI encoding cannot be preserved without loss: $path"
    }
    $eol = if ($text.Contains($EolCRLF)) { $EolCRLF } else { [string][char]10 }
    return [pscustomobject]@{
        Path=$path; Bytes=$bytes; Encoding=$enc; Text=$text; Eol=$eol
    }
}
function As-Bytes($Doc, [string]$Text) {
    $payload = $Doc.Encoding.GetBytes($Text)
    $bom = $Doc.Encoding.GetPreamble()
    $data = [byte[]]::new($bom.Length+$payload.Length)
    [Array]::Copy($bom,0,$data,0,$bom.Length)
    [Array]::Copy($payload,0,$data,$bom.Length,$payload.Length)
    return ,$data
}
function Section-Rows([string]$Text, [string]$Section) {
    $active = ''
    $headers = 0
    $rows = @{}
    $lines = [regex]::Split($Text,'(?<=\n)')
    for ($i=0;$i -lt $lines.Length;$i++) {
        $body = ([string]$lines[$i]).TrimEnd([char[]]@([char]13,[char]10))
        $head = [regex]::Match($body,'^\s*\[\s*(?<section>[^\]]+?)\s*\]\s*$')
        if ($head.Success) {
            $active = $head.Groups['section'].Value.Trim()
            if ($active -ieq $Section) { $headers++ }
            continue
        }
        if ($active -ine $Section) { continue }
        $entry = [regex]::Match($body,
            '^(?<prefix>\s*(?<key>[^=\s]+)\s*=\s*)(?<value>.*?)\s*$')
        if (-not $entry.Success) { continue }
        $key = $entry.Groups['key'].Value
        if ($rows.ContainsKey($key)) {
            throw "Duplicate $key in [$Section] is ambiguous. No settings changed."
        }
        $rows[$key] = [pscustomobject]@{
            Index=$i; Prefix=$entry.Groups['prefix'].Value;
            Value=$entry.Groups['value'].Value.Trim()
        }
    }
    if ($headers -gt 1) {
        throw "Duplicate [$Section] sections are ambiguous. No settings changed."
    }
    return [pscustomobject]@{Rows=$rows; HasSection=($headers -eq 1)}
}
function Lookup-Value($Doc, [string]$Section, [string]$Key) {
    $entries = Section-Rows $Doc.Text $Section
    if ($entries.Rows.ContainsKey($Key)) { return [string]$entries.Rows[$Key].Value }
    return $null
}
function Resolve-SectionDoc($Root, [string]$Section) {
    $redirect = Lookup-Value $Root $Section 'RedirectSection'
    if (-not $redirect -or $redirect -eq '0') { return $Root.Path }
    if ($redirect -eq '1') {
        $redirect = Lookup-Value $Root 'Configuration' 'AlternateUserIni'
        if (-not $redirect) { throw "Missing AlternateUserIni for [$Section]." }
    }
    $path = Resolve-TcText $redirect (Split-Path -Parent $Root.Path)
    if (-not $path -or -not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Invalid redirected [$Section] file: $redirect"
    }
    return $path
}
function Is-OurPlugin([string]$Value, [string]$Expected) {
    $parsed = [regex]::Match($Value,'^\s*\d+\s*,\s*(?<path>.+?)\s*$')
    if (-not $parsed.Success) { return $false }
    $actual = $parsed.Groups['path'].Value.Trim().Trim('"')
    if ([string]::Equals($actual,$Expected,[StringComparison]::OrdinalIgnoreCase)) {
        return $true
    }
    return [regex]::IsMatch($actual,'(?i)(^|[\\/])ISO-WCX[\\/]ISO-WCX\.wcx$')
}
function Is-MissingLegacyIso([string]$Value) {
    $parsed = [regex]::Match($Value,'^\s*\d+\s*,\s*(?<path>.+?)\s*$')
    if (-not $parsed.Success) { return $false }
    $path = $parsed.Groups['path'].Value.Trim().Trim('"')
    if ($path -notmatch '(?i)(^|[\\/])ISO[\\/]iso\.wcx$') { return $false }
    $expanded = Resolve-TcText $path ([string]$env:COMMANDER_PATH)
    return $expanded -and -not (Test-Path -LiteralPath $expanded -PathType Leaf)
}
function Render-Doc($Doc,$PackerUpdates,[string[]]$Added,[bool]$ClearCache) {
    $lines = [regex]::Split($Doc.Text,'(?<=\n)')
    $builder = New-Object Text.StringBuilder
    $active = ''
    $packerFound = $false
    for ($i=0;$i -lt $lines.Length;$i++) {
        $line = [string]$lines[$i]
        $body = $line.TrimEnd([char[]]@([char]13,[char]10))
        $ending = $line.Substring($body.Length)
        $header = [regex]::Match($body,'^\s*\[\s*(?<section>[^\]]+?)\s*\]\s*$')
        if ($header.Success) {
            $active = $header.Groups['section'].Value.Trim()
            [void]$builder.Append($line)
            if ($active -ieq 'PackerPlugins') {
                $packerFound = $true
                if ($ending.Length -eq 0 -and $Added.Count -gt 0) {
                    [void]$builder.Append($Doc.Eol)
                }
                foreach ($new in $Added) { [void]$builder.Append($new+$Doc.Eol) }
            }
            continue
        }
        if ($active -ieq 'PackerPlugins' -and $PackerUpdates.ContainsKey($i)) {
            [void]$builder.Append([string]$PackerUpdates[$i]+$ending)
            continue
        }
        if ($ClearCache -and $active -ieq 'PackerPlugins64' -and
            $body -match '^\s*\$checksum\$\s*=') {
            continue
        }
        [void]$builder.Append($line)
    }
    if ($Added.Count -gt 0 -and -not $packerFound) {
        if ($builder.Length -gt 0 -and
            -not $builder.ToString().EndsWith([string][char]10)) {
            [void]$builder.Append($Doc.Eol)
        }
        [void]$builder.Append('[PackerPlugins]'+$Doc.Eol)
        foreach ($new in $Added) { [void]$builder.Append($new+$Doc.Eol) }
    }
    return $builder.ToString()
}
function Test-PluginBinary([string]$Path, [int]$Machine) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    $stream = [IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
    try {
        if ($stream.Length -lt 136) { return $false }
        $reader = [IO.BinaryReader]::new($stream)
        if ($reader.ReadUInt16() -ne 0x5A4D) { return $false }
        $stream.Position = 0x3C
        $offset = [long]$reader.ReadUInt32()
        if ($offset -lt 64 -or $offset -gt $stream.Length-6) { return $false }
        $stream.Position = $offset
        return ($reader.ReadUInt32() -eq 0x00004550 -and
                $reader.ReadUInt16() -eq $Machine)
    } finally { $stream.Dispose() }
}

$ini = Find-TcIni
$root = Read-Doc $ini
$packerPath = Resolve-SectionDoc $root 'PackerPlugins'
$cachePath = Resolve-SectionDoc $root 'PackerPlugins64'
$packer = if ($root.Path -ieq $packerPath) { $root } else { Read-Doc $packerPath }
$cache = if ($packer.Path -ieq $cachePath) { $packer }
         elseif ($root.Path -ieq $cachePath) { $root } else { Read-Doc $cachePath }
if (-not $PluginDirectory) {
    $PluginDirectory = Split-Path -Parent $PSCommandPath
    if (-not (Test-Path -LiteralPath (Join-Path $PluginDirectory 'ISO-WCX.wcx'))) {
        $PluginDirectory = Join-Path $env:COMMANDER_PATH 'plugins\wcx\ISO-WCX'
    }
}
$plugin = [IO.Path]::GetFullPath($PluginDirectory)
$target = Join-Path $plugin 'ISO-WCX.wcx'
if (-not (Test-PluginBinary $target 0x014C) -or
    -not (Test-PluginBinary (Join-Path $plugin 'ISO-WCX.wcx64') 0x8664)) {
    throw "Valid Win32 ISO-WCX.wcx and x64 ISO-WCX.wcx64 were not found in $plugin"
}

$section = Section-Rows $packer.Text 'PackerPlugins'
if ($section.Rows.ContainsKey('zip') -and
    (Is-OurPlugin ([string]$section.Rows['zip'].Value) $target)) {
    Write-Warning 'ZIP is currently assigned to ISO-WCX. To restore native ZIP handling, run ISO-WCX-RemoveAssociation.ps1 -Extension zip (dry run), then -Apply with TC closed. Other ZIP plugins must remain untouched.'
}
$updates = @{}
$added = New-Object 'System.Collections.Generic.List[string]'
$skipped = New-Object 'System.Collections.Generic.List[string]'
$updated = New-Object 'System.Collections.Generic.List[string]'
foreach ($ext in $Extensions) {
    $newValue = "$CapabilityMask,$target"
    if (-not $section.Rows.ContainsKey($ext)) {
        $added.Add("$ext=$newValue")
        continue
    }
    $row = $section.Rows[$ext]
    $existing = [string]$row.Value
    if ((Is-OurPlugin $existing $target) -or
        ($ext -eq 'iso' -and (Is-MissingLegacyIso $existing))) {
        if ($existing -cne $newValue) {
            $updates[$row.Index] = $row.Prefix+$newValue
            $updated.Add($ext)
        }
    } else {
        $skipped.Add($ext)
    }
}
$changes = $updates.Count+$added.Count
Write-Host "INI: $ini"
Write-Host "Packer plugins: $($packer.Path)"
Write-Host "64-bit cache: $($cache.Path)"
Write-Host "ISO-WCX: $target"
Write-Host "Extensions: $($Extensions.Count); add: $($added.Count); refresh: $($updated.Count); preserve foreign: $($skipped.Count)"
if ($added.Count -gt 0) { Write-Host "ADD: $(($added | ForEach-Object { ($_ -split '=')[0] }) -join ', ')" }
if ($updated.Count -gt 0) { Write-Host "REFRESH: $($updated -join ', ')" }
if ($skipped.Count -gt 0) { Write-Host "OTHER PLUGINS UNCHANGED: $($skipped -join ', ')" }
Write-Host 'ZIP/RAR/7z associations, toolbar buttons and user commands are never changed.'
if ($changes -eq 0) {
    Write-Host 'Nothing to change. All eligible associations are already registered.'
    return
}

$plans = New-Object 'System.Collections.Generic.List[object]'
$newPacker = Render-Doc $packer $updates $added.ToArray() ($cache.Path -ieq $packer.Path)
if ($newPacker -cne $packer.Text) {
    $plans.Add([pscustomobject]@{Doc=$packer; Payload=(As-Bytes $packer $newPacker)})
}
if ($cache.Path -ine $packer.Path) {
    $newCache = Render-Doc $cache @{} @() $true
    if ($newCache -cne $cache.Text) {
        $plans.Add([pscustomobject]@{Doc=$cache;Payload=(As-Bytes $cache $newCache)})
    }
}
if (-not $Apply) {
    Write-Host 'DRY RUN. No files changed. Close Total Commander and rerun with -Apply.'
    return
}
if (@(Get-Process -Name 'TOTALCMD','TOTALCMD64' -ErrorAction SilentlyContinue).Count -gt 0) {
    throw 'Close every Total Commander instance before modifying its settings.'
}
$stages = New-Object 'System.Collections.Generic.List[object]'
try {
    foreach ($plan in $plans) {
        if (-not (Equal-Bytes ([IO.File]::ReadAllBytes($plan.Doc.Path)) $plan.Doc.Bytes)) {
            throw "INI changed during preparation: $($plan.Doc.Path)"
        }
        $suffix = [guid]::NewGuid().ToString('N')
        $pending = "$($plan.Doc.Path).isowcx-bulk-pending-$suffix"
        $backup = "$($plan.Doc.Path).isowcx-bulk-backup-$suffix"
        [IO.File]::WriteAllBytes($pending,[byte[]]$plan.Payload)
        $stages.Add([pscustomobject]@{
            Doc=$plan.Doc;Pending=$pending;Backup=$backup;Committed=$false
        })
    }
    foreach ($stage in $stages) {
        if (-not (Equal-Bytes ([IO.File]::ReadAllBytes($stage.Doc.Path)) $stage.Doc.Bytes)) {
            throw "INI changed before commit: $($stage.Doc.Path)"
        }
    }
    foreach ($stage in $stages) {
        [IO.File]::Replace($stage.Pending,$stage.Doc.Path,$stage.Backup)
        $stage.Committed = $true
        Write-Host "Backup: $($stage.Backup)"
    }
} catch {
    for ($i=$stages.Count-1;$i -ge 0;$i--) {
        $stage = $stages[$i]
        if ($stage.Committed -and (Test-Path -LiteralPath $stage.Backup)) {
            try { [IO.File]::Copy($stage.Backup,$stage.Doc.Path,$true) }
            catch { Write-Warning "Manual restore needed: $($stage.Backup)" }
        }
    }
    throw
} finally {
    foreach ($stage in $stages) {
        if (Test-Path -LiteralPath $stage.Pending) {
            Remove-Item -LiteralPath $stage.Pending -Force
        }
    }
}
Write-Host "SUCCESS: registered/updated $changes image types. Restart Total Commander."
