#pragma once

#include <AssetDatabase/AssetDatabaseLock.h>
#include <AssetDatabase/ProjectLayout.h>
#include <Core/Containers/Array.h>
#include <Core/Text/String.h>
#include <Platform/File.h>

// 고아 임시 파일 정리다(Docs/Design/AssetSystem.ko.md 7.1절 12·16 — asset-index-rebuild). 저장의
// 임시 파일(Content 아래 `<이름>.rasset.<36자 소문자 guid>.tmp`)과 캐시 쓰기의 임시 파일
// (`Intermediate/AssetIndex.cache.<guid>.tmp`), 별칭 표 쓰기의 임시 파일
// (`Config/AssetAliases.json.<guid>.tmp` — 7.1절 17 A2e1)은 그 쓰기가 끝나지 못하면 남는다. 잠금을
// 쥔 전체 정리가 정확한 모양의 이름만 지운다(링크는 따라가지 않는다).
namespace rocks {

// 지우지 못한 임시 파일 하나다.
struct TemporaryFileFailure {
    explicit TemporaryFileFailure(Allocator& allocator) noexcept : path(allocator) {
    }

    TemporaryFileFailure(TemporaryFileFailure&&) noexcept = default;
    TemporaryFileFailure& operator=(TemporaryFileFailure&&) noexcept = default;

    // 프로젝트 루트 상대 경로(`Content/…`·`Intermediate/…`)다.
    String path;
    ErrorCode error = ErrorCode::None;
};

// layout의 Content 전체와 Intermediate·Config 바로 아래의 고아 임시 파일을 지운다. removed에 지운
// 수를 더하고, 지우지 못한 파일은 failures에 붙인다(정리는 계속한다). 열거할 수 없는 하위
// 디렉터리도 failures다. 없는 디렉터리는 비어 있는 것으로 본다. lock은 이 프로젝트의 잡은 쓰기
// 잠금이어야 한다.
//
// 오류: 잠금이 아니면 InvalidArgument, Content·Intermediate·Config 자신의 열거 실패(없음 밖)는 그
// 파일 오류, 할당 실패.
Result<void> DeleteTemporaryOrphans(platform::FileSystem& files, const ProjectLayout& layout,
                                    const AssetDatabaseLock& lock, usize& removed,
                                    Array<TemporaryFileFailure>& failures) noexcept;

} // namespace rocks
