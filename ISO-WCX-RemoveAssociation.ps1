# Optional Total Commander WCX association removal (PowerShell 5.1+).
# Removes only an explicitly selected ISO-WCX [PackerPlugins] extension.
# Dry-run by default; -Apply creates byte-for-byte recovery backups.
# Follows active RedirectSection entries without modifying unrelated INIs,
# toolbar buttons, user commands, comments or plugin registrations.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$IniPath,
    [ValidatePattern('^[A-Za-z0-9+_.-]{1,20}$')][string]$Extension='iso',
    [string]$PluginPath,
    [switch]$Apply
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'

function Read-Ini([string]$Name) {
    $resolved=(Resolve-Path -LiteralPath $Name -ErrorAction Stop).ProviderPath
    $file=Get-Item -LiteralPath $resolved -ErrorAction Stop
    if(($file.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Refusing to edit a linked/reparse-point INI: $resolved"
    }
    if($file.Length -gt 16MB) { throw "Refusing oversized INI: $resolved" }
    $bytes=[IO.File]::ReadAllBytes($resolved)
    $offset=0
    $enc=[Text.Encoding]::Default
    if($bytes.Length -ge 3 -and $bytes[0] -eq 239 -and $bytes[1] -eq 187 -and $bytes[2] -eq 191) {
        $enc=[Text.UTF8Encoding]::new($true,$true); $offset=3
    } elseif($bytes.Length -ge 2 -and $bytes[0] -eq 255 -and $bytes[1] -eq 254) {
        $enc=[Text.UnicodeEncoding]::new($false,$true,$true); $offset=2
    } elseif($bytes.Length -ge 2 -and $bytes[0] -eq 254 -and $bytes[1] -eq 255) {
        $enc=[Text.UnicodeEncoding]::new($true,$true,$true); $offset=2
    }
    $text=$enc.GetString($bytes,$offset,$bytes.Length-$offset)
    if($text.IndexOf([char]0) -ge 0) {
        throw "INI has an unexpected NUL byte: $resolved"
    }
    # Preserve Russian/Hebrew user commands, including non-UTF8 ANSI files.
    $roundtrip=$enc.GetBytes($text)
    if($roundtrip.Length -ne $bytes.Length-$offset) {
        throw "INI encoding cannot be round-tripped: $resolved"
    }
    for($i=0;$i -lt $roundtrip.Length;$i++) {
        if($roundtrip[$i] -ne $bytes[$offset+$i]) {
            throw "INI encoding conversion would lose bytes: $resolved"
        }
    }
    return [pscustomobject]@{
        Path=$resolved
        Bytes=$bytes
        Encoding=$enc
        Lines=[regex]::Split($text,'(?<=\n)')
    }
}

function Find-IniValue($Doc,[string]$Section,[string]$Key) {
    $active=''
    $value=$null
    foreach($raw in $Doc.Lines) {
        $line=([string]$raw).TrimEnd([char[]]@([char]13,[char]10))
        $header=[regex]::Match($line,'^\s*\[\s*(?<name>[^\]]+?)\s*\]\s*$')
        if($header.Success) {
            $active=$header.Groups['name'].Value
            continue
        }
        if($active -ine $Section) { continue }
        $match=[regex]::Match($line,'^\s*(?<key>[^=\s]+)\s*=\s*(?<value>.*?)\s*$')
        if($match.Success -and $match.Groups['key'].Value -ieq $Key) {
            if($null -ne $value) {
                throw "Duplicate $Key in [$Section] cannot be resolved safely."
            }
            $value=$match.Groups['value'].Value
        }
    }
    return $value
}

function Resolve-SectionIni($Root,[string]$Section) {
    $value=Find-IniValue $Root $Section 'RedirectSection'
    if([string]::IsNullOrWhiteSpace($value) -or $value -eq '0') {
        return $Root.Path
    }
    if($value -eq '1') {
        $value=Find-IniValue $Root 'Configuration' 'AlternateUserIni'
        if([string]::IsNullOrWhiteSpace($value)) {
            throw "RedirectSection=1 for [$Section] has no AlternateUserIni"
        }
    }
    $path=[Environment]::ExpandEnvironmentVariables($value.Trim().Trim('"'))
    if($path.Contains('%')) {
        throw "Unknown environment variable in [$Section] redirect."
    }
    if(-not [IO.Path]::IsPathRooted($path)) {
        $path=Join-Path (Split-Path -Parent $Root.Path) $path
    }
    if(-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing redirected [$Section] INI: $path"
    }
    return (Resolve-Path -LiteralPath $path).ProviderPath
}

function Matches-Plugin([string]$Value) {
    $parsed=[regex]::Match($Value,'^\s*\d+\s*,\s*(?<path>.+?)\s*$')
    if(-not $parsed.Success) {return $false}
    $dll=$parsed.Groups['path'].Value.Trim().Trim('"')
    if($PluginPath) {
        $a=($dll -replace '/','\').TrimEnd('\')
        $b=($PluginPath.Trim().Trim('"') -replace '/','\').TrimEnd('\')
        return [string]::Equals($a,$b,[StringComparison]::OrdinalIgnoreCase)
    }
    return [regex]::IsMatch($dll,'(?i)(^|[\\/])ISO-WCX[\\/]ISO-WCX\.wcx$')
}

function Find-Rows($Doc,[string]$Section,[string]$Key) {
    $active=''
    $indices=New-Object 'System.Collections.Generic.List[int]'
    $foreign=New-Object 'System.Collections.Generic.List[int]'
    for($i=0;$i -lt $Doc.Lines.Length;$i++) {
        $line=([string]$Doc.Lines[$i]).TrimEnd([char[]]@([char]13,[char]10))
        $header=[regex]::Match($line,'^\s*\[\s*(?<name>[^\]]+?)\s*\]\s*$')
        if($header.Success) {
            $active=$header.Groups['name'].Value
            continue
        }
        if($active -ine $Section) {continue}
        $match=[regex]::Match($line,'^\s*(?<key>[^=\s]+)\s*=\s*(?<value>.*?)\s*$')
        if(-not $match.Success -or $match.Groups['key'].Value -ine $Key) {
            continue
        }
        if($Section -ieq 'PackerPlugins64' -or
           (Matches-Plugin $match.Groups['value'].Value)) {
            $indices.Add($i)
        } else {
            $foreign.Add($i)
        }
    }
    return [pscustomobject]@{Selected=$indices;Foreign=$foreign}
}

function Remove-Rows($Doc,$Indices) {
    $omit=New-Object 'System.Collections.Generic.HashSet[int]'
    foreach($i in $Indices) { [void]$omit.Add([int]$i) }
    $builder=New-Object System.Text.StringBuilder
    for($i=0;$i -lt $Doc.Lines.Length;$i++) {
        if(-not $omit.Contains($i)) {
            [void]$builder.Append([string]$Doc.Lines[$i])
        }
    }
    $encoded=$Doc.Encoding.GetBytes($builder.ToString())
    $bom=$Doc.Encoding.GetPreamble()
    $output=[byte[]]::new($bom.Length+$encoded.Length)
    [Array]::Copy($bom,0,$output,0,$bom.Length)
    [Array]::Copy($encoded,0,$output,$bom.Length,$encoded.Length)
    return ,$output
}

function Bytes-Equal([byte[]]$A,[byte[]]$B) {
    if($A.Length -ne $B.Length) {return $false}
    for($i=0;$i -lt $A.Length;$i++) {
        if($A[$i] -ne $B[$i]) {return $false}
    }
    return $true
}

$root=Read-Ini $IniPath
$packerPath=Resolve-SectionIni $root 'PackerPlugins'
$cachePath=Resolve-SectionIni $root 'PackerPlugins64'
$packer=if($packerPath -ieq $root.Path) {$root} else {Read-Ini $packerPath}
$cache=if($cachePath -ieq $packer.Path) {$packer}
       elseif($cachePath -ieq $root.Path) {$root} else {Read-Ini $cachePath}
$matches=Find-Rows $packer 'PackerPlugins' $Extension
if($matches.Foreign.Count -gt 0) {
    throw "Extension '$Extension' has an unrelated WCX handler; refusing ambiguous removal."
}
Write-Host "Active PackerPlugins INI: $($packer.Path)"
Write-Host "Active PackerPlugins64 INI: $($cache.Path)"
Write-Host "Extension: $Extension; matching ISO-WCX rows: $($matches.Selected.Count)"
if($matches.Selected.Count -eq 0) {
    Write-Host 'No matching ISO-WCX association. Nothing changed.'
    return
}
$checksum=Find-Rows $cache 'PackerPlugins64' '$checksum$'
if(-not $Apply) {
    Write-Host 'DRY RUN: no file modified. Close Total Commander and rerun with -Apply.'
    return
}
if(@(Get-Process -Name 'TOTALCMD','TOTALCMD64' -ErrorAction SilentlyContinue).Count -gt 0) {
    throw 'Close all Total Commander windows before changing wincmd.ini.'
}

# Build exactly one private candidate per distinct physical INI. When the
# cache and packer settings are redirected independently, both candidates
# receive backups before either original can be replaced.
$plans=New-Object 'System.Collections.Generic.List[object]'
$drop=New-Object 'System.Collections.Generic.List[int]'
foreach($i in $matches.Selected) {$drop.Add($i)}
if($packer.Path -ieq $cache.Path) {
    foreach($i in $checksum.Selected) {$drop.Add($i)}
    $plans.Add([pscustomobject]@{Doc=$packer;Output=(Remove-Rows $packer $drop)})
} else {
    $plans.Add([pscustomobject]@{Doc=$packer;Output=(Remove-Rows $packer $drop)})
    if($checksum.Selected.Count -gt 0) {
        $plans.Add([pscustomobject]@{
            Doc=$cache;Output=(Remove-Rows $cache $checksum.Selected)
        })
    }
}
$stages=New-Object 'System.Collections.Generic.List[object]'
try {
    foreach($plan in $plans) {
        $current=[IO.File]::ReadAllBytes($plan.Doc.Path)
        if(-not (Bytes-Equal $current $plan.Doc.Bytes)) {
            throw "INI changed while planning the edit: $($plan.Doc.Path)"
        }
        $suffix=[guid]::NewGuid().ToString('N')
        $pending="$($plan.Doc.Path).isowcx-pending-$suffix"
        $backup="$($plan.Doc.Path).isowcx-association-backup-$suffix"
        [IO.File]::WriteAllBytes($pending,[byte[]]$plan.Output)
        $stages.Add([pscustomobject]@{
            Doc=$plan.Doc;Pending=$pending;Backup=$backup;Committed=$false
        })
    }
    foreach($stage in $stages) {
        if(-not (Bytes-Equal ([IO.File]::ReadAllBytes($stage.Doc.Path)) $stage.Doc.Bytes)) {
            throw "INI changed immediately before commit: $($stage.Doc.Path)"
        }
    }
    foreach($stage in $stages) {
        [IO.File]::Replace($stage.Pending,$stage.Doc.Path,$stage.Backup)
        $stage.Committed=$true
        Write-Host "Backup: $($stage.Backup)"
    }
} catch {
    # Restore all previously replaced INIs when a later redirected cache
    # replacement failed. Preserve the recovery backups on disk.
    foreach($stage in $stages) {
        if($stage.Committed -and (Test-Path -LiteralPath $stage.Backup)) {
            try {
                [IO.File]::Copy($stage.Backup,$stage.Doc.Path,$true)
            } catch {
                Write-Warning "Manual recovery required from $($stage.Backup)"
            }
        }
    }
    throw
} finally {
    foreach($stage in $stages) {
        if(Test-Path -LiteralPath $stage.Pending) {
            Remove-Item -LiteralPath $stage.Pending -Force
        }
    }
}
Write-Host "SUCCESS: removed $($matches.Selected.Count) ISO-WCX '$Extension' association(s)."
Write-Host 'Other WCX plugins, toolbar buttons and commands were preserved.'
Write-Host 'Restart Total Commander to reload the active association sections.'
