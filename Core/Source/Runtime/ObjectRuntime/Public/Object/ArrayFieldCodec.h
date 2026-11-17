#pragma once

#include <Base/TypeTraits.h>
#include <Object/InstancedStructFieldCodec.h>

// 필드 codec 사슬의 다섯째 조각으로 배열 필드 codec이다(Docs/Design/ReflectionFoundations.ko.md
// 5.6절). 원소 wire 준비(ElementWireValue·ElementArrayShape·PrepareEnumElements), RecordList
// 한도(ArrayRecordListLimits), 쓰기(EncodeArrayField), 원소 종류별 읽기(DecodeValueArray·
// DecodeReferenceArray·DecodeStructArray)와 그 분기(DecodeArrayField), 생성 Trace 도우미
// (TraceArrayField — 정적 배열 오버로드 포함)가 있다. 배열·정적 배열 공용 원소 쓰기는
// EncodeArrayElements다. Object/GeneratedFieldCodec.h로 받는다.
namespace rocks::detail {

// 배열 원소 하나를 WriteValueWire 입력(value·text)으로 옮긴다. String·경로는 복사하지 않고 view로
// 둔다(원소 버퍼는 encode 고정이 지킨다).
template <class Element>
Result<void> ElementWireValue(const Element& element, FieldValue& value,
                              StringView& text) noexcept {
    value.kind = FieldValueKind<Element>();
    if constexpr (IsSameV<Element, String>) {
        text = element.View();
        return Result<void>::Success();
    } else if constexpr (SoftReferenceField<Element>::Supported) {
        value.guid = element.asset.value;
        text = element.objectPath.View();
        return Result<void>::Success();
    } else {
        return CopyFieldValue(element, value);
    }
}

// 단일 값 원소의 schema 최소 크기 shape다(Digest는 길이 prefix 8 + 32 byte exact, 그 밖은 wire
// 크기).
inline WireValueShape ElementArrayShape(ValueKind kind) noexcept {
    if (kind == ValueKind::Digest256) {
        return {8 + Sha256Digest::Bytes, true};
    }

    return {};
}

// enum 원소 배열의 판정 집합을 준비한다: 원소가 있으면 EnumValueSet을 만들어 set에 두고 count ×
// 조회 작업을 원소를 다루기 전에 Spend한다. 빈 배열은 만들지도 청구하지도 않는다(저장·로드 두
// 단계가 같은 조건 — set은 실패 상태로 남는다). enumeration이 없으면 Unsupported다.
inline Result<void> PrepareEnumElements(FieldCodecContext& context, const EnumSchema* enumeration,
                                        usize count, Result<EnumValueSet>& set) noexcept {
    if (!enumeration) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    if (count == 0) {
        return Result<void>::Success();
    }

    set = EnumValueSet::Build(*enumeration, context);
    if (!set) {
        return Result<void>::Failure(set.Error());
    }

    uint64 lookups = 0;
    if (!TryMultiply(uint64{count}, set.Value().LookupWork(), lookups)) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    return context.Spend(lookups);
}

// struct 원소 배열(RecordList)의 목록 한도다(5.6절): 항목 수 MaxArrayElements, payload는 필드 값
// 한도 valueBytes, 항목 하나는 그 − 12(count 4 + 길이 8). 저장(EncodeArrayField — valueBytes =
// NestedRecordLimits의 전체 크기)과 직렬화기의 로드(graph_detail::ArrayLoadLimits —
// maxValueBytes)가 이 함수 하나로 같은 값을 만든다.
inline RecordListLimits ArrayRecordListLimits(usize valueBytes) noexcept {
    RecordListLimits limits;
    limits.maxItems = MaxArrayElements;
    limits.maxPayloadBytes = valueBytes;
    limits.maxItemBytes = valueBytes > 12 ? valueBytes - 12 : 0;
    return limits;
}

// 참조 원소의 handle이다(ObjectRefArray의 원소 읽기는 ObjectHandle, 정적 배열은 ObjectRef<T>).
inline ObjectHandle ElementReferenceHandle(ObjectHandle handle) noexcept {
    return handle;
}

template <class T>
ObjectHandle ElementReferenceHandle(const ObjectRef<T>& reference) noexcept {
    return reference.Handle();
}

template <class Element, class Elements>
Result<uint32> EncodeArrayElements(const Elements& array, usize count, ArchiveWriter& out,
                                   FieldEncodeContext& context) noexcept;

// 지금 위치(배열 필드 — context.Field())의 컨테이너 array를 out에 제자리로 쓰고 wire tag를
// 돌려준다(5.6절). 순서: MatchesSchema(struct 원소는 MatchesStructView도 — TypeMismatch) →
// Size ≤ maxElements(넘으면 배열 필드 경로의 ArrayTooLong 진단 뒤 CapacityExceeded — Required 검사
// 모드는 보지 않는다: 로드가 필드별 상한을 걸지 않으므로 넘친 배열도 열려야 한다) →
// AddElements(Size) → writer Begin(ValueArray — maxBytes·maxCount = 값 한도·MaxArrayElements,
// struct 원소는 RecordList — ArrayRecordListLimits(값 한도)) → 원소마다 ElementIndexScope::Set(i)
// 뒤 원소 → End. 원소마다 다시 검증한다: enum 선언 값(비어 있지 않은 배열마다 EnumValueSet 한 번 +
// 원소 수 × 조회 작업 선청구), Soft null 규칙·경로,
// rejectMissingRequired면 Required null(진단 경로 [f[i]]), 참조 원소는
// WriteReference({element.type, NativeTypeKey<T>()}, null은 0). struct 원소는 BeginItem →
// EncodeStructRecord → EndItem이다(치환 hook은 index 프레임에서). 가변 원소마다 Check()로 일찍
// 끊는다. 작업량은 out의 청구다. 호출자가 owner를 encode 고정한 상태여야 한다(원소 버퍼가 owner
// 밖이다).
template <class Container>
Result<uint32> EncodeArrayField(const Container& array, ArchiveWriter& out,
                                FieldEncodeContext& context) noexcept {
    using Traits = ArrayField<Container>;
    using Element = typename Traits::Element;
    static_assert(Traits::Supported);
    static_assert(ValueKindCanBeArrayElement(FieldValueKind<Element>()),
                  "Reflected array elements must be single values or reflected structs");
    const FieldSchema& field = context.Field();
    if (!MatchesSchema<Container>(field)) {
        return Result<uint32>::Failure(ErrorCode::TypeMismatch);
    }

    if constexpr (ReflectedStructField<Element>::Supported) {
        if (!MatchesStructView<Element>(context)) {
            return Result<uint32>::Failure(ErrorCode::TypeMismatch);
        }
    }

    // 필드별 상한은 저장(과 경로 쓰기)에서만 건다 — 넘친 배열은 그 경로로 진단한 뒤 거부한다.
    const usize count = array.Size();
    if (count > field.maxElements && !context.IsRequiredCheck()) {
        if (auto reported =
                context.Report(FieldDiagnosticKind::ArrayTooLong, static_cast<int64>(count));
            !reported) {
            return Result<uint32>::Failure(reported.Error());
        }

        return Result<uint32>::Failure(ErrorCode::CapacityExceeded);
    }

    return EncodeArrayElements<Element>(array, count, out, context);
}

// 배열·정적 배열(리플렉션 B9g) 필드 원소 count개를 out에 쓰고 wire tag를 돌려준다(EncodeArrayField·
// EncodeStaticArrayField 공용 — 호출자가 MatchesSchema·MatchesStructView와 원소 수 상한을 봤다).
// elements[i]는 원소 i다(ObjectRefArray는 ObjectHandle, 정적 배열은 원소 형식 그대로). 순서와
// 검증은 EncodeArrayField 주석의 AddElements 뒤와 같다.
template <class Element, class Elements>
Result<uint32> EncodeArrayElements(const Elements& array, usize count, ArchiveWriter& out,
                                   FieldEncodeContext& context) noexcept {
    const FieldSchema& field = context.Field();
    if (auto counted = context.AddElements(count); !counted) {
        return Result<uint32>::Failure(counted.Error());
    }

    const usize maxBytes = context.NestedRecordLimits().archive.maxTotalBytes;
    if constexpr (ReflectedStructField<Element>::Supported) {
        const StructView view = context.Current()->structure;
        auto list = RecordListWriter::Begin(out, static_cast<uint32>(count),
                                            ArrayRecordListLimits(maxBytes));
        if (!list) {
            return Result<uint32>::Failure(list.Error());
        }

        ElementIndexScope element(context);
        for (usize index = 0; index < count; ++index) {
            element.Set(static_cast<uint32>(index));
            if (auto begun = list.Value().BeginItem(); !begun) {
                return Result<uint32>::Failure(begun.Error());
            }

            if (auto written = EncodeStructRecord(view, &array[index], out, context); !written) {
                return written;
            }

            if (auto ended = list.Value().EndItem(); !ended) {
                return Result<uint32>::Failure(ended.Error());
            }
        }

        if (auto ended = list.Value().End(); !ended) {
            return Result<uint32>::Failure(ended.Error());
        }

        return Result<uint32>::Success(static_cast<uint32>(WireType::RecordList));
    } else {
        constexpr bool variable = IsSameV<Element, String> || IsSameV<Element, Name> ||
                                  SoftReferenceField<Element>::Supported ||
                                  InstancedStructField<Element>::Supported;
        auto elementWire = FieldWireType(field.element.kind, context.ReferenceWireType());
        if (!elementWire) {
            return elementWire;
        }

        auto writer =
            ValueArrayWriter::Begin(out, elementWire.Value(), static_cast<uint32>(count), maxBytes,
                                    MaxArrayElements, ElementArrayShape(field.element.kind));
        if (!writer) {
            return Result<uint32>::Failure(writer.Error());
        }

        // enum 원소는 비어 있지 않은 배열마다 판정 집합을 한 번 만들고 원소 수만큼의 조회 작업을
        // 먼저 청구한다(로드 두 단계와 같은 조건).
        Result<EnumValueSet> set = Result<EnumValueSet>::Failure(ErrorCode::Unsupported);
        if constexpr (ReflectedEnumField<Element>::Supported) {
            if (auto prepared = PrepareEnumElements(context, context.FieldEnum(), count, set);
                !prepared) {
                return Result<uint32>::Failure(prepared.Error());
            }
        }

        const EnumValueSet* lookup = set ? &set.Value() : nullptr;
        FieldValue value(context.Scratch());
        ElementIndexScope element(context);
        for (usize index = 0; index < count; ++index) {
            element.Set(static_cast<uint32>(index));
            StringView text;
            if constexpr (InstancedStructField<Element>::Supported) {
                // InstancedStruct 원소(리플렉션 B9j)는 u64 길이 + 내용이다.
                if (auto written = EncodeInstancedStructElement(array[index], out, context);
                    !written) {
                    return Result<uint32>::Failure(written.Error());
                }
            } else {
                if constexpr (ObjectReferenceField<Element>::Supported) {
                    value.kind = ValueKind::ObjectReference;
                    value.reference = ElementReferenceHandle(array[index]);
                } else if (auto copied = ElementWireValue(array[index], value, text); !copied) {
                    return Result<uint32>::Failure(copied.Error());
                }

                if (auto written =
                        WriteValueWire(out, context, field.element, context.FieldEnum(), value,
                                       text, StaticReferenceNative<Element>(), lookup);
                    !written) {
                    return Result<uint32>::Failure(written.Error());
                }
            }

            if constexpr (variable) {
                if (auto checked = writer.Value().Check(); !checked) {
                    return Result<uint32>::Failure(checked.Error());
                }
            }
        }

        if (auto ended = writer.Value().End(); !ended) {
            return Result<uint32>::Failure(ended.Error());
        }

        return Result<uint32>::Success(static_cast<uint32>(WireType::ValueArray));
    }
}

// 해석한 단일 값 하나(할당하지 않는 형식 — String·SoftObjectRef 말고 모두)를 C++ 값으로 만든다
// (배열 원소·Map 키와 값·Set 원소 공용). Name은 parsed.name(2단계 토큰, 1단계는 기본 None), weak
// 참조는 노드 handle(1단계는 비어 있다 — 관찰은 직렬화기가 막는다), ClassRef는 판정을 마친 클래스
// TypeId(ReportAndNull로 바꾼 미등록 클래스는 null)다.
template <class Element>
Element ParsedScalarValue(const ParsedValue& parsed) noexcept {
    static_assert(!IsSameV<Element, String> && !SoftReferenceField<Element>::Supported,
                  "allocating values are built by the caller");
    if constexpr (ReflectedEnumField<Element>::Supported) {
        return static_cast<Element>(parsed.signedInteger);
    } else if constexpr (IsSameV<Element, bool>) {
        return parsed.boolean;
    } else if constexpr (IsSameV<Element, Guid>) {
        return parsed.guid;
    } else if constexpr (IsSameV<Element, AssetId>) {
        return AssetId{parsed.guid};
    } else if constexpr (IsSameV<Element, Sha256Digest>) {
        return parsed.digest;
    } else if constexpr (IsSameV<Element, Name>) {
        return parsed.name;
    } else if constexpr (WeakReferenceField<Element>::Supported) {
        return Element(parsed.reference.handle);
    } else if constexpr (ClassReferenceField<Element>::Supported) {
        return Element(parsed.guid);
    } else if constexpr (IsSameV<Element, float>) {
        return parsed.float32;
    } else if constexpr (IsSameV<Element, double>) {
        return parsed.float64;
    } else if constexpr (IsSameV<Element, int8> || IsSameV<Element, int16> ||
                         IsSameV<Element, int32> || IsSameV<Element, int64>) {
        return static_cast<Element>(parsed.signedInteger);
    } else {
        static_assert(IsSameV<Element, uint8> || IsSameV<Element, uint16> ||
                      IsSameV<Element, uint32> || IsSameV<Element, uint64>);
        return static_cast<Element>(parsed.unsignedInteger);
    }
}

// 해석한 단일 값 원소 하나를 target 끝에 넣는다(예약된 용량 안 — 주소가 옮겨지지 않는다).
// String·SoftObjectRef는 target의 allocator로 만든 뒤 TryAssign한다(실패하면 빈 원소가 남고 그 오류
// — 2단계면 그래프 rollback). 그 밖은 ParsedScalarValue다.
template <class Element>
Result<void> AppendParsedElement(Array<Element>& target, const ParsedValue& parsed) noexcept {
    if constexpr (IsSameV<Element, String>) {
        auto added = target.TryEmplaceBack(target.GetAllocator());
        if (!added) {
            return Result<void>::Failure(added.Error());
        }

        return added.Value()->TryAssign(parsed.text);
    } else if constexpr (SoftReferenceField<Element>::Supported) {
        auto added = target.TryEmplaceBack(target.GetAllocator());
        if (!added) {
            return Result<void>::Failure(added.Error());
        }

        return added.Value()->TryAssign(AssetId{parsed.guid}, parsed.text);
    } else {
        return target.TryPushBack(ParsedScalarValue<Element>(parsed));
    }
}

// 2단계 몫 선청구 값: 원소 메모리 count × elementBytes + 원소 수 + 1 + extra.
inline Result<void> ReserveArrayApply(FieldDecodeContext& context, usize count, usize elementBytes,
                                      uint64 extra) noexcept {
    uint64 work = 0;
    if (!TryMultiply(uint64{count}, uint64{elementBytes}, work) || !TryAdd(work, count, work) ||
        !TryAdd(work, uint64{1}, work) || !TryAdd(work, extra, work)) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    return context.ReserveApply(work);
}

// 단일 값 원소 배열(Array<E>)의 decode다(DecodeArrayField의 ValueArray 가지). target이 없으면
// 검증만, 있으면(관찰·적용) Clear → TryReserve(count) → 원소마다 채운다. weak 원소
// (Array<WeakRef<T>>)는 문맥의 ReadWeakReference로 읽고(정적 대상 타입 NativeTypeKey<T>) 2단계에서
// 노드 handle을 그대로 담는다(GC 간선이 아니므로 Registry 컨테이너가 아니다).
template <class Element>
Result<void> DecodeValueArray(Array<Element>* target, const FieldView& field,
                              FieldDecodeContext& context) noexcept {
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
    const usize count = reader.Count();
    if (auto counted = context.AddElements(count); !counted) {
        return counted;
    }

    // enum 원소는 비어 있지 않은 배열마다 판정 집합을 한 번 만들고 원소 수만큼의 조회 작업을 원소
    // 읽기 전에 청구한다(1단계는 등록 enum과 정적 IsDeclared를 원소마다 모두 확인한다).
    Result<EnumValueSet> set = Result<EnumValueSet>::Failure(ErrorCode::Unsupported);
    if constexpr (ReflectedEnumField<Element>::Supported) {
        if (auto prepared = PrepareEnumElements(context, context.FieldEnum(), count, set);
            !prepared) {
            return prepared;
        }
    }

    // Name 원소는 1단계에서 토큰이 없고 weak 원소는 대상 handle이 없으므로 관찰(1단계 임시 값
    // 채우기)할 수 없다 — 관찰 struct는 Name·weak 참조를 담지 않는다(직렬화기가 ObservedStructs에서
    // 거부한다, Docs/Design/ReflectionExtensions.ko.md 4.1·5.1절).
    if constexpr (IsSameV<Element, Name> || WeakReferenceField<Element>::Supported) {
        if (target && context.IsRecording()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    if (target) {
        target->Clear();
        if (auto reserved = target->TryReserve(count); !reserved) {
            return reserved;
        }
    }

    const EnumValueSet* lookup = set ? &set.Value() : nullptr;
    usize textBytes = 0;
    usize references = 0;
    ElementIndexScope element(context);
    for (usize index = 0; index < count; ++index) {
        element.Set(static_cast<uint32>(index));
        ParsedValue parsed;
        if (auto read = ReadValueWire(reader.Elements(), context, type, context.FieldEnum(), lookup,
                                      StaticReferenceNative<Element>(), parsed);
            !read) {
            return read;
        }

        // weak 원소는 non-null마다 2단계의 연결 몫이 있다(강한 참조 배열과 같은 규칙).
        if constexpr (WeakReferenceField<Element>::Supported) {
            references += parsed.reference.isNull ? 0 : 1;
        }

        if constexpr (ReflectedEnumField<Element>::Supported) {
            if (auto resolved =
                    ResolveUndeclaredEnum(context, *context.FieldEnum(), lookup, parsed);
                !resolved) {
                return resolved;
            }

            // 등록 enum이 정적 enum보다 넓어도 2단계가 입력 때문에 실패하지 않게 한다.
            if (!GeneratedType<Element>::IsDeclared(parsed.signedInteger)) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        }

        // 원소 진단은 원소 검증 직후 [f[i]]로 낸다.
        if (auto reported = ReportParsedValue(context, type, parsed); !reported) {
            return reported;
        }

        // Name 원소는 문자열을 복사하지 않는다(1단계는 기록, 2단계는 2a 토큰 — 원소마다 같은 순서).
        if constexpr (IsSameV<Element, Name>) {
            if (auto resolved = ResolveParsedName(context, parsed); !resolved) {
                return resolved;
            }
        } else if (!TryAdd(textBytes, parsed.text.ByteCount(), textBytes)) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }

        if (target) {
            if (auto appended = AppendParsedElement(*target, parsed); !appended) {
                return appended;
            }
        }
    }

    if (auto finished = reader.Finish(); !finished) {
        return finished;
    }

    uint64 textWork = 0;
    if (!TryMultiply(uint64{textBytes}, uint64{2}, textWork) ||
        !TryAdd(textWork, uint64{references}, textWork)) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    return ReserveArrayApply(context, count, sizeof(Element), textWork);
}

// InstancedStruct 원소 배열(Array<InstancedStruct<>> — 리플렉션 B9j)의 decode다(DecodeArrayField의
// 가지 — 원소 tag InstancedStruct, u64 길이 + 내용). target이 없으면 검증만(관찰 자리는 오지 않는다
// — 관찰 struct는 InstancedStruct를 담지 않는다, InvalidArgument), 있으면(2단계) Clear →
// TryReserve(count) → 원소마다 빈 값을 넣고 그 자리에 DecodeInstancedStructElement다. 2단계 몫은
// 원소 메모리 count × sizeof(원소) + 원소 수 + 1이다(담긴 값 블록·멤버는 멤버 codec과 원소의 1이
// 덮는다).
template <class Element>
Result<void> DecodeInstancedArray(ConstructionContext* construction, ObjectHandle owner,
                                  Array<Element>* target, const FieldView& field,
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
    const usize count = reader.Count();
    if (auto counted = context.AddElements(count); !counted) {
        return counted;
    }

    if (target) {
        target->Clear();
        if (auto reserved = target->TryReserve(count); !reserved) {
            return reserved;
        }
    }

    ElementIndexScope element(context);
    for (usize index = 0; index < count; ++index) {
        element.Set(static_cast<uint32>(index));
        Element* slot = nullptr;
        if (target) {
            auto added = target->TryEmplaceBack();
            if (!added) {
                return Result<void>::Failure(added.Error());
            }

            slot = added.Value();
        }

        if (auto decoded =
                DecodeInstancedStructElement(construction, owner, slot, reader.Elements(), context);
            !decoded) {
            return decoded;
        }
    }

    if (auto finished = reader.Finish(); !finished) {
        return finished;
    }

    return ReserveArrayApply(context, count, sizeof(Element), 0);
}

// ObjectRefArray decode다(원소 tag = ReferenceWireType(), null 원소 허용). 1단계는 원소마다
// ReadReference — null은 token을 남기지 않고(Required null 진단은 적용 뒤 실효 값 검사가 [f[i]]로
// 낸다), non-null은 (token, 필드 기대 형식 {element.type, NativeTypeKey<T>()})을 남긴다. 2단계는
// handle(null 포함)을 Scratch Array<ObjectHandle>(count 예약)에 모아 AssignReferences를 한 번
// 부른다.
template <class T>
Result<void> DecodeReferenceArray(ConstructionContext* construction, ObjectHandle owner,
                                  ObjectRefArray<T>* target, const FieldView& field,
                                  FieldDecodeContext& context) noexcept {
    const FieldSchema& schema = context.Field();
    auto opened = context.OpenValueArray(field, context.ReferenceWireType(), MaxArrayElements);
    if (!opened) {
        return Result<void>::Failure(opened.Error());
    }

    ValueArrayReader& reader = opened.Value();
    const usize count = reader.Count();
    if (auto counted = context.AddElements(count); !counted) {
        return counted;
    }

    const bool apply = construction != nullptr && target != nullptr;
    Array<ObjectHandle> handles(context.Scratch());
    if (apply) {
        if (auto reserved = handles.TryReserve(count); !reserved) {
            return reserved;
        }
    }

    usize references = 0;
    ElementIndexScope element(context);
    for (usize index = 0; index < count; ++index) {
        element.Set(static_cast<uint32>(index));
        ParsedValue parsed;
        if (auto read = ReadValueWire(reader.Elements(), context, schema.element, nullptr, nullptr,
                                      NativeTypeKey<T>(), parsed);
            !read) {
            return read;
        }

        if (auto reported = ReportParsedValue(context, schema.element, parsed); !reported) {
            return reported;
        }

        references += parsed.reference.isNull ? 0 : 1;
        if (apply) {
            if (auto pushed = handles.TryPushBack(parsed.reference.handle); !pushed) {
                return pushed;
            }
        }
    }

    if (auto finished = reader.Finish(); !finished) {
        return finished;
    }

    // 원소 메모리는 staged 배열 버퍼와 Scratch 사본(각 count × sizeof(ObjectHandle))이고, 참조 연결
    // 수를 더한다.
    if (auto reserved =
            ReserveArrayApply(context, count, 2 * sizeof(ObjectHandle), uint64{references});
        !reserved) {
        return reserved;
    }

    if (!apply) {
        return Result<void>::Success();
    }

    return construction->AssignReferences(owner, *target, handles.View());
}

// struct 원소 배열(Array<S>·StructRefArray<S>) decode다(목록 항목 Record — 버전 범위 (1,
// UINT32_MAX)). 원소마다 ElementIndexScope::Set(i) 뒤 DecodeStructValue(목록 항목 출처)다.
// Array<S>는 검증(target 없음)이면 값 없이, 관찰·적용이면 Clear → TryReserve(count) → 원소를
// TryEmplaceBack(allocator 생성자가 있으면 target의 allocator)한 직후 그 주소에
// 해석한다(예약했으므로 주소가 고정된다). StructRefArray<S>는 2단계에서 AssignElements(owner,
// count) 뒤 원소마다 StructElementScope 안에서 원소 주소에 적용한다(참조 멤버는
// SetElementReference로 간다).
template <class Container>
Result<void> DecodeStructArray(ConstructionContext* construction, ObjectHandle owner,
                               Container* target, const FieldView& field,
                               FieldDecodeContext& context) noexcept {
    using Element = typename ArrayField<Container>::Element;
    const StructView view = context.Current()->structure;
    auto opened = context.OpenRecordList(field, view.Schema().id);
    if (!opened) {
        return Result<void>::Failure(opened.Error());
    }

    RecordListReader& list = opened.Value();
    const usize count = list.ItemCount();
    if (auto counted = context.AddElements(count); !counted) {
        return counted;
    }

    usize elementBytes = sizeof(Element);
    if constexpr (ArrayField<Container>::Registry) {
        // StructRefArray: 원소 블록 + 색인 포인터.
        elementBytes += sizeof(void*);
        const bool apply = construction != nullptr && target != nullptr;
        if (apply) {
            if (auto assigned = construction->AssignElements(owner, *target, count); !assigned) {
                return assigned;
            }
        }

        ElementIndexScope element(context);
        for (usize index = 0; index < count; ++index) {
            element.Set(static_cast<uint32>(index));
            StructRecordSource source;
            source.list = &list;
            source.index = index;
            if (apply) {
                StructElementScope located(context, StructRefArrayAccess::Location(*target, index));
                if (auto decoded = DecodeStructValue(view, construction, owner, &(*target)[index],
                                                     source, context);
                    !decoded) {
                    return decoded;
                }
            } else if (auto decoded =
                           DecodeStructValue(view, nullptr, owner, nullptr, source, context);
                       !decoded) {
                return decoded;
            }
        }
    } else {
        if (target) {
            target->Clear();
            if (auto reserved = target->TryReserve(count); !reserved) {
                return reserved;
            }
        }

        ElementIndexScope element(context);
        for (usize index = 0; index < count; ++index) {
            element.Set(static_cast<uint32>(index));
            Element* value = nullptr;
            if (target) {
                Result<Element*> added = Result<Element*>::Failure(ErrorCode::Unsupported);
                if constexpr (IsConstructibleV<Element, Allocator&>) {
                    added = target->TryEmplaceBack(target->GetAllocator());
                } else {
                    added = target->TryEmplaceBack();
                }

                if (!added) {
                    return Result<void>::Failure(added.Error());
                }

                value = added.Value();
            }

            StructRecordSource source;
            source.list = &list;
            source.index = index;
            if (auto decoded = DecodeStructValue(view, construction, owner, value, source, context);
                !decoded) {
                return decoded;
            }
        }
    }

    return ReserveArrayApply(context, count, elementBytes, 0);
}

// 지금 위치(배열 필드)의 저장값 field를 해석한다(5.6절). 세 방식은 struct와 같다: 검증
// (construction·target null — 원소를 할당하지 않는다), 관찰(construction null, target = 관찰 임시
// 값 안의 컨테이너 — ObservationScratch 원소로 채운다, 참조 없는 struct라 Array<E>만 온다), 적용(둘
// 다 non-null — staged 객체 안). 1·2단계가 같은 reader 호출 순서를 실행한다: wire
// 확인(TypeMismatch) → MatchesSchema(TypeMismatch) → OpenValueArray(maxCount =
// MaxArrayElements)·OpenRecordList → AddElements(count) → 원소 → Finish(trailing InvalidArgument) →
// ReserveApply 한 번(원소 메모리 count × sizeof(E) — StructRefArray는 + 색인 포인터,
// ObjectRefArray는 Scratch 사본 포함 — + 원소 수 + 1 + 2 × 문자열·경로 byte, ObjectRefArray는 +
// 참조 수). 로드는 필드별 maxElements를 걸지 않는다. 2단계 실패는 할당과 Registry 상태뿐이다(입력
// 검증은 모두 1단계).
template <class Container>
Result<void> DecodeArrayField(ConstructionContext* construction, ObjectHandle owner,
                              Container* target, const FieldView& field,
                              FieldDecodeContext& context) noexcept {
    using Traits = ArrayField<Container>;
    using Element = typename Traits::Element;
    static_assert(Traits::Supported);
    static_assert(ValueKindCanBeArrayElement(FieldValueKind<Element>()),
                  "Reflected array elements must be single values or reflected structs");
    const FieldSchema& schema = context.Field();
    auto wire = FieldWireType(schema, context.ReferenceWireType());
    if (!wire) {
        return Result<void>::Failure(wire.Error());
    }

    if (field.wireType != wire.Value()) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    if (!MatchesSchema<Container>(schema)) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    if constexpr (ReflectedStructField<Element>::Supported) {
        if (!MatchesStructView<Element>(context)) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }

        return DecodeStructArray(construction, owner, target, field, context);
    } else if constexpr (ObjectReferenceField<Element>::Supported) {
        return DecodeReferenceArray(construction, owner, target, field, context);
    } else if constexpr (InstancedStructField<Element>::Supported) {
        return DecodeInstancedArray(construction, owner, target, field, context);
    } else {
        return DecodeValueArray(target, field, context);
    }
}

// 생성 Trace가 모든 배열 필드·멤버에 부르는 오버로드 집합이다(5.6절 Trace). 실제 C++ 타입이 호출을
// 고르므로 descriptor에서 컨테이너를 잘못 추론해도 Trace가 빠지지 않는다: Array<E>는 강한 참조가
// 없어 아무것도 하지 않고, ObjectRefArray·StructRefArray는 모든 원소를 방문한다.
//
// 사용 예(생성 Trace):
//   detail::TraceArrayField(value.*(&::Mesh::materials), visitor);
template <class E>
void TraceArrayField(const Array<E>&, TraceVisitor&) noexcept {
}

template <class T>
void TraceArrayField(const ObjectRefArray<T>& array, TraceVisitor& visitor) noexcept {
    array.Trace(visitor);
}

template <class S>
void TraceArrayField(const StructRefArray<S>& array, TraceVisitor& visitor) noexcept {
    array.Trace(visitor);
}

// 정적 배열(T x[N] — 리플렉션 B9g)은 ObjectRef 원소를 모두 방문하고 struct 원소는 원소마다 그
// struct의 생성 Trace(참조 없는 struct는 빈 본문)를 부른다. 그 밖의 원소는 강한 참조가 없다.
template <class E, usize N>
void TraceArrayField(const E (&array)[N], TraceVisitor& visitor) noexcept {
    if constexpr (ObjectReferenceField<E>::Supported) {
        for (const E& element : array) {
            visitor.Visit(element);
        }
    } else if constexpr (ReflectedStructField<E>::Supported) {
        for (const E& element : array) {
            GeneratedType<E>::Trace(element, visitor);
        }
    } else {
        (void)array;
        (void)visitor;
    }
}

} // namespace rocks::detail
