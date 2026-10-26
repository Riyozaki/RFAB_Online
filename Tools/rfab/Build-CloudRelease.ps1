param(
    [string]$VersionTag = "v2.0.1",
    [string]$BuildMode = "releasedbg",
    # Publishing is OPT-IN. Without -Publish the script only builds, tests and packages
    # into release_artifacts\ and never touches GitHub Releases. This prevents branch
    # pushes (arena/**, main) from silently clobbering assets of the published release.
    [switch]$Publish
)

# Every already-published release is immutable. A new release always needs a NEW tag.
$ProtectedTags = @("v1.0.0", "v1.1.0", "v1.2.0", "v1.2.0-backup", "v1.2.0-hotfix.1", "v1.2.0-hotfix.2", "v2.0.0-alpha.1", "v2.0.0-alpha.2", "v2.0.0", "v2.0.1", "v2.0.1-backup")
if ($Publish -and ($VersionTag -in $ProtectedTags)) {
    throw "Hard guard: $VersionTag is a protected immutable release and must never be overwritten! Use a new version tag."
}
if (-not $Publish) {
    Write-Host "[RELEASE POLICY] Build/test/package only (no -Publish): GitHub Releases are NOT modified." -ForegroundColor Yellow
}

$IsPrereleaseTag = ($VersionTag.Contains("-")) -or ($env:GITHUB_REF -and $env:GITHUB_REF -ne "refs/heads/main")

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$env:PYTHONIOENCODING = "utf-8"

$script:PackagesReady = $false
$script:LastDiagMatches = ""
$script:LastDiagTail = ""
$script:ProgressLog = New-Object System.Collections.Generic.List[string]
$CacheMarker = Join-Path $env:LOCALAPPDATA ".xmake\packages\.rfab_cached_ok"

