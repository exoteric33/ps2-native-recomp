# Tests the app's update comparison (app\app.ps1, Compare-TreeToFiles) without the window.
# Run: powershell -NoProfile -File tests\app-update-check.ps1 -Root C:\PS2Native [-Scratch wt\app-crlf-test]
# -Root is an unpacked GitHub ZIP of the newest public version. Needs the network (3 GitHub API calls at most).
param([Parameter(Mandatory)][string]$Root, [string]$Scratch = (Join-Path $env:TEMP 'app-crlf-test'))
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$app = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\app\app.ps1') -Raw
# Pull the helper functions out of the app (they sit at 4-space indent and end with a line '    }').
foreach ($name in 'Get-BlobSha', 'ConvertTo-Lf', 'Compare-TreeToFiles') {
    $m = [regex]::Match($app, "(?ms)^    function $name\b.*?^    }\r?$")
    if (-not $m.Success) { throw "function $name not found in app.ps1" }
    . ([scriptblock]::Create($m.Value))
}
$api = 'https://api.github.com/repos/exoteric33/ps2-native-recomp'
$h = @{ 'User-Agent' = 'ps2-native-recomp-app' }
$sha = (Invoke-RestMethod "$api/commits/main" -Headers $h).sha
$tree = Invoke-RestMethod "$api/git/trees/${sha}?recursive=1" -Headers $h
"newest commit $sha"
$fail = 0
function Check($label, $r, $expect) {
    $got = if ($r.Same -and $r.Count -gt 20) { 'current' } else { 'newer' }
    $ok = $got -eq $expect
    if (-not $ok) { $script:fail++ }
    "{0,-34} -> {1} (expected {2}) {3}" -f $label, $got, $expect, $(if ($ok) { 'OK' } else { 'FAIL' })
}
# Old behaviour: raw hash only (what the app did before the fix).
$old = @{ Same = $true; Count = 0 }
foreach ($e in $tree.tree) {
    if ($e.type -ne 'blob' -or $e.path -like '*.md' -or $e.path -like '.github/*') { continue }
    $old.Count++
    $f = Join-Path $Root $e.path.Replace('/', '\')
    if (-not (Test-Path -LiteralPath $f -PathType Leaf) -or (Get-BlobSha ([IO.File]::ReadAllBytes($f))) -ne $e.sha) { $old.Same = $false; break }
}
Check 'unpacked ZIP, raw hash only (old)' $old 'newer'
Check 'unpacked ZIP, with CRLF fix' (Compare-TreeToFiles $tree $Root) 'current'

# Copies with one changed byte: a .ps1 and a .cpp file.
if (Test-Path $Scratch) { Remove-Item -Recurse -Force $Scratch }
try {
    foreach ($case in @(@{ N = 'ps1'; Pat = 'app\app.ps1' }, @{ N = 'cpp'; Pat = 'runtime\src\disc.cpp' })) {
        $copy = Join-Path $Scratch $case.N
        New-Item -ItemType Directory -Force -Path $copy | Out-Null
        & robocopy $Root $copy /E /NFL /NDL /NJH /NJS /NP /XD .git | Out-Null
        $file = Join-Path $copy $case.Pat
        $b = [IO.File]::ReadAllBytes($file)
        $i = [int]($b.Length / 2)
        $b[$i] = if ($b[$i] -eq 65) { 66 } else { 65 }   # one letter changed in the middle
        [IO.File]::WriteAllBytes($file, $b)
        Check "one byte changed in $($case.Pat)" (Compare-TreeToFiles $tree $copy) 'newer'
    }
} finally { if (Test-Path $Scratch) { Remove-Item -Recurse -Force $Scratch } }
if ($fail) { "FAILED: $fail"; exit 1 } else { 'ALL OK' }
