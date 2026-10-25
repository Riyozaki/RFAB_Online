param(
    [string]$VersionTag = "v1.2.0",
    [string]$BuildMode = "releasedbg"
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

function Invoke-RobocopySafe([string]$Src, [string]$Dst, [string[]]$ExtraArgs = @("/E")) {
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & robocopy ($Src.TrimEnd('\')) ($Dst.TrimEnd('\')) @ExtraArgs /NFL /NDL /NJH /NJS /NP /R:2 /W:1 | Out-Null
    $rc = $LASTEXITCODE
    $ErrorActionPreference = $prevEap
    $global:LASTEXITCODE = 0
    if ($rc -ge 8) {
        throw "robocopy failed ($rc): '$Src' -> '$Dst'"
    }
}

$RepoRoot = [System.IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) "..\.."))
Set-Location -LiteralPath $RepoRoot

$DistribBin = Join-Path $RepoRoot "distrib\bin"
$BuildRel   = Join-Path $RepoRoot "build\windows\x64\$BuildMode"
$GameFiles  = Join-Path $RepoRoot "GameFiles\Skyrim"

if (-not (Test-Path -LiteralPath $DistribBin)) {
    throw "distrib\bin not found at $DistribBin! Run 'xmake install -o distrib' first."
}

$OutDir     = Join-Path $RepoRoot "release_artifacts"
$StageOuter = Join-Path $RepoRoot "release_stage"
$PkgName    = "RFAB_Online_${VersionTag}"
$StageRoot  = Join-Path $StageOuter $PkgName

if (Test-Path -LiteralPath $OutDir)     { Remove-Item -LiteralPath $OutDir -Recurse -Force }
if (Test-Path -LiteralPath $StageOuter) { Remove-Item -LiteralPath $StageOuter -Recurse -Force }

$StageMo2    = Join-Path $StageRoot "RFAB_Online_MO2"
$StageClient = Join-Path $StageMo2  "SkyrimTogetherReborn"
$StageServer = Join-Path $StageRoot "RFAB_Online_Server"
$StageTools  = Join-Path $StageRoot "Tools\rfab"
$PdbDir      = Join-Path $StageOuter "PDB_Symbols"

New-Item -ItemType Directory -Path $StageClient, $StageServer, $StageTools, $PdbDir, $OutDir -Force | Out-Null

Write-Host "==> [1/6] Staging GameFiles/Skyrim & RFAB_Online_Bindings.json..." -ForegroundColor Cyan
$PrebuiltMo2 = Join-Path $RepoRoot "dist_ready\RFAB_Online_MO2"
if (Test-Path -LiteralPath $PrebuiltMo2) {
    Invoke-RobocopySafe -Src $PrebuiltMo2 -Dst $StageMo2 -ExtraArgs @("/E")
}
Invoke-RobocopySafe -Src $GameFiles -Dst $StageMo2 -ExtraArgs @("/E")
New-Item -ItemType Directory -Path (Join-Path $StageMo2 "SKSE\Plugins") -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $RepoRoot "Data\SKSE\Plugins\RFAB_Online_Bindings.json") `
          -Destination (Join-Path $StageMo2 "SKSE\Plugins\RFAB_Online_Bindings.json") -Force

Write-Host "==> [2/6] Staging Client binaries, CEF, Discord SDK, Cursors & Offline UI..." -ForegroundColor Cyan
Invoke-RobocopySafe -Src $DistribBin -Dst $StageClient -ExtraArgs @("/E", "/XF", "SkyrimTogetherServer.exe", "STServer.dll", "*Tests.exe", "*.pdb", "*.lib", "*.exp")

foreach ($binName in @("SkyrimSE.exe", "EarlyLoad.dll", "TPProcess.exe")) {
    $dstBin = Join-Path $StageClient $binName
    if (-not (Test-Path -LiteralPath $dstBin)) {
        $found = Get-ChildItem -LiteralPath $BuildRel -Filter $binName -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($found) { Copy-Item -LiteralPath $found.FullName -Destination $dstBin -Force }
    }
}
Copy-Item -LiteralPath (Join-Path $StageClient "SkyrimSE.exe") `
          -Destination (Join-Path $StageClient "SkyrimTogether.exe") -Force

# Always stage our custom compiled RFAB Online Angular CEF UI bundle (with «Единство в битве» Right-Ctrl toggle)
# over any upstream xmake skyrim-together-ui package files in $DistribBin\UI!
$CustomUi = Join-Path $RepoRoot "dist_ready\RFAB_Online_MO2\SkyrimTogetherReborn\UI"
if (Test-Path -LiteralPath (Join-Path $CustomUi "index.html")) {
    Invoke-RobocopySafe -Src $CustomUi -Dst (Join-Path $StageClient "UI") -ExtraArgs @("/MIR")
}

