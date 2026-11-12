#pragma once

#include <Base/Types.h>
#include <Core/Result.h>
#include <Core/Text/StringBuilder.h>
#include <Core/Text/StringView.h>

// Unicode 정규화(NFC·NFD), 단순 case folding, 대소문자 무시 비교 키, General_Category·결합 등급·
// Default_Ignorable 조회다(UE에는 같은 공개 기능이 없다 — ICU 대신 UCD에서 생성한 표를 쓴다).
//
// 언제 쓰나: 에셋 경로처럼 사람이 입력한 이름을 저장·비교할 때. 같은 글자가 정규화 형태만 다르게
// 들어오는 경우(macOS 파일 이름은 NFD, 대부분의 입력은 NFC)와 대소문자 차이를 하나의 키로 모은다.
//
// 사용 예:
//   StringBuilder<128> key(allocator);
//   if (auto made = TryMakeCaselessKey(name, key); !made) { Report(made.Error()); }
//   // "Straße"와 "STRASSE"는 같은 키가 아니다(단순 folding은 ß를 ss로 바꾸지 않는다).
//   // "Å"(U+00C5)와 "Å"는 같은 키다.
//
// 데이터: Unicode 18.0.0 UCD(Core/Source/ThirdParty/UnicodeData)에서 생성한 표
// (Private/Text/UnicodeTables.generated.h). 설계: Docs/Design/CoreDataFoundations.ko.md 1절.
//
// 공통 규칙:
// - 입력은 유효한 UTF-8이어야 한다. 아니면 InvalidArgument다.
// - 입력 상한은 64 MiB다. 넘으면 Overflow다.
// - 결과는 out 뒤에 붙인다. 실패하면 out은 호출 전 내용 그대로다(임시 저장소는 out의 allocator).
namespace rocks {

enum class NormalizationForm : uint8 {
    NFC, // 정준 분해 후 정준 합성(저장·비교의 기본 형태)
    NFD  // 정준 분해만
};

// General_Category 30값(UCD 약어 그대로). 미할당 코드 포인트는 Cn이다.
enum class GeneralCategory : uint8 {
    Lu,
    Ll,
    Lt,
    Lm,
    Lo,
    Mn,
    Mc,
    Me,
    Nd,
    Nl,
    No,
    Pc,
    Pd,
    Ps,
    Pe,
    Pi,
    Pf,
    Po,
    Sm,
    Sc,
    Sk,
    So,
    Zs,
    Zl,
    Zp,
    Cc,
    Cf,
    Cs,
    Co,
    Cn
};

// 표를 만든 Unicode 버전이다("18.0.0").
StringView UnicodeVersionText() noexcept;

// input을 form으로 정규화해 out에 붙인다(UAX #15).
Result<void> TryNormalize(NormalizationForm form, StringView input,
                          StringBuilderBase& out) noexcept;

// input이 이미 form인지 본다. 빠른 검사(quick check)와 결합 등급 순서로 대부분 판정하고, 애매한
// 경우(NFC_QC = Maybe)만 allocator로 정규화해 비교한다. 잘못된 UTF-8은 InvalidArgument, 그 비교의
// 할당 실패는 allocator 오류(OutOfMemory 등)다.
Result<bool> IsNormalized(NormalizationForm form, StringView input, Allocator& allocator) noexcept;

// 단순 case folding(CaseFolding.txt의 C·S)을 적용해 out에 붙인다. 코드 포인트 수는 같고 UTF-8 byte
// 수는 바뀔 수 있다(예: U+023A → U+2C65). 길이가 바뀌는 full folding(ß → ss)과 터키어 규칙은 쓰지
// 않는다.
Result<void> TrySimpleCaseFold(StringView input, StringBuilderBase& out) noexcept;

// 대소문자를 무시하는 비교 키 = NFC(SimpleCaseFold(NFD(input)))를 out에 붙인다(Unicode §3.13 정준
// caseless 일치의 simple folding 판). 멱등이다: 키의 키는 같은 키다.
Result<void> TryMakeCaselessKey(StringView input, StringBuilderBase& out) noexcept;

// 코드 포인트 하나의 속성이다. 0x10FFFF를 넘으면 Cn·0·false다(surrogate는 Cs).
GeneralCategory GetGeneralCategory(char32_t codePoint) noexcept;
uint8 GetCanonicalCombiningClass(char32_t codePoint) noexcept;
bool IsDefaultIgnorable(char32_t codePoint) noexcept;

// 단순 case folding 한 글자(없으면 그대로)다.
char32_t SimpleCaseFold(char32_t codePoint) noexcept;

} // namespace rocks
