# ISO-WCX read-only field diagnostic; Windows PowerShell 5.1 / PowerShell 7.
# No TC config edits, image writes, mounting, unmounting or workspace deletion.
# Run: powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\ISO-WCX-Diagnostic.ps1 -WimPath "D:\sources\boot.wim" -CheckDism
[CmdletBinding()]
param(
    [string]$WimPath,
    [string]$IniPath,
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
    if (-not ('IsoWcxTcPanelQuery' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class IsoWcxTcPanelQuery {
    [DllImport("user32.dll", EntryPoint="SendMessageTimeoutW", SetLastError=true)]
    public static extern IntPtr SendMessageTimeout(
        IntPtr window, uint message, IntPtr wParam, IntPtr lParam,
        uint flags, uint timeout, out IntPtr result);
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
                $ok=[IsoWcxTcPanelQuery]::SendMessageTimeout(
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
            if ($active -ne 'left' -or $values[1019] -lt 1 -or
                $leftType -ne 'filesystem' -or $rightType -ne 'WCX-plugin') {
                Say 'TC_F5_CONTEXT_WARNING: expected active LEFT filesystem panel with selected files and RIGHT WCX destination.'
            } else {
                Say 'TC_F5_CONTEXT: source/destination panel types and selected files are correct.'
            }
            Say 'Panel query does not include the WCX inner subdirectory. Confirm RIGHT path ends with boot.wim\image1\ and lists Windows/Program Files.'
        } catch {
            Say ('TC_PANEL_QUERY pid=' + $proc.Id + ': ' + $_.Exception.Message)
        }
    }
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
        Get-Content -LiteralPath $file -Encoding UTF8 -Tail 40 |
            ForEach-Object { Say ([string]$_) }
    } catch { Say ('Не удалось прочитать журнал: ' + $_.Exception.Message) }
}
try { Check-Installed-Wcx } catch { Say ('WCX файл: ' + $_.Exception.Message) }
try { Check-Edit-Log } catch { Say ('Журнал: ' + $_.Exception.Message) }

if ($ReportPath) {
    $target = [IO.Path]::GetFullPath(
        [Environment]::ExpandEnvironmentVariables($ReportPath))
    $directory = Split-Path -Parent $target
    if (-not (Test-Path -LiteralPath $directory -PathType Container)) {
        throw ('Папка для отчёта не существует: ' + $directory)
    }
    [IO.File]::WriteAllLines(
        $target,$script:Lines.ToArray(),
        (New-Object System.Text.UTF8Encoding($true)))
    Write-Host ('Отчёт: ' + $target)
}
