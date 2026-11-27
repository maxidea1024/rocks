#!/bin/sh
# Rocks setup wizard for macOS hosts (Docs/Design/SetupWizard.ko.md). ./Setup.sh at the repository root runs this file.
# It checks every prerequisite, asks once, installs only what is missing or wrong, and checks again. Values that a new terminal needs
# (PATH for the .NET SDK and PowerShell 7, ROCKS_CLANG_FORMAT, ANDROID_*) go into one marked block of the shell startup file, so running it again
# changes nothing. Downloads are the pinned files of Build/Toolchain.json (URL and SHA-256 or SHA-512), the same entries that
# Build/InstallFormatter.ps1 reads. Linux is not a C++ build host yet: there the script installs only what works today (the .NET SDK and
# clang-format for the C# tools and the policy and format checks).
#
# Usage: ./Setup.sh [--check] [--yes] [--android]
#   --check      Check only. Exit code 0 when everything passes, 1 otherwise.
#   --yes        Install without asking.
#   --android    Also check and install the pinned Android NDK (Build/SelectAndroidNdk.ps1 runs with the PowerShell 7 of the default steps).
#   --self-test  Run the unit checks of this script and exit.
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
build_command='dotnet msbuild Rocks.proj -t:Test -p:Configuration=Debug -p:RocksTarget=Foundation'
block_begin='# >>> Rocks setup >>>'
block_end='# <<< Rocks setup <<<'
network_hint='check the network or proxy (HTTPS_PROXY) and run ./Setup.sh again'
manual_guide='follow appendix B of Docs/Guides/BuildEnvironment.ko.md (manual install)'

check_only=0
assume_yes=0
android=0
self_test=0
for argument in "$@"; do
    case "$argument" in
        --check) check_only=1 ;;
        --yes) assume_yes=1 ;;
        --android) android=1 ;;
        --self-test) self_test=1 ;;
        *) echo "Unknown option: $argument (use --check, --yes, --android)" >&2; exit 2 ;;
    esac
done

# --- Pure helpers (covered by --self-test) ------------------------------------------------------------------------------------------------

# First "version" string after the line that contains the anchor key. Build/Toolchain.json and global.json keep one key per line.
json_version_after() {
    awk -v anchor="\"$2\"" 'index($0, anchor) { found = 1 } found && /"version"/ {
        sub(/.*"version"[ \t]*:[ \t]*"/, ""); sub(/".*/, ""); print; exit }' "$1"
}

# Integer value of a key such as "appleClang": 17.
json_integer() {
    sed -n "s/.*\"$2\"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p" "$1" | head -n 1
}

# First verified --version line of a platform: "MacOS": ["Apple clang version 17.0.0 (clang-1700.0.13.5)"].
json_verified_line() {
    sed -n "s/.*\"$2\"[[:space:]]*:[[:space:]]*\[[[:space:]]*\"\([^\"]*\)\".*/\1/p" "$1" | head -n 1
}

# "url hash" of a one-line download entry: "<key>": { "url": "...", "<hash field>": "..." }. The first entry with the key wins: clangFormat
# and ispc share platform keys (macos-Arm64, ...), so clangFormat must stay above ispc in Build/Toolchain.json (the self test checks that
# those keys still give clang-format wheels).
json_download() {
    sed -n "s/.*\"$2\"[[:space:]]*:[[:space:]]*{[[:space:]]*\"url\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\"[[:space:]]*,[[:space:]]*\"$3\"[[:space:]]*:[[:space:]]*\"\([0-9a-f]*\)\".*/\1 \2/p" "$1" | head -n 1
}

