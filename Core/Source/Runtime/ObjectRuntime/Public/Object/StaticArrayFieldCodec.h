#pragma once

#include <Object/MapFieldCodec.h>

// 필드 codec 사슬의 여덟째 조각으로 정적 배열(T x[N]) 필드 codec이다(리플렉션 B9g —
// Docs/Design/ReflectionExtensions.ko.md 8절). wire는 배열과 같다: 단일 값 원소(참조 포함)는
// ValueArray, struct 원소는 RecordList이고 encode는 늘 N개를 쓴다. decode는 count가 N이 아니면
// InvalidArgument다(차원은 배치다 — N을 바꾸면 Version을 올리고 migrate한다, 조용히 자르거나 채우지
// 않는다). 원소는 소유 객체(또는 struct 값) 저장소 안에 있으므로 2단계는 원소 자리에 제자리로 쓴다:
// 단일 값은 대입(String·Soft는 원소의 allocator로 TryAssign), ObjectRef 원소는
// SetStructMemberReference(소유 객체 저장소 안 주소의 SetReference — StructRefArray 원소 안이면
// SetElementReference), struct 원소는 DecodeStructValue다. Registry 컨테이너가 아니므로 GC는 생성
// Trace의 TraceArrayField(정적 배열 오버로드 — Object/ArrayFieldCodec.h)가 원소를 방문한다.
// Object/GeneratedFieldCodec.h로 받는다.
namespace rocks::detail {

// 지금 위치(정적 배열 필드 — context.Field())의 array를 out에 쓰고 wire tag를 돌려준다. 순서:
// MatchesSchema(원소 종류·maxElements == N — 어긋나면 TypeMismatch, struct 원소는
// MatchesStructView도) → EncodeArrayElements(AddElements(N) → writer → 원소마다
// ElementIndexScope::Set(i) 뒤 원소 → End). 원소 검증·Required null 진단([f[i]])·참조 wire는 배열과
// 같다. 원소 수는 고정이므로 길이 상한 진단(ArrayTooLong)이 없다. 호출자가 owner를 encode 고정한
// 상태여야 한다(원소 버퍼가 owner 안이지만 String·경로 원소의 버퍼는 밖이다).
//
// 사용 예(생성 codec이 고른다):
//   return EncodeStaticArrayField(source.*member, out, context);
template <class Member>
Result<uint32> EncodeStaticArrayField(const Member& array, ArchiveWriter& out,
                                      FieldEncodeContext& context) noexcept {
    using Traits = StaticArrayField<Member>;
    using Element = typename Traits::Element;
    static_assert(Traits::Supported);
    static_assert(ValueKindCanBeStaticArrayElement(FieldValueKind<Element>()),
                  "Reflected static array elements must be single values or reflected structs; "
                  "multi-dimensional arrays are not reflected (RMC2307)");
    static_assert(Traits::Count <= MaxStaticArrayElements,
                  "Reflected static arrays hold at most MaxStaticArrayElements elements (RMC2307)");
    if (!MatchesSchema<Member>(context.Field())) {
        return Result<uint32>::Failure(ErrorCode::TypeMismatch);
    }

    if constexpr (ReflectedStructField<Element>::Supported) {
        if (!MatchesStructView<Element>(context)) {
            return Result<uint32>::Failure(ErrorCode::TypeMismatch);
        }
    }

    return EncodeArrayElements<Element>(array, Traits::Count, out, context);
}

// 해석한 단일 값 하나를 원소 자리 element에 쓴다(정적 배열 decode의 관찰·적용 — ObjectRef 원소는
// 호출자가 SetStructMemberReference로 쓴다). String·SoftObjectRef는 원소의 allocator로
// TryAssign하고(실패하면 그 오류 — 2단계면 그래프 rollback) 그 밖은 ParsedScalarValue다.
template <class Element>
Result<void> AssignParsedElement(Element& element, const ParsedValue& parsed) noexcept {
    if constexpr (IsSameV<Element, String>) {
        return element.TryAssign(parsed.text);
    } else if constexpr (SoftReferenceField<Element>::Supported) {
        return element.TryAssign(AssetId{parsed.guid}, parsed.text);
    } else {
        element = ParsedScalarValue<Element>(parsed);
        return Result<void>::Success();
    }
}

// 단일 값·참조 원소 정적 배열의 decode다(DecodeStaticArrayField의 ValueArray 가지). 세 방식은
// 배열과 같다: 검증(target null), 관찰(construction null — 관찰 임시 struct 안, 참조 원소는 오지
// 않는다), 적용(staged 객체 안 — 참조 원소는 SetStructMemberReference). count ≠ N은 원소를 읽기
// 전에 InvalidArgument다.
template <class Element, usize N>
Result<void> DecodeStaticValueArray(ConstructionContext* construction, ObjectHandle owner,
                                    Element (*target)[N], const FieldView& field,
                                    FieldDecodeContext& context) noexcept {
    constexpr bool reference = ObjectReferenceField<Element>::Supported;
    const FieldSchema& schema = context.Field();
    const ValueType& type = schema.element;
    auto elementWire = FieldWireType(type.kind, context.ReferenceWireType());
    if (!elementWire) {
        return Result<void>::Failure(elementWire.Error());
    }

    auto opened = context.OpenValueArray(field, elementWire.Value(), MaxArrayElements,
                                         ElementArrayShape(type.kind));
    if (!opened) {
        return Result<void>::Failure(opened.Error());
    }

    ValueArrayReader& reader = opened.Value();
    if (reader.Count() != N) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto counted = context.AddElements(N); !counted) {
        return counted;
    }

