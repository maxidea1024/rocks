<#
Rocks setup wizard for Windows hosts (Docs/Design/SetupWizard.ko.md). Setup.bat at the repository root starts it with PowerShell 7 when
present and with Windows PowerShell 5.1 otherwise, so this file stays ASCII and avoids PowerShell 7 only syntax.

It checks every prerequisite the way a new terminal will see it (machine and user environment read from the registry, not this
process), asks once, installs only what is missing or wrong, and checks again. Once everything passes, running it again changes nothing.

Building needs no Visual Studio IDE: when no Visual Studio 17.8 or newer is installed, Setup installs Build Tools for Visual Studio 2026
(MSVC and the Windows SDK, which clang-cl uses). -WithIde installs Visual Studio Community with the Clang IntelliSense components first and
reuses it for the build.

Usage: Setup.bat [-Check] [-Yes] [-WithIde] [-Android] [-ToolchainRoot <directory>] [-Skip <step name>,...]
  -Check          Check only. Exit code 0 when everything passes, 1 otherwise.
  -Yes            Install without asking.
  -WithIde        Also check and install the Visual Studio IDE with the components in .vsconfig.
  -Android        Also check and install the pinned Android NDK.
  -ToolchainRoot  Where LLVM and clang-format are unpacked (default %LOCALAPPDATA%\Rocks\toolchains).
  -Skip           Leave out the named steps (for example 'FXC SDK' on machines that never run shader tests; CI uses it).
  -SelfTest       Run the unit checks of this script and exit.
#>
[CmdletBinding()]
param(
    [switch] $Check,
    [switch] $Yes,
    [switch] $WithIde,
    [switch] $Android,
    [string] $ToolchainRoot = '',
    [string[]] $Skip = @(),
    [switch] $SelfTest)

Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'
$RocksRoot = Split-Path -Parent $PSScriptRoot
$BuildCommand = 'dotnet msbuild Rocks.proj -t:Test -p:Configuration=Debug -p:RocksTarget=Foundation'

# Registry location of the user environment (HKCU). Self tests pass a scratch subkey instead.
$UserEnvironmentKey = 'Environment'

# Components the build needs, installed as Build Tools for Visual Studio when no Visual Studio exists. SDK 10.0.22621 carries the pinned
# FXC (d3dcompiler_47.dll, Engine/Source/ThirdParty/D3DCompiler/ThirdParty.json) that shader tests bootstrap. The IDE components (Clang
# IntelliSense) are in .vsconfig, which -WithIde installs and Visual Studio reads when it opens the repository.
$VsToolsComponent = 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'
$WindowsSdkComponent = 'Microsoft.VisualStudio.Component.Windows11SDK.26100'
$FxcSdkComponent = 'Microsoft.VisualStudio.Component.Windows11SDK.22621'
$VcComponents = @($VsToolsComponent, $WindowsSdkComponent, $FxcSdkComponent)
$BuildToolsComponents = @('Microsoft.VisualStudio.Workload.VCTools') + $VcComponents

# Shown when an automatic install cannot run or fails without a more specific next step.
$ManualGuide = 'follow appendix A of Docs/Guides/BuildEnvironment.ko.md (manual install)'
$NetworkHint = 'check the network or proxy (HTTPS_PROXY) and run Setup again'

# Self tests set this to keep the fake runs of the main flow quiet.
$script:RocksQuiet = $false

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

