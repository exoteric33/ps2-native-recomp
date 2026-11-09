# Fetches a portable toolchain into one folder, so scripts/port.sh runs on a Windows PC that has nothing installed:
# no Git, Python, compiler or CMake, and no administrator rights. Downloads only from the official release pages,
# checks every file against a pinned SHA-256, unpacks the zips and writes env.ps1 + shell.cmd. Safe to run again:
# whatever is already there and valid is skipped. Details: docs/portable.md
#
#   powershell -ExecutionPolicy Bypass -File scripts\portable.ps1             download and unpack what is missing
#   powershell -ExecutionPolicy Bypass -File scripts\portable.ps1 -Check      only report what is there (exit 1 if not all)
#   tools\portable\shell.cmd                                                  a window with only these tools on the PATH
#
# -Dir        where to put it (default: <repo>\tools\portable, which git ignores)
# -Force      download and unpack everything again
# -KeepArchives  keep the downloaded zips in <Dir>\downloads (default: delete each after unpacking)
# -NoTar      unpack with .NET instead of tar.exe (slower; for a Windows 10 that has no tar.exe)
# -ForApp     also print "@app download <bytes done> <bytes total>" about twice a second while a file downloads, which
#             the Windows app (app\app.ps1) reads for its progress bar. Without it the output is unchanged.
#
# The app also dot-sources this file (. scripts\portable.ps1) to read $tools and call Is-Installed; that defines the
# functions and runs nothing.

param(
    [string]$Dir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'tools\portable'),
    [switch]$Check,
    [switch]$Force,
    [switch]$KeepArchives,
    [switch]$NoTar,
    [switch]$ForApp
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2
$ProgressPreference = 'SilentlyContinue'

# Every tool is one official zip. Probe is a file that must exist after unpacking; Top is the single folder the zip
# wraps its files in (empty when the files sit at the top of the zip).
$tools = @(
    @{ Name = 'llvm-mingw'; Title = 'llvm-mingw 20260908 (clang 23.1.1, UCRT, x86_64)'
       File = 'llvm-mingw-20260908-ucrt-x86_64.zip'
       Url  = 'https://github.com/mstorsjo/llvm-mingw/releases/download/20260908/llvm-mingw-20260908-ucrt-x86_64.zip'
       Sha256 = '1bcf74d06b724aeecaa6412ca85f5b26fb1da770e7cdcefa9263c9c5c3ad34b6'; Size = 190677197
       Top = 'llvm-mingw-20260908-ucrt-x86_64'; Probe = 'bin\clang++.exe' },
    @{ Name = 'cmake'; Title = 'CMake 4.4.4'
       File = 'cmake-4.4.4-windows-x86_64.zip'
       Url  = 'https://github.com/Kitware/CMake/releases/download/v4.4.4/cmake-4.4.4-windows-x86_64.zip'
       Sha256 = 'bace36e94b31c68ab6fa295f26dfa11219e0701cf7c94b0284a7d1cb13dac536'; Size = 54400562
       Top = 'cmake-4.4.4-windows-x86_64'; Probe = 'bin\cmake.exe' },
    @{ Name = 'ninja'; Title = 'Ninja 1.13.2'
       File = 'ninja-win.zip'
       Url  = 'https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip'
       Sha256 = '07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65'; Size = 291570
       Top = ''; Probe = 'ninja.exe' },
    @{ Name = 'python'; Title = 'Python 3.13.16 (embeddable package, amd64)'
       File = 'python-3.13.16-embed-amd64.zip'
       Url  = 'https://www.python.org/ftp/python/3.13.16/python-3.13.16-embed-amd64.zip'
       Sha256 = '97dae5274cc54867065e8d5a3226e48c35017ed332a0fdb0e27d5b5821961297'; Size = 11439094
       Top = ''; Probe = 'python.exe' },
    @{ Name = 'mingit'; Title = 'MinGit 2.56.0.2 (64-bit): bash and the Unix tools'
       File = 'MinGit-2.56.0.2-64-bit.zip'
       Url  = 'https://github.com/git-for-windows/git/releases/download/v2.56.0.windows.2/MinGit-2.56.0.2-64-bit.zip'
       Sha256 = 'da35e72aa21c005a5a0d298cfbae110bc1609a815730ea0dde84b01a1b3cd3be'; Size = 39806486
       Top = ''; Probe = 'usr\bin\sh.exe' },
    # One exe, not a zip. MinGit lacks a few small commands the scripts use (sleep, tee, cksum, stat, ln, realpath,
    # sha256sum, ...); BusyBox has them all. Each name in Applets becomes a copy of busybox.exe (it works out what to do
    # from its own file name). The folder comes after MinGit on the PATH, so MinGit's GNU tools win where both exist.
    @{ Name = 'busybox'; Title = 'BusyBox for Windows FRP-6075 (64-bit): the small Unix commands MinGit lacks'
       File = 'busybox-w64u-FRP-6075-g169694ebd.exe'
       Url  = 'https://frippery.org/files/busybox/busybox-w64u-FRP-6075-g169694ebd.exe'
       Sha256 = '6e263d154d8548d1eb936f65d1d8312c80df31c45974e48d6335e4dcc0f4f34c'; Size = 675840
       Single = 'busybox.exe'; Top = ''; Probe = 'busybox.exe'
       Applets = @('ln', 'realpath', 'sha256sum', 'sha1sum', 'md5sum', 'cksum', 'sleep', 'tee', 'stat', 'seq', 'nproc') }
)
# Space to have free on the drive: the zips plus the unpacked tools (measured 1.25 GB) plus room for wgpu and SDL2.
$neededBytes = 2GB