Write-Host "==> [3/6] Staging Standalone Dedicated Server (RFAB_Online_Server)..." -ForegroundColor Cyan
foreach ($srvBin in @("SkyrimTogetherServer.exe", "STServer.dll")) {
    $cand1 = Join-Path $DistribBin $srvBin
    if (Test-Path -LiteralPath $cand1) {
        Copy-Item -LiteralPath $cand1 -Destination (Join-Path $StageServer $srvBin) -Force
    } else {
        $foundSrv = Get-ChildItem -LiteralPath $BuildRel -Filter $srvBin -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $foundSrv) { throw "Missing compiled server binary: $srvBin" }
        Copy-Item -LiteralPath $foundSrv.FullName -Destination (Join-Path $StageServer $srvBin) -Force
    }
}

$ClientOnlyDlls = @(
    "EarlyLoad.dll", "libcef.dll", "chrome_elf.dll", "discord_game_sdk.dll",
    "d3dcompiler_47.dll", "dxcompiler.dll", "dxil.dll", "libEGL.dll",
    "libGLESv2.dll", "vk_swiftshader.dll", "vulkan-1.dll"
)
Get-ChildItem -LiteralPath $DistribBin -Filter "*.dll" -File | Where-Object {
    $_.Name -notin $ClientOnlyDlls -and $_.Name -ne "STServer.dll"
} | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $StageServer $_.Name) -Force
}

New-Item -ItemType Directory -Path (Join-Path $StageServer "config"), (Join-Path $StageServer "Data") -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $RepoRoot "dist_ready\RFAB_Online_Server\config\STServer.ini") `
          -Destination (Join-Path $StageServer "config\STServer.ini") -Force
$EulaText = ";Please indicate your agreement to the Tilted platform service agreement`r`n[EULA]`r`nbConfirmEULA=true`r`n"
[System.IO.File]::WriteAllText((Join-Path $StageServer "config\EULA.txt"), $EulaText, (New-Object System.Text.UTF8Encoding($false)))
$DirectSrvBat = "@echo off`r`nchcp 65001 >nul`r`ntitle RFAB Online Dedicated Server (Port 10578)`r`ncd /d `"%~dp0`"`r`nset TILTED_ACCEPT_EULA=true`r`nSkyrimTogetherServer.exe`r`npause`r`n"
[System.IO.File]::WriteAllText((Join-Path $StageServer "START_SERVER.bat"), $DirectSrvBat, [System.Text.Encoding]::ASCII)

Copy-Item -LiteralPath (Join-Path $RepoRoot "Tools\rfab\Setup-RfabOnline.ps1") `
          -Destination (Join-Path $StageTools "Setup-RfabOnline.ps1") -Force
Copy-Item -LiteralPath (Join-Path $RepoRoot "INSTALL_RFAB_ONLINE.bat") `
          -Destination (Join-Path $StageRoot "INSTALL_RFAB_ONLINE.bat") -Force
