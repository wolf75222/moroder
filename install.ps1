# Moroder installer / updater for Windows.
#
#   irm https://raw.githubusercontent.com/wolf75222/moroder/main/install.ps1 | iex
#
# - Downloads the latest release's Windows build (checksum verified): moroder.exe
#   with libmpv and everything it needs, into %LOCALAPPDATA%\Programs\moroder
# - Adds yt-dlp.exe next to it when yt-dlp isn't installed
# - Puts the folder on your user PATH. Run it again to update.
#
# $env:MORODER_VERSION = "1.4.0"   installs a given version
# $env:MORODER_PREFIX  = "D:\apps\moroder"   installs elsewhere

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"     # Invoke-WebRequest is many times faster without it
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch {}

$Repo = "wolf75222/moroder"
$Api  = "https://api.github.com/repos/$Repo"

function Ok($label, $detail)   { Write-Host "  " -NoNewline; Write-Host "✓" -ForegroundColor Green -NoNewline; Write-Host ("  {0,-14} " -f $label) -NoNewline; Write-Host $detail -ForegroundColor DarkGray }
function Step($label, $detail) { Write-Host "  " -NoNewline; Write-Host "·" -ForegroundColor Yellow -NoNewline; Write-Host ("  {0,-14} " -f $label) -NoNewline; Write-Host $detail -ForegroundColor DarkGray }
function Die($message)         { Write-Host ""; Write-Host "  ✗ " -ForegroundColor Red -NoNewline; Write-Host $message; Write-Host ""; exit 1 }

$Start = Get-Date

# Which version: the one asked for, or the latest release.
$Version = $env:MORODER_VERSION
if (-not $Version) {
    try { $Version = (Invoke-RestMethod "$Api/releases/latest" -Headers @{ "User-Agent" = "moroder-installer" }).tag_name }
    catch { Die "couldn't find the latest release of $Repo (offline?)" }
}
$Version = $Version.TrimStart("v")

$Arch = if ([Environment]::Is64BitOperatingSystem) { "x86_64" } else { Die "Moroder needs 64-bit Windows" }
$Prefix = if ($env:MORODER_PREFIX) { $env:MORODER_PREFIX } else { Join-Path $env:LOCALAPPDATA "Programs\moroder" }
$Asset = "moroder-$Version-windows-$Arch"
$Url = "https://github.com/$Repo/releases/download/v$Version/$Asset.zip"

Write-Host ""
Write-Host "  ▶ Moroder $Version" -ForegroundColor Red -NoNewline
Write-Host "  Windows $Arch" -ForegroundColor DarkGray
Write-Host ""
Write-Host "  Installing into" -ForegroundColor DarkGray
Write-Host "    program   $Prefix\moroder.exe"
Write-Host "    settings  ~\.config\moroder  (created by moroder setup)"
Write-Host ""

$Tmp = Join-Path ([IO.Path]::GetTempPath()) ("moroder-" + [Guid]::NewGuid())
New-Item -ItemType Directory -Path $Tmp | Out-Null
try {
    # Download and check.
    Step "Download" "$Asset.zip"
    try {
        Invoke-WebRequest $Url -OutFile "$Tmp\$Asset.zip" -UseBasicParsing
        Invoke-WebRequest "$Url.sha256" -OutFile "$Tmp\$Asset.zip.sha256" -UseBasicParsing
    } catch { Die "no Windows build for $Version ($Url)" }
    $Expected = ((Get-Content "$Tmp\$Asset.zip.sha256" -Raw).Trim() -split "\s+")[0].ToLower()
    $Actual = (Get-FileHash "$Tmp\$Asset.zip" -Algorithm SHA256).Hash.ToLower()
    if ($Expected -ne $Actual) { Die "checksum mismatch, nothing installed" }
    $Size = "{0:N1} MB" -f ((Get-Item "$Tmp\$Asset.zip").Length / 1MB)
    Ok "Download" "$Size, checksum ok"

    # Install: a running moroder.exe can't be overwritten, so it goes first.
    Get-Process moroder -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$Prefix*" } | Stop-Process -Force -ErrorAction SilentlyContinue
    Expand-Archive "$Tmp\$Asset.zip" -DestinationPath $Tmp -Force
    New-Item -ItemType Directory -Path $Prefix -Force | Out-Null
    $Keep = Join-Path $Prefix "yt-dlp.exe"
    Get-ChildItem $Prefix | Where-Object { $_.FullName -ne $Keep } | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
    Copy-Item "$Tmp\$Asset\*" $Prefix -Recurse -Force
    Ok "Install" "$Prefix\moroder.exe"

    # yt-dlp: the one already installed, else its standalone .exe next to moroder.
    $YtDlp = Get-Command yt-dlp -ErrorAction SilentlyContinue
    if ($YtDlp -and $YtDlp.Source -notlike "$Prefix*") {
        if ($env:MORODER_UPDATING) { try { & yt-dlp -U *> $null } catch {} }
        Ok "yt-dlp" ((& yt-dlp --version) + " (" + $YtDlp.Source + ")")
    } elseif (Test-Path $Keep) {
        try { & $Keep -U *> $null } catch {}
        Ok "yt-dlp" (& $Keep --version)
    } else {
        Step "yt-dlp" "downloading"
        Invoke-WebRequest "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe" -OutFile $Keep -UseBasicParsing
        Ok "yt-dlp" (& $Keep --version)
    }

    # On the user PATH (new terminals), and in this one.
    $UserPath = [Environment]::GetEnvironmentVariable("Path", "User")
    if (-not (($UserPath -split ";") -contains $Prefix)) {
        [Environment]::SetEnvironmentVariable("Path", (($UserPath.TrimEnd(";") + ";" + $Prefix).TrimStart(";")), "User")
        Ok "PATH" "added (open a new terminal)"
    } else {
        Ok "PATH" "already on it"
    }
    if (-not (($env:Path -split ";") -contains $Prefix)) { $env:Path = "$env:Path;$Prefix" }
}
finally {
    Remove-Item $Tmp -Recurse -Force -ErrorAction SilentlyContinue
}

$Took = [int]((Get-Date) - $Start).TotalSeconds
Write-Host ""
Write-Host "  ✓ " -ForegroundColor Green -NoNewline
Write-Host "Moroder $Version is installed" -NoNewline
Write-Host " ($Took s)" -ForegroundColor DarkGray
if (Test-Path (Join-Path $HOME ".config\moroder\browser.json")) {
    Write-Host "    Run moroder"
} else {
    Write-Host "    Next: moroder setup to sign in once, then moroder"
    Write-Host "    No account? moroder -a" -ForegroundColor DarkGray
}
Write-Host ""
