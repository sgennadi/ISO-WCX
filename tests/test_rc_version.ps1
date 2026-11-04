param(
    [Parameter(Mandatory=$true)]
    [ValidateSet("Win32","x64")]
    [string] $Arch,
    [Parameter(Mandatory=$true)]
    [ValidateRange(1,65535)]
    [int] $ExpectedBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# Verify the actual compiled VERSIONINFO and the generated TC installer,
# rather than trusting the GitHub artifact name alone.
$binary = if ($Arch -eq "Win32") { "ISO-WCX.wcx" } else { "ISO-WCX.wcx64" }
$pluginPath = Join-Path $PWD.Path "build/Release/$binary"
if (-not (Test-Path -LiteralPath $pluginPath)) {
    throw "Compiled WCX was not found: $pluginPath"
}
$info = [System.Diagnostics.FileVersionInfo]::GetVersionInfo(
    (Resolve-Path -LiteralPath $pluginPath).Path)
$expected = "v0.21.1 RC1 (Build $ExpectedBuild)"
if ($info.FileVersion -ne $expected -or $info.ProductVersion -ne $expected) {
    throw "Invalid DLL version: $($info.FileVersion); expected: $expected"
}
if ($info.FileMajorPart -ne 0 -or $info.FileMinorPart -ne 21 -or
    $info.FileBuildPart -ne 1 -or
    $info.FilePrivatePart -ne $ExpectedBuild) {
    throw "Incorrect binary VS_FIXEDFILEINFO: $($info.FileMajorPart).$($info.FileMinorPart).$($info.FileBuildPart).$($info.FilePrivatePart)"
}
$manifestPath = Join-Path $PWD.Path "build/pluginst.inf"
if (-not (Test-Path -LiteralPath $manifestPath)) {
    throw "Configured pluginst.inf not found"
}
$manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8
if ($manifest -notmatch "(?m)^version=0\.21\.1\r?$" -or
    -not $manifest.Contains($expected)) {
    throw "Total Commander installer metadata is not RC1 Build $ExpectedBuild"
}

Write-Host "PASS: $binary - $expected (DLL, VERSIONINFO and pluginst.inf agree)"