function Say([string]$text) { Write-Host $text }
function Fail([string]$text) { throw $text }
function Mb([double]$bytes) { '{0:0.#}' -f ($bytes / 1MB) }

$Dir = [IO.Path]::GetFullPath($Dir)
$archives = Join-Path $Dir 'downloads'
$scratch = Join-Path $Dir '.unpack'
$repo = Split-Path -Parent $PSScriptRoot

function Marker($tool) { Join-Path (Join-Path $Dir $tool.Name) '.installed' }
# What the marker file must say for a tool to count as installed: the hash, and for BusyBox also its list of commands.
function Stamp($tool) {
    if ($tool.ContainsKey('Applets')) { return "$($tool.Sha256) $($tool.Applets -join ',')" }
    return $tool.Sha256
}
function Is-Installed($tool) {
    $marker = Marker $tool
    (Test-Path -LiteralPath $marker) -and
        ((Get-Content -LiteralPath $marker -Raw).Trim() -eq (Stamp $tool)) -and
        (Test-Path -LiteralPath (Join-Path (Join-Path $Dir $tool.Name) $tool.Probe))
}

# Downloads $tool.Url to $dest. GitHub's servers time out now and then for some players, so a failed try is repeated (5 tries,
# 5 seconds apart; every try continues the half-finished file). Only when the last one fails does the script stop, and then
# it says how to fetch the file by hand (docs\fetch-retry.md).
function Download-File($tool, [string]$dest) {
    $tries = 5
    for ($try = 1; $try -le $tries; $try++) {
        try { Download-Once $tool $dest; return }
        catch {
            if ($try -lt $tries) {
                Say "  $(($_.Exception.Message -split "`n")[0].Trim())"
                Say "  trying again in 5 seconds (try $($try + 1) of $tries)"
                Start-Sleep -Seconds 5
                continue
            }
            $hand = Join-Path $repo 'downloads'
            Fail ($_.Exception.Message + "`n  To get it by hand: open $($tool.Url) in a browser, save the file as $($tool.File), " +
                  "put it into $hand, then run this again (in the app: press Try again).")
        }
    }
}

