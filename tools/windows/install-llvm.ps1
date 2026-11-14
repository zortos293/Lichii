# SPDX-License-Identifier: GPL-3.0-or-later

# Installs the LLVM release that the Windows CI jobs compile with (clang-cl, lld-link, llvm-lib)
# and exports its location as LLVM_ROOT for later steps. The installer is pinned by checksum so a
# runner image update never changes the compiler.

$ErrorActionPreference = 'Stop'

$version = '22.1.8'
$sha256 = '16e5709785fef73c854646241c4a92c5cd574318d1b33c63330dd7721903e55c'

$installer = Join-Path $env:RUNNER_TEMP 'llvm.exe'
$url = "https://github.com/llvm/llvm-project/releases/download/llvmorg-$version/LLVM-$version-win64.exe"
# Authenticated download: GitHub release assets throttle unauthenticated clients.
$headers = @{ }
if ($env:GITHUB_TOKEN) { $headers.Authorization = "token $env:GITHUB_TOKEN" }
for ($attempt = 1; $attempt -le 5; $attempt++) {
    try {
        Invoke-WebRequest -Uri $url -OutFile $installer -TimeoutSec 300 -Headers $headers
        break
    } catch {
        Remove-Item -LiteralPath $installer -Force -ErrorAction SilentlyContinue
        if ($attempt -eq 5) { throw }
        $delay = [Math]::Min(5 * $attempt, 30)
        Write-Warning "LLVM download failed (attempt $attempt/5): $($_.Exception.Message). Retrying in $delay seconds."
        Start-Sleep -Seconds $delay
    }
}
if ((Get-FileHash -Algorithm SHA256 $installer).Hash -ne $sha256) {
    throw 'LLVM installer checksum mismatch'
}
$root = Join-Path $env:RUNNER_TEMP 'llvm'
Start-Process -Wait -FilePath $installer -ArgumentList '/S', "/D=$root"
$reported = & "$root/bin/clang-cl.exe" --version
$reported
if (-not ($reported -match "clang version $([regex]::Escape($version))")) {
    throw "Unexpected clang-cl: $reported"
}
"LLVM_ROOT=$($root.Replace('\', '/'))" | Out-File -Append -Encoding utf8 $env:GITHUB_ENV
