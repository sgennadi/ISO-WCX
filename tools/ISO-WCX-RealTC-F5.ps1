# Experimental host test of F5 on a REAL Total Commander 11.58.1 x64.
# Runs ONLY on disposable GitHub CI. Never writes a production WIM/INI.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Wcx32,
    [Parameter(Mandatory=$true)][string]$Wcx64,
    [Parameter(Mandatory=$true)][string]$Wcx64Control,
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
$controlPluginDir=Join-Path $work 'wcx-ci-old-caps'
$logDir=Join-Path $work 'local-app-data'
foreach($dir in @($work,$tcDir,$unpacked,$pluginDir,$controlPluginDir,$logDir)) {
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
}
Copy-Item -LiteralPath (Resolve-Path -LiteralPath $Wcx32).Path -Destination (Join-Path $pluginDir 'ISO-WCX.wcx')
Copy-Item -LiteralPath (Resolve-Path -LiteralPath $Wcx64).Path -Destination (Join-Path $pluginDir 'ISO-WCX.wcx64')
# Shipping WCX returns 159; only the disposable negative control returns 158.
# CI's 158 case uses a separately compiled x64 DLL whose actual
# GetPackerCaps() returns 158. Changing INI alone is NOT a valid A/B test.
Copy-Item -LiteralPath (Join-Path $pluginDir 'ISO-WCX.wcx') -Destination (
    Join-Path $controlPluginDir 'ISO-WCX.wcx')
Copy-Item -LiteralPath (
    (Resolve-Path -LiteralPath $Wcx64Control).Path) -Destination (
    Join-Path $controlPluginDir 'ISO-WCX.wcx64')
