# Verify the managed self-extractor without running install.cmd or changing shortcuts.
param(
    [Parameter(Mandatory = $true)] [string]$Installer,
    [Parameter(Mandatory = $true)] [string]$BuildOut,
    [Parameter(Mandatory = $true)] [string]$WorkDirectory,
    [string]$Publisher = 'Bit Precise, LLC'
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
function Get-Publisher($Signature) {
    $Signature.SignerCertificate.GetNameInfo(
        [System.Security.Cryptography.X509Certificates.X509NameType]::SimpleName, $false)
}
try {
    $Installer = (Get-Item -LiteralPath $Installer).FullName
    $sig = Get-AuthenticodeSignature -LiteralPath $Installer
    if ($sig.Status -ne 'Valid' -or -not $sig.TimeStamperCertificate) {
        throw 'Installer signature must be valid and timestamped'
    }
    if ((Get-Publisher $sig) -ne $Publisher) { throw 'Wrong installer publisher' }
    $expected = (Get-Content -LiteralPath ($Installer + '.sha256') -Raw).Trim().Split()[0]
    $actual = (Get-FileHash -LiteralPath $Installer -Algorithm SHA256).Hash
    if ($expected -ne $actual) { throw 'Installer checksum mismatch' }
    if (Test-Path -LiteralPath $WorkDirectory) { throw "Directory already exists: $WorkDirectory" }
    New-Item -ItemType Directory -Path $WorkDirectory | Out-Null
    $assembly = [Reflection.Assembly]::LoadFile($Installer)
    $resources = @($assembly.GetManifestResourceNames() | Where-Object { $_.EndsWith('ByodPayload.zip') })
    if ($resources.Count -ne 1) { throw 'Expected one embedded runtime ZIP in managed installer' }
    $zipPath = Join-Path $WorkDirectory 'payload.zip'
    $inputStream = $assembly.GetManifestResourceStream($resources[0])
    $outputStream = [IO.File]::Create($zipPath)
    try { $inputStream.CopyTo($outputStream) } finally { $outputStream.Dispose(); $inputStream.Dispose() }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $runtime = Join-Path $WorkDirectory 'runtime'
    [IO.Compression.ZipFile]::ExtractToDirectory($zipPath, $runtime)
    $manifest = Get-Content -LiteralPath (Join-Path $runtime 'byod-build-manifest.json') -Raw | ConvertFrom-Json
    $sourceManifest = Join-Path $BuildOut 'byod-build-manifest.json'
    if ((Get-FileHash -LiteralPath $sourceManifest -Algorithm SHA256).Hash -ne $manifest.signing.source_manifest_sha256) {
        throw 'Canonical build manifest changed'
    }
    foreach ($required in @('chrome.exe','chrome.dll','resources.pak','icudtl.dat','v8_context_snapshot.bin')) {
        if (-not $manifest.files.PSObject.Properties[$required] -or -not $manifest.build_files.PSObject.Properties[$required]) {
            throw "Missing required manifest entry: $required"
        }
    }
    $checked = 0
    foreach ($property in $manifest.files.PSObject.Properties) {
        $path = Join-Path $runtime $property.Name
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $property.Value.sha256) {
            throw "Payload hash mismatch: $($property.Name)"
        }
        $checked++
    }
    foreach ($property in $manifest.build_files.PSObject.Properties) {
        $path = Join-Path $BuildOut $property.Name
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $property.Value.sha256) {
            throw "Canonical build file changed: $($property.Name)"
        }
    }
    $own = 0
    $vendor = 0
    $binaries = @(Get-ChildItem -LiteralPath $runtime -Recurse -File |
        Where-Object { $_.Extension.ToLowerInvariant() -in @('.exe', '.dll') })
    foreach ($file in $binaries) {
        $s = Get-AuthenticodeSignature -LiteralPath $file.FullName
        if ($s.Status -ne 'Valid') { throw "Invalid payload signature: $($file.Name): $($s.Status)" }
        if ((Get-Publisher $s) -eq $Publisher) {
            if (-not $s.TimeStamperCertificate) { throw "Missing timestamp: $($file.Name)" }
            $own++
        } else {
            $original = Join-Path $BuildOut $file.FullName.Substring($runtime.Length + 1)
            if ((Get-FileHash -LiteralPath $original -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash) {
                throw "Third-party file changed: $($file.Name)"
            }
            $vendor++
        }
    }
    foreach ($name in @('chrome.exe','chrome.dll','chrome_elf.dll','chrome_wer.dll')) {
        $s = Get-AuthenticodeSignature -LiteralPath (Join-Path $runtime $name)
        if ((Get-Publisher $s) -ne $Publisher) { throw "Wrong publisher on core binary: $name" }
    }
    $report = [PSCustomObject]@{
        Result = 'PASS'; Installer = $Installer; Publisher = $Publisher
        SHA256 = $actual.ToLowerInvariant(); VerifiedFileHashes = $checked
        OwnSignedBinaries = $own; PreservedVendorBinaries = $vendor
        CanonicalBuildUnchanged = $true; ExtractedRuntime = $runtime
    } | ConvertTo-Json
    $report | Set-Content -LiteralPath (Join-Path $WorkDirectory 'verification.json') -Encoding UTF8
    Write-Output $report
    exit 0
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
}
