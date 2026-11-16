#pragma once

#include <Object/FieldValueParse.h>

// 필드 codec 사슬의 셋째 조각으로 struct 값 codec이다(Docs/Design/ReflectionFoundations.ko.md
// 4.6절). struct ID·버전 헤더의 중첩 Record 쓰기(EncodeStructRecord)·읽기(DecodeStructRecord·
// DecodeRecordField·DecodeStructValue·DecodeStructField), struct 멤버 형식 대조
// (MatchesStructView)와 struct 멤버 참조 적용(SetStructMemberReference)이 있다.
// Object/GeneratedFieldCodec.h로 받는다.
namespace rocks::detail {

// struct 값(value)을 struct ID·버전 헤더의 중첩 Record로 out에 제자리로 쓴다(Docs/Design/
// ReflectionFoundations.ko.md 4.6절 — RecordWriter, 값 byte를 한 번만 쓰므로 중첩 깊이만큼 복사가
// 늘지 않는다). 참조를 담지 않는 struct는 먼저 SubstituteStruct hook을 거친다(배열 원소면 지금
// 위치의 index가 원소다). 멤버는 스키마 순서로 FieldScope 안에서 멤버 thunk가 out에 바로 쓰고, 유효
// 플래그가 제외(IsExcluded)인 멤버는 쓰지 않는다. Required 검사 모드는 Required 위치를 품지 않는
// 멤버(CarriesRequired가 거짓)도 쓰지 않는다 — 출력은 버리므로 형식과 무관하고, 검사와 무관한 기준
// 값(예: 생성자의 미선언 enum)의 저장 검증이 검사를 실패시키지 않으며 비용·버퍼가 준다. thunk가 쓴
// wire는 필드 단위
// FieldWireType으로 다시 확인한다(TypeMismatch). 쓴 TLV 수를 AddFields로 그래프 누계에 더한다.
// 작업량은 out의 청구로 센다(호출자가 out의 WorkUsed를 청구한다). 실패하면 out의 내용은 정해지지
// 않는다.
inline Result<uint32> EncodeStructRecord(StructView view, const void* value, ArchiveWriter& out,
                                         FieldEncodeContext& context) noexcept {
    if (view.IsEmpty() || !value) {
        return Result<uint32>::Failure(ErrorCode::Unsupported);
    }

    const TypeSchema& schema = view.Schema();
    if (!view.ContainsStrongReference()) {
        auto substituted = context.SubstituteStruct(schema.id, view.StructOps().nativeType, value);
        if (!substituted) {
            return Result<uint32>::Failure(substituted.Error());
        }

        value = substituted.Value();
    }

    auto record = RecordWriter::Begin(out, schema.id, schema.version, context.NestedRecordLimits());
    if (!record) {
        return Result<uint32>::Failure(record.Error());
    }

    for (usize index = 0; index < schema.fields.Size(); ++index) {
        const FieldSchema& member = schema.fields[index];
        const StructView nested = view.FieldStruct(index);
        if (context.IsRequiredCheck() && !CarriesRequired(member, nested)) {
            continue;
        }

        FieldScope scope(context, member, schema.id, view.FieldEnum(index), nested,
                         FieldCodecFrame::NoIndex, view.FieldKeyEnum(index));
        if (context.IsExcluded(context.EffectiveFlags())) {
            continue;
        }

        const RuntimeStructFieldOps& ops = view.FieldOps(index);
        if (!ops.encode) {
            return Result<uint32>::Failure(ErrorCode::Unsupported);
        }

        if (auto begun = record.Value().BeginField(member.id); !begun) {
            return Result<uint32>::Failure(begun.Error());
        }

        auto wire = ops.encode(ops, value, out, context);
        if (!wire) {
            return wire;
        }

        // thunk가 스키마 필드의 wire를 썼는지 다시 확인한다(배열은 원소에 따라 ValueArray·
        // RecordList).
        auto expected = FieldWireType(member, context.ReferenceWireType());
        if (!expected || expected.Value() != wire.Value()) {
            return Result<uint32>::Failure(ErrorCode::TypeMismatch);
        }

        if (auto ended = record.Value().EndField(wire.Value()); !ended) {
            return Result<uint32>::Failure(ended.Error());
        }
    }

    if (auto ended = record.Value().End(); !ended) {
        return Result<uint32>::Failure(ended.Error());
    }

    if (auto counted = context.AddFields(record.Value().FieldCount()); !counted) {
        return Result<uint32>::Failure(counted.Error());
    }

    return Result<uint32>::Success(static_cast<uint32>(WireType::Record));
}

// 맨 위 Record(context.CurrentRecord())를 struct 값 하나로 해석한다. 저장 필드마다 스키마 위치를
// 찾아(정렬 색인) 멤버 thunk를 부르고, 모르는 필드·저장된 Transient(Unsupported)·excludedFlags
// 멤버(InvalidArgument)는 객체 Record와 같은 규칙이다. Record에 없는 멤버는 그 자리의 값(기준 값 —
// 소유 객체 생성자나 원소 기본 생성자가 만든 값)을 그대로 둔다(누락 = 기준, Docs/Design/
// ObjectFoundations.ko.md 4절). 검증한 멤버마다 MarkPresent를 한 번 부른다(1단계만 — override 집합
// 자리). construction·value는 멤버 thunk의 세 방식(검증·관찰·적용)을 정한다.
inline Result<void> DecodeStructRecord(StructView view, ConstructionContext* construction,
                                       ObjectHandle owner, void* value,
                                       FieldDecodeContext& context) noexcept {
    RecordReader* record = context.CurrentRecord();
    if (view.IsEmpty() || !record) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const TypeSchema& schema = view.Schema();
    uint64 searchWork = 1;
    for (usize count = schema.fields.Size(); count != 0; count >>= 1) {
        ++searchWork;
    }

    // 저장 필드마다 정렬 색인 이진 탐색 한 번이다(빠진 멤버를 도는 일은 없다 — 누락 = 기준).
    uint64 indexWork = 0;
    if (!TryMultiply(uint64{record->FieldCount()}, searchWork, indexWork)) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    if (auto spent = context.Spend(indexWork); !spent) {
        return spent;
    }

    for (usize stored = 0; stored < record->FieldCount(); ++stored) {
        auto field = record->Next();
        if (!field) {
            return Result<void>::Failure(field.Error());
        }

        const usize index = view.FieldIndex(field.Value().id);
        if (index == StructView::NoField) {
            if (auto skipped = SkipUnknownField(context, field.Value().id); !skipped) {
                return skipped;
            }

            continue;
        }

        const FieldSchema& member = schema.fields[index];
        if (HasFlag(member.flags, FieldFlags::Transient)) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        FieldScope scope(context, member, schema.id, view.FieldEnum(index), view.FieldStruct(index),
                         FieldCodecFrame::NoIndex, view.FieldKeyEnum(index));
        // 이 데이터에 없어야 하는 플래그(바깥에서 전파된 것 포함 — 쿠킹 로드의 EditorOnly)의
        // 멤버다.
        if (context.IsExcluded(context.EffectiveFlags())) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        const RuntimeStructFieldOps& ops = view.FieldOps(index);
        if (!ops.decode) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        if (auto decoded = ops.decode(ops, construction, owner, value, field.Value(), context);
            !decoded) {
            return decoded;
        }

        if (auto noted = context.MarkPresent(); !noted) {
            return noted;
        }
    }

    return Result<void>::Success();
}

// 객체 Record의 아는 필드 하나(지금 위치 — context.Field())를 등록 함수표 ops로 해석한다.
// 직렬화기가 필드마다 FieldScope 안에서 부르며, 해석이 성공하면 present 판정 MarkPresent를 한 번
// 부른다(1단계만 — 2단계는 문맥이 기록하지 않아 무동작, A5 override 집합 자리 —
// Docs/Design/ObjectFoundations.ko.md 4절). 실패한 필드는 알리지 않는다. 모르는 필드·저장된
// Transient·excludedFlags 판정은 호출자가 먼저 한다(그런 필드는 여기에 오지 않는다). decode가
// 없으면 Unsupported다.
//
// 사용 예(직렬화기 1단계):
//   FieldScope scope(context, schema.fields[index], schema.id, lease.FieldEnum(index),
//                    lease.FieldStruct(index));
//   if (auto decoded = detail::DecodeRecordField(*ops, nullptr, ObjectHandle{}, view, context);
//       !decoded) {
//       return decoded;
//   }
inline Result<void> DecodeRecordField(const RuntimeFieldOps& ops, ConstructionContext* construction,
                                      ObjectHandle owner, const FieldView& field,
                                      FieldDecodeContext& context) noexcept {
    if (!ops.decode) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    if (auto decoded = ops.decode(ops, construction, owner, field, context); !decoded) {
        return decoded;
    }

    return context.MarkPresent();
}

// struct 값 하나의 저장 출처다: 필드 값(field — 중첩 Record TLV), struct 원소 배열의 목록
// 항목(list·index), 필드 값 안의 Record 내용(record — Optional<값 struct>의 내부, 리플렉션 B9e). 셋
// 중 하나만 쓴다.
struct StructRecordSource {
    const FieldView* field = nullptr;
    RecordListReader* list = nullptr;
    usize index = 0;
    Span<const uint8> record{};
};

// struct 값 하나(중첩 Record — 필드 값·목록 항목·Record 내용)를 해석한다(4.6·5.6절): Record 스택에
// 열고(필드는 OpenRecord, 항목은 OpenListItem, Record 내용은 OpenRecordBytes — 모두 버전 범위 (1,
// UINT32_MAX)) 버전을 판정해 같으면 그대로, 낮으면 원본 Record byte(필드 payload·항목 ItemBytes·
// Record 내용)로 MigrateStruct한 결과를 원본 scope를 닫은 뒤 현재 버전으로만 다시 열고, 높으면
// newerVersion 정책(LoadNewerSkippingUnknown이면 지금 위치 경로의 NewerVersion 진단 — 배열 원소면
// [f[i]])을 따른 뒤 DecodeStructRecord. 1단계에서 관찰 대상 struct면 검증과 같은 한 번의 해석으로
// 값을 채워(value가 있으면 그 자리 — 관찰 중인 조상이나 관찰 배열의 원소, 없으면 ObservationScratch
// 임시 값, 모든 경로에서 소멸) ObserveStruct를 부른다. 마이그레이션 보관과 2단계 커서는 방문
// 순서다.
inline Result<void> DecodeStructValue(StructView view, ConstructionContext* construction,
                                      ObjectHandle owner, void* value, StructRecordSource source,
                                      FieldDecodeContext& context) noexcept {
    const bool nested = source.record.Data() != nullptr;
    const usize sources = (source.field ? 1u : 0u) + (source.list ? 1u : 0u) + (nested ? 1u : 0u);
    if (view.IsEmpty() || sources != 1) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    const TypeSchema& schema = view.Schema();
    const RuntimeStructOps& structOps = view.StructOps();
    // 관찰용 임시 값의 수명이다(모든 경로에서 소멸·반환).
    struct Temporary {
        const RuntimeStructOps* ops = nullptr;
        Allocator* allocator = nullptr;
        void* memory = nullptr;

        ~Temporary() {
            if (memory) {
                ops->destroy(memory);
                if (!allocator->Deallocate(memory)) {
                    platform::Fatal("Struct observation scratch ownership mismatch");
                }
            }
        }
    } temporary;
    const bool observe = construction == nullptr && context.IsObservedStruct(schema.id);
    if (observe && value == nullptr) {
        Allocator& scratch = context.ObservationScratch();
        auto memory = scratch.Allocate(structOps.size, structOps.alignment, 0x4f425356u);
        if (!memory) {
            return Result<void>::Failure(memory.Error());
        }

        structOps.construct(memory.Value(), scratch);
        temporary.ops = &structOps;
        temporary.allocator = &scratch;
        temporary.memory = memory.Value();
        value = memory.Value();
    }

    if (auto spent = context.Spend(1); !spent) {
        return spent;
    }

    auto scope =
        source.list ? context.OpenListItem(*source.list, source.index)
        : nested
            ? context.OpenRecordBytes(source.record, schema.id, 1, NumericLimits<uint32>::Max())
            : context.OpenRecord(*source.field, schema.id, 1, NumericLimits<uint32>::Max());
    if (!scope) {
        return Result<void>::Failure(scope.Error());
    }

    const uint32 stored = context.CurrentRecord()->SchemaVersion();
    Result<void> decoded = Result<void>::Success();
    if (stored < schema.version) {
        // 원본 Record 바이트로 올리고, 원본 scope를 닫은 뒤 결과를 현재 버전으로만 연다.
        Span<const uint8> original;
        if (source.list) {
            auto bytes = source.list->ItemBytes(source.index);
            if (!bytes) {
                return Result<void>::Failure(bytes.Error());
            }

            original = bytes.Value();
        } else if (nested) {
            original = source.record;
        } else {
            original = source.field->payload;
        }

        auto migrated = context.MigrateStruct(schema.id, stored, schema.version, original);
        if (!migrated) {
            return Result<void>::Failure(migrated.Error());
        }

        scope.Value().Close();
        auto reopened = context.OpenMigratedRecord(migrated.Value(), schema.id, schema.version);
        if (!reopened) {
            return Result<void>::Failure(reopened.Error());
        }

        decoded = DecodeStructRecord(view, construction, owner, value, context);
    } else {
        if (stored > schema.version) {
            if (context.Policy().newerVersion != NewerVersionPolicy::LoadNewerSkippingUnknown) {
                return Result<void>::Failure(ErrorCode::Unsupported);
            }

            // 진단 없이 모르는 멤버를 건너뛰지 않는다(직렬화기가 hook 없이 이 정책을 주면 거부).
            if (!context.CanReport()) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }

            if (auto reported = context.Report(FieldDiagnosticKind::NewerVersion, stored);
                !reported) {
                return reported;
            }
        }

        decoded = DecodeStructRecord(view, construction, owner, value, context);
    }

