param(
    [string]$VersionTag = "v1.0.0",
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

if (-not (Test-Path -LiteralPath (Join-Path $StageClient "UI\index.html"))) {
    $FallbackUi = Join-Path $RepoRoot "dist_ready\RFAB_Online_MO2\SkyrimTogetherReborn\UI"
    Invoke-RobocopySafe -Src $FallbackUi -Dst (Join-Path $StageClient "UI") -ExtraArgs @("/E")
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
[assembly: AssemblyVersion("1.5.97.8")]
[assembly: AssemblyFileVersion("1.5.97.8")]
public class Program { public static void Main() {} }
'@
Add-Type -TypeDefinition $CsCode -OutputAssembly $DummyExe -OutputType ConsoleApplication

[System.IO.File]::WriteAllBytes((Join-Path $MockRfab "MO2\ModOrganizer.exe"), [byte[]](0x4D, 0x5A, 0x00, 0x00))
[System.IO.File]::WriteAllText((Join-Path $MockRfab "MO2\mods\RFAB_Online\STALE_LEFTOVER_FROM_OLD_TEST.txt"), "must be wiped")
[System.IO.File]::WriteAllText((Join-Path $MockRfab "MO2\mods\RFAB_Online_MO2\STALE_LEGACY_DIR.txt"), "must be wiped")
[System.IO.File]::WriteAllText((Join-Path $MockRfab "MO2\mods\EngineFixes\SKSE\Plugins\EngineFixes.toml"), "[Patches]`r`nMemoryManager = true`r`nRegularQuicksaves = false`r`n")
[System.IO.File]::WriteAllText((Join-Path $MockRfab "MO2\mods\Souls\SKSE\Plugins\SkyrimSoulsRE.ini"), "[SLOWMOTION]`r`nfSlowMotionMultiplier = 0.200000`r`nbEnableSlowMotionInventoryMenu = true`r`n")
[System.IO.File]::WriteAllText((Join-Path $MockRfab "MO2\mods\Autosave\SKSE\Plugins\[RFAB] Autosave.ini"), "[Main]`r`nEnabled = true`r`n")
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
Remove-Item -LiteralPath $MockRfab -Recurse -Force -ErrorAction SilentlyContinue
Write-Host "  [OK] End-to-end clean-wipe installer test passed 100%!" -ForegroundColor Green

Write-Host "==> [6/6] Compressing Release ZIP Archives & SHA256 Checksums..." -ForegroundColor Cyan
Get-ChildItem -LiteralPath $BuildRel -Filter "*.pdb" -Recurse -File -ErrorAction SilentlyContinue | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $PdbDir $_.Name) -Force
}

Add-Type -AssemblyName System.IO.Compression.FileSystem | Out-Null
$InstallerZip = Join-Path $OutDir "RFAB_Online_${VersionTag}_Full_Installer.zip"
$PdbZip       = Join-Path $OutDir "RFAB_Online_${VersionTag}_PDB_Symbols.zip"

[System.IO.Compression.ZipFile]::CreateFromDirectory($StageRoot, $InstallerZip, [System.IO.Compression.CompressionLevel]::Optimal, $true)
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
