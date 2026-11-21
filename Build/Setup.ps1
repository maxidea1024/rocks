<#
Rocks setup wizard for Windows hosts (Docs/Design/SetupWizard.ko.md). Setup.bat at the repository root starts it with PowerShell 7 when
present and with Windows PowerShell 5.1 otherwise, so this file stays ASCII and avoids PowerShell 7 only syntax.

It checks every prerequisite the way a new terminal will see it (machine and user environment read from the registry, not this
process), asks once, installs only what is missing or wrong, and checks again. Once everything passes, running it again changes nothing.

Building needs no Visual Studio IDE: when no Visual Studio is installed, Setup installs Build Tools for Visual Studio 2026 (MSVC and the
Windows SDK, which clang-cl uses). -WithIde adds Visual Studio Community with the Clang IntelliSense components for editing in the IDE.

Usage: Setup.bat [-Check] [-Yes] [-WithIde] [-Android] [-ToolchainRoot <directory>]
  -Check          Check only. Exit code 0 when everything passes, 1 otherwise.
  -Yes            Install without asking.
  -WithIde        Also check and install the Visual Studio IDE with the components in .vsconfig.
  -Android        Also check and install the pinned Android NDK.
  -ToolchainRoot  Where LLVM and clang-format are unpacked (default %LOCALAPPDATA%\Rocks\toolchains).
  -SelfTest       Run the unit checks of this script and exit.
#>
[CmdletBinding()]
param(
    [switch] $Check,
    [switch] $Yes,
    [switch] $WithIde,
    [switch] $Android,
    [string] $ToolchainRoot = '',
    [switch] $SelfTest)

Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'
$RocksRoot = Split-Path -Parent $PSScriptRoot
$BuildCommand = 'dotnet msbuild Rocks.proj -t:Test -p:Configuration=Debug -p:RocksTarget=Foundation'

# Registry location of the user environment (HKCU). Self tests pass a scratch subkey instead.
$UserEnvironmentKey = 'Environment'

# Components the build needs, installed as Build Tools for Visual Studio when no Visual Studio exists. The IDE components (Clang
# IntelliSense) are in .vsconfig, which -WithIde installs and Visual Studio reads when it opens the repository.
$VsToolsComponent = 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'
$WindowsSdkComponent = 'Microsoft.VisualStudio.Component.Windows11SDK.26100'
$BuildToolsComponents = @('Microsoft.VisualStudio.Workload.VCTools', $VsToolsComponent, $WindowsSdkComponent)

# Install is a script block run as `& $Install $Data` from the script scope (so it sees the functions below); Data carries the values it
# needs. Hint is the next action when the item cannot be installed automatically.
function New-RocksStatus([bool] $Ok, [string] $Detail, [scriptblock] $Install = $null, [string] $Hint = '', [hashtable] $Data = @{}) {
    return [pscustomobject]@{ Ok = $Ok; Detail = $Detail; Install = $Install; Hint = $Hint; Data = $Data }
}

# Windows PowerShell 5.1 writes a parsed JSON array to the pipeline as one object, so arrays are parsed as an argument, not piped.
function ConvertFrom-RocksJson([string] $Text) { return ConvertFrom-Json $Text }

# --- Pure helpers (covered by -SelfTest) -------------------------------------------------------------------------------------------------

# True when the SDK version is in the band global.json asks for: same major.minor and feature band (hundreds of the patch), patch at
# least the pinned one (rollForward latestPatch). Prerelease versions never match (allowPrerelease false).
function Test-RocksSdkBand([string] $Candidate, [string] $Required) {
    if ($Candidate -notmatch '^(\d+)\.(\d+)\.(\d+)$') { return $false }
    $c = @([int] $Matches[1], [int] $Matches[2], [int] $Matches[3])
    if ($Required -notmatch '^(\d+)\.(\d+)\.(\d+)$') { throw "global.json sdk.version is not major.minor.patch: $Required" }
    $r = @([int] $Matches[1], [int] $Matches[2], [int] $Matches[3])
    return $c[0] -eq $r[0] -and $c[1] -eq $r[1] -and [math]::Floor($c[2] / 100) -eq [math]::Floor($r[2] / 100) -and $c[2] -ge $r[2]
}

# Major version from the first --version line of a Clang driver, or $null.
function Get-RocksClangMajor([string] $Line) {
    if ($Line -match '(?:^|\s)(?:Apple )?clang version (\d+)\.\d+\.\d+') { return [int] $Matches[1] }
    return $null
}

# True when the first --version line matches an entry of Build/Toolchain.json clang.verified. An entry is "clang version X.Y.Z (hash)";
# the official build prints the hash inside the repository URL, so the version must match and the hash must appear.
function Test-RocksVerifiedClang([string] $Line, [string[]] $Verified) {
    foreach ($entry in $Verified) {
        if ($entry -notmatch '^(?<name>(?:Apple )?clang version) (?<version>\d+\.\d+\.\d+)(?: \((?<tag>[^)]+)\))?$') { continue }
        $tag = if ($Matches.ContainsKey('tag')) { $Matches['tag'] } else { '' }
        $prefix = '^' + [regex]::Escape($Matches['name'] + ' ' + $Matches['version']) + '(?:\s|$)'
        if (-not [regex]::IsMatch($Line, $prefix)) { continue }
        if ($tag -and -not $Line.Contains($tag)) { continue }
        return $true
    }

    return $false
}