Copy-Item -LiteralPath (Join-Path $RepoRoot "PROCHTI_MENYA.txt") `
          -Destination (Join-Path $StageRoot "PROCHTI_MENYA.txt") -Force

Write-Host "==> [4/6] Running PE64 Transitive DLL Import Auto-Bundler & Integrity Gate..." -ForegroundColor Cyan
$XmakePkgDir = Join-Path $env:LOCALAPPDATA ".xmake\packages"
$ErrorActionPreference = "Continue"
& python (Join-Path $RepoRoot "Tools\rfab\verify_release_bundle.py") $StageRoot $DistribBin $BuildRel $XmakePkgDir
$pyCode = $LASTEXITCODE
$ErrorActionPreference = "Stop"
if ($pyCode -ne 0) { throw "verify_release_bundle.py failed ($pyCode)!" }

Write-Host "==> [5/6] Live Windows x64 Server Smoke-Boot & Clean-Wipe Installer Verification..." -ForegroundColor Cyan
$SmokeDir = Join-Path $env:TEMP ("rfab_srv_smoke_" + [Guid]::NewGuid().ToString("N"))
Invoke-RobocopySafe -Src $StageServer -Dst $SmokeDir -ExtraArgs @("/E")
[System.IO.File]::WriteAllText((Join-Path $SmokeDir "Data\loadorder.txt"), "Skyrim.esm`nSkyrimTogether.esp`n", (New-Object System.Text.UTF8Encoding($false)))
$env:TILTED_ACCEPT_EULA = "true"
$proc = Start-Process -FilePath (Join-Path $SmokeDir "SkyrimTogetherServer.exe") `
                      -WorkingDirectory $SmokeDir `
                      -RedirectStandardOutput (Join-Path $SmokeDir "stdout.txt") `
                      -RedirectStandardError  (Join-Path $SmokeDir "stderr.txt") `
                      -PassThru -NoNewWindow
Start-Sleep -Seconds 4
if ($proc.HasExited) {
    $so = Get-Content -LiteralPath (Join-Path $SmokeDir "stdout.txt") -Raw -ErrorAction SilentlyContinue
    $se = Get-Content -LiteralPath (Join-Path $SmokeDir "stderr.txt") -Raw -ErrorAction SilentlyContinue
    throw "SkyrimTogetherServer.exe exited prematurely with code $($proc.ExitCode)! STDOUT: $so | STDERR: $se"
}
Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $SmokeDir -Recurse -Force -ErrorAction SilentlyContinue
Write-Host "  [OK] Server booted cleanly and stayed alive on clean Windows x64!" -ForegroundColor Green

# End-to-end test of Setup-RfabOnline.ps1 over a simulated dirty old RFAB + MO2 directory
$MockRfab = Join-Path $env:TEMP ("rfab_clean_wipe_e2e_" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path `
    (Join-Path $MockRfab "Data\SKSE\Plugins"), `
    (Join-Path $MockRfab "MO2\mods\RFAB_Online"), `
    (Join-Path $MockRfab "MO2\mods\RFAB_Online_MO2"), `
    (Join-Path $MockRfab "MO2\mods\EngineFixes\SKSE\Plugins"), `
    (Join-Path $MockRfab "MO2\mods\Souls\SKSE\Plugins"), `
    (Join-Path $MockRfab "MO2\mods\Autosave\SKSE\Plugins"), `
    (Join-Path $MockRfab "MO2\mods\NSF\NetScriptFramework\Plugins"), `
    (Join-Path $MockRfab "MO2\profiles\RFAB") -Force | Out-Null

$DummyExe = Join-Path $MockRfab "SkyrimSE.exe"
$CsCode = @'
using System.Reflection;
[assembly: AssemblyVersion("1.0.0.0")]
[assembly: AssemblyFileVersion("1.0.0.0")]
[assembly: AssemblyInformationalVersion("1.0.0.0")]
public class Program { public static void Main() {} }
'@
Add-Type -TypeDefinition $CsCode -OutputAssembly $DummyExe -OutputType ConsoleApplication
[System.IO.File]::Copy($DummyExe, (Join-Path $MockRfab "SkyrimSE.cdx"), $true)
[System.IO.File]::WriteAllBytes((Join-Path $MockRfab "skse64_1_5_97.dll"), [byte[]](0x4D, 0x5A, 0x00, 0x00))

[System.IO.File]::WriteAllBytes((Join-Path $MockRfab "MO2\ModOrganizer.exe"), [byte[]](0x4D, 0x5A, 0x00, 0x00))
[System.IO.File]::WriteAllText((Join-Path $MockRfab "MO2\mods\RFAB_Online\STALE_LEFTOVER_FROM_OLD_TEST.txt"), "must be wiped")
[System.IO.File]::WriteAllText((Join-Path $MockRfab "MO2\mods\RFAB_Online_MO2\STALE_LEGACY_DIR.txt"), "must be wiped")
[System.IO.File]::WriteAllText((Join-Path $MockRfab "MO2\mods\EngineFixes\SKSE\Plugins\EngineFixes.toml"), "[Patches]`r`nMemoryManager = true`r`nRegularQuicksaves = false`r`n[Experimental]`r`nS = a`r`nt = r`r`nSaveGameMaxSize = true`r`n")
New-Item -ItemType Directory -Path (Join-Path $MockRfab "MO2\profiles\RFAB\saves") -Force | Out-Null
$MockTruncEss = New-Object byte[] 512
[System.Text.Encoding]::ASCII.GetBytes("TESV_SAVEGAME").CopyTo($MockTruncEss, 0)
[System.IO.File]::WriteAllBytes((Join-Path $MockRfab "MO2\profiles\RFAB\saves\TruncatedScrapHeap.ess"), $MockTruncEss)
[System.IO.File]::WriteAllText((Join-Path $MockRfab "MO2\mods\Souls\SKSE\Plugins\SkyrimSoulsRE.ini"), "[SLOWMOTION]`r`nfSlowMotionMultiplier = 0.200000`r`nbEnableSlowMotionInventoryMenu = true`r`n")
[System.IO.File]::WriteAllText((Join-Path $MockRfab "MO2\mods\Autosave\SKSE\Plugins\[RFAB] Autosave.ini"), "[Main]`r`nEnable autosave = 1`r`nSave after combat = 1`r`n")
[System.IO.File]::WriteAllText((Join-Path $MockRfab "MO2\mods\NSF\NetScriptFramework\Plugins\BetterStealing.dll"), "stub")
[System.IO.File]::WriteAllText((Join-Path $MockRfab "MO2\ModOrganizer.ini"), "[General]`r`nselected_profile=@ByteArray(RFAB)`r`n")
[System.IO.File]::WriteAllText((Join-Path $MockRfab "MO2\profiles\RFAB\modlist.txt"), "-RFAB_Online`r`n+Requiem`r`n")
[System.IO.File]::WriteAllText((Join-Path $MockRfab "MO2\profiles\RFAB\plugins.txt"), "*Requiem.esp`r`n*[RFAB] Balance.esp`r`n")
[System.IO.File]::WriteAllText((Join-Path $MockRfab "MO2\profiles\RFAB\loadorder.txt"), "Skyrim.esm`r`nUpdate.esm`r`nDawnguard.esm`r`nHearthFires.esm`r`nDragonborn.esm`r`nRequiem.esp`r`n[RFAB] Balance.esp`r`n")

$ErrorActionPreference = "Continue"
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $StageTools "Setup-RfabOnline.ps1") -RfabDir $MockRfab -NonInteractive
$e2eCode = $LASTEXITCODE
$ErrorActionPreference = "Stop"
if ($e2eCode -ne 0) { throw "End-to-end Setup-RfabOnline.ps1 test failed ($e2eCode)!" }

if (Test-Path -LiteralPath (Join-Path $MockRfab "MO2\mods\RFAB_Online\STALE_LEFTOVER_FROM_OLD_TEST.txt")) {
    throw "Clean wipe failed: old file inside MO2\mods\RFAB_Online survived!"
}
if (Test-Path -LiteralPath (Join-Path $MockRfab "MO2\mods\RFAB_Online_MO2")) {
    throw "Clean wipe failed: legacy MO2\mods\RFAB_Online_MO2 folder survived!"
}
if (-not (Test-Path -LiteralPath (Join-Path $MockRfab "MO2\mods\RFAB_Online\SkyrimTogetherReborn\SkyrimSE.exe"))) {
    throw "Installed SkyrimSE.exe missing in MO2\mods\RFAB_Online\SkyrimTogetherReborn!"
}
if (-not (Test-Path -LiteralPath (Join-Path $MockRfab "RFAB_Online_Server\SkyrimTogetherServer.exe"))) {
    throw "Standalone server missing in <RfabDir>\RFAB_Online_Server!"
}
if (-not (Test-Path -LiteralPath (Join-Path $MockRfab "START_RFAB_SERVER.bat"))) {
    throw "START_RFAB_SERVER.bat missing in <RfabDir>!"
}
$patchedEf = [System.IO.File]::ReadAllText((Join-Path $MockRfab "MO2\mods\EngineFixes\SKSE\Plugins\EngineFixes.toml"))
if ($patchedEf -notmatch 'MemoryManager\s*=\s*false' -or $patchedEf -notmatch 'FormCaching\s*=\s*false' -or $patchedEf -notmatch 'TreeLODReferenceCaching\s*=\s*false' -or $patchedEf -notmatch 'SaveGameMaxSize\s*=\s*false' -or $patchedEf -match '(?m)^\s*S\s*=\s*a') {
    throw "EngineFixes.toml was not healed to MemoryManager/FormCaching/TreeLODReferenceCaching/SaveGameMaxSize = false (or retained invalid 'S = a'):`n$patchedEf"
}
if (-not (Test-Path -LiteralPath (Join-Path $MockRfab "MO2\profiles\RFAB\saves\Quarantined_Corrupt_v100\TruncatedScrapHeap.ess"))) {
    throw "TruncatedScrapHeap.ess was not quarantined during Setup-RfabOnline.ps1!"
}
$patchedAutosave = [System.IO.File]::ReadAllText((Join-Path $MockRfab "MO2\mods\Autosave\SKSE\Plugins\[RFAB] Autosave.ini"))
if ($patchedAutosave -notmatch 'Save after combat\s*=\s*0') {
    throw "[RFAB] Autosave.ini was not patched (PowerShell bracket wildcard regression)!"
}
Remove-Item -LiteralPath $MockRfab -Recurse -Force -ErrorAction SilentlyContinue
Write-Host "  [OK] End-to-end clean-wipe installer test passed 100%!" -ForegroundColor Green

