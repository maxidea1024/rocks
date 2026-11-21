#!/bin/sh
# Rocks setup wizard for macOS hosts (Docs/Design/SetupWizard.ko.md). ./Setup.sh at the repository root runs this file.
# It checks every prerequisite, asks once, installs only what is missing or wrong, and checks again. Values that a new terminal needs
# (PATH for the .NET SDK, ROCKS_CLANG_FORMAT, ANDROID_*) go into one marked block of the shell startup file, so running it again
# changes nothing. Linux is not a build host yet: there the script only checks what works today (C# tools, policy and format checks).
#
# Usage: ./Setup.sh [--check] [--yes] [--android]
#   --check      Check only. Exit code 0 when everything passes, 1 otherwise.
#   --yes        Install without asking.
#   --android    Also check and install the pinned Android NDK.
#   --self-test  Run the unit checks of this script and exit.
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
build_command='dotnet msbuild Rocks.proj -t:Test -p:Configuration=Debug -p:RocksTarget=Foundation'
toolchain_root="${ROCKS_TOOLCHAIN_ROOT:-$HOME/Library/Application Support/Rocks/toolchains}"
block_begin='# >>> Rocks setup >>>'
block_end='# <<< Rocks setup <<<'

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
    expect 'json: clang-format version' 'printf "%s" "$(json_version_after "$here/Toolchain.json" clangFormat)" | grep -Eq "^[0-9]+\.[0-9]+\.[0-9]+$"'
    expect 'json: NDK version' 'printf "%s" "$(json_version_after "$here/Toolchain.json" ndk)" | grep -Eq "^[0-9]+\.[0-9]+\.[0-9]+$"'
    expect 'json: Apple clang minimum' 'printf "%s" "$(json_integer "$here/Toolchain.json" appleClang)" | grep -Eq "^[0-9]+$"'
    expect 'json: verified macOS line' 'json_verified_line "$here/Toolchain.json" MacOS | grep -q "^Apple clang version "'

    scratch=$(mktemp -d)
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
case "${SHELL:-}" in
    */zsh) startup="$HOME/.zshrc" ;;
    */bash) startup="$HOME/.bash_profile" ;;
    *) startup="$HOME/.profile" ;;
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

required_sdk=$(json_version_after "$root/global.json" sdk)
format_version=$(json_version_after "$here/Toolchain.json" clangFormat)
ndk_version=$(json_version_after "$here/Toolchain.json" ndk)
apple_minimum=$(json_integer "$here/Toolchain.json" appleClang)
verified_line=$(json_verified_line "$here/Toolchain.json" MacOS)

# Each check sets status (OK or MISSING), detail, and action (name of an install function, empty when there is none) and hint.
check_xcode() {
    action=install_xcode; hint=''
    if ! xcode-select -p >/dev/null 2>&1 || ! xcrun --find clang++ >/dev/null 2>&1; then
        status=MISSING; detail='Xcode command line tools not found'; return
    fi
    compiler=$(command -v clang++ 2>/dev/null || true)
    line=$("${compiler:-clang++}" --version 2>/dev/null | head -n 1)
    major=$(clang_major "$line")
    case "$line" in
        "Apple clang version"*) ;;
        *) status=MISSING; action=''; detail="first clang++ on PATH is '$line' ($compiler)"
           hint='Remove other LLVM directories from PATH so /usr/bin/clang++ (Xcode) comes first.'; return ;;
    esac
    if [ -z "$major" ] || [ "$major" -lt "$apple_minimum" ]; then
        status=MISSING; action=''; detail="$line, needs Apple clang $apple_minimum or newer"; hint='Update Xcode from the App Store.'; return
    fi
    status=OK; action=''
    detail="$line, SDK $(xcrun --show-sdk-version 2>/dev/null)"
    [ "$line" = "$verified_line" ] || detail="$detail (CI verifies '$verified_line')"
}

install_xcode() {
    xcode-select --install >/dev/null 2>&1 || true
    echo 'Finish the Command Line Tools installer window (or install Xcode from the App Store), then run ./Setup.sh again.'
    return 1
}

check_git() {
    action=''; hint='Git comes with the Xcode command line tools.'
    if ! command -v git >/dev/null 2>&1; then status=MISSING; detail='git not found'; return; fi
    status=OK; detail=$(git --version | sed 's/^git version //')
}

check_dotnet() {
    action=install_dotnet; hint=''
    dotnet=$(find_dotnet)
    if [ -z "$dotnet" ]; then status=MISSING; detail="not found (needs $required_sdk band)"; return; fi
    best=''
    for sdk in $("$dotnet" --list-sdks 2>/dev/null | cut -d ' ' -f 1); do
        if sdk_in_band "$sdk" "$required_sdk"; then best=$sdk; fi
    done
    if [ -z "$best" ]; then status=MISSING; detail="needs the $required_sdk band; installed: $("$dotnet" --list-sdks 2>/dev/null | cut -d ' ' -f 1 | tr '\n' ' ')"; return; fi
    status=OK; detail="$best at $(dirname "$dotnet")"
}

