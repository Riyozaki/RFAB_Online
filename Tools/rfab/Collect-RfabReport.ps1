<#
.SYNOPSIS
    Collects everything needed to analyse an RFAB Online problem into one zip on the Desktop.

.DESCRIPTION
    One playtest has to give a place in the code, not "it crashed". This script gathers, from an
    installed RFAB + RFAB Online:
      - client logs (tp_client*.log), the startup and crash logs, the last crash dump;
      - the symbol table installed with the client (RFAB_Online_Symbols.gz), so that crash
        addresses can be named for exactly this binary (Tools/rfab/symbolize_crash.py);
      - the co-op config overlay report;
      - server logs and STServer.ini (passwords and tokens are blanked);
      - NetScriptFramework crash logs and SKSE logs;
      - the MO2 profile lists (plugins, load order, mod list);
      - versions, sizes, PE timestamps and SHA-256 of the installed binaries, basic system info.
    Nothing is sent anywhere: the result is a zip file the player attaches to the report.

    Every step is independent. A step that fails is written down in REPORT.txt and the rest
    is still collected.

.PARAMETER GameRoot
    The RFAB folder (the one that contains MO2\mods\RFAB_Online). Detected automatically.
.PARAMETER OutDir
    Where to put the zip. Desktop by default.
.PARAMETER MaxLogMB
    Logs bigger than this keep only their last MaxLogMB megabytes.
.PARAMETER NoDumps
    Leave crash dumps (*.dmp) out.
.PARAMETER NoUi
    Never show a dialog and never open Explorer (CI, self-test).
.PARAMETER SelfTest
    Build a fake installation in TEMP, collect it and check the result. Exit code 0 or 1.
#>
param(
    [string]$GameRoot = "",
    [string]$OutDir = "",
    [int]$MaxLogMB = 30,
    [switch]$NoDumps,
    [switch]$NoUi,
    [switch]$SelfTest
)

$ErrorActionPreference = "Stop"
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

$CollectorVersion = "1"
$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$script:Notes = New-Object System.Collections.Generic.List[string]
$script:Manifest = New-Object System.Collections.Generic.List[string]
$script:Binaries = New-Object System.Collections.Generic.List[string]

function Join-Parts([string]$Base, [string[]]$Parts) {
    # Windows PowerShell 5.1: Join-Path takes exactly two paths.
    $result = $Base
    foreach ($part in $Parts) { $result = Join-Path $result $part }
    return $result
}

function Add-Note([string]$Text) {
    [void]$script:Notes.Add($Text)
}

function Copy-Capped([string]$Src, [string]$Dst, [long]$MaxBytes) {
    # Reads through a shared handle (the game may still have the log open) and keeps the END
    # of a file that is over the limit: the end of a log is the part that matters.
    $dstDir = Split-Path -Parent $Dst
    if (-not (Test-Path -LiteralPath $dstDir)) { New-Item -ItemType Directory -Path $dstDir -Force | Out-Null }
    $share = [System.IO.FileShare]"ReadWrite, Delete"
    $in = New-Object System.IO.FileStream($Src, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, $share)
    $truncated = $false
    try {
        if ($MaxBytes -gt 0 -and $in.Length -gt $MaxBytes) {
            [void]$in.Seek($in.Length - $MaxBytes, [System.IO.SeekOrigin]::Begin)
            $truncated = $true
        }
        $out = New-Object System.IO.FileStream($Dst, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write)
        try { $in.CopyTo($out) } finally { $out.Dispose() }
    } finally { $in.Dispose() }
    return $truncated
}

