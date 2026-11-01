# PS2 Native Recomp: the Windows app. Double-click Start.cmd (in the folder above this one) to open it.
#
# One window, four screens: welcome, choose the disc image, build, done (Play / desktop shortcut). It adds no build
# logic of its own and runs the repo's scripts as child processes, with only the portable tools on the PATH:
#   1. scripts\portable.ps1 -ForApp   downloads the build tools into tools\portable (only what is missing; the
#                                     "@app download <done> <total>" lines of that script drive the progress bar),
#   2. scripts\setup.sh --native-only fetches the graphics and window libraries, builds the translator tools,
#   3. scripts\port.sh <name> <image> --native-only   turns the disc image into build\native-<name>\ps2run.exe.
# A built game can be made faster (the button "Make it faster" on the done and welcome screens): the same port.sh with
# --no-explore, which compiles the vector programs that ps2run recorded while the game was played.
# Their output goes to games\<name>\build.log and shows under "Show details". After a build the game's start command
# is kept in games\<name>\app.json, which Play, the desktop shortcut and the welcome screen read. Details: docs\app.md
#
# Windows PowerShell 5.1 and WinForms only (both are part of Windows 10/11). Keep this file plain ASCII: PowerShell 5.1
# reads a file without a byte order mark in the ANSI code page.

Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$Root = Split-Path -Parent $PSScriptRoot
$ToolsDir = Join-Path $Root 'tools\portable'
$GamesDir = Join-Path $Root 'games'
$AppTitle = 'PS2 Native Recomp'
$ToolsDownloadMb = 312   # portable.ps1 (284 MB) + wgpu-native (15 MB) + SDL2 (14 MB), see docs\portable.md

function Write-AppLog([string]$text) {
    try {
        New-Item -ItemType Directory -Force -Path $GamesDir | Out-Null
        Add-Content -LiteralPath (Join-Path $GamesDir 'app.log') -Value ('{0:s} {1}' -f (Get-Date), $text) -Encoding UTF8
    } catch { }
}

function Show-Fatal([string]$text) {
    Write-AppLog $text
    try {
        Add-Type -AssemblyName System.Windows.Forms
        [void][Windows.Forms.MessageBox]::Show("The app hit a problem and could not continue.`n`n$text`n`nThe details are in $(Join-Path $GamesDir 'app.log')", $AppTitle, 'OK', 'Error')
    } catch { }
}