Write-Host "==> [6/6] Compressing Release ZIP Archives & SHA256 Checksums..." -ForegroundColor Cyan
Get-ChildItem -LiteralPath $BuildRel -Filter "*.pdb" -Recurse -File -ErrorAction SilentlyContinue | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $PdbDir $_.Name) -Force
}

Add-Type -AssemblyName System.IO.Compression.FileSystem | Out-Null
$InstallerZip = Join-Path $OutDir "RFAB_Online_${VersionTag}_Full_Installer.zip"
$QuickUpdZip  = Join-Path $OutDir "RFAB_Online_${VersionTag}_Quick_Update.zip"
$PdbZip       = Join-Path $OutDir "RFAB_Online_${VersionTag}_PDB_Symbols.zip"

$QuickDir = Join-Path $StageOuter "Quick_Update"
$QuickSrvDir = Join-Path $QuickDir "Server"
New-Item -ItemType Directory -Path $QuickDir, $QuickSrvDir -Force | Out-Null
foreach ($qf in @("SkyrimSE.exe", "SkyrimTogether.exe", "EarlyLoad.dll", "TPProcess.exe")) {
    Copy-Item -LiteralPath (Join-Path $StageClient $qf) -Destination (Join-Path $QuickDir $qf) -Force
}
Copy-Item -LiteralPath (Join-Path $StageMo2 "SkyrimTogether.esp") -Destination (Join-Path $QuickDir "SkyrimTogether.esp") -Force
if (Test-Path -LiteralPath (Join-Path $StageMo2 "SkyrimTogetherQuestPatches.esp")) {
    Copy-Item -LiteralPath (Join-Path $StageMo2 "SkyrimTogetherQuestPatches.esp") -Destination (Join-Path $QuickDir "SkyrimTogetherQuestPatches.esp") -Force
}
Copy-Item -LiteralPath (Join-Path $StageMo2 "SKSE\Plugins\RFAB_Online_Bindings.json") -Destination (Join-Path $QuickDir "RFAB_Online_Bindings.json") -Force
Invoke-RobocopySafe -Src (Join-Path $StageClient "UI") -Dst (Join-Path $QuickDir "UI") -ExtraArgs @("/MIR")
foreach ($sf in @("SkyrimTogetherServer.exe", "STServer.dll")) {
    Copy-Item -LiteralPath (Join-Path $StageServer $sf) -Destination (Join-Path $QuickSrvDir $sf) -Force
}