# 0 when the SDK version is in the band global.json asks for (same major.minor and feature band, patch at least the pinned one).
sdk_in_band() {
    candidate=$1
    required=$2
    case "$candidate" in *[!0-9.]*|'') return 1 ;; esac
    c_major=${candidate%%.*}; c_rest=${candidate#*.}; c_minor=${c_rest%%.*}; c_patch=${c_rest#*.}
    r_major=${required%%.*}; r_rest=${required#*.}; r_minor=${r_rest%%.*}; r_patch=${r_rest#*.}
    case "$c_patch" in *.*|'') return 1 ;; esac
    [ "$c_major" -eq "$r_major" ] && [ "$c_minor" -eq "$r_minor" ] && [ $((c_patch / 100)) -eq $((r_patch / 100)) ] && [ "$c_patch" -ge "$r_patch" ]
}

# Major version from a Clang --version first line, or empty.
clang_major() {
    printf '%s\n' "$1" | sed -n 's/.*clang version \([0-9][0-9]*\)\.[0-9][0-9]*\.[0-9][0-9]*.*/\1/p' | head -n 1
}

# Hex digest of a file: file_digest 256|512 <file> (shasum on macOS, sha256sum/sha512sum on Linux).
file_digest() {
    if command -v shasum >/dev/null 2>&1; then
        shasum -a "$1" "$2" | cut -d ' ' -f 1
    else
        "sha$1sum" "$2" | cut -d ' ' -f 1
    fi
}

# Downloads a URL to a file and checks its digest. Prints the next step and returns 1 on failure.
download_verified() {
    url=$1; bits=$2; expected=$3; target=$4
    if ! curl -fsSL "$url" -o "$target"; then echo "Could not download $url. Next: $network_hint."; return 1; fi
    actual=$(file_digest "$bits" "$target")
    if [ "$actual" != "$expected" ]; then
        rm -f "$target"
        echo "SHA-$bits mismatch for $url (got $actual). Next: $network_hint; if it repeats, the pinned entry in Build/Toolchain.json is wrong."
        return 1
    fi
}

# Writes "export NAME=value" (key NAME) into the marked block of the file, replacing an earlier line with the same key. Prints "changed"
# when the file was rewritten and nothing when it already held that line.
block_set() {
    file=$1
    key=$2
    line="$3 # rocks:$key"
    [ -f "$file" ] || : > "$file"
    temporary="$file.rocks-setup.$$"
    awk -v begin="$block_begin" -v end="$block_end" -v key="# rocks:$key" -v line="$line" '
        $0 == begin { inside = 1; print; next }
        inside && $0 == end { if (!written) print line; written = 1; inside = 0; print; next }
        inside && index($0, key) == length($0) - length(key) + 1 && index($0, key) > 0 { if (!written) print line; written = 1; next }
        { print }
        END { if (!written) { print begin; print line; print end } }' "$file" > "$temporary"
    if cmp -s "$file" "$temporary"; then
        rm -f "$temporary"
    else
        mv "$temporary" "$file"
        echo changed
    fi
}

# Value of "export NAME=..." written by block_set, without quotes, or empty.
block_get() {
    [ -f "$1" ] || return 0
    sed -n "s/^export $2=\"\(.*\)\" # rocks:$2\$/\1/p" "$1" | head -n 1
}

# --- Self test ----------------------------------------------------------------------------------------------------------------------------