function Add-ReportFile([string]$Src, [string]$RelDst, [long]$MaxBytes) {
    # Returns $true when the file was added.
    try {
        if (-not (Test-Path -LiteralPath $Src -PathType Leaf)) { return $false }
        $item = Get-Item -LiteralPath $Src
        $dst = Join-Path $script:Stage $RelDst
        $truncated = Copy-Capped $Src $dst $MaxBytes
        $mark = ""
        if ($truncated) { $mark = " (only the last $([math]::Round($MaxBytes / 1MB)) MB)" }
        [void]$script:Manifest.Add(("{0} | {1} bytes | {2:yyyy-MM-dd HH:mm:ss}{3} | from {4}" -f $RelDst, $item.Length, $item.LastWriteTime, $mark, $Src))
        return $true
    } catch {
        Add-Note ("could not copy {0}: {1}" -f $Src, $_.Exception.Message)
        return $false
    }
}

function Get-PeTimestamp([string]$Path) {
    # TimeDateStamp of the PE header, lower-case hex. It is what the linker map calls "Timestamp is".
    try {
        $fs = New-Object System.IO.FileStream($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
        try {
            if ($fs.Length -lt 0x40) { return "" }
            $br = New-Object System.IO.BinaryReader($fs)
            if ($br.ReadUInt16() -ne 0x5A4D) { return "" }
            $fs.Position = 0x3C
            $peOffset = $br.ReadInt32()
            if ($peOffset -le 0 -or ($peOffset + 12) -gt $fs.Length) { return "" }
            $fs.Position = $peOffset
            if ($br.ReadUInt32() -ne 0x00004550) { return "" }
            $fs.Position = $peOffset + 8
            return ("{0:x8}" -f $br.ReadUInt32())
        } finally { $fs.Dispose() }
    } catch { return "" }
}

function Add-BinaryInfo([string]$Path, [string]$Label) {
    try {
        if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return }
        $item = Get-Item -LiteralPath $Path
        $version = ""
        try { $version = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($Path).FileVersion } catch { }
        $hash = ""
        try { $hash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() } catch { }
        # symbolize_crash.py reads this line: "<path> | ... | pe_timestamp=<hex> | ..."
        [void]$script:Binaries.Add(("{0} | size={1} | version={2} | modified={3:yyyy-MM-dd HH:mm:ss} | pe_timestamp={4} | sha256={5}" -f `
            $Label, $item.Length, $version, $item.LastWriteTime, (Get-PeTimestamp $Path), $hash))
    } catch {
        Add-Note ("could not describe {0}: {1}" -f $Path, $_.Exception.Message)
    }
}

function Find-GameRoot([string]$Hint, [string]$ScriptDir, [bool]$AllowDialog) {
    $candidates = New-Object System.Collections.Generic.List[string]
    if (-not [string]::IsNullOrWhiteSpace($Hint)) { [void]$candidates.Add($Hint) }
    $walk = $ScriptDir
    for ($i = 0; $i -lt 5 -and -not [string]::IsNullOrWhiteSpace($walk); $i++) {
        [void]$candidates.Add($walk)
        $walk = Split-Path -Parent $walk
    }
    try {
        $regKey = "HKCU:\Software\TiltedPhoques\TiltedEvolution\Skyrim Special Edition"
        if (Test-Path -LiteralPath $regKey) {
            $props = Get-ItemProperty -LiteralPath $regKey -ErrorAction SilentlyContinue
            if ($props.RfabRoot) { [void]$candidates.Add($props.RfabRoot) }
            if ($props.Mo2Path) { [void]$candidates.Add((Split-Path -Parent $props.Mo2Path)); [void]$candidates.Add($props.Mo2Path) }
            if ($props.TitlePath) { [void]$candidates.Add($props.TitlePath) }
        }
    } catch { }
    foreach ($candidate in $candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate) -or -not (Test-Path -LiteralPath $candidate)) { continue }
        if (Test-Path -LiteralPath (Join-Parts $candidate @("MO2", "mods", "RFAB_Online"))) { return $candidate }
        if (Test-Path -LiteralPath (Join-Parts $candidate @("mods", "RFAB_Online"))) { return $candidate }
    }
    if ($AllowDialog) {
        try {
            Add-Type -AssemblyName System.Windows.Forms | Out-Null
            $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
            $dialog.Description = "Select your RFAB folder (the one that contains MO2\mods\RFAB_Online)"
            if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { return $dialog.SelectedPath }
        } catch { }
    }
    return ""
}

function Hide-Secrets([string]$Text) {
    # "sPassword = hunter2" -> "sPassword = <hidden>"; an empty value stays empty.
    $lines = $Text -split "`r?`n"
    $out = New-Object System.Collections.Generic.List[string]
    foreach ($line in $lines) {
        if ($line -match '^(\s*[;#]?\s*[^;#=\s]*(?i:password|token|secret|apikey)[^=\s]*\s*=\s*)(\S.*)$') {
            [void]$out.Add($Matches[1] + "<hidden>")
        } else {
            [void]$out.Add($line)
        }
    }
    return ($out -join "`r`n")
}

function Invoke-Step([string]$Name, [scriptblock]$Body) {
    try { & $Body } catch { Add-Note ("step '{0}' failed: {1}" -f $Name, $_.Exception.Message) }
}

function Invoke-Collect([string]$Root, [string]$TargetDir, [int]$LogCapMB, [bool]$WithDumps) {
    $stamp = (Get-Date).ToString("yyyyMMdd_HHmmss")
    $script:Stage = Join-Path ([System.IO.Path]::GetTempPath()) ("rfab_report_" + [Guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $script:Stage -Force | Out-Null
    $logCap = [long]$LogCapMB * 1MB

    $mo2Base = $Root
    if (Test-Path -LiteralPath (Join-Parts $Root @("MO2", "mods", "RFAB_Online"))) { $mo2Base = Join-Path $Root "MO2" }
    $modRoot = Join-Parts $mo2Base @("mods", "RFAB_Online")
    $clientDir = Join-Path $modRoot "SkyrimTogetherReborn"

    Invoke-Step "client logs" {
        if (-not (Test-Path -LiteralPath $clientDir)) { Add-Note "client folder not found: $clientDir"; return }
        $clientSearchDirs = @(
            $clientDir,
            (Join-Parts $mo2Base @("overwrite", "SkyrimTogetherReborn")),
            (Join-Parts $Root @("Data", "SkyrimTogetherReborn"))
        )
        $foundClientLogs = New-Object System.Collections.Generic.List[object]
        foreach ($candDir in $clientSearchDirs) {
            $candLogs = Join-Path $candDir "logs"
            if (Test-Path -LiteralPath $candLogs) {
                foreach ($log in @(Get-ChildItem -LiteralPath $candLogs -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -like "*.log*" })) {
                    [void]$foundClientLogs.Add($log)
                }
            }
        }
        if ($foundClientLogs.Count -gt 0) {
            $seenNames = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
            foreach ($log in @($foundClientLogs | Sort-Object LastWriteTime -Descending | Select-Object -First 12)) {
                $dstName = $log.Name
                if (-not $seenNames.Add($dstName)) { $dstName = ("overwrite_" + $log.Name) }
                [void](Add-ReportFile $log.FullName (Join-Parts "client" @("logs", $dstName)) $logCap)
            }
        } else { Add-Note ("no client log folder: {0} (the client has never started from this install)" -f (Join-Path $clientDir "logs")) }
        foreach ($file in @(Get-ChildItem -LiteralPath $clientDir -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -like "RFAB_Online_*" })) {
            [void](Add-ReportFile $file.FullName (Join-Path "client" $file.Name) $logCap)
        }
        foreach ($candDir in $clientSearchDirs) {
            $overlayDir = Join-Path $candDir "rfab_coop_config"
            if (Test-Path -LiteralPath $overlayDir) {
                foreach ($file in @(Get-ChildItem -LiteralPath $overlayDir -File -Recurse -ErrorAction SilentlyContinue | Where-Object { $_.Length -lt 2MB } | Select-Object -First 40)) {
                    [void](Add-ReportFile $file.FullName (Join-Parts "client" @("rfab_coop_config", $file.Name)) $logCap)
                }
                break
            }
        }
        if ($WithDumps) {
            $foundDumps = New-Object System.Collections.Generic.List[object]
            foreach ($candDir in $clientSearchDirs) {
                if (Test-Path -LiteralPath $candDir) {
                    foreach ($dump in @(Get-ChildItem -LiteralPath $candDir -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -like "crash_*.dmp" })) {
                        [void]$foundDumps.Add($dump)
                    }
                }
            }
            foreach ($dump in @($foundDumps | Sort-Object LastWriteTime -Descending | Select-Object -First 2)) {
                [void](Add-ReportFile $dump.FullName (Join-Path "client" $dump.Name) 0)
            }
        }
        foreach ($binary in @("SkyrimSE.exe", "SkyrimTogether.exe", "EarlyLoad.dll", "TPProcess.exe")) {
            Add-BinaryInfo (Join-Path $clientDir $binary) ("client\" + $binary)
        }
    }

    Invoke-Step "game folder logs" {
        foreach ($logName in @("RFAB_Online_Startup.log", "RFAB_Online_Crash.log")) {
            [void](Add-ReportFile (Join-Path $Root $logName) (Join-Path "game" $logName) $logCap)
        }
        foreach ($binary in @("SkyrimSE.exe", "skse64_loader.exe", "skse64_1_5_97.dll")) {
            Add-BinaryInfo (Join-Path $Root $binary) ("game\" + $binary)
        }
    }

    Invoke-Step "server" {
        $serverDir = ""
        foreach ($candidate in @((Join-Path $Root "RFAB_Online_Server"), (Join-Path (Split-Path -Parent $Root) "RFAB_Online_Server"))) {
            if ($candidate -and (Test-Path -LiteralPath $candidate)) { $serverDir = $candidate; break }
        }
        if (-not $serverDir) { Add-Note "no RFAB_Online_Server folder next to the game (fine if this machine does not host)"; return }
        $serverLogs = Join-Path $serverDir "logs"
        if (Test-Path -LiteralPath $serverLogs) {
            foreach ($log in @(Get-ChildItem -LiteralPath $serverLogs -File -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 12)) {
                [void](Add-ReportFile $log.FullName (Join-Parts "server" @("logs", $log.Name)) $logCap)
            }
        }
        $ini = Join-Parts $serverDir @("config", "STServer.ini")
        if (Test-Path -LiteralPath $ini) {
            $text = Hide-Secrets ([System.IO.File]::ReadAllText($ini, [System.Text.Encoding]::UTF8))
            $dst = Join-Parts $script:Stage @("server", "config", "STServer.ini")
            New-Item -ItemType Directory -Path (Split-Path -Parent $dst) -Force | Out-Null
            [System.IO.File]::WriteAllText($dst, $text, $Utf8NoBom)
            [void]$script:Manifest.Add("server\config\STServer.ini | passwords and tokens hidden | from $ini")
        }
        foreach ($file in @(Get-ChildItem -LiteralPath $serverDir -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -like "RFAB_Online_*" })) {
            [void](Add-ReportFile $file.FullName (Join-Path "server" $file.Name) $logCap)
        }
        $checkpoints = Join-Parts $serverDir @("Data", "rfab_checkpoints.bin")
        if (Test-Path -LiteralPath $checkpoints) {
            $cp = Get-Item -LiteralPath $checkpoints
            Add-Note ("server checkpoint store: {0} bytes, written {1:yyyy-MM-dd HH:mm:ss} (not included)" -f $cp.Length, $cp.LastWriteTime)
        }
        foreach ($binary in @("SkyrimTogetherServer.exe", "STServer.dll")) {
            Add-BinaryInfo (Join-Path $serverDir $binary) ("server\" + $binary)
        }
    }

    Invoke-Step "NetScriptFramework crash logs" {
        $found = New-Object System.Collections.Generic.List[object]
        foreach ($dir in @((Join-Parts $mo2Base @("overwrite", "NetScriptFramework", "Crash")), (Join-Parts $Root @("Data", "NetScriptFramework", "Crash")))) {
            if (Test-Path -LiteralPath $dir) {
                foreach ($file in @(Get-ChildItem -LiteralPath $dir -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -like "Crash_*.txt" })) { [void]$found.Add($file) }
            }
        }
        foreach ($file in @($found | Sort-Object LastWriteTime -Descending | Select-Object -First 5)) {
            [void](Add-ReportFile $file.FullName (Join-Path "netscriptframework" $file.Name) $logCap)
        }
    }

    Invoke-Step "SKSE logs" {
        $documents = [Environment]::GetFolderPath("MyDocuments")
        if ([string]::IsNullOrWhiteSpace($documents)) { return }
        $skseDir = Join-Parts $documents @("My Games", "Skyrim Special Edition", "SKSE")
        if (-not (Test-Path -LiteralPath $skseDir)) { Add-Note "no SKSE log folder: $skseDir"; return }
        foreach ($log in @(Get-ChildItem -LiteralPath $skseDir -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -like "*.log" } | Sort-Object LastWriteTime -Descending | Select-Object -First 40)) {
            [void](Add-ReportFile $log.FullName (Join-Path "skse" $log.Name) (5MB))
        }
    }

    Invoke-Step "MO2 profile" {
        $profilesDir = Join-Path $mo2Base "profiles"
        if (-not (Test-Path -LiteralPath $profilesDir)) { Add-Note "no MO2 profiles folder: $profilesDir"; return }
        $selected = ""
        $mo2Ini = Join-Path $mo2Base "ModOrganizer.ini"
        if (Test-Path -LiteralPath $mo2Ini) {
            foreach ($line in [System.IO.File]::ReadAllLines($mo2Ini)) {
                if ($line -match '^\s*selected_profile\s*=\s*(?:@ByteArray\((.*)\)|(.*))\s*$') {
                    if ($Matches[1]) { $selected = $Matches[1] } else { $selected = $Matches[2] }
                    break
                }
            }
        }
        $profiles = @(Get-ChildItem -LiteralPath $profilesDir -Directory -ErrorAction SilentlyContinue)
        $chosen = @($profiles | Where-Object { $_.Name -eq $selected })
        if ($chosen.Count -eq 0) { $chosen = @($profiles | Select-Object -First 4) }
        foreach ($profileDir in $chosen) {
            foreach ($listName in @("plugins.txt", "loadorder.txt", "modlist.txt")) {
                [void](Add-ReportFile (Join-Path $profileDir.FullName $listName) (Join-Parts "mo2_profile" @($profileDir.Name, $listName)) (4MB))
            }
        }
        Add-Note ("MO2 selected profile: '{0}'" -f $selected)
    }

    $system = New-Object System.Collections.Generic.List[string]
    Invoke-Step "system info" {
        [void]$system.Add(("local time: {0:yyyy-MM-dd HH:mm:ss}   UTC: {1:yyyy-MM-dd HH:mm:ss}" -f (Get-Date), (Get-Date).ToUniversalTime()))
        [void]$system.Add("time zone: " + [System.TimeZoneInfo]::Local.Id)
        [void]$system.Add("os: " + [System.Environment]::OSVersion.VersionString)
        [void]$system.Add("powershell: " + $PSVersionTable.PSVersion.ToString())
        if (Get-Command Get-CimInstance -ErrorAction SilentlyContinue) {
            try {
                $os = Get-CimInstance Win32_OperatingSystem
                [void]$system.Add(("windows: {0} build {1}" -f $os.Caption, $os.BuildNumber))
                [void]$system.Add(("memory: {0:N1} GB total, {1:N1} GB free" -f ($os.TotalVisibleMemorySize / 1MB), ($os.FreePhysicalMemory / 1MB)))
                foreach ($cpu in @(Get-CimInstance Win32_Processor)) { [void]$system.Add("cpu: " + $cpu.Name) }
                foreach ($gpu in @(Get-CimInstance Win32_VideoController)) { [void]$system.Add(("gpu: {0}, driver {1}" -f $gpu.Name, $gpu.DriverVersion)) }
            } catch { [void]$system.Add("hardware details unavailable: " + $_.Exception.Message) }
        }
    }

    $report = New-Object System.Collections.Generic.List[string]
    [void]$report.Add("RFAB Online report, collector version $CollectorVersion")
    [void]$report.Add("game root: $Root")
    [void]$report.Add("client: $clientDir")
    [void]$report.Add("")
    [void]$report.Add("[system]")
    foreach ($line in $system) { [void]$report.Add($line) }
    [void]$report.Add("")
    [void]$report.Add("[binaries]")
    foreach ($line in $script:Binaries) { [void]$report.Add($line) }
    [void]$report.Add("")
    [void]$report.Add("[files]")
    foreach ($line in $script:Manifest) { [void]$report.Add($line) }
    [void]$report.Add("")
    [void]$report.Add("[notes]")
    foreach ($line in $script:Notes) { [void]$report.Add($line) }
    [System.IO.File]::WriteAllLines((Join-Path $script:Stage "REPORT.txt"), $report, $Utf8NoBom)

    if (-not (Test-Path -LiteralPath $TargetDir)) { New-Item -ItemType Directory -Path $TargetDir -Force | Out-Null }
    $zipPath = Join-Path $TargetDir ("RFAB_Report_" + $stamp + ".zip")
    if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
    Add-Type -AssemblyName System.IO.Compression.FileSystem | Out-Null
    [System.IO.Compression.ZipFile]::CreateFromDirectory($script:Stage, $zipPath, [System.IO.Compression.CompressionLevel]::Optimal, $false)
    Remove-Item -LiteralPath $script:Stage -Recurse -Force -ErrorAction SilentlyContinue
    return $zipPath
}

function Invoke-SelfTest {
    $root = Join-Path ([System.IO.Path]::GetTempPath()) ("rfab_collect_selftest_" + [Guid]::NewGuid().ToString("N"))
    $failures = New-Object System.Collections.Generic.List[string]
    try {
        $client = Join-Parts $root @("MO2", "mods", "RFAB_Online", "SkyrimTogetherReborn")
        New-Item -ItemType Directory -Path (Join-Path $client "logs") -Force | Out-Null
        New-Item -ItemType Directory -Path (Join-Path $client "rfab_coop_config") -Force | Out-Null
        New-Item -ItemType Directory -Path (Join-Parts $root @("MO2", "profiles", "RFAB")) -Force | Out-Null
        New-Item -ItemType Directory -Path (Join-Parts $root @("MO2", "overwrite", "NetScriptFramework", "Crash")) -Force | Out-Null
        New-Item -ItemType Directory -Path (Join-Parts $root @("RFAB_Online_Server", "logs")) -Force | Out-Null
        New-Item -ItemType Directory -Path (Join-Parts $root @("RFAB_Online_Server", "config")) -Force | Out-Null

        # A minimal PE header: "MZ", e_lfanew = 0x40, "PE\0\0", machine, sections, TimeDateStamp 0x6736A5C1.
        $pe = New-Object byte[] 0x60
        $pe[0] = 0x4D; $pe[1] = 0x5A; $pe[0x3C] = 0x40
        $pe[0x40] = 0x50; $pe[0x41] = 0x45
        $pe[0x44] = 0x64; $pe[0x45] = 0x86
        $pe[0x48] = 0xC1; $pe[0x49] = 0xA5; $pe[0x4A] = 0x36; $pe[0x4B] = 0x67
        [System.IO.File]::WriteAllBytes((Join-Path $client "SkyrimSE.exe"), $pe)

        $bigLog = New-Object System.Text.StringBuilder
        for ($i = 0; $i -lt 40000; $i++) { [void]$bigLog.Append("line $i of the client log, long enough to exceed the cap`n") }
        [void]$bigLog.Append("THE LAST LINE`n")
        [System.IO.File]::WriteAllText((Join-Parts $client @("logs", "tp_client.log")), $bigLog.ToString(), $Utf8NoBom)
        [System.IO.File]::WriteAllText((Join-Path $client "RFAB_Online_Crash.log"), "[UNHANDLED_CRASH] sample`n", $Utf8NoBom)
        [System.IO.File]::WriteAllBytes((Join-Path $client "RFAB_Online_Symbols.gz"), [byte[]](0x1F, 0x8B, 0x08, 0x00))
        [System.IO.File]::WriteAllBytes((Join-Path $client "crash_2026-11-14_10-00-00.dmp"), [byte[]](0x4D, 0x44, 0x4D, 0x50))
        [System.IO.File]::WriteAllText((Join-Parts $client @("rfab_coop_config", "coop_config_report.txt")), "overlay report`n", $Utf8NoBom)
        [System.IO.File]::WriteAllText((Join-Path $root "RFAB_Online_Startup.log"), "startup`n", $Utf8NoBom)
        [System.IO.File]::WriteAllText((Join-Parts $root @("MO2", "ModOrganizer.ini")), "[General]`r`nselected_profile=@ByteArray(RFAB)`r`n", $Utf8NoBom)
        [System.IO.File]::WriteAllText((Join-Parts $root @("MO2", "profiles", "RFAB", "plugins.txt")), "*RFAB.esp`r`n", $Utf8NoBom)
        [System.IO.File]::WriteAllText((Join-Parts $root @("MO2", "overwrite", "NetScriptFramework", "Crash", "Crash_2026_11_14.txt")), "nsf crash`n", $Utf8NoBom)
        [System.IO.File]::WriteAllText((Join-Parts $root @("RFAB_Online_Server", "logs", "STServerOut.log")), "server log`n", $Utf8NoBom)
        [System.IO.File]::WriteAllText((Join-Parts $root @("RFAB_Online_Server", "config", "STServer.ini")),
            "[GameServer]`r`nsPassword = hunter2`r`nsAdminPassword = `r`nuPort = 10578`r`n; sPassword = commented`r`n", $Utf8NoBom)

        $outDir = Join-Path $root "out"
        $zip = Invoke-Collect $root $outDir 1 $true

        if (-not (Test-Path -LiteralPath $zip)) { [void]$failures.Add("no zip was written") }
        else {
            Add-Type -AssemblyName System.IO.Compression.FileSystem | Out-Null
            $archive = [System.IO.Compression.ZipFile]::OpenRead($zip)
            try {
                $entries = @{}
                foreach ($entry in $archive.Entries) { $entries[$entry.FullName.Replace("\", "/")] = $entry }
                foreach ($expected in @(
                        "REPORT.txt",
                        "client/logs/tp_client.log",
                        "client/RFAB_Online_Crash.log",
                        "client/RFAB_Online_Symbols.gz",
                        "client/crash_2026-11-14_10-00-00.dmp",
                        "client/rfab_coop_config/coop_config_report.txt",
                        "game/RFAB_Online_Startup.log",
                        "server/logs/STServerOut.log",
                        "server/config/STServer.ini",
                        "netscriptframework/Crash_2026_11_14.txt",
                        "mo2_profile/RFAB/plugins.txt")) {
                    if (-not $entries.ContainsKey($expected)) { [void]$failures.Add("missing in the zip: $expected") }
                }
                $readEntry = {
                    param($name)
                    if (-not $entries.ContainsKey($name)) { return "" }
                    $reader = New-Object System.IO.StreamReader($entries[$name].Open(), [System.Text.Encoding]::UTF8)
                    try { return $reader.ReadToEnd() } finally { $reader.Dispose() }
                }
                $reportText = & $readEntry "REPORT.txt"
                if ($reportText -notmatch 'client\\SkyrimSE\.exe \| size=96 \| .*pe_timestamp=6736a5c1') { [void]$failures.Add("REPORT.txt has no PE timestamp line for the client: " + $reportText) }
                if ($reportText -notmatch "MO2 selected profile: 'RFAB'") { [void]$failures.Add("the selected MO2 profile was not recognised") }
                $iniText = & $readEntry "server/config/STServer.ini"
                if ($iniText -match "hunter2") { [void]$failures.Add("a password leaked into the report") }
                if ($iniText -notmatch "sPassword = <hidden>") { [void]$failures.Add("the password line was not replaced") }
                if ($iniText -notmatch "(?m)^sAdminPassword = \s*$") { [void]$failures.Add("an empty password must stay empty") }
                if ($iniText -notmatch "uPort = 10578") { [void]$failures.Add("ordinary settings must stay readable") }
                $logText = & $readEntry "client/logs/tp_client.log"
                if ($logText.Length -gt (1MB + 16)) { [void]$failures.Add("the log was not capped: " + $logText.Length) }
                if (-not $logText.TrimEnd().EndsWith("THE LAST LINE")) { [void]$failures.Add("a capped log must keep its end") }
            } finally { $archive.Dispose() }
        }
        if ((Hide-Secrets "sToken=abc") -ne "sToken=<hidden>") { [void]$failures.Add("Hide-Secrets: token") }
        if ((Hide-Secrets "uPort = 1") -ne "uPort = 1") { [void]$failures.Add("Hide-Secrets: ordinary key") }
    } catch {
        [void]$failures.Add("exception: " + $_.Exception.Message + " @ " + $_.ScriptStackTrace)
    } finally {
        Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
    }
    if ($failures.Count -gt 0) {
        Write-Host "Collect-RfabReport self-test FAILED:"
        foreach ($failure in $failures) { Write-Host "  - $failure" }
        return 1
    }
    Write-Host "[RFAB_RESULT] report collector self-test: 11 files collected, secrets hidden, log capped (PowerShell $($PSVersionTable.PSVersion))"
    return 0
}

if ($SelfTest) {
    exit (Invoke-SelfTest)
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Find-GameRoot $GameRoot $scriptDir (-not $NoUi)
if ([string]::IsNullOrWhiteSpace($root)) {
    Write-Host "RFAB folder not found. Run again and pick the folder that contains MO2\mods\RFAB_Online," -ForegroundColor Red
    Write-Host "or pass it:  Collect-RfabReport.ps1 -GameRoot ""D:\Games\RFAB""" -ForegroundColor Red
    exit 1
}
if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = [Environment]::GetFolderPath("Desktop")
    if ([string]::IsNullOrWhiteSpace($OutDir)) { $OutDir = $root }
}

Write-Host "==> RFAB: $root" -ForegroundColor Cyan
$zipPath = Invoke-Collect $root $OutDir $MaxLogMB (-not $NoDumps)
$sizeMb = [math]::Round((Get-Item -LiteralPath $zipPath).Length / 1MB, 1)
Write-Host ""
Write-Host "Report: $zipPath ($sizeMb MB)" -ForegroundColor Green
foreach ($note in $script:Notes) { Write-Host "  note: $note" -ForegroundColor DarkGray }
Write-Host ""
Write-Host "Attach this file to your report, and write what you were doing and at what time." -ForegroundColor Cyan
Write-Host "Each player runs this on their own PC: every client has its own logs." -ForegroundColor Cyan
if (-not $NoUi) {
    try { Start-Process -FilePath "explorer.exe" -ArgumentList ("/select,""" + $zipPath + """") } catch { }
}
exit 0
