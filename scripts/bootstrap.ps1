<#
    bootstrap.ps1 - turnkey build of the GKrellM NVIDIA plugin for Windows.

    Downloads and configures everything that is needed, checks each
    prerequisite, compiles the plugin and installs it.

    Normal use is a double click on build.bat. Running the script directly:

        powershell -ExecutionPolicy Bypass -File scripts\bootstrap.ps1

    Switches:
        -DryRun        run every check, change nothing, download nothing
        -Force         redo steps whose output already exists
        -SkipInstall   build only, do not copy the DLL into the plugin folder
        -KeepWork      do not delete the work folder afterwards
        -Msys2Root     use an existing MSYS2 at this path
        -GkrellmDir    folder containing gkrellm.exe
        -TarballUrl    explicit URL of the GKrellM source tarball
        -Sha256        expected SHA256 of that tarball
#>

[CmdletBinding()]
param(
    [switch] $DryRun,
    [switch] $Force,
    [switch] $SkipInstall,
    [switch] $KeepWork,
    [string] $Msys2Root  = "",
    [string] $GkrellmDir = "",
    [string] $TarballUrl = "",
    [string] $Sha256     = ""
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'

# --------------------------------------------------------------------------
# Paths and logging
# --------------------------------------------------------------------------

$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir
$WorkDir     = Join-Path $ProjectRoot 'work'
$BuildDir    = Join-Path $ProjectRoot 'build'
$LogFile     = Join-Path $ProjectRoot 'build-log.txt'

$script:Revision  = 'r7 (2026-07-28)'
$script:StepNo    = 0
$script:StepTotal = 11
$script:Started   = Get-Date

function Write-Log {
    param([string] $Text, [string] $Colour = 'Gray')
    Write-Host $Text -ForegroundColor $Colour
    try { Add-Content -Path $LogFile -Value $Text -Encoding UTF8 } catch { }
}

function Step {
    param([string] $Title)
    $script:StepNo++
    Write-Log ""
    Write-Log ("[{0}/{1}] {2}" -f $script:StepNo, $script:StepTotal, $Title) 'Cyan'
}

function Info { param([string] $T) Write-Log ("      " + $T) 'Gray' }
function Good { param([string] $T) Write-Log ("      OK   " + $T) 'Green' }
function Warn { param([string] $T) Write-Log ("      WARN " + $T) 'Yellow' }

function Fail {
    param([string] $Problem, [string] $Remedy = "")
    Write-Log ""
    Write-Log "==========================================================" 'Red'
    Write-Log " BUILD FAILED" 'Red'
    Write-Log "==========================================================" 'Red'
    Write-Log ""
    Write-Log ("  Problem: " + $Problem) 'Red'
    if ($Remedy) {
        Write-Log ""
        Write-Log "  What to do:" 'Yellow'
        foreach ($line in ($Remedy -split "`n")) { Write-Log ("    " + $line) 'Yellow' }
    }
    Write-Log ""
    Write-Log ("  Full transcript: " + $LogFile) 'Gray'
    Write-Log ""
    exit 1
}

# --------------------------------------------------------------------------

"" | Set-Content -Path $LogFile -Encoding UTF8
Write-Log "==========================================================" 'White'
Write-Log " GKrellM NVIDIA plugin - turnkey build" 'White'
Write-Log (" script " + $script:Revision) 'White'
Write-Log (" started " + $script:Started.ToString("yyyy-MM-dd HH:mm:ss")) 'White'
Write-Log "==========================================================" 'White'
if ($DryRun) { Write-Log ""; Warn "DRY RUN - nothing will be downloaded, installed or changed." }

# ==========================================================================
Step "Checking the machine"
# ==========================================================================

$os = Get-CimInstance Win32_OperatingSystem
Info ("Windows      : " + $os.Caption + " (build " + $os.BuildNumber + ")")

if ([int]$os.BuildNumber -lt 7600) {
    Fail "Windows 7 or newer is required." "GKrellM 2.5.1 for Windows does not support older releases."
}

if (-not [Environment]::Is64BitOperatingSystem) {
    Fail "A 64-bit version of Windows is required." @"
GKrellM for Windows has been 64-bit only since version 2.4.0.
The last 32-bit release was 2.3.11.
"@
}
Good "64-bit Windows"

Info ("PowerShell   : " + $PSVersionTable.PSVersion.ToString())
if ($PSVersionTable.PSVersion.Major -lt 5) {
    Fail "PowerShell 5.0 or newer is required." "Install Windows Management Framework 5.1."
}

$drive = (Get-Item $ProjectRoot).PSDrive.Name
$free  = (Get-PSDrive $drive).Free
Info ("Free space   : {0:N1} GB on {1}:" -f ($free / 1GB), $drive)
if ($free -lt 3GB) {
    Fail ("Not enough free disk space on drive {0}: ({1:N1} GB available)." -f $drive, ($free / 1GB)) `
         "About 3 GB is needed: roughly 2 GB for the MSYS2 toolchain and the rest for downloads and build output."
}
Good "enough free disk space"

try {
    [Net.ServicePointManager]::SecurityProtocol = `
        [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
} catch { }

if (-not $DryRun) {
    try {
        $null = Invoke-WebRequest -Uri 'https://gkrellm.srcbox.net/' -UseBasicParsing -TimeoutSec 20 -Method Head
        Good "internet reachable"
    } catch {
        Fail "Cannot reach gkrellm.srcbox.net." @"
Check your internet connection.
Behind a corporate proxy, set it first, for example:
    netsh winhttp set proxy proxy.example.com:8080
and make sure PowerShell can use it.
"@
    }
}

# ==========================================================================
Step "Locating GKrellM"
# ==========================================================================

function Find-Gkrellm {
    if ($GkrellmDir) {
        $c = Join-Path $GkrellmDir 'gkrellm.exe'
        if (Test-Path $c) { return $c }
        Fail ("gkrellm.exe not found in the folder given with -GkrellmDir: " + $GkrellmDir)
    }

    $candidates = @()
    foreach ($pair in @(
            @($env:ProgramFiles,        'GKrellM\gkrellm.exe'),
            @(${env:ProgramFiles(x86)}, 'GKrellM\gkrellm.exe'),
            @($env:LOCALAPPDATA,        'Programs\GKrellM\gkrellm.exe'))) {
        if ($pair[0]) { $candidates += (Join-Path $pair[0] $pair[1]) }
    }

    $keys = @(
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*',
        'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*'
    )
    foreach ($k in $keys) {
        try {
            Get-ItemProperty $k -ErrorAction SilentlyContinue |
                Where-Object { $_.DisplayName -like '*GKrellM*' } |
                ForEach-Object {
                    if ($_.PSObject.Properties.Name -contains 'InstallLocation' -and $_.InstallLocation) {
                        $candidates += (Join-Path $_.InstallLocation 'gkrellm.exe')
                    }
                }
        } catch { }
    }

    foreach ($c in $candidates) {
        if ($c -and (Test-Path $c)) { return (Resolve-Path $c).Path }
    }

    # Nothing in the usual places. Sweep the likely roots on every fixed
    # drive before giving up. Bounded by depth and by a wall clock budget so
    # this can never turn into a multi-minute scan of the whole disk.
    Info "not in the standard locations - searching the disks"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $budget = [TimeSpan]::FromSeconds(45)

    $roots = @()
    foreach ($d in (Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue)) {
        if (-not $d.Free) { continue }
        $r = $d.Root
        foreach ($sub in @('Program Files', 'Program Files (x86)', 'Bin', 'Tools',
                           'Apps', 'Programs', 'PortableApps', 'Util', 'Utils')) {
            $p = Join-Path $r $sub
            if (Test-Path $p) { $roots += $p }
        }
        $roots += $r
    }

    foreach ($root in $roots) {
        if ($sw.Elapsed -gt $budget) {
            Warn "search budget exhausted; pass the folder with -GkrellmDir"
            break
        }
        $depth = 3
        if ($root -match '^[A-Za-z]:\\$') { $depth = 2 }
        try {
            $hit = Get-ChildItem -Path $root -Filter 'gkrellm.exe' -File -Recurse `
                        -Depth $depth -Force -ErrorAction SilentlyContinue |
                   Select-Object -First 1
            if ($hit) {
                Info ("found by search: " + $hit.FullName)
                return $hit.FullName
            }
        } catch { }
    }

    return $null
}

$GkrellmExe = Find-Gkrellm
if (-not $GkrellmExe) {
    Fail "GKrellM for Windows is not installed (gkrellm.exe was not found)." @"
Install it first, then run this script again:
    https://www.srcbox.net/projects/gkrellm/

Version 2.5.0 or newer is required, because only those releases export
the plugin API from gkrellm.exe.

If it is installed in an unusual place, pass the folder explicitly:
    build.bat -GkrellmDir "D:\Tools\GKrellM"
"@
}

$GkrellmHome = Split-Path -Parent $GkrellmExe
Good ("gkrellm.exe  : " + $GkrellmExe)

$verInfo = (Get-Item $GkrellmExe).VersionInfo
$verText = $verInfo.ProductVersion
if (-not $verText) { $verText = $verInfo.FileVersion }
if ($verText) {
    Info ("version      : " + $verText)
    $m = [regex]::Match($verText, '(\d+)\.(\d+)\.(\d+)')
    if ($m.Success) {
        $maj = [int]$m.Groups[1].Value; $min = [int]$m.Groups[2].Value
        if ($maj -lt 2 -or ($maj -eq 2 -and $min -lt 5)) {
            Fail ("GKrellM " + $verText + " is too old.") @"
This plugin needs GKrellM 2.5.0 or newer. Earlier Windows builds did not
export their API from gkrellm.exe, so no plugin can link against them.
Upgrade at https://www.srcbox.net/projects/gkrellm/
"@
        }
    }
} else {
    Warn "gkrellm.exe carries no version resource; the export check in step 9 will decide."
}

$PluginDir = Join-Path $env:USERPROFILE '.gkrellm2\plugins'
Info ("plugin folder: " + $PluginDir)

# ==========================================================================
Step "Checking for an NVIDIA GPU"
# ==========================================================================
# Not fatal: the plugin is meant to be built on one machine and possibly used
# on another, and it degrades silently at runtime anyway.

$nvsmi = $null
foreach ($p in @(
        (Join-Path $env:SystemRoot 'System32\nvidia-smi.exe'),
        (Join-Path $env:ProgramFiles 'NVIDIA Corporation\NVSMI\nvidia-smi.exe'))) {
    if (Test-Path $p) { $nvsmi = $p; break }
}

if ($nvsmi) {
    Good ("nvidia-smi   : " + $nvsmi)
    if (-not $DryRun) {
        try {
            $names = & $nvsmi --query-gpu=index,name --format=csv,noheader 2>$null
            foreach ($line in $names) { if ($line.Trim()) { Info ("GPU          : " + $line.Trim()) } }
        } catch { Warn "nvidia-smi could not be queried; continuing anyway." }
    }
} else {
    Warn "nvidia-smi.exe not found - no NVIDIA driver on this machine."
    Warn "The plugin will still build. At runtime it simply shows nothing."
}

# ==========================================================================
Step "Locating or installing MSYS2"
# ==========================================================================

function Find-Msys2 {
    if ($Msys2Root) {
        if (Test-Path (Join-Path $Msys2Root 'usr\bin\bash.exe')) { return $Msys2Root }
        Fail ("No MSYS2 found at the path given with -Msys2Root: " + $Msys2Root)
    }
    # Enumerate the drives that actually exist. Join-Path raises a terminating
    # DriveNotFound error for a hard coded path on a drive letter that is not
    # present, so no candidate list may be written by hand.
    $cands = @()
    if ($env:LOCALAPPDATA) { $cands += (Join-Path $env:LOCALAPPDATA 'msys64') }
    foreach ($d in (Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue)) {
        if (-not $d.Free) { continue }
        $cands += ($d.Root + 'msys64')
        $cands += ($d.Root + 'msys2')
    }

    foreach ($p in $cands) {
        try {
            if (Test-Path (Join-Path $p 'usr\bin\bash.exe')) { return $p }
        } catch { }
    }
    return $null
}

$Msys = Find-Msys2

if ($Msys -and -not $Force) {
    Good ("MSYS2        : " + $Msys)
}
elseif ($DryRun) {
    Warn "MSYS2 is not installed; it would be downloaded and installed (about 100 MB)."
    $Msys = 'C:\msys64'
}
else {
    Info "MSYS2 is not installed. Fetching the current installer."

    New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null

    $installerUrl  = ''
    $installerName = ''
    $installerSize = 0

    # Primary source: the canonical always-current installer on the MSYS2
    # package repository. This URL is stable and needs no API call.
    $canonical = 'https://repo.msys2.org/distrib/msys2-x86_64-latest.exe'
    try {
        $head = Invoke-WebRequest -Uri $canonical -UseBasicParsing -TimeoutSec 30 -Method Head
        $installerUrl  = $canonical
        $installerName = 'msys2-x86_64-latest.exe'
        try { $installerSize = [int64]$head.Headers['Content-Length'] } catch { $installerSize = 0 }
        Good "installer source: repo.msys2.org"
    } catch {
        Warn ("repo.msys2.org is not reachable: " + $_.Exception.Message)
    }

    # Fallback: whatever the newest GitHub release offers.
    if (-not $installerUrl) {
        Info "falling back to the GitHub release list"
        try {
            $rel = Invoke-RestMethod -Uri 'https://api.github.com/repos/msys2/msys2-installer/releases/latest' `
                                     -UseBasicParsing -TimeoutSec 30 `
                                     -Headers @{ 'User-Agent' = 'gkrellm-gpu-bootstrap' }

            $names = @()
            foreach ($a in $rel.assets) { $names += $a.name }

            $asset = $rel.assets |
                     Where-Object { $_.name -like 'msys2-x86_64-*.exe' -and $_.name -notlike '*.sig' } |
                     Select-Object -First 1
            if (-not $asset) {
                $asset = $rel.assets |
                         Where-Object { $_.name -like 'msys2-base-x86_64-*.sfx.exe' } |
                         Select-Object -First 1
            }

            if (-not $asset) {
                Fail "No usable MSYS2 installer in the latest GitHub release." @"
Assets that were offered:
$( if ($names.Count) { '    ' + ($names -join "`n    ") } else { '    (none)' } )

Install MSYS2 by hand from https://www.msys2.org/ and re-run:
    build.bat -Msys2Root C:\msys64
"@
            }
            $installerUrl  = $asset.browser_download_url
            $installerName = $asset.name
            $installerSize = $asset.size
            Good ("installer source: GitHub (" + $installerName + ")")
        } catch {
            Fail "Could not obtain the MSYS2 installer from either source." @"
$($_.Exception.Message)

Install MSYS2 by hand from https://www.msys2.org/ and re-run:
    build.bat -Msys2Root C:\msys64
"@
        }
    }

    $installer = Join-Path $WorkDir $installerName

    if ($installerSize -gt 0) {
        Info ("downloading  : " + $installerName + (" ({0:N0} MB)" -f ($installerSize / 1MB)))
    } else {
        Info ("downloading  : " + $installerName)
    }
    try {
        Invoke-WebRequest -Uri $installerUrl -OutFile $installer -UseBasicParsing -TimeoutSec 900
    } catch {
        Fail "Download of the MSYS2 installer failed." $_.Exception.Message
    }
    Good "installer downloaded"

    $Msys = 'C:\msys64'
    Info "installing to C:\msys64 (this takes a few minutes)"
    $p = Start-Process -FilePath $installer `
                       -ArgumentList @('in', '--confirm-command', '--accept-messages', '--root', 'C:/msys64') `
                       -Wait -PassThru
    if ($p.ExitCode -ne 0) {
        Fail ("The MSYS2 installer exited with code " + $p.ExitCode + ".") @"
This is usually caused by antivirus software, AppLocker or a group policy
blocking the unattended install.

Install MSYS2 by hand from https://www.msys2.org/ and re-run:
    build.bat -Msys2Root C:\msys64
"@
    }
    if (-not (Test-Path (Join-Path $Msys 'usr\bin\bash.exe'))) {
        Fail "The MSYS2 installer reported success but C:\msys64\usr\bin\bash.exe does not exist."
    }
    Good "MSYS2 installed"
}

$Bash = Join-Path $Msys 'usr\bin\bash.exe'

function Invoke-Msys {
    param(
        [string] $Command,
        [string] $Subsystem = 'MSYS',
        [switch] $AllowFailure
    )
    $env:MSYSTEM        = $Subsystem
    $env:CHERE_INVOKING = '1'

    Write-Log ("      $ " + $Command) 'DarkGray'

    # Native programs write progress and warnings to stderr as a matter of
    # course. With $ErrorActionPreference = 'Stop' PowerShell 5.1 promotes
    # any such line into a terminating NativeCommandError, which would abort
    # the build on, for example, pacman's entirely normal
    # "terminate other MSYS2 programs before proceeding" notice.
    # Merge stderr into stdout inside bash itself. If PowerShell were the one
    # doing the redirection, every stderr line would arrive as an ErrorRecord
    # and, under $ErrorActionPreference = 'Stop', abort the build on messages
    # as harmless as pacman's "package is up to date -- skipping".
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out  = & $Bash -lc ("exec 2>&1; " + $Command)
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $prevEap
    }

    foreach ($line in $out) {
        if ($line -is [Management.Automation.ErrorRecord]) {
            $text = $line.ToString()
        } else {
            $text = [string] $line
        }
        Write-Log ("      | " + $text) 'DarkGray'
    }

    if ($code -ne 0 -and -not $AllowFailure) {
        Fail ("A command inside MSYS2 failed with exit code " + $code + ":`n    " + $Command) `
             "The output above is also in $LogFile."
    }
    return $code
}

function Get-MsysPath {
    param([string] $WinPath)
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $r = & $Bash -lc ("cygpath -u '" + $WinPath.Replace("'", "'\''") + "'") 2>$null
    } finally {
        $ErrorActionPreference = $prevEap
    }
    return (("" + $r).Trim())
}

# ==========================================================================
Step "Updating the MSYS2 package database"
# ==========================================================================

if ($DryRun) {
    Warn "would run: pacman -Syuu (twice)"
} else {
    # The first pass may replace pacman itself and terminate the shell; that
    # is expected and is the single most common reason unattended MSYS2
    # scripting breaks. Run it twice and tolerate a non-zero first exit.
    Info "first pass (may restart itself - this is normal)"
    Invoke-Msys 'pacman -Syuu --noconfirm --needed' -AllowFailure | Out-Null
    Info "second pass"
    Invoke-Msys 'pacman -Syuu --noconfirm --needed' | Out-Null
    Good "package database up to date"
}

# ==========================================================================
Step "Installing the MINGW64 toolchain"
# ==========================================================================

$packages = @(
    'mingw-w64-x86_64-gcc',
    'mingw-w64-x86_64-pkgconf',
    'mingw-w64-x86_64-gtk2',
    'mingw-w64-x86_64-binutils',
    'make', 'tar', 'bzip2'
)

if ($DryRun) {
    Warn ("would install: " + ($packages -join ' '))
} else {
    Info "checking package availability"
    $missing = @()
    foreach ($pkg in $packages) {
        $rc = Invoke-Msys ("pacman -Si " + $pkg + " >/dev/null 2>&1") -AllowFailure
        if ($rc -ne 0) { $missing += $pkg }
    }
    if ($missing.Count -gt 0) {
        Fail ("These packages are not in the MSYS2 repositories: " + ($missing -join ', ')) @"
GTK+ 2 is deprecated and may eventually be dropped from MSYS2.
If mingw-w64-x86_64-gtk2 is the missing one, the plugin cannot be built
with MSYS2 any more and cross-compiling with MXE is the remaining option.
"@
    }
    Good "all packages available"

    Info ("installing: " + ($packages -join ' '))
    Invoke-Msys ('pacman -S --noconfirm --needed ' + ($packages -join ' ')) | Out-Null
    Good "toolchain installed"
}

# ==========================================================================
Step "Downloading the GKrellM source tarball"
# ==========================================================================
# Only the headers are needed. They are not part of the binary release, so
# the matching source tarball has to be fetched.

New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null

if (-not $TarballUrl) {
    if ($DryRun) {
        $TarballUrl = 'https://gkrellm.srcbox.net/releases/gkrellm-2.5.1.tar.bz2'
        Warn "would discover the tarball URL from gkrellm.srcbox.net"
    } else {
        Info "discovering the current release from gkrellm.srcbox.net"
        try {
            $page = Invoke-WebRequest -Uri 'https://gkrellm.srcbox.net/' -UseBasicParsing -TimeoutSec 30
            $m = [regex]::Match($page.Content, 'href\s*=\s*["'']([^"'']*gkrellm-(\d+\.\d+\.\d+)\.tar\.bz2)["'']')
            if (-not $m.Success) {
                Fail "Could not find a source tarball link on the GKrellM home page." `
                     "Pass one explicitly: build.bat -TarballUrl <url>"
            }
            $href = $m.Groups[1].Value
            if ($href -match '^https?://') { $TarballUrl = $href }
            elseif ($href.StartsWith('/'))  { $TarballUrl = 'https://gkrellm.srcbox.net' + $href }
            else                            { $TarballUrl = 'https://gkrellm.srcbox.net/' + $href }
        } catch {
            Fail "Could not read the GKrellM home page." $_.Exception.Message
        }
    }
}