if [ "$self_test" -eq 1 ]; then
    failures=0
    expect() {
        if ! eval "$2"; then
            echo "FAIL $1"
            failures=$((failures + 1))
        fi
    }

    table="$here/Toolchain.json"
    expect 'sdk band: exact' 'sdk_in_band 10.0.302 10.0.302'
    expect 'sdk band: later patch' 'sdk_in_band 10.0.399 10.0.302'
    expect 'sdk band: older patch' '! sdk_in_band 10.0.301 10.0.302'
    expect 'sdk band: next feature band' '! sdk_in_band 10.0.404 10.0.302'
    expect 'sdk band: other major' '! sdk_in_band 9.0.302 10.0.302'
    expect 'sdk band: prerelease' '! sdk_in_band 10.0.302-preview.1 10.0.302'
    expect 'clang major: Apple' '[ "$(clang_major "Apple clang version 17.0.0 (clang-1700.0.13.5)")" = 17 ]'
    expect 'clang major: LLVM' '[ "$(clang_major "clang version 22.1.8 (https://github.com/llvm/llvm-project ca7933e)")" = 22 ]'
    expect 'clang major: not clang' '[ -z "$(clang_major "gcc version 14")" ]'
    expect 'json: global.json sdk version' 'printf "%s" "$(json_version_after "$root/global.json" sdk)" | grep -Eq "^[0-9]+\.[0-9]+\.[0-9]+$"'
    expect 'json: pinned .NET SDK equals global.json' '[ "$(json_version_after "$table" dotnet)" = "$(json_version_after "$root/global.json" sdk)" ]'
    expect 'json: clang-format version' 'printf "%s" "$(json_version_after "$table" clangFormat)" | grep -Eq "^[0-9]+\.[0-9]+\.[0-9]+$"'
    expect 'json: NDK version' 'printf "%s" "$(json_version_after "$table" ndk)" | grep -Eq "^[0-9]+\.[0-9]+\.[0-9]+$"'
    expect 'json: ISPC version' 'printf "%s" "$(json_version_after "$table" ispc)" | grep -Eq "^[0-9]+\.[0-9]+\.[0-9]+$"'
    expect 'json: Apple clang minimum' 'printf "%s" "$(json_integer "$table" appleClang)" | grep -Eq "^[0-9]+$"'
    expect 'json: verified macOS line' 'json_verified_line "$table" MacOS | grep -q "^Apple clang version "'
    for key in macos-Arm64 macos-X64 linux-X64; do
        expect "json: clang-format wheel $key" 'json_download "$table" '"$key"' sha256 | grep -Eq "^https://[^ ]*/clang_format-$(json_version_after "$table" clangFormat)-[^ ]*\.whl [0-9a-f]{64}$"'
    done
    for key in powershell-osx-arm64 powershell-osx-x64; do
        expect "json: PowerShell archive $key" 'json_download "$table" '"$key"' sha256 | grep -Eq "^https://[^ ]*/powershell-$(json_version_after "$table" powershell)-'"${key#powershell-}"'\.tar\.gz [0-9a-f]{64}$"'
    done
    for key in osx-arm64 osx-x64 linux-x64; do
        expect "json: .NET SDK archive $key" 'json_download "$table" '"$key"' sha512 | grep -Eq "^https://[^ ]*/dotnet-sdk-$(json_version_after "$table" dotnet)-'"$key"'\.tar\.gz [0-9a-f]{128}$"'
    done

    scratch=$(mktemp -d)
    printf 'abc' > "$scratch/file"
    expect 'digest: SHA-256' '[ "$(file_digest 256 "$scratch/file")" = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad ]'
    expect 'download: bad URL fails with a next step' 'download_verified "file:///nonexistent/rocks" 256 00 "$scratch/x" 2>/dev/null | grep -q "Next:"'
    rc="$scratch/rc"
    printf 'export EXISTING=1\n' > "$rc"
    expect 'block: first write changes the file' '[ "$(block_set "$rc" ROCKS_CLANG_FORMAT "export ROCKS_CLANG_FORMAT=\"/a b/clang-format\"")" = changed ]'
    expect 'block: same write changes nothing' '[ -z "$(block_set "$rc" ROCKS_CLANG_FORMAT "export ROCKS_CLANG_FORMAT=\"/a b/clang-format\"")" ]'
    expect 'block: value reads back' '[ "$(block_get "$rc" ROCKS_CLANG_FORMAT)" = "/a b/clang-format" ]'
    expect 'block: new value replaces the line' '[ "$(block_set "$rc" ROCKS_CLANG_FORMAT "export ROCKS_CLANG_FORMAT=\"/c/clang-format\"")" = changed ]'
    expect 'block: second key joins the block' '[ "$(block_set "$rc" ANDROID_NDK_HOME "export ANDROID_NDK_HOME=\"/ndk\"")" = changed ]'
    expect 'block: one block, two lines, user line kept' '[ "$(grep -c "^# >>> Rocks setup >>>$" "$rc")" = 1 ] && [ "$(grep -c "# rocks:" "$rc")" = 2 ] && grep -q "^export EXISTING=1$" "$rc"'
    expect 'block: replaced value reads back' '[ "$(block_get "$rc" ROCKS_CLANG_FORMAT)" = "/c/clang-format" ]'
    rm -rf "$scratch"

    if [ "$failures" -eq 0 ]; then echo 'Setup self test: PASS (0 failure(s))'; exit 0; fi
    echo "Setup self test: FAIL ($failures failure(s))"
    exit 1
