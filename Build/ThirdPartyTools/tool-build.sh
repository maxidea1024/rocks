#!/bin/sh
# Shared functions for the offline tool build of prebuilt binaries (manifest kind: binary) (macOS arm64, POSIX sh).
# Per-library scripts (Engine/Source/ThirdParty/<Lib>/Build/Build.sh) read this file with `. <this file>`. Same order as the Windows version
# (ToolBuild.ps1): download the manifest sources and verify SHA-256 -> source tree and patches -> compare committed public headers -> (the library script builds)
# -> install name (@loader_path), ad-hoc signing and dSYM -> tar.gz archive and host entry JSON. Does not publish.
#
# Status: unverified. It could not be run on this machine (Windows). Confirm on the first macOS host that runs it (ShaderSystem.ko.md section 8 progress).
# The manifest is read with plutil, a default macOS tool (no reliance on jq or Python).

set -eu

ROCKS_TOOL_HOST="macos-arm64"

# Reads one manifest value. Example: rocks_manifest "$library" build.source.url
rocks_manifest() {
    plutil -extract "$2" raw -o - "$1/ThirdParty.json"
}

rocks_manifest_count() {
    plutil -extract "$2" raw -o - "$1/ThirdParty.json" 2>/dev/null || echo 0
}

rocks_sha256() {
    shasum -a 256 "$1" | cut -d ' ' -f 1
}

# url sha256 destination downloads: downloads, verifies SHA-256, strips the single top-level directory and extracts into destination.
rocks_expand_verified_source() {
    url=$1
    sha=$2
    destination=$3
    downloads=$4
    mkdir -p "$downloads"
    archive="$downloads/$sha.tar.gz"
    if [ ! -f "$archive" ]; then
        curl -fsSL -o "$archive.partial" "$url"
        mv "$archive.partial" "$archive"
    fi

    actual=$(rocks_sha256 "$archive")
    if [ "$actual" != "$sha" ]; then
        rm -f "$archive"
        echo "SHA-256 mismatch for $url: expected $sha, got $actual" >&2
        exit 1
    fi

    rm -rf "$destination.unpack" "$destination"
    mkdir -p "$destination.unpack" "$(dirname "$destination")"
    tar -xzf "$archive" -C "$destination.unpack"
    count=$(find "$destination.unpack" -mindepth 1 -maxdepth 1 | wc -l | tr -d ' ')
    if [ "$count" != "1" ]; then
        echo "$url must contain exactly one top-level directory" >&2
        exit 1
    fi

    mv "$destination.unpack"/* "$destination"
    rm -rf "$destination.unpack"
}

# library work: builds the source tree and applies patches. The result path is "$work/source".
rocks_initialize_source_tree() {
    library=$1
    work=$2
    source="$work/source"
    rocks_expand_verified_source "$(rocks_manifest "$library" build.source.url)" \
        "$(rocks_manifest "$library" build.source.sha256)" "$source" "$work/downloads"
    count=$(rocks_manifest_count "$library" build.dependencies)
    index=0
    while [ "$index" -lt "$count" ]; do
        path=$(rocks_manifest "$library" "build.dependencies.$index.path")
        rocks_expand_verified_source "$(rocks_manifest "$library" "build.dependencies.$index.url")" \
            "$(rocks_manifest "$library" "build.dependencies.$index.sha256")" "$source/$path" "$work/downloads"
        index=$((index + 1))
    done

    count=$(rocks_manifest_count "$library" build.patches)
    index=0
    while [ "$index" -lt "$count" ]; do
        git -C "$source" apply --whitespace=nowarn "$library/$(rocks_manifest "$library" "build.patches.$index")"
        index=$((index + 1))
    done
}

# library source: checks that every file of the source directory committed to the repository (upstreamDirectory) matches the same path in the source tree
# byte for byte (the match between committed files and the manifest SHA-256 is checked by the third-party policy check).
rocks_assert_committed_files() {
    library=$1
    source=$2
    upstream="$library/$(rocks_manifest "$library" upstreamDirectory)"
    (cd "$upstream" && find . -type f | sed 's:^\./::') | while read -r path; do
        if ! cmp -s "$upstream/$path" "$source/$path"; then
            echo "committed file $path differs from the build source" >&2
            exit 1
        fi
    done
}
# Changes a dylib's install name to @loader_path/<name>, and same-directory dependencies to @loader_path, then signs it ad hoc.
rocks_fix_dylib() {
    dylib=$1
    name=$(basename "$dylib")
    install_name_tool -id "@loader_path/$name" "$dylib"
    for dependency in $(otool -L "$dylib" | tail -n +2 | awk '{print $1}'); do
        case "$dependency" in
            @rpath/*) install_name_tool -change "$dependency" "@loader_path/${dependency#@rpath/}" "$dylib" ;;
        esac
    done

    codesign --force --sign - "$dylib"
}

# stage output files...: files is a list of "<path relative to stage>=<role>". Writes the archive and host entry JSON to output.
rocks_tool_archive() {
    library=$1
    stage=$2
    output=$3
    shift 3
    name=$(rocks_manifest "$library" name)
    version=$(rocks_manifest "$library" version)
    revision=$(rocks_manifest "$library" build.revision)
    tag="third-party-$name-$version-r$revision"
    asset="$name-$version-r$revision-$ROCKS_TOOL_HOST.tar.gz"
    mkdir -p "$output"
    archive="$output/$asset"
    staged=$(cd "$stage" && find . -type f | sed 's:^\./::' | LC_ALL=C sort)
    listed=$(for entry in "$@"; do echo "${entry%%=*}"; done | LC_ALL=C sort)
    if [ "$staged" != "$listed" ]; then
        echo "staged files differ from the listed files" >&2
        exit 1
    fi

    rm -f "$archive"
    (cd "$stage" && COPYFILE_DISABLE=1 tar --no-xattrs --no-mac-metadata -czf "$archive" $(ls))
    repository=${GITHUB_REPOSITORY:-$(git -C "$library" remote get-url origin | sed -E 's#^.*github\.com[:/]([^/]+/[^/.]+)(\.git)?$#\1#')}
    {
        echo "{"
        echo "  \"$ROCKS_TOOL_HOST\": {"
        echo "    \"archive\": {"
        echo "      \"url\": \"https://github.com/$repository/releases/download/$tag/$asset\","
        echo "      \"sha256\": \"$(rocks_sha256 "$archive")\","
        echo "      \"size\": $(stat -f %z "$archive")"
        echo "    },"
        echo "    \"files\": ["
        separator=""
        for entry in $(for item in "$@"; do echo "$item"; done | LC_ALL=C sort); do
            path=${entry%%=*}
            role=${entry#*=}
            printf '%s      { "path": "%s", "sha256": "%s", "size": %s, "role": "%s" }' "$separator" "$path" \
                "$(rocks_sha256 "$stage/$path")" "$(stat -f %z "$stage/$path")" "$role"
            separator=",
"
        done
        echo ""
        echo "    ]"
        echo "  }"
        echo "}"
    } >"$archive.host.json"
    echo "archive: $archive"
    echo "sha256:  $(rocks_sha256 "$archive")"
    echo "release: $tag"
}
