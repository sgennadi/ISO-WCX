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
            if (-not $found) { Say 'ISO-WCX не обнаружен среди доступных модулей процесса.' }
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
try { Check-Ini } catch { Say ('INI: ' + $_.Exception.Message) }
try { Check-WimFile } catch { Say ('WIM: ' + $_.Exception.Message) }

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