# Everything below is wrapped, so a failure shows a message box instead of a window that silently never opens.
try {

# A file from a zip download carries the "from the internet" mark; clear it on our own scripts (the launcher already
# runs them with the execution policy bypassed, this only keeps PowerShell from asking questions).
try { Get-ChildItem -LiteralPath $PSScriptRoot, (Join-Path $Root 'scripts') -File -Filter *.ps1 | Unblock-File } catch { }

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
try {
    # Draw sharp (not stretched) on screens with 125 % / 150 % scaling.
    Add-Type -Namespace Ps2App -Name Native -MemberDefinition '[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool SetProcessDPIAware();'
    [void][Ps2App.Native]::SetProcessDPIAware()
} catch { }
[Windows.Forms.Application]::EnableVisualStyles()
try { [Windows.Forms.Application]::SetCompatibleTextRenderingDefault($false) } catch { }
[Windows.Forms.Application]::SetUnhandledExceptionMode('CatchException')
[Windows.Forms.Application]::add_ThreadException({ param($sender, $e) Show-Fatal $e.Exception.ToString() })

# ---------------------------------------------------------------------------------------------------------------------
# Helpers that know nothing about the window

function Get-SafeName([string]$fileName) {
    $n = [IO.Path]::GetFileNameWithoutExtension($fileName)
    $n = $n -replace '\([^)]*\)|\[[^\]]*\]', ' '                      # "(USA)", "[SLUS-12345]"
    $n = $n.Normalize([Text.NormalizationForm]::FormD) -replace '\p{M}', ''  # u-umlaut -> u
    $n = ($n -replace '[^A-Za-z0-9]+', '-').Trim('-')
    if ($n.Length -gt 32) { $n = $n.Substring(0, 32).Trim('-') }
    if (-not $n) { $n = 'game' }
    $n
}

function Test-GameName([string]$name) { $name -match '^[A-Za-z0-9][A-Za-z0-9_-]{0,31}$' }

function Get-GameFile([string]$name, [string]$file) { Join-Path (Join-Path $GamesDir $name) $file }

function Read-Game([string]$name) {
    $file = Get-GameFile $name 'app.json'
    if (-not (Test-Path -LiteralPath $file)) { return $null }
    try { $json = Get-Content -LiteralPath $file -Raw -Encoding UTF8 | ConvertFrom-Json } catch { return $null }
    $game = New-GameRecord $name
    foreach ($key in @($game.Keys)) {
        if ($key -eq 'sections') { continue }
        $prop = $json.PSObject.Properties[$key]
        if ($prop -and $null -ne $prop.Value) { $game[$key] = [string]$prop.Value }
    }
    $list = $json.PSObject.Properties['sections']
    if ($list -and $null -ne $list.Value) { $game.sections = @($list.Value | ForEach-Object { [string]$_ }) }
    $game
}

# What the app keeps per game in games\<name>\app.json. pnach, sections, patches and widescreen are about the PCSX2 patches the
# game was built with: the patch file and the ticked sections (the choice is offered again at the next build), the list of
# data patches that ps2run writes into memory while the game runs, and whether the game is played in 16:9.
function New-GameRecord([string]$name) {
    @{ name = $name; source = ''; built = ''; took = ''; elf = ''; iso = ''; pnach = ''; sections = @(); patches = ''; widescreen = '' }
}

function Write-Game($game) {
    $full = New-GameRecord $game.name
    foreach ($key in @($game.Keys)) { $full[$key] = $game[$key] }
    $full.sections = @($full.sections)
    New-Item -ItemType Directory -Force -Path (Join-Path $GamesDir $full.name) | Out-Null
    ($full | ConvertTo-Json) | Set-Content -LiteralPath (Get-GameFile $full.name 'app.json') -Encoding UTF8
}

# A name is free when no game folder uses it, or the folder was made by this app for the same disc image.
function Get-NameProblem([string]$name, [string]$image) {
    if (-not (Test-GameName $name)) { return 'Use letters, numbers, - and _ only (up to 32 characters, starting with a letter or number).' }
    $folder = Join-Path $GamesDir $name
    if (-not (Test-Path -LiteralPath $folder)) { return $null }
    $game = Read-Game $name
    if ($game -and $game.source -ieq $image) { return $null }
    'A different game already uses this name. Please choose another name.'
}

function Get-FreeName([string]$name, [string]$image) {
    $candidate = $name
    for ($i = 2; (Get-NameProblem $candidate $image) -and $i -lt 100; $i++) { $candidate = "$name-$i" }
    $candidate
}

# Built games, newest first (at most three): the ones with an app.json that says built and a runner that exists.
function Get-BuiltGames {
    $found = @()
    if (Test-Path -LiteralPath $GamesDir) {
        foreach ($dir in Get-ChildItem -LiteralPath $GamesDir -Directory) {
            $game = Read-Game $dir.Name
            if ($game -and $game.built -and (Test-Path -LiteralPath (Join-Path $Root "build\native-$($dir.Name)\ps2run.exe"))) { $found += $game }
        }
    }
    @($found | Sort-Object { $_.built } -Descending | Select-Object -First 3)
}

function Resolve-AppPath([string]$path) {
    if ([IO.Path]::IsPathRooted($path)) { return $path }
    Join-Path $Root $path
}

# The path as app.json keeps it: relative to the app folder when it lies inside it (so the folder can be moved), else as it is.
function Get-RootRelative([string]$file) {
    if ($file.StartsWith($Root + '\', [StringComparison]::OrdinalIgnoreCase)) { return $file.Substring($Root.Length + 1) }
    $file
}

function Format-Size([double]$bytes) {
    if ($bytes -ge 1GB) { return ('{0:0.0} GB' -f ($bytes / 1GB)) }
    if ($bytes -lt 1MB) { return ('{0:0} KB' -f [Math]::Max(1, $bytes / 1KB)) }
    '{0:0} MB' -f ($bytes / 1MB)
}

function Format-Time([double]$seconds) { '{0}:{1:00}' -f [int][Math]::Floor($seconds / 60), [int]($seconds % 60) }

function ConvertTo-PosixPath([string]$path) {
    $path = $path.Replace('\', '/')
    if ($path -match '^([A-Za-z]):/(.*)$') { return $Matches[1] + ':/' + $Matches[2] }   # bash.exe takes C:/dir/file
    $path
}

# One argument of a Windows command line, quoted so that bash.exe (the MSYS runtime parses the line itself) gets it back exactly:
# the rules of CommandLineToArgvW (a " becomes \", backslashes before a " are doubled). Plain words stay bare. Section names of
# patch files have spaces, colons and now and then quotes, ampersands, brackets or an apostrophe; all of them survive this.
function ConvertTo-Arg([string]$text) {
    if ($text -ne '' -and $text -match '^[A-Za-z0-9_./:=+-]+$') { return $text }
    $quoted = New-Object Text.StringBuilder
    [void]$quoted.Append('"')
    $backslashes = 0
    foreach ($ch in $text.ToCharArray()) {
        if ($ch -eq '\') { $backslashes++ }
        elseif ($ch -eq '"') { [void]$quoted.Append('\' * ($backslashes * 2 + 1)).Append('"'); $backslashes = 0 }
        else { if ($backslashes) { [void]$quoted.Append('\' * $backslashes); $backslashes = 0 }; [void]$quoted.Append($ch) }
    }
    if ($backslashes) { [void]$quoted.Append('\' * ($backslashes * 2)) }
    [void]$quoted.Append('"')
    $quoted.ToString()
}

# A desktop shortcut with Unicode paths. WScript.Shell keeps paths in the ANSI code page, so for a folder with letters outside it
# (Chinese on a Western Windows) it cannot make a working shortcut (it throws, or leaves "?" in the target); IShellLinkW can.
# The icon is a file and an index (0 = the first icon of the file), not WScript's "file,0" text.
function New-Shortcut([string]$lnk, [string]$target, [string]$arguments, [string]$workDir, [string]$iconFile, [string]$description) {
    if (-not ('Ps2App.ShellLink' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
namespace Ps2App {
    public static class ShellLink {
        [ComImport, Guid("00021401-0000-0000-C000-000000000046")] class CShellLink { }
        [ComImport, InterfaceType(ComInterfaceType.InterfaceIsIUnknown), Guid("000214F9-0000-0000-C000-000000000046")]
        interface IShellLinkW {
            void GetPath([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder pszFile, int cch, IntPtr pfd, uint fFlags);
            void GetIDList(out IntPtr ppidl);
            void SetIDList(IntPtr pidl);
            void GetDescription([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder pszName, int cch);
            void SetDescription([MarshalAs(UnmanagedType.LPWStr)] string pszName);
            void GetWorkingDirectory([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder pszDir, int cch);
            void SetWorkingDirectory([MarshalAs(UnmanagedType.LPWStr)] string pszDir);
            void GetArguments([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder pszArgs, int cch);
            void SetArguments([MarshalAs(UnmanagedType.LPWStr)] string pszArgs);
            void GetHotkey(out short pwHotkey);
            void SetHotkey(short wHotkey);
            void GetShowCmd(out int piShowCmd);
            void SetShowCmd(int iShowCmd);
            void GetIconLocation([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder pszIconPath, int cch, out int piIcon);
            void SetIconLocation([MarshalAs(UnmanagedType.LPWStr)] string pszIconPath, int iIcon);
            void SetRelativePath([MarshalAs(UnmanagedType.LPWStr)] string pszPathRel, uint dwReserved);
            void Resolve(IntPtr hwnd, uint fFlags);
            void SetPath([MarshalAs(UnmanagedType.LPWStr)] string pszFile);
        }
        [ComImport, InterfaceType(ComInterfaceType.InterfaceIsIUnknown), Guid("0000010b-0000-0000-C000-000000000046")]
        interface IPersistFile {
            void GetClassID(out Guid pClassID);
            [PreserveSig] int IsDirty();
            void Load([MarshalAs(UnmanagedType.LPWStr)] string pszFileName, uint dwMode);
            void Save([MarshalAs(UnmanagedType.LPWStr)] string pszFileName, [MarshalAs(UnmanagedType.Bool)] bool fRemember);
            void SaveCompleted([MarshalAs(UnmanagedType.LPWStr)] string pszFileName);
            void GetCurFile([MarshalAs(UnmanagedType.LPWStr)] out string ppszFileName);
        }
        public static void Create(string lnk, string target, string arguments, string workDir, string iconFile, string description) {
            IShellLinkW link = (IShellLinkW)new CShellLink();
            link.SetPath(target);
            link.SetArguments(arguments);
            link.SetWorkingDirectory(workDir);
            link.SetIconLocation(iconFile, 0);
            link.SetDescription(description);
            ((IPersistFile)link).Save(lnk, true);
        }
    }
}
'@
    }
    [Ps2App.ShellLink]::Create($lnk, $target, $arguments, $workDir, $iconFile, $description)
}

function Get-InstallProblem {
    # Any letters work in the folder name: tar.exe (it reads its command line in the ANSI code page) is run inside the folder
    # with plain file names by scripts\portable.ps1 and scripts\fetch_*.sh, and the desktop shortcut is written with
    # IShellLinkW (Unicode, see New-Shortcut) instead of WScript.Shell.
    # The compiler's folders are deep and Windows stops at 260 characters.
    if ($Root.Length -gt 90) {
        return "The path of this folder is too long for the build tools (Windows stops at 260 characters):`n$Root`nPlease move the whole app folder to a short path such as C:\PS2Native, then start it again."
    }
    $null
}

function Test-DiscImage([string]$path) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return 'That file does not exist.' }
    switch ([IO.Path]::GetExtension($path).ToLower()) {
        '.iso' {
            $stream = [IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')
            try { $head = New-Object byte[] 6; [void]$stream.Seek(0x8000, 'Begin'); $got = $stream.Read($head, 0, 6) } finally { $stream.Dispose() }
            if ($got -ne 6 -or $head[0] -ne 1 -or [Text.Encoding]::ASCII.GetString($head, 1, 5) -ne 'CD001') {
                return 'This does not look like a PS2 disc image. It has to be an .iso file with normal 2048-byte sectors, made from the DVD.'
            }
        }
        '.zip' {
            Add-Type -AssemblyName System.IO.Compression.FileSystem
            try {
                $zip = [IO.Compression.ZipFile]::OpenRead($path)
                try { $hasIso = @($zip.Entries | Where-Object { $_.FullName -like '*.iso' }).Count -gt 0 } finally { $zip.Dispose() }
            } catch { return 'This .zip file cannot be opened.' }
            if (-not $hasIso) { return 'There is no .iso file inside this .zip.' }
        }
        '.7z' {
            if (-not (Test-Path -LiteralPath (Join-Path $env:ProgramFiles '7-Zip\7z.exe'))) {
                return 'A .7z file needs the free program 7-Zip (7-zip.org). Install it, or unpack the .iso yourself and choose that.'
            }
        }
        default { return 'Please choose an .iso file, or a .zip or .7z file with an .iso inside.' }
    }
    $null
}

# What is still to fetch. portable.ps1 is dot-sourced inside this function so that its settings and names do not leak.
function Get-ToolchainState {
    . (Join-Path $Root 'scripts\portable.ps1')
    $missing = @()
    $bytes = [int64]0
    foreach ($tool in $tools) {
        if (-not (Is-Installed $tool)) {
            $missing += [pscustomobject]@{ Name = $tool.Name; Title = $tool.Title; Size = [int64]$tool.Size }
            $bytes += [int64]$tool.Size
        }
    }
    [pscustomobject]@{ Missing = $missing; Bytes = $bytes; HasEnv = (Test-Path -LiteralPath (Join-Path $Dir 'env.ps1')) }
}

# The app's own Python (the one the build uses) is there and complete.
function Test-AppPython {
    . (Join-Path $Root 'scripts\portable.ps1')
    $python = $tools | Where-Object { $_.Name -eq 'python' } | Select-Object -First 1
    [bool](Is-Installed $python)
}

function Test-SetupNeeded {
    foreach ($file in 'build\tools\eerecomp.exe', 'build\tools\vurecomp.exe', 'third_party\wgpu-native\lib\libwgpu_native.a', 'third_party\sdl2\lib\libSDL2.dll.a') {
        if (-not (Test-Path -LiteralPath (Join-Path $Root $file))) { return $true }
    }
    $false
}

# The command that starts a built game: the runner, its arguments and the folder to start it in.
function Get-PlayCommand([string]$name) {
    $game = Read-Game $name
    if (-not $game -or -not $game.built) { return $null }
    $exe = Join-Path $Root "build\native-$name\ps2run.exe"
    if (-not (Test-Path -LiteralPath $exe)) { return $null }
    $elf = Resolve-AppPath $game.elf
    $iso = Resolve-AppPath $game.iso
    $arguments = '"{0}" --disc "{1}" --window' -f $elf, $iso
    # A game built with PCSX2 patches is played the way port.sh's closing "Play it natively" line says: the patched program
    # (elf above is that one), the list of data patches the runner writes into memory, and the 16:9 picture when a ticked
    # section is made for a 16:9 screen.
    if ($game.widescreen) { $arguments += ' --widescreen' }
    if ($game.patches) { $arguments += ' --patches "{0}"' -f (Resolve-AppPath $game.patches) }
    $arguments += ' --memcard "{0}"' -f (Get-GameFile $name "$name.ps2")
    @{ Exe = $exe; Elf = $elf; Iso = $iso; Card = (Get-GameFile $name "$name.ps2"); Dir = (Join-Path $GamesDir $name); Arguments = $arguments }
}

# How many vector programs (VU1 microprograms) were recorded since the last build. ps2run writes every vector program it has to
# interpret into games\<name>\vu1, one .bin file each and only when that file is not there yet. A build compiles all the files of
# that folder into the game program (port.sh step 5 and 6), so a .bin file that is newer than build\native-<name>\ps2run.exe was
# recorded after the program was built and is not in it yet. The exe is the last thing a build makes, so a build that was
# cancelled or failed half way leaves the old exe and its files still count. Files the build had to leave to the interpreter are
# older than the exe too, so they are not counted again and again.
function Get-RecordedCount([string]$name) {
    $dir = Join-Path (Join-Path $GamesDir $name) 'vu1'
    if (-not (Test-Path -LiteralPath $dir -PathType Container)) { return 0 }
    $exe = Join-Path $Root "build\native-$name\ps2run.exe"
    $since = [DateTime]::MinValue
    if (Test-Path -LiteralPath $exe) { $since = [IO.File]::GetLastWriteTimeUtc($exe) }
    $count = 0
    foreach ($file in [IO.Directory]::EnumerateFiles($dir, '*.bin')) {
        if ([IO.File]::GetLastWriteTimeUtc($file) -gt $since) { $count++ }
    }
    $count
}

# A running game keeps its ps2run.exe open, and Windows then refuses to replace it: found by the path of the program, so another
# copy of the app (or a developer's build in another folder) is not mistaken for this game.
function Test-GameRunning([string]$exe) {
    foreach ($process in @(Get-Process -Name 'ps2run' -ErrorAction SilentlyContinue)) {
        try { if ($process.Path -ieq $exe) { return $true } } catch { }
    }
    $false
}

# ---------------------------------------------------------------------------------------------------------------------
# Updates (the Update button). The only network use is two small lookups at the start (the newest version of the public repo)
# and, when the player presses Update, the download of that version's zip. Nothing is ever uploaded.
#
# How the versions are told apart: the newest one is the commit id GitHub names for the branch main of the public repo (a 40-digit
# hex text, asked with the "sha" media type). The installed one is tools\update\installed.txt, written by an update. A copy that
# came straight from the zip has no such file; it is compared file by file with the newest version instead (GitHub lists the
# git hash of every file; the app hashes its own files the same way, .md files and .github left out), and when they are equal the
# file is written. The zip of a commit carries the commit id as its zip comment, which is how a downloaded zip is checked.

$UpdateRepo = 'exoteric33/ps2-native-recomp'
$UpdateApi = "https://api.github.com/repos/$UpdateRepo"
$UpdateArchive = "https://github.com/$UpdateRepo/archive"
# For tests only (docs\app.md): a local test server instead of GitHub. Anything but a local address is ignored.
if ($env:PS2APP_UPDATE_TEST -match '^http://(127\.0\.0\.1|localhost):\d+$') { $UpdateApi = $env:PS2APP_UPDATE_TEST + '/api'; $UpdateArchive = $env:PS2APP_UPDATE_TEST + '/archive' }
$UpdateDir = Join-Path $Root 'tools\update'
# What an update never touches: everything the player made or downloaded, and a git clone's own folder.
$UpdateKeep = @('games', 'tools', 'build', 'generated', 'isos', 'third_party', 'wt', 'logs', '.git')

function Get-InstalledVersion {
    $file = Join-Path $UpdateDir 'installed.txt'
    try { if (Test-Path -LiteralPath $file) { $text = (Get-Content -LiteralPath $file -Raw).Trim(); if ($text -match '^[0-9a-f]{40}$') { return $text } } } catch { }
    ''
}

# A clone made with git is updated with git, and a developer can switch the lookup off.
function Test-UpdateAllowed {
    if ($env:PS2APP_NO_UPDATE_CHECK -eq '1') { return $false }
    if (Test-Path -LiteralPath (Join-Path $Root '.git')) { return $false }
    $true
}

# The games waiting to be built again after an update: tools\update\rebuild.txt, one game name per line.
function Get-PendingRebuild {
    $file = Join-Path $UpdateDir 'rebuild.txt'
    $names = @()
    try {
        if (Test-Path -LiteralPath $file) {
            foreach ($line in Get-Content -LiteralPath $file) {
                $n = $line.Trim()
                if ((Test-GameName $n) -and (Test-Path -LiteralPath (Join-Path $Root "build\native-$n\ps2run.exe"))) { $names += $n }
            }
        }
    } catch { }
    @($names)
}

function Set-PendingRebuild($names) {
    $file = Join-Path $UpdateDir 'rebuild.txt'
    try {
        if (@($names).Count -eq 0) { if (Test-Path -LiteralPath $file) { Remove-Item -LiteralPath $file -Force }; return }
        New-Item -ItemType Directory -Force -Path $UpdateDir | Out-Null
        Set-Content -LiteralPath $file -Value @($names) -Encoding UTF8
    } catch { Write-AppLog $_.Exception.ToString() }
}

# Every game that was built (not only the three on the welcome screen), in alphabetical order.
function Get-AllBuiltNames {
    $found = @()
    if (Test-Path -LiteralPath $GamesDir) {
        foreach ($dir in Get-ChildItem -LiteralPath $GamesDir -Directory) {
            $game = Read-Game $dir.Name
            if ($game -and $game.built -and (Test-Path -LiteralPath (Join-Path $Root "build\native-$($dir.Name)\ps2run.exe"))) { $found += $dir.Name }
        }
    }
    @($found | Sort-Object)
}

# The worker runs in its own runspace (a second thread), so the window never waits for the network. It talks to the window through
# the synchronized table $sh: Result is empty while it works. Mode 'check' says whether a newer version exists; mode 'download'
# fetches, checks and unpacks it into tools\update\new.
$UpdateWorker = @'
param($sh, $mode, $api, $archive, $root, $dir, $sha)
$ErrorActionPreference = 'Stop'
try {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    function Open-Get([string]$url, [string]$accept, [int]$timeout) {
        $req = [Net.HttpWebRequest]::Create($url)
        $req.UserAgent = 'ps2-native-recomp-app'
        $req.Accept = $accept
        $req.Timeout = $timeout
        $req.ReadWriteTimeout = $timeout
        $req.GetResponse()
    }
    function Get-Text([string]$url, [string]$accept) {
        $r = Open-Get $url $accept 10000
        try { (New-Object IO.StreamReader($r.GetResponseStream(), [Text.Encoding]::UTF8)).ReadToEnd() } finally { $r.Close() }
    }
    if ($mode -eq 'check') {
        $newest = (Get-Text "$api/commits/main" 'application/vnd.github.sha').Trim()
        if ($newest -notmatch '^[0-9a-f]{40}$') { throw 'unexpected answer' }
        $sh.Sha = $newest
        $installed = ''
        try { $installed = (Get-Content -LiteralPath (Join-Path $dir 'installed.txt') -Raw).Trim() } catch { }
        if ($installed -eq $newest) { $sh.Result = 'current'; return }
        # Not the version this app recorded (or none): are the files the same as the newest version's anyway?
        $tree = (Get-Text "$api/git/trees/${newest}?recursive=1" 'application/vnd.github+json') | ConvertFrom-Json
        $same = $true
        $count = 0
        foreach ($entry in $tree.tree) {
            if ($entry.type -ne 'blob') { continue }
            $path = [string]$entry.path
            if ($path -like '*.md' -or $path -like '.github/*') { continue }
            $count++
            $file = Join-Path $root ($path.Replace('/', '\'))
            if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { $same = $false; break }
            $bytes = [IO.File]::ReadAllBytes($file)
            $head = [Text.Encoding]::ASCII.GetBytes("blob $($bytes.Length)`0")
            $hash = [Security.Cryptography.SHA1]::Create()
            [void]$hash.TransformBlock($head, 0, $head.Length, $null, 0)
            [void]$hash.TransformFinalBlock($bytes, 0, $bytes.Length)
            $hex = ($hash.Hash | ForEach-Object { $_.ToString('x2') }) -join ''
            if ($hex -ne [string]$entry.sha) { $same = $false; break }
        }
        if ($same -and $count -gt 20) {
            New-Item -ItemType Directory -Force -Path $dir | Out-Null
            Set-Content -LiteralPath (Join-Path $dir 'installed.txt') -Value $newest -Encoding ASCII
            $sh.Result = 'current'
        } else { $sh.Result = 'newer' }
        return
    }

    # mode 'download'
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $part = Join-Path $dir 'download.zip.part'
    $zipFile = Join-Path $dir 'download.zip'
    foreach ($f in $part, $zipFile) { if (Test-Path -LiteralPath $f) { Remove-Item -LiteralPath $f -Force } }
    $sh.Phase = 'download'
    $resp = Open-Get "$archive/$sha.zip" 'application/zip' 15000
    $sh.Total = [double]$resp.ContentLength
    $in = $resp.GetResponseStream()
    $out = [IO.File]::Create($part)
    try {
        $buffer = New-Object byte[] 65536
        while ($true) {
            if ($sh.Cancel) { $sh.Result = 'cancelled'; return }
            $n = $in.Read($buffer, 0, $buffer.Length)
            if ($n -le 0) { break }
            $out.Write($buffer, 0, $n)
            $sh.Done = $sh.Done + $n
            if ($sh.Done -gt 150MB) { throw 'invalid: too big' }
        }
    } finally { $out.Dispose(); $in.Dispose(); $resp.Close() }
    if ($sh.Total -gt 0 -and $sh.Done -ne $sh.Total) { throw 'network: the download broke off' }
    Move-Item -LiteralPath $part -Destination $zipFile

    # Is it the zip of the version that was looked up, made by the public repo? The zip comment is the commit id.
    $sh.Phase = 'check'
    $bytes = [IO.File]::ReadAllBytes($zipFile)
    $comment = $null
    for ($i = $bytes.Length - 22; $i -ge [Math]::Max(0, $bytes.Length - 22 - 65535); $i--) {
        if ($bytes[$i] -eq 0x50 -and $bytes[$i + 1] -eq 0x4B -and $bytes[$i + 2] -eq 5 -and $bytes[$i + 3] -eq 6) {
            $len = $bytes[$i + 20] + 256 * $bytes[$i + 21]
            if ($i + 22 + $len -eq $bytes.Length) { $comment = [Text.Encoding]::ASCII.GetString($bytes, $i + 22, $len); break }
        }
    }
    if ($null -eq $comment) { throw 'network: the zip file is incomplete' }
    if ($comment.Trim() -ne $sha) { throw 'invalid: the zip does not name the expected version' }
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $new = Join-Path $dir 'new'
    if (Test-Path -LiteralPath $new) { Remove-Item -LiteralPath $new -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $new | Out-Null
    $zip = [IO.Compression.ZipFile]::OpenRead($zipFile)
    try {
        $top = $null
        $seen = @{}
        foreach ($entry in $zip.Entries) {
            $name = $entry.FullName.Replace('\', '/')
            $parts = @($name.Split('/') | Where-Object { $_ -ne '' })
            if ($parts.Count -eq 0) { continue }
            if ($name.StartsWith('/') -or $name -match ':' -or ($parts -contains '..')) { throw 'invalid: unsafe path in the zip' }
            if ($null -eq $top) { $top = $parts[0] } elseif ($parts[0] -ne $top) { throw 'invalid: more than one top folder' }
            if ($parts.Count -eq 1) { if ($name.EndsWith('/')) { continue } else { throw 'invalid: file outside the top folder' } }
            $rel = ($parts[1..($parts.Count - 1)]) -join '\'
            $seen[$parts[1].ToLower()] = $true
            $target = Join-Path $new $rel
            if ($name.EndsWith('/')) { New-Item -ItemType Directory -Force -Path $target | Out-Null; continue }
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $true)
        }
    } finally { $zip.Dispose() }
    # File times decide what the build tools compile again: a file with the same content as today's keeps today's time (so the
    # translator tools and the games' programs are not compiled again for nothing), a changed or new file gets the time of now
    # (the zip's own times are the commit's, which can be older than the programs that were built since).
    $stamp = [DateTime]::UtcNow
    $sha1 = [Security.Cryptography.SHA1]::Create()
    foreach ($f in Get-ChildItem -LiteralPath $new -Recurse -File -Force) {
        $rel = $f.FullName.Substring($new.Length + 1)
        $mine = Join-Path $root $rel
        $when = $stamp
        if (Test-Path -LiteralPath $mine -PathType Leaf) {
            try {
                $info = Get-Item -LiteralPath $mine -Force
                if ($info.Length -eq $f.Length -and ([BitConverter]::ToString($sha1.ComputeHash([IO.File]::ReadAllBytes($mine))) -eq [BitConverter]::ToString($sha1.ComputeHash([IO.File]::ReadAllBytes($f.FullName))))) { $when = $info.LastWriteTimeUtc }
            } catch { }   # a file in use counts as changed
        }
        $f.LastWriteTimeUtc = $when
    }
    foreach ($must in 'app\app.ps1', 'app\ui.cs', 'Start.cmd', 'scripts\port.sh', 'scripts\setup.sh', 'CMakeLists.txt') {
        if (-not (Test-Path -LiteralPath (Join-Path $new $must))) { throw "invalid: $must is missing" }
    }
    $sh.Result = 'ready'
} catch {
    $text = $_.Exception.Message
    $sh.Error = $text
    $sh.Result = if ($text -like 'invalid:*') { 'invalid' } elseif ($text -like 'network:*' -or $sh.Phase -ne 'check') { 'failed' } else { 'unpack' }
    foreach ($f in Get-ChildItem -LiteralPath $dir -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -like 'download*' }) { Remove-Item -LiteralPath $f.FullName -Force -ErrorAction SilentlyContinue }
} finally {
    if ($sh.Result -eq 'cancelled') {
        foreach ($f in Get-ChildItem -LiteralPath $dir -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -like 'download*' }) { Remove-Item -LiteralPath $f.FullName -Force -ErrorAction SilentlyContinue }
    }
}
'@

function Start-UpdateWorker([string]$mode, [string]$sha) {
    $sh = [hashtable]::Synchronized(@{ Result = ''; Sha = $sha; Error = ''; Phase = ''; Done = [double]0; Total = [double]0; Cancel = $false })
    $runspace = [runspacefactory]::CreateRunspace()
    $runspace.ApartmentState = 'MTA'
    $runspace.Open()
    $ps = [powershell]::Create()
    $ps.Runspace = $runspace
    [void]$ps.AddScript($UpdateWorker).AddArgument($sh).AddArgument($mode).AddArgument($UpdateApi).AddArgument($UpdateArchive).AddArgument($Root).AddArgument($UpdateDir).AddArgument($sha)
    $handle = $ps.BeginInvoke()
    @{ Sh = $sh; Ps = $ps; Runspace = $runspace; Handle = $handle; Mode = $mode }
}

function Stop-UpdateWorker($job) {
    if ($null -eq $job) { return }
    try { $job.Sh.Cancel = $true } catch { }
    # Never wait for the network: the thread is a background thread and ends with the app.
    try { [void]$job.Ps.BeginStop($null, $null) } catch { }
}

# A rename on the same drive: it happens completely or not at all (Move-Item copies and deletes file by file when it cannot
# rename, and a failure half way leaves a folder in two places).
function Move-Entry([string]$from, [string]$to) {
    if (Test-Path -LiteralPath $from -PathType Container) { [IO.Directory]::Move($from, $to) } else { [IO.File]::Move($from, $to) }
}

# Puts the unpacked version (tools\update\new) in place of the app's own files. Everything the player made stays (UpdateKeep).
# A top-level folder or file of the new version replaces the old one as a whole, so a source file that is gone in the new version
# is gone here too; the old ones are moved to tools\update\old first (a rename, nothing is copied), and when anything goes wrong
# they are moved back, so a failed update leaves the app as it was.
function Install-Update([string]$sha) {
    $new = Join-Path $UpdateDir 'new'
    $old = Join-Path $UpdateDir 'old'
    if (-not (Test-Path -LiteralPath (Join-Path $new 'app\app.ps1'))) { throw 'The unpacked update is incomplete.' }
    if (Test-Path -LiteralPath $old) { Remove-Item -LiteralPath $old -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $old | Out-Null
    $newNames = @(Get-ChildItem -LiteralPath $new -Force | ForEach-Object { $_.Name } | Where-Object { $UpdateKeep -notcontains $_.ToLower() })
    $manifest = Join-Path $UpdateDir 'files.txt'
    $oldNames = @()
    try { if (Test-Path -LiteralPath $manifest) { $oldNames = @(Get-Content -LiteralPath $manifest | Where-Object { $_ }) } } catch { }
    $gone = @($oldNames | Where-Object { ($newNames -notcontains $_) -and ($UpdateKeep -notcontains $_.ToLower()) -and (Test-Path -LiteralPath (Join-Path $Root $_)) })
    $undo = New-Object 'Collections.Generic.List[object]'
    try {
        foreach ($name in $gone) {
            Move-Entry (Join-Path $Root $name) (Join-Path $old $name)
            $undo.Add(@{ Kind = 'old'; Name = $name })
        }
        foreach ($name in $newNames) {
            $target = Join-Path $Root $name
            if (Test-Path -LiteralPath $target) {
                Move-Entry $target (Join-Path $old $name)
                $undo.Add(@{ Kind = 'old'; Name = $name })
            }
            Move-Entry (Join-Path $new $name) $target
            $undo.Add(@{ Kind = 'new'; Name = $name })
        }
    } catch {
        $problem = $_
        for ($i = $undo.Count - 1; $i -ge 0; $i--) {
            $step = $undo[$i]
            try {
                if ($step.Kind -eq 'new') { Move-Entry (Join-Path $Root $step.Name) (Join-Path $new $step.Name) }
                else { Move-Entry (Join-Path $old $step.Name) (Join-Path $Root $step.Name) }
            } catch { Write-AppLog ("Update rollback: " + $_.Exception.Message) }
        }
        throw $problem
    }
    # Done: remember the version and the names, and what is left to build again.
    Set-Content -LiteralPath $manifest -Value $newNames -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $UpdateDir 'installed.txt') -Value $sha -Encoding ASCII
    Set-PendingRebuild (Get-AllBuiltNames)
    foreach ($leftover in 'old', 'new') {
        try { if (Test-Path -LiteralPath (Join-Path $UpdateDir $leftover)) { Remove-Item -LiteralPath (Join-Path $UpdateDir $leftover) -Recurse -Force } } catch { }
    }
    try { Remove-Item -LiteralPath (Join-Path $UpdateDir 'download.zip') -Force } catch { }
}

# One window per app folder: two at once would build into the same folders.
$created = $false
$Mutex = New-Object Threading.Mutex($true, ('Local\Ps2NativeRecompApp_' + ($Root.ToLower() -replace '[^a-z0-9]', '_')), [ref]$created)
if (-not $created) {
    [void][Windows.Forms.MessageBox]::Show('PS2 Native Recomp is already open. Look for its window in the taskbar.', $AppTitle, 'OK', 'Information')
    exit 0
}

# ---------------------------------------------------------------------------------------------------------------------
# Child processes

$State = @{ Build = $null; Image = ''; Name = ''; Game = $null; Playing = $null; Seq = 0; Patch = $null; Newest = ''; UpdateJob = $null; Batch = $null; Restarting = $false }

# PCSX2 patches of the chosen game (optional). Status: idle (not asked yet), working, found, none (the collection has no file for
# this game) or error. Chosen holds the ticked section names; Items the tick boxes (Key = section name, S = what pnach.py says).
$WholeFile = '<the patch file as it is>'
function New-PatchState {
    @{ Status = 'idle'; Message = ''; Detail = ''; File = ''; Title = ''; Items = @(); Always = $null; Chosen = (New-Object 'Collections.Generic.List[string]'); Run = $null; Remembered = $false }
}
$State.Patch = New-PatchState

# Starts a program with its output captured. When $buildEnvironment is set the child sees only the portable tools
# (tools\portable\env.ps1 does that to this process for the moment of the start; the app's own environment is put back).
function Start-Child([string]$exe, [string]$arguments, [bool]$buildEnvironment) {
    $saved = $null
    if ($buildEnvironment) {
        $saved = [Environment]::GetEnvironmentVariables('Process')
        & { . (Join-Path $ToolsDir 'env.ps1') -Quiet -Keep 'PS2_JOBS', 'PS2_SCALE' }
    }
    try {
        $psi = New-Object Diagnostics.ProcessStartInfo
        $psi.FileName = $exe
        $psi.Arguments = $arguments
        $psi.WorkingDirectory = $Root
        $psi.UseShellExecute = $false
        $psi.CreateNoWindow = $true
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        $psi.StandardOutputEncoding = [Text.Encoding]::UTF8
        $psi.StandardErrorEncoding = [Text.Encoding]::UTF8
        $process = New-Object Diagnostics.Process
        $process.StartInfo = $psi
        $State.Seq++
        $id = "ps2app$($State.Seq)"
        [void](Register-ObjectEvent -InputObject $process -EventName OutputDataReceived -SourceIdentifier "$id.out")
        [void](Register-ObjectEvent -InputObject $process -EventName ErrorDataReceived -SourceIdentifier "$id.err")
        [void]$process.Start()
        $process.BeginOutputReadLine()
        $process.BeginErrorReadLine()
    }
    finally {
        if ($saved) {
            foreach ($name in @([Environment]::GetEnvironmentVariables('Process').Keys)) {
                if (-not $saved.Contains($name)) { [Environment]::SetEnvironmentVariable($name, $null, 'Process') }
            }
            foreach ($name in @($saved.Keys)) { [Environment]::SetEnvironmentVariable($name, [string]$saved[$name], 'Process') }
        }
    }
    @{ Process = $process; Id = $id }
}

# The lines the child printed since the last call, oldest first.
function Read-ChildOutput($child) {
    $lines = New-Object 'Collections.Generic.List[string]'
    $events = @(Get-Event -ErrorAction SilentlyContinue | Where-Object { $_.SourceIdentifier -like "$($child.Id).*" } | Sort-Object EventIdentifier)
    foreach ($e in $events) {
        $data = $e.SourceEventArgs.Data
        Remove-Event -EventIdentifier $e.EventIdentifier -ErrorAction SilentlyContinue
        if ($null -ne $data) { $lines.Add($data) }
    }
    ,$lines
}

function Stop-Child($child) {
    if ($null -eq $child) { return }
    try {
        if (-not $child.Process.HasExited) { & taskkill.exe /PID $child.Process.Id /T /F 2>&1 | Out-Null }
    } catch { }
    foreach ($sub in @(Get-EventSubscriber -ErrorAction SilentlyContinue | Where-Object { $_.SourceIdentifier -like "$($child.Id).*" })) {
        Unregister-Event -SubscriptionId $sub.SubscriptionId -ErrorAction SilentlyContinue
    }
    foreach ($e in @(Get-Event -ErrorAction SilentlyContinue | Where-Object { $_.SourceIdentifier -like "$($child.Id).*" })) {
        Remove-Event -EventIdentifier $e.EventIdentifier -ErrorAction SilentlyContinue
    }
}

# The newest "[n/m] ..." line ninja wrote to a log file it is writing right now: how far a compile is.
function Get-NinjaProgress([string]$file) {
    try {
        $info = Get-Item -LiteralPath $file -ErrorAction Stop
        if (([DateTime]::Now - $info.LastWriteTime).TotalSeconds -gt 4) { return $null }
        $stream = [IO.File]::Open($file, 'Open', 'Read', 'ReadWrite')
        try {
            $from = [Math]::Max(0, $stream.Length - 4096)
            [void]$stream.Seek($from, 'Begin')
            $bytes = New-Object byte[] ($stream.Length - $from)
            [void]$stream.Read($bytes, 0, $bytes.Length)
        } finally { $stream.Dispose() }
        $text = [Text.Encoding]::ASCII.GetString($bytes)
        $found = [regex]::Matches($text, '\[(\d+)/(\d+)\]')
        if ($found.Count -eq 0) { return $null }
        $last = $found[$found.Count - 1]
        @{ N = [int]$last.Groups[1].Value; M = [int]$last.Groups[2].Value }
    } catch { $null }
}

# ---------------------------------------------------------------------------------------------------------------------
# The window

# Layout is written for 96 dpi (800 x 600) and multiplied by $L. The process is DPI aware, so the fonts (sized in points)
# already grow with the screen scale; $FontMult only corrects them when $L differs from it. $L is the screen scale, but
# smaller when the window would not fit the screen (for example 125 % on a 1366 x 768 laptop).
# PS2APP_TEST_DPI=144 pretends to be a 150 % screen, to check the layout on a PC that has 100 %.
$Dpi = 96.0
try { $gfx = [Drawing.Graphics]::FromHwnd([IntPtr]::Zero); $Dpi = [double]$gfx.DpiX; $gfx.Dispose() } catch { }
$Real = $Dpi / 96
$L = $Real
if ($env:PS2APP_TEST_DPI) { $L = [double]$env:PS2APP_TEST_DPI / 96; $Real = 1.0 }
$area = [Windows.Forms.Screen]::PrimaryScreen.WorkingArea
$L = [Math]::Max(0.75, [Math]::Min($L, [Math]::Min(($area.Height - 48 * $Real) / 600, ($area.Width - 16 * $Real) / 800)))
$FontMult = $L / $Real
function S([double]$value) { [int][Math]::Round($value * $L) }
function New-Point([double]$x, [double]$y) { New-Object Drawing.Point((S $x), (S $y)) }
function New-Size([double]$w, [double]$h) { New-Object Drawing.Size((S $w), (S $h)) }

# The colours, written down once (RGB). The drawn controls (app\ui.cs) get the same values through Ps2Ui.Pal below.
function New-Rgb([int]$r, [int]$g, [int]$b) { [Drawing.Color]::FromArgb($r, $g, $b) }
$C = @{
    Ink = New-Rgb 232 240 255      # calm light text
    Muted = New-Rgb 166 184 218    # secondary text (still 7:1 on the dark page)
    Accent = New-Rgb 92 200 255    # the one cool accent: soft cyan-blue
    Danger = New-Rgb 255 142 142
    Good = New-Rgb 125 227 176
    Warn = New-Rgb 255 210 122
    Field = New-Rgb 10 20 48       # inside of a text box
    Page = New-Rgb 6 12 32         # behind the gradient, never seen
    Slab = New-Rgb 11 21 50        # the scrolling list of extras and the build log
}

Add-Type -TypeDefinition (Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'ui.cs')) -ReferencedAssemblies System.Windows.Forms, System.Drawing
[Ps2Ui.Pal]::Scale = $L
foreach ($key in 'Ink', 'Muted', 'Accent', 'Danger', 'Good', 'Warn', 'Field') { [Ps2Ui.Pal]::$key = $C[$key] }

function New-Font([double]$points, [bool]$bold = $false, [string]$family = 'Segoe UI') {
    $style = if ($bold) { [Drawing.FontStyle]::Bold } else { [Drawing.FontStyle]::Regular }
    New-Object Drawing.Font($family, [single]($points * $FontMult), $style, [Drawing.GraphicsUnit]::Point)
}
$TitleFont = 'Segoe UI Light'

function Add-Label($parent, [string]$name, [string]$text, [int]$x, [int]$y, [int]$w, [int]$h, $font, $color, [string]$align = 'TopLeft') {
    $label = New-Object Windows.Forms.Label
    $label.Name = $name
    $label.Text = $text
    $label.AutoSize = $false
    $label.Location = New-Point $x $y
    $label.Size = New-Size $w $h
    $label.Font = $font
    $label.ForeColor = $color
    $label.BackColor = [Drawing.Color]::Transparent
    $label.TextAlign = $align
    $label.UseMnemonic = $false
    $parent.Controls.Add($label)
    $label
}

function New-Glass($parent, [string]$name, [int]$x, [int]$y, [int]$w, [int]$h, [int]$glyph = 0) {
    $glass = New-Object Ps2Ui.Glass
    $glass.Name = $name
    $glass.Location = New-Point $x $y
    $glass.Size = New-Size $w $h
    $glass.Glyph = $glyph
    $parent.Controls.Add($glass)
    $glass
}

function Add-Badge($parent, [string]$name, [string]$number, [int]$x, [int]$y) {
    $badge = New-Object Ps2Ui.Badge
    $badge.Name = $name
    $badge.Text = $number
    $badge.Font = New-Font 11.5 $true
    $badge.Location = New-Point $x $y
    $badge.Size = New-Size 30 30
    $parent.Controls.Add($badge)
    $badge
}

function Set-ButtonEnabled($button, [bool]$enabled) { $button.Enabled = $enabled }

function Add-Button($parent, [string]$name, [string]$text, [int]$x, [int]$y, [int]$w, [int]$h, [string]$kind, [double]$points) {
    $button = New-Object Ps2Ui.Btn
    $button.Name = $name
    $button.Text = $text
    $button.AccessibleName = $text
    $button.Tag = $kind
    $button.Kind = $(if ($kind -eq 'primary') { 1 } else { 0 })
    $button.Location = New-Point $x $y
    $button.Size = New-Size $w $h
    $button.Font = New-Font $points ($kind -eq 'primary')
    $button.Cursor = [Windows.Forms.Cursors]::Hand
    $parent.Controls.Add($button)
    $button
}

# Our own small icon (a disc on a blue tile), drawn in code.
function New-AppIcon {
    $bmp = New-Object Drawing.Bitmap 32, 32
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $tile = [Ps2Ui.Pal]::Round((New-Object Drawing.RectangleF(1, 1, 30, 30)), 7)
    $brush = New-Object Drawing.Drawing2D.LinearGradientBrush((New-Object Drawing.Rectangle(0, 0, 32, 33)), (New-Rgb 52 126 238), (New-Rgb 8 22 74), 90)
    $g.FillPath($brush, $tile)
    $ring = New-Object Drawing.Pen((New-Rgb 150 224 255), 2.4)
    $g.DrawEllipse($ring, 7, 7, 18, 18)
    $g.FillEllipse((New-Object Drawing.SolidBrush((New-Rgb 8 22 74))), 13, 13, 6, 6)
    $g.Dispose()
    [Drawing.Icon]::FromHandle($bmp.GetHicon())
}

$form = New-Object Windows.Forms.Form
$form.Text = $AppTitle
$form.AutoScaleMode = 'None'
$form.ClientSize = New-Size 800 600
$form.StartPosition = 'CenterScreen'
$form.FormBorderStyle = 'FixedSingle'
$form.MaximizeBox = $false
$form.BackColor = $C.Page
$form.Font = New-Font 11
try { $form.Icon = New-AppIcon } catch { Write-AppLog $_.Exception.ToString() }

$P = @{}   # the screens
foreach ($screen in 'welcome', 'choose', 'patches', 'build', 'done', 'update') {
    $panel = New-Object Ps2Ui.Page
    $panel.Name = "screen-$screen"
    $panel.Dock = 'Fill'
    $panel.Visible = $false
    $form.Controls.Add($panel)
    $P[$screen] = $panel
}
$U = @{}   # the controls the code talks to

# --- welcome: what the app does in three cards, and the games built before (like saves on a memory card)
$w = $P['welcome']
[void](Add-Label $w 'title' $AppTitle 38 16 720 54 (New-Font 28 $false $TitleFont) $C.Ink)
[void](Add-Label $w 'subtitle' 'Turn your own PlayStation 2 game into a program for this PC.' 41 70 720 26 (New-Font 12) $C.Muted)
$steps = @(
    @('1', 'Choose your disc', 'An .iso file ripped from a PS2 DVD you own (or a .zip or .7z with an .iso inside).'),
    @('2', 'Press Build', "Free build tools are downloaded once (about $ToolsDownloadMb MB). Then the game becomes a PC program: 5 to 30 minutes."),
    @('3', 'Press Play', 'Start the game from here, or make a desktop shortcut and start it from there next time.')
)
$x = 40
$U.stepCards = @()
foreach ($s in $steps) {
    $card = New-Glass $w "stepcard$($s[0])" $x 104 232 142
    $U.stepCards += $card
    [void](Add-Badge $card "step$($s[0])" $s[0] 14 14)
    [void](Add-Label $card "step$($s[0])-title" $s[1] 54 16 170 26 (New-Font 12 $true) $C.Ink)
    [void](Add-Label $card "step$($s[0])-text" $s[2] 16 54 204 84 (New-Font 10) $C.Muted)
    $x += 244
}
$U.gamesHeader = Add-Label $w 'gamesHeader' 'YOUR GAMES' 40 258 110 20 (New-Font 9.5 $true) $C.Accent
$U.gamesRule = New-Object Ps2Ui.Rule
$U.gamesRule.Location = New-Point 142 267
$U.gamesRule.Size = New-Size 618 4
$w.Controls.Add($U.gamesRule)
$U.gamesEmpty = Add-Label $w 'gamesEmpty' 'Nothing built yet. A game you build shows up here like a save on a memory card, with a Play button.' 40 288 720 48 (New-Font 11.5) $C.Muted
$U.installProblem = Add-Label $w 'installProblem' '' 40 288 720 150 (New-Font 11.5 $true) $C.Danger
$U.start = Add-Button $w 'Start' 'Add a game' 240 502 320 58 'primary' 17
$U.privacy = Add-Label $w 'privacy' 'Nothing is uploaded. Everything stays in this app''s folder (about 2 GB of disk space is needed).' 40 568 720 22 (New-Font 9.5) $C.Muted 'TopCenter'
# The games built before: one row each (Play, Extras, and Make it faster while vector programs wait to be compiled).
$U.again = New-Object Ps2Ui.Clear
$U.again.Name = 'again'
$U.again.Location = New-Point 40 284
$U.again.Size = New-Size 720 212
$w.Controls.Add($U.again)
$U.fasterButtons = @{}
$U.patchMode = ''
# The notice in the place of the three cards: a newer version is out (Update), or the app was updated and the games should be built
# again (Build my games again). See Update-Banner.
$U.noteCard = New-Glass $w 'NoteCard' 40 104 720 142
$U.noteCard.Visible = $false
$U.noteTitle = Add-Label $U.noteCard 'noteTitle' '' 18 12 440 30 (New-Font 15 $true) $C.Accent
$U.noteText = Add-Label $U.noteCard 'noteText' '' 18 44 440 62 (New-Font 10.5) $C.Ink
$U.noteSmall = Add-Label $U.noteCard 'noteSmall' '' 18 108 440 30 (New-Font 9) $C.Muted
$U.noteButton = Add-Button $U.noteCard 'NoteButton' 'Update' 478 18 226 66 'primary' 18
$U.noteLater = Add-Button $U.noteCard 'NoteLater' 'Not now' 478 92 226 40 'secondary' 11
$U.noteMode = ''

# --- choose: the disc image and the name, then a card for the extras, then Build
$w = $P['choose']
[void](Add-Label $w 'title' 'Add a game' 38 16 720 54 (New-Font 28 $false $TitleFont) $C.Ink)
$U.chooseSub = Add-Label $w 'text' 'Pick your disc, choose extras if you like, then press Build.' 41 70 720 26 (New-Font 12) $C.Muted
$card = New-Glass $w 'DiscCard' 40 108 720 238
[void](Add-Badge $card 'badge1' '1' 16 14)
[void](Add-Label $card 'cardTitle1' 'Your disc image' 56 16 400 26 (New-Font 13 $true) $C.Ink)
$U.choose = Add-Button $card 'ChooseImage' 'Choose disc image...' 16 54 250 50 'primary' 14
$U.fileName = Add-Label $card 'fileName' 'No disc image chosen yet.' 284 52 420 26 (New-Font 12.5 $true) $C.Muted
$U.filePath = Add-Label $card 'filePath' 'An .iso made from a PS2 DVD you own, or a .zip or .7z file that holds one.' 284 80 420 24 (New-Font 9.5) $C.Muted
$U.filePath.AutoEllipsis = $true
$U.imageError = Add-Label $card 'imageError' '' 16 110 688 38 (New-Font 11 $true) $C.Danger
[void](Add-Label $card 'nameLabel' 'Name of the game' 16 150 300 22 (New-Font 11 $true) $C.Ink)
$U.nameField = New-Object Ps2Ui.Field
$U.nameField.Name = 'GameNameFrame'
$U.nameField.Location = New-Point 16 174
$U.nameField.Size = New-Size 300 40
$card.Controls.Add($U.nameField)
$U.name = New-Object Windows.Forms.TextBox
$U.name.Name = 'GameName'
$U.name.AccessibleName = 'Name of the game'
$U.name.BorderStyle = 'None'
$U.name.BackColor = $C.Field
$U.name.ForeColor = $C.Ink
$U.name.Location = New-Point 12 9
$U.name.Size = New-Size 276 24
$U.name.Font = New-Font 13
$U.name.MaxLength = 32
$U.nameField.Controls.Add($U.name)
$U.name.Add_Enter({ $U.nameField.Hot = $true; $U.nameField.Invalidate() })
$U.name.Add_Leave({ $U.nameField.Hot = $false; $U.nameField.Invalidate() })
$U.nameHint = Add-Label $card 'nameHint' "Letters, numbers, - and _ only. It names the game's folder and its memory card." 336 176 368 38 (New-Font 10) $C.Muted
$U.nameError = Add-Label $card 'nameError' '' 16 214 688 20 (New-Font 10.5 $true) $C.Danger
$card = New-Glass $w 'ExtrasCard' 40 358 720 120
[void](Add-Badge $card 'badge2' '2' 16 14)
[void](Add-Label $card 'cardTitle2' 'Extras (optional)' 56 16 400 26 (New-Font 13 $true) $C.Ink)
$U.patchSummary = Add-Label $card 'PatchSummary' '' 16 52 470 62 (New-Font 10.5) $C.Muted
$U.patchButton = Add-Button $card 'Patches' 'Choose extras...' 500 40 204 56 'secondary' 13
$U.buildNote = Add-Label $w 'buildNote' 'The first build takes 5 to 30 minutes.' 300 486 460 20 (New-Font 10) $C.Muted 'TopRight'
$U.chooseBack = Add-Button $w 'Back' 'Back' 40 512 150 52 'secondary' 13
$U.build = Add-Button $w 'Build' 'Build' 440 508 320 60 'primary' 17

# --- extras (the patches): what they are, what was found for this game, what is switched on
$w = $P['patches']
[void](Add-Label $w 'title' 'Extras for this game' 38 16 720 54 (New-Font 28 $false $TitleFont) $C.Ink)
$U.patchGame = Add-Label $w 'patchGame' '' 41 70 720 26 (New-Font 12) $C.Muted
$U.patchCard = New-Glass $w 'ExtrasInfoCard' 40 104 720 176
$U.patchIntro = Add-Label $U.patchCard 'patchIntro' '' 16 12 688 200 (New-Font 10.5) $C.Ink
$U.lookup = Add-Button $U.patchCard 'LookForPatches' 'Look for extras for this game' 16 120 330 46 'primary' 13
$U.patchStatus = Add-Label $U.patchCard 'PatchStatus' '' 362 118 342 50 (New-Font 11.5 $true) $C.Ink 'MiddleLeft'
$U.patchDetail = Add-Label $U.patchCard 'PatchDetail' '' 16 38 520 30 (New-Font 10) $C.Muted
$U.patchList = New-Object Windows.Forms.Panel
$U.patchList.Name = 'PatchList'
$U.patchList.AccessibleName = 'Extras found'
$U.patchList.Location = New-Point 40 192
$U.patchList.Size = New-Size 720 304
$U.patchList.AutoScroll = $true
$U.patchList.BorderStyle = 'None'
$U.patchList.BackColor = $C.Slab
$U.patchList.Visible = $false
$w.Controls.Add($U.patchList)
$U.patchList.Add_MouseEnter({ $U.patchList.Focus() })
# Dark scroll bars (Windows 10 1809 and newer; older Windows keeps the light ones).
try {
    Add-Type -Namespace Ps2App -Name Theme -MemberDefinition '[System.Runtime.InteropServices.DllImport("uxtheme.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)] public static extern int SetWindowTheme(System.IntPtr h, string app, string list);'
    foreach ($dark in @($U.patchList)) { [void][Ps2App.Theme]::SetWindowTheme($dark.Handle, 'DarkMode_Explorer', $null) }
} catch { }
$U.patchNote = Add-Label $w 'patchNote' '' 40 504 380 56 (New-Font 10.5) $C.Muted
$U.patchesDone = Add-Button $w 'PatchesDone' 'Skip extras' 440 504 320 60 'primary' 16

# --- build
$w = $P['build']
$U.buildTitle = Add-Label $w 'buildTitle' 'Building' 38 16 720 54 (New-Font 26 $false $TitleFont) $C.Ink
$U.buildCard = New-Glass $w 'BuildCard' 40 84 720 206
$U.status = Add-Label $U.buildCard 'status' '' 18 14 684 58 (New-Font 15 $true) $C.Ink
$U.detail = Add-Label $U.buildCard 'detail' '' 18 74 684 44 (New-Font 11) $C.Muted
$U.bar = New-Object Ps2Ui.Bar
$U.bar.Name = 'Progress'
$U.bar.AccessibleName = 'Progress'
$U.bar.AccessibleRole = 'ProgressBar'
$U.bar.Location = New-Point 18 128
$U.bar.Size = New-Size 684 22
$U.bar.Minimum = 0
$U.bar.Maximum = 1000
$U.buildCard.Controls.Add($U.bar)
$U.elapsed = Add-Label $U.buildCard 'elapsed' '' 18 158 360 22 (New-Font 10.5) $C.Muted
$U.percent = Add-Label $U.buildCard 'percent' '' 502 156 200 24 (New-Font 11 $true) $C.Ink 'TopRight'
$U.buildExtras = Add-Label $U.buildCard 'buildExtras' '' 18 180 684 20 (New-Font 10) $C.Muted
$U.tip = Add-Label $w 'tip' 'This can take a while. You can leave this window open and use your PC for other things. Please do not shut the PC down.' 42 306 716 50 (New-Font 11) $C.Muted
$U.log = New-Object Windows.Forms.TextBox
$U.log.Name = 'Log'
$U.log.AccessibleName = 'Build log'
$U.log.Multiline = $true
$U.log.ReadOnly = $true
$U.log.ScrollBars = 'Vertical'
$U.log.WordWrap = $true
$U.log.MaxLength = 4000000
$U.log.BorderStyle = 'FixedSingle'
$U.log.BackColor = $C.Field
$U.log.ForeColor = $C.Ink
$U.log.Font = New-Font 9 $false 'Consolas'
$U.log.Location = New-Point 40 302
$U.log.Size = New-Size 720 194
$U.log.Visible = $false
$w.Controls.Add($U.log)
try { [void][Ps2App.Theme]::SetWindowTheme($U.log.Handle, 'DarkMode_Explorer', $null) } catch { }
$U.details = Add-Button $w 'ShowDetails' 'Show details' 40 508 200 52 'secondary' 13
$U.cancel = Add-Button $w 'Cancel' 'Cancel' 600 508 160 52 'secondary' 13
$U.retry = Add-Button $w 'TryAgain' 'Try again' 400 502 180 58 'primary' 14
$U.buildBack = Add-Button $w 'BackToChoose' 'Back' 600 508 160 52 'secondary' 13

# --- done: the game like a save on a memory card, then Play
$w = $P['done']
[void](Add-Label $w 'title' 'Your game is ready' 40 14 720 28 (New-Font 13 $true) $C.Accent)
$U.doneCard = New-Glass $w 'GameCard' 40 48 720 178 1
$U.doneName = Add-Label $U.doneCard 'doneName' '' 76 10 628 40 (New-Font 22 $true) $C.Ink
$U.doneText = Add-Label $U.doneCard 'doneText' '' 76 52 628 40 (New-Font 11) $C.Muted
$U.doneChipsLabel = Add-Label $U.doneCard 'doneExtrasLabel' 'Extras:' 76 98 60 26 (New-Font 10.5 $true) $C.Muted 'MiddleLeft'
$U.doneHint = Add-Label $U.doneCard 'doneHint' '' 76 130 628 42 (New-Font 10) $C.Muted
$U.play = Add-Button $w 'Play' 'Play' 220 236 360 66 'primary' 22
$U.shortcut = Add-Button $w 'MakeShortcut' 'Make desktop shortcut' 220 308 360 46 'secondary' 14
$U.doneResult = Add-Label $w 'doneResult' '' 40 358 720 22 (New-Font 11) $C.Muted 'TopCenter'
# Make it faster: shown only when vector programs were recorded since the last build (see Get-RecordedCount).
$U.fasterBox = New-Glass $w 'FasterBox' 40 384 720 80
$U.fasterBox.Visible = $false
[void](Add-Label $U.fasterBox 'FasterText' 'Was the game slow in places you saw for the first time? Make it faster (takes a few minutes at most).' 16 6 470 46 (New-Font 11) $C.Ink)
$U.fasterCount = Add-Label $U.fasterBox 'FasterCount' '' 16 54 470 22 (New-Font 10) $C.Muted
$U.faster = Add-Button $U.fasterBox 'MakeFaster' 'Make it faster' 496 15 208 50 'secondary' 14
[void](Add-Label $w 'doneTip' 'Tip: in the game window, F1 opens the menu (saves, keys, picture) and F11 switches full screen on and off.' 40 470 720 22 (New-Font 10) $C.Muted 'TopCenter')
$U.another = Add-Button $w 'BuildAnother' 'Add another game' 40 512 210 48 'secondary' 12
$U.changeExtras = Add-Button $w 'ChangeExtras' 'Change extras...' 262 512 210 48 'secondary' 12
$U.close = Add-Button $w 'Close' 'Close' 620 512 140 48 'secondary' 12

# --- update: downloading the newest version, a failure, and the result of Build my games again
$w = $P['update']
$U.updTitle = Add-Label $w 'updTitle' 'Updating the app' 38 16 720 54 (New-Font 26 $false $TitleFont) $C.Ink
$U.updCard = New-Glass $w 'UpdateCard' 40 84 720 170
$U.updStatus = Add-Label $U.updCard 'updStatus' '' 18 14 684 58 (New-Font 15 $true) $C.Ink
$U.updDetail = Add-Label $U.updCard 'updDetail' '' 18 74 684 44 (New-Font 11) $C.Muted
$U.updBar = New-Object Ps2Ui.Bar
$U.updBar.Name = 'UpdateProgress'
$U.updBar.AccessibleName = 'Progress'
$U.updBar.AccessibleRole = 'ProgressBar'
$U.updBar.Location = New-Point 18 128
$U.updBar.Size = New-Size 684 22
$U.updBar.Minimum = 0
$U.updBar.Maximum = 1000
$U.updCard.Controls.Add($U.updBar)
$U.updList = Add-Label $w 'updList' '' 44 270 712 226 (New-Font 10.5) $C.Ink
$U.updCancel = Add-Button $w 'UpdateCancel' 'Cancel' 600 508 160 52 'secondary' 13
$U.updRetry = Add-Button $w 'UpdateTryAgain' 'Try again' 400 502 180 58 'primary' 14
$U.updBack = Add-Button $w 'UpdateBack' 'Back' 600 508 160 52 'secondary' 13
$U.updDone = Add-Button $w 'UpdateDone' 'Done' 440 502 320 60 'primary' 17

function Show-Screen([string]$name) {
    foreach ($key in @($P.Keys)) { $P[$key].Visible = ($key -eq $name) }
    switch ($name) {
        'welcome' { $form.AcceptButton = $U.start; Update-Welcome }
        'choose' { $form.AcceptButton = $U.build; Update-ChooseState }
        'patches' { $form.AcceptButton = $U.patchesDone; Update-PatchScreen }
        'done' { $form.AcceptButton = $U.play; Update-FasterOffers }
        'update' { $form.AcceptButton = $null }
        default { $form.AcceptButton = $null }
    }
}

# --- Words and shapes for the extras (patches). The patch file's own names are PCSX2's; this is only how they are shown.
# Name, PCSX2's note and pnach.py's "looks like a 16:9 set" flag in; a short label, a title and plain sentences out.
function Get-ExtraKind([string]$name, [string]$description, [bool]$wide) {
    if ($name -eq $WholeFile) {
        return @{ Kind = 'file'; Short = 'Patch file'; Title = 'Use the patch file as it is'; Does = 'The file has no named parts, so it is used as a whole.'; Careful = '' }
    }
    if ($wide -or $name -match '16\s*[:/x]\s*9|wide\s*screen') {
        return @{ Kind = 'wide'; Short = 'Widescreen'; Title = 'Widescreen (16:9)'; Does = 'Shows the game in 16:9 for a wide monitor, instead of the old 4:3 picture.'
                  Careful = 'Menus, videos and some effects may look stretched or cut off.' }
    }
    if ($name -match '60\s*-?\s*fps|\bfps\b|frame\s*rate|60\s*frames') {
        return @{ Kind = 'fps'; Short = '60 fps'; Title = '60 frames per second'; Does = 'Asks the game for 60 pictures a second instead of 30, so movement looks smoother.'
                  Careful = 'Some games tie their speed to the frame rate and may run too fast or act strangely. It also needs a fast PC.' }
    }
    $note = if ($description) { " ($description)" } else { '' }
    if ($name -match 'skip' -and $name -match 'cut\s*scene|intro|movie|video|fmv|logo|cinematic') {
        return @{ Kind = 'skip'; Short = 'Skip videos'; Title = 'Skip videos'; Does = "Lets you skip the game's videos or intro screens$note."; Careful = 'You may miss parts of the story.' }
    }
    if ($name -match 'infinite|unlimited|invincib|god\s*mode|cheat|unlock|all\s+(items|weapons|cars)|no\s+damage|one\s*hit') {
        return @{ Kind = 'cheat'; Short = 'Cheat'; Title = $name; Does = "A cheat: it changes the rules of the game$note."; Careful = 'It can make the game too easy and may change your saved games.' }
    }
    $does = if ($description) { "$description" } else { 'The patch file gives no description.' }
    if ($does -notmatch '[.!?)"]$') { $does += '.' }
    @{ Kind = 'other'; Short = $name; Title = $name; Does = $does; Careful = '' }
}

# The short labels of what a built game has, from the ticked sections kept in app.json.
function Get-ExtraLabels($game) {
    $labels = @()
    if ($game) {
        foreach ($section in @($game.sections)) { $labels += (Get-ExtraKind $section '' $false).Short }
        if ($game.widescreen -and ($labels -notcontains 'Widescreen')) { $labels = @('Widescreen') + $labels }
    }
    @($labels | Select-Object -Unique)
}

# Chips for those labels, left to right from $x; what does not fit before $maxRight becomes "+n".
function Add-ExtraChips($parent, $labels, [int]$x, [int]$y, [int]$maxRight, [string]$namePrefix) {
    $index = 0
    foreach ($label in $labels) {
        $index++
        $chip = New-Object Ps2Ui.Chip
        $chip.Name = "$namePrefix-$index"
        $chip.Text = $label
        $chip.Tone = 3
        $chip.Font = New-Font 9
        $chip.Fit()
        $remaining = $labels.Count - $index
        if ($x + $chip.Width -gt (S $maxRight) -and $index -gt 1) {
            $chip.Text = '+{0}' -f ($labels.Count - $index + 1); $chip.Tone = 0; $chip.Fit()
            $chip.Location = New-Object Drawing.Point($x, (S $y)); $parent.Controls.Add($chip)
            return
        }
        $chip.Location = New-Object Drawing.Point($x, (S $y))
        $parent.Controls.Add($chip)
        $x += $chip.Width + (S 6)
    }
}

function Format-Took([string]$seconds) {
    $n = 0
    if (-not [int]::TryParse($seconds, [ref]$n) -or $n -le 0) { return '' }
    if ($n -lt 90) { return 'under 2 minutes' }
    '{0} minutes' -f [int][Math]::Round($n / 60)
}

function Format-Built([string]$built) {
    try { return ([datetime]$built).ToString('d MMM yyyy, HH:mm') } catch { return '' }
}

# ---------------------------------------------------------------------------------------------------------------------
# Welcome and choose

$InstallProblem = Get-InstallProblem
$Tip = New-Object Windows.Forms.ToolTip

function Start-Game([string]$name) {
    $play = Get-PlayCommand $name
    if (-not $play) { [void][Windows.Forms.MessageBox]::Show("The game '$name' is not built yet (or its files are gone). Please build it again.", $AppTitle, 'OK', 'Information'); return $false }
    if (-not (Test-Path -LiteralPath $play.Iso)) {
        [void][Windows.Forms.MessageBox]::Show("The disc image of '$name' is not at its old place any more:`n$($play.Iso)`nPut it back, or build the game again.", $AppTitle, 'OK', 'Information')
        return $false
    }
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $play.Exe
    $psi.Arguments = $play.Arguments
    $psi.WorkingDirectory = $play.Dir
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $State.Playing = [Diagnostics.Process]::Start($psi)
    $State.PlayingName = $name
    $playTimer.Start()
    $true
}

# While a game that this app started is open: when it ends, look again whether it recorded vector programs (Make it faster). A game
# started from a desktop shortcut is not watched; the app looks again when its window is activated (Add_Activated below).
$playTimer = New-Object Windows.Forms.Timer
$playTimer.Interval = 1500
$playTimer.Add_Tick({
    try {
        if ($null -eq $State.Playing -or $State.Playing.HasExited) {
            # Closing the game window ends it with code 0; anything else is a crash, and the game has no console to say so.
            $code = if ($State.Playing) { $State.Playing.ExitCode } else { 0 }
            $State.Playing = $null; $playTimer.Stop(); Update-FasterOffers
            if ($code -ne 0) {
                $logs = Get-GameFile $State.PlayingName 'logs'
                [void][Windows.Forms.MessageBox]::Show("The game '$($State.PlayingName)' stopped with an error. If it keeps happening, the newest file in this folder says what went wrong:`n$logs", $AppTitle, 'OK', 'Information')
            }
        }
    } catch { $playTimer.Stop(); Write-AppLog $_.Exception.ToString() }
})

function Update-Welcome {
    Update-Banner
    $U.installProblem.Text = if ($InstallProblem) { $InstallProblem } else { '' }
    $U.installProblem.Visible = [bool]$InstallProblem
    $U.privacy.Visible = -not $InstallProblem
    Set-ButtonEnabled $U.start (-not $InstallProblem)
    $U.start.Visible = -not $InstallProblem
    foreach ($old in @($U.again.Controls)) { $U.again.Controls.Remove($old); $old.Dispose() }
    $U.fasterButtons = @{}
    $games = @(Get-BuiltGames)
    $show = ($games.Count -gt 0 -and -not $InstallProblem)
    $U.gamesEmpty.Visible = (-not $show) -and (-not $InstallProblem)
    $U.gamesHeader.Visible = -not $InstallProblem
    $U.gamesRule.Visible = -not $InstallProblem
    if (-not $show) { return }
    $y = 0
    foreach ($g in $games) {
        $gameName = $g.name
        $row = New-Glass $U.again "Game-$gameName" 0 $y 720 66 1
        $row.Strong = $false
        $title = Add-Label $row "GameTitle-$gameName" $gameName 64 6 270 26 (New-Font 13 $true) $C.Ink
        $title.AutoEllipsis = $true
        $built = Format-Built $g.built
        $caption = if ($built) { "Built $built" } else { 'Built' }
        $line = Add-Label $row "GameBuilt-$gameName" $caption 64 36 150 20 (New-Font 9.5) $C.Muted
        $labels = @(Get-ExtraLabels $g)
        if ($labels.Count -gt 0) { Add-ExtraChips $row $labels (S 216) 36 515 "GameExtra-$gameName" }
        else { [void](Add-Label $row "GameNoExtras-$gameName" 'no extras' 216 36 100 20 (New-Font 9.5) $C.Muted) }
        $b = Add-Button $row "Play-$gameName" 'Play' 626 9 86 48 'primary' 13
        $b.AccessibleName = "Play $gameName"
        $b.Add_Click({ if (Start-Game $gameName) { } }.GetNewClosure())
        $e = Add-Button $row "Extras-$gameName" 'Extras...' 526 9 94 48 'secondary' 10.5
        $e.AccessibleName = "Change the extras of $gameName"
        $Tip.SetToolTip($e, 'Change the extras (widescreen, 60 fps ...) of this game. The game is built again with them.')
        $e.Add_Click({ Open-ExtrasFor $gameName }.GetNewClosure())
        # Next to the title, only while vector programs were recorded since the build: same action as on the Done screen.
        $f = Add-Button $row "Faster-$gameName" 'Make it faster' 340 5 180 30 'secondary' 9.5
        $f.Visible = $false
        $f.Add_Click({ Start-Faster $gameName }.GetNewClosure())
        $U.fasterButtons[$gameName] = $f
        $y += 72
    }
    Update-FasterOffers
}

# The Make it faster offers that are on screen: the box of the Done screen and the buttons in the rows of the welcome screen.
# Only text and visibility change here, no control is replaced, so a click that is just landing is never lost. Called when
# a screen opens and whenever the window is activated again (the game window was in front while it recorded).
function Update-FasterOffers {
    if ($P['done'].Visible -and $State.Game) {
        $count = Get-RecordedCount $State.Game
        $U.fasterBox.Visible = $count -gt 0
        if ($count -gt 0) { $U.fasterCount.Text = Format-RecordedText $count }
    }
    if ($P['welcome'].Visible) {
        foreach ($gameName in @($U.fasterButtons.Keys)) {
            $button = $U.fasterButtons[$gameName]
            $count = Get-RecordedCount $gameName
            $button.Visible = $count -gt 0
            if ($count -gt 0) {
                $button.Text = "Make it faster ($count)"
                $button.AccessibleName = $button.Text
                $Tip.SetToolTip($button, ((Format-RecordedText $count) + ' The game may be slow in places you saw for the first time. Making it faster takes a few minutes at most.'))
            }
        }
    }
}

function Format-RecordedText([int]$count) {
    if ($count -eq 1) { return '1 new vector program was recorded since the last build.' }
    "$count new vector programs were recorded since the last build."
}

function Update-ChooseState {
    $problem = if ($State.Image) { Get-NameProblem $U.name.Text.Trim() $State.Image } else { $null }
    $U.nameError.Text = if ($State.Image -and $problem) { $problem } else { '' }
    Set-ButtonEnabled $U.build ([bool]$State.Image -and -not $problem)
    Set-ButtonEnabled $U.patchButton ([bool]$State.Image)
    $U.patchSummary.Text = if ($State.Image) { Get-PatchSummary } else { 'Choose a disc image first. Then you can look for widescreen, 60 fps and other extras for this game.' }
    $U.patchSummary.ForeColor = if ($State.Image) { $C.Ink } else { $C.Muted }
}

function Set-Image([string]$path) {
    $problem = Test-DiscImage $path
    Reset-Patches
    $U.imageError.Text = if ($problem) { $problem } else { '' }
    $U.chooseSub.Text = 'Pick your disc, choose extras if you like, then press Build.'
    $U.choose.Kind = $(if ($problem) { 1 } else { 0 })
    $U.choose.Invalidate()
    if ($problem) {
        $State.Image = ''
        $U.fileName.Text = 'That file cannot be used.'
        $U.fileName.ForeColor = $C.Danger
        $U.filePath.Text = $path
        $U.name.Text = ''
    } else {
        $State.Image = $path
        $U.fileName.Text = '{0}   ({1})' -f [IO.Path]::GetFileName($path), (Format-Size (Get-Item -LiteralPath $path).Length)
        $U.fileName.ForeColor = $C.Ink
        $U.filePath.Text = $path
        $U.name.Text = Get-FreeName (Get-SafeName $path) $path
        Restore-Patches $U.name.Text $path
    }
    Update-ChooseState
}

$U.start.Add_Click({ Show-Screen 'choose' })
$U.chooseBack.Add_Click({ Show-Screen 'welcome' })
$U.choose.Add_Click({
    $dialog = New-Object Windows.Forms.OpenFileDialog
    $dialog.Title = 'Choose your PS2 disc image'
    $dialog.Filter = 'PS2 disc images (*.iso, *.zip, *.7z)|*.iso;*.zip;*.7z'
    $dialog.CheckFileExists = $true
    if (Test-Path -LiteralPath (Join-Path $Root 'isos')) { $dialog.InitialDirectory = Join-Path $Root 'isos' }
    if ($dialog.ShowDialog($form) -eq 'OK') { Set-Image $dialog.FileName }
    $dialog.Dispose()
})
$U.name.Add_TextChanged({ Update-ChooseState })
$U.patchButton.Add_Click({ Show-Screen 'patches' })

# Change the extras of a game that is built already: the choose screen with its disc image and name filled in, then the extras
# screen. Building again is what puts the new extras into the game.
function Open-ExtrasFor([string]$name) {
    $game = Read-Game $name
    $image = if ($game) { $game.source } else { '' }
    if (-not $image -or -not (Test-Path -LiteralPath $image)) {
        [void][Windows.Forms.MessageBox]::Show("The disc image of '$name' is not at its old place any more:`n$image`nPut it back, or add the game again from its disc image.", $AppTitle, 'OK', 'Information')
        return
    }
    Set-Image $image
    if (-not $State.Image) { Show-Screen 'choose'; return }
    $U.name.Text = $name
    Reset-Patches
    Restore-Patches $name $image
    $U.chooseSub.Text = "Changing the extras of ${name}: choose them, then press Build. The game is built again with them."
    Show-Screen 'patches'
}

# ---------------------------------------------------------------------------------------------------------------------
# Patches (optional): the step between choosing the image and building. Only when the player presses "Look for patches" does the
# app ask PCSX2's public patch collection for the file of this game (scripts\pnach.py fetch --json, run with the app's own
# Python). The sections of that file become tick boxes; the ticked ones go to port.sh as --pnach / --pnach-section. Details:
# docs\pnach.md and docs\app.md

function Get-ChosenKeys {
    $p = $State.Patch
    if ($p.Items.Count -eq 0) { return @($p.Chosen) }   # remembered from the last build, the file has not been read yet
    @($p.Items | Where-Object { $p.Chosen.Contains($_.Key) } | ForEach-Object { $_.Key })
}

# What the next build gets: the patch file and the ticked sections (Chosen also keeps the "whole file" box, Sections does not).
function Get-BuildPatches {
    $keys = @(Get-ChosenKeys)
    if ($keys.Count -eq 0 -or -not $State.Patch.File) { return @{ File = ''; Sections = @(); Chosen = @() } }
    @{ File = $State.Patch.File; Sections = @($keys | Where-Object { $_ -ne $WholeFile }); Chosen = $keys }
}

function Get-PatchSummary {
    $keys = @(Get-ChosenKeys)
    if ($keys.Count -gt 0) {
        $names = @($keys | ForEach-Object { (Get-ExtraKind $_ '' $false).Short })
        $text = 'Chosen: ' + ($names -join ', ') + '.'
        if ($text.Length -gt 90) { $text = '{0} extras chosen.' -f $keys.Count }
        return $text + ' They are built into the game when you press Build.'
    }
    switch ($State.Patch.Status) {
        'none' { return 'The patch collection has nothing for this game. It will be built as it is.' }
        'found' { return 'Extras exist for this game, but none is chosen. It will be built as it is.' }
    }
    'Widescreen, 60 fps and other community patches. Nothing is downloaded until you ask.'
}

function Reset-Patches {
    if ($State.Patch -and $State.Patch.Run) { Stop-PatchRun }
    $State.Patch = New-PatchState
    Clear-PatchList
}

# The choice made at the last build of this very disc image, if the patch file is still there.
function Restore-Patches([string]$name, [string]$image) {
    $game = Read-Game $name
    if (-not $game -or $game.source -ine $image -or -not $game.pnach -or $game.sections.Count -eq 0) { return }
    $file = Resolve-AppPath $game.pnach
    if (-not (Test-Path -LiteralPath $file)) { return }
    $p = $State.Patch
    $p.File = $file
    $p.Remembered = $true
    foreach ($section in $game.sections) { $p.Chosen.Add($section) }
}

# The disc image the lookup reads. pnach.py needs a plain .iso; from an archive the game's own copy is used once a build made it.
function Get-PatchImage {
    if ($State.Image -match '\.iso$') { return $State.Image }
    $name = $U.name.Text.Trim()
    if (Test-GameName $name) {
        $copy = Get-GameFile $name 'disc.iso'
        if (Test-Path -LiteralPath $copy) { return $copy }
    }
    $null
}

function Set-PatchError([string]$message, [string]$detail) {
    $p = $State.Patch
    $p.Status = 'error'
    $p.Message = $message
    $p.Detail = $detail
}

function Start-PatchRun([string]$mode, [string]$then = '') {
    $p = $State.Patch
    $image = Get-PatchImage
    $script = Join-Path $Root 'scripts\pnach.py'
    $python = Join-Path $ToolsDir 'python\python.exe'
    try {
        switch ($mode) {
            'python' {
                $helper = Join-Path $Root 'app\get-python.ps1'
                $powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
                $child = Start-Child $powershell "-NoProfile -ExecutionPolicy Bypass -File `"$helper`"" $false
                $p.Message = 'Downloading a small helper program (Python, 11 MB) ...'
            }
            'fetch' {
                $child = Start-Child $python ('-I -X utf8 "{0}" fetch --json "{1}"' -f $script, $image) $false
                $p.Message = 'Asking the patch collection about this game ...'
            }
            'sections' {
                $child = Start-Child $python ('-I -X utf8 "{0}" sections --json "{1}" "{2}"' -f $script, $p.File, $image) $false
                $p.Message = 'Reading the patch file ...'
            }
        }
    }
    catch {
        Write-AppLog $_.Exception.ToString()
        Set-PatchError 'The patch lookup could not be started.' $_.Exception.Message
        Show-PatchState
        return
    }
    $p.Status = 'working'
    $p.Detail = ''
    $p.Run = @{ Mode = $mode; Then = $then; Child = $child; Lines = (New-Object 'Collections.Generic.List[string]'); Clock = [Diagnostics.Stopwatch]::StartNew(); ExitSeen = $null; Error = '' }
    Show-PatchState
    $patchTimer.Start()
}

function Stop-PatchRun {
    $patchTimer.Stop()
    $p = $State.Patch
    if ($p.Run) {
        Stop-Child $p.Run.Child
        $p.Run = $null
        if ($p.Status -eq 'working') { $p.Status = if ($p.Items.Count -gt 0) { 'found' } else { 'idle' }; $p.Message = '' }
    }
}

function Start-PatchLookup {
    $p = $State.Patch
    if ($p.Run) { return }
    if (-not (Get-PatchImage)) {
        Set-PatchError 'This disc image is an archive.' 'Looking for patches needs a plain .iso file. Unpack the .iso from the archive and choose that file instead.'
        Show-PatchState
        return
    }
    if (Test-AppPython) { Start-PatchRun 'fetch' } else { Start-PatchRun 'python' 'fetch' }
}

function Update-PatchRun {
    $p = $State.Patch
    $run = $p.Run
    if ($null -eq $run) { $patchTimer.Stop(); return }
    $lines = Read-ChildOutput $run.Child
    $before = $p.Message
    foreach ($line in $lines) {
        if ($run.Mode -eq 'python') {
            if ($line -match '^@app download (\d+) (\d+)$') { $p.Message = 'Downloading a small helper program (Python): {0:0} of {1:0} MB' -f ([double]$Matches[1] / 1MB), ([double]$Matches[2] / 1MB) }
            elseif ($line -match '^  (checksum ok|unpacking)') { $p.Message = 'Unpacking the helper program ...' }
            elseif ($line -match '^ERROR: (.*)$') { $run.Error = $Matches[1] }
        }
        $run.Lines.Add($line)
    }
    if ($run.Child.Process.HasExited) {
        if ($null -eq $run.ExitSeen) { $run.ExitSeen = [DateTime]::Now }
        elseif ($lines.Count -eq 0 -or ([DateTime]::Now - $run.ExitSeen).TotalSeconds -gt 3) { Complete-PatchRun; return }
    }
    elseif ($run.Clock.Elapsed.TotalSeconds -gt $(if ($run.Mode -eq 'python') { 900 } else { 120 })) {
        Stop-PatchRun
        Set-PatchError 'The lookup took too long.' 'Please check your internet connection and press the button again.'
        Show-PatchState
        return
    }
    if ($p.Message -ne $before) { Show-PatchState }
}

function Complete-PatchRun {
    $p = $State.Patch
    $run = $p.Run
    $code = $run.Child.Process.ExitCode
    Stop-Child $run.Child
    $p.Run = $null
    $patchTimer.Stop()
    if ($run.Mode -eq 'python') {
        if ($code -ne 0 -or -not (Test-AppPython)) {
            Set-PatchError 'The helper program could not be downloaded.' 'Please check your internet connection and press the button again.'
        } else {
            Start-PatchRun $run.Then
            return
        }
        Show-PatchState
        return
    }
    $answer = $null
    foreach ($line in $run.Lines) {
        if ($line.TrimStart().StartsWith('{')) { try { $answer = $line | ConvertFrom-Json } catch { } }
    }
    if ($null -eq $answer) {
        $last = @($run.Lines | Where-Object { $_.Trim() } | Select-Object -Last 2) -join ' '
        Set-PatchError 'The lookup did not work.' "The log of the lookup says: $last"
    }
    elseif ($answer.status -eq 'ok') {
        if ($run.Mode -eq 'fetch') { $p.Remembered = $false }   # a fresh answer from the collection, not the choice kept from the last build
        Set-PatchInfo $answer
    }
    elseif ($answer.status -eq 'none') {
        $p.Status = 'none'
        if ([string]$answer.message -like 'PCSX2 has patches for*') {
            $p.Message = 'No patches for this version of the game.'
            $p.Detail = [string]$answer.message
        } else {
            $p.Message = 'No patches for this game.'
            $p.Detail = 'PCSX2''s collection has no patch file for this disc. That is normal: not every game has one. Press Done and build the game as it is.'
        }
    }
    else {
        $message = [string]$answer.message
        if ($message -like 'cannot reach*') {
            Set-PatchError 'The patch collection could not be reached.' 'Is this PC online? Connect to the internet and press the button again. (The game can be built without patches.)'
        } else {
            Set-PatchError 'The patches could not be fetched.' $message
        }
    }
    Show-PatchState
}

# pnach.py said ok: show the sections of the file as tick boxes. Ticks of an earlier choice stay when the section still exists.
function Set-PatchInfo($answer) {
    $p = $State.Patch
    $p.File = [string]$answer.file
    $p.Title = ''
    $titleProp = $answer.PSObject.Properties['title']
    if ($titleProp -and $titleProp.Value) { $p.Title = ([string]$titleProp.Value) -replace '\s+\S+_[0-9A-Fa-f]{8}$', '' }
    $items = @()
    foreach ($section in @($answer.sections)) { $items += [pscustomobject]@{ Key = [string]$section.name; S = $section } }
    $p.Always = $null
    if ($items.Count -eq 0 -and $answer.always) { $items += [pscustomobject]@{ Key = $WholeFile; S = $answer.always } }
    elseif ($answer.always) { $p.Always = $answer.always }
    $p.Items = $items
    $keys = @($items | ForEach-Object { $_.Key })
    foreach ($old in @($p.Chosen)) { if ($keys -notcontains $old) { [void]$p.Chosen.Remove($old) } }
    $p.Status = 'found'
    $p.Message = ''
    $p.Detail = ''
    Build-PatchList
}

function Format-PatchProblem($problem) {
    $n = [int]$problem.count
    $text = [string]$problem.text
    switch ([string]$problem.kind) {
        'conditional' {
            if ($n -eq 1) { return '1 change depends on a condition that cannot be checked here, so it is always switched on.' }
            return "$n changes depend on conditions that cannot be checked here, so they are always switched on."
        }
        'memory' {
            if ($n -eq 1) { return "1 change points outside the PS2's memory and is skipped." }
            return "$n changes point outside the PS2's memory and are skipped."
        }
        'extended' {
            if ($n -eq 1) { return '1 special code is not supported and is skipped.' }
            return "$n special codes are not supported and are skipped."
        }
        'malformed' {
            if ($n -eq 1) { return '1 line could not be read and is skipped.' }
            return "$n lines could not be read and are skipped."
        }
    }
    if ($text -like 'IOP*') {
        if ($n -eq 1) { return "1 change is for the PS2's second processor (IOP), which is not supported." }
        return "$n changes are for the PS2's second processor (IOP), which is not supported."
    }
    if ($n -eq 1) { return "1 change is of a kind that is not supported ($text)." }
    "$n changes are of a kind that is not supported ($text)."
}

function Get-Prop($obj, [string]$name, $default = '') {
    if ($null -eq $obj) { return $default }
    $prop = $obj.PSObject.Properties[$name]
    if ($prop -and $null -ne $prop.Value) { return $prop.Value }
    $default
}

# How well one extra fits this game, from what pnach.py reports (lines, usable, problems, writes): a chip and a sentence.
function Get-PatchFit($section) {
    $lines = [int](Get-Prop $section 'lines' 0)
    $usable = [int](Get-Prop $section 'usable' $lines)
    $problems = @(Get-Prop $section 'problems' @())
    $outside = [int](Get-Prop (Get-Prop $section 'writes' $null) 'outside' 0)
    if ($lines -eq 0) { return @{ Chip = 'Empty'; Tone = 2; Text = 'This part of the file has no changes in it, so it does nothing.' } }
    $chip = 'Works here'
    $tone = 1
    $parts = @()
    if ($problems.Count -gt 0) {
        $chip = if ($usable -le 0) { 'Cannot be used' } else { 'Partly works' }
        $tone = 2
        $parts += ('{0} of its {1} changes can be used here.' -f $usable, $lines)
        foreach ($problem in $problems) { $parts += (Format-PatchProblem $problem) }
    }
    if ($outside -gt 0) {
        if ($tone -eq 1) { $chip = 'May show no effect'; $tone = 2 }
        $parts += 'Part of it changes code or data that the game loads later from the disc, so it may show no effect.'
    }
    if ($parts.Count -eq 0) { $parts += $(if ($lines -eq 1) { 'Its one change can be used here.' } else { "All $lines of its changes can be used here." }) }
    @{ Chip = $chip; Tone = $tone; Text = ($parts -join ' ') }
}

function Clear-PatchList {
    foreach ($old in @($U.patchList.Controls)) { $U.patchList.Controls.Remove($old); $old.Dispose() }
    $U.patchList.Visible = $false
}

function Set-PatchTicked([string]$key, [bool]$on) {
    $chosen = $State.Patch.Chosen
    if ($on -and -not $chosen.Contains($key)) { $chosen.Add($key) }
    elseif (-not $on) { [void]$chosen.Remove($key) }
    $U.patchSummary.Text = Get-PatchSummary
    Update-PatchFooter
}

function Measure-Height([string]$text, $font, [int]$widthPx) {
    $flags = [Windows.Forms.TextFormatFlags]::WordBreak -bor [Windows.Forms.TextFormatFlags]::NoPrefix
    [Windows.Forms.TextRenderer]::MeasureText($text, $font, (New-Object Drawing.Size(($widthPx - (S 8)), 0)), $flags).Height + (S 4)
}

function Add-WrappedLabel($parent, [string]$name, [string]$text, [int]$x, [int]$y, [int]$widthPx, $font, $color) {
    $label = New-Object Windows.Forms.Label
    $label.Name = $name
    $label.AutoSize = $false
    $label.UseMnemonic = $false
    $label.Text = $text
    $label.Font = $font
    $label.ForeColor = $color
    $label.BackColor = [Drawing.Color]::Transparent
    $label.Location = New-Object Drawing.Point($x, $y)
    $label.Size = New-Object Drawing.Size($widthPx, (Measure-Height $text $font $widthPx))
    $parent.Controls.Add($label)
    $label
}

# One card per extra: a switch with its plain title, what it does, what to watch for, how well it fits this game, and the name the
# patch has in PCSX2's collection.
function Build-PatchList {
    Clear-PatchList
    $p = $State.Patch
    if ($p.Items.Count -eq 0) { return }
    $panel = $U.patchList
    $panel.SuspendLayout()
    $outer = $panel.ClientSize.Width - [Windows.Forms.SystemInformation]::VerticalScrollBarWidth
    $cardX = S 6
    $cardW = $outer - (S 12)
    $textX = S 75
    $textW = $cardW - $textX - (S 16)
    $y = S 6
    $index = 0
    foreach ($item in $p.Items) {
        $index++
        $section = $item.S
        $kind = Get-ExtraKind $item.Key ([string](Get-Prop $section 'description' '')) ([bool](Get-Prop $section 'widescreen' $false))
        $fit = Get-PatchFit $section
        $card = New-Object Ps2Ui.Glass
        $card.Name = "PatchCard-$index"
        $card.Location = New-Object Drawing.Point($cardX, $y)
        $chip = New-Object Ps2Ui.Chip
        $chip.Name = "PatchFit-$index"
        $chip.Text = $fit.Chip
        $chip.Tone = $fit.Tone
        $chip.Font = New-Font 9 $true
        $chip.Fit()
        $chip.Location = New-Object Drawing.Point(($cardW - $chip.Width - (S 14)), (S 13))
        $box = New-Object Ps2Ui.Toggle
        $box.Name = "Patch-$index"
        $box.Text = $kind.Title
        $box.AccessibleName = $kind.Title
        $box.Tag = $item.Key
        $box.Font = New-Font 12 $true
        $box.Location = New-Object Drawing.Point((S 14), (S 8))
        $box.Size = New-Object Drawing.Size(($cardW - (S 28) - $chip.Width - (S 8)), (S 34))
        $box.Checked = $p.Chosen.Contains($item.Key)
        $box.Add_CheckedChanged({ param($sender, $e) Set-PatchTicked ([string]$sender.Tag) $sender.Checked })
        $card.Controls.Add($box)
        $card.Controls.Add($chip)
        $cy = S 46
        $does = Add-WrappedLabel $card "PatchAbout-$index" $kind.Does $textX $cy $textW (New-Font 10.5) $C.Ink
        $does.Cursor = [Windows.Forms.Cursors]::Hand
        $does.Add_Click({ param($sender, $e) $sender.Parent.Controls | Where-Object { $_ -is [Ps2Ui.Toggle] } | ForEach-Object { $_.Checked = -not $_.Checked } })
        $cy += $does.Height
        if ($kind.Careful) {
            $label = Add-WrappedLabel $card "PatchCareful-$index" ('Good to know: ' + $kind.Careful) $textX $cy $textW (New-Font 10) $C.Warn
            $cy += $label.Height
        }
        $color = if ($fit.Tone -eq 1) { $C.Good } else { $C.Warn }
        $label = Add-WrappedLabel $card "PatchNote-$index" $fit.Text $textX $cy $textW (New-Font 10) $color
        $cy += $label.Height
        if ($item.Key -ne $WholeFile) {
            $meta = "Patch name: $($item.Key)."
            $author = [string](Get-Prop $section 'author' '')
            if ($author) { $meta += " By $author." }
            $description = [string](Get-Prop $section 'description' '')
            if (@('wide', 'fps', 'skip', 'cheat') -contains $kind.Kind -and $description) { $meta += " Its note: $description" }
            $label = Add-WrappedLabel $card "PatchMeta-$index" $meta $textX $cy $textW (New-Font 9) $C.Muted
            $cy += $label.Height
        }
        $card.Size = New-Object Drawing.Size($cardW, ($cy + (S 10)))
        $panel.Controls.Add($card)
        $y += $card.Height + (S 8)
    }
    $panel.ResumeLayout()
    $panel.Visible = $true
    try { [void][Ps2App.Theme]::SetWindowTheme($panel.Handle, 'DarkMode_Explorer', $null) } catch { }
}

function Update-PatchScreen {
    $U.patchGame.Text = 'For "{0}". This step is optional: you can skip it.' -f $U.name.Text.Trim()
    Show-PatchState
    $p = $State.Patch
    if ($p.Remembered -and $p.Items.Count -eq 0 -and -not $p.Run -and (Test-AppPython) -and (Get-PatchImage)) { Start-PatchRun 'sections' }
}

# The line under the list and the label of the big button: what is switched on right now.
function Update-PatchFooter {
    $p = $State.Patch
    $keys = @(Get-ChosenKeys)
    if ($p.Status -eq 'working') {
        $U.patchNote.Text = ''
        $U.patchesDone.Text = 'Stop and go back'
        return
    }
    if ($keys.Count -gt 0) {
        $names = @($keys | ForEach-Object { (Get-ExtraKind $_ '' $false).Short })
        $text = ($names -join ', ')
        if ($text.Length -gt 70) { $text = '{0} extras' -f $keys.Count }
        $U.patchNote.Text = "Switched on: $text. They are built into the game when you press Build on the previous screen."
        $U.patchesDone.Text = $(if ($keys.Count -eq 1) { 'Use this extra' } else { "Use $($keys.Count) extras" })
    } else {
        $U.patchNote.Text = 'Nothing is switched on. The game is built as it is.'
        $U.patchesDone.Text = 'Skip extras'
    }
}

function Show-PatchState {
    $p = $State.Patch
    $working = ($p.Status -eq 'working')
    $found = ($p.Status -eq 'found' -and $p.Items.Count -gt 0)
    Set-ButtonEnabled $U.lookup (-not $working)
    $U.patchStatus.ForeColor = if ($p.Status -eq 'error') { $C.Danger } else { $C.Ink }
    $intro = "Extras are small patches from the PCSX2 community. They change how a game looks or plays, for example:`r`n    Widescreen: a 16:9 picture for a wide monitor.`r`n    60 fps: smoother movement (some games then run too fast).`r`n    Skipping videos, camera and picture fixes, cheats.`r`n`r`nHow it works: press the button, switch on the extras you want, then press Build. Extras are built into the game, so changing them later means building it again.`r`n`r`nThe button asks PCSX2's free patch collection on GitHub whether it has a file for your game. Only the game's ID number is sent; the game itself never leaves this PC. Nothing is sent before you press it."
    if (-not (Test-AppPython)) { $intro += ' The first time, a small helper program (Python, 11 MB) is downloaded too.' }
    $status = ''
    $detail = ''
    switch ($p.Status) {
        'working' { $status = $p.Message }
        'found' {
            if ($p.Items.Count -eq 0) { $status = 'The patch file has nothing to switch on.'; $intro = 'The patch collection has a file for this game, but it has no parts that can be switched on.' }
            else {
                $count = $p.Items.Count
                $what = if ($count -eq 1) { '1 extra' } else { "$count extras" }
                $for = if ($p.Title) { " for $($p.Title)" } else { ' for this game' }
                $status = if ($p.Remembered) { "Your $what from last time$for." } else { "Found $what$for." }
                $detail = 'Switch on what you want. They come from the PCSX2 community: most work here, none is guaranteed. If the game acts up, build it again without them.'
                if ($p.Always) {
                    $count = [int]$p.Always.lines
                    $detail += $(if ($count -eq 1) { ' The file also has 1 change that always applies when you use any of its extras.' } else { " The file also has $count changes that always apply when you use any of its extras." })
                }
            }
        }
        'none' { $status = $p.Message; $intro = $p.Detail }
        'error' { $status = $p.Message; $intro = $p.Detail }
    }
    $U.patchStatus.Text = $status
    $U.patchDetail.Text = $detail
    $U.patchIntro.Text = $intro
    if ($found) {
        if ($U.patchMode -ne 'list') {
            $U.patchMode = 'list'
            $U.patchCard.Height = S 96
            $U.patchIntro.Visible = $false
            $U.patchDetail.Visible = $true
            $U.patchStatus.Location = New-Point 16 8; $U.patchStatus.Size = New-Size 530 26
            $U.patchDetail.Location = New-Point 16 34; $U.patchDetail.Size = New-Size 530 56
            $U.lookup.Kind = 0; $U.lookup.Text = 'Look again'; $U.lookup.AccessibleName = 'Look again'
            $U.lookup.Location = New-Point 560 24; $U.lookup.Size = New-Size 144 46
            $U.lookup.Font = New-Font 13 $false
            $U.lookup.Invalidate()
        }
    } else {
        # The intro grows with its text; the button and the answer sit under it.
        $U.patchMode = 'intro'
        $U.patchIntro.Visible = $true
        $U.patchDetail.Visible = $false
        $U.patchIntro.Height = Measure-Height $intro $U.patchIntro.Font $U.patchIntro.Width
        $below = $U.patchIntro.Bottom + (S 8)
        $U.patchStatus.Location = New-Object Drawing.Point((S 362), $below); $U.patchStatus.Size = New-Size 342 50
        $U.lookup.Kind = 1; $U.lookup.Text = 'Look for extras for this game'; $U.lookup.AccessibleName = $U.lookup.Text
        $U.lookup.Location = New-Object Drawing.Point((S 16), $below); $U.lookup.Size = New-Size 330 46
        $U.lookup.Font = New-Font 13 $true
        $U.patchCard.Height = $below + (S 46) + (S 14)
        $U.lookup.Invalidate()
    }
    if ($found) {
        $U.patchList.Top = $U.patchCard.Bottom + (S 10)
        $U.patchList.Height = (S 496) - $U.patchList.Top
    }
    $U.patchSummary.Text = Get-PatchSummary
    Update-PatchFooter
}

$patchTimer = New-Object Windows.Forms.Timer
$patchTimer.Interval = 200
$patchTimer.Add_Tick({ try { Update-PatchRun } catch { $patchTimer.Stop(); Write-AppLog $_.Exception.ToString(); Stop-PatchRun; Set-PatchError 'The app hit a problem.' $_.Exception.Message; Show-PatchState } })

$U.lookup.Add_Click({ Start-PatchLookup })
$U.patchesDone.Add_Click({ Stop-PatchRun; Show-Screen 'choose' })

# ---------------------------------------------------------------------------------------------------------------------
# Build

$StepText = @{
    1 = @('Reading the disc image', 'Checking that the file is a PS2 disc.')
    2 = @('Finding the game''s program', 'Reading which program starts the game.')
    3 = @('Translating the game', 'The game''s code is turned into code for this PC. Big games take a while.')
    4 = @('Testing the game to find missing parts', 'The game is started without a window to find code the first pass missed. Each round can take a minute or more.')
    5 = @('Preparing the game''s vector programs', 'Almost there.')
    6 = @('Building the game', 'Putting the pieces together into a program.')
}
# Where each port.sh step sits on the bar (percent of the port stage) and how fast it creeps toward its end (seconds).
$StepRange = @{ 1 = @(0, 2, 5); 2 = @(2, 4, 5); 3 = @(4, 18, 60); 4 = @(18, 76, 240); 5 = @(76, 78, 5); 6 = @(78, 100, 60) }
# Make it faster runs port.sh with --no-explore: no testing round, so the bar and the words are different.
$FasterText = @{
    1 = @('Getting ready', 'Checking the disc image.')
    2 = @('Getting ready', 'Reading which program starts the game.')
    3 = @('Translating the game again', 'This is quick: the earlier work is reused.')
    4 = @('Translating the game again', 'This is quick: the earlier work is reused.')
    5 = @('Preparing the new vector programs', 'These are the parts of the game that you played for the first time.')
    6 = @('Building the game', 'Putting the pieces together into a program.')
}
$FasterRange = @{ 1 = @(0, 3, 3); 2 = @(3, 6, 3); 3 = @(6, 40, 10); 4 = @(40, 40, 1); 5 = @(40, 55, 4); 6 = @(55, 100, 15) }
function Get-StepText($b, [int]$step) { if ($b.Faster) { $FasterText[$step] } else { $StepText[$step] } }

function Set-Status([string]$status, [string]$detail) {
    if ($U.status.Text -ne $status) { $U.status.Text = $status }
    if ($U.detail.Text -ne $detail) { $U.detail.Text = $detail }
}

function Add-LogLine($b, [string]$line) { $b.Pending.Add($line) }

function Flush-Log($b) {
    if ($b.Pending.Count -eq 0) { return }
    $text = ($b.Pending -join "`r`n") + "`r`n"
    $b.Pending.Clear()
    try { [IO.File]::AppendAllText($b.LogFile, $text, (New-Object Text.UTF8Encoding($false))) } catch { }
    if ($U.log.TextLength -gt 200000) { $U.log.Text = $U.log.Text.Substring($U.log.TextLength - 100000) }   # the file keeps everything
    $U.log.AppendText($text)
}

# $patch is given only for Make it faster: the patch choice the game was built with (Start-Faster reads it from app.json).
# $batch is given only for Build my games again after an update: @{ Index; Total; First } (First = also prepare the translator tools).
function Start-Build([string]$name, [string]$image, [bool]$faster = $false, $patch = $null, $batch = $null) {
    if (-not $faster) { $patch = Get-BuildPatches }
    $b = @{ Name = $name; Image = $image; Plan = @(); Index = -1; Pending = (New-Object 'Collections.Generic.List[string]'); Clock = [Diagnostics.Stopwatch]::StartNew()
            LogFile = (Get-GameFile $name 'build.log'); Child = $null; Bar = 0.0; Tail = New-Object 'Collections.Generic.List[string]'; Error = ''; Failed = $false; Step = 0
            Patch = $patch; Wide = $false; Faster = $faster; Batch = $batch }
    New-Item -ItemType Directory -Force -Path (Join-Path $GamesDir $name) | Out-Null
    [IO.File]::WriteAllText($b.LogFile, '', (New-Object Text.UTF8Encoding($false)))
    $State.Build = $b
    $U.log.Clear()
    $U.buildTitle.Text = if ($batch) { "Building again ($($batch.Index) of $($batch.Total)): $name" } elseif ($faster) { "Making it faster: $name" } else { "Building: $name" }
    $U.cancel.Text = 'Cancel'
    $U.tip.Text = if ($faster) { 'This takes a short while. You can leave this window open and use your PC for other things.' } else { 'This can take a while. You can leave this window open and use your PC for other things. Please do not shut the PC down.' }
    $U.bar.Value = 0
    $U.percent.Text = '0 %'
    $U.elapsed.Text = ''
    $U.log.Visible = $false
    $U.tip.Visible = $true
    $U.details.Text = 'Show details'
    $U.bar.Visible = $true; $U.elapsed.Visible = $true; $U.percent.Visible = $true
    $U.status.ForeColor = $C.Ink
    $U.status.Height = S 58
    $U.detail.Top = S 74
    $U.detail.Height = S 44
    $U.buildCard.Height = S 206
    $U.buildExtras.Visible = $true
    $U.buildExtras.Text = Get-BuildExtrasText $b
    Set-LogPlace
    Set-Status 'Getting ready' ''
    $U.cancel.Visible = $true; $U.retry.Visible = $false; $U.buildBack.Visible = $false
    Show-Screen 'build'

    $drive = [IO.Path]::GetPathRoot($Root)
    if (-not $faster -and $drive -and -not $drive.StartsWith('\\')) {
        $free = (New-Object IO.DriveInfo($drive)).AvailableFreeSpace
        if ($free -lt 3GB) {
            Set-BuildFailed $b "There is not enough free disk space on drive $drive ($(Format-Size $free) free, about 3 GB needed). Free some space and try again."
            return
        }
    }
    $tc = Get-ToolchainState
    $b.Tools = @{ Missing = @($tc.Missing); Total = [double]$tc.Bytes; Done = 0.0; Cur = 0.0; Current = $null; Short = '' }
    if ($tc.Missing.Count -gt 0 -or -not $tc.HasEnv) { $b.Plan += @{ Id = 'tools'; Weight = 30 } }
    if ((Test-SetupNeeded) -or $b.Plan.Count -gt 0 -or ($batch -and $batch.First)) { $b.Plan += @{ Id = 'setup'; Weight = 12 } }
    $b.Plan += @{ Id = 'port'; Weight = 58 }
    $sum = 0.0
    foreach ($s in $b.Plan) { $sum += $s.Weight }
    $base = 0.0
    foreach ($s in $b.Plan) { $s.Base = $base / $sum * 100; $s.Span = $s.Weight / $sum * 100; $base += $s.Weight }

    # Make it faster keeps app.json as it is until the new program is there, so a game stays playable if it fails.
    if (-not $faster) { Write-Game @{ name = $name; source = $image; pnach = (Get-RootRelative $b.Patch.File); sections = @($b.Patch.Chosen) } }
    Start-Stage 0
    $timer.Start()
}

function Start-Stage([int]$index) {
    $b = $State.Build
    $b.Index = $index
    $stage = $b.Plan[$index]
    $b.Stage = $stage
    $b.StageClock = [Diagnostics.Stopwatch]::StartNew()
    $b.Step = 0; $b.Round = 0; $b.F = 0.0; $b.ExitSeen = $null; $b.Error = ''; $b.Phase = 0; $b.Ninja = $null; $b.NinjaAt = [DateTime]::MinValue
    $bash = Join-Path $ToolsDir 'mingit\usr\bin\bash.exe'
    switch ($stage.Id) {
        'tools' {
            Add-LogLine $b '--- Downloading the build tools ---'
            Set-Status 'Downloading build tools' "About $ToolsDownloadMb MB, once."
            $script = Join-Path $Root 'scripts\portable.ps1'
            $powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
            $b.Child = Start-Child $powershell "-NoProfile -ExecutionPolicy Bypass -File `"$script`" -ForApp" $false
        }
        'setup' {
            Add-LogLine $b '--- Preparing the libraries and translator tools ---'
            Set-Status 'Preparing the translator tools' 'Fetching the graphics and window libraries and building the translator.'
            $b.Child = Start-Child $bash ('"{0}" --native-only' -f (ConvertTo-PosixPath (Join-Path $Root 'scripts\setup.sh'))) $true
        }
        'port' {
            Add-LogLine $b $(if ($b.Faster) { '--- Making the game faster ---' } else { '--- Building the game ---' })
            Set-Status (Get-StepText $b 1)[0] 'Getting started.'
            $portArguments = '"{0}" "{1}" "{2}" --native-only' -f (ConvertTo-PosixPath (Join-Path $Root 'scripts\port.sh')), $b.Name, (ConvertTo-PosixPath $b.Image)
            # The patch file and every ticked section, exactly as docs\pnach.md describes them. ConvertTo-Arg quotes names with spaces,
            # quotes or other special characters so that they reach port.sh unchanged.
            if ($b.Patch.File) {
                $portArguments += ' --pnach ' + (ConvertTo-Arg (ConvertTo-PosixPath $b.Patch.File))
                foreach ($section in $b.Patch.Sections) { $portArguments += ' --pnach-section ' + (ConvertTo-Arg $section) }
            }
            # Make it faster: the same build without the testing rounds. Step 5 compiles every vector program that ps2run recorded in
            # games\<name>\vu1; the patch options above are the ones the game was built with, port.sh applies them again.
            if ($b.Faster) { $portArguments += ' --no-explore' }
            Add-LogLine $b ('command: bash ' + $portArguments)
            $b.Child = Start-Child $bash $portArguments $true
        }
    }
}

function Read-BuildLine($b, [string]$raw) {
    $line = $raw.TrimEnd()
    if ($line -eq '') { return }
    switch ($b.Stage.Id) {
        'tools' {
            $t = $b.Tools
            if ($line -match '^@app download (\d+) (\d+)$') { $t.Cur = [double]$Matches[1]; return }
            if ($line -match '^\[(\d+)/(\d+)\] (.+) : (.+)$' -and $line -notmatch ': already there$') {
                $title = $Matches[3]
                $t.Current = $t.Missing | Where-Object { $_.Title -eq $title } | Select-Object -First 1
                $t.Cur = 0.0
                $t.Short = ($title -replace ' \(.*$', '' -replace ':.*$', '')
                Set-Status 'Downloading build tools' $t.Short
            }
            elseif ($line -match '^  checksum ok') { Set-Status 'Unpacking build tools' $t.Short }
            elseif ($line -match '^  (unpacking|setting up)') { Set-Status 'Unpacking build tools' $t.Short }
            elseif ($line -match '^  done in ') { if ($t.Current) { $t.Done += $t.Current.Size }; $t.Cur = 0.0; $t.Current = $null }
            elseif ($line -match '^Checking that every tool starts') { Set-Status 'Checking the build tools' '' }
            elseif ($line -match '^ERROR: (.*)$') { $b.Error = $Matches[1] }
        }
        'setup' {
            if ($line -match '^\s*(%\s+Total|Dload\s|\d+\s+[\d.]+[kMGT]?\s+\d+\s)') { return }   # curl's progress meter
            if ($line -match '^== fetching wgpu') { $b.Phase = 1; Set-Status 'Downloading the graphics library' 'wgpu-native, about 15 MB.' }
            elseif ($line -match '^== fetching SDL2') { $b.Phase = 2; Set-Status 'Downloading the window library' 'SDL2, about 14 MB.' }
            elseif ($line -match '^\[(\d+)/(\d+)\]') {
                $b.Phase = 3; $b.F = [Math]::Max($b.F, 0.4 + 0.58 * [double]$Matches[1] / [double]$Matches[2])
                Set-Status 'Building the translator tools' ("Step {0} of {1}." -f $Matches[1], $Matches[2])
            }
            elseif ($line -match '^== ready') { $b.F = 1.0 }
        }
        'port' {
            if ($line -match '^== \[(\d)/6\]') {
                $b.Step = [int]$Matches[1]; $b.StepClock = [Diagnostics.Stopwatch]::StartNew()
                $text = Get-StepText $b $b.Step
                Set-Status $text[0] $text[1]
            }
            elseif ($line -match '^round (\d+):') { $b.Round = [int]$Matches[1] }
            elseif ($line -match '^Play it natively:.* --widescreen( |$)') { $b.Wide = $true }   # a ticked section is made for a 16:9 screen
        }
    }
    Add-LogLine $b $line
    $b.Tail.Add($line)
    if ($b.Tail.Count -gt 40) { $b.Tail.RemoveAt(0) }
}

# The bar: where we are on the whole plan, never going back.
function Update-Progress($b) {
    $stage = $b.Stage
    $f = 0.0
    $seconds = $b.StageClock.Elapsed.TotalSeconds
    switch ($stage.Id) {
        'tools' {
            $t = $b.Tools
            $f = if ($t.Total -gt 0) { ($t.Done + $t.Cur) / $t.Total } else { 0.3 }
            if ($t.Current) { $detail = '{0}: {1:0} of {2:0} MB' -f $t.Short, ($t.Cur / 1MB), ($t.Current.Size / 1MB); if ($U.status.Text -like 'Downloading*') { Set-Status 'Downloading build tools' $detail } }
        }
        'setup' {
            $creep = 0.4 * (1 - [Math]::Exp(-$seconds / 40))
            $f = [Math]::Max($b.F, [Math]::Max($creep, @(0, 0.05, 0.25, 0.4)[$b.Phase]))
        }
        'port' {
            if ($b.Step -ge 1) {
                $r = if ($b.Faster) { $FasterRange[$b.Step] } else { $StepRange[$b.Step] }
                $stepSeconds = $b.StepClock.Elapsed.TotalSeconds
                $g = 1 - [Math]::Exp(-$stepSeconds / $r[2])
                if ($b.Step -eq 4) { $g = [Math]::Max($g, 1 - [Math]::Pow(0.5, $b.Round)) }
                if ($b.Step -eq 4 -or $b.Step -eq 6) {
                    if (([DateTime]::Now - $b.NinjaAt).TotalSeconds -ge 1) {
                        $b.NinjaAt = [DateTime]::Now
                        $game = Get-NinjaProgress (Join-Path $Root "build\native-$($b.Name)\ninja.log")
                        $kind = 'game'
                        $runtime = @(Get-ChildItem -LiteralPath (Join-Path $Root 'build') -Directory -Filter 'runtime-native-*' -ErrorAction SilentlyContinue |
                            ForEach-Object { Get-NinjaProgress (Join-Path $_.FullName 'ninja.log') } | Where-Object { $_ })
                        if ($runtime.Count -gt 0) { $b.Ninja = $runtime[0]; $kind = 'runtime' } elseif ($game) { $b.Ninja = $game } else { $b.Ninja = $null }
                        if ($b.Ninja) { $b.Ninja.Kind = $kind }
                    }
                    if ($b.Ninja -and $b.Ninja.N -gt 0 -and $b.Ninja.M -gt 0) {
                        if ($b.Step -eq 4) { $g = [Math]::Max($g, 0.45 * $b.Ninja.N / $b.Ninja.M) }
                        if ($b.Ninja.Kind -eq 'runtime') { Set-Status 'Building the PS2 runtime' ('Compiling file {0} of {1}. This is done once and reused by every game.' -f $b.Ninja.N, $b.Ninja.M) }
                        else { Set-Status 'Compiling the translated game' ('Compiling file {0} of {1}.' -f $b.Ninja.N, $b.Ninja.M) }
                    } elseif ($b.Step -eq 4 -and $b.Round -gt 0) {
                        Set-Status $StepText[4][0] ("Round $($b.Round) is done. Each round can take a minute or more.")
                    } elseif ($U.status.Text -like 'Compiling*' -or $U.status.Text -like 'Building the PS2*') {
                        $text = Get-StepText $b $b.Step
                        Set-Status $text[0] $text[1]
                    }
                }
                $inner = $r[0] + ($r[1] - $r[0]) * [Math]::Min($g, 0.97)
                $f = $inner / 100
            }
        }
    }
    $value = $stage.Base + $stage.Span * [Math]::Min($f, 0.98)
    $b.Bar = [Math]::Max($b.Bar, $value)
    $U.bar.Value = [int][Math]::Min(1000, $b.Bar * 10)
    $U.percent.Text = '{0} %' -f [int]$b.Bar
    $U.elapsed.Text = 'Time so far: ' + (Format-Time $b.Clock.Elapsed.TotalSeconds)
}

function Update-Build {
    $b = $State.Build
    if ($null -eq $b -or $null -eq $b.Child) { return }
    $lines = Read-ChildOutput $b.Child
    foreach ($line in $lines) { Read-BuildLine $b $line }
    Flush-Log $b
    if ($b.Child.Process.HasExited) {
        if ($null -eq $b.ExitSeen) { $b.ExitSeen = [DateTime]::Now }
        elseif ($lines.Count -eq 0 -or ([DateTime]::Now - $b.ExitSeen).TotalSeconds -gt 3) { Complete-Stage $b; return }
    }
    Update-Progress $b
}

function Complete-Stage($b) {
    $code = $b.Child.Process.ExitCode
    Stop-Child $b.Child
    $b.Child = $null
    if ($code -ne 0) {
        Add-LogLine $b "[the step ended with exit code $code]"
        Flush-Log $b
        Set-BuildFailed $b (Get-FailureMessage $b)
        return
    }
    if ($b.Index + 1 -lt $b.Plan.Count) { Start-Stage ($b.Index + 1); return }
    Complete-Build $b
}

function Get-FailureMessage($b) {
    $tail = ($b.Tail -join "`n")
    switch ($b.Stage.Id) {
        'tools' {
            if ($b.Error -match 'SHA-256') { return 'A downloaded file failed its checksum test and was thrown away. Please try again. If it keeps failing, a proxy or virus scanner may be changing the download.' }
            if ($b.Error -match 'free disk space') { return 'There is not enough free disk space for the build tools. Free some space and try again.' }
            return 'The build tools could not be downloaded. Please check your internet connection and try again; the download continues where it stopped.'
        }
        'setup' { return 'The libraries or translator tools could not be prepared. If your internet connection was down, try again.' }
    }
    if ($tail -match 'not an ISO 9660 image') { return 'This file is not a PS2 disc image. It has to be an .iso made from the DVD.' }
    if ($tail -match 'no \.iso inside the archive') { return 'There is no .iso file inside that archive.' }
    if ($tail -match '7z not found') { return 'A .7z file needs the free program 7-Zip (7-zip.org). Install it, or unpack the .iso yourself.' }
    if ($tail -match 'BOOT2') { return 'This disc has no PS2 game program that the app can start. Is it a PS2 game disc?' }
    switch ($b.Step) {
        1 { return 'The disc image could not be read. Is it a complete .iso file made from a PS2 DVD?' }
        2 { return 'The disc has no program the app can start. Is it a PS2 game disc?' }
        3 { return 'The game''s program could not be translated. It may not be supported yet, or the disc image is damaged.' }
        4 { return 'The game could not be tested. It may not be supported yet.' }
        5 { return 'The game''s vector programs could not be prepared. It may not be supported yet.' }
        6 { return 'The game could not be built. It may not be supported yet.' }
    }
    'The build stopped before it really started. The log has the details.'
}

function Set-BuildFailed($b, [string]$message) {
    $timer.Stop()
    if ($b.Child) { Stop-Child $b.Child; $b.Child = $null }
    # Build my games again: one game that does not build does not stop the others (a failure of the tools does).
    if ($b.Batch -and $b.ContainsKey('Stage') -and $b.Stage.Id -eq 'port') {
        $b.Failed = $true
        Flush-Log $b
        Complete-Rebuild $b 'failed' $message
        return
    }
    $State.Build = $b
    $b.Failed = $true
    $U.buildTitle.Text = 'The build stopped'
    $U.status.ForeColor = $C.Danger
    $U.status.Text = $message
    $U.status.Height = S 84
    $U.detail.Top = S 100
    $U.detail.Height = S 110
    $U.buildCard.Height = S 232
    $U.buildExtras.Visible = $false
    Set-LogPlace
    $extra = if ($b.Error) { "$($b.Error)`n" } else { '' }
    $advice = if ($b.Batch) { 'Press Try again to go on with your games, or Back.' } else { 'Press Try again, or Back to choose another disc image or other extras.' }
    $U.detail.Text = "${extra}${advice}`nThe full log is saved here (Show details has it too):`n$($b.LogFile)"
    $U.bar.Visible = $false; $U.elapsed.Visible = $false; $U.percent.Visible = $false; $U.tip.Visible = $false
    $U.cancel.Visible = $false; $U.retry.Visible = $true; $U.buildBack.Visible = $true
    Flush-Log $b
    Show-Screen 'build'
}

function Complete-Build($b) {
    $timer.Stop()
    $name = $b.Name
    $exe = Join-Path $Root "build\native-$name\ps2run.exe"
    if (-not (Test-Path -LiteralPath $exe)) {
        Set-BuildFailed $b 'The build finished, but the game program was not made. The log has the details.'
        return
    }
    # The same program and disc as port.sh's closing "Play it natively" line.
    $program = ''
    $cnf = Get-GameFile $name 'disc\SYSTEM.CNF'
    if (Test-Path -LiteralPath $cnf) {
        foreach ($line in Get-Content -LiteralPath $cnf) { if ($line -match '^\s*BOOT2\s*=\s*.*\\([^\\;]+);') { $program = $Matches[1]; break } }
    }
    $elf = Get-GameFile $name 'core.elf'
    if (-not (Test-Path -LiteralPath $elf)) { $elf = Get-GameFile $name "disc\$program" }
    $iso = Get-GameFile $name 'disc.iso'
    if (-not (Test-Path -LiteralPath $iso)) { $iso = $b.Image }   # port.sh could not link it: it plays from where it was
    if (-not $program -or -not (Test-Path -LiteralPath $elf)) {
        Set-BuildFailed $b 'The build finished, but the game''s start file was not found. The log has the details.'
        return
    }
    # With patches port.sh translated a patched copy of the program (games\<name>\patched) and wrote the data patches the runner
    # applies while the game runs to games\<name>\patches.txt; the game has to be started with both (see Get-PlayCommand).
    $patchList = ''
    if ($b.Patch.File) {
        $patched = Get-GameFile $name ('patched\' + [IO.Path]::GetFileName($elf))
        $patchList = Get-GameFile $name 'patches.txt'
        if (-not (Test-Path -LiteralPath $patched) -or -not (Test-Path -LiteralPath $patchList)) {
            Set-BuildFailed $b 'The build finished, but the patched game program was not found. The log has the details.'
            return
        }
        $elf = $patched
    }
    # Make it faster may have been given the game's own disc.iso, not the image the player chose: app.json keeps the image they chose.
    $source = $b.Image
    $took = [string][int]$b.Clock.Elapsed.TotalSeconds
    if ($b.Faster) { $old = Read-Game $name; if ($old -and $old.source) { $source = $old.source }; if ($old) { $took = $old.took } }
    Write-Game @{ name = $name; source = $source; built = (Get-Date).ToString('s'); took = $took; elf = (Get-RootRelative $elf); iso = (Get-RootRelative $iso)
                  pnach = (Get-RootRelative $b.Patch.File); sections = @($b.Patch.Chosen); patches = (Get-RootRelative $patchList); widescreen = $(if ($b.Wide) { '1' } else { '' }) }
    if ($b.Batch) { Complete-Rebuild $b 'done' ''; return }
    $State.Game = $name
    try { [Media.SystemSounds]::Asterisk.Play() } catch { }
    $lead = if ($b.Faster) { "$name is faster now." } else { "$name is built." }
    Show-Ready $name "$lead Play opens the game in its own window; close that window to stop it."
}

# The Done screen of a built game, the screen with Play: after a build, after Make it faster, and back from a Make it faster that
# was cancelled or failed.
function Show-Ready([string]$name, [string]$text) {
    $State.Game = $name
    $game = Read-Game $name
    $U.doneName.Text = $name
    $U.doneText.Text = $text
    $U.doneResult.Text = ''
    foreach ($old in @($U.doneCard.Controls | Where-Object { $_.Name -like 'doneChip-*' -or $_.Name -eq 'doneNoExtras' })) { $U.doneCard.Controls.Remove($old); $old.Dispose() }
    $labels = @(Get-ExtraLabels $game)
    $took = if ($game) { Format-Took $game.took } else { '' }
    $again = if ($took) { " (the last build took $took)" } else { '' }
    if ($labels.Count -gt 0) {
        Add-ExtraChips $U.doneCard $labels (S 140) 99 700 'doneChip'
        $U.doneHint.Text = "To change the extras, the game is built again$again. Press Change extras."
    } else {
        [void](Add-Label $U.doneCard 'doneNoExtras' 'none (built as it is)' 140 98 400 26 (New-Font 10.5) $C.Muted 'MiddleLeft')
        $U.doneHint.Text = "For widescreen or 60 fps, press Change extras. The game is then built again$again."
    }
    Show-Screen 'done'
}

$timer = New-Object Windows.Forms.Timer
$timer.Interval = 250
$timer.Add_Tick({ try { Update-Build } catch { $timer.Stop(); Write-AppLog $_.Exception.ToString(); if ($State.Build) { Set-BuildFailed $State.Build "The app hit a problem: $($_.Exception.Message)" } } })

$U.build.Add_Click({
    $name = $U.name.Text.Trim()
    if ($State.Image -and -not (Get-NameProblem $name $State.Image)) {
        if (@(Get-ChosenKeys).Count -gt 0 -and -not (Test-Path -LiteralPath $State.Patch.File)) {
            [void][Windows.Forms.MessageBox]::Show('The patch file chosen for this game is not there any more. Open Patches and look for it again, or build the game without patches.', $AppTitle, 'OK', 'Information')
            Reset-Patches
            Update-ChooseState
            return
        }
        $State.Name = $name
        Start-Build $name $State.Image
    }
})
# The log sits under the card, wherever the card ends (it is taller when a build has failed).
function Set-LogPlace {
    $U.log.Top = $U.buildCard.Bottom + (S 12)
    $U.log.Height = (S 498) - $U.log.Top
    $U.tip.Top = $U.buildCard.Bottom + (S 14)
}

function Get-BuildExtrasText($b) {
    $keys = @($b.Patch.Chosen)
    if ($keys.Count -eq 0) { return 'Extras: none. The game is built as it is.' }
    $names = @($keys | ForEach-Object { (Get-ExtraKind $_ '' $false).Short })
    'Extras built in: ' + ($names -join ', ')
}

$U.details.Add_Click({
    $show = -not $U.log.Visible
    $U.log.Visible = $show
    $U.tip.Visible = (-not $show) -and -not ($State.Build -and $State.Build.Failed)
    $U.details.Text = if ($show) { 'Hide details' } else { 'Show details' }
    if ($show) { $U.log.SelectionStart = $U.log.TextLength; $U.log.ScrollToCaret() }
})
$U.cancel.Add_Click({
    $answer = [Windows.Forms.MessageBox]::Show('Stop the build? What was already done is kept, so the next build continues from there.', $AppTitle, 'YesNo', 'Question')
    if ($answer -eq 'Yes' -and $State.Build) {
        $timer.Stop()
        Stop-Child $State.Build.Child
        $faster = $State.Build.Faster
        $name = $State.Build.Name
        $wasBatch = [bool]$State.Build.Batch
        $State.Build = $null
        if ($wasBatch) { $State.Batch = $null; Show-Screen 'welcome' }
        elseif ($faster) { Show-Ready $name "$name is as it was before. Play opens the game in its own window; close that window to stop it." }
        else { Show-Screen 'choose' }
    }
})
$U.retry.Add_Click({
    if ($State.Build -and $State.Build.Batch) { Start-RebuildAll }
    elseif ($State.Build -and $State.Build.Faster) { Start-Faster $State.Build.Name }
    elseif ($State.Name -and $State.Image) { Start-Build $State.Name $State.Image }
})
$U.buildBack.Add_Click({
    $b = $State.Build
    $State.Build = $null
    if ($b -and $b.Batch) { $State.Batch = $null; Show-Screen 'welcome' }
    elseif ($b -and $b.Faster) { Show-Ready $b.Name "$($b.Name) is as it was before. Play opens the game in its own window; close that window to stop it." }
    else { Show-Screen 'choose' }
})

# ---------------------------------------------------------------------------------------------------------------------
# Make it faster: compile the vector programs that were recorded while the game was played. The same port.sh as the build, with
# --no-explore (about 10 seconds for God of War II instead of minutes) and the patch options the game was built with, which
# app.json keeps. Shown on the Done screen and under the Play buttons of the welcome screen; the build screen is the usual one.

# What a rebuild of an already built game needs, or why it cannot start: @{ Error = <plain sentence> } or @{ Image; Patch }.
function Get-FasterPlan([string]$name) {
    $game = Read-Game $name
    $exe = Join-Path $Root "build\native-$name\ps2run.exe"
    if (-not $game -or -not $game.built -or -not (Test-Path -LiteralPath $exe)) {
        return @{ Error = "The game '$name' is not built yet (or its files are gone). Please build it again." }
    }
    if (Test-GameRunning $exe) {
        return @{ Error = "The game '$name' is still open. Close its window first, then try again." }
    }
    # port.sh needs the disc image only when games\<name>\disc.iso is gone; offer it the place the build used, else where the game plays from.
    $image = $game.source
    if (-not $image -or -not (Test-Path -LiteralPath $image)) { $image = Resolve-AppPath $game.iso }
    if (-not (Test-Path -LiteralPath $image)) {
        return @{ Error = "The disc image of '$name' is not at its old place any more:`n$image`nPut it back, or build the game again." }
    }
    $patch = @{ File = ''; Sections = @(); Chosen = @() }
    if ($game.pnach) {
        $file = Resolve-AppPath $game.pnach
        if (-not (Test-Path -LiteralPath $file)) {
            return @{ Error = "The patch file the game was built with is not there any more:`n$file`nBuild the game again (Patches, then Build), so that it keeps its patches." }
        }
        $patch = @{ File = $file; Sections = @($game.sections | Where-Object { $_ -ne $WholeFile }); Chosen = @($game.sections) }
    }
    @{ Image = $image; Patch = $patch }
}

function Start-Faster([string]$name) {
    $plan = Get-FasterPlan $name
    if ($plan.ContainsKey('Error')) {
        [void][Windows.Forms.MessageBox]::Show($plan.Error, $AppTitle, 'OK', 'Information')
        return
    }
    Start-Build $name $plan.Image $true $plan.Patch
}

# ---------------------------------------------------------------------------------------------------------------------
# Update, and Build my games again

# The notice on the welcome screen, in the place of the three cards. Priority: games waiting to be built again (right after an
# update), else a newer version. Only texts and visibility change, so a click that is just landing is never lost.
function Update-Banner {
    $mode = ''
    $pending = @(Get-PendingRebuild)
    if (-not $InstallProblem) {
        if ($pending.Count -gt 0) { $mode = 'rebuild' }
        elseif ($State.Newest -and $State.Newest -ne (Get-InstalledVersion)) { $mode = 'update' }
    }
    if ($mode -eq '' -and (Test-Path -LiteralPath (Join-Path $UpdateDir 'rebuild.txt')) -and -not $State.Batch) { Set-PendingRebuild @() }
    $U.noteCard.Visible = [bool]$mode
    foreach ($card in $U.stepCards) { $card.Visible = -not $mode }
    if ($mode -ne $U.noteMode) {
        $U.noteMode = $mode
        switch ($mode) {
            'update' {
                $U.noteTitle.Text = 'A newer version is out'
                $U.noteText.Text = 'The update brings the newest fixes. Your games, saves, quick saves and the downloaded build tools all stay. Afterwards the app starts again and offers to build your games again.'
                $U.noteSmall.Text = "Version $($State.Newest.Substring(0, 7)). About 1 MB from GitHub; nothing is uploaded."
                $U.noteButton.Text = 'Update'
                $U.noteButton.Font = New-Font 18 $true
                $U.noteLater.Visible = $false
            }
            'rebuild' {
                $U.noteTitle.Text = 'The app was updated'
                $U.noteText.Text = 'To get the fixes into your games, they are built once more. Extras, saves, quick saves and your progress stay.'
                $U.noteButton.Font = New-Font 13.5 $true
                $U.noteLater.Visible = $true
            }
        }
        $U.noteButton.AccessibleName = $U.noteButton.Text
    }
    if ($mode -eq 'rebuild') {
        $n = $pending.Count
        $U.noteButton.Text = 'Build my games again'
        $U.noteButton.AccessibleName = $U.noteButton.Text
        $U.noteSmall.Text = $(if ($n -eq 1) { '1 game is waiting. If its disc image is gone, it is skipped.' } else { "$n games are waiting. A game with no disc image is skipped." })
    }
}

# --- Looking for a newer version (once, when the app starts; quietly)
$updTimer = New-Object Windows.Forms.Timer
$updTimer.Interval = 250

function Close-UpdateJob {
    $job = $State.UpdateJob
    $State.UpdateJob = $null
    if ($job) { Stop-UpdateWorker $job }
}

function Update-UpdateJob {
    $job = $State.UpdateJob
    if ($null -eq $job) { $updTimer.Stop(); return }
    $sh = $job.Sh
    if ($job.Mode -eq 'download') {
        if ($sh.Phase -eq 'download' -and $sh.Total -gt 0) {
            $U.updBar.Value = [int][Math]::Min(1000, $sh.Done / $sh.Total * 1000)
        }
        if ($sh.Phase -eq 'check' -and $U.updStatus.Text -ne 'Checking and unpacking the download') {
            $U.updStatus.Text = 'Checking and unpacking the download'
            $U.updBar.Value = 1000
            $U.updCancel.Visible = $false
        }
    }
    if ($sh.Result -eq '') {
        # The worker ended without an answer: treat it as a failure.
        if ($job.Handle.IsCompleted) { $sh.Result = 'failed'; $sh.Error = 'the worker ended' } else { return }
    }
    $result = [string]$sh.Result
    $error_ = [string]$sh.Error
    Close-UpdateJob
    $updTimer.Stop()
    if ($job.Mode -eq 'check') {
        if ($result -eq 'newer') { $State.Newest = [string]$sh.Sha; if ($P['welcome'].Visible) { Update-Banner } }
        return
    }
    switch ($result) {
        'ready' {
            try {
                Install-Update ([string]$sh.Sha)
            } catch {
                Write-AppLog $_.Exception.ToString()
                Show-UpdateFailed 'The update could not be put in place, so nothing was changed.' "A file may be in use. Close other windows that show this folder, then try again.`n$($_.Exception.Message)"
                return
            }
            Restart-App
        }
        'cancelled' { Show-Screen 'welcome' }
        'invalid' {
            Write-AppLog "Update refused: $error_"
            Show-UpdateFailed 'The downloaded file is not a version of this app, so it was not used.' 'Nothing was changed. If this happens again, download the zip from the project page by hand.'
        }
        'unpack' {
            Write-AppLog "Update could not be unpacked: $error_"
            Show-UpdateFailed 'The new version was downloaded, but it could not be prepared.' "Nothing was changed. Close other windows that show this folder, then try again.`n$error_"
        }
        default {
            Write-AppLog "Update download failed: $error_"
            Show-UpdateFailed 'The new version could not be downloaded.' 'Nothing was changed. Please check your internet connection and try again.'
        }
    }
}
$updTimer.Add_Tick({ try { Update-UpdateJob } catch { $updTimer.Stop(); Write-AppLog $_.Exception.ToString(); Close-UpdateJob; Show-UpdateFailed 'The update hit a problem, so nothing was changed.' $_.Exception.Message } })

function Start-UpdateCheck {
    if (-not (Test-UpdateAllowed) -or $InstallProblem) { return }
    try { $State.UpdateJob = Start-UpdateWorker 'check' ''; $updTimer.Start() } catch { Write-AppLog $_.Exception.ToString() }
}

# --- The update itself
function Show-UpdateScreen([string]$title, [string]$status, [string]$detail) {
    $U.updTitle.Text = $title
    $U.updStatus.Text = $status
    $U.updStatus.ForeColor = $C.Ink
    $U.updDetail.Text = $detail
    $U.updList.Text = ''
    $U.updBar.Value = 0
    $U.updBar.Visible = $true
    $U.updCard.Height = S 170
    $U.updCancel.Visible = $false; $U.updRetry.Visible = $false; $U.updBack.Visible = $false; $U.updDone.Visible = $false
    Show-Screen 'update'
}

function Start-UpdateDownload {
    if (-not $State.Newest) { return }
    Close-UpdateJob
    Show-UpdateScreen 'Updating the app' 'Downloading the newest version' 'Only the app''s own files are replaced. Your games, saves, quick saves and the build tools stay where they are.'
    $U.updCancel.Visible = $true
    try {
        $State.UpdateJob = Start-UpdateWorker 'download' $State.Newest
        $updTimer.Start()
    } catch {
        Write-AppLog $_.Exception.ToString()
        Show-UpdateFailed 'The update could not be started, so nothing was changed.' $_.Exception.Message
    }
}

function Show-UpdateFailed([string]$message, [string]$detail) {
    $updTimer.Stop()
    Show-UpdateScreen 'The update stopped' $message $detail
    $U.updStatus.ForeColor = $C.Danger
    $U.updBar.Visible = $false
    $U.updRetry.Visible = $true; $U.updBack.Visible = $true
}

# The new files are in place: start the app again, so the new code runs, and close this window. The mutex goes first, so the
# new window is not turned away as "already open".
function Restart-App {
    $State.Restarting = $true
    try { $Mutex.ReleaseMutex(); $Mutex.Dispose() } catch { }
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = Join-Path $env:SystemRoot 'System32\cmd.exe'
    $psi.Arguments = '/c ""' + (Join-Path $Root 'Start.cmd') + '""'
    $psi.WorkingDirectory = $Root
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    [void][Diagnostics.Process]::Start($psi)
    $form.Close()
}

$U.noteButton.Add_Click({
    if ($U.noteMode -eq 'update') { Start-UpdateDownload }
    elseif ($U.noteMode -eq 'rebuild') { Start-RebuildAll }
})
$U.noteLater.Add_Click({
    $answer = [Windows.Forms.MessageBox]::Show('Your games keep working as they are; only the new fixes are missing in them. Skip building them again?', $AppTitle, 'YesNo', 'Question')
    if ($answer -eq 'Yes') { Set-PendingRebuild @(); Update-Banner }
})
$U.updCancel.Add_Click({ Close-UpdateJob; $updTimer.Stop(); Show-Screen 'welcome' })
$U.updRetry.Add_Click({ if ($State.Newest) { Start-UpdateDownload } else { Show-Screen 'welcome' } })
$U.updBack.Add_Click({ Show-Screen 'welcome' })
$U.updDone.Add_Click({ Show-Screen 'welcome' })

# --- Build my games again: every game, one after the other, with the usual build screen. The same as Make it faster for each
# game (port.sh --no-explore keeps its extras, recorded vector programs and saves), and the translator tools are prepared once
# first, because the new version may have changed them. rebuild.txt is shortened after each game, so a stopped run goes on
# with the rest.
function Start-RebuildAll {
    $names = @(Get-PendingRebuild)
    if ($names.Count -eq 0) { Set-PendingRebuild @(); Show-Screen 'welcome'; return }
    $open = @($names | Where-Object { Test-GameRunning (Join-Path $Root "build\native-$_\ps2run.exe") })
    if ($open.Count -gt 0) {
        [void][Windows.Forms.MessageBox]::Show("A game is still open: $($open -join ', ').`nClose its window first, then press the button again.", $AppTitle, 'OK', 'Information')
        return
    }
    $State.Batch = @{ Queue = (New-Object 'Collections.Generic.List[string]'); Total = $names.Count; Count = 0; First = $true; Results = (New-Object 'Collections.Generic.List[object]') }
    foreach ($n in $names) { $State.Batch.Queue.Add($n) }
    Start-NextRebuild
}

function Start-NextRebuild {
    $batch = $State.Batch
    while ($batch.Queue.Count -gt 0) {
        $name = $batch.Queue[0]
        $batch.Queue.RemoveAt(0)
        $batch.Count++
        $plan = Get-FasterPlan $name
        if ($plan.ContainsKey('Error')) {
            $batch.Results.Add(@{ Name = $name; Status = 'skipped'; Text = $plan.Error })
            Set-PendingRebuild @(Get-PendingRebuild | Where-Object { $_ -ne $name })
            continue
        }
        $info = @{ Index = $batch.Count; Total = $batch.Total; First = $batch.First }
        $batch.First = $false
        Start-Build $name $plan.Image $true $plan.Patch $info
        return
    }
    Show-RebuildResult
}

function Complete-Rebuild($b, [string]$status, [string]$text) {
    $batch = $State.Batch
    $batch.Results.Add(@{ Name = $b.Name; Status = $status; Text = $text })
    Set-PendingRebuild @(Get-PendingRebuild | Where-Object { $_ -ne $b.Name })
    $State.Build = $null
    Start-NextRebuild
}

function Show-RebuildResult {
    $batch = $State.Batch
    $State.Batch = $null
    $results = $batch.Results.ToArray()   # not @(...): PowerShell 5.1 cannot turn a List of hashtables into an array that way
    $ok = @($results | Where-Object { $_.Status -eq 'done' }).Count
    $bad = $results.Count - $ok
    $title = if ($bad -eq 0) { 'Your games are up to date' } else { 'Your games were built again' }
    $status = if ($ok -eq 1) { '1 game was built again.' } else { "$ok games were built again." }
    $detail = if ($bad -eq 0) { 'Play opens each game as before. Its saves, quick saves and extras are still there.' } else { 'Some games were left as they were. You can see why below. They still work as before.' }
    Show-UpdateScreen $title $status $detail
    $U.updBar.Visible = $false
    $U.updCard.Height = S 130
    $U.updList.Top = $U.updCard.Bottom + (S 16)
    $lines = New-Object 'Collections.Generic.List[string]'
    foreach ($r in $results | Select-Object -First 8) {
        $one = ([string]$r.Text -replace '\s+', ' ').Trim()
        if ($one.Length -gt 150) { $one = $one.Substring(0, 147) + '...' }
        $word = switch ($r.Status) { 'done' { 'built again' } 'skipped' { 'skipped' } default { 'did not build' } }
        $lines.Add($(if ($one) { "$($r.Name): $word. $one" } else { "$($r.Name): $word." }))
    }
    if ($results.Count -gt 8) { $lines.Add("... and $($results.Count - 8) more.") }
    $U.updList.Text = ($lines -join "`n")
    $U.updDone.Visible = $true
    try { [Media.SystemSounds]::Asterisk.Play() } catch { }
}

# ---------------------------------------------------------------------------------------------------------------------
# Done

$U.play.Add_Click({
    if (Start-Game $State.Game) { $U.doneResult.Text = 'The game was started. If no window shows up, give it a few seconds.' }
})
$U.faster.Add_Click({ if ($State.Game) { Start-Faster $State.Game } })
$U.changeExtras.Add_Click({ if ($State.Game) { Open-ExtrasFor $State.Game } })
$U.shortcut.Add_Click({
    $play = Get-PlayCommand $State.Game
    if (-not $play) { return }
    $desktop = [Environment]::GetFolderPath('Desktop')
    $path = Join-Path $desktop "$($State.Game).lnk"
    try {
        New-Shortcut $path $play.Exe $play.Arguments $play.Dir $play.Exe "Play $($State.Game)"
        $U.doneResult.ForeColor = $C.Muted
        $U.doneResult.Text = "The shortcut `"$($State.Game)`" is on your desktop."
    } catch {
        Write-AppLog $_.Exception.ToString()
        $U.doneResult.ForeColor = $C.Danger
        $U.doneResult.Text = "The shortcut could not be made: $($_.Exception.Message)"
    }
})
$U.another.Add_Click({
    Reset-Patches; $State.Image = ''
    $U.fileName.Text = 'No disc image chosen yet.'; $U.fileName.ForeColor = $C.Muted
    $U.filePath.Text = 'An .iso made from a PS2 DVD you own, or a .zip or .7z file that holds one.'
    $U.imageError.Text = ''; $U.name.Text = ''
    $U.chooseSub.Text = 'Pick your disc, choose extras if you like, then press Build.'
    Show-Screen 'choose'
})
$U.close.Add_Click({ $form.Close() })

$form.Add_FormClosing({
    param($sender, $e)
    if ($State.Patch.Run) { Stop-PatchRun }
    if ($State.UpdateJob -and $State.UpdateJob.Mode -eq 'download' -and -not $State.Restarting) { Close-UpdateJob }
    if ($State.Build -and $State.Build.Child) {
        $answer = [Windows.Forms.MessageBox]::Show('A build is still running. Stop it and close the app?', $AppTitle, 'YesNo', 'Question')
        if ($answer -ne 'Yes') { $e.Cancel = $true; return }
        $timer.Stop()
        Stop-Child $State.Build.Child
    }
})
# Esc goes back one screen (a CancelButton would close the whole window, so it is done by hand).
$form.KeyPreview = $true
$form.Add_KeyDown({
    param($sender, $e)
    if ($e.KeyCode -ne 'Escape') { return }
    if ($P['choose'].Visible) { $e.Handled = $true; $U.chooseBack.PerformClick() }
    elseif ($P['patches'].Visible) { $e.Handled = $true; $U.patchesDone.PerformClick() }
})
$form.Add_Shown({ $form.Activate() })
# The game window was in front while it recorded vector programs: look again when this window comes back.
$form.Add_Activated({ try { Update-FasterOffers } catch { Write-AppLog $_.Exception.ToString() } })

# Leftovers of an update that was stopped half way (nothing of them is used), then the quiet look for a newer version.
foreach ($leftover in 'new', 'old') { try { Remove-Item -LiteralPath (Join-Path $UpdateDir $leftover) -Recurse -Force -ErrorAction Stop } catch { } }
try { Get-ChildItem -LiteralPath $UpdateDir -File -ErrorAction Stop | Where-Object { $_.Name -like 'download*' } | Remove-Item -Force } catch { }
Start-UpdateCheck
Show-Screen 'welcome'
[void]$form.ShowDialog()
Close-UpdateJob
$timer.Dispose()

}
catch {
    Show-Fatal $_.Exception.ToString()
    exit 1
}
