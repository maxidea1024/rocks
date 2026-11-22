#pragma once

#include <Core/Result.h>
#include <Meta/TypeSchema.h>

// 텍스트·JSON codec의 용도(TextUse)와 용도별 필드 제외 판정이다(리플렉션 B9i —
// Docs/Design/ReflectionExtensions.ko.md 10.1절). 용도가 출력·입력에서 뺄 필드 플래그를 고른다.
// UE FProperty::ShouldPort(Property.cpp:1153-1182)와 JsonObjectConverter의 기본 SkipFlags를
// 따른다.
//
// | 용도       | 출력 제외                                                      | 입력 제외   |
// |------------|----------------------------------------------------------------|-------------|
// | Edit       | Transient · NonTryoutDuplicateTransient                        | Transient   |
// | Copy       | Transient · NonTryoutDuplicateTransient · DuplicateTransient · | 출력과 같다 |
// |            | TextExportTransient                                            |             |
// | TryoutCopy | Transient · DuplicateTransient · TextExportTransient           | 출력과 같다 |
// | Report     | Transient                                                      | 입력 불가   |
//
// 판정 규칙:
// - 판정에 넘기는 flags는 그 위치의 유효 플래그다(PropagateFieldFlags — 바깥 struct·배열
//   필드의 Transient·ReadOnly·EditorOnly가 안쪽으로 전파된다). 나머지 제외 플래그
//   (DuplicateTransient·NonTryoutDuplicateTransient·TextExportTransient)는 전파되지 않으므로,
//   codec은 경로·순회의 단계마다 판정하고 제외된 단계의 부분 트리(struct 멤버·원소·Map 값)에
//   들어가지 않는다. 그래서 제외된 struct 필드는 멤버까지 통째로 빠진다(9.1절 — 포함 필터와
//   달리 제외는 멤버마다 따로 보지 않는다).
// - EditorOnly·SaveGame·Required는 텍스트 용도와 무관하다(텍스트 codec은 도구·편집기
//   문맥에서 쓴다).
// - Edit 입력은 NonTryoutDuplicateTransient·TextExportTransient 필드를 받는다(인스턴스 고유
//   값도 도구가 고칠 수 있다. UE에서 이 둘의 제외는 텍스트 출력 규칙이고 속성 편집은
//   ShouldPort를 거치지 않는다).
//
// 사용 예(출력 순회의 필드 하나):
//   const uint32 flags = PropagateFieldFlags(parentFlags, field.flags);
//   if (IsTextOutputExcluded(options.use, flags)) {
//       continue;   // 이 필드와 그 아래를 쓰지 않는다
//   }
namespace rocks::reflection_text {

// 텍스트·JSON codec의 용도다. 기본값은 Edit다(TextCodecOptions::use).
enum class TextUse : uint8 {
    // 도구 편집: asset-set·asset-new 입력, asset-info 기본 출력, 설정
    // JSON(TargetProfile·CookSettings).
    Edit,
    // 복사·붙여넣기, 다른 에셋에 값 옮기기(UE PPF_Copy).
    Copy,
    // Tryout 세션의 복사(UE PPF_Copy + PPF_DuplicateForPIE).
    TryoutCopy,
    // 검증 보고·asset-info --all 출력 전용이다. 입력에 쓰면 InvalidArgument다.
    Report
};

// 입력 판정 결과다(ClassifyTextInput).
enum class TextFieldInput : uint8 {
    // 값을 받는다.
    Accept,
    // 용도의 입력 제외 플래그에 걸린다 — 진단 ExcludedProperty로 거부한다(UE는 경고 후 건너뛴다).
    Excluded,
    // 유효 플래그에 ReadOnly가 있다. 살아 있는 객체 대상이면 진단 ReadOnly이고, staged 객체(생성
    // 중) 대상이거나 값이 지금 값과 같으면(무동작, override 경로에도 넣지 않는다) 받는다. 이 판정은
    // 대상과 값을 아는 대입 묶음(PrepareAssignments)이 한다.
    ReadOnly
};

// 정의된 TextUse 값인지 본다(도구가 정수·문자열에서 만든 값을 검사할 때 쓴다).
ROCKS_REFLECTIONTEXT_API bool IsValidTextUse(TextUse use) noexcept;

// 용도의 출력 제외 플래그(FieldFlags 비트의 합)를 돌려준다. 전제: IsValidTextUse(use) — 아니면
// Fatal.
ROCKS_REFLECTIONTEXT_API uint32 TextOutputExcludedFlags(TextUse use) noexcept;

// 용도의 입력 제외 플래그를 돌려준다. 오류: Report(입력 불가)와 정의되지 않은 값은 InvalidArgument.
// 입력 진입점(PrepareAssignments·ImportJson)이 맨 앞에서 불러 용도를 검사한다.
ROCKS_REFLECTIONTEXT_API Result<uint32> TextInputExcludedFlags(TextUse use) noexcept;

// flags(그 위치의 유효 플래그)인 필드를 용도의 출력에서 빼는지 본다. 전제: IsValidTextUse(use).
ROCKS_REFLECTIONTEXT_API bool IsTextOutputExcluded(TextUse use, uint32 flags) noexcept;

// flags(그 위치의 유효 플래그)인 필드에 대한 입력 판정이다. 제외가 ReadOnly보다
// 먼저다(Transient이면서 ReadOnly인 필드는 Excluded). 전제: 입력을 받는
// 용도(TextInputExcludedFlags가 성공하는 값) — Report나 정의되지 않은 값이면 Fatal.
ROCKS_REFLECTIONTEXT_API TextFieldInput ClassifyTextInput(TextUse use, uint32 flags) noexcept;

} // namespace rocks::reflection_text