# Minimum Clang major that an MSVC STL accepts (STL1000), read from yvals_core.h like Rocks.Build (ToolchainTable.ParseStlClangMinimum).
function Get-RocksStlClangMinimum([string] $Text) {
    $match = [regex]::Match($Text, '#if __clang_major__ < (?<major>\d+)[ \t]*\r?\n[ \t]*#error STL1000')
    if ($match.Success) { return [int] $match.Groups['major'].Value }
    return $null
}

function ConvertTo-RocksPathKey([string] $Entry) {
    $expanded = [Environment]::ExpandEnvironmentVariables($Entry.Trim().Trim('"'))
    return $expanded.TrimEnd('\', '/').ToLowerInvariant()
}

# Puts Entry first in a PATH value and drops other spellings of the same directory (case, trailing separator, %VARIABLE% form).
# Empty items are removed. Calling it again with the same entry returns the same text.
function Join-RocksPathFront([string] $Value, [string] $Entry) {
    $key = ConvertTo-RocksPathKey $Entry
    $rest = @(($Value -split ';') | Where-Object { $_.Trim() -and (ConvertTo-RocksPathKey $_) -ne $key })
    return (@($Entry) + $rest) -join ';'
}

# --- Environment as a new terminal sees it --------------------------------------------------------------------------------------------

function Get-RocksEnvironmentValue([string] $Name) {
    $user = [Environment]::GetEnvironmentVariable($Name, 'User')
    if ($user) { return $user }
    return [Environment]::GetEnvironmentVariable($Name, 'Machine')
}

# Windows builds the PATH of a new process as the machine PATH followed by the user PATH.
function Get-RocksNewShellPath {
    $machine = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    $user = [Environment]::GetEnvironmentVariable('Path', 'User')
    return [Environment]::ExpandEnvironmentVariables((@($machine, $user) | Where-Object { $_ }) -join ';')
}

function Find-RocksExecutable([string] $Name, [string] $PathValue) {
    $suffixes = if ([IO.Path]::GetExtension($Name)) { @('') } else { @('.exe', '.cmd', '.bat') }
    foreach ($directory in ($PathValue -split ';')) {
        $directory = $directory.Trim().Trim('"')
        if (-not $directory) { continue }
        foreach ($suffix in $suffixes) {
            $candidate = Join-Path $directory ($Name + $suffix)
            if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
        }
    }

    return $null
}

function ConvertTo-RocksArgument([string] $Value) {
    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') { return $Value }
    # CommandLineToArgvW rules: backslashes before a quote and at the end are doubled, quotes are escaped.
    $escaped = [regex]::Replace($Value, '(\\*)"', [Text.RegularExpressions.MatchEvaluator] { param($m) ($m.Groups[1].Value * 2) + '\"' })
    $escaped = [regex]::Replace($escaped, '(\\+)$', [Text.RegularExpressions.MatchEvaluator] { param($m) $m.Groups[1].Value * 2 })
    return '"' + $escaped + '"'
}

# Runs a program and captures its output without letting stderr turn into PowerShell errors (Windows PowerShell 5.1 does that).
function Invoke-RocksNative([string] $FilePath, [string[]] $Arguments, [hashtable] $Environment = @{}) {
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $FilePath
    $info.Arguments = (@($Arguments) | ForEach-Object { ConvertTo-RocksArgument $_ }) -join ' '
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.CreateNoWindow = $true
    $info.WorkingDirectory = $RocksRoot
    foreach ($name in $Environment.Keys) { $info.EnvironmentVariables[$name] = $Environment[$name] }
    $process = [Diagnostics.Process]::Start($info)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $process.WaitForExit()
    return [pscustomobject]@{ ExitCode = $process.ExitCode; StdOut = $stdout.Result; StdErr = $stderr.Result }
}

function Get-RocksFirstLine([string] $Text) {
    return (($Text -split "`r?`n") | Where-Object { $_.Trim() } | Select-Object -First 1)
}

# --- Writing the user environment ---------------------------------------------------------------------------------------------------

function Send-RocksEnvironmentChange {
    # Tells Explorer and other top-level windows that the environment changed, so programs started from them see the new values.
    if (-not ('Rocks.Setup.NativeMethods' -as [type])) {
        Add-Type -Namespace Rocks.Setup -Name NativeMethods -MemberDefinition @'
[DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
public static extern IntPtr SendMessageTimeout(IntPtr hWnd, uint msg, UIntPtr wParam, string lParam, uint flags, uint timeout, out UIntPtr result);
'@
    }

    $result = [UIntPtr]::Zero
    [void] [Rocks.Setup.NativeMethods]::SendMessageTimeout([IntPtr] 0xffff, 0x1A, [UIntPtr]::Zero, 'Environment', 2, 5000, [ref] $result)
}

# Puts Entry first in the user PATH. The raw value keeps %VARIABLE% references and its REG_EXPAND_SZ kind (Environment.SetEnvironmentVariable
# would expand them and store REG_SZ). Returns $true when the value changed.
function Set-RocksUserPathFront([string] $Entry, [string] $SubKey = $UserEnvironmentKey) {
    $key = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($SubKey)
    try {
        $old = [string] $key.GetValue('Path', '', [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
        $kind = if ($key.GetValueNames() -contains 'Path') { $key.GetValueKind('Path') } else { [Microsoft.Win32.RegistryValueKind]::ExpandString }
        if ($kind -ne [Microsoft.Win32.RegistryValueKind]::String) { $kind = [Microsoft.Win32.RegistryValueKind]::ExpandString }
        $new = Join-RocksPathFront $old $Entry
        if ($new -ceq $old) { return $false }
        $key.SetValue('Path', $new, $kind)
    }
    finally { $key.Dispose() }
    if ($SubKey -eq 'Environment') { Send-RocksEnvironmentChange }
    return $true
}

function Set-RocksUserVariable([string] $Name, [string] $Value, [string] $SubKey = $UserEnvironmentKey) {
    $key = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($SubKey)
    try {
        if ([string] $key.GetValue($Name, '', [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames) -ceq $Value) { return $false }
        $key.SetValue($Name, $Value, [Microsoft.Win32.RegistryValueKind]::String)
    }
    finally { $key.Dispose() }
    if ($SubKey -eq 'Environment') { Send-RocksEnvironmentChange }
    return $true
}

# --- Installers ------------------------------------------------------------------------------------------------------------------------

function Install-RocksWinget([string] $Id, [string] $Version = '', [string] $Override = '') {
    $winget = Find-RocksExecutable 'winget' ($env:Path + ';' + (Get-RocksNewShellPath))
    if (-not $winget) { throw "winget not found. Next: install 'App Installer' from the Microsoft Store, then run Setup again." }
    $arguments = @('install', '--id', $Id, '--exact', '--source', 'winget', '--accept-package-agreements', '--accept-source-agreements')
    if ($Version) { $arguments += @('--version', $Version) }
    if ($Override) { $arguments += @('--override', $Override) } else { $arguments += '--silent' }
    # Start-Process with an explicitly quoted command line: Windows PowerShell 5.1 drops quotes inside native arguments (the --override text).
    $process = Start-Process -FilePath $winget -ArgumentList ((@($arguments) | ForEach-Object { ConvertTo-RocksArgument $_ }) -join ' ') -NoNewWindow -Wait -PassThru
    if ($process.ExitCode -ne 0) {
        throw "winget install $Id failed with exit code $($process.ExitCode). Next: run 'winget install --id $Id' yourself and read its message."
    }
}

function Get-RocksPwsh {
    $pwsh = Find-RocksExecutable 'pwsh' (Get-RocksNewShellPath)
    if (-not $pwsh) {
        $default = Join-Path $env:ProgramFiles 'PowerShell\7\pwsh.exe'
        if (Test-Path -LiteralPath $default) { $pwsh = $default }
    }

    return $pwsh
}

function Invoke-RocksBuildScript([string] $Script, [string[]] $Arguments) {
    $pwsh = Get-RocksPwsh
    if (-not $pwsh) { throw 'PowerShell 7 is required for this step. Next: let Setup install PowerShell 7, then run Setup again.' }
    & $pwsh -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot $Script) @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Build/$Script failed with exit code $LASTEXITCODE (see its message above)." }
}

# --- Checks -----------------------------------------------------------------------------------------------------------------------------

function Get-RocksToolchainTable { return Get-Content -LiteralPath (Join-Path $PSScriptRoot 'Toolchain.json') -Raw | ConvertFrom-Json }

function Get-RocksToolchainDirectory {
    if ($ToolchainRoot) { return $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($ToolchainRoot) }
    return Join-Path $env:LOCALAPPDATA 'Rocks\toolchains'
}

function Get-RocksGitStatus {
    $git = Find-RocksExecutable 'git' (Get-RocksNewShellPath)
    $install = {
        param($d)
        if ($d.InstallGit) { Install-RocksWinget 'Git.Git' }
        $found = Find-RocksExecutable 'git' (Get-RocksNewShellPath)
        if (-not $found) { throw 'git is still not on PATH. Next: open a new terminal and run Setup again.' }
        $result = Invoke-RocksNative $found @('config', '--global', 'core.longpaths', 'true')
        if ($result.ExitCode -ne 0) { throw "git config failed: $($result.StdErr.Trim())" }
    }
    if (-not $git) { return New-RocksStatus $false 'not found' $install '' @{ InstallGit = $true } }

    $version = (Get-RocksFirstLine (Invoke-RocksNative $git @('--version')).StdOut) -replace '^git version ', ''
    $longPaths = (Invoke-RocksNative $git @('config', '--global', '--get', 'core.longpaths')).StdOut.Trim()
    if ($longPaths -ne 'true') { return New-RocksStatus $false "$version, core.longpaths is not true" $install '' @{ InstallGit = $false } }
    return New-RocksStatus $true "$version, core.longpaths=true"
}

function Get-RocksDotnetStatus {
    $required = (Get-Content -LiteralPath (Join-Path $RocksRoot 'global.json') -Raw | ConvertFrom-Json).sdk.version
    $major = ($required -split '\.')[0]
    $band = ($required -replace '^(\d+\.\d+)\.(\d)\d\d$', '$1.$2xx')
    $install = { param($d) Install-RocksWinget "Microsoft.DotNet.SDK.$($d.Major)" $d.Version }
    $data = @{ Major = $major; Version = $required }
    $dotnet = Find-RocksExecutable 'dotnet' (Get-RocksNewShellPath)
    if (-not $dotnet) { return New-RocksStatus $false "not found (needs $band, at least $required)" $install '' $data }
    $sdks = @(((Invoke-RocksNative $dotnet @('--list-sdks')).StdOut -split "`r?`n") | ForEach-Object { ($_ -split ' ')[0] } | Where-Object { $_ })
    $match = @($sdks | Where-Object { Test-RocksSdkBand $_ $required } | Sort-Object { [version] $_ } -Descending)
    if ($match.Count -gt 0) { return New-RocksStatus $true $match[0] }
    $found = if ($sdks.Count -gt 0) { $sdks -join ', ' } else { 'none' }
    return New-RocksStatus $false "needs $band (at least $required); installed: $found" $install '' $data
}

function Get-RocksPwshStatus {
    $pwsh = Get-RocksPwsh
    $install = { Install-RocksWinget 'Microsoft.PowerShell' }
    if (-not $pwsh) { return New-RocksStatus $false 'not found' $install }
    $version = (Invoke-RocksNative $pwsh @('-NoProfile', '-NonInteractive', '-Command', '$PSVersionTable.PSVersion.ToString()')).StdOut.Trim()
    if ($version -notmatch '^(\d+)\.' -or [int] $Matches[1] -lt 7) { return New-RocksStatus $false "found $version, needs 7 or newer" $install }
    return New-RocksStatus $true $version
}

function Get-RocksVsConfigComponents {
    return @((Get-Content -LiteralPath (Join-Path $RocksRoot '.vsconfig') -Raw | ConvertFrom-Json).components)
}

function Get-RocksVsWhere { return Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe' }

# Newest installation that vswhere reports for the arguments, with its package list, or $null.
function Get-RocksVsQuery([string[]] $Arguments) {
    $vswhere = Get-RocksVsWhere
    if (-not (Test-Path -LiteralPath $vswhere)) { return $null }
    $json = (Invoke-RocksNative $vswhere (@('-latest', '-include', 'packages', '-format', 'json') + $Arguments)).StdOut
    $instances = @(ConvertFrom-RocksJson $json)
    if ($instances.Count -eq 0) { return $null }
    return $instances[0]
}

# The installation the build picks (Rocks.Tooling.Common NativeEnvironment: -products * includes Build Tools), or $null.
function Get-RocksVsInstance { return Get-RocksVsQuery @('-products', '*', '-requires', $VsToolsComponent) }

# The newest Visual Studio IDE (vswhere without -products reports Community, Professional and Enterprise only), or $null.
function Get-RocksVsIdeInstance { return Get-RocksVsQuery @() }

# Path of the MSVC toolset the build would use: ROCKS_MSVC_TOOLSET when set, otherwise the installation's default toolset.
function Get-RocksMsvcToolsDirectory($Instance) {
    $msvc = Join-Path $Instance.installationPath 'VC\Tools\MSVC'
    $pinned = Get-RocksEnvironmentValue 'ROCKS_MSVC_TOOLSET'
    if ($pinned) {
        return Get-ChildItem -LiteralPath $msvc -Directory -ErrorAction SilentlyContinue |
            Where-Object { ($_.Name + '.').StartsWith($pinned + '.') } | Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty FullName
    }

    $defaultFile = Join-Path $Instance.installationPath 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt'
    if (-not (Test-Path -LiteralPath $defaultFile)) { return $null }
    return Join-Path $msvc (Get-Content -LiteralPath $defaultFile -Raw).Trim()
}

# Adds components to an existing installation (Build Tools or IDE) with the Visual Studio Installer.
function Install-RocksVsModify([string] $InstallationPath, [string[]] $Selection) {
    $setup = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\setup.exe'
    if (-not (Test-Path -LiteralPath $setup)) { throw 'Visual Studio Installer not found. Next: run Setup again; it installs Build Tools for Visual Studio 2026.' }
    $arguments = @('modify', '--installPath', $InstallationPath) + $Selection + @('--passive', '--norestart') | ForEach-Object { ConvertTo-RocksArgument $_ }
    # The Visual Studio installer needs administrator rights and shows the UAC prompt itself.
    $process = Start-Process -FilePath $setup -ArgumentList ($arguments -join ' ') -Verb RunAs -Wait -PassThru
    # 3010 means success with a restart required.
    if ($process.ExitCode -ne 0 -and $process.ExitCode -ne 3010) {
        throw "Visual Studio Installer exited with code $($process.ExitCode). Next: open Visual Studio Installer, choose Modify, and add the missing components."
    }
}

function ConvertTo-RocksVsAdd([string[]] $Components) { return @($Components | ForEach-Object { '--add'; $_ }) }

function Get-RocksWindowsSdks {
    $kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Include'
    return @(Get-ChildItem -LiteralPath $kits -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'um\Windows.h') } | Sort-Object Name -Descending)
}

# C++ build tools: MSVC (STL and C runtime) and a Windows SDK, which clang-cl uses. Build Tools for Visual Studio (no IDE) is enough.
function Get-RocksBuildToolsStatus {
    $install = {
        param($d)
        if ($d.Path) { Install-RocksVsModify $d.Path (ConvertTo-RocksVsAdd $d.Add); return }
        $override = '--wait --passive --norestart ' + ((ConvertTo-RocksVsAdd $BuildToolsComponents) -join ' ')
        Install-RocksWinget 'Microsoft.VisualStudio.BuildTools' '' $override
    }
    $instance = Get-RocksVsInstance
    if (-not $instance) {
        $vswhere = Get-RocksVsWhere
        if (Test-Path -LiteralPath $vswhere) {
            $all = @(ConvertFrom-RocksJson (Invoke-RocksNative $vswhere @('-all', '-products', '*', '-format', 'json')).StdOut)
            $broken = @($all | Where-Object { -not $_.isComplete })
            if ($broken.Count -gt 0) {
                $path = $broken[0].installationPath
                $hint = 'Open Visual Studio Installer and choose Repair for that installation.'
                return New-RocksStatus $false "installation at $path is incomplete" $install $hint @{ Path = $path; Add = @($VsToolsComponent, $WindowsSdkComponent) }
            }
        }

        return New-RocksStatus $false 'MSVC build tools not found (installs Build Tools for Visual Studio 2026, no IDE)' $install '' @{ Path = '' }
    }

    $name = "$($instance.displayName) $($instance.installationVersion)"
    $data = @{ Path = $instance.installationPath; Add = @($WindowsSdkComponent) }
    $sdk = Get-RocksWindowsSdks
    if ($sdk.Count -eq 0) { return New-RocksStatus $false "$name, no Windows SDK" $install '' $data }
    $tools = Get-RocksMsvcToolsDirectory $instance
    if (-not $tools -or -not (Test-Path -LiteralPath $tools)) {
        $pinned = Get-RocksEnvironmentValue 'ROCKS_MSVC_TOOLSET'
        $hint = 'Install that MSVC toolset in Visual Studio Installer or clear ROCKS_MSVC_TOOLSET.'
        return New-RocksStatus $false "$name, MSVC toolset $pinned (ROCKS_MSVC_TOOLSET) is not installed" $null $hint
    }

    return New-RocksStatus $true "$name, MSVC $(Split-Path -Leaf $tools), Windows SDK $($sdk[0].Name)"
}

# Visual Studio IDE for editing (-WithIde): Community or a higher edition with the components in .vsconfig (Clang IntelliSense).
function Get-RocksVsIdeStatus {
    $config = Join-Path $RocksRoot '.vsconfig'
    $install = {
        param($d)
        if ($d.Path) { Install-RocksVsModify $d.Path @('--config', $d.Config); return }
        Install-RocksWinget 'Microsoft.VisualStudio.Community' '' ('--wait --passive --norestart --config ' + (ConvertTo-RocksArgument $d.Config))
    }
    $instance = Get-RocksVsIdeInstance
    if (-not $instance) { return New-RocksStatus $false 'Visual Studio IDE not found (installs Visual Studio Community 2026)' $install '' @{ Path = ''; Config = $config } }
    $installed = @($instance.packages | ForEach-Object { $_.id })
    $missing = @(Get-RocksVsConfigComponents | Where-Object { $installed -notcontains $_ })
    $name = "$($instance.displayName) $($instance.installationVersion)"
    if ($missing.Count -gt 0) {
        return New-RocksStatus $false "$name, missing: $($missing -join ', ')" $install '' @{ Path = $instance.installationPath; Config = $config }
    }

    return New-RocksStatus $true "$name with the .vsconfig components"
}

function Get-RocksLlvmStatus {
    $table = Get-RocksToolchainTable
    $version = $table.llvm.windows.version
    $verified = @($table.clang.verified.Windows)
    $root = Get-RocksToolchainDirectory
    $bin = Join-Path $root "llvm-$version\bin"
    $install = {
        param($d)
        $sevenZip = Join-Path $env:ProgramFiles '7-Zip\7z.exe'
        if (-not (Find-RocksExecutable '7z' (Get-RocksNewShellPath)) -and -not (Test-Path -LiteralPath $sevenZip)) { Install-RocksWinget '7zip.7zip' }
        Invoke-RocksBuildScript 'InstallLlvm.ps1' @('-ToolchainRoot', $d.Root)
        [void] (Set-RocksUserPathFront $d.Bin)
    }
    $data = @{ Root = $root; Bin = $bin }
    $hint = ''
    $shadow = Find-RocksExecutable 'clang-cl' ([Environment]::ExpandEnvironmentVariables([string] [Environment]::GetEnvironmentVariable('Path', 'Machine')))
    if ($shadow) {
        $hint = "A clang-cl in the system PATH ($(Split-Path -Parent $shadow)) comes before user entries; remove that entry or move $bin to the top of the system Path (administrator)."
    }

    $clang = Find-RocksExecutable 'clang-cl' (Get-RocksNewShellPath)
    if (-not $clang) { return New-RocksStatus $false "clang-cl not on PATH (needs LLVM $version)" $install $hint $data }
    $line = Get-RocksFirstLine (Invoke-RocksNative $clang @('--version')).StdOut
    if (-not (Test-RocksVerifiedClang $line $verified)) {
        return New-RocksStatus $false "first clang-cl on PATH is '$line' ($clang), needs LLVM $version" $install $hint $data
    }

    $instance = Get-RocksVsInstance
    if ($instance) {
        $tools = Get-RocksMsvcToolsDirectory $instance
        $header = ''
        if ($tools) { $header = Join-Path $tools 'include\yvals_core.h' }
        if ($header -and (Test-Path -LiteralPath $header)) {
            $minimum = Get-RocksStlClangMinimum (Get-Content -LiteralPath $header -Raw)
            $major = Get-RocksClangMajor $line
            if ($minimum -and $major -lt $minimum) {
                $toolset = Split-Path -Leaf $tools
                return New-RocksStatus $false "MSVC $toolset STL needs Clang $minimum (STL1000), PATH has Clang $major" $install $hint $data
            }
        }
    }

    $shown = $line -replace '\s*\(.*$', ''
    return New-RocksStatus $true "$shown at $(Split-Path -Parent $clang)"
}

function Get-RocksClangFormatStatus {
    $version = (Get-RocksToolchainTable).clangFormat.version
    $root = Get-RocksToolchainDirectory
    $install = {
        param($d)
        Invoke-RocksBuildScript 'InstallFormatter.ps1' @('-ToolchainRoot', $d.Root)
        [void] (Set-RocksUserVariable 'ROCKS_CLANG_FORMAT' $d.Target)
    }
    $data = @{ Root = $root; Target = (Join-Path $root "clang-format-$version\clang-format.exe") }
    $configured = Get-RocksEnvironmentValue 'ROCKS_CLANG_FORMAT'
    if (-not $configured) { return New-RocksStatus $false "ROCKS_CLANG_FORMAT is not set (needs clang-format $version)" $install '' $data }
    if (-not (Test-Path -LiteralPath $configured -PathType Leaf)) {
        return New-RocksStatus $false "ROCKS_CLANG_FORMAT points to a missing file: $configured" $install '' $data
    }

    $line = Get-RocksFirstLine (Invoke-RocksNative $configured @('--version')).StdOut
    if ($line -notmatch ('clang-format version ' + [regex]::Escape($version) + '(?:\s|$)')) {
        return New-RocksStatus $false "ROCKS_CLANG_FORMAT is '$line', needs $version" $install '' $data
    }

    return New-RocksStatus $true "$version at $configured"
}

function Get-RocksAndroidStatus {
    $version = (Get-RocksToolchainTable).android.ndk.version
    $sdk = @((Get-RocksEnvironmentValue 'ANDROID_HOME'), (Get-RocksEnvironmentValue 'ANDROID_SDK_ROOT')) | Where-Object { $_ } | Select-Object -First 1
    $defaultSdk = Join-Path $env:LOCALAPPDATA 'Android\Sdk'
    if (-not $sdk -and (Test-Path -LiteralPath $defaultSdk)) { $sdk = $defaultSdk }
    if (-not $sdk) {
        return New-RocksStatus $false 'Android SDK not found' $null 'Install Android Studio (it creates %LOCALAPPDATA%\Android\Sdk), then run Setup.bat -Android again.'
    }

    $ndk = Join-Path $sdk "ndk\$version"
    $install = {
        param($d)
        # Build/SelectAndroidNdk.ps1 reads the SDK location from its environment and installs the NDK with sdkmanager when missing.
        $env:ANDROID_HOME = $d.Sdk
        Invoke-RocksBuildScript 'SelectAndroidNdk.ps1' @()
        if (-not [Environment]::GetEnvironmentVariable('ANDROID_HOME', 'User')) { [void] (Set-RocksUserVariable 'ANDROID_HOME' $d.Sdk) }
        [void] (Set-RocksUserVariable 'ANDROID_NDK_HOME' $d.Ndk)
    }
    $data = @{ Sdk = $sdk; Ndk = $ndk }
    $properties = Join-Path $ndk 'source.properties'
    if (-not (Test-Path -LiteralPath $properties)) { return New-RocksStatus $false "NDK $version is not installed in $sdk" $install '' $data }
    $revisionLine = Get-Content -LiteralPath $properties | Where-Object { $_ -match '^\s*Pkg\.Revision\s*=' } | Select-Object -First 1
    $revision = ([string] $revisionLine -replace '^\s*Pkg\.Revision\s*=\s*', '').Trim()
    if ($revision -ne $version) { return New-RocksStatus $false "$ndk reports revision '$revision'" $install '' $data }
    $selected = Get-RocksEnvironmentValue 'ANDROID_NDK_HOME'
    if (-not $selected -or (ConvertTo-RocksPathKey $selected) -ne (ConvertTo-RocksPathKey $ndk)) {
        return New-RocksStatus $false "ANDROID_NDK_HOME is '$selected', should be $ndk" $install '' $data
    }

    return New-RocksStatus $true "NDK $version at $ndk"
}

# --- Self test -------------------------------------------------------------------------------------------------------------------------

function Invoke-RocksSelfTest {
    $failures = New-Object Collections.Generic.List[string]
    function Assert-Rocks([bool] $Condition, [string] $Message) { if (-not $Condition) { $failures.Add($Message) } }

    Assert-Rocks (Test-RocksSdkBand '10.0.302' '10.0.302') 'sdk band: exact version'
    Assert-Rocks (Test-RocksSdkBand '10.0.399' '10.0.302') 'sdk band: later patch in the band'
    Assert-Rocks (-not (Test-RocksSdkBand '10.0.301' '10.0.302')) 'sdk band: older patch'
    Assert-Rocks (-not (Test-RocksSdkBand '10.0.404' '10.0.302')) 'sdk band: next feature band'
    Assert-Rocks (-not (Test-RocksSdkBand '9.0.302' '10.0.302')) 'sdk band: other major'
    Assert-Rocks (-not (Test-RocksSdkBand '10.0.302-preview.1' '10.0.302')) 'sdk band: prerelease'

    $verified = @('clang version 22.1.8 (ca7933e47d3a3451d81e72ac174dcb5aa28b59d1)')
    Assert-Rocks (Test-RocksVerifiedClang 'clang version 22.1.8 (https://github.com/llvm/llvm-project ca7933e47d3a3451d81e72ac174dcb5aa28b59d1)' $verified) 'clang: official build line'
    Assert-Rocks (-not (Test-RocksVerifiedClang 'clang version 22.1.8 (https://github.com/llvm/llvm-project 0000000)' $verified)) 'clang: other hash'
    Assert-Rocks (-not (Test-RocksVerifiedClang 'clang version 22.1.80 (ca7933e47d3a3451d81e72ac174dcb5aa28b59d1)' $verified)) 'clang: longer version'
    Assert-Rocks (-not (Test-RocksVerifiedClang 'clang version 21.1.6' $verified)) 'clang: older version'
    Assert-Rocks (Test-RocksVerifiedClang 'Apple clang version 17.0.0 (clang-1700.0.13.5)' @('Apple clang version 17.0.0 (clang-1700.0.13.5)')) 'clang: Apple line'
    Assert-Rocks ((Get-RocksClangMajor 'clang version 21.1.6') -eq 21) 'clang major: plain'
    Assert-Rocks ((Get-RocksClangMajor 'Apple clang version 17.0.0 (clang-1700.0.13.5)') -eq 17) 'clang major: Apple'
    Assert-Rocks ($null -eq (Get-RocksClangMajor 'gcc version 14')) 'clang major: not clang'

    $stl = "#ifdef __clang__`n#if __clang_major__ < 22`n#error STL1000: Unexpected compiler version, expected Clang 22.0.0 or newer.`n#endif"
    Assert-Rocks ((Get-RocksStlClangMinimum $stl) -eq 22) 'stl: STL1000 condition'
    Assert-Rocks ($null -eq (Get-RocksStlClangMinimum "#if __clang_major__ < 22`n#define X")) 'stl: other comparison'
    Assert-Rocks (((ConvertTo-RocksVsAdd @('A', 'B')) -join ' ') -eq '--add A --add B') 'vs: --add list'
    Assert-Rocks ((@($BuildToolsComponents) -join ' ') -eq 'Microsoft.VisualStudio.Workload.VCTools Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.Component.Windows11SDK.26100') 'vs: Build Tools components'
    $ide = @(Get-RocksVsConfigComponents)
    Assert-Rocks ($ide -contains 'Microsoft.VisualStudio.Component.VC.Llvm.Clang' -and $ide -contains 'Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset') 'vs: .vsconfig has the Clang IDE components'

    Assert-Rocks ((Join-RocksPathFront 'C:\a;C:\b' 'C:\llvm\bin') -eq 'C:\llvm\bin;C:\a;C:\b') 'path: prepend'
    Assert-Rocks ((Join-RocksPathFront 'C:\a;c:\LLVM\bin\;C:\b' 'C:\llvm\bin') -eq 'C:\llvm\bin;C:\a;C:\b') 'path: other spelling moves first'
    Assert-Rocks ((Join-RocksPathFront 'C:\llvm\bin;C:\a' 'C:\llvm\bin') -eq 'C:\llvm\bin;C:\a') 'path: already first'
    Assert-Rocks ((Join-RocksPathFront ';;C:\a;' 'C:\llvm\bin') -eq 'C:\llvm\bin;C:\a') 'path: empty items dropped'
    $local = Join-Path $env:LOCALAPPDATA 'Rocks\bin'
    Assert-Rocks ((Join-RocksPathFront '%LOCALAPPDATA%\Rocks\bin;C:\a' $local) -eq "$local;C:\a") 'path: %VARIABLE% spelling is the same directory'

    # Registry writes go to a scratch subkey so the real user environment is never touched.
    # The key sits directly under Software so deleting it leaves no parent key behind.
    $scratch = 'Software\RocksSetupSelfTest-' + [Guid]::NewGuid().ToString('N')
    try {
        $key = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($scratch)
        $key.SetValue('Path', '%USERPROFILE%\tools;C:\a', [Microsoft.Win32.RegistryValueKind]::ExpandString)
        $key.Dispose()
        Assert-Rocks (Set-RocksUserPathFront 'C:\llvm\bin' $scratch) 'registry: first write changes the value'
        Assert-Rocks (-not (Set-RocksUserPathFront 'C:\llvm\bin' $scratch)) 'registry: second write changes nothing'
        $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($scratch)
        $raw = $key.GetValue('Path', '', [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
        Assert-Rocks ($raw -ceq 'C:\llvm\bin;%USERPROFILE%\tools;C:\a') "registry: raw value keeps %USERPROFILE% ($raw)"
        Assert-Rocks ($key.GetValueKind('Path') -eq [Microsoft.Win32.RegistryValueKind]::ExpandString) 'registry: kind stays REG_EXPAND_SZ'
        $key.Dispose()
        Assert-Rocks (Set-RocksUserPathFront 'C:\new' "$scratch\Empty") 'registry: missing Path is created'
        Assert-Rocks (Set-RocksUserVariable 'ROCKS_CLANG_FORMAT' 'C:\cf\clang-format.exe' $scratch) 'registry: variable written'
        Assert-Rocks (-not (Set-RocksUserVariable 'ROCKS_CLANG_FORMAT' 'C:\cf\clang-format.exe' $scratch)) 'registry: same variable unchanged'
    }
    finally { [Microsoft.Win32.Registry]::CurrentUser.DeleteSubKeyTree($scratch, $false) }

    Assert-Rocks ((ConvertTo-RocksArgument 'plain') -eq 'plain') 'argument: plain'
    Assert-Rocks ((ConvertTo-RocksArgument 'C:\Program Files\x') -eq '"C:\Program Files\x"') 'argument: space'
    Assert-Rocks ((ConvertTo-RocksArgument 'C:\dir with space\') -eq '"C:\dir with space\\"') 'argument: trailing backslash'
    Assert-Rocks ((ConvertTo-RocksArgument 'a"b') -eq '"a\"b"') 'argument: quote'
    Assert-Rocks ((ConvertTo-RocksArgument '') -eq '""') 'argument: empty'

    foreach ($failure in $failures) { Write-Host "FAIL $failure" }
    Write-Host "Setup self test: $(if ($failures.Count -eq 0) { 'PASS' } else { 'FAIL' }) ($($failures.Count) failure(s), PowerShell $($PSVersionTable.PSVersion))"
    return $failures.Count
}

# --- Main ------------------------------------------------------------------------------------------------------------------------------

function Write-RocksLine([int] $Index, [int] $Count, [string] $Name, [string] $State, [string] $Detail) {
    $color = switch ($State) { 'OK' { 'Green' } 'installed' { 'Green' } 'installing' { 'Cyan' } default { 'Yellow' } }
    Write-Host ('[{0}/{1}] {2,-16} ' -f $Index, $Count, $Name) -NoNewline
    Write-Host ('{0,-10}' -f $State) -ForegroundColor $color -NoNewline
    Write-Host " $Detail"
}

function Invoke-RocksChecks($Steps) {
    $results = @()
    for ($index = 0; $index -lt $Steps.Count; ++$index) {
        try { $status = & $Steps[$index].Check }
        catch { $status = New-RocksStatus $false "check failed: $($_.Exception.Message)" }
        $state = if ($status.Ok) { 'OK' } else { 'MISSING' }
        Write-RocksLine ($index + 1) $Steps.Count $Steps[$index].Name $state $status.Detail
        if (-not $status.Ok -and $status.Hint) { Write-Host "        Next: $($status.Hint)" }
        $results += $status
    }

    return , $results
}

function Write-RocksNextStep {
    Write-Host ''
    Write-Host 'Setup is complete. Open a new terminal (or restart your IDE) and run:' -ForegroundColor Green
    Write-Host "  cd `"$RocksRoot`""
    Write-Host "  $BuildCommand"
}

# Explorer runs Setup.bat as Explorer -> cmd.exe -> PowerShell. Then the console window closes on exit, so wait for Enter first.
function Exit-Rocks([int] $Code) {
    try {
        $shell = Get-CimInstance Win32_Process -Filter "ProcessId = $PID"
        $cmd = Get-CimInstance Win32_Process -Filter "ProcessId = $($shell.ParentProcessId)"
        $starter = Get-CimInstance Win32_Process -Filter "ProcessId = $($cmd.ParentProcessId)"
        if ($cmd.Name -eq 'cmd.exe' -and $starter -and $starter.Name -eq 'explorer.exe') { [void] (Read-Host 'Press Enter to close') }
    }
    catch { }
    exit $Code
}

if ($SelfTest) { exit [int] ((Invoke-RocksSelfTest) -ne 0) }

$steps = @(
    @{ Name = 'Git'; Check = { Get-RocksGitStatus } },
    @{ Name = '.NET SDK'; Check = { Get-RocksDotnetStatus } },
    @{ Name = 'PowerShell 7'; Check = { Get-RocksPwshStatus } },
    @{ Name = 'C++ build tools'; Check = { Get-RocksBuildToolsStatus } },
    @{ Name = 'LLVM'; Check = { Get-RocksLlvmStatus } },
    @{ Name = 'clang-format'; Check = { Get-RocksClangFormatStatus } })
if ($WithIde) { $steps += @{ Name = 'VS IDE'; Check = { Get-RocksVsIdeStatus } } }
if ($Android) { $steps += @{ Name = 'Android NDK'; Check = { Get-RocksAndroidStatus } } }

Write-Host "Rocks setup for Windows ($RocksRoot)"
$results = Invoke-RocksChecks $steps
$pending = @(for ($index = 0; $index -lt $steps.Count; ++$index) { if (-not $results[$index].Ok) { $index } })
if ($pending.Count -eq 0) {
    Write-RocksNextStep
    Exit-Rocks 0
}

if ($Check) {
    Write-Host ''
    Write-Host 'Some items are missing. Run Setup.bat without -Check to install them.' -ForegroundColor Yellow
    Exit-Rocks 1
}

if (-not $Yes) {
    Write-Host ''
    $answer = ''
    try { $answer = Read-Host 'Install the missing items? [Y/n]' }
    catch { Write-Host 'Cannot ask in this session. Run Setup.bat -Yes to install without asking.'; Exit-Rocks 1 }
    if ($answer -match '^\s*n') { Write-Host 'Nothing was installed.'; Exit-Rocks 1 }
}

Write-Host ''
foreach ($index in $pending) {
    $step = $steps[$index]
    $status = $results[$index]
    if (-not $status.Install) {
        Write-RocksLine ($index + 1) $steps.Count $step.Name 'FAILED' "$($status.Detail). Next: $($status.Hint)"
        continue
    }

    Write-RocksLine ($index + 1) $steps.Count $step.Name 'installing' ''
    try {
        & $status.Install $status.Data
        Write-RocksLine ($index + 1) $steps.Count $step.Name 'installed' ''
    }
    catch {
        $next = if ($status.Hint) { " Next: $($status.Hint)" } else { '' }
        Write-RocksLine ($index + 1) $steps.Count $step.Name 'FAILED' ($_.Exception.Message + $next)
    }
}

Write-Host ''
Write-Host 'Checking again:'
$results = Invoke-RocksChecks $steps
if (@($results | Where-Object { -not $_.Ok }).Count -eq 0) {
    Write-RocksNextStep
    Exit-Rocks 0
}

Write-Host ''
Write-Host 'Some items still need attention (see the lines above). Fix them and run Setup.bat again.' -ForegroundColor Yellow
Exit-Rocks 1
