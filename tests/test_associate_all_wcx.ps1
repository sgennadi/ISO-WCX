# Synthetic end-to-end integration tests for ISO-WCX-AssociateAll.ps1.
# Never touches the real Total Commander user profile.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
function Assert([bool]$Ok,[string]$Why) {
    if (-not $Ok) { throw "BULK ASSOCIATION TEST FAILED: $Why" }
}
function Same([byte[]]$A,[byte[]]$B) {
    if($A.Length -ne $B.Length){return $false}
    for($i=0;$i -lt $A.Length;$i++){if($A[$i] -ne $B[$i]){return $false}}
    return $true
}
function FakePe([int]$Machine) {
    $b = [byte[]]::new(256)
    $b[0] = 77; $b[1] = 90; $b[60] = 128
    $b[128] = 80; $b[129] = 69
    $b[132] = [byte]($Machine -band 255)
    $b[133] = [byte](($Machine -shr 8) -band 255)
    return ,$b
}
$tool = Join-Path (Split-Path -Parent $PSScriptRoot) 'ISO-WCX-AssociateAll.ps1'
$root = Join-Path $env:TEMP ('isowcx-bulk-assoc-test-'+[guid]::NewGuid().ToString('N'))
$plugin = Join-Path $root 'plugins\wcx\ISO-WCX'
New-Item -ItemType Directory -Path $plugin -Force | Out-Null
[IO.File]::WriteAllBytes((Join-Path $plugin 'ISO-WCX.wcx'),(FakePe 0x014C))
[IO.File]::WriteAllBytes((Join-Path $plugin 'ISO-WCX.wcx64'),(FakePe 0x8664))
$target = Join-Path $plugin 'ISO-WCX.wcx'
$enc = [Text.UTF8Encoding]::new($true,$true)
$eol = [string][char]13+[char]10
$ini = Join-Path $root 'wincmd.ini'
# Construct Unicode test data independently of PowerShell 5.1 script encoding.
$hebrew = -join ([char[]]@(0x05D0,0x05D1,0x05D2,0x05D3))
$russian = -join ([char[]]@(0x0420,0x0443,0x0441,0x0441,0x043A,0x0438,0x0439))
function Set-Ini([string]$Path,[string[]]$Lines) {
    [IO.File]::WriteAllText($Path,(($Lines -join $eol)+$eol),$enc)
}
function Text-Ini([string]$Path) {
    return [IO.File]::ReadAllText($Path,$enc)
}
function Backups([string]$Path) {
    # Keep a zero-element array as an array under strict-mode PowerShell 5.1.
    $files = @(Get-ChildItem -LiteralPath (Split-Path -Parent $Path) -Filter ((Split-Path -Leaf $Path)+'.isowcx-bulk-backup-*'))
    return ,$files
}
try {
    # Main INI: preserve Cyrillic/Hebrew user settings, other plugins and ZIP.
    Set-Ini $ini @(
        '[Configuration]'
        "LanguageIni=$russian"
        "Buttonbar=C:\MyCommands\$hebrew.bar"
        'UserCmd=cm_myimportantmacro'
        '[PackerPlugins]'
        "iso=222,$target"
        'cue=735,C:\Other\cue.wcx'
        'zip=735,C:\Existing\zip.wcx'
        'rar=735,C:\Existing\rar.wcx'
        '7z=735,C:\Existing\7z.wcx'
        '[PackerPlugins64]'
        '$checksum$=123456'
        'exts=iso*cue*zip*rar*7z*'
    )
    $before = [IO.File]::ReadAllBytes($ini)
    & $tool -IniPath $ini -PluginDirectory $plugin | Out-Null
    Assert (Same $before ([IO.File]::ReadAllBytes($ini))) 'dry run modified INI'
    Assert ((Backups $ini).Count -eq 0) 'dry run made a backup'
    & $tool -IniPath $ini -PluginDirectory $plugin -Apply | Out-Null
    $after = [IO.File]::ReadAllBytes($ini)
    $txt = Text-Ini $ini
    Assert ($txt.Contains("iso=158,$target")) 'old mask was not refreshed'
    Assert ($txt.Contains("wim=158,$target")) 'wim missing'
    Assert ($txt.Contains("vhdx=158,$target")) 'vhdx missing'
    Assert ($txt.Contains("swm=158,$target")) 'swm missing'
    Assert ($txt.Contains("imz=158,$target")) 'imz missing'
    Assert (-not $txt.Contains("chd=158,$target")) 'experimental CHD unexpectedly registered'
    Assert (-not $txt.Contains("bin=158,$target")) 'ambiguous BIN unexpectedly registered'
    Assert ($txt.Contains('zip=735,C:\Existing\zip.wcx')) 'ZIP was hijacked'
    Assert ($txt.Contains('rar=735,C:\Existing\rar.wcx')) 'RAR was hijacked'
    Assert ($txt.Contains('7z=735,C:\Existing\7z.wcx')) '7z was hijacked'
    Assert ($txt.Contains('cue=735,C:\Other\cue.wcx')) 'foreign handler overwritten'
    Assert ($txt.Contains("Buttonbar=C:\MyCommands\$hebrew.bar")) 'custom buttons lost'
    Assert ($txt.Contains('UserCmd=cm_myimportantmacro')) 'user commands lost'
    Assert (-not $txt.Contains('$checksum$=')) 'stale WCX64 cache checksum remained'
    Assert ($txt.Contains('exts=iso*cue*zip*rar*7z*')) 'unrelated 64-bit cache fields lost'
    Assert ($after[0] -eq 239 -and $after[1] -eq 187 -and $after[2] -eq 191) 'UTF-8 BOM lost'
    Assert ((Backups $ini).Count -eq 1) 'one backup not created'
    Assert (Same $before ([IO.File]::ReadAllBytes((Backups $ini)[0].FullName))) 'backup is not byte exact'
    & $tool -IniPath $ini -PluginDirectory $plugin -Apply | Out-Null
    Assert (Same $after ([IO.File]::ReadAllBytes($ini))) 'rerun not idempotent'
    Assert ((Backups $ini).Count -eq 1) 'rerun added unnecessary backup'
    & $tool -IniPath $ini -PluginDirectory $plugin -IncludeOptional -Apply | Out-Null
    Assert ((Text-Ini $ini).Contains("chd=158,$target")) 'explicit CHD opt-in failed'
    Assert ((Text-Ini $ini).Contains("bin=158,$target")) 'explicit BIN opt-in failed'

    # Two redirected PackerPlugins INIs and separate root must remain isolated.
    $rootIni = Join-Path $root 'redirected-root.ini'
    $packers = Join-Path $root 'redirected-packers.ini'
    $cache = Join-Path $root 'redirected-cache.ini'
    Set-Ini $rootIni @(
        '[Configuration]'
        'LanguageIni=Hebrew'
        'Buttonbar=C:\OtherButtons.bar'
        '[PackerPlugins]'
        'RedirectSection=redirected-packers.ini'
        '[PackerPlugins64]'
        'RedirectSection=redirected-cache.ini'
    )
    Set-Ini $packers @(
        '[PackerPlugins]'
        'vhd=222,C:\Old\ISO-WCX\ISO-WCX.wcx'
        'zip=735,C:\Other\zip.wcx'
        '[Unrelated]'
        'UserMacro=hello'
    )
    Set-Ini $cache @(
        '[PackerPlugins64]'
        '$checksum$=987'
        'exts=zip*vhd*'
        '[Unrelated]'
        'OtherSetting=123'
    )
    $rootOriginal = [IO.File]::ReadAllBytes($rootIni)
    $cacheOriginal = [IO.File]::ReadAllBytes($cache)
    & $tool -IniPath $rootIni -PluginDirectory $plugin | Out-Null
    Assert (Same $rootOriginal ([IO.File]::ReadAllBytes($rootIni))) 'redirect dry-run mutated root'
    Assert (Same $cacheOriginal ([IO.File]::ReadAllBytes($cache))) 'redirect dry-run mutated cache'
    & $tool -IniPath $rootIni -PluginDirectory $plugin -Apply | Out-Null
    Assert (Same $rootOriginal ([IO.File]::ReadAllBytes($rootIni))) 'root buttons overwritten'
    Assert ((Text-Ini $packers).Contains("vhd=158,$target")) 'redirected own plugin was not updated'
    Assert ((Text-Ini $packers).Contains('zip=735,C:\Other\zip.wcx')) 'redirected ZIP overwritten'
    Assert ((Text-Ini $cache).Contains('exts=zip*vhd*')) 'redirected cache data lost'
    Assert (-not (Text-Ini $cache).Contains('$checksum$=')) 'redirected cache checksum remained'
    Assert ((Backups $packers).Count -eq 1) 'redirected packer backup missing'
    Assert ((Backups $cache).Count -eq 1) 'redirected cache backup missing'

    # A pre-existing duplicate key is ambiguous: refuse all changes.
    $collision = Join-Path $root 'duplicate.ini'
    Set-Ini $collision @(
        '[PackerPlugins]'
        'ISO=158,C:\Other\iso.wcx'
        'iso=158,C:\Other\another.wcx'
    )
    $collisionOriginal = [IO.File]::ReadAllBytes($collision)
    $caught = $false
    try { & $tool -IniPath $collision -PluginDirectory $plugin -Apply | Out-Null }
    catch { $caught = $true }
    Assert $caught 'duplicate ISO keys should be rejected'
    Assert (Same $collisionOriginal ([IO.File]::ReadAllBytes($collision))) 'duplicate INI was changed'

    # Missing or mismatched x64 WCX must never leave partial registrations.
    $invalid = Join-Path $root 'invalid.ini'
    Set-Ini $invalid @('[PackerPlugins]','zip=123,C:\Other\zip.wcx')
    $invalidBefore = [IO.File]::ReadAllBytes($invalid)
    [IO.File]::WriteAllBytes((Join-Path $plugin 'ISO-WCX.wcx64'),(FakePe 0x014C))
    $caught = $false
    try { & $tool -IniPath $invalid -PluginDirectory $plugin -Apply | Out-Null }
    catch { $caught = $true }
    Assert $caught 'incorrect PE architecture accepted'
    Assert (Same $invalidBefore ([IO.File]::ReadAllBytes($invalid))) 'bad DLL changed INI'
    Assert ((Backups $invalid).Count -eq 0) 'bad DLL created backup'

    Write-Host 'PASS: image associations, dry run, UTF-8 BOM, buttons, ZIP exclusion, redirects, x64 cache, backup, idempotency, optional formats, duplicate and PE failure'
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