fi

# --- Host and environment -----------------------------------------------------------------------------------------------------------------

host=$(uname -s)
machine=$(uname -m)
case "$host" in
    Darwin)
        toolchain_root="${ROCKS_TOOLCHAIN_ROOT:-$HOME/Library/Application Support/Rocks/toolchains}"
        case "$machine" in arm64) format_key=macos-Arm64; dotnet_key=osx-arm64 ;; *) format_key=macos-X64; dotnet_key=osx-x64 ;; esac
        pwsh_key=powershell-$dotnet_key
        case "${SHELL:-}" in */zsh) startup="$HOME/.zshrc" ;; */bash) startup="$HOME/.bash_profile" ;; *) startup="$HOME/.profile" ;; esac
        ;;
    Linux)
        toolchain_root="${ROCKS_TOOLCHAIN_ROOT:-${XDG_DATA_HOME:-$HOME/.local/share}/rocks/toolchains}"
        format_key=linux-X64; dotnet_key=linux-x64
        [ "$machine" = x86_64 ] || { format_key=''; dotnet_key=''; }
        case "${SHELL:-}" in */zsh) startup="$HOME/.zshrc" ;; */bash) startup="$HOME/.bashrc" ;; *) startup="$HOME/.profile" ;; esac
        ;;
    *)
        echo "Unsupported host: $host. Use Setup.bat on Windows." >&2
        exit 2
        ;;
esac

# A new terminal sees the current environment plus the marked block of the startup file.
setting() {
    value=$(eval "printf '%s' \"\${$1:-}\"")
    [ -n "$value" ] || value=$(block_get "$startup" "$1")
    printf '%s' "$value"
}

find_dotnet() {
    if [ -x "$HOME/.dotnet/dotnet" ]; then printf '%s' "$HOME/.dotnet/dotnet"; return; fi
    command -v dotnet 2>/dev/null || true
}

table="$here/Toolchain.json"
required_sdk=$(json_version_after "$root/global.json" sdk)
format_version=$(json_version_after "$table" clangFormat)
ndk_version=$(json_version_after "$table" ndk)
ispc_version=$(json_version_after "$table" ispc)
apple_minimum=$(json_integer "$table" appleClang)
verified_line=$(json_verified_line "$table" MacOS)

# Each check sets status (OK or MISSING), detail, action (name of an install function, empty when there is none) and hint.
check_xcode() {
    action=install_xcode; hint=''
    if ! xcode-select -p >/dev/null 2>&1 || ! xcrun --find clang++ >/dev/null 2>&1; then
        status=MISSING; detail='Command Line Tools not found'; return
    fi
    compiler=$(command -v clang++ 2>/dev/null || true)
    line=$("${compiler:-clang++}" --version 2>/dev/null | head -n 1)
    major=$(clang_major "$line")
    case "$line" in
        "Apple clang version"*) ;;
        *) status=MISSING; action=''; detail="first clang++ on PATH is '$line' ($compiler)"
           hint='Remove other LLVM directories from PATH so /usr/bin/clang++ comes first.'; return ;;
    esac
    if [ -z "$major" ] || [ "$major" -lt "$apple_minimum" ]; then
        status=MISSING; action=''; detail="$line, needs Apple clang $apple_minimum or newer"
        hint='Update the Command Line Tools (softwareupdate --list) or Xcode from the App Store.'; return
    fi
    status=OK; action=''
    detail="$line, SDK $(xcrun --show-sdk-version 2>/dev/null)"
    [ "$line" = "$verified_line" ] || detail="$detail (CI verifies '$verified_line'; builds print an unverified-toolchain notice)"
}

