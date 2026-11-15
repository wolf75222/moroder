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

# Run through iex, a script shares the session of the shell that called it:
# in a block of its own, its settings ($ErrorActionPreference...) and
# variables don't stay behind there, and a failure doesn't close that window.
& {
    $ErrorActionPreference = "Stop"
    $ProgressPreference = "SilentlyContinue"     # Invoke-WebRequest is many times faster without it
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch {}

    # Symbols by code: the file stays ASCII, since Windows PowerShell 5.1 reads a
    # .ps1 without a BOM in the ANSI code page, where a UTF-8 check mark ends a string.
    $Tick = [char]0x2713; $Cross = [char]0x2717; $Dot = [char]0x00B7; $Play = [char]0x25B6

    $Repo = "wolf75222/moroder"
    $Api  = "https://api.github.com/repos/$Repo"

    function Ok($label, $detail)   { Write-Host "  " -NoNewline; Write-Host $Tick -ForegroundColor Green -NoNewline; Write-Host ("  {0,-14} " -f $label) -NoNewline; Write-Host $detail -ForegroundColor DarkGray }
    function Step($label, $detail) { Write-Host "  " -NoNewline; Write-Host $Dot -ForegroundColor Yellow -NoNewline; Write-Host ("  {0,-14} " -f $label) -NoNewline; Write-Host $detail -ForegroundColor DarkGray }
    function Die($message)         { throw [OperationCanceledException]::new($message) }

    try {
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
        Write-Host "  $Play Moroder $Version" -ForegroundColor Red -NoNewline
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

            # Install. A running moroder.exe (the one asking for this update, say) and
            # its DLLs can't be overwritten or deleted, but they can be moved: the old
            # files go to a side folder, removed on the next install once they're free.
            Expand-Archive "$Tmp\$Asset.zip" -DestinationPath $Tmp -Force
            New-Item -ItemType Directory -Path $Prefix -Force | Out-Null
            Get-ChildItem $Prefix -Directory -Filter ".old-*" | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
            $Keep = Join-Path $Prefix "yt-dlp.exe"
            $KeepDeno = Join-Path $Prefix "deno.exe"
            $Old = Join-Path $Prefix (".old-" + [Guid]::NewGuid().ToString("N").Substring(0, 8))
            $Current = Get-ChildItem $Prefix | Where-Object { $_.FullName -ne $Keep -and $_.FullName -ne $KeepDeno -and $_.Name -notlike ".old-*" }
            if ($Current) {
                New-Item -ItemType Directory -Path $Old | Out-Null
                $Current | Move-Item -Destination $Old -Force
            }
            Copy-Item "$Tmp\$Asset\*" $Prefix -Recurse -Force
            if (Test-Path $Old) { Remove-Item $Old -Recurse -Force -ErrorAction SilentlyContinue }   # free already, unless in use
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

            # Deno: yt-dlp solves YouTube's challenges with it (the only JavaScript
            # runtime it uses by default: node / bun must be turned on by hand), and
            # without it streams don't resolve.
            $Js = @("deno") | Where-Object { Get-Command $_ -ErrorAction SilentlyContinue } | Select-Object -First 1
            if ($Js -and -not (Test-Path $KeepDeno)) {
                Ok "JS runtime" "deno (already installed)"
            } elseif (Test-Path $KeepDeno) {
                Ok "JS runtime" ("deno " + ((& $KeepDeno --version) -split "\s+")[1])
            } else {
                Step "JS runtime" "downloading Deno (yt-dlp needs it for YouTube)"
                Invoke-WebRequest "https://github.com/denoland/deno/releases/latest/download/deno-x86_64-pc-windows-msvc.zip" -OutFile "$Tmp\deno.zip" -UseBasicParsing
                Expand-Archive "$Tmp\deno.zip" -DestinationPath "$Tmp\deno" -Force
                Copy-Item "$Tmp\deno\deno.exe" $KeepDeno -Force
                Ok "JS runtime" ("deno " + ((& $KeepDeno --version) -split "\s+")[1])
            }

            # Icons: CaskaydiaMono Nerd Font (Cascadia with the icons) for this user,
            # and a "Moroder" profile in Windows Terminal that uses it. The profile is
            # a settings fragment of its own: your settings.json isn't touched.
            $Fonts = Join-Path $Prefix "fonts"
            if (Test-Path $Fonts) {
                $UserFonts = Join-Path $env:LOCALAPPDATA "Microsoft\Windows\Fonts"
                New-Item -ItemType Directory -Path $UserFonts -Force | Out-Null
                $Reg = "HKCU:\Software\Microsoft\Windows NT\CurrentVersion\Fonts"
                if (-not (Test-Path $Reg)) { New-Item -Path $Reg -Force | Out-Null }
                foreach ($ttf in Get-ChildItem $Fonts -Filter *.ttf) {
                    $Target = Join-Path $UserFonts $ttf.Name
                    if (-not (Test-Path $Target)) { Copy-Item $ttf.FullName $Target -Force }
                    $Style = if ($ttf.Name -like "*Bold*") { "Bold" } else { "Regular" }
                    New-ItemProperty -Path $Reg -Name "CaskaydiaMono Nerd Font Mono $Style (TrueType)" -Value $Target -PropertyType String -Force | Out-Null
                }
                $Fragment = Join-Path $env:LOCALAPPDATA "Microsoft\Windows Terminal\Fragments\Moroder"
                New-Item -ItemType Directory -Path $Fragment -Force | Out-Null
                $Profile = @{
                    profiles = @(@{
                        guid = "{6f1b2d6e-3a4c-4f0e-9b7a-6d0f2c1e9a51}"
                        name = "Moroder"
                        commandline = "cmd.exe /c set MORODER_ICONS=nerd&& `"$Prefix\moroder.exe`""
                        startingDirectory = "%USERPROFILE%"
                        font = @{ face = "CaskaydiaMono Nerd Font Mono" }
                        closeOnExit = "graceful"
                    })
                }
                $Profile | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $Fragment "moroder.json") -Encoding UTF8
                # moroder.exe started in a console window of its own (where the
                # old console is the default terminal): the window's settings key
                # is the program's path, "\" as "_". Moroder sees the Nerd Font
                # there and shows its icons; other console windows are untouched.
                $ConsoleKey = "HKCU:\Console\" + ((Join-Path $Prefix "moroder.exe") -replace "\\", "_")
                if (-not (Test-Path $ConsoleKey)) { New-Item -Path $ConsoleKey -Force | Out-Null }
                New-ItemProperty -Path $ConsoleKey -Name "FaceName" -Value "CaskaydiaMono Nerd Font Mono" -PropertyType String -Force | Out-Null
                New-ItemProperty -Path $ConsoleKey -Name "FontFamily" -Value 54 -PropertyType DWord -Force | Out-Null
                New-ItemProperty -Path $ConsoleKey -Name "FontWeight" -Value 400 -PropertyType DWord -Force | Out-Null
                Ok "Icons" "CaskaydiaMono Nerd Font + a Moroder profile in Windows Terminal"
            }

            # On the user PATH (new terminals), and in this one.
            $UserPath = [Environment]::GetEnvironmentVariable("Path", "User")
            if (-not (($UserPath -split ";") -contains $Prefix)) {
                [Environment]::SetEnvironmentVariable("Path", (($UserPath.TrimEnd(";") + ";" + $Prefix).TrimStart(";")), "User")
                Ok "PATH" "added"
            } else {
                Ok "PATH" "already on it"
            }
            # Terminals already open (bash, zsh, cmd, another PowerShell) keep their
            # old PATH. A tiny launcher in a folder they already have on it
            # (WindowsApps, on every user's PATH) makes moroder work there right now.
            # Moroder deletes it once new terminals see the real PATH.
            $Launcher = $false
            $ShimDir = Join-Path $env:LOCALAPPDATA "Microsoft\WindowsApps"
            $Seen = $env:Path -split ";" | ForEach-Object { $_.TrimEnd("\") }
            if ((Test-Path $ShimDir) -and ($Seen -contains $ShimDir.TrimEnd("\")) -and -not ($Seen -contains $Prefix.TrimEnd("\"))) {
                try {
                    $Exe = Join-Path $Prefix "moroder.exe"
                    $NoBom = New-Object System.Text.UTF8Encoding $false
                    # cmd and PowerShell. (goto) leaves the batch first: no "Terminate
                    # batch job?" on Ctrl+C, and no error once moroder deletes it. In
                    # the OEM code page cmd reads, through %LOCALAPPDATA% when it can
                    # (an accented user name stays out of it).
                    $CmdExe = $Exe
                    if ($Exe.StartsWith($env:LOCALAPPDATA, [StringComparison]::OrdinalIgnoreCase)) { $CmdExe = "%LOCALAPPDATA%" + $Exe.Substring($env:LOCALAPPDATA.Length) }
                    $Oem = [Text.Encoding]::GetEncoding([Globalization.CultureInfo]::CurrentCulture.TextInfo.OEMCodePage)
                    [IO.File]::WriteAllText((Join-Path $ShimDir "moroder.cmd"), "@(goto) 2>nul & `"$CmdExe`" %*`r`n", $Oem)
                    # bash / zsh (Git Bash, MSYS2, Cygwin): no extension, a shebang
                    $ExeSh = $Exe -replace "'", "'\''"
                    $Sh = "#!/bin/sh`n# moroder launcher (install.ps1); removed once the PATH is in place`nexe='$ExeSh'`nif command -v cygpath >/dev/null 2>&1; then exe=`$(cygpath -u `"`$exe`"); elif [ -n `"`$WSL_DISTRO_NAME`" ] && command -v wslpath >/dev/null 2>&1; then exe=`$(wslpath -u `"`$exe`"); fi`nexec `"`$exe`" `"`$@`"`n"
                    [IO.File]::WriteAllText((Join-Path $ShimDir "moroder"), $Sh, $NoBom)
                    $Launcher = $true
                } catch { }
            }
            # No launcher, and the shell that ran this didn't have moroder: say what to do.
            $NeedNew = -not $Launcher -and -not ($Seen -contains $Prefix.TrimEnd("\"))
            if (-not (($env:Path -split ";") -contains $Prefix)) { $env:Path = "$env:Path;$Prefix" }
        }
        finally {
            Remove-Item $Tmp -Recurse -Force -ErrorAction SilentlyContinue
        }

        $Took = [int]((Get-Date) - $Start).TotalSeconds
        Write-Host ""
        Write-Host "  $Tick " -ForegroundColor Green -NoNewline
        Write-Host "Moroder $Version is installed" -NoNewline
        Write-Host " ($Took s)" -ForegroundColor DarkGray
        if (Test-Path (Join-Path $HOME ".config\moroder\browser.json")) {
            Write-Host "    Run moroder, or open the Moroder profile in Windows Terminal (icons)"
        } else {
            Write-Host "    Next: moroder setup to sign in once, then moroder"
            Write-Host "    In Windows Terminal, the Moroder profile (tab menu) shows the icons" -ForegroundColor DarkGray
            Write-Host "    No account? moroder -a" -ForegroundColor DarkGray
        }
        if ($NeedNew) {
            Write-Host "    moroder not found? Open a new terminal" -ForegroundColor DarkGray
        }
        # A suggestion, not a requirement.
        if ($env:TERM_PROGRAM -ne "WarpTerminal") {
            Write-Host ""
            Write-Host "    Moroder looks its best in Warp: " -ForegroundColor DarkGray -NoNewline
            Write-Host "https://www.warp.dev" -ForegroundColor DarkGray
        }
        Write-Host ""
    } catch [OperationCanceledException] {
        Write-Host ""; Write-Host "  $Cross " -ForegroundColor Red -NoNewline; Write-Host $_.Exception.Message; Write-Host ""
        if ($PSCommandPath) { exit 1 }      # .\install.ps1: a failed exit code (CI)
    }
}