$TarballName = Split-Path -Leaf ($TarballUrl -split '\?')[0]
$TarballPath = Join-Path $WorkDir $TarballName
Info ("url          : " + $TarballUrl)

if ($DryRun) {
    Warn "would download the tarball (about 650 KB)"
}
elseif ((Test-Path $TarballPath) -and -not $Force) {
    Good ("already downloaded: " + $TarballName)
}
else {
    try {
        Invoke-WebRequest -Uri $TarballUrl -OutFile $TarballPath -UseBasicParsing -TimeoutSec 300
    } catch {
        Fail ("Download failed: " + $TarballUrl) $_.Exception.Message
    }
    Good ("downloaded " + $TarballName)
}

if (-not $DryRun) {
    $hash = (Get-FileHash -Path $TarballPath -Algorithm SHA256).Hash.ToLower()
    if ($Sha256) {
        if ($hash -ne $Sha256.ToLower()) {
            Fail "SHA256 mismatch on the downloaded tarball." @"
expected : $($Sha256.ToLower())
actual   : $hash

The download is corrupt or has been tampered with. Delete
    $TarballPath
and try again.
"@
        }
        Good ("SHA256 verified: " + $hash)
    } else {
        Info ("SHA256       : " + $hash)
        Warn "No expected checksum was supplied, so integrity rests on HTTPS alone."
        Warn "To pin it, compare with the value published by the project and re-run"
        Warn "  build.bat -Sha256 <value>"
    }
}

