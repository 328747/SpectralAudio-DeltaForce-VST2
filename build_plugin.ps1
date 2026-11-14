# Build the original neural model + independent VST2 wrapper locally.
# No Windows APO registration changes, administrative rights, or downloaded
# precompiled DLL are used by this build script.
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$src = Join-Path $PSScriptRoot 'spectral-audio'
$build = Join-Path $PSScriptRoot 'build-win'
$dist = Join-Path $PSScriptRoot 'dist'

function Fail([string]$message) { throw $message }
$cmake = Get-Command cmake.exe -ErrorAction SilentlyContinue
if (-not $cmake) { Fail 'CMake not found. Install CMake (add to PATH), then run BUILD_PLUGIN.cmd again.' }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { Fail 'Visual Studio 2022 C++ Build Tools not found. Install Desktop development with C++.' }
$vsPath = & $vswhere -latest -products '*' -requires 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64' -property installationPath
$vsVersion = & $vswhere -latest -products '*' -requires 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64' -property installationVersion
if (-not $vsPath) { Fail 'MSVC x64 toolchain missing. Install Desktop development with C++ workload.' }
$major = [int]($vsVersion -split '\.')[0]
$generator = switch ($major) {
  17 { 'Visual Studio 17 2022' }
  18 { 'Visual Studio 18 2026' }
  default { Fail "Unsupported Visual Studio major version: $major" }
}

if (-not (Test-Path (Join-Path $src 'generated\model_data.cpp'))) {
  Write-Host '[1/4] Fetching original Spectral Audio source and embedded Delta Force model...'
  $git = Get-Command git.exe -ErrorAction SilentlyContinue
  if ($git) {
    & git clone --depth 1 https://github.com/white-give-man/spectral-audio.git "$src"
    if ($LASTEXITCODE -ne 0) { Fail 'Git clone failed. Check network access to GitHub.' }
  } else {
    $zip = Join-Path $env:TEMP 'spectral-audio-main.zip'
    $tempExtract = Join-Path $env:TEMP ('spectral-audio-extract-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $tempExtract | Out-Null
    try {
      Invoke-WebRequest -Uri 'https://github.com/white-give-man/spectral-audio/archive/refs/heads/main.zip' -OutFile $zip -UseBasicParsing
      Expand-Archive -LiteralPath $zip -DestinationPath $tempExtract -Force
      Move-Item (Join-Path $tempExtract 'spectral-audio-main') $src
    } finally {
      Remove-Item $tempExtract -Recurse -Force -ErrorAction SilentlyContinue
      Remove-Item $zip -Force -ErrorAction SilentlyContinue
    }
  }
}
if (-not (Test-Path (Join-Path $src 'generated\model_data.cpp'))) { Fail 'Upstream model_data.cpp is missing; cannot build actual AI model.' }
Write-Host "[2/4] Configuring $generator x64..."
& cmake -S $PSScriptRoot -B $build -G $generator -A x64 -DSPECTRAL_MOCK=OFF -DBUILD_TESTING=ON -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
if ($LASTEXITCODE -ne 0) { Fail 'CMake configuration failed.' }
Write-Host '[3/4] Building real Spectral Audio VST2 DLL (Release)...'
& cmake --build $build --config Release --target SpectralAudio_DeltaForce vst_win_smoke --parallel 4
if ($LASTEXITCODE -ne 0) { Fail 'MSVC build failed.' }
Write-Host '[3a/4] Running real-model VST host smoke test...'
& ctest --test-dir $build --config Release --output-on-failure
if ($LASTEXITCODE -ne 0) { Fail 'Real-model VST host smoke test failed. DLL will not be packaged.' }
$dll = Join-Path $build 'Release\SpectralAudio_DeltaForce.dll'
if (-not (Test-Path $dll)) { Fail 'The compiler did not produce SpectralAudio_DeltaForce.dll.' }
New-Item -ItemType Directory -Force -Path $dist | Out-Null
Copy-Item $dll (Join-Path $dist 'SpectralAudio_DeltaForce.dll') -Force
Write-Host '[4/4] Inspecting PE architecture and VST2 entrypoints...'
# PE: e_lfanew at 0x3c; Machine 0x8664 means AMD64.
$stream = [System.IO.File]::OpenRead((Join-Path $dist 'SpectralAudio_DeltaForce.dll'))
try {
  $reader = New-Object System.IO.BinaryReader($stream)
  $stream.Position = 0x3c
  $peOffset = $reader.ReadInt32()
  $stream.Position = $peOffset
  $signature = $reader.ReadUInt32()
  $machine = $reader.ReadUInt16()
  if ($signature -ne 0x00004550 -or $machine -ne 0x8664) { Fail 'Output is not an x64 Windows DLL.' }
} finally { $stream.Dispose() }
Write-Host ('BUILD OK: ' + (Join-Path $dist 'SpectralAudio_DeltaForce.dll')) -ForegroundColor Green
Write-Host 'Import this DLL into Equalizer APO Configuration Editor > VST plugin.'
Write-Host 'Do not install the upstream Spectral Audio APO on the same audio endpoint.'