install_xcode() {
    xcode-select --install >/dev/null 2>&1 || true
    echo 'Next: finish the Command Line Tools installer window, then run ./Setup.sh again.'
    return 1
}

check_git() {
    action=''
    if [ "$host" = Darwin ]; then hint='Git comes with the Command Line Tools.'; else hint='Install Git with the package manager (for example sudo apt install git).'; fi
    if ! command -v git >/dev/null 2>&1; then status=MISSING; detail='git not found'; return; fi
    status=OK; hint=''; detail=$(git --version | sed 's/^git version //')
}

check_dotnet() {
    action=install_dotnet; hint=''
    [ -n "$dotnet_key" ] || { action=''; hint="Install .NET SDK $required_sdk from https://dotnet.microsoft.com/download (no pinned archive for $machine)."; }
    dotnet=$(find_dotnet)
    if [ -z "$dotnet" ]; then status=MISSING; detail="not found (needs the $required_sdk band)"; return; fi
    best=''
    for sdk in $("$dotnet" --list-sdks 2>/dev/null | cut -d ' ' -f 1); do
        if sdk_in_band "$sdk" "$required_sdk"; then best=$sdk; fi
    done
    if [ -z "$best" ]; then status=MISSING; detail="needs the $required_sdk band; installed: $("$dotnet" --list-sdks 2>/dev/null | cut -d ' ' -f 1 | tr '\n' ' ')"; return; fi
    status=OK; detail="$best at $(dirname "$dotnet")"
}

install_dotnet() {
    pinned=$(json_version_after "$table" dotnet)
    if [ "$pinned" != "$required_sdk" ]; then echo "Build/Toolchain.json dotnet.sdk.version ($pinned) differs from global.json ($required_sdk). Next: fix Build/Toolchain.json."; return 1; fi
    entry=$(json_download "$table" "$dotnet_key" sha512)
    [ -n "$entry" ] || { echo "No pinned .NET SDK archive for $dotnet_key in Build/Toolchain.json. Next: $manual_guide."; return 1; }
    archive=$(mktemp)
    download_verified "${entry% *}" 512 "${entry#* }" "$archive" || return 1
    mkdir -p "$HOME/.dotnet" && tar -xzf "$archive" -C "$HOME/.dotnet" || { rm -f "$archive"; echo "Could not unpack the .NET SDK into ~/.dotnet. Next: check free disk space and run ./Setup.sh again."; return 1; }
    rm -f "$archive"
    block_set "$startup" DOTNET_PATH 'export PATH="$HOME/.dotnet:$PATH"' >/dev/null
}

# PowerShell 7 runs Build/Tools.ps1 (Rocks.proj BootstrapTools prepares the build tools with it, Docs/Design/ProjectFiles.ko.md section 7.2)
# and Build/SelectAndroidNdk.ps1, so every macOS build needs it.
pwsh_directory() {
    printf '%s' "$toolchain_root/powershell-$(json_version_after "$table" powershell)"
}

