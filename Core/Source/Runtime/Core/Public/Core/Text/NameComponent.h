#pragma once

#include <Base/Types.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/StringBuilder.h>
#include <Core/Text/StringView.h>

// 이름 구성 요소 규칙 검사와 대소문자 무시 비교 키다(Docs/Design/AssetSystem.ko.md 4.2절,
// Docs/Design/ObjectFoundations.ko.md 1.1절). 객체 이름(ObjectRegistry의 Outer 아래 이름)과 A2 논리
// 경로의 구성 요소가 같은 규칙을 쓴다.
//
// 언제 쓰나: 사람이 입력한 이름을 Registry·경로에 넣기 전에 검사할 때, 같은 이름 공간 안의 유일성을
// 대소문자·정규화 형태와 무관하게 비교할 때. 이 함수들은 정규화하지 않는다 — 비 NFC 입력은 거부하고
// 정규화는 입력 계층(경로 정책·명령행)이 한다.
//
// 규칙: UTF-8 NFC, 1–64 byte, 허용 문자는 General_Category L*·N*·M*와 '_'·'-'(첫 문자는 L*·N*),
// Default_Ignorable_Code_Point 거부(L*·M*이어도), Windows 예약 이름(대소문자 무시 CON PRN AUX NUL
// COM0–9 LPT0–9 COM¹²³ LPT¹²³) 거부. 따라서 '.'·'/'·공백은 들어갈 수 없다.
namespace rocks {

// 이름 구성 요소의 byte 상한이다.
inline constexpr usize MaxNameComponentBytes = 64;

// ValidateNameComponent가 거부한 사유다.
enum class NameComponentError : uint8 {
    None,              // 위반 없음(성공 또는 할당 실패)
    Empty,             // 빈 문자열
    TooLong,           // 64 byte 초과
    NotNfc,            // NFC가 아니다(분해형 등)
    InvalidFirst,      // 첫 문자가 L*·N*가 아니다
    InvalidCharacter,  // 허용하지 않는 문자 또는 잘못된 UTF-8
    DefaultIgnorable,  // Default_Ignorable_Code_Point(보이지 않는 문자)
    ReservedDeviceName // Windows 예약 장치 이름
};

// component가 이름 구성 요소 규칙을 지키는지 검사한다. 할당 없는 검사(길이·UTF-8·문자·예약 이름)를
// 먼저 하고 마지막에 NFC를 본다. ASCII만으로 된 이름은 NFC 검사도 할당하지 않으며, 비ASCII 이름도
// NFC 빠른 검사로 판정되면 할당하지 않는다(애매한 결합 문자 구간과, offset을 받을 때의 NotNfc 위치
// 계산만 scratch로 정규화한다).
//
// 인자:
// - component: 검사할 UTF-8 byte 열(호출 동안만 빌린다).
// - scratch: NFC 판정·위치 계산의 임시 저장소. 반환 전에 모두 돌려준다.
// - reason: null이 아니면 결과 사유를 쓴다(성공·할당 실패는 None).
// - offset: null이 아니면 위반 위치(byte)를 쓴다(TooLong은 64, NotNfc는 정규화 결과와 처음 달라지는
//   code point 시작, 잘못된 UTF-8은 유효한 앞부분의 길이, 예약 이름은 0, 성공·할당 실패는 0).
//   null이면 NotNfc 위치를 계산하지 않는다(그 정규화 할당도 없다).
//
// 반환: 규칙 위반은 InvalidArgument, NFC 판정 scratch 할당 실패는 그 오류(OutOfMemory 등)다.
//
// 사용 예:
//   NameComponentError reason{};
//   usize offset = 0;
//   if (auto valid = ValidateNameComponent(name, scratch, &reason, &offset); !valid) {
//       // 도구 계층이 reason·offset으로 InvalidPath 진단을 만든다
//   }
Result<void> ValidateNameComponent(StringView component, Allocator& scratch,
                                   NameComponentError* reason = nullptr,
                                   usize* offset = nullptr) noexcept;

// 이름 구성 요소의 대소문자 무시 비교 키를 out 뒤에 붙인다. 키는 TryMakeCaselessKey와 같다
// (NFC(simple case folding(NFD(x))) — Core/Text/Unicode.h). ASCII만으로 된 이름은 ASCII 소문자화로
// 같은 키를 할당 없이 만든다(out의 용량이 모자랄 때만 out이 늘어난다). 비ASCII 이름은 정규화 임시
// 저장소를 out의 allocator로 할당한다.
//
// component는 ValidateNameComponent를 통과한 이름이어야 한다(그 밖의 입력은 TryMakeCaselessKey 규칙
// 대로 잘못된 UTF-8이면 InvalidArgument다). 빈 이름의 키는 빈 문자열이다. 실패하면 out은 호출 전
// 내용 그대로이고, 할당 실패는 그 오류(OutOfMemory 등)다.
//
// 사용 예:
//   StringBuilder<128> key(allocator);
//   if (auto made = TryMakeNameComponentKey(name, key); !made) {
//       return made;
//   }
//   // "Body"·"BODY"·"body"는 같은 키 "body"다.
Result<void> TryMakeNameComponentKey(StringView component, StringBuilderBase& out) noexcept;

} // namespace rocks