# Minimum Clang major that an MSVC STL accepts (STL1000), read from yvals_core.h with the same pattern as Rocks.Build
# (ToolchainTable.ParseStlClangMinimum): 14.29 writes "#error STL1000", 14.44 and 14.52 write "_EMIT_STL_ERROR(STL1000, ...)".
function Get-RocksStlClangMinimum([string] $Text) {
    $match = [regex]::Match($Text, '#if __clang_major__ < (?<major>\d+)[ \t]*\r?\n[ \t]*(?:#error[ \t]+|_EMIT_STL_ERROR\([ \t]*)STL1000\b')
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
    if (-not $winget) { throw "winget not found (Windows Server and LTSC lack it). Next: install 'App Installer' from the Microsoft Store, or $ManualGuide." }
    $arguments = @('install', '--id', $Id, '--exact', '--source', 'winget', '--accept-package-agreements', '--accept-source-agreements')
    if ($Version) { $arguments += @('--version', $Version) }
    if ($Override) { $arguments += @('--override', $Override) } else { $arguments += '--silent' }
    # Start-Process with an explicitly quoted command line: Windows PowerShell 5.1 drops quotes inside native arguments (the --override text).
    $process = Start-Process -FilePath $winget -ArgumentList ((@($arguments) | ForEach-Object { ConvertTo-RocksArgument $_ }) -join ' ') -NoNewWindow -Wait -PassThru
    if ($process.ExitCode -ne 0) {
        throw "winget install $Id failed with exit code $($process.ExitCode). Next: read the winget message above; for download errors $NetworkHint."
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
    if ($LASTEXITCODE -ne 0) { throw "Build/$Script failed with exit code $LASTEXITCODE. Next: read its message above; for download errors $NetworkHint." }
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
    $longPaths = (Invoke-RocksNative $git @('config', '--get', 'core.longpaths')).StdOut.Trim()
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

# vswhere version range of the accepted installations (Build/Toolchain.json visualStudio.minimumVersion, the same lower bound as
# Rocks.Tooling.Common NativeEnvironment and UE 5.8).
function Get-RocksVsRange { return '[' + (Get-RocksToolchainTable).visualStudio.minimumVersion + ',)' }

# Installations vswhere reports for the arguments (newest first), with their package lists. Empty when vswhere is missing.
function Get-RocksVsQuery([string[]] $Arguments) {
    $vswhere = Get-RocksVsWhere
    if (-not (Test-Path -LiteralPath $vswhere)) { return @() }
    $json = (Invoke-RocksNative $vswhere (@('-include', 'packages', '-format', 'json', '-version', (Get-RocksVsRange)) + $Arguments)).StdOut
    return @(ConvertFrom-RocksJson $json)
}

function Select-RocksFirst($Items) {
    $all = @($Items)
    if ($all.Count -eq 0) { return $null }
    return $all[0]
}

# The installation the build picks (NativeEnvironment: -products * includes Build Tools), or $null.
function Get-RocksVsInstance { return Select-RocksFirst (Get-RocksVsQuery @('-latest', '-products', '*', '-requires', $VsToolsComponent)) }

# The newest accepted installation of any product, with or without the C++ tools (complete installations only), or $null.
function Get-RocksVsAnyInstance { return Select-RocksFirst (Get-RocksVsQuery @('-latest', '-products', '*')) }

# The newest Visual Studio IDE (vswhere without -products reports Community, Professional and Enterprise only), or $null.
function Get-RocksVsIdeInstance { return Select-RocksFirst (Get-RocksVsQuery @('-latest')) }

# An accepted installation that is not complete (an interrupted install or update), or $null.
function Get-RocksVsIncompleteInstance {
    return Select-RocksFirst (@(Get-RocksVsQuery @('-all', '-products', '*')) | Where-Object { -not $_.isComplete })
}

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
    if (-not (Test-Path -LiteralPath $setup)) { throw "Visual Studio Installer not found. Next: run Setup again, or $ManualGuide." }
    $arguments = @('modify', '--installPath', $InstallationPath) + $Selection + @('--passive', '--norestart') | ForEach-Object { ConvertTo-RocksArgument $_ }
    # The Visual Studio installer needs administrator rights and shows the UAC prompt itself.
    $process = Start-Process -FilePath $setup -ArgumentList ($arguments -join ' ') -Verb RunAs -Wait -PassThru
    # 3010 means success with a restart required.
    if ($process.ExitCode -ne 0 -and $process.ExitCode -ne 3010) {
        throw "Visual Studio Installer exited with code $($process.ExitCode). Next: open Visual Studio Installer, choose Modify or Repair, and add the missing components."
    }
}

function ConvertTo-RocksVsAdd([string[]] $Components) { return @($Components | ForEach-Object { '--add'; $_ }) }

function Install-RocksBuildTools {
    $override = '--wait --passive --norestart ' + ((ConvertTo-RocksVsAdd $BuildToolsComponents) -join ' ')
    Install-RocksWinget 'Microsoft.VisualStudio.BuildTools' '' $override
}

function Get-RocksWindowsSdks {
    $kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Include'
    return @(Get-ChildItem -LiteralPath $kits -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'um\Windows.h') } | Sort-Object Name -Descending)
}