check_pwsh() {
    action=install_pwsh; hint=''
    candidate=$(command -v pwsh 2>/dev/null || true)
    [ -n "$candidate" ] || { [ -x "$(pwsh_directory)/pwsh" ] && candidate="$(pwsh_directory)/pwsh"; }
    if [ -z "$candidate" ]; then status=MISSING; detail='not found (Rocks.proj prepares the build tools with Build/Tools.ps1)'; return; fi
    version=$("$candidate" -NoProfile -NonInteractive -Command '$PSVersionTable.PSVersion.Major' 2>/dev/null)
    case "$version" in
        7|[89]|[1-9][0-9]) status=OK; detail=$("$candidate" -NoProfile -NonInteractive -Command '$PSVersionTable.PSVersion.ToString()' 2>/dev/null) ;;
        *) status=MISSING; detail="$candidate is not PowerShell 7 or later" ;;
    esac
}

# Unpacks the pinned PowerShell archive (Build/Toolchain.json, SHA-256 checked) and puts it on PATH through the startup block.
install_pwsh() {
    entry=$(json_download "$table" "$pwsh_key" sha256)
    [ -n "$entry" ] || { echo "No pinned PowerShell archive for $pwsh_key in Build/Toolchain.json. Next: $manual_guide."; return 1; }
    archive=$(mktemp)
    download_verified "${entry% *}" 256 "${entry#* }" "$archive" || return 1
    directory=$(pwsh_directory)
    mkdir -p "$directory" && tar -xzf "$archive" -C "$directory" && chmod +x "$directory/pwsh"
    result=$?
    rm -f "$archive"
    [ "$result" -eq 0 ] || { echo "Could not unpack PowerShell into $directory. Next: check that the directory is writable and run ./Setup.sh again."; return 1; }
    block_set "$startup" PWSH_PATH "export PATH=\"$directory:\$PATH\"" >/dev/null
    PATH="$directory:$PATH"
}

check_format() {
    action=install_format; hint=''
    [ -n "$format_key" ] || { action=''; hint="No pinned clang-format for $machine; set ROCKS_CLANG_FORMAT to a clang-format $format_version binary."; }
    configured=$(setting ROCKS_CLANG_FORMAT)
    if [ -z "$configured" ]; then status=MISSING; detail="ROCKS_CLANG_FORMAT is not set (needs clang-format $format_version)"; return; fi
    if [ ! -x "$configured" ]; then status=MISSING; detail="ROCKS_CLANG_FORMAT points to a missing file: $configured"; return; fi
    line=$("$configured" --version 2>/dev/null | head -n 1)
    case "$line" in
        *"clang-format version $format_version"|*"clang-format version $format_version "*) status=OK; detail="$format_version at $configured" ;;
        *) status=MISSING; detail="ROCKS_CLANG_FORMAT is '$line', needs $format_version" ;;
    esac
}

# Extracts only the native binary from the pinned wheel (the same file Build/InstallFormatter.ps1 installs); no package code runs.
install_format() {
    command -v unzip >/dev/null 2>&1 || { echo 'unzip not found. Next: install unzip (for example sudo apt install unzip) and run ./Setup.sh again.'; return 1; }
    entry=$(json_download "$table" "$format_key" sha256)
    [ -n "$entry" ] || { echo "No pinned clang-format wheel for $format_key in Build/Toolchain.json. Next: $manual_guide."; return 1; }
    wheel=$(mktemp)
    download_verified "${entry% *}" 256 "${entry#* }" "$wheel" || return 1
    directory="$toolchain_root/clang-format-$format_version"
    mkdir -p "$directory" && unzip -p "$wheel" clang_format/data/bin/clang-format > "$directory/clang-format" && chmod +x "$directory/clang-format"
    result=$?
    rm -f "$wheel"
    [ "$result" -eq 0 ] || { echo "Could not unpack clang-format into $directory. Next: check that the directory is writable and run ./Setup.sh again."; return 1; }
    "$directory/clang-format" --version | grep -q "clang-format version $format_version" || { echo "The unpacked clang-format is not $format_version. Next: $manual_guide."; return 1; }
    block_set "$startup" ROCKS_CLANG_FORMAT "export ROCKS_CLANG_FORMAT=\"$directory/clang-format\"" >/dev/null
}

