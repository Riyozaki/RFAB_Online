<#
.SYNOPSIS
    Parses every PowerShell script under Tools\rfab with the real PowerShell parser.

.DESCRIPTION
    The build, packaging, installer and report scripts run under Windows PowerShell 5.1, and a
    mistake in one of them used to show up only at the end of a half-hour Windows build. This
    check runs wherever PowerShell exists (GitHub's Linux runners ship PowerShell 7) and fails on:
      - a syntax error;
      - an operator that PowerShell 7 accepts and Windows PowerShell 5.1 does not
        (ternary, ??, ??=, ?., ?[], && and || between pipelines);
      - a script that contains non-ASCII bytes but has no UTF-8 BOM: 5.1 would read it in the
        ANSI code page of the machine.
    Scripts embedded in here-strings are text to the parser and are not checked.
#>
param(
    [string]$Root = ""
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($Root)) { $Root = Split-Path -Parent $MyInvocation.MyCommand.Path }

$failures = New-Object System.Collections.Generic.List[string]
$notIn51 = @("QuestionQuestion", "QuestionQuestionEquals", "QuestionDot", "QuestionLBracket")
$files = @(Get-ChildItem -LiteralPath $Root -Filter "*.ps1" -File | Sort-Object Name)

foreach ($file in $files) {
    $tokens = $null
    $errors = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($file.FullName, [ref]$tokens, [ref]$errors)
    foreach ($problem in @($errors)) {
        [void]$failures.Add(("{0}({1}): {2}" -f $file.Name, $problem.Extent.StartLineNumber, $problem.Message))
    }
    foreach ($token in @($tokens)) {
        if ($notIn51 -contains $token.Kind.ToString()) {
            [void]$failures.Add(("{0}({1}): '{2}' does not exist in Windows PowerShell 5.1" -f $file.Name, $token.Extent.StartLineNumber, $token.Text))
        }
    }
    if ($null -ne $ast) {
        $tooNew = $ast.FindAll({ param($node) $node.GetType().Name -eq "TernaryExpressionAst" -or $node.GetType().Name -eq "PipelineChainAst" }, $true)
        foreach ($node in @($tooNew)) {
            [void]$failures.Add(("{0}({1}): {2} does not exist in Windows PowerShell 5.1" -f $file.Name, $node.Extent.StartLineNumber, $node.GetType().Name))
        }
    }
    $bytes = [System.IO.File]::ReadAllBytes($file.FullName)
    $hasBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
    if (-not $hasBom) {
        foreach ($byte in $bytes) {
            if ($byte -gt 127) {
                [void]$failures.Add(("{0}: non-ASCII text without a UTF-8 BOM (Windows PowerShell 5.1 would misread it)" -f $file.Name))
                break
            }
        }
    }
}

if ($files.Count -eq 0) { [void]$failures.Add("no *.ps1 found under $Root") }

if ($failures.Count -gt 0) {
    Write-Host "powershell syntax check FAILED:"
    foreach ($failure in $failures) { Write-Host "  - $failure" }
    exit 1
}
Write-Host ("[RFAB_RESULT] powershell syntax: {0} scripts parsed by PowerShell {1}" -f $files.Count, $PSVersionTable.PSVersion)
exit 0