    Result<EnumValueSet> set = Result<EnumValueSet>::Failure(ErrorCode::Unsupported);
    if constexpr (ReflectedEnumField<Element>::Supported) {
        if (auto prepared = PrepareEnumElements(context, context.FieldEnum(), N, set); !prepared) {
            return prepared;
        }
    }

    // Name 원소는 1단계에서 토큰이 없고 weak 원소는 대상 handle이 없으므로 관찰할 수 없다(배열과
    // 같다 — 관찰 struct는 Name·weak 참조를 담지 않는다).
    if constexpr (IsSameV<Element, Name> || WeakReferenceField<Element>::Supported) {
        if (target && context.IsRecording()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    // 참조 원소는 적용(2단계)에서만 쓴다 — 관찰 struct는 강한 참조를 담지 않는다.
    const bool apply = reference ? construction != nullptr && target != nullptr : target != nullptr;
    const EnumValueSet* lookup = set ? &set.Value() : nullptr;
    usize textBytes = 0;
    usize references = 0;
    ElementIndexScope element(context);
    for (usize index = 0; index < N; ++index) {
        element.Set(static_cast<uint32>(index));
        ParsedValue parsed;
        if (auto read = ReadValueWire(reader.Elements(), context, type, context.FieldEnum(), lookup,
                                      StaticReferenceNative<Element>(), parsed);
            !read) {
            return read;
        }

        if constexpr (reference || WeakReferenceField<Element>::Supported) {
            references += parsed.reference.isNull ? 0 : 1;
        }

        if constexpr (ReflectedEnumField<Element>::Supported) {
            if (auto resolved =
                    ResolveUndeclaredEnum(context, *context.FieldEnum(), lookup, parsed);
                !resolved) {
                return resolved;
            }

            if (!GeneratedType<Element>::IsDeclared(parsed.signedInteger)) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        }

        if (auto reported = ReportParsedValue(context, type, parsed); !reported) {
            return reported;
        }

        if constexpr (IsSameV<Element, Name>) {
            if (auto resolved = ResolveParsedName(context, parsed); !resolved) {
                return resolved;
            }
        } else if (!TryAdd(textBytes, parsed.text.ByteCount(), textBytes)) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }

        if (!apply) {
            continue;
        }

        if constexpr (reference) {
            if (auto written = SetStructMemberReference(*construction, context, owner,
                                                        (*target)[index], parsed.reference.handle);
                !written) {
                return written;
            }
        } else if (auto assigned = AssignParsedElement((*target)[index], parsed); !assigned) {
            return assigned;
        }
    }

    if (auto finished = reader.Finish(); !finished) {
        return finished;
    }

    // 2단계 몫은 원소 자리 쓰기 N(원소 메모리는 소유 저장소 안이라 할당하지 않는다) + 문자열·경로
    // byte 2배(값 사본과 원소의 새 버퍼) + 참조 연결 수다(단일 값 필드의 ParseFieldValue와 같은
    // 규칙).
    uint64 applyWork = 0;
    if (!TryMultiply(uint64{textBytes}, uint64{2}, applyWork) ||
        !TryAdd(applyWork, uint64{references}, applyWork) ||
        !TryAdd(applyWork, uint64{N}, applyWork)) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    return context.ReserveApply(applyWork);
}

// struct 원소 정적 배열의 decode다(목록 항목 Record — 버전 범위 (1, UINT32_MAX)). count ≠ N은
// 원소를 읽기 전에 InvalidArgument다. 원소마다 ElementIndexScope::Set(i) 뒤 원소 자리(target 없으면
// 값 없이)에 DecodeStructValue(목록 항목 출처)다. 참조 struct 원소의 참조 멤버는
// SetStructMemberReference로 간다(소유 객체 저장소 안 주소).
template <class Element, usize N>
Result<void> DecodeStaticStructArray(ConstructionContext* construction, ObjectHandle owner,
                                     Element (*target)[N], const FieldView& field,
                                     FieldDecodeContext& context) noexcept {
    const StructView view = context.Current()->structure;
    auto opened = context.OpenRecordList(field, view.Schema().id);
    if (!opened) {
        return Result<void>::Failure(opened.Error());
    }

    RecordListReader& list = opened.Value();
    if (list.ItemCount() != N) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto counted = context.AddElements(N); !counted) {
        return counted;
    }