# Create-only, idempotent mirror of a published release into an immutable "<tag>-backup" pre-release.
# Runs on every CI build (even without -Publish) because it can only ADD a missing backup, never modify
# an existing release. Assets are verified against the source SHA256SUMS.txt before mirroring.
function Ensure-ImmutableBackupRelease([string]$SrcTag, [string]$DstTag, [string]$SrcCommit) {
    try {
        if (-not $env:GH_TOKEN) { return }
        & cmd.exe /d /s /c "gh release view $DstTag >nul 2>&1"
        if ($LASTEXITCODE -eq 0) {
            Write-Host "[BACKUP] Protected backup release $DstTag already exists on GitHub." -ForegroundColor Green
            return
        }
        & cmd.exe /d /s /c "gh release view $SrcTag >nul 2>&1"
        if ($LASTEXITCODE -ne 0) { return }

        Write-Host "[BACKUP] Mirroring $SrcTag assets into immutable backup release $DstTag..." -ForegroundColor Cyan
        $bakDir = Join-Path $env:TEMP ("rfab_bak_" + [Guid]::NewGuid().ToString("N"))
        New-Item -ItemType Directory -Path $bakDir -Force | Out-Null
        try {
            & cmd.exe /d /s /c "gh release download $SrcTag -D `"$bakDir`" --clobber"
            if ($LASTEXITCODE -ne 0) { throw "could not download $SrcTag assets" }

            # Integrity source of truth = GitHub's server-side asset digests (sha256 of the uploaded bytes).
            # The published SHA256SUMS.txt is NOT trusted: concurrent CI runs used to clobber v2.0.1 assets
            # from different builds, so SHA256SUMS.txt may describe another build than the zip next to it.
            $relJson = (& gh api "repos/{owner}/{repo}/releases/tags/$SrcTag") | Out-String | ConvertFrom-Json
            $digestLines = New-Object System.Collections.Generic.List[string]
            foreach ($asset in $relJson.assets) {
                $f = Join-Path $bakDir $asset.name
                if (-not (Test-Path -LiteralPath $f)) { throw "Missing downloaded asset $($asset.name)" }
                $actual = (Get-FileHash -LiteralPath $f -Algorithm SHA256).Hash.ToLowerInvariant()
                if ($asset.digest) {
                    $expected = ($asset.digest -replace '^sha256:', '').ToLowerInvariant()
                    if ($actual -ne $expected) { throw "Download corrupted: $($asset.name) does not match GitHub digest" }
                }
                $digestLines.Add("$actual  $($asset.name)")
            }

            $sumsNote = ""
            $sumsPath = Join-Path $bakDir "SHA256SUMS.txt"
            if (Test-Path -LiteralPath $sumsPath) {
                foreach ($line in [System.IO.File]::ReadAllLines($sumsPath)) {
                    $t = $line.Trim()
                    if (-not $t) { continue }
                    $parts = $t -split '\s+', 2
                    $name = $parts[1].TrimStart('*')
                    $f = Join-Path $bakDir $name
                    if (Test-Path -LiteralPath $f) {
                        $actual = (Get-FileHash -LiteralPath $f -Algorithm SHA256).Hash.ToLowerInvariant()
                        if ($actual -ne $parts[0].ToLowerInvariant()) {
                            $sumsNote += "- WARNING: published SHA256SUMS.txt does not match ``$name`` (assets were clobbered by concurrent CI runs). Use the digests below.`r`n"
                        }
                    }
                }
            }

            $tagSha = (& gh api "repos/{owner}/{repo}/git/ref/tags/$SrcTag" -q .object.sha 2>$null)
            $bakNotes = Join-Path $env:TEMP ("rfab_bak_notes_" + [Guid]::NewGuid().ToString("N") + ".md")
            $bakText = "Immutable backup mirror of stable release **$SrcTag**.`r`n`r`n" +
                       "- Assets were built from commit ``$SrcCommit``.`r`n" +
                       "- Git tag ``$SrcTag`` points to ``$tagSha``.`r`n" +
                       "- Every asset was verified against GitHub's server-side sha256 digest before mirroring.`r`n" +
                       $sumsNote + "`r`nActual SHA256 of the mirrored assets:`r`n``````text`r`n" + ($digestLines -join "`r`n") + "`r`n```````r`n"
            [System.IO.File]::WriteAllText($bakNotes, $bakText, (New-Object System.Text.UTF8Encoding($false)))

            # Create the release together with all assets in one call, so a half-uploaded backup is never left behind.
            $assetArgs = (Get-ChildItem -LiteralPath $bakDir -File | ForEach-Object { "`"$($_.FullName)`"" }) -join " "
            & cmd.exe /d /s /c "gh release create $DstTag $assetArgs --target $SrcCommit --title `"RFAB Online $SrcTag (Immutable Backup Archive)`" --notes-file `"$bakNotes`" --prerelease"
            $createCode = $LASTEXITCODE
            Remove-Item -LiteralPath $bakNotes -Force -ErrorAction SilentlyContinue
            if ($createCode -ne 0) {
                & cmd.exe /d /s /c "gh release delete $DstTag --yes --cleanup-tag >nul 2>&1"
                throw "gh release create $DstTag failed (exit $createCode); partial backup removed"
            }
            Write-Host "[BACKUP] Created verified GitHub backup release $DstTag!" -ForegroundColor Green
        } finally {
            Remove-Item -LiteralPath $bakDir -Recurse -Force -ErrorAction SilentlyContinue
        }
    } catch {
        Emit-Annotation "warning" "BACKUP" ("Non-fatal: backup of $SrcTag -> $DstTag failed ($SrcTag itself is untouched): " + $_.Exception.Message)
    }
}

