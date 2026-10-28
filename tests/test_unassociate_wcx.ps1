# Synthetic regression for ISO-WCX-RemoveAssociation.ps1. Never touches
# the active Total Commander settings, buttons or installed plugins.
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
function Assert-True([bool]$value,[string]$reason) {
    if(-not $value) { throw "TEST FAILED: $reason" }
}
function Equal-Bytes([byte[]]$a,[byte[]]$b) {
    if($a.Length -ne $b.Length) {return $false}
    for($i=0;$i -lt $a.Length;$i++) {
        if($a[$i] -ne $b[$i]) {return $false}
    }
    return $true
}
$tool=Join-Path (Split-Path -Parent $PSScriptRoot) 'ISO-WCX-RemoveAssociation.ps1'
$root=Join-Path $env:TEMP ("iso-wcx-unassociate-test-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root -Force | Out-Null
$ini=Join-Path $root 'wincmd.ini'
$encoding=[Text.UTF8Encoding]::new($true,$true)
$crlf=[string][char]13+[string][char]10
$plugin='C:\Program Files\TotalCmd\plugins\wcx\ISO-WCX\ISO-WCX.wcx'
$source=(@(
    '[Configuration]'
    'LanguageIni=Russian'
    'Buttonbar=C:\UserCommands\mybuttons.bar'
    '[PackerPlugins]'
    "iso=222,$plugin"
    "ISO=192,$plugin"
    "wim=222,$plugin"
    'zip=735,C:\Other\zip.wcx'
    '[PackerPlugins64]'
    '$checksum$=125'
    'exts=iso*wim*zip*'
) -join $crlf) + $crlf
try {
    [IO.File]::WriteAllText($ini,$source,$encoding)
    $before=[IO.File]::ReadAllBytes($ini)
    & $tool -IniPath $ini -Extension iso | Out-Null
    Assert-True (Equal-Bytes $before ([IO.File]::ReadAllBytes($ini))) 'dry-run changed INI'
    Assert-True (@(Get-ChildItem $root -Filter '*.isowcx-association-backup-*').Count -eq 0) 'dry-run made backup'
    & $tool -IniPath $ini -Extension iso -Apply | Out-Null
    $after=[IO.File]::ReadAllBytes($ini)
    Assert-True ($after.Length -ge 3 -and $after[0] -eq 239 -and $after[1] -eq 187 -and $after[2] -eq 191) 'lost UTF-8 BOM'
    $text=$encoding.GetString($after,3,$after.Length-3)
    $expected=(@(
        '[Configuration]'
        'LanguageIni=Russian'
        'Buttonbar=C:\UserCommands\mybuttons.bar'
        '[PackerPlugins]'
        "wim=222,$plugin"
        'zip=735,C:\Other\zip.wcx'
        '[PackerPlugins64]'
        'exts=iso*wim*zip*'
    ) -join $crlf) + $crlf
    Assert-True ($text -ceq $expected) 'changed buttonbar/unrelated plugin/cache rows or line endings'
    $backups=@(Get-ChildItem $root -Filter '*.isowcx-association-backup-*')
    Assert-True ($backups.Count -eq 1) 'missing or duplicate backup'
    Assert-True (Equal-Bytes $before ([IO.File]::ReadAllBytes($backups[0].FullName))) 'backup bytes differ from source'
    & $tool -IniPath $ini -Extension iso -Apply | Out-Null
    Assert-True (Equal-Bytes $after ([IO.File]::ReadAllBytes($ini))) 'idempotency failed'
    Assert-True (@(Get-ChildItem $root -Filter '*.isowcx-association-backup-*').Count -eq 1) 'idempotent run created backup'

    # Refuse ambiguous extension where an unrelated user handler exists.
    $mixedText=(@('[PackerPlugins]',"iso=222,$plugin",
                  'iso=192,C:\Other\iso.wcx') -join $crlf) + $crlf
    [IO.File]::WriteAllText($ini,$mixedText,$encoding)
    $mixed=[IO.File]::ReadAllBytes($ini)
    $caught=$false
    try { & $tool -IniPath $ini -Extension iso -Apply | Out-Null }
    catch { $caught=$true }
    Assert-True $caught 'mixed unrelated handler should be refused'
    Assert-True (Equal-Bytes $mixed ([IO.File]::ReadAllBytes($ini))) 'ambiguous association changed source'
    Write-Host 'PASS: exact ISO-WCX association removal, duplicate keys, backup, BOM, buttons, other extensions, idempotency and ambiguous refusal'
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