$QuickUpdatePs1 = @'
$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$SrcDir = Split-Path -Parent $MyInvocation.MyCommand.Path

$GameRoot = $null
$RegKey = "HKCU:\Software\TiltedPhoques\TiltedEvolution\Skyrim Special Edition"
if (Test-Path -LiteralPath $RegKey) {
    $GameRoot = (Get-ItemProperty -LiteralPath $RegKey -Name "TitlePath" -ErrorAction SilentlyContinue).TitlePath
}
if ([string]::IsNullOrWhiteSpace($GameRoot) -or -not (Test-Path -LiteralPath $GameRoot)) {
    Add-Type -AssemblyName System.Windows.Forms | Out-Null
    $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
    $dlg.Description = "Select your RFAB root folder (e.g. G:\RFAB Online SkyrimSE)"
    if ($dlg.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { exit 1 }
    $GameRoot = $dlg.SelectedPath
}

Write-Host "==> RFAB root: $GameRoot" -ForegroundColor Cyan

# Stop running old server / orphaned TPProcess so DLLs & EXEs can be overwritten cleanly
$runningSrv = @(Get-Process -Name "SkyrimTogetherServer" -ErrorAction SilentlyContinue)
if ($runningSrv.Count -gt 0) {
    $runningSrv | Stop-Process -Force -ErrorAction SilentlyContinue
    $runningSrv | Wait-Process -Timeout 5 -ErrorAction SilentlyContinue
}
if (-not (Get-Process -Name "SkyrimSE" -ErrorAction SilentlyContinue) -and
    -not (Get-Process -Name "SkyrimTogether" -ErrorAction SilentlyContinue)) {
    $orphanTp = @(Get-Process -Name "TPProcess" -ErrorAction SilentlyContinue)
    if ($orphanTp.Count -gt 0) {
        $orphanTp | Stop-Process -Force -ErrorAction SilentlyContinue
        $orphanTp | Wait-Process -Timeout 5 -ErrorAction SilentlyContinue
    }
}
Start-Sleep -Milliseconds 400

$ModRoot = Join-Path $GameRoot "MO2\mods\RFAB_Online"
$ClientDst = Join-Path $ModRoot "SkyrimTogetherReborn"
if (-not (Test-Path -LiteralPath $ClientDst)) {
    throw "Client folder not found: $ClientDst (run INSTALL_RFAB_ONLINE.bat from Full_Installer first!)"
}
foreach ($cf in @("SkyrimSE.exe", "SkyrimTogether.exe", "EarlyLoad.dll", "TPProcess.exe")) {
    if (Test-Path -LiteralPath (Join-Path $SrcDir $cf)) {
        Copy-Item -LiteralPath (Join-Path $SrcDir $cf) -Destination (Join-Path $ClientDst $cf) -Force
    }
}
foreach ($espName in @("SkyrimTogether.esp", "SkyrimTogetherQuestPatches.esp")) {
    if (Test-Path -LiteralPath (Join-Path $SrcDir $espName)) {
        Copy-Item -LiteralPath (Join-Path $SrcDir $espName) -Destination (Join-Path $ModRoot $espName) -Force
    }
}
if (Test-Path -LiteralPath (Join-Path $SrcDir "RFAB_Online_Bindings.json")) {
    New-Item -ItemType Directory -Path (Join-Path $ModRoot "SKSE\Plugins") -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $SrcDir "RFAB_Online_Bindings.json") -Destination (Join-Path $ModRoot "SKSE\Plugins\RFAB_Online_Bindings.json") -Force
}
foreach ($badRel in @(
    "scripts\TempleBlessingScript.pex",
    "scripts\C00TrainerScript.pex",
    "scripts\ResourceFurnitureScript.pex",
    "scripts\PetFramework_ParentQuestScript.pex",
    "scripts\petframework_petquest.pex",
    "scripts\source\TempleBlessingScript.psc",
    "scripts\source\C00TrainerScript.psc",
    "scripts\source\ResourceFurnitureScript.psc",
    "scripts\source\PetFramework_ParentQuestScript.psc",
    "scripts\source\PetFramework_PetQuest.psc",
    "meshes\Interface\logo\logo.nif",
    "meshes\Interface\logo\logo01ae.nif"
)) {
    $badPath = Join-Path $ModRoot $badRel
    if (Test-Path -LiteralPath $badPath) { Remove-Item -LiteralPath $badPath -Force -ErrorAction SilentlyContinue }
}
if (Test-Path -LiteralPath (Join-Path $SrcDir "UI")) {
    Remove-Item -LiteralPath (Join-Path $ClientDst "UI") -Recurse -Force -ErrorAction SilentlyContinue
    Copy-Item -LiteralPath (Join-Path $SrcDir "UI") -Destination (Join-Path $ClientDst "UI") -Recurse -Force
}
Get-ChildItem -LiteralPath (Join-Path $GameRoot "MO2") -Filter "EngineFixes.toml" -Recurse -ErrorAction SilentlyContinue | ForEach-Object {
    $rawEf = [System.IO.File]::ReadAllText($_.FullName, [System.Text.Encoding]::UTF8)
    $updEf = [regex]::Replace($rawEf, '(?m)^\s*(?:S\s*=\s*a|t\s*=\s*r|f\s*=\s*a)\s*\r?\n?', '')
    $updEf = [regex]::Replace($updEf, '(?mi)^\s*\[SaveGameMaxSize\]\s*$', '[Experimental]')
    foreach ($pair in @(@("MemoryManager","false"),@("ScaleformAllocator","false"),@("FormCaching","false"),@("TreeLODReferenceCaching","false"),@("RegularQuicksaves","true"),@("MaxStdio","8192"),@("SaveGameMaxSize","false"))) {
        $k = $pair[0]; $v = $pair[1]
        $updEf = [regex]::Replace($updEf, "(?mi)^(\s*$([regex]::Escape($k))\s*=\s*)([^\r\n;#]+)(.*)$", "`${1}$v`${3}")
    }
    $nlEf = if ($updEf.Contains("`r`n")) { "`r`n" } else { "`n" }
    foreach ($reqPatchPair in @(@("FormCaching","false"),@("TreeLODReferenceCaching","false"))) {
        $rk = $reqPatchPair[0]; $rv = $reqPatchPair[1]
        if ($updEf -notmatch "(?mi)^\s*$([regex]::Escape($rk))\s*=") {
            if ($updEf -match '(?mi)^\s*\[Patches\]') {
                $updEf = [regex]::Replace($updEf, '(?mi)^(\s*\[Patches\][^\r\n]*\r?\n)', "`${1}$rk = $rv$nlEf")
            } else {
                $updEf = $updEf.TrimEnd() + "$nlEf[Patches]$nlEf$rk = $rv$nlEf"
            }
        }
    }
    if ($updEf -notmatch '(?mi)^\s*SaveGameMaxSize\s*=') {
        if ($updEf -match '(?mi)^\s*\[Experimental\]') {
            $updEf = [regex]::Replace($updEf, '(?mi)^(\s*\[Experimental\][^\r\n]*\r?\n)', "`${1}SaveGameMaxSize = false$nlEf")
        } else {
            $updEf = $updEf.TrimEnd() + "$nlEf[Experimental]$nlEf" + "SaveGameMaxSize = false$nlEf"
        }
    }
    if ($updEf -ne $rawEf) { [System.IO.File]::WriteAllText($_.FullName, $updEf, (New-Object System.Text.UTF8Encoding($false))) }
}
Get-ChildItem -LiteralPath (Join-Path $GameRoot "MO2") -Filter "*Autosave.ini" -Recurse -ErrorAction SilentlyContinue | Where-Object { $_.Name -ieq "[RFAB] Autosave.ini" } | ForEach-Object {
    $rawAs = [System.IO.File]::ReadAllText($_.FullName, [System.Text.Encoding]::UTF8)
    $updAs = $rawAs
    foreach ($ak in @("Enable autosave","Save after combat","Save on location change","Save on sleep/wait")) {
        $updAs = [regex]::Replace($updAs, "(?mi)^(\s*$([regex]::Escape($ak))\s*=\s*)([^\r\n;#]+)(.*)$", "`${1}0`${3}")
    }
    if ($updAs -ne $rawAs) { [System.IO.File]::WriteAllText($_.FullName, $updAs, (New-Object System.Text.UTF8Encoding($false))) }
}

