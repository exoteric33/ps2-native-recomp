# Tests how the app (app\app.ps1, Get-ChdInfo) judges a .chd file, without the window and without a real disc: made-up 124-byte headers.
# Run: powershell -NoProfile -File tests\app-chd-check.ps1   (prints "app chd: 0 failed")
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2
$app = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\app\app.ps1') -Raw
$line = [regex]::Match($app, '(?m)^\$ChdCodecs = .*$')
if (-not $line.Success) { throw 'the codec list is not in app.ps1' }
Invoke-Expression $line.Value
$fn = [regex]::Match($app, '(?ms)^function Get-ChdInfo\b.*?^}\r?$')
if (-not $fn.Success) { throw 'Get-ChdInfo not found in app.ps1' }
. ([scriptblock]::Create($fn.Value))

$dir = Join-Path ([IO.Path]::GetTempPath()) ('app-chd-check-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $dir | Out-Null
$failed = 0
function Check([string]$label, [bool]$ok) {
    if (-not $ok) { $script:failed++; "FAIL: $label" }
}
function New-Header([int]$version = 5, [string[]]$codecs = @('lzma', 'zlib', 'huff', 'flac'), [uint64]$logical = 8533671936, [int]$parentAt = -1) {
    $b = New-Object byte[] 124
    [Text.Encoding]::ASCII.GetBytes('MComprHD').CopyTo($b, 0)
    $b[11] = 124
    $b[15] = [byte]$version
    for ($i = 0; $i -lt 4; $i++) {
        if ($i -lt $codecs.Count) { [Text.Encoding]::ASCII.GetBytes($codecs[$i]).CopyTo($b, 16 + 4 * $i) }
    }
    $v = $logical
    for ($i = 7; $i -ge 0; $i--) { $b[32 + $i] = [byte]($v % 256); $v = [math]::Floor($v / 256) }
    if ($parentAt -ge 0) { $b[$parentAt] = 7 }
    , $b
}
function Write-Chd([string]$name, [byte[]]$bytes) {
    $p = Join-Path $dir $name
    [IO.File]::WriteAllBytes($p, $bytes)
    $p
}
try {
    $r = Get-ChdInfo (Write-Chd 'good.chd' (New-Header))
    Check 'a DVD file with the default codecs is accepted' ($r.Error -eq '')
    Check 'its unpacked size is read (8533671936 bytes, the size of a double layer DVD image)' ($r.Size -eq 8533671936)
    foreach ($set in @('zstd', 'zlib', 'lzma', 'huff', 'flac'), @('cdzs', 'cdlz', 'cdzl', 'cdfl')) {
        Check ('codecs ' + ($set -join ',') + ' are accepted') ((Get-ChdInfo (Write-Chd 'c.chd' (New-Header 5 $set))).Error -eq '')
    }
    Check 'no compression is accepted' ((Get-ChdInfo (Write-Chd 'none.chd' (New-Header 5 @()))).Error -eq '')
    $r = Get-ChdInfo (Write-Chd 'v4.chd' (New-Header 4))
    Check 'version 4 is refused and the words name the version' ($r.Error -match 'old version \(4\)')
    $r = Get-ChdInfo (Write-Chd 'parent.chd' (New-Header 5 @('lzma') 100 110))
    Check 'a file that needs a parent is refused' ($r.Error -match 'parent')
    $r = Get-ChdInfo (Write-Chd 'av.chd' (New-Header 5 @('avhu')))
    Check 'a codec the app cannot read is refused and named' ($r.Error -match 'avhu')
    $r = Get-ChdInfo (Write-Chd 'short.chd' ([byte[]](1..50)))
    Check 'a short file is refused' ($r.Error -match 'does not look like')
    $r = Get-ChdInfo (Write-Chd 'text.chd' ([Text.Encoding]::ASCII.GetBytes('hello' * 40)))
    Check 'a text file called .chd is refused' ($r.Error -match 'does not look like')
    $r = Get-ChdInfo (Join-Path $dir 'missing.chd')
    Check 'a missing file is refused' ($r.Error -ne '')
    Check 'every refusal has a Size too (the caller reads both)' ($r.Size -eq 0)
} finally {
    Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue
}
"app chd: $failed failed"
exit $(if ($failed) { 1 } else { 0 })
