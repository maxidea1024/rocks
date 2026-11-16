#!/bin/sh
# 미리 빌드한 바이너리(manifest kind: binary)의 오프라인 도구 빌드 공용 함수다(macOS arm64, POSIX sh).
# 라이브러리별 스크립트(Engine/Source/ThirdParty/<Lib>/Build/Build.sh)가 `. <이 파일>`로 읽어 쓴다. Windows 판(ToolBuild.ps1)과
# 같은 순서다: manifest의 원본을 받아 SHA-256 검사 → 원본 트리·patch → 커밋한 공개 헤더 대조 → (라이브러리 스크립트가 빌드)
# → install name(@loader_path)·ad-hoc 서명·dSYM → tar.gz archive와 호스트 항목 JSON. 게시는 하지 않는다.
#
# 상태: 미검증. 이 기계(Windows)에서 실행하지 못했다. 처음 실행하는 macOS 호스트에서 확인한다(ShaderSystem.ko.md 8절 진척).
# manifest는 macOS 기본 도구 plutil로 읽는다(jq·Python에 기대지 않는다).

set -eu

ROCKS_TOOL_HOST="macos-arm64"

# manifest 값 하나를 읽는다. 예: rocks_manifest "$library" build.source.url
rocks_manifest() {
    plutil -extract "$2" raw -o - "$1/ThirdParty.json"
}

rocks_manifest_count() {
    plutil -extract "$2" raw -o - "$1/ThirdParty.json" 2>/dev/null || echo 0
}

rocks_sha256() {
    shasum -a 256 "$1" | cut -d ' ' -f 1
}

# url sha256 destination downloads: 받아 SHA-256을 검사하고 최상위 디렉터리 하나를 벗겨 destination에 푼다.
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

# library work: 원본 트리를 만들고 patch를 적용한다. 결과 경로는 "$work/source"다.
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

# library source: 저장소에 커밋한 원본 디렉터리(upstreamDirectory)의 모든 파일이 원본 트리의 같은 경로와 byte 단위로 같은지
# 본다(커밋한 파일과 manifest SHA-256의 일치는 정책 검사 third-party가 본다).
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
# dylib의 install name을 @loader_path/<이름>으로, 같은 디렉터리 의존도 @loader_path로 바꾸고 ad-hoc 서명한다.
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

# stage output files...: files는 "<stage 기준 경로>=<role>" 목록이다. archive와 호스트 항목 JSON을 output에 쓴다.
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
