#pragma once

#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Platform/File.h>

// 작은 파일의 원자 교체 쓰기다(Docs/Design/AssetSystem.ko.md 7.1절 16·17 — 색인 캐시와 별칭 표가
// 함께 쓴다). `<directory>/<fileName>.<36자 소문자 guid>.tmp`에 쓰고 Flush·Close한 뒤 Replace한다.
// 고아 임시 파일 정리(TemporaryFiles)가 이 모양의 이름을 지운다.
namespace rocks::asset_database_detail {

// 교체 결과다. notDurable이면 교체는 보이지만 디렉터리 동기화를 확인하지 못했다(Replace의
// NotDurable — 호출자가 경고로 보고하거나 무시한다).
struct AtomicWriteOutcome {
    bool notDurable = false;
};

// directory를 만들고(없으면) bytes로 fileName을 교체한다. 오류: 디렉터리 만들기·임시 파일 만들기·
// 쓰기·Flush·Close·교체 IO(임시 파일은 지운다 — 대상은 그대로), 임시 이름 발급 실패, 할당 실패.
Result<AtomicWriteOutcome> WriteFileAtomically(platform::FileSystem& files, Allocator& allocator,
                                               StringView directory, StringView fileName,
                                               Span<const uint8> bytes) noexcept;

} // namespace rocks::asset_database_detail
