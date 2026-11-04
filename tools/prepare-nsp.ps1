param(
    [string]$DevkitPro = 'D:/devkitPro',
    [string]$HacBrewPack,
    [string]$Keyset
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$source = Join-Path $root 'native/switch'
$stage = Join-Path $root 'build/nsp'
$titleId = '0100f5370a000000'
$versionMatch = [regex]::Match((Get-Content "$source/Makefile" -Raw), '(?m)^APP_VERSION\s*:=\s*(\S+)')
if (!$versionMatch.Success) { throw 'APP_VERSION missing in Switch Makefile.' }
$appVersion = $versionMatch.Groups[1].Value
function Run-Tool([string]$Tool, [string[]]$Arguments) {
    & $Tool @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Tool failed: $LASTEXITCODE" }
}
if (!(Test-Path "$source/switchy-op.elf")) {
    throw 'Build native/switch with devkitPro make first.'
}
New-Item -ItemType Directory -Force "$stage/exefs", "$stage/control", "$root/dist/nsp" | Out-Null
Run-Tool "$DevkitPro/tools/bin/elf2nso.exe" @("$source/switchy-op.elf", "$stage/exefs/main")
Run-Tool "$DevkitPro/tools/bin/npdmtool.exe" @("$source/npdm.json", "$stage/exefs/main.npdm")
Run-Tool "$DevkitPro/tools/bin/nacptool.exe" @('--create', 'switchy-op', 'nemakemake', $appVersion, "$stage/control/control.nacp", "--titleid=$titleId")
Copy-Item "$source/icon.jpg" "$stage/control/icon_AmericanEnglish.dat" -Force
if (!$HacBrewPack -and !$Keyset) {
    Write-Host "NSP inputs prepared in $stage. Supply -HacBrewPack and -Keyset to package."
    return
}
if (!(Test-Path -LiteralPath $HacBrewPack) -or !(Test-Path -LiteralPath $Keyset)) {
    throw 'Provide existing -HacBrewPack and -Keyset files.'
}
Push-Location $stage
try {
    Run-Tool $HacBrewPack @('--keyset', $Keyset, '--exefsdir', "$stage/exefs", '--controldir', "$stage/control", '--nspdir', "$root/dist/nsp", '--noromfs', '--nologo', '--titleid', $titleId)
} finally { Pop-Location }