$plugin=Join-Path $pluginDir 'ISO-WCX.wcx'
$controlPlugin=Join-Path $controlPluginDir 'ISO-WCX.wcx'
$oldLocalApp=$env:LOCALAPPDATA
$oldRepair=$env:ISO_WCX_DISABLE_ASSOC_REPAIR
$env:LOCALAPPDATA=$logDir
$env:ISO_WCX_DISABLE_ASSOC_REPAIR='1'
$logFile=Join-Path $logDir 'ISO-WCX/Logs/edit-errors.log'
$report=[ordered]@{
    purpose='Isolate whether Total Commander 11.58 invokes PackFilesW on F5'
    source='Disposable CI-only WIM and ISO, never user images'
    shipping_caps=159
    control_158_is_test_only=$true
    template_wim_unchanged=$true
    cases=@()
    errors=@()
}
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;
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
    [DllImport("user32.dll")]
    private static extern bool IsWindowEnabled(IntPtr hwnd);
    private static readonly uint WM_GETTEXT=0x000D;
    private static readonly uint BM_CLICK=0x00F5;
    private delegate bool EnumChildDelegate(IntPtr hwnd,IntPtr param);
    [DllImport("user32.dll")]
    private static extern bool EnumChildWindows(IntPtr parent,
        EnumChildDelegate callback,IntPtr param);
    [DllImport("user32.dll",EntryPoint="SendMessageTimeoutW",
        CharSet=CharSet.Unicode,SetLastError=true)]
    private static extern IntPtr SendMessageTimeoutText(IntPtr hwnd,uint message,
        IntPtr wparam,[Out]StringBuilder lparam,
        uint flags,uint timeout,out IntPtr result);
    [DllImport("user32.dll",EntryPoint="SendMessageTimeoutW",SetLastError=true)]
    private static extern IntPtr SendMessageTimeoutRaw(IntPtr hwnd,uint message,
        IntPtr wparam,IntPtr lparam,
        uint flags,uint timeout,out IntPtr result);
    private sealed class Child {
        public IntPtr hwnd;
        public string kind;
        public string text;
        public bool enabled;
    }
    private static string ReadChildCaption(IntPtr hwnd) {
        var text=new StringBuilder(1024);
        IntPtr count;
        if(SendMessageTimeoutText(hwnd,WM_GETTEXT,
             (IntPtr)text.Capacity,text,2,750,out count)==IntPtr.Zero)
             return "";
        return text.ToString();
    }
    // The real Total Commander trial reminder on the Windows runner shows
    // "Please press button Nr. 1 to start the program!". UIAutomation
    // sometimes does not expose that static text, so use native HWNDs.
    // This does not suppress the publisher's reminder: it reads the literal
    // requested number and clicks ONLY that enabled button in the SAME
    // top-level "Total Commander" dialog. Otherwise it refuses.
    // explicitDigit can be supplied ONLY by the strict screenshot/OCR
    // reader after it observed the publisher's entire imperative text.
    public static string ClickRequestedTrialButton(uint targetPid,string explicitDigit) {
        var results=new List<string>();
        bool clicked=false;
        EnumWindows((top,param)=>{
            uint owner=0;
            GetWindowThreadProcessId(top,out owner);
            if(owner!=targetPid || !IsWindowVisible(top))return true;
            string title=CaptionOf(top);
            if(!title.Equals("Total Commander",
                StringComparison.OrdinalIgnoreCase))return true;
            var controls=new List<Child>();
            EnumChildWindows(top,(child,unused)=>{
                var cls=new StringBuilder(128);
                GetClassName(child,cls,cls.Capacity);
                controls.Add(new Child {
                    hwnd=child,kind=cls.ToString(),text=ReadChildCaption(child),
                    enabled=IsWindowEnabled(child)
                });
                return true;
            },IntPtr.Zero);
            var captions=new StringBuilder();
            foreach(var item in controls) {
                if(item.text.Length>0) {
                    captions.Append(' ').Append(item.text);
                }
            }
            string text=captions.ToString();
            var match=Regex.Match(text,
                @"Please\s+press\s+button\s+Nr\.?\s*([123])\s+to\s+start\s+the\s+program",
                RegexOptions.IgnoreCase);
            results.Add("trial_dialog="+title+
                " child_count="+controls.Count+
                " explicit_instruction="+match.Success+
                " trusted_ocr_digit="+(explicitDigit ?? "")+
                " text="+text.Substring(0,Math.Min(text.Length,330)));
            if(!match.Success &&
               !(explicitDigit=="1" || explicitDigit=="2" ||
                 explicitDigit=="3"))return true;
            string digit=match.Success?match.Groups[1].Value:explicitDigit;
            Child candidate=null;
            int count=0;
            foreach(var control in controls) {
                // Total Commander Win32 shortcut captions contain '&1',
                // '&2', '&3', not merely '1', '2', '3'.
                if(control.text.Replace("&","").Trim()!=digit ||
                   control.kind.IndexOf("Button",
                       StringComparison.OrdinalIgnoreCase)<0 ||
                   !control.enabled)continue;
                candidate=control;
                count++;
            }
            if(count!=1) {
                results.Add("required_button="+digit+
                    " eligible_buttons="+count+" no_button_pressed");
                return true;
            }
            IntPtr returned;
            IntPtr ok=SendMessageTimeoutRaw(candidate.hwnd,BM_CLICK,
                IntPtr.Zero,IntPtr.Zero,2,1500,out returned);
            clicked=ok!=IntPtr.Zero;
            results.Add("required_button="+digit+
                " native_BM_CLICK="+(clicked?"success":"failure"));
            return !clicked;
        },IntPtr.Zero);
        return "NATIVE_TRIAL_CLICKED="+(clicked?"1":"0")+
            " "+String.Join(" | ",results.ToArray());
    }
    // Return the bounds only if exactly one visible owner-process dialog
    // contains the three publisher-provided shareware number buttons.
    // A separate TC main window may have the same title: do not count it
    // as an ambiguous trial dialog or OCR the whole desktop.
    [StructLayout(LayoutKind.Sequential)]
    private struct WindowRect {
        public int left, top, right, bottom;
    }
    [DllImport("user32.dll",SetLastError=true)]
    private static extern bool GetWindowRect(IntPtr hwnd,out WindowRect rect);
    private static bool HasTrialNumberButtons(IntPtr top) {
        bool one=false,two=false,three=false;
        EnumChildWindows(top,(child,param)=>{
            if(!IsWindowEnabled(child))return true;
            var cls=new StringBuilder(128);
            GetClassName(child,cls,cls.Capacity);
            if(cls.ToString().IndexOf("Button",
                 StringComparison.OrdinalIgnoreCase)<0)return true;
            string name=ReadChildCaption(child).Replace("&","").Trim();
            if(name=="1")one=true;
            if(name=="2")two=true;
            if(name=="3")three=true;
            return true;
        },IntPtr.Zero);
        return one && two && three;
    }
    public static int[] UniqueTrialDialogBounds(uint targetPid) {
        IntPtr match=IntPtr.Zero;
        int count=0;
        EnumWindows((top,param)=>{
            uint owner=0;
            GetWindowThreadProcessId(top,out owner);
            if(owner!=targetPid || !IsWindowVisible(top) ||
               !CaptionOf(top).Equals("Total Commander",
                   StringComparison.OrdinalIgnoreCase) ||
               !HasTrialNumberButtons(top))return true;
            match=top;
            count++;
            return true;
        },IntPtr.Zero);
        if(count!=1)return new int[0];
        WindowRect rect;
        if(!GetWindowRect(match,out rect) ||
           rect.right<=rect.left || rect.bottom<=rect.top)
           return new int[0];
        return new int[]{rect.left,rect.top,
                         rect.right-rect.left,rect.bottom-rect.top};
    }
    // The user-facing F5 path is not always a "Copy" dialog. When
    // PK_CAPS_NEW is enabled, TC can display the native "Pack files" dialog.
    // Our earlier UIAutomation-only test did not confirm it and therefore
    // never reached PackFilesW. Accept only a dialog owned by THIS exact TC
    // process and explicitly targeting THIS disposable test.wim image.
    public static string ConfirmDisposablePackFiles(uint targetPid,string expectedWim) {
        int found=0, eligible=0, confirmed=0, okCount=0, cancelCount=0;
        bool pathSeen=false;
        string reason="no Pack files dialog";
        EnumWindows((top,param)=>{
            uint owner=0;
            GetWindowThreadProcessId(top,out owner);
            if(owner!=targetPid || !IsWindowVisible(top) ||
                !CaptionOf(top).Equals("Pack files",
                    StringComparison.OrdinalIgnoreCase))return true;
            found++;
            var controls=new List<Child>();
            EnumChildWindows(top,(child,unused)=>{
                var cls=new StringBuilder(128);
                GetClassName(child,cls,cls.Capacity);
                controls.Add(new Child {
                    hwnd=child,kind=cls.ToString(),
                    text=ReadChildCaption(child),enabled=IsWindowEnabled(child)
                });
                return true;
            },IntPtr.Zero);
            Child ok=null;
            int localOk=0,localCancel=0;
            bool localPath=false;
            foreach(var control in controls) {
                var text=control.text.Replace("&","").Trim();
                if(text.IndexOf(expectedWim,
                        StringComparison.OrdinalIgnoreCase)>=0)
                    localPath=true;
                if(control.kind.IndexOf("Button",
                        StringComparison.OrdinalIgnoreCase)<0 ||
                   !control.enabled)continue;
                if(text.Equals("OK",StringComparison.OrdinalIgnoreCase)) {
                    localOk++;ok=control;
                }else if(text.Equals("Cancel",
                          StringComparison.OrdinalIgnoreCase))
                    localCancel++;
            }
            pathSeen|=localPath;
            okCount+=localOk;
            cancelCount+=localCancel;
            if(localPath && localOk==1 && localCancel==1 && ok!=null) {
                eligible++;
                IntPtr result;
                if(SendMessageTimeoutRaw(ok.hwnd,BM_CLICK,IntPtr.Zero,
                    IntPtr.Zero,2,1500,out result)!=IntPtr.Zero) {
                    confirmed++;
                    reason="verified temporary WIM pack-dialog OK";
                } else reason="native OK click timed out";
            } else {
                reason="Pack files dialog not unambiguous: path="+localPath+
                    " ok="+localOk+" cancel="+localCancel;
            }
            return true;
        },IntPtr.Zero);
        return "TC_DISPOSABLE_PACK_DIALOG found="+found+
            " eligible="+eligible+" clicked="+(found==1 && confirmed==1?1:0)+
            " path_seen="+(pathSeen?1:0)+" ok="+okCount+
            " cancel="+cancelCount+" reason="+reason;
    }
    // Detect the actual TC-owned pre-callback error, which can have title
    // "wim", not "Function not supported". This is read-only.
    public static string DetectHostUnsupported(uint targetPid) {
        int found=0;
        EnumWindows((top,param)=>{
            uint owner=0;
            GetWindowThreadProcessId(top,out owner);
            if(owner!=targetPid || !IsWindowVisible(top))return true;
            if(!CaptionOf(top).Equals("wim",StringComparison.OrdinalIgnoreCase))
                return true;
            EnumChildWindows(top,(child,unused)=>{
                if(ReadChildCaption(child).IndexOf("Function not supported",
                    StringComparison.OrdinalIgnoreCase)>=0)found++;
                return true;
            },IntPtr.Zero);
            return true;
        },IntPtr.Zero);
        return "TC_HOST_UNSUPPORTED_DIALOG="+(found>0?1:0);
    }
    // The official first-run TC window "Configuration" / "Layout" can
    // remain modal AFTER the publisher's numbered start reminder. This is
    // a separate dialog; if it stays open, cm_GoToDir and cm_Copy never
    // reach the panels. Accept only the exact known Configuration window
    // with a single visible enabled OK and Cancel pair. This runs solely
    // under disposable GitHub CI, with a private temporary wincmd.ini.
    public static string AcceptDisposableFirstRunConfiguration(uint targetPid) {
        int dialogs=0;
        bool clicked=false;
        string reason="not present";
        EnumWindows((top,param)=>{
            uint owner=0;
            GetWindowThreadProcessId(top,out owner);
            if(owner!=targetPid || !IsWindowVisible(top) ||
               !CaptionOf(top).Equals("Configuration",
                   StringComparison.OrdinalIgnoreCase))return true;
            dialogs++;
            IntPtr ok=IntPtr.Zero;
            int okCount=0, cancelCount=0;
            EnumChildWindows(top,(child,unused)=>{
                if(!IsWindowEnabled(child))return true;
                var cls=new StringBuilder(128);
                GetClassName(child,cls,cls.Capacity);
                if(cls.ToString().IndexOf("Button",
                     StringComparison.OrdinalIgnoreCase)<0)return true;
                string name=ReadChildCaption(child).Replace("&","").Trim();
                if(name.Equals("OK",StringComparison.OrdinalIgnoreCase)) {
                    ok=child;okCount++;
                }else if(name.Equals("Cancel",
                         StringComparison.OrdinalIgnoreCase))cancelCount++;
                return true;
            },IntPtr.Zero);
            if(okCount==1 && cancelCount==1) {
                IntPtr returned;
                clicked=SendMessageTimeoutRaw(ok,BM_CLICK,IntPtr.Zero,
                    IntPtr.Zero,2,1500,out returned)!=IntPtr.Zero;
                reason=clicked?"accepted temporary first-run configuration":
                    "configuration OK click failed";
            }else {
                reason="ambiguous Configuration dialog controls: ok="+
                    okCount+" cancel="+cancelCount;
            }
            return true;
        },IntPtr.Zero);
        return "TC_FIRST_RUN_CONFIGURATION dialogs="+dialogs+
               " accepted="+(dialogs==1&&clicked?1:0)+
               " result="+(dialogs>1?"ambiguous dialogs":reason);
    }
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
    // Keep desktop inspection bounded to the disposable CI TC process.
    public static IntPtr[] OwnedVisibleTopLevelHandles(uint targetPid) {
        var found=new List<IntPtr>();
        EnumWindows((hwnd,param)=>{
            uint owner=0;
            GetWindowThreadProcessId(hwnd,out owner);
            if(owner==targetPid && IsWindowVisible(hwnd))
                found.Add(hwnd);
            return true;
        },IntPtr.Zero);
        return found.ToArray();
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
    # Only the real TTOTAL_CMD class accepts WM_USER+50/51 commands.
    # Process.MainWindowHandle may instead be the shareware trial dialog;
    # posting cm_Copy/GoToDir there silently does nothing.
    for($i=0;$i -lt 50;$i++) {
        $h=[RealTcHostProbe]::FindOwnedMainWindow([uint32]$targetPid)
        if($h -ne [IntPtr]::Zero) { return $h }
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
# The trial reminder's instruction is owner-drawn; neither WM_GETTEXT nor
# UIAutomation exposes "Please press button Nr. N". Read exactly ONE
# screenshot with an OCR engine installed on CI (not shipped with ISO-WCX).
# Clicking a numeric button is authorized only after an explicit digit 1/2/3
# is recognized in the complete publisher instruction on the screenshot.
function Read-RequestedTrialDigit([int]$tcPid,[string]$caseDir) {
    $result=[ordered]@{
        digit=''; reason='not scanned'; instruction=''; bounds=@()
    }
    # A TC main window and the shareware dialog can share the same title.
    # Identify the dialog by its three enabled number buttons, not title
    # count. Refuse OCR if multiple plausible prompts are present.
    $bounds=@([RealTcHostProbe]::UniqueTrialDialogBounds(
        [uint32]$tcPid))
    $result.bounds=$bounds
    if($bounds.Count -ne 4) {
        $result.reason='No unique visible numbered Total Commander trial dialog'
        return $result
    }
    $exe=(Get-Command 'tesseract.exe' -ErrorAction SilentlyContinue)
    if($null -eq $exe) {
        $candidate='C:\Program Files\Tesseract-OCR\tesseract.exe'
        if(Test-Path -LiteralPath $candidate) {
            $engine=$candidate
        }else {
            $result.reason='OCR engine unavailable on CI'
            return $result
        }
    }else {
        $engine=$exe.Source
    }
    Save-RunnerScreenshot $caseDir
    $picture=Join-Path $caseDir 'totalcmd-screen.png'
    if(-not (Test-Path -LiteralPath $picture)) {
        $result.reason='Could not capture trial screenshot'
        return $result
    }
    $cropped=Join-Path $caseDir 'trial-dialog.png'
    try {
        Add-Type -AssemblyName System.Drawing
        Add-Type -AssemblyName System.Windows.Forms
        $screen=[System.Windows.Forms.Screen]::PrimaryScreen.Bounds
        $bitmap=[System.Drawing.Bitmap]::new($picture)
        try {
            $desired=[System.Drawing.Rectangle]::new(
                [int]$bounds[0]-$screen.Left,
                [int]$bounds[1]-$screen.Top,
                [int]$bounds[2],[int]$bounds[3])
            $limits=[System.Drawing.Rectangle]::new(
                0,0,$bitmap.Width,$bitmap.Height)
            $region=[System.Drawing.Rectangle]::Intersect(
                $desired,$limits)
            if($region.Width -lt 80 -or $region.Height -lt 45) {
                $result.reason='Trial dialog outside captured CI screen'
                return $result
            }
            $crop=$bitmap.Clone($region,$bitmap.PixelFormat)
            try {
                $crop.Save($cropped,
                    [System.Drawing.Imaging.ImageFormat]::Png)
            }finally { $crop.Dispose() }
        }finally { $bitmap.Dispose() }
        # Exactly one OCR read per case, limited to the publisher dialog.
        # Never infer the required number from the buttons themselves.
        $lines=@(& $engine $cropped 'stdout' '--psm' '6' '-l' 'eng' 2>$null)
        $recognized=[string]::Join(' ',$lines)
        $match=[regex]::Match($recognized,
            '(?i)\bPlease\s+press\s+button\s+Nr\.?\s*([123])\s+to\s+start\s+the\s+program\b')
        $result.instruction=$match.Value
        if($match.Success) {
            $result.digit=$match.Groups[1].Value
            $result.reason='Publisher instruction recognized in cropped CI screenshot'
        }else {
            $result.reason='Cropped OCR did not recognize explicit trial instruction'
        }
    }catch {
        $result.reason='Trial OCR unavailable: '+$_.Exception.Message
    }
    return $result
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
    # Avoid UIAutomation RootElement.FindAll(Descendants) over the whole
    # runner desktop: another process can block the accessibility walker.
    return @([RealTcHostProbe]::OwnedTopLevelWindows([uint32]$tcPid))
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
    for($attempt=0;$attempt -lt 30;$attempt++) {
        # Restrict UIA to HWNDs that belong to this specific TC process.
        $handles=@([RealTcHostProbe]::OwnedVisibleTopLevelHandles(
            [uint32]$tcPid))
        foreach($hwnd in $handles) {
            # A short-lived native child handle can disappear between Win32
            # enumeration and UIA.FromHandle. That is an ordinary race, not
            # a failure of ISO/WIM packing. Preserve the observation and
            # continue inspecting other windows instead of aborting F5.
            try {
                $dialog=[System.Windows.Automation.AutomationElement]::FromHandle(
                    $hwnd)
                if($null -eq $dialog) { continue }
                $title=[string]$dialog.Current.Name
            } catch {
                $answer.windows=@($answer.windows)+@(
                    'UIA_WINDOW_SKIPPED handle='+$hwnd+
                    ' reason='+$_.Exception.Message)
                continue
            }
            if(-not $title -or $dialog.Current.IsOffscreen) { continue }
            if($title -match '(?i)Function not supported|not supported|not implemented') {
                $answer.prompted=$true
                $answer.kind='host refused F5 before copy confirmation'
                $answer.windows=@($answer.windows)+@($title)
                return $answer
            }
            if($title -notmatch '(?i)copy|pack|копир|упаков|копирование') { continue }
            try {
                $items=$dialog.FindAll(
                    [System.Windows.Automation.TreeScope]::Descendants,
                    [System.Windows.Automation.Condition]::TrueCondition)
            } catch {
                $answer.windows=@($answer.windows)+@(
                    'UIA_CHILDREN_SKIPPED title='+$title+
                    ' reason='+$_.Exception.Message)
                continue
            }
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
# Keep final verification bounded even when another CI process holds an
# obsolete Windows image handle. A stuck Get-FileHash must never block the
# report after Total Commander has already completed both F5 operations.
function Get-BoundedFileHash([string]$file,[int]$timeoutSeconds=20) {
    $job=Start-Job -ScriptBlock {
        param($path)
        (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    } -ArgumentList @($file)
    try {
        if($null -eq (Wait-Job -Job $job -Timeout $timeoutSeconds)) {
            throw ('SHA256 verification exceeded '+$timeoutSeconds+
                ' seconds: '+$file)
        }
        if($job.State -ne 'Completed') {
            throw ('SHA256 verifier failed for '+$file+': '+$job.State)
        }
        $value=@(Receive-Job -Job $job -ErrorAction Stop)
        if($value.Count -ne 1 -or
           $value[0] -notmatch '^[0-9a-fA-F]{64}$') {
            throw ('Unexpected SHA256 result for '+$file)
        }
        return [string]$value[0]
    } finally {
        Remove-Job -Job $job -Force -ErrorAction SilentlyContinue
    }
}
function Test-Mask([int]$mask,[string]$source,[string]$templateImage,[string]$format='wim') {
    if($format -notin @('wim','iso')) { throw 'Unsupported TC F5 test format' }
    $suffix=if($format -eq 'wim') { '' } else { '-'+$format }
    Write-Host ("TC_F5_STAGE=START format="+$format+" mask="+$mask)
    $caseDir=Join-Path $work ("caps-"+$mask+$suffix)
    New-Item -ItemType Directory -Force -Path $caseDir | Out-Null
    $image=Join-Path $caseDir ("test."+$format)
    Copy-Item -LiteralPath $templateImage -Destination $image
    # Keep the INI OUTSIDE the archive-directory listing, otherwise
    # cm_GoToFirstFile could pick wincmd.ini rather than our only test.wim.
    $profileDir=Join-Path $work ("profile-"+$mask+$suffix)
    New-Item -ItemType Directory -Force -Path $profileDir | Out-Null
    $ini=Join-Path $profileDir 'wincmd.ini'
    $caseLocalApp=Join-Path $work ("local-app-data-"+$mask+$suffix)
    New-Item -ItemType Directory -Force -Path $caseLocalApp | Out-Null
    $env:LOCALAPPDATA=$caseLocalApp
    $script:logFile=Join-Path $caseLocalApp 'ISO-WCX/Logs/edit-errors.log'
    # 158 negative control uses a separate DLL and disposable INI. Ship only 159.
    $casePlugin=if($mask -eq 158) { $controlPlugin } else { $plugin }
    $contents=@"
[Configuration]
OnlyOnce=0
AutoInstallPlugins=0
# CI-only first-run configuration: avoid Total Commander's shareware
# 'Configuration > Layout' setup modal, observed in F5 evidence screenshots.
# This does NOT disable the publisher's registration/start reminder.
FirstTime=0
FirstTimeIconLib=0
FirstLang=0
[PackerPlugins]
$format=$mask,$casePlugin
"@
    [IO.File]::WriteAllText($ini,$contents,[Text.Encoding]::Unicode)
    $case=[ordered]@{
        format=$format; mask=$mask; opened_wcx=$false; source_selected=$false
        active=$null; left_type=$null; right_type=$null
        right_archive_name_length=$null
        f5_sent=$false; packfiles_callback=$false
        packfiles_success=$false; wim_contents_verified=$false
        iso_contents_verified=$false; image_contents_verified=$false; events=@()
        confirmation=$null; windows=@(); session=$null
        trial_acknowledged=$false; trial_dialog=@()
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
        $case.navigation+= 'CI_Dll='+$casePlugin
        Write-Host ("TC_F5_STAGE=PLUGIN_DLL mask="+$mask+
            " ini="+$case.observed_ini_binding)
        # The official shareware start dialog is NOT a WIM/pack error.
        # Read its displayed required number and acknowledge that exact
        # button; keep GUI/INI results inconclusive if it cannot be read.
        $case.trial_dialog=@()
        $trialCompleted=$false
        # Native text retrieval first, owner-drawn screenshot OCR once as
        # a strict fallback. Do not assume the requested number is always 1.
        $native=[RealTcHostProbe]::ClickRequestedTrialButton(
            [uint32]$tc.Id,'')
        $case.trial_dialog+= $native
        if($native -match '^NATIVE_TRIAL_CLICKED=1') {
            $trialCompleted=$true
        }else {
            $ocr=Read-RequestedTrialDigit $tc.Id $caseDir
            $case.trial_dialog+= (
                'TRIAL_OCR='+$ocr.reason+' requested_digit='+$ocr.digit+
                ' bounds='+(@($ocr.bounds) -join ','))
            if($ocr.digit -match '^[123]$') {
                $verified=[RealTcHostProbe]::ClickRequestedTrialButton(
                    [uint32]$tc.Id,[string]$ocr.digit)
                $case.trial_dialog+= $verified
                $trialCompleted=($verified -match '^NATIVE_TRIAL_CLICKED=1')
            }
            if(-not $trialCompleted) {
                # No desktop-wide UIAutomation fallback after a failed
                # verified native/OCR instruction: abort this CI case.
                $case.trial_dialog+=
                    'TRIAL_NOT_ACKNOWLEDGED=no verified native/OCR instruction'
            }
        }
        $case.trial_acknowledged=$trialCompleted
        Write-Host ("TC_F5_STAGE=TRIAL mask="+$mask+
            " acknowledged="+$trialCompleted)
        if($trialCompleted) {
            # A distinct, modal Configuration > Layout window was visible
            # in the previous real-TC evidence screenshots and blocked
            # cm_GoToDir. Acknowledge it only in this disposable CI host.
            $case.trial_dialog+=
                [RealTcHostProbe]::AcceptDisposableFirstRunConfiguration(
                    [uint32]$tc.Id)
        }
        if(-not $trialCompleted) {
            $case.session=Session-Snapshot $tc.Id
            $case.windows=$case.session.top_level_windows
            $case.events=Tail-Wcx
            $case.outcome='INCONCLUSIVE: publisher trial startup prompt not acknowledged; F5 NOT SENT'
            return $case
        }
        Start-Sleep -Seconds 1
        Write-Host ("TC_F5_STAGE=FIND_MAIN mask="+$mask)
        $window=Find-TcWindow $tc.Id
        if($window -eq [IntPtr]::Zero) {
            $case.session=Session-Snapshot $tc.Id
            $case.windows=$case.session.top_level_windows
            $case.events=Tail-Wcx
            $case.outcome='INCONCLUSIVE: TC window unavailable, trial_acknowledged='+$case.trial_acknowledged+'; F5 NOT SENT'
            return $case
        }
        # Only the disposable test image exists in the right filesystem
        # folder. All commands below are documented Total Commander IDs:
        # cm_FocusRight=4002, cm_GoToFirstFile=2050,
        # cm_GoToDir=2003, cm_GoToFirstEntry=2049,
        # cm_FocusLeft=4001. This is equivalent to Enter on test.wim
        # and Enter on its image1 directory, then Tab back to the source.
        Write-Host ("TC_F5_STAGE=INITIAL_WINDOWS mask="+$mask+
            " "+(@([RealTcHostProbe]::OwnedTopLevelWindows(
                [uint32]$tc.Id)) -join ' | '))
        $case.navigation+= '4002 cm_FocusRight'
        Command-Tc $window 4002
        Start-Sleep -Milliseconds 250
        $case.navigation+= '2050 cm_GoToFirstFile (test.'+$format+')'
        Command-Tc $window 2050
        Start-Sleep -Milliseconds 250
        $case.navigation+= '2003 cm_GoToDir (open WCX test.'+$format+')'
        Write-Host ("TC_F5_STAGE=OPEN_WIM mask="+$mask+
            " rightType="+(Query-Tc $window 1026))
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
            # Preserve the actual host window evidence when archive navigation
            # is blocked, rather than incorrectly attributing a modal TC
            # first-run configuration dialog to WCX OpenArchive/PackFiles.
            $case.session=Session-Snapshot $tc.Id
            $case.windows=$case.session.top_level_windows
            $case.events=Tail-Wcx
            $case.outcome='INCONCLUSIVE: cm_GoToDir did not open disposable '+$format+', trial_acknowledged='+$case.trial_acknowledged+'; F5 NOT SENT'
            return $case
        }
        if($format -eq 'wim') {
            # WIM has synthetic imageN roots. ISO opens directly at /.
            $case.navigation+= '2049 cm_GoToFirstEntry (image1)'
            Command-Tc $window 2049
            Start-Sleep -Milliseconds 250
            $case.navigation+= '2003 cm_GoToDir (enter image1)'
            Command-Tc $window 2003
            Start-Sleep -Milliseconds 750
        }
        $case.navigation+= '4001 cm_FocusLeft (return to source)'
        Command-Tc $window 4001
        Start-Sleep -Milliseconds 350
        # The earlier F5 CI screenshot showed a distinct TC modal
        # Configuration > Packer window even AFTER the WIM path opened.
        # It blocks ordinary F5, so diagnose and dismiss only this
        # exact verified modal in the disposable test process.
        $precopyConfig=[RealTcHostProbe]::AcceptDisposableFirstRunConfiguration(
            [uint32]$tc.Id)
        $case.navigation+= $precopyConfig
        Write-Host ("TC_F5_STAGE=PRECOPY_CONFIGURATION mask="+$mask+
            " "+$precopyConfig)
        if($precopyConfig -match 'accepted=1') {
            Start-Sleep -Milliseconds 350
        }
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
        if($format -eq 'wim' -and $case.right_path -and
           $case.right_path -notmatch '(?i)\\image1(?:\\|$)') {
            $case.events=Tail-Wcx
            $case.outcome='INCONCLUSIVE: WIM panel is not inside image1; F5 was NOT SENT'
            return $case
        }
        if($format -eq 'iso' -and $case.right_path -and
           $case.right_path -notmatch '(?i)\.iso(?:[\\/]|$)') {
            $case.events=Tail-Wcx
            $case.outcome='INCONCLUSIVE: right panel is not inside the disposable ISO; F5 was NOT SENT'
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
        # Capture the *real* file panels before pressing F5: earlier
        # failed runs showed Configuration > Packer only AFTER copy.
        Save-RunnerScreenshot $caseDir
        $beforeF5=Join-Path $caseDir 'totalcmd-screen.png'
        if(Test-Path -LiteralPath $beforeF5) {
            Copy-Item -LiteralPath $beforeF5 -Destination (
                Join-Path $caseDir 'before-f5.png') -Force
        }
        $case.navigation+= 'TC_F5_STAGE=PRE_F5_WINDOWS '+(
            @([RealTcHostProbe]::OwnedTopLevelWindows(
                [uint32]$tc.Id)) -join ' | ')
        Write-Host ("TC_F5_STAGE=SEND_F5 mask="+$mask+
            " rightType="+$case.right_type+" selected="+$case.source_selected)
        Command-Tc $window 905
        $case.f5_sent=$true
        Start-Sleep -Milliseconds 500
        $postF5Windows=@([RealTcHostProbe]::OwnedTopLevelWindows(
            [uint32]$tc.Id))
        $case.navigation+= 'TC_F5_STAGE=POST_F5_WINDOWS '+(
            $postF5Windows -join ' | ')
        Write-Host ("TC_F5_STAGE=POST_F5_WINDOWS mask="+$mask+
            " "+($postF5Windows -join ' | '))
        # A fresh test profile can open the Packer Configuration modal
        # the FIRST time F5 is invoked. Close only that precisely verified
        # modal and retry cm_Copy ONCE. Never do so on a user installation.
        $afterCopyConfiguration=
            [RealTcHostProbe]::AcceptDisposableFirstRunConfiguration(
                [uint32]$tc.Id)
        $case.navigation+= 'TC_F5_AFTER_COPY_CONFIGURATION '+
            $afterCopyConfiguration
        Write-Host ("TC_F5_STAGE=POST_F5_CONFIGURATION mask="+$mask+
            " "+$afterCopyConfiguration)
        if($afterCopyConfiguration -match 'accepted=1') {
            Start-Sleep -Milliseconds 400
            $case.navigation+= 'cm_Copy retried ONCE after CI-only modal'
            Command-Tc $window 905
            Start-Sleep -Milliseconds 400
        }
        # Native F5/Pack dialog is a VCL Win32 window. UIAutomation can
        # fail to expose its OK button even when the dialog is visible.
        # Confirm ONLY if the dialog itself contains this disposable WIM
        # path and a unique OK/Cancel pair. This is not a user-side action.
        $nativePack=[RealTcHostProbe]::ConfirmDisposablePackFiles(
            [uint32]$tc.Id,$image)
        $case.navigation+= $nativePack
        Write-Host ("TC_F5_STAGE=NATIVE_PACK mask="+$mask+" "+$nativePack)
        if($nativePack -match 'clicked=1') {
            $confirmation=[ordered]@{
                prompted=$true; confirmed=$true
                kind='native disposable WIM Pack files dialog accepted'
                windows=@($nativePack); visible_labels=@()
            }
        } else {
            $hostError=[RealTcHostProbe]::DetectHostUnsupported(
                [uint32]$tc.Id)
            $case.navigation+= $hostError
            Write-Host ("TC_F5_STAGE=HOST_ERROR mask="+$mask+
                " "+$hostError)
            if($hostError -match '=1$') {
                $confirmation=[ordered]@{
                    prompted=$true; confirmed=$false
                    kind='host reports Function not supported before WCX callback'
                    windows=@($hostError); visible_labels=@()
                }
            } else {
                $confirmation=Confirm-Only-DisposableCopy $tc.Id $image
            }
        }
        $case.confirmation=$confirmation
        Write-Host ("TC_F5_STAGE=CONFIRM mask="+$mask+
            " kind="+$confirmation.kind)
        if($confirmation.windows.Count -gt 0) {
            Write-Host ("TC_F5_WINDOW_OBSERVATIONS mask="+$mask+
                " "+(@($confirmation.windows) -join ' | '))
        }
        Start-Sleep -Seconds 5
        $case.events=Tail-Wcx
        $case.windows=Window-Names $tc.Id
        $case.packfiles_callback=@($case.events |
            Where-Object { $_ -match 'PackFiles(W| ANSI) entered' }).Count -gt 0
        $case.packfiles_success=@($case.events |
            Where-Object { $_ -match 'PackFilesW finished' -and
                           $_ -match 'return=0' }).Count -gt 0
        if($case.packfiles_success) {
            if($format -eq 'wim') {
                $verify=Join-Path $caseDir 'verify-applied'
                New-Item -ItemType Directory -Path $verify -Force | Out-Null
                & "$env:WINDIR\System32\dism.exe" /English /Apply-Image "/ImageFile:$image" /Index:1 "/ApplyDir:$verify" | Out-Null
                $actual=Join-Path $verify 'add.txt'
                $baseline=Join-Path $verify 'original.txt'
                $case.wim_contents_verified=($LASTEXITCODE -eq 0 -and
                    (Test-Path -LiteralPath $actual -PathType Leaf) -and
                    (Test-Path -LiteralPath $baseline -PathType Leaf) -and
                    ((Get-Content -LiteralPath $actual -Raw).Trim() -eq
                      'Real TC F5 input file'))
                $case.image_contents_verified=$case.wim_contents_verified
            } else {
                # Check ISO via an independent ISO9660 parser and path tables.
                $python=(Get-Command python.exe -ErrorAction Stop).Source
                $verifyOutput=@(& $python 'tools/iso_wcx_ci_iso_f5.py' verify $image)
                $verifyExitCode=$LASTEXITCODE
                Write-Host ('TC_F5_ISO_READBACK '+($verifyOutput -join ' | '))
                # Never let the Python process' ordinary stdout leak into
                # Test-Mask function output: the only return object must be
                # the structured per-case result, not a stray status string.
                $case.iso_contents_verified=($verifyExitCode -eq 0)
                $case.image_contents_verified=$case.iso_contents_verified
            }
            Write-Host ("TC_F5_STAGE=VERIFY_IMAGE format="+$format+
                " mask="+$mask+" success="+$case.image_contents_verified)
        }
        Write-Host ("TC_F5_STAGE=AFTER_F5 mask="+$mask+
            " callback="+$case.packfiles_callback+
            " outcome_hint="+$confirmation.kind)
        $case.outcome=if($case.packfiles_success -and
                            $case.image_contents_verified) {
            'PASS: real TC F5 wrote '+$format+' and contents verified'
        }elseif($case.packfiles_callback) {
            'FAIL: WCX callback invoked but '+$format+' contents not verified'
        }elseif($confirmation.prompted -and -not $confirmation.confirmed) {
            'INCONCLUSIVE: F5 raised a modal copy/error dialog without a confirmed WCX callback'
        }elseif(-not $confirmation.prompted) {
            'INCONCLUSIVE: cm_Copy posted but the host copy dialog was not observed'
        }else {
            'FAIL: host did not invoke PackFiles after disposable copy confirmation'
        }
        Write-Host ("TC_F5_STAGE=CASE_COMPLETE mask="+$mask+
            " result="+$case.outcome)
        return $case
    } catch {
        # Preserve partial F5 state, especially when an unstable GUI
        # accessibility provider throws after cm_Copy was already sent.
        $case.outcome='INCONCLUSIVE: TC host probe error: '+$_.Exception.Message
        $case.events=Tail-Wcx
        if($tc) {
            $case.windows=Window-Names $tc.Id
        }
        Write-Warning ("TC_F5_STAGE=EXCEPTION mask="+$mask+
            " f5_sent="+$case.f5_sent+
            " reason="+$_.Exception.Message)
        return $case
    } finally {
        Write-Host ("TC_F5_STAGE=CLEANUP_BEGIN mask="+$mask+
            " format="+$format+" verified="+$case.image_contents_verified)
        if($tc) {
            # On successful WIM verification the before-F5 screenshot is
            # already preserved. CopyFromScreen in a newly disposed VCL
            # desktop can hang on a headless Windows runner; skip this
            # best-effort postmortem screenshot on confirmed success.
            if(-not $case.image_contents_verified) {
                try { Save-RunnerScreenshot $caseDir } catch {}
            }
            Write-Host ("TC_F5_STAGE=STOP_TC mask="+$mask)
            try {
                $tc.Refresh()
                if(-not $tc.HasExited) { Stop-Process -Id $tc.Id -Force }
            }catch{}
        }
        Write-Host ("TC_F5_STAGE=CLEANUP_DONE mask="+$mask)
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
    $sha=Get-BoundedFileHash $templateWim
    # Evaluate both real TC modes on a one-index disposable WIM.
    foreach($mask in @(158,159)) {
        $report.cases+=Test-Mask $mask $source $templateWim 'wim'
    }
    Write-Host 'TC_F5_STAGE=WIM_CASES_RETURNED'
    # Independently generated plain ISO9660 with existing HELLO.TXT.
    # No user image is ever opened or modified by these GitHub CI cases.
    $templateIso=Join-Path $work 'original-test.iso'
    $python=(Get-Command python.exe -ErrorAction Stop).Source
    & $python 'tools/iso_wcx_ci_iso_f5.py' create $templateIso
    if($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $templateIso)) {
        throw 'Could not generate disposable ISO9660 F5 fixture'
    }
    $isoSha=Get-BoundedFileHash $templateIso
    # The production GetPackerCaps=159 binary must accept normal F5 to
    # the ISO root; replacement/delete are covered in ISO writer tests.
    $report.cases+=Test-Mask 159 $source $templateIso 'iso'
    Write-Host 'TC_F5_STAGE=CASES_RETURNED'
    Write-Host 'TC_F5_STAGE=VERIFY_TEMPLATES_BEGIN'
    $report.template_wim_unchanged=
        $sha -eq (Get-BoundedFileHash $templateWim)
    $report.template_iso_unchanged=
        $isoSha -eq (Get-BoundedFileHash $templateIso)
    Write-Host ('TC_F5_STAGE=VERIFY_TEMPLATES_END wim='+
        $report.template_wim_unchanged+' iso='+$report.template_iso_unchanged)
    if(-not $report.template_wim_unchanged -or
       -not $report.template_iso_unchanged) {
        throw 'A disposable ISO/WIM template unexpectedly changed'
    }
    $attempted=@($report.cases | Where-Object {$_.f5_sent})
    Write-Host ('TC_F5_STAGE=CHECK_OUTCOMES attempted='+$attempted.Count)
    if($attempted.Count -lt 2) {
        throw 'INCONCLUSIVE HOST: F5 was not sent to BOTH WIM and ISO panels.'
    }
    foreach($format in @('wim','iso')) {
        if(@($report.cases | Where-Object {
               $_.format -eq $format -and $_.mask -eq 159 -and
               $_.packfiles_success -and $_.image_contents_verified
           }).Count -ne 1) {
            throw ('Real TC F5 did not produce independently verified '+$format+
                   ' output from shipping WCX 159.')
        }
    }
    Write-Host 'TC_F5_STAGE=CHECK_OUTCOMES_PASS'
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
    Write-Host 'TC_F5_STAGE=REPORT_SERIALIZE_BEGIN'
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $path -Encoding UTF8
    Write-Host 'TC_F5_STAGE=REPORT_SERIALIZE_END'
    Write-Host ('REAL_TC_F5_REPORT=' + $path)
    Get-Content -LiteralPath $path
}
if($failure) { exit 1 }