# One try to download $tool.Url to $dest, continuing a half-finished <dest>.part when the server allows it.
function Download-Once($tool, [string]$dest) {
    $part = "$dest.part"
    $total = [int64]$tool.Size
    $have = 0
    if (Test-Path -LiteralPath $part) { $have = (Get-Item -LiteralPath $part).Length }
    if ($have -gt $total) { Remove-Item -LiteralPath $part -Force; $have = 0 }
    if ($have -eq $total) { Move-Item -LiteralPath $part -Destination $dest -Force; return }

    $request = [Net.HttpWebRequest]::Create($tool.Url)
    $request.UserAgent = 'ps2-native-recomp-portable'
    $request.Proxy = [Net.WebRequest]::GetSystemWebProxy()
    $request.Proxy.Credentials = [Net.CredentialCache]::DefaultCredentials
    $request.ReadWriteTimeout = 60000
    $request.Timeout = 60000
    if ($have -gt 0) { $request.AddRange([int64]$have) }
    try { $response = $request.GetResponse() }
    catch {
        $detail = $_.Exception.Message
        if ($_.Exception.InnerException) { $detail = $_.Exception.InnerException.Message }
        Fail "Could not reach $($tool.Url)`n  $detail"
    }
    try {
        $append = ($have -gt 0) -and ([int]$response.StatusCode -eq 206)
        if (-not $append) { $have = 0 }
        $mode = if ($append) { [IO.FileMode]::Append } else { [IO.FileMode]::Create }
        $stream = $response.GetResponseStream()
        $output = New-Object IO.FileStream($part, $mode, [IO.FileAccess]::Write)
        try {
            $buffer = New-Object byte[] (1MB)
            $done = $have
            $nextReport = [Math]::Floor($done * 10 / $total) + 1
            $clock = [Diagnostics.Stopwatch]::StartNew()
            $lastBytes = -1000
            if ($have -gt 0) { Say "  continuing at $(Mb $have) of $(Mb $total) MB" }
            while (($read = $stream.Read($buffer, 0, $buffer.Length)) -gt 0) {
                $output.Write($buffer, 0, $read)
                $done += $read
                if ($ForApp -and $clock.ElapsedMilliseconds -ge ($lastBytes + 500)) {
                    $lastBytes = $clock.ElapsedMilliseconds
                    Say "@app download $done $total"
                }
                if ($total -gt 5MB -and ($done * 10 / $total) -ge $nextReport -and $nextReport -le 10) {
                    Say ("  {0} of {1} MB ({2}%)" -f (Mb $done), (Mb $total), ($nextReport * 10))
                    $nextReport++
                }
            }
        }
        finally { $output.Dispose(); $stream.Dispose() }
    }
    catch {
        Fail "The download of $($tool.File) stopped: $($_.Exception.Message)`n  Run this script again; it continues where it stopped."
    }
    finally { $response.Close() }
    Move-Item -LiteralPath $part -Destination $dest -Force
}

function Get-Verified($tool) {
    $zip = Join-Path $archives $tool.File
    for ($attempt = 1; $attempt -le 2; $attempt++) {
        if (-not (Test-Path -LiteralPath $zip)) {
            # A file the player saved by hand into <repo>\downloads (its name from the URL) is used instead of downloading.
            # It is copied, so a wrong one is only thrown away as a copy (the check below) and the next try downloads.
            $hand = Join-Path (Join-Path $repo 'downloads') $tool.File
            if (($attempt -eq 1) -and (Test-Path -LiteralPath $hand)) {
                Say "  using the file you saved by hand: $hand"
                New-Item -ItemType Directory -Force -Path $archives | Out-Null
                Copy-Item -LiteralPath $hand -Destination $zip -Force
            }
            else { Download-File $tool $zip }
        }
        $actual = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLower()
        if ($actual -eq $tool.Sha256) { Say '  checksum ok'; return $zip }
        Remove-Item -LiteralPath $zip -Force
        if (Test-Path -LiteralPath "$zip.part") { Remove-Item -LiteralPath "$zip.part" -Force }
        if ($attempt -eq 2) {
            Fail ("The file $($tool.File) does not match its pinned SHA-256, so it was deleted and not used.`n" +
                  "  expected $($tool.Sha256)`n  got      $actual`n" +
                  "  A damaged download or a proxy that rewrites files can cause this. Run the script again; if it fails again, " +
                  "the file at the source has changed and the pin in scripts\portable.ps1 needs a review.")
        }
        Say '  checksum does not match: downloading it once more'
    }
}

