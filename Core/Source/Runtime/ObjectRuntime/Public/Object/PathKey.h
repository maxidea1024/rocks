#pragma once

#include <Base/Guid.h>
#include <Core/Containers/Array.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Meta/TypeSchema.h>

// 경로 key 단계(리플렉션 B9f3 — Docs/Design/ReflectionExtensions.ko.md 1.5·7.3절)의 키 값이다.
// PathKey(MetaSchema)는 키 종류와 그 종류의 wire payload 정규형 byte다. 이 헤더는 정규형을 만드는
// AppendPathKey와 읽는 ReadPathKey, 둘이 쓰는 비소유 값 PathKeyValue를 둔다. wire 대응은 그래프
// 저장의 ValueArray 원소와 같은 byte이므로(Object/FieldValueWire.h WriteValueWire) 저장한 키와
// 경로 키를 같은 규칙으로 비교할 수 있다. 할당은 AppendPathKey의 out 하나뿐이고 스레드와 무관하다.
namespace rocks {

// 키 값 하나의 비소유 표현이다. kind가 쓰는 칸을 고른다:
//   Bool                 — boolean
//   UInt8–UInt64         — unsignedInteger(좁은 폭은 그 범위 안)
//   Int8–Int64·Enum      — signedInteger(좁은 폭은 그 범위 안 — enum은 열거자 값)
//   Guid·AssetId·ClassReference — guid(ClassRef는 클래스 TypeId, null은 0)
//   Digest256            — digest
//   Utf8String·Name      — text(Name은 표시 문자열 — 토큰이 아니다. None은 저장 wire와 같이 빈
//                          문자열로 쓰고 AppendPathKey는 "None" 등 None 원문도 빈 문자열로 쓴다 —
//                          문자열 경로의 표기는 `None`)
//   SoftObjectReference  — guid(asset)와 text(객체 경로)
// text는 빌린 뷰다(ReadPathKey의 결과는 PathKey wire를 빌린다 — wire보다 오래 쓰지 않는다).
//
// 사용 예:
//   PathKeyValue key;
//   key.kind = ValueKind::UInt32;
//   key.unsignedInteger = 7;
struct PathKeyValue {
    ValueKind kind = ValueKind::Opaque;
    bool boolean = false;
    uint64 unsignedInteger = 0;
    int64 signedInteger = 0;
    Guid guid{};
    Sha256Digest digest{};
    StringView text;
};

// value를 그 종류의 wire 정규형으로 out 끝에 붙인다(out의 allocator로 한 번 할당).
// 오류(실패하면 out은 그대로다):
//   InvalidArgument — kind가 Map 키 종류(성질 표 ValueKindCanBeMapKey)가 아님, String·Name·Soft
//                     경로가 유효 UTF-8이 아님, Soft null 규칙·경로 상한(SoftObjectRef::Validate)
//   Overflow        — 좁은 정수 종류의 범위 밖 값
//   OutOfMemory     — out 할당
// Name의 내용 규칙(NFC·길이)은 보지 않는다 — 조회는 pool에 없으면 없음이고, 삽입이 내용 규칙을
// 검사한다(경로 연산).
//
// 사용 예:
//   Array<uint8> wire(allocator);
//   PathKeyValue key;
//   key.kind = ValueKind::Utf8String;
//   key.text = "metal";
//   if (auto made = AppendPathKey(key, wire); !made) { return made; }
//   const PathKey keys[] = {{key.kind, wire.View()}};
Result<void> AppendPathKey(const PathKeyValue& value, Array<uint8>& out) noexcept;

// key.wire를 key.kind의 정규형으로 읽는다(할당 없음 — text는 key.wire를 빌린다).
// 오류: kind가 Map 키 종류가 아니거나 wire가 정규형이 아니면(길이·Bool 0·1·좁은 정수 범위·길이
// prefix·UTF-8·Digest 길이 32·Soft null 규칙과 경로 상한, Name None이 빈 문자열이 아님)
// InvalidArgument다.
//
// 사용 예:
//   auto value = ReadPathKey(path.keys[0]);
//   if (value && value.Value().kind == ValueKind::Name) { /* value.Value().text */ }
Result<PathKeyValue> ReadPathKey(const PathKey& key) noexcept;

namespace detail {

// value의 wire 정규형 byte 수다(AppendPathKey와 같은 검사·오류).
Result<usize> PathKeyWireBytes(const PathKeyValue& value) noexcept;

// value의 wire 정규형을 out에 쓴다(out은 PathKeyWireBytes(value) byte 이상 — 할당 없음, 문자열 경로
// 해석의 고정 버퍼가 쓴다). 크기가 모자라면 CapacityExceeded이고 out은 그대로다. 반환은 쓴 byte
// 수다.
Result<usize> WritePathKeyWire(const PathKeyValue& value, Span<uint8> out) noexcept;

} // namespace detail

} // namespace rocks
