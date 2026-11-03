# Experimental host test of F5 on a REAL Total Commander 11.58.1 x64.
# Runs ONLY on disposable GitHub CI. Never writes a production WIM/INI.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Wcx32,
    [Parameter(Mandatory=$true)][string]$Wcx64,
    [string]$ReportPath='tc-f5-evidence/real-tc-f5.json'
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
if (-not $env:RUNNER_TEMP -or $env:GITHUB_ACTIONS -ne 'true') {
    throw 'Experimental F5 writes disposable WIMs: this tool is CI-only.'
}
$work=Join-Path $env:RUNNER_TEMP 'ISO-WCX-RealTC-F5'
$tcDir=Join-Path $work 'totalcmd'
$unpacked=Join-Path $work 'installer'
$pluginDir=Join-Path $work 'wcx'
$logDir=Join-Path $work 'local-app-data'
foreach($dir in @($work,$tcDir,$unpacked,$pluginDir,$logDir)) {
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
}
Copy-Item -LiteralPath (Resolve-Path -LiteralPath $Wcx32).Path -Destination (Join-Path $pluginDir 'ISO-WCX.wcx')
Copy-Item -LiteralPath (Resolve-Path -LiteralPath $Wcx64).Path -Destination (Join-Path $pluginDir 'ISO-WCX.wcx64')
$plugin=Join-Path $pluginDir 'ISO-WCX.wcx'
$oldLocalApp=$env:LOCALAPPDATA
$oldRepair=$env:ISO_WCX_DISABLE_ASSOC_REPAIR
$env:LOCALAPPDATA=$logDir
$env:ISO_WCX_DISABLE_ASSOC_REPAIR='1'
$logFile=Join-Path $logDir 'ISO-WCX/Logs/edit-errors.log'
$report=[ordered]@{
    purpose='Isolate whether Total Commander 11.58 invokes PackFilesW on F5'
    source='Disposable CI-only WIM, not any user image'
    shipping_caps=158
    alternative_159_is_test_only=$true
    template_wim_unchanged=$true
    cases=@()
    errors=@()
}
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class RealTcHostProbe {
    [DllImport("user32.dll",EntryPoint="FindWindowW",CharSet=CharSet.Unicode)]
    public static extern IntPtr FindWindow(string className,string title);
    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hwnd,out uint pid);
    [DllImport("user32.dll",EntryPoint="SendMessageTimeoutW",SetLastError=true)]
    public static extern IntPtr SendMessageTimeout(IntPtr hwnd,uint msg,
        IntPtr wp,IntPtr lp,uint flags,uint ms,out IntPtr value);
    [DllImport("user32.dll",SetLastError=true)]
    public static extern bool PostMessage(IntPtr hwnd,uint msg,UIntPtr wp,IntPtr lp);
}
'@
function Find-TcWindow([int]$targetPid) {
    for($i=0;$i -lt 50;$i++) {
        $h=[RealTcHostProbe]::FindWindow('TTOTAL_CMD',$null)
        if($h -ne [IntPtr]::Zero) {
            [uint32]$owner=0
            [void][RealTcHostProbe]::GetWindowThreadProcessId($h,[ref]$owner)
            if($owner -eq $targetPid)return $h
        }
        Start-Sleep -Milliseconds 300
    }
    return [IntPtr]::Zero
}
function Query-Tc([IntPtr]$h,[int]$query) {
    $answer=[IntPtr]::Zero
    $ok=[RealTcHostProbe]::SendMessageTimeout($h,[uint32]1074,
        [IntPtr]::new($query),[IntPtr]::Zero,[uint32]2,[uint32]1300,[ref]$answer)
    if($ok -eq [IntPtr]::Zero)return $null
    return $answer.ToInt64()
}
function Command-Tc([IntPtr]$h,[int]$command) {
    if(-not [RealTcHostProbe]::PostMessage($h,[uint32]1075,
       [UIntPtr]::new([uint32]$command),[IntPtr]::Zero)) {
        throw "Cannot send internal TC command $command"
    }
}
function Allow-RequestedTrialButton([int]$tcPid) {
    # Acknowledge only the exact shareware number displayed by TC.
    # Do not bypass the reminder, license check, or guess.
    try {
        Add-Type -AssemblyName UIAutomationClient
        $root=[System.Windows.Automation.AutomationElement]::RootElement
        $condition=[System.Windows.Automation.PropertyCondition]::new(
            [System.Windows.Automation.AutomationElement]::ProcessIdProperty,$tcPid)
        $all=$root.FindAll([System.Windows.Automation.TreeScope]::Descendants,$condition)
        $words=New-Object 'System.Collections.Generic.List[string]'
        $buttons=New-Object 'System.Collections.Generic.List[object]'
        for($i=0;$i -lt $all.Count;$i++) {
            $entry=$all.Item($i)
            $label=[string]$entry.Current.Name
            if([string]::IsNullOrWhiteSpace($label))continue
            $words.Add($label)
            if($entry.Current.ControlType -eq
               [System.Windows.Automation.ControlType]::Button -and
               $label.Trim() -match '^[123]$') { $buttons.Add($entry) }
        }
        $requested=[regex]::Match([string]::Join(' ',$words),
            '(?i)(?:press|click).{0,65}?(?:number\s*|button\s*|[#]\s*)?([123])\b')
        if(-not $requested.Success)return $false
        foreach($button in $buttons) {
            if($button.Current.Name.Trim() -eq $requested.Groups[1].Value) {
                $button.GetCurrentPattern(
                    [System.Windows.Automation.InvokePattern]::Pattern).Invoke()
                return $true
            }
        }
    }catch{}
    return $false
}
function Window-Names([int]$tcPid) {
    try {
        Add-Type -AssemblyName UIAutomationClient
        $root=[System.Windows.Automation.AutomationElement]::RootElement
        $filter=[System.Windows.Automation.PropertyCondition]::new(
            [System.Windows.Automation.AutomationElement]::ProcessIdProperty,$tcPid)
        $all=$root.FindAll([System.Windows.Automation.TreeScope]::Descendants,$filter)
        $names=New-Object 'System.Collections.Generic.List[string]'
        for($i=0;$i -lt $all.Count;$i++) {
            $entry=$all.Item($i)
            if($entry.Current.ControlType -eq
               [System.Windows.Automation.ControlType]::Window) {
                $label=[string]$entry.Current.Name
                if($label)$names.Add($label)
            }
        }
        return @($names)
    }catch { return @('UIAutomation unavailable') }
}
function Confirm-Only-DisposableCopy([int]$tcPid,[string]$temporaryImage) {
    # F5 often opens an ordinary copy-confirmation dialog before it calls
    # WCX PackFilesW. A test that merely posts cm_Copy and waits will
    # incorrectly report missing callbacks. Only accept a positively
    # identified, visible TC Copy/Pack dialog with the disposable WIM path.
    $answer=[ordered]@{
        prompted=$false; confirmed=$false; kind='none'
        windows=@(); visible_labels=@()
    }
    try { Add-Type -AssemblyName UIAutomationClient } catch {
        $answer.kind='UI Automation unavailable'
        return $answer
    }
    $root=[System.Windows.Automation.AutomationElement]::RootElement
    $filter=[System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::ProcessIdProperty,$tcPid)
    for($attempt=0;$attempt -lt 30;$attempt++) {
        $all=$root.FindAll([System.Windows.Automation.TreeScope]::Descendants,$filter)
        $windows=New-Object 'System.Collections.Generic.List[object]'
        for($i=0;$i -lt $all.Count;$i++) {
            $element=$all.Item($i)
            if($element.Current.ControlType -eq
               [System.Windows.Automation.ControlType]::Window) {
                $windows.Add($element)
            }
        }
        foreach($dialog in $windows) {
            $title=[string]$dialog.Current.Name
            if(-not $title -or $dialog.Current.IsOffscreen)continue
            if($title -match '(?i)Function not supported|not supported|not implemented') {
                $answer.prompted=$true
                $answer.kind='host refused F5 before copy confirmation'
                $answer.windows=@($answer.windows)+@($title)
                return $answer
            }
            if($title -notmatch '(?i)copy|pack|копир|упаков|копирование')continue
            $items=$dialog.FindAll(
                [System.Windows.Automation.TreeScope]::Descendants,
                [System.Windows.Automation.Condition]::TrueCondition)
            $labels=New-Object 'System.Collections.Generic.List[string]'
            $okButtons=New-Object 'System.Collections.Generic.List[object]'
            for($j=0;$j -lt $items.Count;$j++) {
                $item=$items.Item($j)
                $name=[string]$item.Current.Name
                if($name)$labels.Add($name)
                if($item.Current.ControlType -eq
                   [System.Windows.Automation.ControlType]::Edit) {
                    try {
                        $val=$item.GetCurrentPattern(
                            [System.Windows.Automation.ValuePattern]::Pattern).Current.Value
                        if($val)$labels.Add([string]$val)
                    }catch{}
                }
                if($item.Current.ControlType -eq
                   [System.Windows.Automation.ControlType]::Button -and
                   $name -match '^(?i:OK|Start|Copy|Pack|Копировать|Упаковать)$' -and
                   $item.Current.IsEnabled -and -not $item.Current.IsOffscreen) {
                    $okButtons.Add($item)
                }
            }
            $visible=([string]::Join(' ',$labels))
            $answer.windows=@($answer.windows)+@($title)
            $answer.visible_labels=@($answer.visible_labels)+@(
                $visible.Substring(0,[Math]::Min($visible.Length,500)))
            # Require the literal ephemeral WIM filename (or entire path)
            # inside the actual dialog, not just a similarly named window.
            if($visible.IndexOf($temporaryImage,
                   [StringComparison]::OrdinalIgnoreCase) -lt 0 -and
               $visible.IndexOf([IO.Path]::GetFileName($temporaryImage),
                   [StringComparison]::OrdinalIgnoreCase) -lt 0) {
                continue
            }
            $answer.prompted=$true
            if($okButtons.Count -ne 1) {
                $answer.kind='recognized copy dialog but no unambiguous confirm button'
                return $answer
            }
            try {
                $okButtons[0].GetCurrentPattern(
                    [System.Windows.Automation.InvokePattern]::Pattern).Invoke()
                $answer.confirmed=$true
                $answer.kind='explicit disposable WIM copy confirmed'
            }catch {
                $answer.kind='copy dialog could not be confirmed: '+$_.Exception.Message
            }
            return $answer
        }
        Start-Sleep -Milliseconds 250
    }
    $answer.kind='no copy dialog observed within 7.5 seconds'
    return $answer
}
function Tail-Wcx {
    if(Test-Path -LiteralPath $logFile)return @(Get-Content -LiteralPath $logFile -Tail 45)
    return @()
}
function Test-Mask([int]$mask,[string]$source,[string]$templateWim) {
    $caseDir=Join-Path $work "caps-$mask"
    New-Item -ItemType Directory -Force -Path $caseDir | Out-Null
    $image=Join-Path $caseDir 'test.wim'
    Copy-Item -LiteralPath $templateWim -Destination $image
    $ini=Join-Path $caseDir 'wincmd.ini'
    # 159 is an EXPERIMENT in a private temporary INI only. Never deploy 159.
    $contents=@"
[Configuration]
OnlyOnce=0
AutoInstallPlugins=0
[PackerPlugins]
wim=$mask,$plugin
[PackerPlugins64]
exts=wim*
"@
    [IO.File]::WriteAllText($ini,$contents,[Text.Encoding]::Unicode)
    $case=[ordered]@{
        mask=$mask; opened_wcx=$false; source_selected=$false
        active=$null; left_type=$null; right_type=$null
        right_archive_name_length=$null
        f5_sent=$false; packfiles_callback=$false; events=@()
        confirmation=$null; windows=@(); outcome='not tested'
    }
    $tc=$null
    try {
        $tcExe=Join-Path $tcDir 'TOTALCMD64.EXE'
        $arguments=@('/N',('/I="' + $ini + '"'),
                     ('/L="' + $source + '"'),
                     ('/R="' + $image + '\image1"'),'/P=L')
        $tc=Start-Process -FilePath $tcExe -ArgumentList $arguments -WorkingDirectory $tcDir -PassThru
        Start-Sleep -Seconds 4
        $tc.Refresh()
        if($tc.HasExited){$case.outcome='TC exited early';return $case}
        [void](Allow-RequestedTrialButton $tc.Id)
        Start-Sleep -Seconds 3
        $window=Find-TcWindow $tc.Id
        if($window -eq [IntPtr]::Zero) {
            $case.outcome='TC main window unavailable (trial prompt/CI desktop)'
            return $case
        }
        $case.active=Query-Tc $window 1000
        $case.left_type=Query-Tc $window 1025
        $case.right_type=Query-Tc $window 1026
        $case.right_archive_name_length=Query-Tc $window 1024
        $case.opened_wcx=($case.right_type -ge 10000 -and
            $case.right_archive_name_length -gt 0)
        if($case.active -ne 1 -or $case.left_type -ne 0 -or
           -not $case.opened_wcx) {
            $case.outcome='TC did not navigate into WIM image1 as right WCX pane'
            return $case
        }
        # Official TC external command API WM_USER+51=1075:
        # 3302=cm_SelectAllFiles; 905=cm_Copy (same as F5 button).
        Command-Tc $window 3302
        Start-Sleep -Milliseconds 700
        $case.source_selected=(Query-Tc $window 1019) -gt 0
        if(-not $case.source_selected) {
            $case.outcome='TC did not select the test source file'
            return $case
        }
        Command-Tc $window 905
        $case.f5_sent=$true
        $confirmation=Confirm-Only-DisposableCopy $tc.Id $image
        $case.confirmation=$confirmation
        Start-Sleep -Seconds 5
        $case.events=Tail-Wcx
        $case.windows=Window-Names $tc.Id
        $case.packfiles_callback=@($case.events |
            Where-Object { $_ -match 'PackFiles(W| ANSI) entered' }).Count -gt 0
        $case.outcome=if($case.packfiles_callback) {
            'PASS: real Total Commander invoked PackFiles'
        }elseif($confirmation.prompted -and -not $confirmation.confirmed) {
            'INCONCLUSIVE: F5 raised a modal copy/error dialog without a confirmed WCX callback'
        }elseif(-not $confirmation.prompted) {
            'INCONCLUSIVE: cm_Copy posted but the host copy dialog was not observed'
        }else {
            'FAIL: host did not invoke PackFiles after disposable copy confirmation'
        }
        return $case
    } finally {
        if($tc) {
            try {
                $tc.Refresh()
                if(-not $tc.HasExited)Stop-Process -Id $tc.Id -Force
            }catch{}
        }
    }
}
$failure=$null
try {
    # Official 11.58.1, released 2026-10-29; same build as the user's TC.
    # SHA-256 independently published by Chocolatey TotalCommander 11.58.1.
    $package=Join-Path $work 'tcmd115801x32_64.exe'
    Invoke-WebRequest -Uri 'https://totalcommander.ch/115801/tcmd115801x32_64.exe' -OutFile $package -MaximumRedirection 4
    $hash=(Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash.ToLowerInvariant()
    if($hash -ne '4696c3ac8a4b7f889e99e7348940e3e08f4b6cf62c4c572a71ae4252d813ea1d') {
        throw "Official Total Commander installer checksum mismatch: $hash"
    }
    $sign=Get-AuthenticodeSignature -FilePath $package
    if($sign.Status -ne 'Valid' -or
       $sign.SignerCertificate.Subject -notmatch 'Ghisler Software') {
        throw 'Original TC publisher signature missing or invalid'
    }
    & 7z x -y "-o$unpacked" $package | Out-Null
    if($LASTEXITCODE -ne 0)throw 'Cannot unpack original TC installer'
    # Combined x86/x64 installer can have more than one CAB. Extract
    # independently and select only the x64 folder to avoid DLL mixing.
    $cabs=@(Get-ChildItem -LiteralPath $unpacked -Recurse -File |
            Where-Object {$_.Name -match '(?i)^INSTALL.*[.]CAB$'})
    if($cabs.Count -eq 0)throw 'Combined official TC INSTALL.CAB missing'
    for($i=0;$i -lt $cabs.Count;$i++) {
        $dest=Join-Path $work ('cab-'+$i)
        New-Item -ItemType Directory -Force -Path $dest | Out-Null
        & 7z x -y "-o$dest" $cabs[$i].FullName | Out-Null
        if($LASTEXITCODE -ne 0)throw 'Cannot extract official TC CAB'
    }
    $exe=@(Get-ChildItem -LiteralPath $work -Recurse -File -Filter 'TOTALCMD64.EXE' |
           Where-Object {$_.FullName -match '[\\/]cab-[0-9]+[\\/]'})
    if($exe.Count -ne 1)throw ('Expected exactly one TC x64 executable, found '+$exe.Count)
    $sourceDir=$exe[0].Directory.FullName
    Get-ChildItem -LiteralPath $sourceDir -Force |
        Move-Item -Destination $tcDir -Force
    if(-not (Test-Path -LiteralPath (Join-Path $tcDir 'TOTALCMD64.EXE'))) {
        throw 'Portable official TC 11.58.1 x64 could not be assembled'
    }
    $source=Join-Path $work 'source'
    $capture=Join-Path $work 'capture'
    New-Item -ItemType Directory -Path $source,$capture -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $source 'add.txt'),
        'Real TC F5 input file')
    [IO.File]::WriteAllText((Join-Path $capture 'original.txt'),
        'Disposable original WIM content')
    $templateWim=Join-Path $work 'original-test.wim'
    & "$env:WINDIR\System32\dism.exe" /English /Capture-Image ("/ImageFile:$templateWim") ("/CaptureDir:$capture") /Name:CI /Compress:None | Out-Null
    if($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $templateWim)) {
        throw "DISM cannot capture disposable one-index WIM: $LASTEXITCODE"
    }
    $sha=(Get-FileHash -LiteralPath $templateWim -Algorithm SHA256).Hash
    foreach($mask in @(158,159)) {
        $report.cases+=Test-Mask $mask $source $templateWim
    }
    $report.template_wim_unchanged=
        $sha -eq (Get-FileHash -LiteralPath $templateWim -Algorithm SHA256).Hash
    if(-not $report.template_wim_unchanged)throw 'WIM template unexpectedly changed'
    if(@($report.cases | Where-Object {$_.packfiles_callback}).Count -eq 0) {
        throw 'No real TC F5 PackFiles callback observed; review JSON evidence'
    }
}catch{
    $failure=$_.Exception.Message
    $report.errors+= $failure
    Write-Warning $failure
}finally{
    $env:LOCALAPPDATA=$oldLocalApp
    $env:ISO_WCX_DISABLE_ASSOC_REPAIR=$oldRepair
    $path=[IO.Path]::GetFullPath($ReportPath)
    $folder=Split-Path -Parent $path
    New-Item -ItemType Directory -Path $folder -Force | Out-Null
    $report | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $path -Encoding UTF8
    Write-Host ('REAL_TC_F5_REPORT=' + $path)
    Get-Content -LiteralPath $path
}
if($failure)exit 1
