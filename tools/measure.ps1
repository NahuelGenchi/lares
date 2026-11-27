<#
.SYNOPSIS
Records how loaded a Windows 11 PC is at rest, and compares two recordings.

.DESCRIPTION
Take one snapshot before a change (installing Lares, turning a part of it on)
and one after, in the same conditions, then compare them. docs/GAINS.md has
the method.

A snapshot only reads. It changes no setting, needs no administrator rights,
leaves nothing running, and reads its counters twice: when the idle window
opens and when it closes.

What it writes is meant to be safe to post: no user, computer, device,
process or start-up entry names (other than the process names you ask for),
no serial numbers, no paths and no network details. The script checks its own
result for this PC's account and computer name before it writes the file.

.EXAMPLE
powershell -ExecutionPolicy Bypass -File tools\measure.ps1 -Out before.json -Label before

.EXAMPLE
powershell -ExecutionPolicy Bypass -File tools\measure.ps1 -Out after.json -Label after -Processes lares,lares-pad

.EXAMPLE
powershell -ExecutionPolicy Bypass -File tools\measure.ps1 -Before b1.json,b2.json,b3.json -After a1.json,a2.json,a3.json
#>
[CmdletBinding(DefaultParameterSetName = 'Snapshot')]
param(
    # Where to write the snapshot.
    [Parameter(ParameterSetName = 'Snapshot', Mandatory = $true)]
    [string]$Out,

    # How long the PC is left alone while processor and graphics use are measured.
    [Parameter(ParameterSetName = 'Snapshot')]
    [ValidateRange(5, 3600)]
    [int]$IdleSeconds = 60,

    # Processes whose own memory and processor time are recorded, by name without ".exe".
    [Parameter(ParameterSetName = 'Snapshot')]
    [ValidatePattern('^[A-Za-z0-9._,-]{1,200}$')]
    [string[]]$Processes = @('lares', 'lares-pad', 'lares-share'),

    # A short word for this snapshot, such as "before" or "after".
    [Parameter(ParameterSetName = 'Snapshot')]
    [ValidatePattern('^[A-Za-z0-9 ._-]{1,40}$')]
    [string]$Label = 'snapshot',

    # Snapshots taken before the change. With several, the middle value of each reading is used.
    [Parameter(ParameterSetName = 'Compare', Mandatory = $true)]
    [string[]]$Before,

    # Snapshots taken after the change.
    [Parameter(ParameterSetName = 'Compare', Mandatory = $true)]
    [string[]]$After
)

Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'
$Invariant = [System.Globalization.CultureInfo]::InvariantCulture
$FormatVersion = 1

