param(
    [string]$WorkflowPath = (Join-Path $PSScriptRoot '../.github/workflows/windows.yml')
)

$ErrorActionPreference = 'Stop'
$workflowText = Get-Content -LiteralPath $WorkflowPath -Raw
$gateMatch = [regex]::Match(
    $workflowText,
    '(?ms)^          \$installerScript = Get-Content.*?(?=^          \$requiredRuntimeFiles =)'
)
if (-not $gateMatch.Success) {
    throw 'Could not extract the installer ordering gate from the workflow.'
}

# Remove the YAML block indentation, then execute the workflow's real gate.
$gateText = [regex]::Replace($gateMatch.Value, '(?m)^          ', '')
$tokens = $null
$parseErrors = $null
[void][System.Management.Automation.Language.Parser]::ParseInput(
    $gateText,
    [ref]$tokens,
    [ref]$parseErrors
)
if ($parseErrors.Count -gt 0) {
    throw "Installer ordering gate does not parse: $($parseErrors.Message -join '; ')"
}
$gate = [scriptblock]::Create($gateText)

function Test-InstallerOrderingFixture {
    param(
        [string]$Name,
        [bool]$ExpectAccepted,
        [object[]]$ExpectedLines
    )

    $openMutexLine = $null
    $taskkillLine = $null
    $copyLine = $null
    $accepted = $true
    $caseRoot = Join-Path ([IO.Path]::GetTempPath()) "apd-installer-gate-$([guid]::NewGuid().ToString('N'))"
    $projectScript = Join-Path $caseRoot 'Build/Release/Installer/_CPack_Packages/win64/NSIS/project.nsi'
    $fixturePath = Join-Path $PSScriptRoot "Fixtures/InstallerOrdering/$Name.nsi"

    try {
        [void](New-Item -ItemType Directory -Path (Split-Path -Parent $projectScript) -Force)
        Copy-Item -LiteralPath $fixturePath -Destination $projectScript
        Push-Location -LiteralPath $caseRoot
        try {
            . $gate
        }
        catch {
            $accepted = $false
        }
        finally {
            Pop-Location
        }
    }
    finally {
        Remove-Item -LiteralPath $caseRoot -Recurse -Force -ErrorAction SilentlyContinue
    }

    if ($accepted -ne $ExpectAccepted) {
        throw "$Name expected accepted=$ExpectAccepted, got accepted=$accepted."
    }

    $actualLines = @($openMutexLine, $taskkillLine, $copyLine)
    for ($index = 0; $index -lt $ExpectedLines.Count; $index++) {
        if ($actualLines[$index] -ne $ExpectedLines[$index]) {
            throw "$Name expected line $($ExpectedLines[$index]) at index $index, got $($actualLines[$index])."
        }
    }
}

$cases = @(
    @{ Name = 'valid'; ExpectAccepted = $true; ExpectedLines = @(6, 8, 10) }
    @{ Name = 'full-line-comments'; ExpectAccepted = $false; ExpectedLines = @($null, $null, 3) }
    @{ Name = 'block-comments'; ExpectAccepted = $false; ExpectedLines = @($null, $null, 5) }
    @{ Name = 'trailing-comments'; ExpectAccepted = $false; ExpectedLines = @($null, $null, 5) }
    @{ Name = 'after-copy'; ExpectAccepted = $false; ExpectedLines = @(7, 9, 5) }
    @{ Name = 'missing-user-data-cleanup'; ExpectAccepted = $false; ExpectedLines = @(2, 4, 6) }
    @{ Name = 'missing-roaming-cleanup'; ExpectAccepted = $false; ExpectedLines = @(2, 4, 6) }
    @{ Name = 'missing-migration-env'; ExpectAccepted = $false; ExpectedLines = @(2, 4, 6) }
    @{ Name = 'missing-user-shell-context'; ExpectAccepted = $false; ExpectedLines = @(2, 4, 6) }
    @{ Name = 'migration-guard-after-cleanup'; ExpectAccepted = $false; ExpectedLines = @(2, 4, 6) }
    @{ Name = 'missing-migration-failure-guard'; ExpectAccepted = $false; ExpectedLines = @(2, 4, 6) }
    @{ Name = 'context-restored-outside-uninstall'; ExpectAccepted = $false; ExpectedLines = @(2, 4, 6) }
)

$failures = @()
foreach ($case in $cases) {
    try {
        Test-InstallerOrderingFixture @case
        Write-Host "PASS $($case.Name)"
    }
    catch {
        $failures += $_.Exception.Message
        Write-Host "FAIL $($case.Name): $($_.Exception.Message)"
    }
}

if ($failures.Count -gt 0) {
    throw "Installer ordering fixtures failed: $($failures.Count)/$($cases.Count)."
}
Write-Host "Fixtures: $($cases.Count)/$($cases.Count) passed"
