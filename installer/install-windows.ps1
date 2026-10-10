<#
.SYNOPSIS
    Installerer TheSuperHackers GeneralsGameCode (Zero Hour) build i en eksisterende
    Command & Conquer Generals - Zero Hour installation (Steam, EA App, Origin, CD).

    Indstillingerne (repo, build-tag m.m.) læses fra zh-install.conf ved siden af scriptet.
    Parametre angivet på kommandolinjen går forud for config-filen.

.EXAMPLE
    .\install-windows.ps1
    .\install-windows.ps1 -Config "D:\andet.conf"
    .\install-windows.ps1 -InstallDir "D:\Games\Command & Conquer Generals - Zero Hour"
    .\install-windows.ps1 -Tag latest                                # Nyeste release i Repo (standard)
    .\install-windows.ps1 -Tag historical-2026-10-10-18449d8 -IncludePdb
    .\install-windows.ps1 -Repo TheSuperHackers/GeneralsGameCode -Tag weekly-2026-10-02   # Officiel build
    .\install-windows.ps1 -SkipGenLauncher
#>
[CmdletBinding()]
param(
    [string]$InstallDir,
    [string]$Repo,
    [string]$Tag,
    [switch]$IncludePdb,
    [switch]$SkipGenLauncher,
    [string]$Config
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"   # Gør Invoke-WebRequest markant hurtigere
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

function Write-Info([string]$Message) { Write-Host "[INFO] $Message" -ForegroundColor Cyan }
function Write-Warn([string]$Message) { Write-Host "[ADVARSEL] $Message" -ForegroundColor Yellow }
function Stop-WithError([string]$Message)
{
    Write-Host "[FEJL] $Message" -ForegroundColor Red
    exit 1
}

# Læser NØGLE=værdi fra config-filen. Filen køres ikke som kode.
function Read-InstallConfig([string]$Path)
{
    $values = @{}
    if (-not (Test-Path -LiteralPath $Path))
    {
        return $values
    }
    $knownKeys = @("REPO", "TAG", "INSTALL_DIR", "INCLUDE_PDB", "SKIP_GENLAUNCHER")
    foreach ($line in Get-Content -LiteralPath $Path -Encoding UTF8)
    {
        if ($line -match '^\s*(#|$)')
        {
            continue
        }
        if ($line -notmatch '^\s*([A-Z_]+)\s*=(.*)$')
        {
            Write-Warn "Ugyldig linje i ${Path}: $line"
            continue
        }
        $key = $Matches[1]
        $value = $Matches[2].Trim()
        if ($value -match '^"(.*)"$' -or $value -match "^'(.*)'$")
        {
            $value = $Matches[1]
        }
        if ($knownKeys -notcontains $key)
        {
            Write-Warn "Ukendt nøgle i ${Path}: $key"
            continue
        }
        $values[$key] = $value
    }
    return $values
}

function Test-ConfigFlag([string]$Value)
{
    return @("1", "true", "yes", "ja") -contains ("$Value".Trim().ToLowerInvariant())
}

# Scriptets mappe. $PSScriptRoot/$PSCommandPath kan være tomme (fx PowerShell 2.0 eller
# når scriptet ikke køres som fil), så der falder vi tilbage til $MyInvocation og til sidst
# den aktuelle mappe.
$ScriptPath = $PSCommandPath
if (-not $ScriptPath)
{
    $ScriptPath = $MyInvocation.MyCommand.Path
}
$ScriptDir = $PSScriptRoot
if (-not $ScriptDir -and $ScriptPath)
{
    $ScriptDir = Split-Path -Parent $ScriptPath
}
if (-not $ScriptDir)
{
    $ScriptDir = (Get-Location).Path
}
if (-not $Config)
{
    $Config = Join-Path $ScriptDir "zh-install.conf"
}

# --- Indstillinger: parametre > config-fil > standardværdier ---
$configValues = Read-InstallConfig $Config
if (Test-Path -LiteralPath $Config)
{
    Write-Info "Config: $Config"
}
else
{
    Write-Warn "Ingen config-fil ($Config) - bruger standardværdier."
}
if (-not $PSBoundParameters.ContainsKey("Repo"))
{
    $Repo = if ($configValues["REPO"]) { $configValues["REPO"] } else { "Tricky12321/CnC_Generals_Zero_Hour" }
}
if (-not $PSBoundParameters.ContainsKey("Tag"))
{
    $Tag = if ($configValues["TAG"]) { $configValues["TAG"] } else { "latest" }
}
if (-not $PSBoundParameters.ContainsKey("InstallDir") -and $configValues["INSTALL_DIR"])
{
    $InstallDir = $configValues["INSTALL_DIR"]
}
if (-not $PSBoundParameters.ContainsKey("IncludePdb") -and (Test-ConfigFlag $configValues["INCLUDE_PDB"]))
{
    $IncludePdb = [switch]::Present
}
if (-not $PSBoundParameters.ContainsKey("SkipGenLauncher") -and (Test-ConfigFlag $configValues["SKIP_GENLAUNCHER"]))
{
    $SkipGenLauncher = [switch]::Present
}
if ($Tag -eq "latest")
{
    # Nyeste release med en Zero Hour-zip (generalszh-<tag>.zip). GitHub returnerer releases
    # nyeste først, også pre-releases.
    try
    {
        $releases = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases?per_page=30" -UseBasicParsing
    }
    catch
    {
        Stop-WithError "Kunne ikke hente release-listen for $Repo fra GitHub.`n$($_.Exception.Message)"
    }
    $latest = $releases | Where-Object { $release = $_; $release.assets | Where-Object { $_.name -eq "generalszh-$($release.tag_name).zip" } } | Select-Object -First 1
    if (-not $latest)
    {
        Stop-WithError "Fandt ingen release med en Zero Hour-zip i $Repo."
    }
    $Tag = $latest.tag_name
    Write-Info "Nyeste release: $Tag"
}
Write-Info "Build: $Repo ($Tag)"

$Asset = "generalszh-$Tag.zip"
$Url = "https://github.com/$Repo/releases/download/$Tag/$Asset"
$MarkerFile = "INIZH.big"
$SteamAppId = "2732960"
$GenLauncherManifest = "https://raw.githubusercontent.com/p0ls3r/GenLauncherModsData/master/ReposModificationDataZH3.yaml"
$GenLauncherFallbackUrl = "https://gen.insave.ovh/genlauncher/launcher/GenLauncher1010.zip"

function Test-ZeroHourDir([string]$Path)
{
    return ($Path -and (Test-Path -LiteralPath (Join-Path $Path $MarkerFile)))
}

function Get-SteamLibraries
{
    $libraries = New-Object System.Collections.Generic.List[string]
    $steamRoots = @()

    foreach ($key in @("HKCU:\Software\Valve\Steam", "HKLM:\SOFTWARE\WOW6432Node\Valve\Steam", "HKLM:\SOFTWARE\Valve\Steam"))
    {
        $props = Get-ItemProperty -Path $key -ErrorAction SilentlyContinue
        if ($props)
        {
            if ($props.SteamPath) { $steamRoots += ($props.SteamPath -replace "/", "\") }
            if ($props.InstallPath) { $steamRoots += $props.InstallPath }
        }
    }
    $steamRoots += "${env:ProgramFiles(x86)}\Steam"

    foreach ($root in ($steamRoots | Select-Object -Unique))
    {
        if (-not (Test-Path -LiteralPath (Join-Path $root "steamapps"))) { continue }
        $libraries.Add($root)

        $vdf = Join-Path $root "steamapps\libraryfolders.vdf"
        if (Test-Path -LiteralPath $vdf)
        {
            foreach ($match in (Select-String -LiteralPath $vdf -Pattern '^\s*"path"\s*"(.+)"\s*$'))
            {
                $libraries.Add(($match.Matches[0].Groups[1].Value -replace "\\\\", "\"))
            }
        }
    }
    return $libraries | Select-Object -Unique
}

function Find-ZeroHourDir
{
    # 1. Registreringsdatabase (CD/Origin/EA App/The First Decade)
    $regKeys = @(
        "HKLM:\SOFTWARE\WOW6432Node\Electronic Arts\EA Games\Command and Conquer Generals Zero Hour",
        "HKLM:\SOFTWARE\Electronic Arts\EA Games\Command and Conquer Generals Zero Hour",
        "HKLM:\SOFTWARE\WOW6432Node\EA Games\Command and Conquer Generals Zero Hour",
        "HKLM:\SOFTWARE\EA Games\Command and Conquer Generals Zero Hour"
    )
    foreach ($key in $regKeys)
    {
        $props = Get-ItemProperty -Path $key -ErrorAction SilentlyContinue
        if ($props)
        {
            foreach ($value in @($props.InstallPath, $props.'Install Dir', $props.InstallDir))
            {
                if (Test-ZeroHourDir $value) { return $value.TrimEnd("\") }
            }
        }
    }

    # 2. Steam-biblioteker
    $steamNames = @(
        "Command & Conquer Generals - Zero Hour",
        "Command and Conquer Generals Zero Hour",
        "Command & Conquer Generals Zero Hour"
    )
    foreach ($library in Get-SteamLibraries)
    {
        # Steam-manifestet angiver den præcise installationsmappe
        $manifest = Join-Path $library "steamapps\appmanifest_$SteamAppId.acf"
        if (Test-Path -LiteralPath $manifest)
        {
            $match = Select-String -LiteralPath $manifest -Pattern '^\s*"installdir"\s*"(.+)"\s*$' | Select-Object -First 1
            if ($match)
            {
                $candidate = Join-Path $library ("steamapps\common\" + $match.Matches[0].Groups[1].Value)
                if (Test-ZeroHourDir $candidate) { return $candidate }
            }
        }
        foreach ($name in $steamNames)
        {
            $candidate = Join-Path $library "steamapps\common\$name"
            if (Test-ZeroHourDir $candidate) { return $candidate }
        }
    }

    # 3. Almindelige standardstier
    $defaults = @(
        "${env:ProgramFiles}\EA Games\Command & Conquer Generals - Zero Hour",
        "${env:ProgramFiles(x86)}\EA Games\Command & Conquer Generals - Zero Hour",
        "${env:ProgramFiles(x86)}\EA Games\Command and Conquer Generals Zero Hour",
        "${env:ProgramFiles(x86)}\Origin Games\Command and Conquer Generals Zero Hour",
        "${env:ProgramFiles(x86)}\EA Games\Command & Conquer The First Decade\Command & Conquer(tm) Generals Zero Hour"
    )
    foreach ($candidate in $defaults)
    {
        if (Test-ZeroHourDir $candidate) { return $candidate }
    }
    return $null
}

function Test-WriteAccess([string]$Path)
{
    $probe = Join-Path $Path ".write_test_$([guid]::NewGuid())"
    try
    {
        New-Item -ItemType File -Path $probe -Force | Out-Null
        Remove-Item -LiteralPath $probe -Force
        return $true
    }
    catch
    {
        return $false
    }
}

function New-DesktopShortcut([string]$Name, [string]$TargetPath, [string]$WorkingDirectory)
{
    $shortcutPath = Join-Path ([Environment]::GetFolderPath("Desktop")) "$Name.lnk"
    $shell = New-Object -ComObject WScript.Shell
    $shortcut = $shell.CreateShortcut($shortcutPath)
    $shortcut.TargetPath = $TargetPath
    $shortcut.WorkingDirectory = $WorkingDirectory
    $shortcut.Save()
    Write-Info "Genvej oprettet: $shortcutPath"
}

# Installerer GenLauncher.exe i spilmappen, hvis den ikke allerede findes
function Install-GenLauncher([string]$TargetDir, [string]$WorkDir)
{
    $target = Join-Path $TargetDir "GenLauncher.exe"
    if (Test-Path -LiteralPath $target)
    {
        Write-Info "GenLauncher er allerede installeret - springer over."
        return
    }

    # Download-linket til nyeste version står i GenLauncher's eget manifest
    $url = $null
    try
    {
        $manifest = (Invoke-WebRequest -Uri $GenLauncherManifest -UseBasicParsing).Content
        $match = [regex]::Match($manifest, '(?m)^DownloadLink:\s*(\S+)')
        if ($match.Success)
        {
            $url = $match.Groups[1].Value
        }
    }
    catch
    {
        $url = $null
    }
    if (-not $url)
    {
        Write-Warn "Kunne ikke læse GenLauncher-manifestet - bruger fast link."
        $url = $GenLauncherFallbackUrl
    }

    Write-Info "Downloader GenLauncher fra $url ..."
    $zipPath = Join-Path $WorkDir "genlauncher.zip"
    try
    {
        Invoke-WebRequest -Uri $url -OutFile $zipPath -UseBasicParsing
    }
    catch
    {
        Write-Warn "GenLauncher-download fejlede - springer over. $($_.Exception.Message)"
        return
    }

    $extractDir = Join-Path $WorkDir "genlauncher"
    Expand-Archive -LiteralPath $zipPath -DestinationPath $extractDir -Force
    $exe = Join-Path $extractDir "GenLauncher.exe"
    if (-not (Test-Path -LiteralPath $exe))
    {
        Write-Warn "GenLauncher.exe mangler i arkivet - springer over."
        return
    }

    Copy-Item -LiteralPath $exe -Destination $target -Force
    Unblock-File -LiteralPath $target -ErrorAction SilentlyContinue
    Write-Info "GenLauncher installeret: $target"
    New-DesktopShortcut -Name "GenLauncher" -TargetPath $target -WorkingDirectory $TargetDir

    # GenLauncher kræver .NET Framework 4.8 (Release >= 528040)
    $ndp = Get-ItemProperty -Path "HKLM:\SOFTWARE\Microsoft\NET Framework Setup\NDP\v4\Full" -ErrorAction SilentlyContinue
    if (-not $ndp -or $ndp.Release -lt 528040)
    {
        Write-Warn "GenLauncher kræver .NET Framework 4.8: https://dotnet.microsoft.com/download/dotnet-framework/net48"
    }
}

# --- Find installationsmappe ---
if ($InstallDir)
{
    if (-not (Test-ZeroHourDir $InstallDir))
    {
        Stop-WithError "'$InstallDir' ligner ikke en Zero Hour-mappe ($MarkerFile mangler)."
    }
}
else
{
    Write-Info "Leder efter Zero Hour-installation..."
    $InstallDir = Find-ZeroHourDir
    if (-not $InstallDir)
    {
        Stop-WithError "Zero Hour blev ikke fundet. Angiv mappen: .\install-windows.ps1 -InstallDir `"C:\sti\til\Zero Hour`""
    }
}
Write-Info "Zero Hour-mappe: $InstallDir"

# --- Genstart som administrator hvis mappen er skrivebeskyttet (fx Program Files) ---
if (-not (Test-WriteAccess $InstallDir))
{
    $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if ($isAdmin)
    {
        Stop-WithError "Ingen skriveadgang til '$InstallDir', heller ikke som administrator."
    }
    if (-not $ScriptPath)
    {
        Stop-WithError "Ingen skriveadgang til '$InstallDir'. Kør install-windows.bat som administrator."
    }
    Write-Info "Kræver administratorrettigheder - genstarter som administrator..."
    $argList = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$ScriptPath`"", "-InstallDir", "`"$InstallDir`"", "-Repo", $Repo, "-Tag", $Tag, "-Config", "`"$Config`"")
    if ($IncludePdb) { $argList += "-IncludePdb" }
    if ($SkipGenLauncher) { $argList += "-SkipGenLauncher" }
    Start-Process -FilePath "powershell.exe" -ArgumentList $argList -Verb RunAs -Wait
    exit $LASTEXITCODE
}

# --- Download og udpak ---
$work = Join-Path ([IO.Path]::GetTempPath()) ("zh-superhackers-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $work | Out-Null
try
{
    $zipPath = Join-Path $work $Asset
    Write-Info "Downloader $Asset ..."
    try
    {
        Invoke-WebRequest -Uri $Url -OutFile $zipPath -UseBasicParsing
    }
    catch
    {
        Stop-WithError "Download fejlede: $Url`n$($_.Exception.Message)"
    }

    Write-Info "Pakker ud..."
    $extractDir = Join-Path $work "extracted"
    Expand-Archive -LiteralPath $zipPath -DestinationPath $extractDir -Force
    if (-not (Test-Path -LiteralPath (Join-Path $extractDir "generalszh.exe")))
    {
        Stop-WithError "generalszh.exe mangler i arkivet."
    }

    # --- Installer med backup af eksisterende filer ---
    $backupDir = Join-Path $InstallDir ("_backup_superhackers_" + (Get-Date -Format "yyyy-MM-dd_HHmmss"))
    $installed = 0
    foreach ($file in Get-ChildItem -LiteralPath $extractDir -File)
    {
        if (-not $IncludePdb -and $file.Extension -eq ".pdb")
        {
            continue
        }
        $target = Join-Path $InstallDir $file.Name
        if (Test-Path -LiteralPath $target)
        {
            if (-not (Test-Path -LiteralPath $backupDir))
            {
                New-Item -ItemType Directory -Path $backupDir | Out-Null
            }
            Copy-Item -LiteralPath $target -Destination (Join-Path $backupDir $file.Name) -Force
        }
        Copy-Item -LiteralPath $file.FullName -Destination $target -Force
        Unblock-File -LiteralPath $target -ErrorAction SilentlyContinue
        $installed++
    }

    Set-Content -LiteralPath (Join-Path $InstallDir "superhackers-version.txt") -Value "$Repo $Tag"

    Write-Info "Installeret $installed filer fra $Repo ($Tag)."
    Write-Warn "Alle spillere i et LAN-spil skal køre samme build ($Tag), ellers kommer der mismatch."
    if (Test-Path -LiteralPath $backupDir)
    {
        Write-Info "Backup af overskrevne filer: $backupDir"
    }

    # --- Genvej på skrivebordet ---
    $exePath = Join-Path $InstallDir "generalszh.exe"
    New-DesktopShortcut -Name "Zero Hour (SuperHackers)" -TargetPath $exePath -WorkingDirectory $InstallDir

    if (-not $SkipGenLauncher)
    {
        Install-GenLauncher -TargetDir $InstallDir -WorkDir $work
    }

    Write-Host ""
    Write-Info "Start spillet med: $exePath"
}
finally
{
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