# "powershell -File" hands a list over as one text with commas in it.
function Split-List([string[]]$items) {
    @($items | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
}

function Round1([double]$value) { [math]::Round($value, 1) }
function ToMB([double]$bytes) { [math]::Round($bytes / 1MB, 1) }

# The performance classes are read by their class names, which are the same
# in every display language; counter paths are not.
function Read-Class([string]$name) {
    try { @(Get-CimInstance -ClassName $name -ErrorAction Stop) } catch { @() }
}

function Read-Cpu {
    $row = Read-Class 'Win32_PerfRawData_PerfOS_Processor' | Where-Object { $_.Name -eq '_Total' }
    if (-not $row) { return $null }
    @{ Idle = [double]$row.PercentProcessorTime; Time = [double]$row.Timestamp_Sys100NS }
}

# Running time of every graphics engine, added up over the processes using it.
# An engine's name carries a process number and an adapter number; neither is kept.
function Read-Gpu {
    $engines = @{}
    $time = 0.0
    foreach ($row in (Read-Class 'Win32_PerfRawData_GPUPerformanceCounters_GPUEngine')) {
        if ($row.Name -notmatch '(luid_\w+_phys_\d+_eng_\d+)') { continue }
        $key = $Matches[1]
        $engines[$key] = [double]$engines[$key] + [double]$row.RunningTime
        $time = [double]$row.Timestamp_Sys100NS
    }
    if ($engines.Count -eq 0) { return $null }
    @{ Engines = $engines; Time = $time }
}

function Read-ProcessTimes([string[]]$names) {
    $times = @{}
    foreach ($name in $names) {
        $seconds = 0.0
        foreach ($p in @(Get-Process -Name $name -ErrorAction SilentlyContinue)) {
            try { $seconds += $p.TotalProcessorTime.TotalSeconds } catch { }
        }
        $times[$name] = $seconds
    }
    $times
}

function Test-TurnedOff($approvedKey, [string]$name) {
    if (-not $approvedKey) { return $false }
    $state = $approvedKey.GetValue($name, $null)
    ($state -is [byte[]]) -and ($state.Length -gt 0) -and (($state[0] -band 1) -eq 1)
}

# Start-up entries are counted, never named: the Run keys and the Startup
# folders, less the ones turned off in Settings or Task Manager.
function Count-Startup {
    $on = 0
    $off = 0
    $current = 'Software\Microsoft\Windows\CurrentVersion'
    $sources = @(
        @{ Run = "HKCU:\$current\Run"; Approved = "HKCU:\$current\Explorer\StartupApproved\Run" },
        @{ Run = "HKLM:\$current\Run"; Approved = "HKLM:\$current\Explorer\StartupApproved\Run" },
        @{ Run = 'HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Run'; Approved = "HKLM:\$current\Explorer\StartupApproved\Run32" }
    )
    foreach ($source in $sources) {
        $run = Get-Item -LiteralPath $source.Run -ErrorAction SilentlyContinue
        if (-not $run) { continue }
        $approved = Get-Item -LiteralPath $source.Approved -ErrorAction SilentlyContinue
        foreach ($name in $run.GetValueNames()) {
            if ($name -eq '') { continue }
            if (Test-TurnedOff $approved $name) { $off++ } else { $on++ }
        }
    }
    $folders = @(
        @{ Path = [Environment]::GetFolderPath('Startup'); Approved = "HKCU:\$current\Explorer\StartupApproved\StartupFolder" },
        @{ Path = [Environment]::GetFolderPath('CommonStartup'); Approved = "HKLM:\$current\Explorer\StartupApproved\StartupFolder" }
    )
    foreach ($folder in $folders) {
        if (-not $folder.Path -or -not (Test-Path -LiteralPath $folder.Path)) { continue }
        $approved = Get-Item -LiteralPath $folder.Approved -ErrorAction SilentlyContinue
        foreach ($file in @(Get-ChildItem -LiteralPath $folder.Path -File -Force -ErrorAction SilentlyContinue)) {
            if ($file.Name -eq 'desktop.ini') { continue }
            if (Test-TurnedOff $approved $file.Name) { $off++ } else { $on++ }
        }
    }
    @{ On = $on; Off = $off }
}

function Take-Snapshot {
    $names = Split-List $Processes
    $os = Get-CimInstance -ClassName Win32_OperatingSystem

    Write-Host "Measuring for $IdleSeconds seconds. Leave the PC alone until this finishes."
    $cpu0 = Read-Cpu
    $gpu0 = Read-Gpu
    $proc0 = Read-ProcessTimes $names
    Start-Sleep -Seconds $IdleSeconds
    $cpu1 = Read-Cpu
    $gpu1 = Read-Gpu
    $proc1 = Read-ProcessTimes $names

    $cpuPercent = $null
    if ($cpu0 -and $cpu1 -and ($cpu1.Time -gt $cpu0.Time)) {
        $busy = 1.0 - (($cpu1.Idle - $cpu0.Idle) / ($cpu1.Time - $cpu0.Time))
        $cpuPercent = Round1 ([math]::Max(0.0, [math]::Min(1.0, $busy)) * 100.0)
    }

    # The busiest engine, as Task Manager shows one figure for a graphics card.
    $gpuPercent = $null
    if ($gpu0 -and $gpu1 -and ($gpu1.Time -gt $gpu0.Time)) {
        $busiest = 0.0
        foreach ($key in $gpu1.Engines.Keys) {
            if (-not $gpu0.Engines.ContainsKey($key)) { continue }
            $share = ($gpu1.Engines[$key] - $gpu0.Engines[$key]) / ($gpu1.Time - $gpu0.Time)
            if ($share -gt $busiest) { $busiest = $share }
        }
        $gpuPercent = Round1 ([math]::Min(1.0, $busiest) * 100.0)
    }

    $gpuMemory = $null
    $adapters = @(Read-Class 'Win32_PerfRawData_GPUPerformanceCounters_GPUAdapterMemory')
    if ($adapters.Count -gt 0) {
        $gpuMemory = ToMB (($adapters | Measure-Object -Property DedicatedUsage -Sum).Sum)
    }

    $memory = Read-Class 'Win32_PerfRawData_PerfOS_Memory' | Select-Object -First 1
    $system = Read-Class 'Win32_PerfRawData_PerfOS_System' | Select-Object -First 1
    $totalBytes = [double]$os.TotalVisibleMemorySize * 1KB
    $freeBytes = [double]$os.FreePhysicalMemory * 1KB
    $startup = Count-Startup

    $committed = $null
    if ($memory) { $committed = ToMB ([double]$memory.CommittedBytes) }
    $processCount = $null
    $threadCount = $null
    if ($system) {
        $processCount = [int]$system.Processes
        $threadCount = [int]$system.Threads
    }

    $named = [ordered]@{}
    foreach ($name in $names) {
        $running = @(Get-Process -Name $name -ErrorAction SilentlyContinue)
        $private = 0.0
        $working = 0.0
        foreach ($p in $running) {
            $private += $p.PrivateMemorySize64
            $working += $p.WorkingSet64
        }
        $named[$name.ToLowerInvariant()] = [ordered]@{
            count          = $running.Count
            private_mb     = ToMB $private
            working_set_mb = ToMB $working
            cpu_seconds    = [math]::Round([math]::Max(0.0, $proc1[$name] - $proc0[$name]), 2)
        }
    }

    $snapshot = [ordered]@{
        format              = $FormatVersion
        label               = $Label
        taken_utc           = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ', $Invariant)
        windows_build       = [int]$os.BuildNumber
        minutes_since_boot  = [int]([DateTime]::Now - $os.LastBootUpTime).TotalMinutes
        logical_processors  = [Environment]::ProcessorCount
        memory_total_mb     = ToMB $totalBytes
        idle_seconds        = $IdleSeconds
        memory_in_use_mb    = ToMB ($totalBytes - $freeBytes)
        memory_committed_mb = $committed
        processes           = $processCount
        threads             = $threadCount
        cpu_percent         = $cpuPercent
        gpu_percent         = $gpuPercent
        gpu_memory_mb       = $gpuMemory
        startup_entries     = $startup.On
        startup_entries_off = $startup.Off
        named_processes     = $named
    }

    $json = $snapshot | ConvertTo-Json -Depth 4

    # Nothing here should name this PC or its user. If it does, say so and write nothing.
    $lower = $json.ToLowerInvariant()
    foreach ($private in @($env:USERNAME, $env:COMPUTERNAME, $env:USERDOMAIN)) {
        if ($private -and ($private.Length -ge 3) -and $lower.Contains($private.ToLowerInvariant())) {
            throw 'The snapshot would hold the account or computer name of this PC, in a process name or the label. Nothing was written.'
        }
    }
    if ($json.Contains('\') -or $json.Contains('/')) { throw 'The snapshot would hold a path. Nothing was written.' }

    $target = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Out)
    [System.IO.File]::WriteAllText($target, $json + "`n", (New-Object System.Text.UTF8Encoding $false))
    Write-Host 'Snapshot written. It holds numbers only; read it before you post it.'
    Write-Host $json
}

function Median([double[]]$values) {
    $sorted = @($values | Sort-Object)
    $n = $sorted.Count
    if ($n % 2 -eq 1) { return $sorted[($n - 1) / 2] }
    ($sorted[$n / 2 - 1] + $sorted[$n / 2]) / 2.0
}

function Read-Snapshots([string[]]$paths) {
    $all = @()
    foreach ($path in (Split-List $paths)) {
        $one = Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($one.format -ne $FormatVersion) { throw "$(Split-Path -Leaf $path) is not a snapshot this script can read." }
        $all += $one
    }
    $all
}

# One reading across several snapshots: the middle value, or nothing if any lacks it.
function Reading($snapshots, [scriptblock]$pick) {
    $values = @()
    foreach ($s in $snapshots) {
        $v = & $pick $s
        if ($null -eq $v) { return $null }
        $values += [double]$v
    }
    Median $values
}

function Format-Number($value, [string]$unit) {
    if ($null -eq $value) { return 'not measured' }
    $text = ([double]$value).ToString('#,0.##', $Invariant)
    if ($unit) { "$text $unit" } else { $text }
}

function Compare-Snapshots {
    $b = @(Read-Snapshots $Before)
    $a = @(Read-Snapshots $After)

    $rows = @(
        @{ Name = 'Memory in use'; Unit = 'MB'; Pick = { param($s) $s.memory_in_use_mb } },
        @{ Name = 'Memory committed'; Unit = 'MB'; Pick = { param($s) $s.memory_committed_mb } },
        @{ Name = 'Processes'; Unit = ''; Pick = { param($s) $s.processes } },
        @{ Name = 'Threads'; Unit = ''; Pick = { param($s) $s.threads } },
        @{ Name = 'Processor use at rest'; Unit = '%'; Pick = { param($s) $s.cpu_percent } },
        @{ Name = 'Graphics card use at rest'; Unit = '%'; Pick = { param($s) $s.gpu_percent } },
        @{ Name = 'Graphics memory in use'; Unit = 'MB'; Pick = { param($s) $s.gpu_memory_mb } },
        @{ Name = 'Start-up entries turned on'; Unit = ''; Pick = { param($s) $s.startup_entries } }
    )
    $names = @()
    foreach ($s in ($b + $a)) {
        foreach ($p in $s.named_processes.PSObject.Properties) {
            if ($names -notcontains $p.Name) { $names += $p.Name }
        }
    }
    foreach ($name in $names) {
        $memoryPick = { param($s) $p = $s.named_processes.PSObject.Properties[$name]; if ($p) { $p.Value.private_mb } else { 0 } }.GetNewClosure()
        $timePick = { param($s) $p = $s.named_processes.PSObject.Properties[$name]; if ($p) { $p.Value.cpu_seconds } else { 0 } }.GetNewClosure()
        $rows += @{ Name = "$name.exe, own memory"; Unit = 'MB'; Pick = $memoryPick }
        $rows += @{ Name = "$name.exe, processor time"; Unit = 's'; Pick = $timePick }
    }

    $idle = @($b + $a | ForEach-Object { $_.idle_seconds } | Sort-Object -Unique)
    Write-Host "Before: $($b.Count) snapshot(s). After: $($a.Count) snapshot(s). Idle window: $($idle -join ', ') s. With several snapshots, each reading is the middle value."
    if ($idle.Count -gt 1) { Write-Host 'The snapshots used different idle windows; take them again with the same one.' }
    Write-Host ''
    Write-Host '| Reading | Before | After | Change |'
    Write-Host '| :--- | ---: | ---: | ---: |'
    foreach ($row in $rows) {
        $before = Reading $b $row.Pick
        $after = Reading $a $row.Pick
        $change = 'not measured'
        if (($null -ne $before) -and ($null -ne $after)) {
            $delta = $after - $before
            $change = $delta.ToString('+#,0.##;-#,0.##;0', $Invariant)
            if ($row.Unit) { $change = "$change $($row.Unit)" }
            if (($row.Unit -ne '%') -and ($before -ne 0)) {
                $change += ' (' + ($delta / $before * 100.0).ToString('+0.#;-0.#;0', $Invariant) + '%)'
            }
        }
        Write-Host "| $($row.Name) | $(Format-Number $before $row.Unit) | $(Format-Number $after $row.Unit) | $change |"
    }
    Write-Host ''
    Write-Host 'A lower number is lighter. One PC and one day are a reading, not a promise.'
}

if ($PSCmdlet.ParameterSetName -eq 'Compare') { Compare-Snapshots } else { Take-Snapshot }
