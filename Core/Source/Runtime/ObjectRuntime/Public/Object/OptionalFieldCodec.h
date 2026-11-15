#pragma once

#include <Object/ArrayFieldCodec.h>

// 필드 codec 사슬의 다섯째 조각으로 Optional 필드 codec이다(리플렉션 B9e — Docs/Design/
// ReflectionExtensions.ko.md 1.2·6절). 쓰기(EncodeOptionalField — 단일 값 내부는 EncodeFieldWire,
// 값 struct 내부는 EncodeOptionalStruct)와 읽기(DecodeOptionalField — 단일 값 내부는
// ParseFieldValue, 값 struct 내부는 DecodeOptionalStruct)가 있다. wire는 Optional(14): present byte
// 0·1, set이면 u32 내부 wire + 내부 payload(값 struct는 남은 byte 전부가 Record 하나)다. 필드가
// Record에 없는 것(누락 = 기준 — 생성자 값이 그대로 남는다)과 unset(present 0)은 다른 값이다.
// Object/GeneratedFieldCodec.h로 받는다.
namespace rocks::detail {

// Optional<값 struct> 필드 값 source를 out에 쓰고 wire Optional을 돌려준다. 지금 위치의 등록
// struct(MatchesStructView가 아니면 TypeMismatch)로 unset이면 present 0, set이면 present 1 + wire
// Record + 내부 struct Record(EncodeStructRecord — 제외 멤버·Required 검사 모드·SubstituteStruct·
// 그래프 필드 누계 규칙이 struct 필드와 같다)를 쓴다. 실패하면 out의 내용은 정해지지 않는다.
template <class S>
Result<uint32> EncodeOptionalStruct(const Optional<S>& source, ArchiveWriter& out,
                                    FieldEncodeContext& context) noexcept {
    static_assert(ReflectedStructField<S>::Supported);
    if (!MatchesStructView<S>(context)) {
        return Result<uint32>::Failure(ErrorCode::TypeMismatch);
    }

    if (!source.HasValue()) {
        if (auto written = OptionalWireWriter::WriteUnset(out); !written) {
            return Result<uint32>::Failure(written.Error());
        }

        return Result<uint32>::Success(static_cast<uint32>(WireType::Optional));
    }

    auto set = OptionalWireWriter::BeginSet(out, WireType::Record);
    if (!set) {
        return Result<uint32>::Failure(set.Error());
    }

    auto wire = EncodeStructRecord(context.Current()->structure, &source.Value(), out, context);
    if (!wire) {
        return wire;
    }

    if (wire.Value() != static_cast<uint32>(WireType::Record)) {
        return Result<uint32>::Failure(ErrorCode::TypeMismatch);
    }

    if (auto ended = set.Value().End(); !ended) {
        return Result<uint32>::Failure(ended.Error());
    }

    return Result<uint32>::Success(static_cast<uint32>(WireType::Optional));
}

// 지금 위치(context.Field() — Optional 필드·멤버)의 Optional<T> 값 source를 out에 쓰고 wire
// Optional을 돌려준다(호출자가 MatchesSchema를 확인했다). 값 struct 내부는 EncodeOptionalStruct,
// 단일 값 내부는 FieldValue(kind = 내부 종류, present)로 옮겨 EncodeFieldWire가 쓴다(String은
// 복사하지 않고 view로 넘긴다 — owner 밖 버퍼는 encode 고정 또는 Required 검사 중 표시가 지킨다,
// 값 검증·Name 표시 문자열·enum 선언 값 규칙은 단일 값 필드와 같다).
template <class T>
Result<uint32> EncodeOptionalField(const Optional<T>& source, ArchiveWriter& out,
                                   FieldEncodeContext& context) noexcept {
    if constexpr (ReflectedStructField<T>::Supported) {
        return EncodeOptionalStruct(source, out, context);
    } else {
        constexpr ValueKind kind = FieldValueKind<T>();
        static_assert(ValueKindCanBeOptionalInner(kind), "unsupported Optional inner type");
        FieldValue value(context.Scratch());
        StringView text;
        value.kind = kind;
        value.present = source.HasValue();
        if (source.HasValue()) {
            if constexpr (IsSameV<T, String>) {
                text = source.Value().View();
            } else if (auto copied = CopyTargetValue<kind>(source.Value(), value); !copied) {
                return Result<uint32>::Failure(copied.Error());
            }
        }

        return EncodeFieldWire(out, context, value, text, nullptr);
    }
}

// Optional<값 struct> 필드 하나(지금 위치 — context.Field())를 해석한다. target·construction이
// 멤버 thunk의 세 방식을 정한다: 검증(target null — 값에 손대지 않는다), 관찰(construction null·
// target 있음 — 관찰 임시 값 안), 적용(둘 다 있음 — owner 객체 안). 순서: wire 종류(Optional이
// 아니면 TypeMismatch) → 등록 struct 대조(MatchesStructView, TypeMismatch) → OptionalWireReader
// (내부 wire Record — present byte·내부 tag·Record 하나 검사) → 2단계 몫 1 청구 → unset이면 적용·
// 관찰은 옛 값을 지역으로 옮긴 뒤 비운다(게시 뒤 소멸) → set이면 적용·관찰 자리가 unset일 때 기본
// 값 S(Allocator&) 또는 S()를 먼저 만들고(적용은 ConstructionContext::ObjectAllocator, 관찰은
// ObservationScratch — 그래서 Record에 없는 멤버는 기본 값이고, 생성자가 이미 set으로 만든 자리는
// 그 값이 기준이다) 내부 Record를 OpenRecordBytes로 열어 DecodeStructValue(버전·마이그레이션·
// newerVersion·관찰 규칙이 struct 필드와 같다)로 해석한다. 2단계가 실패하는 것은 할당(기본 값의
// 멤버·String 복사)뿐이다(그 로드는 rollback된다).
template <class S>
Result<void> DecodeOptionalStruct(ConstructionContext* construction, ObjectHandle owner,
                                  Optional<S>* target, const FieldView& field,
                                  FieldDecodeContext& context) noexcept {
    static_assert(ReflectedStructField<S>::Supported);
    if (field.wireType != static_cast<uint32>(WireType::Optional)) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    if (!MatchesStructView<S>(context)) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    auto opened = context.OpenValue(field);
    if (!opened) {
        return Result<void>::Failure(opened.Error());
    }

    auto reader = OptionalWireReader::Open(Move(opened.Value()), WireType::Record);
    if (!reader) {
        return Result<void>::Failure(reader.Error());
    }

    if (auto reserved = context.ReserveApply(1); !reserved) {
        return reserved;
    }

    if (!reader.Value().IsSet()) {
        if (target) {
            // 옛 값은 이 함수를 나갈 때 소멸한다(비운 상태를 먼저 게시한다).
            Optional<S> previous(Move(*target));
            target->Reset();
        }

        return Result<void>::Success();
    }

    S* value = nullptr;
    if (target) {
        if (!target->HasValue()) {
            Allocator& allocator =
                construction ? construction->ObjectAllocator() : context.ObservationScratch();
            target->Emplace(MakeAllocatedValue<S>(allocator));
        }

        value = &target->Value();
    }

    StructRecordSource source;
    source.record = reader.Value().RecordBytes();
    if (auto decoded = DecodeStructValue(context.Current()->structure, construction, owner, value,
                                         source, context);
        !decoded) {
        return decoded;
    }

    return reader.Value().Finish();
}

// Optional 필드 하나(지금 위치 — context.Field(), 호출자가 MatchesSchema를 확인했다)를 해석한다.
// 방식(검증·관찰·적용)은 DecodeOptionalStruct와 같다. 값 struct 내부는 DecodeOptionalStruct, 단일
// 값 내부는 ParseFieldValue(Optional wire 검사 → set이면 내부 값 검증·진단·2단계 몫)로 읽고 정적
// enum 선언도 확인한 뒤(2단계가 입력 때문에 실패하지 않게) 관찰·적용 자리에 AssignOptionalValue로
// 쓴다(unset 자리의 새 String은 적용이면 ConstructionContext::ObjectAllocator, 관찰이면
// ObservationScratch). Name 내부는 1단계 관찰 자리에서 InvalidArgument다(관찰 struct는 Name을 담지
// 않는다 — 1단계에는 토큰이 없다). 2단계 Name 토큰은 2a가 intern한 것이라 다시 검사하지 않는다.
//
// 사용 예(생성 decode thunk 2단계):
//   auto owner = construction->GetForWrite<Mesh>(handle);
//   return DecodeOptionalField(construction, handle, &owner.Value()->ratio, field, context);
template <class T>
Result<void> DecodeOptionalField(ConstructionContext* construction, ObjectHandle owner,
                                 Optional<T>* target, const FieldView& field,
                                 FieldDecodeContext& context) noexcept {
    if constexpr (ReflectedStructField<T>::Supported) {
        return DecodeOptionalStruct(construction, owner, target, field, context);
    } else {
        const bool observe = construction == nullptr && target != nullptr;
        if constexpr (IsSameV<T, Name>) {
            if (observe) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        }

        // 관찰 방식의 문자열 사본은 1단계에만 있는 일이라 ObservationScratch로 한 번만 청구한다.
        FieldValue value(observe ? context.ObservationScratch() : context.Scratch());
        FieldValue* apply = target || ReflectedEnumField<T>::Supported ? &value : nullptr;
        if (auto parsed = ParseFieldValue(field, context, nullptr, apply); !parsed) {
            return parsed;
        }

        // 1단계는 등록 enum 스키마로 판정하므로 정적 enum(직접 쓰기의 기준)으로도 확인한다.
        if constexpr (ReflectedEnumField<T>::Supported) {
            if (value.present && !GeneratedType<T>::IsDeclared(value.signedInteger)) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        }

        if (!target) {
            return Result<void>::Success();
        }

        Allocator& allocator =
            construction ? construction->ObjectAllocator() : context.ObservationScratch();
        return AssignOptionalValue(*target, value, allocator);
    }
}

} // namespace rocks::detail
