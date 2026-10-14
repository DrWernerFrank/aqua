# Builds and installs the Windows Copilot-key provider package for aqua.
# Uses only on-box tools: the .NET Framework compiler (csc), System.Drawing
# for icons, and the Windows SDK's makeappx + signtool. One UAC prompt
# appears when trusting the self-signed certificate.
param(
    [string]$AquaExe  = "C:\Users\Hp\AppData\Local\Programs\aqua\aqua.exe",
    [string]$MusicDir = "C:\Users\Hp\Music",
    [string]$IconPath = ""
)

$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$out  = Join-Path $here "build"

# --- locate tools ---
$sdk = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\bin" -Directory |
    Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty FullName
$makeappx = Join-Path $sdk "x64\makeappx.exe"
$signtool = Join-Path $sdk "x64\signtool.exe"
$csc = "C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
foreach ($t in @($makeappx, $signtool, $csc)) {
    if (-not (Test-Path $t)) { throw "required tool not found: $t (install the Windows SDK)" }
}
if (-not (Test-Path $AquaExe))  { throw "aqua.exe not found: $AquaExe (pass -AquaExe)" }
if ($IconPath -eq "") { $IconPath = Join-Path (Split-Path -Parent $AquaExe) "aqua.ico" }

# --- stage package contents ---
$pkg = Join-Path $out "pkg"
Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path "$pkg\Assets", "$pkg\Public" | Out-Null

# substitute the install paths into the launcher source
$src = Get-Content (Join-Path $here "launcher.cs") -Raw
$src = $src.Replace("%%AQUA_EXE%%", $AquaExe).Replace("%%AQUA_DIR%%", $MusicDir)
Set-Content -Path "$out\launcher.cs" -Value $src -Encoding UTF8

& $csc /nologo /target:winexe /platform:anycpu /out:"$pkg\AquaKeyLauncher.exe" "$out\launcher.cs"
if ($LASTEXITCODE -ne 0) { throw "compile failed" }

# tile icons from aqua.ico; plain dark tiles if there is no icon
Add-Type -AssemblyName System.Drawing
function Make-Png([int]$s, [string]$p) {
    $bmp = New-Object System.Drawing.Bitmap($s, $s)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    if (Test-Path $IconPath) {
        $icon = [System.Drawing.Image]::FromFile($IconPath)
        $g.InterpolationMode = "HighQualityBicubic"
        $g.DrawImage($icon, 0, 0, $s, $s)
        $icon.Dispose()
    } else {
        $g.Clear([System.Drawing.Color]::FromArgb(16, 20, 24))
    }
    $bmp.Save($p, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
}
Make-Png 44  "$pkg\Assets\Square44x44Logo.png"
Make-Png 50  "$pkg\Assets\StoreLogo.png"
Make-Png 150 "$pkg\Assets\Square150x150Logo.png"
Set-Content "$pkg\Public\readme.txt" "aqua Copilot key provider package"
Copy-Item (Join-Path $here "AppxManifest.xml") "$pkg\AppxManifest.xml"

# --- pack ---
$msix = Join-Path $out "aqua.msix"
& $makeappx pack /d $pkg /p $msix /o
if ($LASTEXITCODE -ne 0) { throw "makeappx failed" }

# --- sign; reuse an existing unexpired cert for this subject if present ---
# The subject must match the manifest's Publisher (CN=aqua-player).
$subject = "CN=aqua-player"
$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object {
    $_.Subject -eq $subject -and $_.NotAfter -gt (Get-Date)
} | Select-Object -First 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate -Type Custom -Subject $subject `
        -KeyUsage DigitalSignature -FriendlyName "aqua player signing" `
        -CertStoreLocation "Cert:\CurrentUser\My" `
        -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}")
}
Export-Certificate -Cert $cert -FilePath (Join-Path $out "aqua.cer") | Out-Null
& $signtool sign /fd SHA256 /sha1 $cert.Thumbprint $msix
if ($LASTEXITCODE -ne 0) { throw "signtool failed" }

# --- trust the cert (one UAC prompt), then install ---
Start-Process powershell.exe -Verb RunAs -Wait -ArgumentList '-NoProfile','-Command',"Import-Certificate -FilePath '$out\aqua.cer' -CertStoreLocation Cert:\LocalMachine\TrustedPeople | Out-Null"
Add-AppxPackage -Path $msix

""
"installed - now pick it up in Windows Settings:"
"  Settings > Personalization > Text input > Customize Copilot key > Custom > aqua"
"(or Settings > Bluetooth & devices > Keyboard on some builds)"
