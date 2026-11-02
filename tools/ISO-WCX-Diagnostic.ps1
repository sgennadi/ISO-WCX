# ISO-WCX read-only field diagnostic; Windows PowerShell 5.1 / PowerShell 7.
# No TC config edits, image writes, mounting, unmounting or workspace deletion.
# Run: powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\ISO-WCX-Diagnostic.ps1 -WimPath "D:\sources\boot.wim" -CheckDism
[CmdletBinding()]
param(
    [string]$WimPath,
    [string]$IniPath,
    [string]$WcxPath,
    [switch]$CheckDism,
    [string]$ReportPath
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$script:Lines = New-Object 'System.Collections.Generic.List[string]'

function Say([string]$Message) {
    $script:Lines.Add($Message)
    Write-Host $Message
}
function Find-TcIni {
    if ($IniPath) { return (Resolve-Path -LiteralPath $IniPath).ProviderPath }
    $candidates = New-Object 'System.Collections.Generic.List[string]'
    if ($env:COMMANDER_INI) { $candidates.Add($env:COMMANDER_INI) }
    foreach ($key in @('HKCU:\Software\Ghisler\Total Commander',
                       'HKLM:\Software\Ghisler\Total Commander',
                       'HKLM:\Software\WOW6432Node\Ghisler\Total Commander')) {
        try {
            $item = Get-ItemProperty -Path $key -Name IniFileName -ErrorAction Stop
            if ($item.IniFileName) { $candidates.Add([string]$item.IniFileName) }
        } catch {}
    }
    if ($env:APPDATA) { $candidates.Add((Join-Path $env:APPDATA 'GHISLER\wincmd.ini')) }
    if ($env:COMMANDER_PATH) { $candidates.Add((Join-Path $env:COMMANDER_PATH 'wincmd.ini')) }
    foreach ($candidate in $candidates) {
        $expanded = [Environment]::ExpandEnvironmentVariables($candidate.Trim('"'))
        if (Test-Path -LiteralPath $expanded -PathType Leaf) {
            return (Resolve-Path -LiteralPath $expanded).ProviderPath
        }
    }
    return $null
}
function Check-TcProcesses {
    Say '--- Запущенные экземпляры Total Commander ---'
    $processes = @(Get-Process -Name TOTALCMD,TOTALCMD64 -ErrorAction SilentlyContinue)
    if ($processes.Count -eq 0) {
        Say 'Total Commander закрыт. После установки откройте TC заново.'
        return
    }
    foreach ($process in $processes) {
        Say ('PID ' + $process.Id + ': ' + $process.ProcessName)
        $found = $false
        try {
            foreach ($module in @($process.Modules)) {
                if ($module.ModuleName -match '(?i)^ISO-WCX\.wcx(64)?$') {
                    Say ('Загруженный WCX: ' + $module.FileName)
                    $found = $true
                }
            }
            if (-not $found) { Say 'ISO-WCX не загружен сейчас (это нормально после закрытия панели архива).' }
        } catch {
            Say ('Нет доступа к списку DLL: ' + $_.Exception.Message)
        }
    }
    Say 'Внимание: старые DLL и флаги F5 могут оставаться в памяти до полного закрытия всех TC.'
}
function Check-Ini {
    Say '--- WCX-привязки Total Commander (только чтение) ---'
    $file = Find-TcIni
    if (-not $file) {
        Say 'Активный wincmd.ini не найден. Укажите его через -IniPath.'
        return
    }
    Say ('INI: ' + $file)
    $section = ''
    $found = 0
    $reader = [IO.File]::OpenText($file)
    try {
        while ($null -ne ($line = $reader.ReadLine())) {
            if ($line -match '^\s*\[([^\]]+)\]\s*$') {
                $section = $Matches[1].Trim()
                continue
            }
            if ($section -notin @('PackerPlugins','PackerPlugins64')) { continue }
            if ($line -match '^\s*(wim|iso|udf|esd|swm)\s*=\s*(.*)$') {
                $extension = $Matches[1].ToLowerInvariant()
                $value = $Matches[2].Trim()
                Say ('[' + $section + '] ' + $extension + '=' + $value)
                $found++
                if ($section -eq 'PackerPlugins' -and
                    $value -match '^\s*(\d+)\s*,\s*(.+)$') {
                    $mask = [int]$Matches[1]
                    $target = $Matches[2].Trim().Trim('"')
                    if ($target -match '(?i)ISO-WCX\.wcx(64)?$' -and
                        ($mask -band 2) -eq 0) {
                        Say ('ПРОБЛЕМА: ' + $extension + ' не имеет PK_CAPS_MODIFY; F5 может быть отключён.')
                    } elseif ($target -match '(?i)ISO-WCX\.wcx(64)?$' -and
                              $mask -ne 158) {
                        Say ('ПРОБЛЕМА: ' + $extension + ' использует флаги ' + $mask + ' вместо 158.')
                    }
                }
            }
        }
    } finally { $reader.Dispose() }
    if ($found -eq 0) {
        Say 'Прямых привязок не найдено. Проверьте RedirectSection и настройки плагинов TC.'
    }
}
function Check-Wcx64Cache {
    Say '--- Кэш WCX Total Commander x64 (только чтение) ---'
    $ini = Find-TcIni
    if (-not $ini) { Say 'wincmd.ini не найден'; return }
    $section = ''
    $exts = $null
    $checksum = $false
    $redirect = $false
    $reader = [IO.File]::OpenText($ini)
    try {
        while ($null -ne ($line = $reader.ReadLine())) {
            if ($line -match '^\s*\[([^\]]+)\]\s*$') {
                $section = $Matches[1].Trim()
                continue
            }
            if ($section -eq 'RedirectSection' -and
                $line -match '^\s*PackerPlugins(64)?\s*=') { $redirect = $true }
            if ($section -ne 'PackerPlugins64') { continue }
            if ($line -match '^\s*exts\s*=\s*(.*)$') { $exts = $Matches[1].Trim() }
            if ($line -match '^\s*\$checksum\$\s*=') { $checksum = $true }
        }
    } finally { $reader.Dispose() }
    if ($redirect) {
        Say 'RedirectSection перенаправляет WCX-привязки: проверьте другой INI.'
        return
    }
    if ($checksum) {
        Say 'Кэш PackerPlugins64: контрольная сумма присутствует.'
    } else {
        Say 'Кэш PackerPlugins64: контрольной суммы нет (TC может перестроить кэш).'
    }
    if ($null -eq $exts) {
        Say 'PackerPlugins64 exts отсутствует; TC может перестроить его при запуске.'
        return
    }
    $extensions = @($exts -split '\*')
    foreach ($extension in @('wim','iso','udf')) {
        if ($extensions -contains $extension) {
            Say ('x64 кэш: ' + $extension + ' присутствует')
        } else {
            Say ('ВНИМАНИЕ: ' + $extension +
                ' отсутствует в PackerPlugins64 exts — возможна устаревшая привязка.')
            Say ('WCX64_CACHE_MISSING=' + $extension)
        }
    }
    Say 'Наличие расширения не доказывает, что текущий процесс TC использует новые флаги F5.'
}
function Check-AssociationSummary {
    Say '--- Summary of 23 supported image associations (read-only) ---'
    $ini = Find-TcIni
    if (-not $ini) { Say 'Active TC INI unavailable'; return }
    $all = @('iso','udf','cue','ccd','nrg','mds','mdx',
             'cso','zso','isz','daa','img','raw','vhd','vhdx',
             'wim','esd','swm','ima','vfd','flp','bif','imz')
    $known = @{}
    $section = ''
    $reader = [IO.File]::OpenText($ini)
    try {
        while ($null -ne ($line = $reader.ReadLine())) {
            if ($line -match '^\s*\[([^\]]+)\]\s*$') {
                $section = $Matches[1].Trim()
                continue
            }
            if ($section -ne 'PackerPlugins') { continue }
            if ($line -match '^\s*([^=;\s]+)\s*=\s*(.*)$') {
                $extension = $Matches[1].ToLowerInvariant()
                $setting = $Matches[2].Trim()
                if ($all -contains $extension) {
                    if (-not $known.ContainsKey($extension)) {
                        $known[$extension] = @()
                    }
                    $known[$extension] = @($known[$extension]) + @($setting)
                }
            }
        }
    } finally { $reader.Dispose() }
    $owned158=0
    $ownedOther=0
    $missing=0
    $foreign=0
    $duplicates=0
    foreach ($extension in $all) {
        if (-not $known.ContainsKey($extension)) {
            $missing++
            continue
        }
        $entries = @($known[$extension])
        if ($entries.Count -gt 1) { $duplicates += $entries.Count - 1 }
        foreach ($entry in $entries) {
            $plugin = ($entry -replace '^\s*\d+\s*,\s*','').Trim('"')
            if ($plugin -match '(?i)(?:^|[\\/])ISO-WCX\.wcx(64)?$') {
                if ($entry -match '^\s*158\s*,') {
                    $owned158++
                } else {
                    $ownedOther++
                    Say ('WCX_ASSOC_BAD_MASK=' + $extension + ' value=' + $entry)
                }
            } else {
                $foreign++
            }
        }
    }
    Say ('WCX_ASSOC_SUMMARY owned_158=' + $owned158 +
         ' owned_other=' + $ownedOther + ' missing=' + $missing +
         ' foreign=' + $foreign + ' duplicates=' + $duplicates)
    if ($missing -eq 0 -and $ownedOther -eq 0 -and $duplicates -eq 0) {
        Say 'The second association prompt may correctly be skipped; all image formats have an assigned handler.'
    }
}
function Get-TcF5Context {
    param(
        [string]$Active,
        [string]$LeftType,
        [string]$RightType,
        [long]$LeftFiles,
        [long]$RightFiles,
        [long]$LeftDirs,
        [long]$RightDirs
    )
    if ($Active -notin @('left','right')) {
        return [pscustomobject]@{ IsValid=$false; Source='unknown'; Destination='unknown'; Reason='No active file panel' }
    }
    $selected = if ($Active -eq 'left') { $LeftFiles+$LeftDirs } else { $RightFiles+$RightDirs }
    $sourceType = if ($Active -eq 'left') { $LeftType } else { $RightType }
    $targetType = if ($Active -eq 'left') { $RightType } else { $LeftType }
    $destination = if ($Active -eq 'left') { 'right' } else { 'left' }
    $ok = ($sourceType -eq 'filesystem' -and $targetType -eq 'WCX-plugin' -and $selected -gt 0)
    $reason = if ($selected -le 0) { 'No selected source file or directory' }
              elseif ($sourceType -ne 'filesystem') { 'Source is not a filesystem panel' }
              elseif ($targetType -ne 'WCX-plugin') { 'Destination is not a WCX plugin panel' }
              else { 'Filesystem to WCX with selected source' }
    return [pscustomobject]@{
        IsValid=$ok; Source=$Active; Destination=$destination; Reason=$reason
    }
}
function Check-TcLivePanels {
    Say '--- Total Commander live F5 panel state (read-only) ---'
    $running = @(Get-Process -Name TOTALCMD,TOTALCMD64 -ErrorAction SilentlyContinue)
    if ($running.Count -eq 0) {
        Say 'TC_PANEL_QUERY: no running Total Commander process'
        return
    }
    # The 11.57+ public WM_USER+50 query is entirely read-only.
    # 1000=active panel, 1019/20=selected files, 1023/24=archive
    # filename length, 1025/26=panel type, >=10000 = WCX plugin.
    # SendMessageTimeout prevents hanging when a modal dialog is open.
    if (-not ('IsoWcxTcPanelQueryV2' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class IsoWcxTcPanelQueryV2 {
    [DllImport("user32.dll", EntryPoint="SendMessageTimeoutW", SetLastError=true)]
    public static extern IntPtr SendMessageTimeout(
        IntPtr window, uint message, IntPtr wParam, IntPtr lParam,
        uint flags, uint timeout, out IntPtr result);
    [DllImport("user32.dll", EntryPoint="GetWindowTextW", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern int GetWindowText(
        IntPtr window, StringBuilder buffer, int maxCount);
}
'@ -ErrorAction Stop
    }
    foreach ($proc in $running) {
        try {
            $window=$proc.MainWindowHandle
            if ($window -eq [IntPtr]::Zero) {
                Say ('TC_PANEL_QUERY pid=' + $proc.Id + ' no visible main window')
                continue
            }
            $values=@{}
            foreach ($code in @(1000,1017,1018,1019,1020,1023,1024,1025,1026)) {
                $result=[IntPtr]::Zero
                $ok=[IsoWcxTcPanelQueryV2]::SendMessageTimeout(
                    $window,[uint32]1074,[IntPtr]$code,[IntPtr]::Zero,
                    [uint32]2,[uint32]1200,[ref]$result)
                if ($ok -eq [IntPtr]::Zero) {
                    $values[$code]=$null
                } else {
                    $values[$code]=$result.ToInt64()
                }
            }
            if ($null -eq $values[1000] -or $null -eq $values[1026]) {
                Say ('TC_PANEL_QUERY pid=' + $proc.Id +
                     ' unavailable (modal dialog, old TC, or Windows UIPI permissions)')
                continue
            }
            $active=$(if ($values[1000] -eq 1) {'left'} elseif ($values[1000] -eq 2) {'right'} else {'unknown'})
            $leftType=$(if ($values[1025] -eq 0) {'filesystem'} elseif ($values[1025] -ge 10000) {'WCX-plugin'} else {'other'})
            $rightType=$(if ($values[1026] -eq 0) {'filesystem'} elseif ($values[1026] -ge 10000) {'WCX-plugin'} else {'other'})
            Say ('TC_PANEL_SNAPSHOT pid=' + $proc.Id +
                 ' active=' + $active + ' left_type=' + $leftType +
                 ' right_type=' + $rightType +
                 ' left_selected_files=' + $values[1019] +
                 ' right_selected_files=' + $values[1020] +
                 ' left_selected_dirs=' + $values[1017] +
                 ' right_selected_dirs=' + $values[1018] +
                 ' right_archive_name_length=' + $values[1024])
            $context=Get-TcF5Context -Active $active -LeftType $leftType -RightType $rightType -LeftFiles $values[1019] -RightFiles $values[1020] -LeftDirs $values[1017] -RightDirs $values[1018]
            if (-not $context.IsValid) {
                Say ('TC_F5_CONTEXT_WARNING: ' + $context.Reason +
                     '; source=' + $context.Source + ' destination=' + $context.Destination)
            } else {
                Say ('TC_F5_CONTEXT: valid filesystem-to-WCX routing; source=' +
                     $context.Source + ' destination=' + $context.Destination)
            }
            # Try the public path-label handles 9=left, 10=right.
            # GetWindowTextW reads a caption without altering TC. If it
            # cannot retrieve a custom-drawn label, report unknown.
            foreach ($side in @('left','right')) {
                $code=if ($side -eq 'left') { 9 } else { 10 }
                $handle=[IntPtr]::Zero
                $valid=[IsoWcxTcPanelQueryV2]::SendMessageTimeout(
                    $window,[uint32]1074,[IntPtr]$code,[IntPtr]::Zero,
                    [uint32]2,[uint32]1200,[ref]$handle)
                $pathText=''
                if ($valid -ne [IntPtr]::Zero -and
                    $handle -ne [IntPtr]::Zero) {
                    $buffer=New-Object System.Text.StringBuilder(8192)
                    $characters=[IsoWcxTcPanelQueryV2]::GetWindowText(
                        $handle,$buffer,$buffer.Capacity)
                    if ($characters -gt 0) {
                        $pathText=$buffer.ToString().Replace([char]13,' ').Replace([char]10,' ')
                    }
                }
                if ($pathText) {
                    Say ('TC_PANEL_PATH side=' + $side + ' text=' + $pathText)
                } else {
                    Say ('TC_PANEL_PATH side=' + $side + ' unavailable; use visible TC path bar')
                }
                if ($context.IsValid -and $context.Destination -eq $side) {
                    if (-not $pathText) {
                        Say 'TC_WIM_DESTINATION_DEPTH=unknown; inspect the archive path visually.'
                    } elseif ($pathText -match '(?i)\.wim[\\/]image[1-9][0-9]*([\\/]|$)') {
                        Say 'TC_WIM_DESTINATION_DEPTH=inside-image-index'
                    } elseif ($pathText -match '(?i)\.wim([\\/]|$)') {
                        Say 'TC_WIM_DESTINATION_DEPTH=synthetic-root; enter image1 before F5.'
                    } else {
                        Say 'TC_WIM_DESTINATION_DEPTH=unrecognized'
                    }
                }
            }
            Say 'WIM F5 must target boot.wim\image1\ or deeper, regardless of panel side.'
        } catch {
            Say ('TC_PANEL_QUERY pid=' + $proc.Id + ': ' + $_.Exception.Message)
        }
    }
}
function Check-TcF5CommandConfig {
    Say '--- F5 host command configuration (read-only) ---'
    $ini=Find-TcIni
    if (-not $ini) {
        Say 'TC_F5_BINDING=unknown (no readable Total Commander INI)'
        return
    }
    $section=''
    $binding=$null
    $redirect=$null
    $copyMode=$null
    # Only the plain F5 key is relevant. Do not modify any shortcuts.
    $reader=[IO.File]::OpenText($ini)
    try {
        while ($null -ne ($line=$reader.ReadLine())) {
            if ($line -match '^\s*\[([^\]]+)\]\s*$') {
                $section=$Matches[1].Trim()
                continue
            }
            if ($line -match '^\s*[;#]') { continue }
            if ($section -eq 'Shortcuts') {
                if ($line -match '^\s*RedirectSection\s*=\s*(.*)$') {
                    $redirect=$Matches[1].Trim()
                } elseif ($line -match '^\s*F5\s*=\s*(.*)$') {
                    $binding=$Matches[1].Trim()
                }
            } elseif ($section -eq 'Configuration' -and
                      $line -match '^\s*AlwaysCopyInBackground\s*=\s*(.*)$') {
                $copyMode=$Matches[1].Trim()
            }
        }
    } finally { $reader.Dispose() }
    if ($redirect) {
        Say ('TC_F5_BINDING=unknown (Shortcuts RedirectSection=' + $redirect + ')')
        Say 'TC_F5_SHORTCUT_WARNING: F5 may be remapped in redirected INI.'
    } elseif ($null -eq $binding) {
        Say 'TC_F5_BINDING=default cm_Copy'
    } else {
        Say ('TC_F5_BINDING=custom ' + $binding)
        if ($binding -notmatch '^(?i:cm_Copy)(?:\s|$)' -and $binding -ne '905') {
            Say 'TC_F5_SHORTCUT_WARNING: F5 is not mapped to cm_Copy (905).'
        }
    }
    if ($null -eq $copyMode -or $copyMode -eq '') {
        $copyMode='0 (default)'
    }
    Say ('TC_F5_COPY_BACKGROUND=' + $copyMode)
    Say 'TC_F5_HOST_NOTE: static settings cannot prove that Total Commander called PackFilesW.'
}
function Check-WimFile {
    Say '--- Заголовок WIM и временные папки ---'
    if (-not $WimPath) {
        Say 'Для проверки image1/image2 укажите -WimPath.'
        return
    }
    $resolved = (Resolve-Path -LiteralPath $WimPath).ProviderPath
    $file = Get-Item -LiteralPath $resolved -Force
    if ($file.PSIsContainer -or
        ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw 'Ожидался обычный файл WIM, не ссылка и не папка.'
    }
    Say ('WIM: ' + $resolved)
    Say ('Размер: ' + $file.Length + ' байт, изменение UTC: ' + $file.LastWriteTimeUtc.ToString('o'))
    $stream = [IO.File]::Open(
        $resolved,[IO.FileMode]::Open,[IO.FileAccess]::Read,
        ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    try {
        $header = New-Object byte[] 208
        if ($stream.Read($header,0,$header.Length) -lt $header.Length -or
            [Text.Encoding]::ASCII.GetString($header,0,5) -ne 'MSWIM') {
            throw 'WIM заголовок повреждён или слишком короткий.'
        }
        $count = [BitConverter]::ToUInt32($header,44)
        $part = [BitConverter]::ToUInt16($header,40)
        $totalParts = [BitConverter]::ToUInt16($header,42)
        $bootIndex = [BitConverter]::ToUInt32($header,120)
        Say ('Индексов в заголовке: ' + $count + '; часть: ' + $part + '/' +
             $totalParts + '; загрузочный индекс: ' + $bootIndex)
        if ($count -eq 2) {
            Say 'В корне образа TC ожидаются image1 И image2.'
        } elseif ($count -eq 1) {
            Say 'В самом файле WIM указан только один индекс.'
        } else {
            Say 'Сверьте количество индексов с выводом DISM /Get-WimInfo.'
        }
    } finally { $stream.Dispose() }

    $parent = Split-Path -Parent $resolved
    $workspaces = @(Get-ChildItem -LiteralPath $parent -Force -Directory |
        Where-Object { $_.Name -match '^\.iso-wcx-edit-[0-9]+-[0-9]+$' })
    Say ('Временных папок ISO-WCX рядом с WIM: ' + $workspaces.Count)
    foreach ($workspace in $workspaces) {
        Say ('Временная папка: ' + $workspace.FullName)
        if (($workspace.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            Say 'ОПАСНО: сама временная папка является reparse point. Не удалять.'
            continue
        }
        foreach ($name in @('mounted','original-archive.backup',
                            'original-archive.backup-recovery',
                            'WIM-MOUNT-RECOVERY.txt','expanded')) {
            $item = Join-Path $workspace.FullName $name
            if (Test-Path -LiteralPath $item) {
                Say ('Обнаружен: ' + $name)
                if ($name -eq 'mounted') {
                    Say 'ОПАСНО: активный WIMGAPI mount нельзя исключить одной командой DISM.'
                }
            }
        }
        Say 'Ничего не удаляйте до проверки нативного WIMGAPI mount и открытых backup handles.'
    }
    if ($CheckDism) {
        Say '--- DISM /Get-WimInfo (без изменений образа) ---'
        $dism = Join-Path $env:WINDIR 'System32\dism.exe'
        if (Test-Path -LiteralPath $dism) {
            & $dism '/English' '/Get-WimInfo' ('/WimFile:' + $resolved) 2>&1 |
                ForEach-Object { Say ([string]$_) }
            Say ('DISM код завершения: ' + $LASTEXITCODE)
        } else {
            Say 'DISM не найден.'
        }
    }
}
Say ('ISO-WCX диагностика: ' + (Get-Date).ToString('yyyy-MM-dd HH:mm:ss zzz'))
Say 'Только чтение: никаких изменений TC, WIM, монтировок и временных папок.'
try { Check-TcProcesses } catch { Say ('TC: ' + $_.Exception.Message) }
try { Check-TcLivePanels } catch { Say ('TC panels: ' + $_.Exception.Message) }
try { Check-Ini } catch { Say ('INI: ' + $_.Exception.Message) }
try { Check-AssociationSummary } catch { Say ('Association summary: ' + $_.Exception.Message) }
try { Check-TcF5CommandConfig } catch { Say ('F5 settings: ' + $_.Exception.Message) }
try { Check-Wcx64Cache } catch { Say ('Кэш x64: ' + $_.Exception.Message) }
try { Check-WimFile } catch { Say ('WIM: ' + $_.Exception.Message) }

function Check-Installed-Wcx {
    Say '--- Установленные модули ISO-WCX ---'
    $ini = Find-TcIni
    if (-not $ini) { Say 'Не удалось определить wincmd.ini.'; return }
    $match = Select-String -LiteralPath $ini -Pattern '(?i)^\s*(wim|iso)\s*=\s*(?:\d+\s*,\s*)?(.+ISO-WCX\.wcx)\s*$' -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (-not $match) { Say 'Не найдена строка с ISO-WCX.wcx в INI.'; return }
    $full = [Environment]::ExpandEnvironmentVariables(
        $match.Matches[0].Groups[2].Value.Trim().Trim('"'))
    foreach ($candidate in @($full,[IO.Path]::ChangeExtension($full,'wcx64'))) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            $item = Get-Item -LiteralPath $candidate
            Say ('Установлен: ' + $item.FullName +
                 ' | размер=' + $item.Length + ' | UTC=' +
                 $item.LastWriteTimeUtc.ToString('o'))
        } else {
            Say ('ОШИБКА: отсутствует ' + $candidate)
        }
    }
    Say 'Отсутствие DLL в списке процессов само по себе нормально при закрытой панели архива.'
}
function Check-WcxExports {
    param([string]$Path)
    Say '--- Native WCX DLL export table (read-only) ---'
    if (-not $Path) {
        $ini=Find-TcIni
        if (-not $ini) { Say 'WCX_EXPORT_CHECK: no TC INI; use -WcxPath'; return }
        $row=Select-String -LiteralPath $ini -Pattern '(?i)^\s*wim\s*=\s*(?:\d+\s*,\s*)?(.+ISO-WCX\.wcx)\s*$' -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $row) { Say 'WCX_EXPORT_CHECK: WIM association not found'; return }
        $Path=[Environment]::ExpandEnvironmentVariables(
            $row.Matches[0].Groups[1].Value.Trim().Trim('"'))
        if ([IntPtr]::Size -eq 8) {
            $Path=[IO.Path]::ChangeExtension($Path,'wcx64')
        }
    }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Say ('WCX_EXPORT_CHECK: no DLL at ' + $Path)
        return
    }
    if (-not ('IsoWcxReadOnlyExportsV1' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class IsoWcxReadOnlyExportsV1 {
    // DONT_RESOLVE_DLL_REFERENCES = 1: read export table without running DllMain
    // or any plug-in code. Do not call any returned function pointer.
    [DllImport("kernel32.dll", EntryPoint="LoadLibraryExW", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern IntPtr LoadLibraryEx(string file, IntPtr ignored, uint flags);
    [DllImport("kernel32.dll", EntryPoint="GetProcAddress", CharSet=CharSet.Ansi, SetLastError=true)]
    public static extern IntPtr GetProcAddress(IntPtr module, string name);
    [DllImport("kernel32.dll", EntryPoint="FreeLibrary", SetLastError=true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool FreeLibrary(IntPtr module);
}
'@ -ErrorAction Stop
    }
    $full=(Resolve-Path -LiteralPath $Path).ProviderPath
    $loaded=[IsoWcxReadOnlyExportsV1]::LoadLibraryEx($full,[IntPtr]::Zero,[uint32]1)
    if ($loaded -eq [IntPtr]::Zero) {
        Say ('WCX_EXPORT_CHECK: PE load failed, Win32=' +
            [Runtime.InteropServices.Marshal]::GetLastWin32Error() +
            ' (check PowerShell and WCX architecture)')
        return
    }
    try {
        $missing=@()
        $required=@('OpenArchiveW','ReadHeaderExW','ProcessFileW',
                    'GetPackerCaps','PackFilesW','PackFiles',
                    'DeleteFilesW','CloseArchive','SetProcessDataProcW')
        foreach ($name in $required) {
            $pointer=[IsoWcxReadOnlyExportsV1]::GetProcAddress($loaded,$name)
            if ($pointer -eq [IntPtr]::Zero) {
                $missing+=@($name)
                Say ('WCX_EXPORT_MISSING=' + $name)
            } else {
                Say ('WCX_EXPORT_PRESENT=' + $name)
            }
        }
        if ($missing.Count -eq 0) {
            Say 'WCX_EXPORT_SUMMARY=PASS: all required WCX ABI entry points are exported'
        } else {
            Say ('WCX_EXPORT_SUMMARY=FAIL: ' + $missing.Count +
                 ' missing; TC cannot call missing entry points')
        }
        Say 'Only export names were inspected; no DLL entry point was executed.'
    } finally {
        [void][IsoWcxReadOnlyExportsV1]::FreeLibrary($loaded)
    }
}
function Check-Edit-Log {
    Say '--- Последние события F5 и очистки WIM ---'
    if (-not $env:LOCALAPPDATA) { Say 'LOCALAPPDATA отсутствует.'; return }
    $file = Join-Path $env:LOCALAPPDATA 'ISO-WCX\Logs\edit-errors.log'
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        Say 'Журнал отсутствует. Новая версия WCX создаёт его при вызове F5 в TC.'
        return
    }
    Say ('Журнал: ' + $file)
    try {
        $tail=@(Get-Content -LiteralPath $file -Encoding UTF8 -Tail 160)
        foreach ($line in @($tail | Select-Object -Last 40)) {
            Say ([string]$line)
        }
        $lastOpen=-1
        for ($i=0; $i -lt $tail.Count; $i++) {
            if ($tail[$i] -match 'OpenArchive(W| ANSI) succeeded') {
                $lastOpen=$i
            }
        }
        if ($lastOpen -lt 0) {
            Say 'TC_F5_CALLBACK_SINCE_OPEN=unknown (archive open not in recent log)'
        } else {
            $recent=@($tail[$lastOpen..($tail.Count-1)])
            $probe=@($recent | Where-Object {
                $_ -match 'TC_F5_WRITE_SHARE_PROBE='
            } | Select-Object -Last 1)
            if ($probe.Count -gt 0) {
                Say ('TC_F5_PROBE_LAST=' + $probe[0])
            } else {
                Say 'TC_F5_PROBE_LAST=absent (check the currently installed WCX build)'
            }
            $callbacks=@($recent | Where-Object {
                $_ -match 'PackFiles(W| ANSI) entered'
            })
            if ($callbacks.Count -eq 0) {
                Say 'TC_F5_CALLBACK_SINCE_OPEN=not-observed'
                Say 'TC_F5_HOST_NOTE: no callback was logged; this alone does not prove F5 was pressed after opening.'
            } else {
                Say 'TC_F5_CALLBACK_SINCE_OPEN=observed'
            }
        }
    } catch { Say ('Не удалось прочитать журнал: ' + $_.Exception.Message) }
}
try { Check-Installed-Wcx } catch { Say ('WCX файл: ' + $_.Exception.Message) }
try { Check-WcxExports -Path $WcxPath } catch { Say ('WCX exports: ' + $_.Exception.Message) }
try { Check-Edit-Log } catch { Say ('Журнал: ' + $_.Exception.Message) }

if ($ReportPath) {
    # .NET GetFullPath resolves relative names against the PROCESS working
    # directory, which can differ from PowerShell's Get-Location. A report
    # passed as ".\ISO-WCX-F5.txt" must land in the invoking PS directory.
    $requested=[Environment]::ExpandEnvironmentVariables($ReportPath)
    if (-not [IO.Path]::IsPathRooted($requested)) {
        $location=Get-Location
        if ($location.Provider.Name -ne 'FileSystem') {
            throw 'Относительный путь отчёта допустим только в файловой системе.'
        }
        $requested=Join-Path -Path $location.ProviderPath -ChildPath $requested
    }
    $target=[IO.Path]::GetFullPath($requested)
    $directory=Split-Path -Parent $target
    if (-not (Test-Path -LiteralPath $directory -PathType Container)) {
        throw ('Папка для отчёта не существует: ' + $directory)
    }
    [IO.File]::WriteAllLines(
        $target,$script:Lines.ToArray(),
        (New-Object System.Text.UTF8Encoding($true)))
    Write-Host ('Отчёт: ' + $target)
}