# tar.exe takes its command line in the ANSI code page, so it cannot open a folder with letters outside it (Chinese on a
# Western Windows). Its working directory is passed on as Unicode, though: it runs inside the zip's folder with the plain
# file name and a relative target. $null when the two folders have no common parent (another drive).
function Get-RelativePath([string]$from, [string]$to) {
    $a = @([IO.Path]::GetFullPath($from).TrimEnd('\').Split('\'))
    $b = @([IO.Path]::GetFullPath($to).TrimEnd('\').Split('\'))
    $common = 0
    while ($common -lt $a.Count -and $common -lt $b.Count -and $a[$common] -ieq $b[$common]) { $common++ }
    if ($common -eq 0) { return $null }
    $parts = @()
    for ($i = $common; $i -lt $a.Count; $i++) { $parts += '..' }
    for ($i = $common; $i -lt $b.Count; $i++) { $parts += $b[$i] }
    if ($parts.Count -eq 0) { return '.' }
    $parts -join '\'
}

function Test-AnsiSafe([string]$text) {
    $ansi = [Text.Encoding]::Default   # the system's ANSI code page (Windows PowerShell 5.1 runs on .NET Framework)
    $ansi.GetString($ansi.GetBytes($text)) -ceq $text
}

function Unpack-Zip([string]$zip, [string]$into) {
    $tar = Join-Path $env:SystemRoot 'System32\tar.exe'
    $dir = Split-Path -Parent $zip
    $name = Split-Path -Leaf $zip
    $relative = Get-RelativePath $dir $into
    if ((Test-Path -LiteralPath $tar) -and -not $NoTar -and $relative -and (Test-AnsiSafe $name) -and (Test-AnsiSafe $relative)) {
        Push-Location -LiteralPath $dir
        try {
            & $tar -xf $name -C $relative
            if ($LASTEXITCODE -ne 0) { Fail "tar.exe could not unpack $zip (exit code $LASTEXITCODE)." }
        }
        finally { Pop-Location }
    }
    else {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        [IO.Compression.ZipFile]::ExtractToDirectory($zip, $into)
    }
}

function Install-Tool($tool) {
    $target = Join-Path $Dir $tool.Name
    $zip = Get-Verified $tool
    if (Test-Path -LiteralPath $scratch) { Remove-Item -LiteralPath $scratch -Recurse -Force }
    New-Item -ItemType Directory -Path $scratch | Out-Null
    if ($tool.ContainsKey('Single')) {
        Say '  setting up'
        $root = Join-Path $scratch $tool.Name
        New-Item -ItemType Directory -Path $root | Out-Null
        Copy-Item -LiteralPath $zip -Destination (Join-Path $root $tool.Single)
        foreach ($applet in $tool.Applets) { Copy-Item -LiteralPath $zip -Destination (Join-Path $root "$applet.exe") }
    }
    else {
        Say '  unpacking'
        Unpack-Zip $zip $scratch
        $root = $scratch
        if ($tool.Top) { $root = Join-Path $scratch $tool.Top }
        if (-not (Test-Path -LiteralPath (Join-Path $root $tool.Probe))) {
            Fail "$($tool.File) unpacked, but $($tool.Probe) is not in it. The zip is not what this script expects."
        }
    }
    if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Recurse -Force }
    Move-Item -LiteralPath $root -Destination $target
    if (Test-Path -LiteralPath $scratch) { Remove-Item -LiteralPath $scratch -Recurse -Force }

    if ($tool.Name -eq 'mingit') {
        # MinGit ships bash as sh.exe only. Git Bash starts it as "bash" (the scripts say #!/usr/bin/env bash), so
        # give it that name too; the same program behaves as bash when it is called bash.
        Copy-Item -LiteralPath (Join-Path $target 'usr\bin\sh.exe') -Destination (Join-Path $target 'usr\bin\bash.exe') -Force
    }
    Set-Content -LiteralPath (Marker $tool) -Value (Stamp $tool) -Encoding ASCII
    if (-not $KeepArchives) { Remove-Item -LiteralPath $zip -Force }
}

# The files that make a window "clean": only these tools and Windows itself are on the PATH.
function Write-EnvFiles {
    $envPs1 = @'
# Generated by scripts\portable.ps1. Dot-source it:  . .\env.ps1
# Leaves this PowerShell window with only the portable tools and Windows itself: the PATH is rebuilt from scratch and
# every environment variable that is not on the short list below is removed (PYTHONHOME, CXX, CMAKE_*, EMSDK, ...).
# Use a new window (shell.cmd does that). Add variables to keep with -Keep, for example  -Keep PS2_JOBS,PS2_SCALE
param([string[]]$Keep = @(), [switch]$Quiet)
$root = $PSScriptRoot
$allowed = @('SystemRoot', 'SystemDrive', 'windir', 'ComSpec', 'PATHEXT', 'TEMP', 'TMP', 'USERNAME', 'USERPROFILE',
    'APPDATA', 'LOCALAPPDATA', 'ProgramData', 'ProgramFiles', 'ProgramFiles(x86)', 'ProgramW6432', 'CommonProgramFiles',
    'CommonProgramFiles(x86)', 'CommonProgramW6432', 'ALLUSERSPROFILE', 'PUBLIC', 'HOMEDRIVE', 'HOMEPATH', 'OS',
    'NUMBER_OF_PROCESSORS', 'PROCESSOR_ARCHITECTURE', 'PROCESSOR_IDENTIFIER', 'PROCESSOR_LEVEL', 'PROCESSOR_REVISION',
    'COMPUTERNAME', 'DriverData', 'PSModulePath', 'Path') + $Keep
foreach ($name in @([Environment]::GetEnvironmentVariables().Keys)) {
    if ($allowed -notcontains $name) { [Environment]::SetEnvironmentVariable($name, $null, 'Process') }
}
$env:Path = (@(
    "$root\llvm-mingw\bin", "$root\cmake\bin", "$root\ninja", "$root\python",
    "$root\mingit\cmd", "$root\mingit\usr\bin", "$root\busybox", "$env:SystemRoot\System32") -join ';')
if (-not $Quiet) {
    Write-Host 'Portable tools are on the PATH (and nothing else):'
    Write-Host '  bash scripts/setup.sh --native-only'
    Write-Host '  bash scripts/port.sh <name> <disc image> --native-only'
}
'@
    Set-Content -LiteralPath (Join-Path $Dir 'env.ps1') -Value $envPs1 -Encoding UTF8
    $shellCmd = @"
@echo off
rem Generated by scripts\portable.ps1. Opens a window in which only the portable tools and Windows itself are on the PATH.
if exist "$repo\scripts\port.sh" ( set "start=$repo" ) else ( set "start=%~dp0" )
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -NoExit -Command "Set-Location -LiteralPath '%start%'; . '%~dp0env.ps1'"
"@
    Set-Content -LiteralPath (Join-Path $Dir 'shell.cmd') -Value $shellCmd -Encoding ASCII
}

function Show-Version([string]$label, [string]$exe, [string[]]$arguments) {
    $lines = @(& $exe @arguments)
    if ($LASTEXITCODE -ne 0 -or $lines.Count -eq 0) { Fail "$label was unpacked but does not start ($exe)." }
    Say ("  {0,-11} {1}" -f $label, $lines[0])
}

function Main {
    $started = Get-Date
    if ($Dir.Length -gt 70) {
        Write-Host 'Note: the folder path is long. Windows stops at 260 characters, and the compiler has deep folders. A shorter path is safer.' -ForegroundColor Yellow
    }

    if ($Check) {
        $missing = 0
        foreach ($tool in $tools) {
            if (Is-Installed $tool) { Say "ok       $($tool.Title)" } else { Say "missing  $($tool.Title)"; $missing++ }
        }
        if ($missing -gt 0) { Say "$missing of $($tools.Count) tools are missing: run scripts\portable.ps1"; exit 1 }
        Say "All $($tools.Count) tools are in $Dir"
        return
    }

    Say "Portable toolchain folder: $Dir"
    $todo = @($tools | Where-Object { $Force -or -not (Is-Installed $_) })
    if ($todo.Count -gt 0) {
        $root = [IO.Path]::GetPathRoot($Dir)
        if ($root -and -not $root.StartsWith('\\')) {
            $free = (New-Object IO.DriveInfo($root)).AvailableFreeSpace
            if ($free -lt $neededBytes) {
                Fail ("Not enough free disk space on ${root}: $(Mb $free) MB free, about $(Mb $neededBytes) MB needed. " +
                      "Free some space, or pass -Dir with a folder on another drive.")
            }
        }
    }
    New-Item -ItemType Directory -Force -Path $Dir | Out-Null
    New-Item -ItemType Directory -Force -Path $archives | Out-Null
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor 3072  # TLS 1.2

    $downloaded = 0
    $index = 0
    foreach ($tool in $tools) {
        $index++
        $label = "[$index/$($tools.Count)] $($tool.Title)"
        if (-not $Force -and (Is-Installed $tool)) { Say "$label : already there"; continue }
        Say "$label : $(Mb $tool.Size) MB"
        $step = Get-Date
        Install-Tool $tool
        $downloaded += $tool.Size
        Say ("  done in {0:N0} s" -f ((Get-Date) - $step).TotalSeconds)
    }

    Write-EnvFiles
    if (-not $KeepArchives -and (Test-Path -LiteralPath $archives) -and -not (Get-ChildItem -LiteralPath $archives -Force)) {
        Remove-Item -LiteralPath $archives
    }

    Say 'Checking that every tool starts:'
    Show-Version 'clang++' (Join-Path $Dir 'llvm-mingw\bin\clang++.exe') @('--version')
    Show-Version 'cmake' (Join-Path $Dir 'cmake\bin\cmake.exe') @('--version')
    Show-Version 'ninja' (Join-Path $Dir 'ninja\ninja.exe') @('--version')
    Show-Version 'python' (Join-Path $Dir 'python\python.exe') @('-I', '--version')
    Show-Version 'bash' (Join-Path $Dir 'mingit\usr\bin\bash.exe') @('--version')
    Show-Version 'busybox' (Join-Path $Dir 'busybox\busybox.exe') @('--help')

    $size = (Get-ChildItem -LiteralPath $Dir -Recurse -Force -File | Measure-Object Length -Sum).Sum
    Say ''
    Say ("Ready: {0} MB on disk in {1}, {2} MB downloaded this time, {3:N0} s." -f (Mb $size), $Dir, (Mb $downloaded), ((Get-Date) - $started).TotalSeconds)
    Say 'Next: start tools\portable\shell.cmd (or run  . tools\portable\env.ps1  in a fresh PowerShell) and then'
    Say '  bash scripts/setup.sh --native-only'
}

# Dot-sourcing (. scripts\portable.ps1) only loads $tools and the functions; running the file does the work.
if ($MyInvocation.InvocationName -ne '.') {
    try { Main }
    catch {
        Write-Host ''
        Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
        exit 1
    }
}