install_dotnet() {
    script=$(mktemp)
    curl -fsSL https://dot.net/v1/dotnet-install.sh -o "$script" || { echo 'Could not download dotnet-install.sh. Next: check the network and run ./Setup.sh again.'; return 1; }
    bash "$script" --version "$required_sdk" --install-dir "$HOME/.dotnet" || { rm -f "$script"; echo "dotnet-install.sh failed. Next: install .NET SDK $required_sdk from https://dotnet.microsoft.com/download."; return 1; }
    rm -f "$script"
    block_set "$startup" DOTNET_PATH 'export PATH="$HOME/.dotnet:$PATH"' >/dev/null
}

check_pwsh() {
    action=install_pwsh; hint=''
    if ! command -v pwsh >/dev/null 2>&1; then status=MISSING; detail='not found (Build/*.ps1 install scripts need it)'; return; fi
    status=OK; detail=$(pwsh -NoProfile -NonInteractive -Command '$PSVersionTable.PSVersion.ToString()' 2>/dev/null)
}

install_pwsh() {
    if command -v brew >/dev/null 2>&1; then
        brew install --cask powershell && return 0
        echo 'brew install --cask powershell failed (see its message above).'
        return 1
    fi
    echo 'Homebrew not found. Next: install Homebrew from https://brew.sh (or PowerShell 7 from Microsoft), then run ./Setup.sh again.'
    return 1
}

check_format() {
    action=install_format; hint=''
    configured=$(setting ROCKS_CLANG_FORMAT)
    if [ -z "$configured" ]; then status=MISSING; detail="ROCKS_CLANG_FORMAT is not set (needs clang-format $format_version)"; return; fi
    if [ ! -x "$configured" ]; then status=MISSING; detail="ROCKS_CLANG_FORMAT points to a missing file: $configured"; return; fi
    line=$("$configured" --version 2>/dev/null | head -n 1)
    case "$line" in
        *"clang-format version $format_version"|*"clang-format version $format_version "*) status=OK; detail="$format_version at $configured" ;;
        *) status=MISSING; detail="ROCKS_CLANG_FORMAT is '$line', needs $format_version" ;;
    esac
}

install_format() {
    command -v pwsh >/dev/null 2>&1 || { echo 'PowerShell 7 is required for this step. Next: let Setup install PowerShell 7, then run ./Setup.sh again.'; return 1; }
    pwsh -NoProfile -File "$here/InstallFormatter.ps1" -ToolchainRoot "$toolchain_root" || return 1
    block_set "$startup" ROCKS_CLANG_FORMAT "export ROCKS_CLANG_FORMAT=\"$toolchain_root/clang-format-$format_version/clang-format\"" >/dev/null
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
    command -v pwsh >/dev/null 2>&1 || { echo 'PowerShell 7 is required for this step. Next: let Setup install PowerShell 7, then run ./Setup.sh again.'; return 1; }
    sdk=$(android_sdk)
    ANDROID_HOME="$sdk" pwsh -NoProfile -File "$here/SelectAndroidNdk.ps1" || return 1
    [ -n "${ANDROID_HOME:-}" ] || block_set "$startup" ANDROID_HOME "export ANDROID_HOME=\"$sdk\"" >/dev/null
    block_set "$startup" ANDROID_NDK_HOME "export ANDROID_NDK_HOME=\"$sdk/ndk/$ndk_version\"" >/dev/null
}

# --- Main ---------------------------------------------------------------------------------------------------------------------------------

if [ "$host" = Linux ]; then
    steps='git dotnet format'
    echo "Rocks setup for Linux ($root)"
    echo 'Linux is not a build host yet: C++ builds need Windows or macOS (planned: Docs/Design/BuildTargets.ko.md section 3.7).'
    echo 'Checking what works today: the C# tools and the policy and format checks.'
elif [ "$host" = Darwin ]; then
    steps='xcode git dotnet pwsh format'
    [ "$android" -eq 1 ] && steps="$steps android"
    echo "Rocks setup for macOS ($root)"
else
    echo "Unsupported host: $host. Use Setup.bat on Windows." >&2
    exit 2
fi

count=$(echo $steps | wc -w | tr -d ' ')
name_of() {
    case "$1" in
        xcode) echo 'Xcode tools' ;; git) echo 'Git' ;; dotnet) echo '.NET SDK' ;; pwsh) echo 'PowerShell 7' ;;
        format) echo 'clang-format' ;; android) echo 'Android NDK' ;;
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

if [ "$check_only" -eq 1 ] || [ "$host" = Linux ]; then
    echo
    if [ "$host" = Linux ]; then
        echo 'Install the missing items yourself (Docs/Guides/BuildEnvironment.ko.md section 4); Setup does not install on Linux.'
    else
        echo 'Some items are missing. Run ./Setup.sh without --check to install them.'
    fi
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
    "check_$step"
    if [ -z "$action" ]; then
        line_out "$index" "$step" FAILED "$detail. Next: $hint"
        continue
    fi
    line_out "$index" "$step" installing ''
    if "$action"; then line_out "$index" "$step" installed ''; else line_out "$index" "$step" FAILED 'see the message above'; fi
done

echo
echo 'Checking again:'
# The new values are in the startup file; load the block so this check sees what a new terminal will see.
[ -f "$HOME/.dotnet/dotnet" ] && PATH="$HOME/.dotnet:$PATH"
run_checks
if [ -z "$pending" ]; then next_step; exit 0; fi
echo
echo 'Some items still need attention (see the lines above). Fix them and run ./Setup.sh again.'
exit 1
