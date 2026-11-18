#pragma once

#include <Core/Containers/OrderedSet.h>
#include <ReflectionText/TextValue.h>

// ReflectionText 구현 안에서만 쓰는 도우미다(리플렉션 B9i3b).
namespace rocks::reflection_text::detail {

// 값 텍스트 하나의 해석 실패다(진단 종류와 사람이 읽는 설명 — 정적 문자열).
struct ScalarFailure {
    TextDiagnosticCode code = TextDiagnosticCode::InvalidValue;
    StringView message;
};

// 진단을 하나 남기고 Success(false)를 돌려준다(진단 할당 실패는 그 오류).
inline Result<bool> Report(TextCodecContext& context, TextDiagnosticCode code, TextSource source,
                           StringView path, StringView message) noexcept {
    auto added = context.Diagnostics().Add(code, source, path, message);
    if (!added) {
        return Result<bool>::Failure(added.Error());
    }

    return Result<bool>::Success(false);
}

// 단일 값 텍스트 하나를 out에 읽는다(입력 byte는 세지 않는다 — 호출자가 센다). 반환이 false면
// failure에 진단 종류·설명을 쓰고 진단은 남기지 않는다(호출자가 남긴다 — 경로 key hook은 경로
// 진단 하나로 보고한다).
Result<bool> ParseScalarCore(TextCodecContext& context, const TextScalarType& type, StringView text,
                             FieldValue& out, ScalarFailure& failure) noexcept;

// value를 숨김이 아닌 열거자로 나타낼 수 없는지(숨김 열거자를 썼는지) 본다. 보통 enum은 그 값의
// 열거자가 Hidden이면, Flags enum은 숨김이 아닌 열거자의 OR로 덮을 수 없는 비트가 남으면 참이다.
bool UsesHiddenEnumerator(const EnumSchema& schema, int64 value) noexcept;

// text가 ASCII 대소문자 무시로 word와 같은지다.
bool TextEqualsIgnoringCase(StringView text, StringView word) noexcept;

// 목록 안 키 중복 판정이다(Set 원소·Map 키 — 명령행 목록과 JSON 배열·객체가 같이 쓴다). Name은
// pool 비교 규칙(Name ==), 그 밖은 값의 정규 텍스트(String은 byte 그대로)로 본다.
class KeySet {
public:
    explicit KeySet(Allocator& allocator) noexcept : names_(allocator), texts_(allocator) {
    }

    // key를 더한다. 이미 있으면 false다. 오류: 할당 실패, 정규 텍스트를 쓰지 못함.
    Result<bool> Add(TextCodecContext& context, const TextScalarType& type,
                     const FieldValue& key) noexcept;

private:
    OrderedSet<Name> names_;
    OrderedSet<String> texts_;
};

// 목록 하나의 원소 수 한도를 본다(options.maxElements → 필드 MaxElements → StaticArray는 정확히
// N). 넘으면 진단(OutOfRange·InvalidValue)을 남기고 false다.
Result<bool> CheckElementCount(TextCodecContext& context, const FieldSchema& field, usize count,
                               TextSource source, StringView pathText) noexcept;

// 바이트 배열(`Array<uint8>` 필드·멤버 통째 — Docs/Design/CurveData.ko.md D18, CV4)의 값 텍스트는
// 명령행·JSON 모두 RFC 4648 Base64 문자열 하나다(Core/Encoding/Base64.h — 엄격: 4의 배수 길이·'='
// 패딩·공백 없음, 빈 텍스트는 빈 배열). text를 풀어 out을 원소 목록(UInt8 FieldValue)으로 채운다.
// Base64가 아니면 InvalidValue, 원소 수 한도(CheckElementCount)를 넘으면 그 진단을 남기고 false다.
Result<bool> ParseByteList(TextCodecContext& context, const FieldSchema& field, StringView text,
                           TextSource source, StringView pathText, TextValue& out) noexcept;

// bytes의 Base64 텍스트를 out 끝에 붙인다(빈 배열은 아무것도 붙이지 않는다).
Result<void> AppendByteText(Allocator& allocator, Span<const uint8> bytes, String& out) noexcept;

// Map 키·Set 원소 값(value)의 경로 key wire를 wire 뒤에 붙인다(Name은 names의 표시 문자열을
// allocator로 얻는다). 오류: AppendPathKey·Name 조회의 오류.
Result<void> AppendValueKeyWire(NamePool* names, Allocator& allocator, const FieldValue& value,
                                Array<uint8>& wire) noexcept;

// 경로 단계 하나의 스키마 범위다: 클래스(lease)의 필드나 struct(view)의 멤버.
struct SchemaScope {
    const TypeLease* lease = nullptr;
    StructView view{};

    const TypeSchema& Schema() const noexcept {
        return lease ? lease->Schema() : view.Schema();
    }

    usize FieldIndex(Guid id) const noexcept {
        return lease ? lease->FieldIndex(id) : view.FieldIndex(id);
    }

    const EnumSchema* FieldEnum(usize index) const noexcept {
        return lease ? lease->FieldEnum(index) : view.FieldEnum(index);
    }

    const EnumSchema* FieldKeyEnum(usize index) const noexcept {
        return lease ? lease->FieldKeyEnum(index) : view.FieldKeyEnum(index);
    }

    StructView FieldStruct(usize index) const noexcept {
        return lease ? lease->FieldStruct(index) : view.FieldStruct(index);
    }
};

} // namespace rocks::reflection_text::detail