    if (!decoded) {
        return decoded;
    }

    if (observe) {
        return context.ObserveStruct(schema.id, structOps.nativeType, value);
    }

    return Result<void>::Success();
}

// struct 필드 하나(중첩 Record TLV)를 해석한다 — 출처가 필드 값인 DecodeStructValue다.
inline Result<void> DecodeStructField(StructView view, ConstructionContext* construction,
                                      ObjectHandle owner, void* value, const FieldView& field,
                                      FieldDecodeContext& context) noexcept {
    StructRecordSource source;
    source.field = &field;
    return DecodeStructValue(view, construction, owner, value, source, context);
}

// 지금 위치의 등록 struct(배열이면 원소 struct)가 정적 thunk의 C++ struct와 같은지다(ID 대조에
// 더해 native 키로 — 수동 등록의 어긋남이 타입 혼동이 되지 않게).
template <class Member>
bool MatchesStructView(const FieldCodecContext& context) noexcept {
    const FieldCodecFrame* frame = context.Current();
    return frame && !frame->structure.IsEmpty() &&
           frame->structure.StructOps().nativeType == NativeTypeKey<Member>();
}

// 참조 멤버의 적용(2단계) 쓰기의 단일 지점이다(4.3·5.6절). 문맥에 StructRefArray 원소 위치가
// 있으면(StructElementScope 안) 컨테이너 기준 범위 검사의 SetElementReference, 없으면 소유 객체
// 저장소 안 주소의 SetReference다.
template <class T>
Result<void> SetStructMemberReference(ConstructionContext& construction,
                                      FieldDecodeContext& context, ObjectHandle owner,
                                      ObjectRef<T>& field, ObjectHandle target) noexcept {
    if (const detail::StructElementLocation* element = context.StructElement()) {
        return construction.SetElementReference(owner, *element, field, target);
    }

    return construction.SetReference(owner, field, target);
}

} // namespace rocks::detail