# ISPC compiles the ISPC sources of the texture compressor (Docs/Design/BuildTargets section 3.12, BD45); builds find it through ROCKS_ISPC.
check_ispc() {
    action=install_ispc; hint=''
    configured=$(setting ROCKS_ISPC)
    if [ -z "$configured" ]; then status=MISSING; detail="ROCKS_ISPC is not set (needs ISPC $ispc_version)"; return; fi
    if [ ! -x "$configured" ]; then status=MISSING; detail="ROCKS_ISPC points to a missing file: $configured"; return; fi
    line=$("$configured" --version 2>/dev/null | head -n 1)
    case "$line" in
        *"(Intel(r) ISPC), $ispc_version"|*"(Intel(r) ISPC), $ispc_version "*) status=OK; detail="$ispc_version at $configured" ;;
        *) status=MISSING; detail="ROCKS_ISPC is '$line', needs $ispc_version" ;;
    esac
}

# Build/InstallIspc.ps1 checks the pinned archive (Build/Toolchain.json ispc.packages) and extracts only the executable.
install_ispc() {
    command -v pwsh >/dev/null 2>&1 || { echo 'Next: install PowerShell 7 first (the step above), then run ./Setup.sh again.'; return 1; }
    pwsh -NoProfile -File "$here/InstallIspc.ps1" -ToolchainRoot "$toolchain_root" >/dev/null || { echo "Build/InstallIspc.ps1 failed. Next: read its message above; for download errors $network_hint."; return 1; }
    block_set "$startup" ROCKS_ISPC "export ROCKS_ISPC=\"$toolchain_root/ispc-$ispc_version/ispc\"" >/dev/null
}

android_sdk() {
    sdk=$(setting ANDROID_HOME)
    [ -n "$sdk" ] || sdk=$(setting ANDROID_SDK_ROOT)
    [ -n "$sdk" ] || { [ -d "$HOME/Library/Android/sdk" ] && sdk="$HOME/Library/Android/sdk"; }
    printf '%s' "$sdk"
}

check_android() {
    action=install_android; hint=''
    sdk=$(android_sdk)
    if [ -z "$sdk" ]; then
        status=MISSING; action=''; detail='Android SDK not found'
        hint='Install Android Studio (it creates ~/Library/Android/sdk), then run ./Setup.sh --android again.'; return
    fi
    ndk="$sdk/ndk/$ndk_version"
    revision=$(sed -n 's/^[[:space:]]*Pkg\.Revision[[:space:]]*=[[:space:]]*//p' "$ndk/source.properties" 2>/dev/null | head -n 1 | tr -d '\r ')
    if [ "$revision" != "$ndk_version" ]; then status=MISSING; detail="NDK $ndk_version is not installed in $sdk"; return; fi
    if [ "$(setting ANDROID_NDK_HOME)" != "$ndk" ]; then status=MISSING; detail="ANDROID_NDK_HOME is '$(setting ANDROID_NDK_HOME)', should be $ndk"; return; fi
    status=OK; detail="NDK $ndk_version at $ndk"
}

install_android() {
    command -v pwsh >/dev/null 2>&1 || { echo 'Next: install PowerShell 7 first (the step above), then run ./Setup.sh --android again.'; return 1; }
    sdk=$(android_sdk)
    ANDROID_HOME="$sdk" pwsh -NoProfile -File "$here/SelectAndroidNdk.ps1" || { echo "Build/SelectAndroidNdk.ps1 failed. Next: read its message above; for download errors $network_hint."; return 1; }
    [ -n "${ANDROID_HOME:-}" ] || block_set "$startup" ANDROID_HOME "export ANDROID_HOME=\"$sdk\"" >/dev/null
    block_set "$startup" ANDROID_NDK_HOME "export ANDROID_NDK_HOME=\"$sdk/ndk/$ndk_version\"" >/dev/null
}

