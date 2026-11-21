#pragma once

#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/StringBuilder.h>
#include <Core/Text/StringView.h>

// UE 객체 경로와 Rocks 논리 경로의 대응이다(Docs/Design/UnrealExchange.ko.md 9.1절, 10.4절 7·13번).
// `/<Mount>/<경로>/<패키지>.<객체>` → `<root>/<Mount>/<경로>/<패키지>`. 구성 요소마다 NFC 정규화 뒤
// 받지 않는 문자를 `_`로 바꾸고, 첫 문자·Windows 예약 이름이면 `U`를 붙이고, 바뀌었거나 64 byte를
// 넘으면 원래 경로 접두의 SHA-256 앞 8자를 붙인다. 결과는 원래 경로만으로 정해진다(함께 가져오는
// 에셋 집합과 무관).
//
// 사용 예:
//   StringBuilder<256> mapped(allocator);
//   ExchangePathOutcome outcome{};
//   if (auto made = MapUnrealObjectPath("/Game/My Props/SM_Rock.SM_Rock", "/Game/Unreal", mapped,
//                                       outcome);
//       !made) { ... }
//   // mapped = "/Game/Unreal/Game/My_Props-1a2b3c4d/SM_Rock", outcome.renamed = true

namespace rocks {

// 경로 대응 결과다.
struct ExchangePathOutcome {
    // 정리 규칙으로 구성 요소 하나라도 바뀌었으면 참이다(NFC 정규화만의 차이는 바꿈이 아니다).
    bool renamed = false;
    // 실패 이유다.
    enum class Failure : uint8 {
        None,
        InvalidObjectPath, // objectPath 문법 위반(IsValidUnrealObjectPath)
        InvalidRoot,       // root가 논리 경로 규칙을 어김
        TooLong            // 결과가 UnrealPathMaxBytes 초과(UnrealPathTooLong)
    } failure = Failure::None;
};

// objectPath가 10.4절 7번 문법을 지키면 참이다: 앞 `/`, 패키지 경로 구성 요소 2개 이상, 빈 구성
// 요소 없음, `.`은 비지 않은 객체 이름 앞 하나뿐, `:`·`\`·제어 문자 없음, 유효한 UTF-8,
// UnrealObjectPathMaxBytes 이하.
bool IsValidUnrealObjectPath(StringView objectPath) noexcept;

// objectPath의 패키지 경로(`.` 앞)다. 문법을 지키지 않는 입력이면 빈 view다.
StringView UnrealPackagePath(StringView objectPath) noexcept;

// root가 Rocks 논리 경로 규칙(AssetSystem 4.2절 — 앞 `/`, 구성 요소마다 ValidateNameComponent, 끝
// `/` 없음, UnrealPathMaxBytes 이하)을 지키는지 본다. 어기면 InvalidArgument, scratch 할당 실패는
// 그 오류다.
Result<void> ValidateUnrealImportRoot(StringView root, Allocator& scratch) noexcept;

// objectPath의 대응 논리 경로를 out 뒤에 붙인다. 임시 저장소는 out의 allocator다.
//
// 반환: 성공. objectPath·root 문법 위반은 InvalidArgument, 결과가 UnrealPathMaxBytes를 넘으면
// CapacityExceeded(outcome.failure에 이유), 할당 실패는 그 오류다. 실패하면 out은 호출 전 그대로다.
Result<void> MapUnrealObjectPath(StringView objectPath, StringView root, StringBuilderBase& out,
                                 ExchangePathOutcome& outcome) noexcept;

} // namespace rocks