function Test-ValidSkyrimEssQuick([string]$EssPath) {
    try {
        $fi = New-Object System.IO.FileInfo($EssPath)
        if (-not $fi.Exists -or $fi.Length -lt 256) { return $false }
        $fs = [System.IO.File]::Open($EssPath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
        try {
            $br = New-Object System.IO.BinaryReader($fs)
            if ([System.Text.Encoding]::ASCII.GetString($br.ReadBytes(13)) -cne "TESV_SAVEGAME") { return $false }
            $hdrSize = [uint32]$br.ReadUInt32()
            if ($hdrSize -lt 36 -or $hdrSize -gt 65536 -or ([int64]17 + [int64]$hdrSize) -ge $fi.Length) { return $false }
            $hdr = $br.ReadBytes([int]$hdrSize)
            if ($hdr.Length -ne [int]$hdrSize) { return $false }
            $ver = [System.BitConverter]::ToUInt32($hdr, 0)
            if ($ver -lt 7 -or $ver -gt 15) { return $false }
            $pos = 8
            for ($s = 0; $s -lt 4; $s++) {
                if ($pos + 2 -gt $hdr.Length) { return $false }
                $pos += 2 + [int][System.BitConverter]::ToUInt16($hdr, $pos)
                if ($pos -gt $hdr.Length) { return $false }
                if ($s -eq 0) { $pos += 4; if ($pos -gt $hdr.Length) { return $false } }
            }
            $pos += 18
            if ($pos + 8 -gt $hdr.Length) { return $false }
            $ssW = [int64][System.BitConverter]::ToUInt32($hdr, $pos)
            $ssH = [int64][System.BitConverter]::ToUInt32($hdr, $pos + 4)
            $pos += 8
            if ($ssW -gt 16384 -or $ssH -gt 16384) { return $false }
            $compType = [uint16]0
            if ($ver -ge 12) {
                if ($pos + 2 -gt $hdr.Length) { return $false }
                $compType = [System.BitConverter]::ToUInt16($hdr, $pos); $pos += 2
                if ($compType -gt 2) { return $false }
            }
            if ($pos -ne $hdr.Length) { return $false }
            $bpp = if ($ver -ge 12) { [int64]4 } else { [int64]3 }
            $bodyOff = [int64]17 + [int64]$hdrSize + ($bpp * $ssW * $ssH)
            if ($fi.Length -le ($bodyOff + 16)) { return $false }
            if ($ver -ge 12 -and $compType -ne 0) {
                [void]$fs.Seek($bodyOff, [System.IO.SeekOrigin]::Begin)
                $uncompLen = [int64]$br.ReadUInt32(); $compLen = [int64]$br.ReadUInt32()
                if ($uncompLen -lt 64 -or $compLen -lt 16 -or ($bodyOff + 8 + $compLen) -gt $fi.Length) { return $false }
            } elseif ($fi.Length -lt ($bodyOff + 64)) { return $false }
            return $true
        } finally { $fs.Dispose() }
    } catch { return $false }
}

$DocsSaves = Join-Path ([Environment]::GetFolderPath("MyDocuments")) "My Games\Skyrim Special Edition\Saves"
$SaveDirsToScan = New-Object System.Collections.Generic.List[string]
if (Test-Path -LiteralPath $DocsSaves) {
    $SaveDirsToScan.Add($DocsSaves)
    Get-ChildItem -LiteralPath $DocsSaves -Directory -ErrorAction SilentlyContinue | Where-Object {
        $_.Name -ine "Quarantined_Corrupt_v100" -and $_.Name -ine "_RFAB_CorruptQuarantine"
    } | ForEach-Object { $SaveDirsToScan.Add($_.FullName) }
}
if (Test-Path -LiteralPath (Join-Path $GameRoot "MO2\profiles")) {
    Get-ChildItem -LiteralPath (Join-Path $GameRoot "MO2\profiles") -Directory -ErrorAction SilentlyContinue | ForEach-Object {
        $ps = Join-Path $_.FullName "saves"
        if (Test-Path -LiteralPath $ps) { $SaveDirsToScan.Add($ps) }
    }
}
$QuarantinedQuick = 0
foreach ($sDir in ($SaveDirsToScan | Select-Object -Unique)) {
    $qDir = Join-Path $sDir "Quarantined_Corrupt_v100"
    Get-ChildItem -LiteralPath $sDir -File -ErrorAction SilentlyContinue | ForEach-Object {
        if (($_.Name.EndsWith(".ess", [System.StringComparison]::OrdinalIgnoreCase) -or $_.Name.EndsWith(".ess.tmp", [System.StringComparison]::OrdinalIgnoreCase)) -and
            (-not (Test-ValidSkyrimEssQuick $_.FullName))) {
            New-Item -ItemType Directory -Path $qDir -Force | Out-Null
            Move-Item -LiteralPath $_.FullName -Destination (Join-Path $qDir $_.Name) -Force -ErrorAction SilentlyContinue
            $QuarantinedQuick++
            if ($_.Name.EndsWith(".ess", [System.StringComparison]::OrdinalIgnoreCase)) {
                $sk = [System.IO.Path]::ChangeExtension($_.FullName, ".skse")
                if (Test-Path -LiteralPath $sk) { Move-Item -LiteralPath $sk -Destination (Join-Path $qDir ([System.IO.Path]::GetFileName($sk))) -Force -ErrorAction SilentlyContinue }
            }
        }
    }
}
if ($QuarantinedQuick -gt 0) {
    Write-Host "  [OK] Quarantined $QuarantinedQuick corrupt/truncated .ess save(s) into Quarantined_Corrupt_v100" -ForegroundColor Yellow
}
Write-Host "  [OK] Updated Client + ESP + Scripts + EngineFixes + Russian UI -> $ClientDst" -ForegroundColor Green

$SrvCandidates = @(
    (Join-Path $GameRoot "RFAB_Online_Server"),
    (Join-Path (Split-Path -Parent $GameRoot) "RFAB_Online_Server")
)
foreach ($SrvDst in $SrvCandidates) {
    if ($SrvDst -and (Test-Path -LiteralPath $SrvDst)) {
        foreach ($sf in @("SkyrimTogetherServer.exe", "STServer.dll")) {
            Copy-Item -LiteralPath (Join-Path $SrcDir "Server\$sf") -Destination (Join-Path $SrvDst $sf) -Force
        }
        $SrvIni = Join-Path $SrvDst "config\STServer.ini"
        if (Test-Path -LiteralPath $SrvIni) {
            $rawIni = [System.IO.File]::ReadAllText($SrvIni, [System.Text.Encoding]::UTF8)
            $updIni = $rawIni
            if ($updIni -match '4=Master \(Requiem default\)') {
                $updIni = $updIni.Replace('4=Master (Requiem default)', '2=Adept (RFAB/Requiem 1.0x standard!), 4=Master')
                $updIni = [regex]::Replace($updIni, '(?mi)^(\s*uDifficulty\s*=\s*)4(\s*)$', '${1}2${2}')
            }
            if ($updIni -notmatch '(?mi)^\s*bEnableItemDrops\s*=') {
                $updIni = [regex]::Replace($updIni, '(?mi)^(\s*uDifficulty\s*=\s*[^\r\n]+\r?\n)', "`$1bEnableItemDrops = true`r`n")
            }
            if ($updIni -match '(?mi)^\s*bEnableMiscQuestSync\s*=\s*false') {
                $updIni = [regex]::Replace($updIni, '(?mi)^(\s*bEnableMiscQuestSync\s*=\s*)false(\s*)$', '${1}true${2}')
            } elseif ($updIni -notmatch '(?mi)^\s*bEnableMiscQuestSync\s*=') {
                $updIni = [regex]::Replace($updIni, '(?mi)^(\s*bEnableItemDrops\s*=\s*[^\r\n]+\r?\n)', "`$1bEnableMiscQuestSync = true`r`n")
            }
            if ($updIni -ne $rawIni) { [System.IO.File]::WriteAllText($SrvIni, $updIni, (New-Object System.Text.UTF8Encoding($false))) }
        }
        Write-Host "  [OK] Updated Server (SkyrimTogetherServer.exe + STServer.dll + STServer.ini) -> $SrvDst" -ForegroundColor Green
    }
}

$StartBat = Join-Path $GameRoot "START_RFAB_SERVER.bat"
if (Test-Path -LiteralPath $StartBat) {
    Start-Process -FilePath "cmd.exe" -ArgumentList "/c `"$StartBat`"" -WorkingDirectory $GameRoot
    Write-Host "  [OK] Fresh RFAB Online Server started on port 10578!" -ForegroundColor Green
}
Write-Host "`nALL DONE! Launch 'RFAB Online - Play' in Mod Organizer 2 and press Right Ctrl / F2 -> 127.0.0.1" -ForegroundColor Cyan
'@
[System.IO.File]::WriteAllText((Join-Path $QuickDir "Update-Quick.ps1"), $QuickUpdatePs1, (New-Object System.Text.UTF8Encoding($true)))

$QuickBat = "@echo off`r`nchcp 65001 >nul`r`ntitle RFAB Online - 1-Click Client + Server Updater`r`npowershell.exe -NoProfile -ExecutionPolicy Bypass -File `"%~dp0Update-Quick.ps1`"`r`npause`r`n"
[System.IO.File]::WriteAllText((Join-Path $QuickDir "UPDATE_RFAB_ONLINE.bat"), $QuickBat, [System.Text.Encoding]::ASCII)

[System.IO.Compression.ZipFile]::CreateFromDirectory($StageRoot, $InstallerZip, [System.IO.Compression.CompressionLevel]::Optimal, $true)
[System.IO.Compression.ZipFile]::CreateFromDirectory($QuickDir,  $QuickUpdZip,  [System.IO.Compression.CompressionLevel]::Optimal, $false)
[System.IO.Compression.ZipFile]::CreateFromDirectory($PdbDir,    $PdbZip,       [System.IO.Compression.CompressionLevel]::Optimal, $false)

$ShaLines = foreach ($f in Get-ChildItem -LiteralPath $OutDir -Filter "*.zip" | Sort-Object Name) {
    $h = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    "$h  $($f.Name)"
}
[System.IO.File]::WriteAllLines((Join-Path $OutDir "SHA256SUMS.txt"), $ShaLines, (New-Object System.Text.UTF8Encoding($false)))

Get-ChildItem -LiteralPath $OutDir | Format-Table Name, @{Label="SizeMB";Expression={[math]::Round($_.Length / 1MB, 2)}} -AutoSize
Write-Host "[RELEASE ARTIFACTS READY IN $OutDir]" -ForegroundColor Green
$global:LASTEXITCODE = 0
exit 0
