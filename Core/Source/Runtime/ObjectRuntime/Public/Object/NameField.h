#pragma once

#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/Name.h>
#include <Core/Text/StringView.h>

// Name 값 종류(ValueKind::Name — 리플렉션 B9c2, Docs/Design/ReflectionExtensions.ko.md 4·4.1절)의
// 내용 규칙과 토큰 검사다. 필드 값은 실행 중 pool 토큰(rocks::Name)이고 저장 형식은 표시
// 문자열(번호 접미 포함 — "Socket_7")이다. None은 빈 문자열로 저장한다(표시 변형 "NONE"도 빈
// 문자열 — 결정적).
//
// pool: 토큰은 객체 Registry의 RegistryOptions::names(Object/ObjectRegistry.h)로 만든 것이어야
// 한다. 기본 None 토큰(Name{})은 pool이 없어도 된다. 쓰기(필드 write·staged write·경로 쓰기)와
// 저장 encode가 이 헤더의 같은 함수로 토큰을 검사하고, 복원 1단계는 표시 문자열을
// ValidateNameFieldText로 검사한 뒤 기록해 2a가 대상 Registry의 pool로 intern한다.
namespace rocks {

class NamePool;

// Name 필드 값의 길이 규칙은 UE 5.8 FName과 같다(Docs/Design/ReflectionExtensions.ko.md 4절·20절
// 결정 4): 번호 접미(`_N` — SplitNameText)를 뗀 base가 UTF-16 코드 단위로 MaxNameBaseUtf16Units
// 이하다. UE는 FName 생성에서 번호를 먼저 떼고(ParseNumberFromName) 남은 길이를 NAME_SIZE(null 포함
// 1,024)와 비교하므로(UnrealNames.cpp FindOrStoreString — `Len >= NAME_SIZE`면 거부) UE 이름이 모두
// 들어가고 UE가 거부하는 이름은 Rocks도 거부한다. 보충 문자(U+10000 이상)는 2단위다.
inline constexpr usize MaxNameBaseUtf16Units = 1023;

// 위 규칙이 허용하는 표시 문자열의 최대 byte다: base 1,023단위의 UTF-8 최대(3 byte 글자 1,023개 —
// 보충 문자는 2단위에 4 byte라 더 짧다) 3,069 byte + `_` + 번호 최대 10자리(2147483646). 내용
// 검사가 UTF-8·NFC 판정 전에 이것으로 긴 입력을 먼저 거른다(이 byte 이하라도 base 단위 규칙을
// 넘으면 거부한다).
inline constexpr usize MaxNameFieldBytes = 3 * MaxNameBaseUtf16Units + 1 + 10;

// text가 Name 필드 값의 내용 규칙을 지키는지 본다. 검사 순서(모두 InvalidArgument): byte 수 >
// MaxNameFieldBytes → 유효 UTF-8 아님 → NUL 있음 → 번호 접미를 뗀 base가 MaxNameBaseUtf16Units
// UTF-16 단위 초과 → NFC 아님(정규화는 하지 않는다 — 입력 계층이 NFC로 만든다). 쓰기 검사·저장
// encode·복원 1단계가 이 함수 하나로 검사한다(복원은 Archive reader가 String의 UTF-8을 먼저 본다 —
// 오류 코드는 같다).
ROCKS_OBJECTRUNTIME_API Result<void> ValidateNameFieldText(StringView text,
                                                           Allocator& scratch) noexcept;

// token을 필드 값으로 쓸 수 있는지 보고, 저장할 표시 문자열을 lease로 돌려준다(저장 encode가 lease
// 동안 View()를 쓴다). None 토큰(기본 None과 번호 없는 None 표시 변형)은 빈 lease(View()가 빈
// 문자열)다.
//
// 검사 순서: 토큰 구조(Name::IsValid가 아니면 InvalidArgument) → 기본 None이면 성공(pool 불필요) →
// pool 없음 Unsupported → pool Acquire(pool이 실행 중이 아니면 ShuttingDown, lease 계수 소진
// Overflow, 찾지 못하면 토큰 domain이 pool의 것이 아닐 때 WrongContext·같을 때 StaleHandle) → 번호
// 없는 None 표시 변형이면 빈 lease → 표시 문자열의 내용 규칙(ValidateNameFieldText —
// InvalidArgument·할당 오류). 성공 경로는 pool gate를 Acquire와 lease 반환에서만 잡는다(domain은
// 찾지 못했을 때만 Stats로 읽는다).
//
// 인자: pool은 대상 Registry의 RegistryOptions::names(없으면 nullptr), scratch는 NFC 판정의 임시
// 할당기다.
// 수명: 성공한 lease는 pool의 lease 규칙을 따른다(pool Shutdown 전에 놓는다). View()는 lease가 사는
// 동안만 쓴다.
//
// 사용 예(저장):
//   auto display = AcquireNameFieldText(context.Names(), value.name, context.Scratch());
//   if (!display) {
//       return Result<void>::Failure(display.Error());
//   }
//
//   return out.WriteString(display.Value().View());
ROCKS_OBJECTRUNTIME_API Result<NameLease> AcquireNameFieldText(NamePool* pool, Name token,
                                                               Allocator& scratch) noexcept;

// token을 Name 필드에 쓸 수 있는지만 본다(AcquireNameFieldText와 같은 검사 — 얻은 lease는 바로
// 놓는다). 필드 write(FieldWriteOperation)·staged write(생성 writeStaged)·경로 쓰기가 값을 게시하기
// 전에 부른다. 오류는 AcquireNameFieldText와 같다.
//
// 사용 예:
//   if (auto valid = ValidateNameFieldToken(registry.Names(), value.name, value.GetAllocator());
//       !valid) {
//       return valid;   // pool 없음 Unsupported, 다른 pool의 토큰 WrongContext 등
//   }
ROCKS_OBJECTRUNTIME_API Result<void> ValidateNameFieldToken(NamePool* pool, Name token,
                                                            Allocator& scratch) noexcept;

// text가 Name으로 None인지다(빈 문자열, 빈 base의 번호 이름 "_7", 번호 없는 "None"의 대소문자
// 변형 — NameTextEquals(text, "")). 복원 2a는 None 문자열을 intern하지 않고 기본 None 토큰으로 둔다
// (저장은 어느 None이든 빈 문자열이므로 결과 byte가 같다). UTF-8을 검사하지 않는다(호출자가 먼저
// ValidateNameFieldText로 검사한다).
//
// 사용 예(복원 2a):
//   if (IsNoneNameText(text)) {
//       token = Name{};   // intern하지 않는다
//   }
ROCKS_OBJECTRUNTIME_API bool IsNoneNameText(StringView text) noexcept;

} // namespace rocks