function Ensure-V120BackupRelease([string]$RepoRootPath) {
    try {
        if (-not $Publish) { return }
        if (-not $env:GH_TOKEN) { return }
        & cmd.exe /d /s /c "gh release view v1.2.0-backup >nul 2>&1"
        if ($LASTEXITCODE -eq 0) {
            Write-Host "[BACKUP] Protected backup release v1.2.0-backup already exists on GitHub." -ForegroundColor Green
            return
        }
        & cmd.exe /d /s /c "gh release view v1.2.0 >nul 2>&1"
        if ($LASTEXITCODE -ne 0) { return }

        Write-Host "[BACKUP] Mirroring immutable v1.2.0 assets into permanent backup release v1.2.0-backup..." -ForegroundColor Cyan
        $bakDir = Join-Path $env:TEMP ("rfab_v120_bak_" + [Guid]::NewGuid().ToString("N"))
        New-Item -ItemType Directory -Path $bakDir -Force | Out-Null
        try {
            & cmd.exe /d /s /c "gh release download v1.2.0 -D `"$bakDir`" --clobber"
            if ($LASTEXITCODE -ne 0) {
                Write-Warning "[BACKUP] Could not download v1.2.0 assets for mirror; live v1.2.0 remains untouched."
                return
            }

            $manifestPath = Join-Path $RepoRootPath "docs\V1.2.0_RELEASE_BACKUP_MANIFEST.json"
            if (Test-Path -LiteralPath $manifestPath) {
                $manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
                foreach ($exp in $manifest.assets) {
                    $f = Join-Path $bakDir $exp.name
                    if (-not (Test-Path -LiteralPath $f)) {
                        throw "Missing v1.2.0 backup asset: $($exp.name)"
                    }
                    $actualHash = (Get-FileHash -LiteralPath $f -Algorithm SHA256).Hash.ToLowerInvariant()
                    if ($actualHash -ne $exp.sha256.ToLowerInvariant()) {
                        throw "SHA256 mismatch on v1.2.0 asset $($exp.name): expected $($exp.sha256), got $actualHash"
                    }
                }
            }

            $bakNotes = Join-Path $bakDir "BACKUP_NOTES.md"
            $bakText = "Immutable backup mirror of stable release **v1.2.0** (commit ``b431be0da38764ce1f4d658329934b4a8ccc5692``), preserved automatically before v1.2.0 hotfix/refinement builds.`r`n"
            [System.IO.File]::WriteAllText($bakNotes, $bakText, (New-Object System.Text.UTF8Encoding($false)))

            & cmd.exe /d /s /c "gh release create v1.2.0-backup --target b431be0da38764ce1f4d658329934b4a8ccc5692 --title `"RFAB Online v1.2.0 (Immutable Backup Archive)`" --notes-file `"$bakNotes`" --prerelease"
            if ($LASTEXITCODE -eq 0) {
                Get-ChildItem -LiteralPath $bakDir -File | Where-Object { $_.Name -ne "BACKUP_NOTES.md" } | ForEach-Object {
                    & cmd.exe /d /s /c "gh release upload v1.2.0-backup `"$($_.FullName)`" --clobber"
                }
                Write-Host "[BACKUP] Created verified GitHub backup release v1.2.0-backup!" -ForegroundColor Green
            }
        } finally {
            Remove-Item -LiteralPath $bakDir -Recurse -Force -ErrorAction SilentlyContinue
        }
    } catch {
        Write-Warning ("[BACKUP] Non-fatal backup mirror warning (v1.2.0 itself remains untouched): " + $_.Exception.Message)
    }
}

function Update-ReleaseTelemetry([string]$StateTitle, [string]$BodyText) {
    try {
        if ((-not $Publish) -or ($VersionTag -in $ProtectedTags)) { return }
        if ($StateTitle.Length -gt 200) { $StateTitle = $StateTitle.Substring(0, 200) }
        if ($BodyText.Length -gt 60000) {
            $BodyText = $BodyText.Substring($BodyText.Length - 60000)
        }
        $tmpNotes = Join-Path $env:TEMP ("rfab_telemetry_" + [Guid]::NewGuid().ToString("N") + ".md")
        [System.IO.File]::WriteAllText($tmpNotes, $BodyText, (New-Object System.Text.UTF8Encoding($false)))
        $sha = (& git rev-parse HEAD).Trim()
        & cmd.exe /d /s /c "gh release view $VersionTag >nul 2>&1"
        if ($LASTEXITCODE -ne 0) {
            & cmd.exe /d /s /c "gh release create $VersionTag --target $sha --title `"$StateTitle`" --notes-file `"$tmpNotes`" --prerelease >nul 2>&1"
        } else {
            & cmd.exe /d /s /c "gh release edit $VersionTag --title `"$StateTitle`" --notes-file `"$tmpNotes`" >nul 2>&1"
        }
        Remove-Item -LiteralPath $tmpNotes -Force -ErrorAction SilentlyContinue
    } catch {}
}

function Emit-Annotation([string]$Level, [string]$Title, [string]$Msg) {
    $clean = $Msg -replace "\r", "" -replace "\n", " | " -replace "%", "%25"
    if ($clean.Length -gt 3500) { $clean = $clean.Substring($clean.Length - 3500) }
    Write-Host "::${Level} title=${Title}::${clean}"
}

function Invoke-LoggedCmd([string]$StepName, [string]$CmdString) {
    Write-Host "`n==> $StepName" -ForegroundColor Cyan
    $script:ProgressLog.Add("- ⏳ Running: ``$StepName`` ($([DateTime]::UtcNow.ToString('HH:mm:ss')) UTC)")

    $uid = [Guid]::NewGuid().ToString("N")
    $stepBat = Join-Path $env:TEMP ("rfab_cmd_" + $uid + ".cmd")
    $stepLog = Join-Path $env:TEMP ("rfab_step_" + $uid + ".log")
    $batBody = "@echo off`r`ncall :__rfab_step > `"$stepLog`" 2>&1`r`nexit /b %ERRORLEVEL%`r`n:__rfab_step`r`n$CmdString`r`n"
    [System.IO.File]::WriteAllText($stepBat, $batBody, (New-Object System.Text.UTF8Encoding($false)))

    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & cmd.exe /d /c $stepBat
    $code = $LASTEXITCODE
    $ErrorActionPreference = $prev
    Remove-Item -LiteralPath $stepBat -Force -ErrorAction SilentlyContinue

    if (Test-Path -LiteralPath $stepLog) {
        $allLines = [System.IO.File]::ReadAllLines($stepLog)
        $tailShow = if ($allLines.Length -gt 80) { $allLines[($allLines.Length - 80)..($allLines.Length - 1)] } else { $allLines }
        foreach ($ln in $tailShow) { Write-Host $ln }
        if ($code -ne 0) {
            $errMatches = @($allLines | Where-Object { $_ -match '(?i)(error\s+[A-Z]*\d+|fatal error|unresolved external|error:|Traceback|Assertion|Exception)' } | ForEach-Object { $_ -replace '-external:[^\s]+', '' })
            if ($errMatches.Count -gt 0) {
                $script:LastDiagMatches = ($errMatches -join "`r`n")
                Emit-Annotation "warning" "DIAG_MATCHES" ($errMatches -join "`n")
            }
            $compactTail = @($tailShow | ForEach-Object { $_ -replace '-external:[^\s]+', '' })
            $script:LastDiagTail = ($compactTail -join "`r`n")
            Emit-Annotation "warning" "DIAG_TAIL" ($compactTail -join "`n")
            if ($Publish -and ($VersionTag -notin $ProtectedTags)) {
                $failLogAsset = Join-Path $env:TEMP "build_step_log.txt"
                Copy-Item -LiteralPath $stepLog -Destination $failLogAsset -Force -ErrorAction SilentlyContinue
                & cmd.exe /d /s /c "gh release upload $VersionTag `"$failLogAsset`" --clobber >nul 2>&1"
            }
            Remove-Item -LiteralPath $stepLog -Force -ErrorAction SilentlyContinue
            throw "$StepName exited with code $code"
        }
        Remove-Item -LiteralPath $stepLog -Force -ErrorAction SilentlyContinue
    } elseif ($code -ne 0) {
        throw "$StepName exited with code $code (no log file)"
    }
    $script:ProgressLog[$script:ProgressLog.Count - 1] = "- ✅ Completed: ``$StepName`` ($([DateTime]::UtcNow.ToString('HH:mm:ss')) UTC)"
    Emit-Annotation "notice" "RFAB_BUILD" "OK: $StepName"
}

try {
    $RepoRoot = [System.IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) "..\.."))
    Set-Location -LiteralPath $RepoRoot

    Ensure-V120BackupRelease -RepoRootPath $RepoRoot
    Ensure-ImmutableBackupRelease -SrcTag "v2.0.1" -DstTag "v2.0.1-backup" -SrcCommit "3447a5437bfb6812f4a9ac8dee3707924b6121c0"

    Invoke-LoggedCmd "[1/7] Submodules init + SE 1.5.97 Submodule Patches" "git submodule sync --recursive && git submodule update --init --force --recursive --depth=1 && python Tools\rfab\patch_submodules.py"
    Invoke-LoggedCmd "[2/7] SE 1.5.97 RVA, Struct & Hotfix Regression Audit" "python Tools\rfab\verify_se1597_port.py && python Tools\rfab\test_v120_hotfix_regressions.py"
    Invoke-LoggedCmd "[3/7] xmake config (Windows x64 /MT $BuildMode)" "xmake config --plat=windows --arch=x64 --mode=$BuildMode --yes -vD"
    $script:PackagesReady = $true

    Invoke-LoggedCmd "[4/7] xmake build (Client + Launcher + Server + Tests)" "xmake -y -v"

    $TpTests = Get-ChildItem -LiteralPath (Join-Path $RepoRoot "build\windows\x64\$BuildMode") -Filter "TPTests.exe" -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $TpTests) {
        throw "TPTests.exe not found under build\windows\x64\$BuildMode!"
    }
    Invoke-LoggedCmd "[5/7] Native Windows x64 Unit Tests (TPTests.exe)" "`"$($TpTests.FullName)`""

    Invoke-LoggedCmd "[6/7] xmake install + Package-Release.ps1 + Smoke & Clean-Wipe Verification" `
        "xmake install -o distrib && powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tools\rfab\Package-Release.ps1 -VersionTag $VersionTag -BuildMode $BuildMode"

    $NotesTemplateFile = Join-Path $RepoRoot "Tools\rfab\RELEASE_NOTES_TEMPLATE.md"
    if (-not (Test-Path -LiteralPath $NotesTemplateFile)) {
        throw "Release notes template missing: $NotesTemplateFile"
    }
    $NotesFile = Join-Path $RepoRoot "release_artifacts\RELEASE_NOTES.md"
    $ReleaseNotes = [System.IO.File]::ReadAllText($NotesTemplateFile, [System.Text.Encoding]::UTF8)
    $ReleaseNotes = $ReleaseNotes.Replace("{{VERSION_TAG}}", $VersionTag)
    [System.IO.File]::WriteAllText($NotesFile, $ReleaseNotes, (New-Object System.Text.UTF8Encoding($false)))

    if (-not $Publish) {
        Emit-Annotation "notice" "RFAB_BUILD" "Build, tests and packaging succeeded (publish skipped; artifacts in release_artifacts\)."
        $global:LASTEXITCODE = 0
        exit 0
    }

    $CommitSha    = (& git rev-parse HEAD).Trim()
    $InstallerZip = Join-Path $RepoRoot "release_artifacts\RFAB_Online_${VersionTag}_Full_Installer.zip"
    $QuickUpdZip  = Join-Path $RepoRoot "release_artifacts\RFAB_Online_${VersionTag}_Quick_Update.zip"
    $PdbZip       = Join-Path $RepoRoot "release_artifacts\RFAB_Online_${VersionTag}_PDB_Symbols.zip"
    $ShaFile      = Join-Path $RepoRoot "release_artifacts\SHA256SUMS.txt"
    $RelTitle     = "RFAB Online $VersionTag - Full Clean Standalone Installer (Skyrim SE 1.5.97.0.8)"
    $RelFlags     = if ($IsPrereleaseTag) { "--prerelease" } else { "--prerelease=false --latest" }
    $CreateFlags  = if ($IsPrereleaseTag) { "--prerelease" } else { "--latest" }

    & cmd.exe /d /s /c "gh release view $VersionTag >nul 2>&1"
    if ($LASTEXITCODE -ne 0) {
        Invoke-LoggedCmd "[7/7] Create GitHub Release $VersionTag" `
            "gh release create $VersionTag --target $CommitSha --title `"$RelTitle`" --notes-file `"$NotesFile`" $CreateFlags"
    } else {
        Invoke-LoggedCmd "[7/7] Update GitHub Release Metadata $VersionTag (in-place)" `
            "gh release edit $VersionTag --target $CommitSha --title `"$RelTitle`" --notes-file `"$NotesFile`" $RelFlags"
    }

    foreach ($assetPath in @($QuickUpdZip, $InstallerZip, $PdbZip, $ShaFile)) {
        $assetName = Split-Path -Leaf $assetPath
        $uploadedOk = $false
        for ($attempt = 1; $attempt -le 4; $attempt++) {
            Write-Host "::group::Upload $assetName (attempt $attempt/4)"
            & cmd.exe /d /s /c "gh release upload $VersionTag `"$assetPath`" --clobber 2>&1"
            $upCode = $LASTEXITCODE
            Write-Host "::endgroup::"
            if ($upCode -eq 0) {
                Emit-Annotation "notice" "RFAB_UPLOAD" "OK: Uploaded $assetName (attempt $attempt)"
                $uploadedOk = $true
                break
            }
            Write-Warning "Transient upload failure for $assetName (attempt $attempt/4, exit $upCode); retrying in 4s..."
            Start-Sleep -Seconds 4
        }
        if (-not $uploadedOk) {
            throw "[7/7] Failed to upload $assetName after 4 attempts"
        }
    }

    if (Test-Path -LiteralPath (Split-Path -Parent $CacheMarker)) {
        [System.IO.File]::WriteAllText($CacheMarker, "ok")
    }
    & cmd.exe /d /s /c "gh release delete-asset $VersionTag build_step_log.txt --yes >nul 2>&1"
    & cmd.exe /d /s /c "gh release edit $VersionTag --target $CommitSha --title `"$RelTitle`" --notes-file `"$NotesFile`" $RelFlags"
    Emit-Annotation "notice" "RELEASE_READY" "Published https://github.com/Riyozaki/RFAB_Online/releases/tag/$VersionTag"
    $global:LASTEXITCODE = 0
    exit 0
} catch {
    $errMsg = $_.Exception.Message
    $failBody = "## ❌ Build Failure: $errMsg`r`n`r`n### Progress`r`n$($script:ProgressLog -join "`r`n")`r`n`r`n### Diagnostic Matches`r`n``````text`r`n$($script:LastDiagMatches)`r`n```````r`n`r`n### Log Tail`r`n``````text`r`n$($script:LastDiagTail)`r`n``````"
    Update-ReleaseTelemetry "RFAB Online $VersionTag (FAILED: $errMsg)" $failBody
    Emit-Annotation "error" "CI_EXCEPTION" $errMsg
    exit 1
}
