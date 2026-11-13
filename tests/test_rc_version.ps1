param(
    [Parameter(Mandatory=$true)]
    [ValidateSet("Win32","x64")]
    [string] $Arch,
    [Parameter(Mandatory=$true)]
    [ValidateRange(1,65535)]
    [int] $ExpectedBuild,
    [Parameter(Mandatory=$false)]
    [ValidateRange(1,99)]
    [int] $ExpectedRC=1
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# Read the Stable project version as the source of truth. Preview is the
# next patch, never a hard-coded version pinned to a previous release.
$projectText = Get-Content -LiteralPath (Join-Path $PSScriptRoot "../CMakeLists.txt") -Raw -Encoding UTF8
$matched = [regex]::Match($projectText, '(?m)^project\(ISO_WCX VERSION (\d+)\.(\d+)\.(\d+) LANGUAGES C CXX\)\r?$')
if (-not $matched.Success) { throw "Cannot parse CMake Stable version" }
$major = [int]$matched.Groups[1].Value
$minor = [int]$matched.Groups[2].Value
$patch = [int]$matched.Groups[3].Value + 1
$expectedNumericVersion = "$major.$minor.$patch"

# Verify the actual compiled VERSIONINFO and the generated TC installer,
# rather than trusting the GitHub artifact name alone.
$binary = if ($Arch -eq "Win32") { "ISO-WCX.wcx" } else { "ISO-WCX.wcx64" }
$pluginPath = Join-Path $PWD.Path "build/Release/$binary"
if (-not (Test-Path -LiteralPath $pluginPath)) {
    throw "Compiled WCX was not found: $pluginPath"
}
$info = [System.Diagnostics.FileVersionInfo]::GetVersionInfo(
    (Resolve-Path -LiteralPath $pluginPath).Path)
$expected = "v$expectedNumericVersion RC$ExpectedRC (Build $ExpectedBuild)"
if ($info.FileVersion -ne $expected -or $info.ProductVersion -ne $expected) {
    throw "Invalid DLL version: $($info.FileVersion); expected: $expected"
}
if ($info.FileMajorPart -ne $major -or $info.FileMinorPart -ne $minor -or
    $info.FileBuildPart -ne $patch -or
    $info.FilePrivatePart -ne $ExpectedBuild) {
    throw "Incorrect binary VS_FIXEDFILEINFO: $($info.FileMajorPart).$($info.FileMinorPart).$($info.FileBuildPart).$($info.FilePrivatePart)"
}
$manifestPath = Join-Path $PWD.Path "build/pluginst.inf"
if (-not (Test-Path -LiteralPath $manifestPath)) {
    throw "Configured pluginst.inf not found"
}
$manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8
if ($manifest -notmatch "(?m)^version=$([regex]::Escape($expectedNumericVersion))\r?$" -or
    -not $manifest.Contains($expected)) {
    throw "Total Commander installer metadata is not RC$ExpectedRC Build $ExpectedBuild"
}

Write-Host "PASS: $binary - $expected (DLL, VERSIONINFO and pluginst.inf agree)"