# --- Main ---------------------------------------------------------------------------------------------------------------------------------

if [ "$host" = Linux ]; then
    steps='git dotnet format'
    echo "Rocks setup for Linux ($root)"
    echo 'Linux is not a C++ build host yet (planned: Docs/Design/BuildTargets section 3.7). Setup prepares the C# tools and the'
    echo 'policy and format checks (Docs/Guides/BuildEnvironment.ko.md section 5).'
else
    steps='xcode git dotnet pwsh format ispc'
    [ "$android" -eq 1 ] && steps="$steps android"
    echo "Rocks setup for macOS ($root)"
fi

count=$(echo $steps | wc -w | tr -d ' ')
name_of() {
    case "$1" in
        xcode) echo 'Xcode tools' ;; git) echo 'Git' ;; dotnet) echo '.NET SDK' ;; pwsh) echo 'PowerShell 7' ;;
        format) echo 'clang-format' ;; ispc) echo 'ISPC' ;; android) echo 'Android NDK' ;;
    esac
}

line_out() {
    printf '[%s/%s] %-16s %-10s %s\n' "$1" "$count" "$(name_of "$2")" "$3" "$4"
}

pending=''
run_checks() {
    pending=''
    index=0
    for step in $steps; do
        index=$((index + 1))
        "check_$step"
        line_out "$index" "$step" "$status" "$detail"
        if [ "$status" != OK ]; then
            pending="$pending $step"
            [ -z "$hint" ] || echo "        Next: $hint"
        fi
    done
}

next_step() {
    echo
    echo 'Setup is complete. Open a new terminal (or restart your IDE) and run:'
    echo "  cd \"$root\""
    if [ "$host" = Linux ]; then
        echo '  dotnet build Programs/Rocks.Tools.csproj'
        echo '  dotnet out/dotnet/bin/Rocks.ToolTests/Debug/net10.0/Rocks.ToolTests.dll policy --root .'
    else
        echo "  $build_command"
    fi
}

run_checks
if [ -z "$pending" ]; then next_step; exit 0; fi

if [ "$check_only" -eq 1 ]; then
    echo
    echo 'Some items are missing. Run ./Setup.sh without --check to install them.'
    exit 1
fi

if [ "$assume_yes" -ne 1 ]; then
    echo
    if [ ! -t 0 ]; then echo 'Cannot ask in this session. Run ./Setup.sh --yes to install without asking.'; exit 1; fi
    printf 'Install the missing items? [Y/n] '
    read -r answer || answer=n
    case "$answer" in [Nn]*) echo 'Nothing was installed.'; exit 1 ;; esac
fi

echo
index=0
for step in $steps; do
    index=$((index + 1))
    case " $pending " in *" $step "*) ;; *) continue ;; esac
    # Check again: an earlier install may already have provided this item.
    "check_$step"
    if [ "$status" = OK ]; then line_out "$index" "$step" OK "$detail"; continue; fi
    if [ -z "$action" ]; then
        line_out "$index" "$step" FAILED "$detail. Next: ${hint:-$manual_guide}"
        continue
    fi
    line_out "$index" "$step" installing ''
    # Install output streams as it comes (sdkmanager, Homebrew); every failure path of an install function prints its own Next: line.
    if "$action"; then
        line_out "$index" "$step" installed ''
    else
        line_out "$index" "$step" FAILED "see the Next: line above (or $manual_guide)"
    fi
done

echo
echo 'Checking again:'
# The new values are in the startup file; this check reads that block too (setting), and the new SDK directory goes on PATH here.
[ -x "$HOME/.dotnet/dotnet" ] && PATH="$HOME/.dotnet:$PATH"
run_checks
if [ -z "$pending" ]; then next_step; exit 0; fi
echo
echo 'Some items still need attention (see the lines above). Fix them and run ./Setup.sh again.'
exit 1
