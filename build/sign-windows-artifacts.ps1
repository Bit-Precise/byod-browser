param(
    [Parameter(Mandatory = $true)] [string]$Path,
    [Parameter(Mandatory = $true)] [string]$Metadata,
    [Parameter(Mandatory = $true)] [string]$SignTool,
    [Parameter(Mandatory = $true)] [string]$Dlib,
    [Parameter(Mandatory = $true)] [string]$Publisher
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
# WAM-backed interactive logins cannot refresh tokens inside Windows SSH sessions.
# This only changes the child process environment, not the user's Azure config.
$env:AZURE_CORE_ENABLE_BROKER_ON_WINDOWS = 'false'

try {
    foreach ($inputPath in @($Metadata, $SignTool, $Dlib)) {
        if (-not (Test-Path -LiteralPath $inputPath -PathType Leaf)) {
            throw "Missing signing input: $inputPath"
        }
    }
    if (Test-Path -LiteralPath $Path -PathType Container) {
        $files = @(Get-ChildItem -LiteralPath $Path -Recurse -File |
            Where-Object { $_.Extension.ToLowerInvariant() -in @('.exe', '.dll') } |
            Sort-Object FullName)
    } else {
        $files = @(Get-Item -LiteralPath $Path)
    }
    if ($files.Count -eq 0) { throw "No signing targets found: $Path" }

    $signed = 0
    $preserved = 0
    foreach ($file in $files) {
        $existing = Get-AuthenticodeSignature -LiteralPath $file.FullName
        if ($existing.Status -eq 'Valid') {
            # Do not overwrite Microsoft's (or another vendor's) valid signature.
            Write-Output "PRESERVED: $($file.Name) [$($existing.SignerCertificate.Subject)]"
            $preserved++
            continue
        }
        if ($existing.Status -ne 'NotSigned') {
            throw "Refusing to replace an invalid signature: $($file.FullName): $($existing.Status)"
        }
        Write-Output "SIGNING: $($file.FullName)"
        & $SignTool sign /v /fd SHA256 /tr 'http://timestamp.acs.microsoft.com' /td SHA256 `
            /d 'BYOD Browser' /dlib $Dlib /dmdf $Metadata $file.FullName
        if ($LASTEXITCODE -ne 0) { throw "SignTool sign failed ($LASTEXITCODE): $($file.FullName)" }
        & $SignTool verify /pa /all /v /tw $file.FullName
        if ($LASTEXITCODE -ne 0) { throw "SignTool verify failed ($LASTEXITCODE): $($file.FullName)" }
        $signature = Get-AuthenticodeSignature -LiteralPath $file.FullName
        if ($signature.Status -ne 'Valid' -or -not $signature.TimeStamperCertificate) {
            throw "A valid timestamped signature is required: $($file.FullName)"
        }
        $actualPublisher = $signature.SignerCertificate.GetNameInfo(
            [System.Security.Cryptography.X509Certificates.X509NameType]::SimpleName, $false)
        if ($actualPublisher -ne $Publisher) {
            throw "Wrong publisher on $($file.FullName): $actualPublisher (expected $Publisher)"
        }
        $signed++
    }
    Write-Output "SIGNING COMPLETE: signed=$signed preserved=$preserved"
    exit 0
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
}