    ElementIndexScope element(context);
    for (usize index = 0; index < N; ++index) {
        element.Set(static_cast<uint32>(index));
        StructRecordSource source;
        source.list = &list;
        source.index = index;
        if (auto decoded = DecodeStructValue(view, construction, owner,
                                             target ? &(*target)[index] : nullptr, source, context);
            !decoded) {
            return decoded;
        }
    }

    // struct 원소는 소유 저장소 안이라 할당이 없다 — 2단계 몫은 멤버 codec이 원소마다
    // 청구한다(struct 필드와 같다).
    return Result<void>::Success();
}

// InstancedStruct 원소 정적 배열(InstancedStruct<> x[N] — 리플렉션 B9j)의 decode다(원소 tag
// InstancedStruct, u64 길이 + 내용). count ≠ N은 원소를 읽기 전에 InvalidArgument다. 원소는 소유
// 저장소 안이라 2단계는 원소 자리에 제자리로 DecodeInstancedStructElement를 한다(관찰 자리는 오지
// 않는다 — InvalidArgument). 2단계 몫은 원소 자리 쓰기 N이다(값 블록·멤버는 원소의 1과 멤버 codec이
// 덮는다).
template <class Element, usize N>
Result<void> DecodeStaticInstancedArray(ConstructionContext* construction, ObjectHandle owner,
                                        Element (*target)[N], const FieldView& field,
                                        FieldDecodeContext& context) noexcept {
    if (target && !construction) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto opened = context.OpenValueArray(field, static_cast<uint32>(WireType::InstancedStruct),
                                         MaxArrayElements);
    if (!opened) {
        return Result<void>::Failure(opened.Error());
    }

    ValueArrayReader& reader = opened.Value();
    if (reader.Count() != N) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto counted = context.AddElements(N); !counted) {
        return counted;
    }

    ElementIndexScope element(context);
    for (usize index = 0; index < N; ++index) {
        element.Set(static_cast<uint32>(index));
        if (auto decoded = DecodeInstancedStructElement(construction, owner,
                                                        target ? &(*target)[index] : nullptr,
                                                        reader.Elements(), context);
            !decoded) {
            return decoded;
        }
    }

    if (auto finished = reader.Finish(); !finished) {
        return finished;
    }

    return context.ReserveApply(N);
}

// 지금 위치(정적 배열 필드)의 저장값 field를 해석한다. 세 방식(검증·관찰·적용)과 두 단계의 reader
// 호출 순서는 배열과 같다: wire 확인(TypeMismatch) → MatchesSchema(TypeMismatch) → OpenValueArray·
// OpenRecordList → count = N 확인(아니면 InvalidArgument) → AddElements(N) → 원소 → Finish(trailing
// InvalidArgument) → 단일 값 원소면 ReserveApply 한 번(원소 자리 쓰기 N + 문자열 byte 2배 + 참조
// 연결 — 원소 메모리는 할당하지 않으므로 N × sizeof(원소)를 청구하지 않는다, struct 원소는 멤버
// codec이 청구한다). 2단계 실패는 할당과 Registry 상태뿐이다(입력 검증은 모두
// 1단계).
//
// 사용 예(생성 codec이 고른다 — 1단계는 target null):
//   return DecodeStaticArrayField(construction, handle, &(owner->*member), field, context);
template <class Member>
Result<void> DecodeStaticArrayField(ConstructionContext* construction, ObjectHandle owner,
                                    Member* target, const FieldView& field,
                                    FieldDecodeContext& context) noexcept {
    using Traits = StaticArrayField<Member>;
    using Element = typename Traits::Element;
    static_assert(Traits::Supported);
    static_assert(ValueKindCanBeStaticArrayElement(FieldValueKind<Element>()),
                  "Reflected static array elements must be single values or reflected structs; "
                  "multi-dimensional arrays are not reflected (RMC2307)");
    const FieldSchema& schema = context.Field();
    auto wire = FieldWireType(schema, context.ReferenceWireType());
    if (!wire) {
        return Result<void>::Failure(wire.Error());
    }

    if (field.wireType != wire.Value()) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    if (!MatchesSchema<Member>(schema)) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    if constexpr (ReflectedStructField<Element>::Supported) {
        if (!MatchesStructView<Element>(context)) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }

        return DecodeStaticStructArray(construction, owner, target, field, context);
    } else if constexpr (InstancedStructField<Element>::Supported) {
        return DecodeStaticInstancedArray(construction, owner, target, field, context);
    } else {
        return DecodeStaticValueArray(construction, owner, target, field, context);
    }
}

} // namespace rocks::detail
