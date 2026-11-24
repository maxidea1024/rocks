#pragma once

#include <AssetDatabase/AssetService.h>
#include <Core/Containers/Array.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>
#include <ReflectionText/TextAssignments.h>

// 임포트 명령의 예약 인자 번역이다(Docs/Design/AssetSystem 13절·12.12절 — `asset-import`의
// `--cube=`·`--array=`·`--cube-array=`·`--volume=`·`--cube-layout=`·`--volume-tile=`과 원본 하나의
// 기본 임포터 선택). 명령 층(AssetCommandlets)은 문법만 알고, 뜻은 제품 호스트가 건 번역기(텍스처 —
// TextureTools TranslateTextureImportArguments)가 정한다.
namespace rocks {

// 번역기가 받는 예약 인자 이름이다(ImportReservedNames의 부분집합 — 그 밖 예약 이름은 명령이 사용법
// 오류로 거부한다).
inline constexpr StringView ImportTranslatedNames[] = {"cube",   "array",       "cube-array",
                                                       "volume", "cube-layout", "volume-tile"};

// 예약 인자 하나다(`--<name>=<value>` — name은 ImportTranslatedNames 하나, 소문자 그대로).
struct AssetImportReservedArgument {
    StringView name{};
    StringView value{};
};

// 번역 결과다. 문자열은 strings가 소유하거나 명령 인자·입력 원본을 가리킨다(명령 동안 유효).
struct AssetImportTranslation {
    explicit AssetImportTranslation(Allocator& allocator) noexcept
        : sources(allocator), options(allocator), strings(allocator) {
    }

    // 고른 임포터다(비면 명령이 준 것 — 없으면 확장자 기본).
    StringView importer{};
    // 원본이다(비면 명령이 준 원본 그대로). 상대 경로는 명령이 작업 디렉터리 기준으로 푼다.
    Array<AssetImportSourceInput> sources;
    // 옵션 대입이다(명령의 설정·옵션 대입 앞에 둔다 — 값 텍스트는 리플렉션 표기).
    Array<reflection_text::TextAssignment> options;
    // 위 값이 가리키는 소유 문자열이다(옮겨도 byte가 그대로인 String — 원소를 지우지 않는다).
    Array<String> strings;
};

// 번역기다. 예약 인자가 있거나, 예약 인자 없이 원본 하나(역할 DefaultImportSourceRole)를 주고
// `--importer=`가 없을 때 명령이 부른다. reserved는 명령 줄 순서, sources는 명령이 준 원본(위치
// 인자 하나는 역할 source — 절대 경로로 푼 뒤), importer는 `--importer=` 값(없으면 빈 값)이다.
// 반환: 사용법 위반이면 false(명령이 사용법을 쓰고 종료 코드 2), 아니면 true(바꿀 것이 없으면 out을
// 비워 둔다). 오류: 할당 실패.
struct AssetImportTranslator {
    Result<bool> (*translate)(void* state, Allocator& allocator,
                              Span<const AssetImportReservedArgument> reserved,
                              Span<const AssetImportSourceInput> sources, StringView importer,
                              AssetImportTranslation& out) noexcept = nullptr;
    void* state = nullptr;
};

} // namespace rocks
