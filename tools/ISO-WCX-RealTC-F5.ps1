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
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class RealTcHostProbe {
    [DllImport("user32.dll",EntryPoint="FindWindowW",CharSet=CharSet.Unicode)]
    public static extern IntPtr FindWindow(string className,string title);
    private delegate bool EnumWindowsDelegate(IntPtr hwnd,IntPtr param);
    [DllImport("user32.dll")]
    private static extern bool EnumWindows(EnumWindowsDelegate callback,IntPtr param);
    [DllImport("user32.dll",EntryPoint="GetClassNameW",CharSet=CharSet.Unicode)]
    private static extern int GetClassName(IntPtr hwnd,StringBuilder buffer,int length);
    [DllImport("user32.dll",EntryPoint="GetWindowTextW",CharSet=CharSet.Unicode)]
    private static extern int GetWindowText(IntPtr hwnd,StringBuilder buffer,int length);
    public static string CaptionOf(IntPtr hwnd) {
        if(hwnd==IntPtr.Zero) return "";
        var text=new StringBuilder(8192);
        GetWindowText(hwnd,text,text.Capacity);
        return text.ToString();
    }
    [DllImport("user32.dll")]
    private static extern bool IsWindowVisible(IntPtr hwnd);
    // Find by both class AND PID: FindWindow(TTOTAL_CMD) alone returns the
    // first window globally, potentially a different TC process on a
    // shared Windows runner. This previously produced a false no-window
    // result even if the new test instance had an actual main window.
    public static IntPtr FindOwnedMainWindow(uint targetPid) {
        IntPtr result=IntPtr.Zero;
        EnumWindows((hwnd,param)=>{
            uint pid=0;
            GetWindowThreadProcessId(hwnd,out pid);
            if(pid==targetPid) {
                var cls=new StringBuilder(256);
                GetClassName(hwnd,cls,cls.Capacity);
                if(cls.ToString().Equals("TTOTAL_CMD",
                       StringComparison.OrdinalIgnoreCase)) {
                    result=hwnd;
                    return false;
                }
            }
            return true;
        },IntPtr.Zero);
        return result;
    }
    public static string[] OwnedTopLevelWindows(uint targetPid) {
        var windows=new List<string>();
        EnumWindows((hwnd,param)=>{
            uint owner=0;
            GetWindowThreadProcessId(hwnd,out owner);
            if(owner==targetPid) {
                var cls=new StringBuilder(256);
                var name=new StringBuilder(1024);
                GetClassName(hwnd,cls,cls.Capacity);
                GetWindowText(hwnd,name,name.Capacity);
                windows.Add("hwnd="+hwnd.ToInt64().ToString()+
                    " class="+cls+" visible="+IsWindowVisible(hwnd).ToString()+
                    " title="+name);
            }
            return true;
        },IntPtr.Zero);
        return windows.ToArray();
    }
    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hwnd,out uint pid);
    [DllImport("user32.dll",EntryPoint="SendMessageTimeoutW",SetLastError=true)]
    public static extern IntPtr SendMessageTimeout(IntPtr hwnd,uint msg,
        IntPtr wp,IntPtr lp,uint flags,uint ms,out IntPtr value);
    [DllImport("user32.dll",SetLastError=true)]
    public static extern bool PostMessage(IntPtr hwnd,uint msg,UIntPtr wp,IntPtr lp);
}
'@
function Session-Snapshot([int]$targetPid) {
    $info=[ordered]@{
        runner_user_interactive=[Environment]::UserInteractive
        powershell_session=[Diagnostics.Process]::GetCurrentProcess().SessionId
        tc_session=$null
        tc_has_exited=$null
        tc_main_window_handle=0
        tc_main_window_title=''
        top_level_windows=@()
        automation_windows=@()
    }
    try {
        $proc=Get-Process -Id $targetPid -ErrorAction Stop
        $proc.Refresh()
        $info.tc_session=$proc.SessionId
        $info.tc_has_exited=$proc.HasExited
        if(-not $proc.HasExited) {
            $info.tc_main_window_handle=$proc.MainWindowHandle.ToInt64()
            $info.tc_main_window_title=$proc.MainWindowTitle
            $info.top_level_windows=@(
                [RealTcHostProbe]::OwnedTopLevelWindows([uint32]$targetPid))
            $info.automation_windows=@(Window-Names $targetPid)
        }
    }catch {
        $info.automation_windows=@('Process inspection failed: '+$_.Exception.Message)
    }
    return $info
}
function Save-RunnerScreenshot([string]$folder) {
    try {
        Add-Type -AssemblyName System.Drawing
        Add-Type -AssemblyName System.Windows.Forms
        $bounds=[System.Windows.Forms.Screen]::PrimaryScreen.Bounds
        if($bounds.Width -le 0 -or $bounds.Height -le 0) { return }
        $bitmap=New-Object Drawing.Bitmap($bounds.Width,$bounds.Height)
        try {
            $drawing=[Drawing.Graphics]::FromImage($bitmap)
            try {
                $drawing.CopyFromScreen($bounds.Left,$bounds.Top,0,0,
                    $bitmap.Size)
            }finally { $drawing.Dispose() }
            $bitmap.Save((Join-Path $folder 'totalcmd-screen.png'),
                [Drawing.Imaging.ImageFormat]::Png)
        }finally { $bitmap.Dispose() }
    }catch{}
}
function Find-TcWindow([int]$targetPid) {
    for($i=0;$i -lt 50;$i++) {
        $h=[RealTcHostProbe]::FindOwnedMainWindow([uint32]$targetPid)
        if($h -ne [IntPtr]::Zero) { return $h }
        # Some TC launcher conditions expose a main handle before the
        # first visible TTOTAL_CMD. It is still PID-checked.
        try {
            $tc=Get-Process -Id $targetPid -ErrorAction Stop
            $tc.Refresh()
            if(-not $tc.HasExited -and
               $tc.MainWindowHandle -ne [IntPtr]::Zero) {
                [uint32]$owner=0
                [void][RealTcHostProbe]::GetWindowThreadProcessId(
                    $tc.MainWindowHandle,[ref]$owner)
                if($owner -eq $targetPid) {
                    return $tc.MainWindowHandle
                }
            }
        }catch {}
        Start-Sleep -Milliseconds 300
    }
    return [IntPtr]::Zero
}
function Query-Tc([IntPtr]$h,[int]$query) {
    $answer=[IntPtr]::Zero
    $ok=[RealTcHostProbe]::SendMessageTimeout($h,[uint32]1074,
        [IntPtr]::new($query),[IntPtr]::Zero,[uint32]2,[uint32]1300,[ref]$answer)
    if($ok -eq [IntPtr]::Zero) { return $null }
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
            if([string]::IsNullOrWhiteSpace($label)) { continue }
            $words.Add($label)
            if($entry.Current.ControlType -eq
               [System.Windows.Automation.ControlType]::Button -and
               $label.Trim() -match '^[123]$') { $buttons.Add($entry) }
        }
        $requested=[regex]::Match([string]::Join(' ',$words),
            '(?i)(?:press|click).{0,65}?(?:number\s*|button\s*|[#]\s*)?([123])\b')
        if(-not $requested.Success) { return $false }
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
                if($label) { $names.Add($label) }
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
            if(-not $title -or $dialog.Current.IsOffscreen) { continue }
            if($title -match '(?i)Function not supported|not supported|not implemented') {
                $answer.prompted=$true
                $answer.kind='host refused F5 before copy confirmation'
                $answer.windows=@($answer.windows)+@($title)
                return $answer
            }
            if($title -notmatch '(?i)copy|pack|копир|упаков|копирование') { continue }
            $items=$dialog.FindAll(
                [System.Windows.Automation.TreeScope]::Descendants,
                [System.Windows.Automation.Condition]::TrueCondition)
            $labels=New-Object 'System.Collections.Generic.List[string]'
            $okButtons=New-Object 'System.Collections.Generic.List[object]'
            for($j=0;$j -lt $items.Count;$j++) {
                $item=$items.Item($j)
                $name=[string]$item.Current.Name
                if($name) { $labels.Add($name) }
                if($item.Current.ControlType -eq
                   [System.Windows.Automation.ControlType]::Edit) {
                    try {
                        $val=$item.GetCurrentPattern(
                            [System.Windows.Automation.ValuePattern]::Pattern).Current.Value
                        if($val) { $labels.Add([string]$val) }
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
    if(Test-Path -LiteralPath $logFile) { return @(Get-Content -LiteralPath $logFile -Tail 45) }
    return @()
}
function Test-Mask([int]$mask,[string]$source,[string]$templateWim) {
    $caseDir=Join-Path $work "caps-$mask"
    New-Item -ItemType Directory -Force -Path $caseDir | Out-Null
    $image=Join-Path $caseDir 'test.wim'
    Copy-Item -LiteralPath $templateWim -Destination $image
    # Keep the INI OUTSIDE the archive-directory listing, otherwise
    # cm_GoToFirstFile could pick wincmd.ini rather than our only test.wim.
    $profileDir=Join-Path $work "profile-$mask"
    New-Item -ItemType Directory -Force -Path $profileDir | Out-Null
    $ini=Join-Path $profileDir 'wincmd.ini'
    $caseLocalApp=Join-Path $work "local-app-data-$mask"
    New-Item -ItemType Directory -Force -Path $caseLocalApp | Out-Null
    $env:LOCALAPPDATA=$caseLocalApp
    $script:logFile=Join-Path $caseLocalApp 'ISO-WCX/Logs/edit-errors.log'
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
        confirmation=$null; windows=@(); session=$null
        observed_ini_binding=$null
        navigation=@(); right_path=''
        outcome='not tested'
    }
    $tc=$null
    try {
        $tcExe=Join-Path $tcDir 'TOTALCMD64.EXE'
        # /R accepts ordinary directories reliably, but archive-internal
        # paths such as test.wim\image1 were observed to leave TC in a
        # regular filesystem folder. Enter the WCX via internal commands.
        $arguments=@('/N',('/I="' + $ini + '"'),
                     ('/L="' + $source + '"'),
                     ('/R="' + $caseDir + '"'),'/P=L')
        $tc=Start-Process -FilePath $tcExe -ArgumentList $arguments -WorkingDirectory $tcDir -PassThru
        Start-Sleep -Seconds 4
        $tc.Refresh()
        if($tc.HasExited){$case.outcome='TC exited early';return $case}
        # Capture whether TC rewrote the disposable 159 mask to 158 at
        # registration. Without this, the A/B result is misleading.
        $case.observed_ini_binding=(@(Get-Content -LiteralPath $ini |
            Where-Object {$_ -match '^\s*wim\s*='} |
            Select-Object -First 1) -join '').Trim()
        [void](Allow-RequestedTrialButton $tc.Id)
        Start-Sleep -Seconds 3
        $window=Find-TcWindow $tc.Id
        if($window -eq [IntPtr]::Zero) {
            $case.session=Session-Snapshot $tc.Id
            $case.windows=$case.session.top_level_windows
            $case.events=Tail-Wcx
            $case.outcome='INCONCLUSIVE: no TC main window; F5 NOT SENT (trial or desktop blocker)'
            return $case
        }
        # Only the disposable test.wim exists in the right filesystem
        # folder. All commands below are documented Total Commander IDs:
        # cm_FocusRight=4002, cm_GoToFirstFile=2050,
        # cm_GoToDir=2003, cm_GoToFirstEntry=2049,
        # cm_FocusLeft=4001. This is equivalent to Enter on test.wim
        # and Enter on its image1 directory, then Tab back to the source.
        $case.navigation+= '4002 cm_FocusRight'
        Command-Tc $window 4002
        Start-Sleep -Milliseconds 250
        $case.navigation+= '2050 cm_GoToFirstFile (test.wim)'
        Command-Tc $window 2050
        Start-Sleep -Milliseconds 250
        $case.navigation+= '2003 cm_GoToDir (open WCX test.wim)'
        Command-Tc $window 2003
        $enteredArchive=$false
        for($i=0;$i -lt 40;$i++) {
            Start-Sleep -Milliseconds 250
            $type=Query-Tc $window 1026
            $archiveLength=Query-Tc $window 1024
            if($null -ne $type -and $type -ge 10000 -and
               $archiveLength -gt 0) {
                $enteredArchive=$true
                break
            }
        }
        if(-not $enteredArchive) {
            $case.right_type=Query-Tc $window 1026
            $case.right_archive_name_length=Query-Tc $window 1024
            $case.events=Tail-Wcx
            $case.outcome='INCONCLUSIVE: cm_GoToDir could not open disposable WIM in right WCX pane'
            return $case
        }
        $case.navigation+= '2049 cm_GoToFirstEntry (image1)'
        Command-Tc $window 2049
        Start-Sleep -Milliseconds 250
        $case.navigation+= '2003 cm_GoToDir (enter image1)'
        Command-Tc $window 2003
        Start-Sleep -Milliseconds 750
        $case.navigation+= '4001 cm_FocusLeft (return to source)'
        Command-Tc $window 4001
        Start-Sleep -Milliseconds 350
        $case.active=Query-Tc $window 1000
        $case.left_type=Query-Tc $window 1025
        $case.right_type=Query-Tc $window 1026
        $case.right_archive_name_length=Query-Tc $window 1024
        $case.opened_wcx=($case.right_type -ge 10000 -and
            $case.right_archive_name_length -gt 0)
        # TC WM_USER+50 query 10 obtains right panel path-label handle.
        # An empty caption may mean custom drawing; do not guess based
        # on the caption alone, but do reject a proven wrong path.
        $pathHandle=Query-Tc $window 10
        if($null -ne $pathHandle -and $pathHandle -gt 0) {
            $case.right_path=[RealTcHostProbe]::CaptionOf(
                [IntPtr]::new($pathHandle))
        }
        if($case.active -ne 1 -or $case.left_type -ne 0 -or
           -not $case.opened_wcx) {
            $case.events=Tail-Wcx
            $case.outcome='INCONCLUSIVE: WCX navigation or focus not confirmed; F5 was NOT SENT'
            return $case
        }
        if($case.right_path -and
           $case.right_path -notmatch '(?i)\\image1(?:\\|$)') {
            $case.events=Tail-Wcx
            $case.outcome='INCONCLUSIVE: right WCX panel is not inside image1; F5 was NOT SENT'
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
            try { Save-RunnerScreenshot $caseDir } catch {}
            try {
                $tc.Refresh()
                if(-not $tc.HasExited) { Stop-Process -Id $tc.Id -Force }
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
    if($LASTEXITCODE -ne 0) { throw 'Cannot unpack original TC installer' }
    # Combined x86/x64 installer can have more than one CAB. Extract
    # independently and select only the x64 folder to avoid DLL mixing.
    $cabs=@(Get-ChildItem -LiteralPath $unpacked -Recurse -File |
            Where-Object {$_.Name -match '(?i)^INSTALL.*[.]CAB$'})
    if($cabs.Count -eq 0) { throw 'Combined official TC INSTALL.CAB missing' }
    for($i=0;$i -lt $cabs.Count;$i++) {
        $dest=Join-Path $work ('cab-'+$i)
        New-Item -ItemType Directory -Force -Path $dest | Out-Null
        & 7z x -y "-o$dest" $cabs[$i].FullName | Out-Null
        if($LASTEXITCODE -ne 0) { throw 'Cannot extract official TC CAB' }
    }
    $exe=@(Get-ChildItem -LiteralPath $work -Recurse -File -Filter 'TOTALCMD64.EXE' |
           Where-Object {$_.FullName -match '[\\/]cab-[0-9]+[\\/]'})
    if($exe.Count -ne 1) { throw ('Expected exactly one TC x64 executable, found '+$exe.Count) }
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
    if(-not $report.template_wim_unchanged) { throw 'WIM template unexpectedly changed' }
    $attempted=@($report.cases | Where-Object {$_.f5_sent})
    if($attempted.Count -eq 0) {
        throw 'INCONCLUSIVE HOST: no live TC main window was available; F5 was NOT SENT. Inspect session and window evidence.'
    }
    if(@($report.cases | Where-Object {$_.packfiles_callback}).Count -eq 0) {
        throw 'F5 was sent but no PackFiles callback observed; inspect confirmation state and host windows.'
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
if($failure) { exit 1 }