# True when a Windows SDK component (Microsoft.VisualStudio.Component.Windows11SDK.<build>) is already on disk, for example from the
# standalone SDK installer or another Visual Studio; the IDE then needs nothing more for it.
function Test-RocksSdkComponentOnDisk([string] $Component) {
    if ($Component -notmatch '\.Windows1[01]SDK\.(\d+)$') { return $false }
    return Test-Path -LiteralPath (Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\Include\10.0.$($Matches[1]).0\um\Windows.h")
}

# Directory name of an MSVC toolset path as a version, or $null.
function Get-RocksToolsetVersion([string] $Directory) {
    $parsed = $null
    if ($Directory -and [version]::TryParse((Split-Path -Leaf $Directory), [ref] $parsed)) { return $parsed }
    return $null
}

# C++ build tools: MSVC (STL and C runtime) and a Windows SDK, which clang-cl uses. Build Tools for Visual Studio (no IDE) is enough.
function Get-RocksBuildToolsStatus {
    $install = {
        param($d)
        if ($d.Path) { Install-RocksVsModify $d.Path (ConvertTo-RocksVsAdd $d.Add); return }
        Install-RocksBuildTools
    }
    $minimum = (Get-RocksToolchainTable).visualStudio.minimumVersion
    $instance = Get-RocksVsInstance
    if (-not $instance) {
        $broken = Get-RocksVsIncompleteInstance
        if ($broken) {
            $hint = 'Open Visual Studio Installer and choose Repair for that installation.'
            return New-RocksStatus $false "installation at $($broken.installationPath) is incomplete" $install $hint @{ Path = $broken.installationPath; Add = $VcComponents }
        }

        # An accepted installation without the C++ tools (for example an IDE installed for C#) gets them instead of a second installation.
        $other = Get-RocksVsAnyInstance
        if ($other) {
            $data = @{ Path = $other.installationPath; Add = $VcComponents }
            return New-RocksStatus $false "$($other.displayName) $($other.installationVersion) has no C++ build tools" $install '' $data
        }

        return New-RocksStatus $false "no Visual Studio $minimum or newer with C++ tools (installs Build Tools for Visual Studio 2026, no IDE)" $install '' @{ Path = '' }
    }

    $name = "$($instance.displayName) $($instance.installationVersion)"
    $data = @{ Path = $instance.installationPath; Add = $VcComponents }
    # @(...): a function returns one SDK as a single object, and StrictMode rejects .Count on it (CI images have one SDK).
    $sdk = @(Get-RocksWindowsSdks)
    if ($sdk.Count -eq 0) { return New-RocksStatus $false "$name, no Windows SDK" $install '' $data }
    $tools = Get-RocksMsvcToolsDirectory $instance
    $pinned = Get-RocksEnvironmentValue 'ROCKS_MSVC_TOOLSET'
    if (-not $tools -or -not (Test-Path -LiteralPath $tools)) {
        $hint = 'Install that MSVC toolset in Visual Studio Installer or clear ROCKS_MSVC_TOOLSET.'
        return New-RocksStatus $false "$name, MSVC toolset $pinned (ROCKS_MSVC_TOOLSET) is not installed" $null $hint
    }

    $minimumMsvc = (Get-RocksToolchainTable).visualStudio.minimumMsvc
    $toolset = Get-RocksToolsetVersion $tools
    if (-not $toolset -or $toolset -lt [version] $minimumMsvc) {
        $hint = if ($pinned) { "ROCKS_MSVC_TOOLSET=$pinned selects it; clear it or pick $minimumMsvc or newer." } else { '' }
        return New-RocksStatus $false "$name, MSVC $(Split-Path -Leaf $tools) is older than $minimumMsvc" $install $hint $data
    }

    return New-RocksStatus $true "$name, MSVC $(Split-Path -Leaf $tools), Windows SDK $($sdk[0].Name)"
}

# The pinned FXC source file (Engine/Source/ThirdParty/D3DCompiler/ThirdParty.json), as a full path in the Windows SDK.
function Get-RocksFxcSource {
    $manifest = Get-Content -LiteralPath (Join-Path $RocksRoot 'Engine/Source/ThirdParty/D3DCompiler/ThirdParty.json') -Raw | ConvertFrom-Json
    $file = @($manifest.hosts.'windows-x64'.files)[0]
    $kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
    return [pscustomobject]@{ Path = (Join-Path $kits ($file.sdkPath -replace '/', '\')); Sdk = $manifest.sdk.version }
}

# Windows SDK 10.0.22621 for the pinned FXC. Only shader tests need it (bootstrap copies the file); the build itself does not.
function Get-RocksFxcSdkStatus {
    $install = {
        param($d)
        $target = Get-RocksVsInstance
        if (-not $target) { $target = Get-RocksVsAnyInstance }
        if ($target) { Install-RocksVsModify $target.installationPath (ConvertTo-RocksVsAdd @($FxcSdkComponent)); return }
        Install-RocksBuildTools
    }
    $fxc = Get-RocksFxcSource
    if (-not (Test-Path -LiteralPath $fxc.Path -PathType Leaf)) { return New-RocksStatus $false "Windows SDK $($fxc.Sdk) not found ($($fxc.Path))" $install }
    return New-RocksStatus $true "Windows SDK $($fxc.Sdk) ($(Split-Path -Leaf $fxc.Path))"
}

# Visual Studio IDE for editing (-WithIde): Community or a higher edition with the components in .vsconfig (Clang IntelliSense and the C++
# tools). The step runs before the build tools step so a new IDE also provides the build tools.
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
    $missing = @(Get-RocksVsConfigComponents | Where-Object { $installed -notcontains $_ -and -not (Test-RocksSdkComponentOnDisk $_) })
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

# --- Main flow -------------------------------------------------------------------------------------------------------------------------

function Write-RocksLine([int] $Index, [int] $Count, [string] $Name, [string] $State, [string] $Detail) {
    if ($script:RocksQuiet) { return }
    $color = switch ($State) { 'OK' { 'Green' } 'installed' { 'Green' } 'installing' { 'Cyan' } default { 'Yellow' } }
    Write-Host ('[{0}/{1}] {2,-16} ' -f $Index, $Count, $Name) -NoNewline
    Write-Host ('{0,-10}' -f $State) -ForegroundColor $color -NoNewline
    Write-Host " $Detail"
}

function Write-RocksText([string] $Text, [string] $Color = '') {
    if ($script:RocksQuiet) { return }
    if ($Color) { Write-Host $Text -ForegroundColor $Color } else { Write-Host $Text }
}

function Invoke-RocksStepCheck($Step) {
    try { return & $Step.Check }
    catch { return New-RocksStatus $false "check failed: $($_.Exception.Message)" }
}

function Invoke-RocksChecks($Steps) {
    $results = @()
    for ($index = 0; $index -lt $Steps.Count; ++$index) {
        $status = Invoke-RocksStepCheck $Steps[$index]
        $state = if ($status.Ok) { 'OK' } else { 'MISSING' }
        Write-RocksLine ($index + 1) $Steps.Count $Steps[$index].Name $state $status.Detail
        if (-not $status.Ok -and $status.Hint) { Write-RocksText "        Next: $($status.Hint)" }
        $results += $status
    }

    return , $results
}

function Write-RocksNextStep {
    Write-RocksText ''
    Write-RocksText 'Setup is complete. Open a new terminal (or restart your IDE) and run:' 'Green'
    Write-RocksText "  cd `"$RocksRoot`""
    Write-RocksText "  $BuildCommand"
}

# Checks every step, asks once (Ask returns the answer), installs only the missing items in order, and checks again. Each step is checked
# once more right before its install, because an earlier install (the IDE, PowerShell 7) may already have provided it. Returns the exit code.
function Invoke-RocksSetup($Steps, [bool] $CheckOnly, [bool] $AssumeYes, [scriptblock] $Ask) {
    $results = Invoke-RocksChecks $Steps
    $pending = @(for ($index = 0; $index -lt $Steps.Count; ++$index) { if (-not $results[$index].Ok) { $index } })
    if ($pending.Count -eq 0) {
        Write-RocksNextStep
        return 0
    }

    if ($CheckOnly) {
        Write-RocksText ''
        Write-RocksText 'Some items are missing. Run Setup.bat without -Check to install them.' 'Yellow'
        return 1
    }

    if (-not $AssumeYes) {
        Write-RocksText ''
        $answer = $null
        try { $answer = & $Ask }
        catch { $answer = $null }
        if ($null -eq $answer) { Write-RocksText 'Cannot ask in this session. Run Setup.bat -Yes to install without asking.'; return 1 }
        if ($answer -match '^\s*n') { Write-RocksText 'Nothing was installed.'; return 1 }
    }

    Write-RocksText ''
    foreach ($index in $pending) {
        $step = $Steps[$index]
        $status = Invoke-RocksStepCheck $step
        if ($status.Ok) {
            Write-RocksLine ($index + 1) $Steps.Count $step.Name 'OK' $status.Detail
            continue
        }

        if (-not $status.Install) {
            $next = if ($status.Hint) { $status.Hint } else { $ManualGuide }
            Write-RocksLine ($index + 1) $Steps.Count $step.Name 'FAILED' "$($status.Detail). Next: $next"
            continue
        }

        Write-RocksLine ($index + 1) $Steps.Count $step.Name 'installing' ''
        try {
            & $status.Install $status.Data
            Write-RocksLine ($index + 1) $Steps.Count $step.Name 'installed' ''
        }
        catch {
            $message = $_.Exception.Message
            if ($message -notmatch 'Next:') { $message += " Next: $(if ($status.Hint) { $status.Hint } else { $ManualGuide })." }
            Write-RocksLine ($index + 1) $Steps.Count $step.Name 'FAILED' $message
        }
    }

    Write-RocksText ''
    Write-RocksText 'Checking again:'
    $results = Invoke-RocksChecks $Steps
    if (@($results | Where-Object { -not $_.Ok }).Count -eq 0) {
        Write-RocksNextStep
        return 0
    }

    Write-RocksText ''
    Write-RocksText 'Some items still need attention (see the lines above). Fix them and run Setup.bat again.' 'Yellow'
    return 1
}

# --- Self test -------------------------------------------------------------------------------------------------------------------------

# A fake step for the main flow tests: State.<Name> says whether it passes, its install sets it (or throws) and counts the call.
# GetNewClosure binds the closure to a new module scope that does not see script-scope functions when the script runs with -Command (as CI
# does), so the closure calls New-RocksStatus through a captured reference.
function New-RocksFakeStep([string] $Name, [hashtable] $State, [bool] $Installable = $true, [bool] $Fails = $false) {
    $newStatus = ${function:New-RocksStatus}
    $check = {
        if ($State[$Name]) { return & $newStatus $true 'fake' }
        $install = $null
        if ($Installable) {
            $install = {
                param($d)
                $d.State['installs'] += @($d.Name)
                if ($d.Fails) { throw 'fake failure' }
                $d.State[$d.Name] = $true
            }
        }

        return & $newStatus $false 'fake missing' $install '' @{ State = $State; Name = $Name; Fails = $Fails }
    }.GetNewClosure()
    return @{ Name = $Name; Check = $check }
}

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

    # The real yvals_core.h lines: 14.52 and 14.44 use _EMIT_STL_ERROR, 14.29 uses #error (Docs/Design/ClangVersionSupport.ko.md 6.2).
    $stl1452 = "#elif defined(__clang__)`n#if __clang_major__ < 22`n_EMIT_STL_ERROR(STL1000, `"Unexpected compiler version, expected Clang 22 or newer.`");`n#endif"
    $stl1429 = "#elif defined(__clang__)`r`n#if __clang_major__ < 11`r`n#error STL1000: Unexpected compiler version, expected Clang 11.0.0 or newer.`r`n#endif"
    Assert-Rocks ((Get-RocksStlClangMinimum $stl1452) -eq 22) 'stl: _EMIT_STL_ERROR form'
    Assert-Rocks ((Get-RocksStlClangMinimum $stl1429) -eq 11) 'stl: #error form'
    Assert-Rocks ($null -eq (Get-RocksStlClangMinimum "#if __clang_major__ < 22`n#define X")) 'stl: other comparison'
    Assert-Rocks ($null -eq (Get-RocksStlClangMinimum "#if __clang_major__ < 22`n_EMIT_STL_ERROR(STL1001, `"x`");")) 'stl: other error'
    # The selected toolset's real header must carry the condition; a format change would otherwise make the check silently pass.
    $instance = Get-RocksVsInstance
    if ($instance) {
        $tools = Get-RocksMsvcToolsDirectory $instance
        $header = if ($tools) { Join-Path $tools 'include\yvals_core.h' } else { '' }
        Assert-Rocks ($header -and (Test-Path -LiteralPath $header) -and $null -ne (Get-RocksStlClangMinimum (Get-Content -LiteralPath $header -Raw))) "stl: condition found in $header"
    }

    Assert-Rocks (((ConvertTo-RocksVsAdd @('A', 'B')) -join ' ') -eq '--add A --add B') 'vs: --add list'
    $ide = @(Get-RocksVsConfigComponents)
    Assert-Rocks (@($VcComponents | Where-Object { $ide -notcontains $_ }).Count -eq 0) 'vs: .vsconfig has every build component (an IDE install also builds)'
    Assert-Rocks ($ide -contains 'Microsoft.VisualStudio.Component.VC.Llvm.Clang' -and $ide -contains 'Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset') 'vs: .vsconfig has the Clang IDE components'
    Assert-Rocks ((Get-RocksVsRange) -match '^\[\d+\.\d+,\)$') 'vs: version range from Build/Toolchain.json'
    $fxc = Get-RocksFxcSource
    Assert-Rocks ($fxc.Path -like "*\bin\$($fxc.Sdk)\x64\d3dcompiler_47.dll" -and $FxcSdkComponent -like "*SDK.$((($fxc.Sdk) -split '\.')[2])") 'fxc: pinned file is the versioned copy of the SDK the wizard installs'
    Assert-Rocks ((Get-RocksToolsetVersion 'C:\x\14.52.37025') -eq [version] '14.52.37025' -and $null -eq (Get-RocksToolsetVersion 'C:\x\latest')) 'vs: toolset directory version'

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

    # Main flow with fake steps: asks once, installs only what is missing, checks each step again before installing it, and returns the
    # exit code of the final check.
    $script:RocksQuiet = $true
    try {
        $state = @{ installs = @(); asks = 0 }
        $ask = { $state['asks'] += 1; return 'y' }.GetNewClosure()
        $state['A'] = $true
        $state['B'] = $true
        $steps = @((New-RocksFakeStep 'A' $state), (New-RocksFakeStep 'B' $state))
        Assert-Rocks ((Invoke-RocksSetup $steps $false $false $ask) -eq 0 -and $state['asks'] -eq 0 -and $state['installs'].Count -eq 0) 'flow: all present, no question, exit 0'

        $state = @{ installs = @(); asks = 0; A = $true }
        $ask = { $state['asks'] += 1; return '' }.GetNewClosure()
        $steps = @((New-RocksFakeStep 'A' $state), (New-RocksFakeStep 'B' $state), (New-RocksFakeStep 'C' $state))
        $code = Invoke-RocksSetup $steps $false $false $ask
        Assert-Rocks ($code -eq 0 -and $state['asks'] -eq 1 -and ($state['installs'] -join ',') -eq 'B,C') "flow: Enter installs only the missing items ($code, $($state['installs'] -join ','))"

        $state = @{ installs = @(); asks = 0 }
        $ask = { $state['asks'] += 1; return 'y' }.GetNewClosure()
        $steps = @((New-RocksFakeStep 'A' $state))
        Assert-Rocks ((Invoke-RocksSetup $steps $true $false $ask) -eq 1 -and $state['asks'] -eq 0 -and $state['installs'].Count -eq 0) 'flow: -Check installs nothing, exit 1'

        $state = @{ installs = @(); asks = 0 }
        $ask = { $state['asks'] += 1; return 'n' }.GetNewClosure()
        $steps = @((New-RocksFakeStep 'A' $state))
        Assert-Rocks ((Invoke-RocksSetup $steps $false $false $ask) -eq 1 -and $state['installs'].Count -eq 0) 'flow: answer n installs nothing, exit 1'

        $state = @{ installs = @(); asks = 0 }
        $ask = { throw 'no console' }
        $steps = @((New-RocksFakeStep 'A' $state))
        Assert-Rocks ((Invoke-RocksSetup $steps $false $false $ask) -eq 1 -and $state['installs'].Count -eq 0) 'flow: no console installs nothing, exit 1'

        $state = @{ installs = @(); asks = 0 }
        $steps = @((New-RocksFakeStep 'A' $state $true $true), (New-RocksFakeStep 'B' $state), (New-RocksFakeStep 'C' $state $false))
        $code = Invoke-RocksSetup $steps $false $true $null
        Assert-Rocks ($code -eq 1 -and ($state['installs'] -join ',') -eq 'A,B' -and $state['B']) 'flow: a failure or a manual item does not stop the others, exit 1'

        # A step that an earlier install provides (the IDE provides the build tools) is checked again and not installed twice.
        $state = @{ installs = @(); asks = 0 }
        $ide = New-RocksFakeStep 'Ide' $state
        $newStatus = ${function:New-RocksStatus}
        $tools = @{ Name = 'Tools'; Check = {
            if ($state['Ide']) { return & $newStatus $true 'from the IDE' }
            return & $newStatus $false 'missing' { param($d) $d.State['installs'] += @('Tools') } '' @{ State = $state }
        }.GetNewClosure() }
        $code = Invoke-RocksSetup @($ide, $tools) $false $true $null
        Assert-Rocks ($code -eq 0 -and ($state['installs'] -join ',') -eq 'Ide') "flow: a step provided by an earlier install is not installed ($($state['installs'] -join ','))"
    }
    finally { $script:RocksQuiet = $false }

    foreach ($failure in $failures) { Write-Host "FAIL $failure" }
    Write-Host "Setup self test: $(if ($failures.Count -eq 0) { 'PASS' } else { 'FAIL' }) ($($failures.Count) failure(s), PowerShell $($PSVersionTable.PSVersion))"
    return $failures.Count
}

# --- Entry -----------------------------------------------------------------------------------------------------------------------------

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

# With -WithIde the IDE comes first, so a new or modified IDE also provides the build tools and Build Tools is not installed next to it.
$steps = @(
    @{ Name = 'Git'; Check = { Get-RocksGitStatus } },
    @{ Name = '.NET SDK'; Check = { Get-RocksDotnetStatus } },
    @{ Name = 'PowerShell 7'; Check = { Get-RocksPwshStatus } })
if ($WithIde) { $steps += @{ Name = 'VS IDE'; Check = { Get-RocksVsIdeStatus } } }
$steps += @(
    @{ Name = 'C++ build tools'; Check = { Get-RocksBuildToolsStatus } },
    @{ Name = 'FXC SDK'; Check = { Get-RocksFxcSdkStatus } },
    @{ Name = 'LLVM'; Check = { Get-RocksLlvmStatus } },
    @{ Name = 'clang-format'; Check = { Get-RocksClangFormatStatus } })
if ($Android) { $steps += @{ Name = 'Android NDK'; Check = { Get-RocksAndroidStatus } } }
$unknown = @($Skip | Where-Object { $name = $_; -not @($steps | Where-Object { $_.Name -eq $name }) })
if ($unknown.Count -gt 0) { Write-Host "Unknown step in -Skip: $($unknown -join ', ')"; exit 2 }
$steps = @($steps | Where-Object { $Skip -notcontains $_.Name })

Write-Host "Rocks setup for Windows ($RocksRoot)"
$ask = { return Read-Host 'Install the missing items? [Y/n]' }
Exit-Rocks (Invoke-RocksSetup $steps ([bool] $Check) ([bool] $Yes) $ask)