# ==========================================================================
Step "Unpacking the headers"
# ==========================================================================

$SourceTree = ""
if ($DryRun) {
    Warn "would unpack the tarball into work\"
    $SourceTree = Join-Path $WorkDir 'gkrellm-2.5.1'
} else {
    $wu = Get-MsysPath $WorkDir
    $tu = Get-MsysPath $TarballPath
    Invoke-Msys ("cd '$wu' && tar xjf '$tu'") | Out-Null

    $dirs = Get-ChildItem -Path $WorkDir -Directory | Where-Object { $_.Name -like 'gkrellm-*' }
    if (-not $dirs) { Fail "The tarball did not contain a gkrellm-* directory." }
    $SourceTree = $dirs[0].FullName
    Good ("source tree  : " + (Split-Path -Leaf $SourceTree))

    # Headers are spread across src/ and shared/, so search rather than assume.
    foreach ($h in @('gkrellm.h', 'gkrellm-public-proto.h', 'gkrellm-visibility.h', 'log.h')) {
        $hit = Get-ChildItem -Path $SourceTree -Filter $h -File -Recurse -ErrorAction SilentlyContinue |
               Select-Object -First 1
        if (-not $hit) {
            if ($h -eq 'gkrellm-visibility.h') {
                Fail "gkrellm-visibility.h is nowhere in the source tree." @"
That header appeared in GKrellM 2.5.0. Older releases used a callback table
instead of dllimport and are not supported by this plugin.
"@
            }
            Fail ("Required header not found anywhere in the source tree: " + $h)
        }
        $rel = $hit.FullName.Substring($SourceTree.Length).TrimStart('\')
        Info ("found: " + $rel)
    }
    Good "all required headers present"
}

# ==========================================================================
Step "Compiling"
# ==========================================================================

if ($DryRun) {
    Warn "would run scripts/compile.sh inside the MINGW64 environment"
} else {
    if ((Test-Path $BuildDir) -and $Force) { Remove-Item -Recurse -Force $BuildDir }
    New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null


    # A browser download or a copy/paste round trip turns the shell script
    # into CRLF, and bash then fails with "$'\r': command not found".
    # Normalise it every time; the operation is idempotent.
    $shPath = Join-Path $ScriptDir 'compile.sh'
    if (-not (Test-Path $shPath)) {
        Fail ("scripts\compile.sh is missing from " + $ScriptDir) @"
The project must keep its folder layout:
    build.bat
    scripts\bootstrap.ps1
    scripts\compile.sh
    src\gkrellm-gpu.c
"@
    }
    try {
        $raw = [IO.File]::ReadAllText($shPath)
        if ($raw -match "`r`n") {
            [IO.File]::WriteAllText($shPath, ($raw -replace "`r`n", "`n"),
                                    (New-Object Text.UTF8Encoding $false))
            Good "compile.sh converted to Unix line endings"
        }
    } catch {
        Warn ("Could not normalise compile.sh line endings: " + $_.Exception.Message)
    }

    if (-not (Test-Path (Join-Path $ProjectRoot 'src\gkrellm-gpu.c'))) {
        Fail "src\gkrellm-gpu.c is missing." "Restore the project folder layout and run again."
    }

    $cmd = "cd '" + (Get-MsysPath $ProjectRoot) + "' && bash scripts/compile.sh" +
           " --gkrellm-exe '" + (Get-MsysPath $GkrellmExe)  + "'" +
           " --source-tree '" + (Get-MsysPath $SourceTree)  + "'" +
           " --project '"     + (Get-MsysPath $ProjectRoot) + "'" +
           " --out '"         + (Get-MsysPath $BuildDir)    + "'"

    Invoke-Msys $cmd -Subsystem 'MINGW64' | Out-Null

    $dll = Join-Path $BuildDir 'gkrellm-gpu.dll'
    if (-not (Test-Path $dll)) { Fail "compile.sh reported success but the DLL is missing." }
    Good ("built: " + $dll + (" ({0:N0} KB)" -f ((Get-Item $dll).Length / 1KB)))
}

# ==========================================================================
Step "Installing the plugin"
# ==========================================================================

if ($DryRun -or $SkipInstall) {
    Warn ("would copy the DLL into " + $PluginDir)
} else {
    New-Item -ItemType Directory -Force -Path $PluginDir | Out-Null
    $target = Join-Path $PluginDir 'gkrellm-gpu.dll'

    $running = Get-Process -Name 'gkrellm' -ErrorAction SilentlyContinue
    if ($running) {
        Warn "GKrellM is running; the plugin file is probably locked."
        Warn "Close GKrellM and re-run, or copy the DLL by hand afterwards."
    }

    try {
        Copy-Item -Path (Join-Path $BuildDir 'gkrellm-gpu.dll') -Destination $target -Force
        Good ("installed: " + $target)
    } catch {
        Fail "Could not copy the plugin into the plugin folder." @"
$($_.Exception.Message)

Close GKrellM and run the script again, or copy the file manually:
    from: $BuildDir\gkrellm-gpu.dll
    to  : $PluginDir
"@
    }
}

# ==========================================================================
Step "Finishing"
# ==========================================================================

if (-not $KeepWork -and -not $DryRun -and (Test-Path $WorkDir)) {
    try { Remove-Item -Recurse -Force $WorkDir; Good "work folder removed" }
    catch { Warn "Could not remove the work folder; it can be deleted by hand." }
} else {
    Info ("work folder kept: " + $WorkDir)
}

$elapsed = (Get-Date) - $script:Started
Write-Log ""
Write-Log "==========================================================" 'Green'
Write-Log " DONE" 'Green'
Write-Log "==========================================================" 'Green'
Write-Log ("  elapsed: {0:mm\:ss}" -f $elapsed) 'Gray'
Write-Log ("  log    : " + $LogFile) 'Gray'
Write-Log ""
if (-not $DryRun -and -not $SkipInstall) {
    Write-Log "  Next:" 'White'
    Write-Log "    1. Start GKrellM (restart it if it was already running)." 'White'
    Write-Log "    2. Right click the panel -> Configuration -> Plugins." 'White'
    Write-Log "    3. Enable 'NVIDIA'." 'White'
    Write-Log ""
    Write-Log "  If the plugin does not appear in that list, this machine has no" 'Gray'
    Write-Log "  NVIDIA driver. That is intentional: the plugin unloads itself" 'Gray'
    Write-Log "  silently instead of showing an empty panel." 'Gray'
    Write-Log ""
}
