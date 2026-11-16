#pragma once

#include <Object/OptionalFieldCodec.h>

// 필드 codec 사슬의 여섯째 조각으로 Map·Set 필드 codec이다(리플렉션 B9f2 — Docs/Design/
// ReflectionExtensions.ko.md 1.2·7.2절). 쓰기(EncodeMapField — OrderedMap<K, V>·OrderedSet<T>)와
// 읽기(DecodeMapField), 1단계 키 중복 판정의 비소유 키(KeyProbe — 입력 byte를 빌린다)가 있다.
// wire는 Map(16: u32 키 wire + u32 값 wire + u32 count + (키, 값) × count — 값 struct는 u64 길이 +
// Record)·Set(15: u32 원소 wire + u32 count + 원소 × count)이고 순서는 삽입 순서다(OrderedMap 순회
// 순서 — 해시·seed와 무관하다). 필드는 통째로 저장·복원한다(차이 단위 통째 — 20절 결정 U7). 같음
// (차이 저장의 기준 비교)은 순서까지 포함한 byte 같음이다. Object/GeneratedFieldCodec.h로 받는다.
//
// 키 자리 규칙(Map 키·Set 원소 — 1.7절):
//   - 값을 바꾸는 decode 정책(미선언 enum ReportAndDefault, unknownClass ReportAndNull)을 쓰지
//     않는다. 미선언 enum 키는 InvalidArgument, 미등록 ClassRef 키는 NotFound다(대체 값이 서로 다른
//     두 키를 같게 만들어 2단계에서 중복이 생기는 일을 막는다).
//   - 파일 안 같은 키 둘은 손상이다(InvalidArgument — 1단계가 판정하고 2단계는 판정하지 않는다).
//     같음은 키 종류 규칙이다: String·Digest·Guid·정수·Bool·enum·AssetId·ClassRef·SoftObjectRef는
//     wire byte(값) 같음, Name은 Core Name 규칙(NameTextEquals — ASCII 대소문자 무시·번호 접미).
//   - Map 키에는 Required가 없다(필드의 Required는 Map 값·Set 원소마다 non-null이다).
namespace rocks::detail {

// 1단계 중복 판정의 소프트 참조 키다(asset과 경로 view — 입력을 빌린다).
struct SoftKeyView {
    AssetId asset{};
    StringView path;

    friend bool operator==(const SoftKeyView& left, const SoftKeyView& right) noexcept {
        return left.asset == right.asset && left.path == right.path;
    }
};

// SoftKeyView의 seed 해시다(asset과 경로 byte).
struct SoftKeyViewHash : SeededHashBase {
    using SeededHashBase::SeededHashBase;

    SoftKeyViewHash() noexcept = default;

    uint64 operator()(const SoftKeyView& value) const noexcept {
        const uint64 path = SipHash24(Seed(), value.path.Data(), value.path.ByteCount());
        const uint64 words[3] = {value.asset.value.high, value.asset.value.low, path};
        return SipHash24Words(Seed(), words, 3);
    }
};

// 1단계 중복 판정의 Name 키다(표시 문자열 view — 1단계에는 토큰이 없다). 같음은 NameTextEquals다.
struct NameKeyView {
    StringView text;
};

// NameKeyView의 같음이다(Core Name 규칙 — 2단계 토큰의 operator==와 같은 답).
struct NameKeyViewEqual {
    bool operator()(const NameKeyView& left, const NameKeyView& right) const noexcept {
        return NameTextEquals(left.text, right.text);
    }
};

// NameKeyView의 seed 해시다. NameTextEquals가 같다고 보는 원문은 같은 값이다: None(빈 base는 번호와
// 무관하게, base "None"은 ASCII 대소문자 무시로 번호까지)과 그 밖(base의 ASCII 대소문자 무시 byte와
// 번호). base는 SipHash24IgnoringAsciiCase로 seed 안에서 해시한다(FNV 키를 섞지 않는다 — 충돌을
// 고를 수 없다).
struct NameKeyViewHash : SeededHashBase {
    using SeededHashBase::SeededHashBase;

    NameKeyViewHash() noexcept = default;

    uint64 operator()(const NameKeyView& value) const noexcept {
        constexpr char none[] = {'N', 'o', 'n', 'e'};
        const NameTextParts parts = SplitNameText(value.text);
        const bool emptyBase = parts.base.ByteCount() == 0;
        const bool noneBase =
            emptyBase ||
            EqualsIgnoringAsciiCase(Span<const char>(parts.base.Data(), parts.base.ByteCount()),
                                    Span<const char>(none, sizeof(none)));
        uint64 words[3] = {};
        words[0] = noneBase ? 1 : 0;
        words[1] = emptyBase ? 0 : parts.number;
        words[2] = noneBase ? 0 : SipHash24IgnoringAsciiCase(Seed(), parts.base);
        return SipHash24Words(Seed(), words, 3);
    }
};

// 키 C++ 형식 K의 1단계 판정 키다: Type은 입력을 빌리는 비교 형식, Make는 검증을 마친
// ParsedValue에서 그 값을 만든다(할당 없음). 기본은 K 값 그대로(정수·bool·enum·Guid·AssetId·
// Sha256Digest·ClassRef — 값 같음 = wire byte 같음)와 그 seed 해시다.
template <class K>
struct KeyProbe {
    using Type = K;
    using HashPolicy = SeededHash<K>;
    using EqualPolicy = Equal<K>;

    static Type Make(const ParsedValue& parsed) noexcept {
        return ParsedScalarValue<K>(parsed);
    }
};

template <>
struct KeyProbe<String> {
    using Type = StringView;
    using HashPolicy = SeededHash<StringView>;
    using EqualPolicy = Equal<StringView>;

    static Type Make(const ParsedValue& parsed) noexcept {
        return parsed.text;
    }
};

template <class T>
struct KeyProbe<SoftObjectRef<T>> {
    using Type = SoftKeyView;
    using HashPolicy = SoftKeyViewHash;
    using EqualPolicy = Equal<SoftKeyView>;

    static Type Make(const ParsedValue& parsed) noexcept {
        return SoftKeyView{AssetId{parsed.guid}, parsed.text};
    }
};

template <>
struct KeyProbe<Name> {
    using Type = NameKeyView;
    using HashPolicy = NameKeyViewHash;
    using EqualPolicy = NameKeyViewEqual;

    static Type Make(const ParsedValue& parsed) noexcept {
        return NameKeyView{parsed.text};
    }
};

// 키 C++ 형식 K의 서로 다른 값 수 상한으로 count를 줄인다(1단계 색인·관찰 컨테이너의 예약 크기):
// bool은 2, 8비트 정수는 256, 16비트 정수는 65,536, Flags가 아닌 enum은 등록 스키마의 선언 수
// (enumeration — 미선언 키는 거부된다)이고 그 밖(넓은 정수·Guid·문자열 등)은 count 그대로다. 그보다
// 많은 항목은 그 상한 + 1째에서 중복으로 거부되므로 넣는 키 수가 이 값을 넘지 않는다.
template <class K>
usize KeyDomainLimit(usize count, const EnumSchema* enumeration) noexcept {
    usize limit = count;
    if constexpr (IsSameV<K, bool>) {
        limit = 2;
    } else if constexpr (IsSameV<K, uint8> || IsSameV<K, int8>) {
        limit = usize{NumericLimits<uint8>::Max()} + 1;
    } else if constexpr (IsSameV<K, uint16> || IsSameV<K, int16>) {
        limit = usize{NumericLimits<uint16>::Max()} + 1;
    } else if constexpr (ReflectedEnumField<K>::Supported) {
        if (enumeration && !HasFlag(enumeration->flags, EnumFlags::Flags)) {
            limit = enumeration->values.Size();
        }
    }

    return limit < count ? limit : count;
}

// 해석한 키·값·원소 하나를 컨테이너에 넣을 C++ 값으로 만든다. String·SoftObjectRef는 allocator로
// 만들어 복사한다(OutOfMemory — 규칙 검사는 1단계 ReadValueWire가 마쳤다), 그 밖은
// ParsedScalarValue다.
template <class E>
Result<E> MakeParsedValue(Allocator& allocator, const ParsedValue& parsed) noexcept {
    if constexpr (IsSameV<E, String>) {
        String value(allocator);
        if (auto copied = value.TryAssign(parsed.text); !copied) {
            return Result<E>::Failure(copied.Error());
        }

        return Result<E>::Success(Move(value));
    } else if constexpr (SoftReferenceField<E>::Supported) {
        E value(allocator);
        if (auto copied = value.TryAssign(AssetId{parsed.guid}, parsed.text); !copied) {
            return Result<E>::Failure(copied.Error());
        }

        return Result<E>::Success(Move(value));
    } else {
        return Result<E>::Success(ParsedScalarValue<E>(parsed));
    }
}

// 지금 위치(Map·Set 필드 — context.Field())의 컨테이너 source를 out에 제자리로 쓰고 wire tag(Map·
// Set)를 돌려준다(호출자가 MatchesSchema를 확인했다). 순서: 값 struct면 MatchesStructView
// (TypeMismatch) → Count ≤ maxElements(넘으면 필드 경로의 ArrayTooLong 진단 뒤 CapacityExceeded —
// Required 검사 모드는 보지 않는다, 로드는 필드별 상한을 걸지 않는다) → AddElements(Count) → writer
// Begin(Map·Set — maxBytes·maxCount = 값 한도·MaxArrayElements) → 항목마다
// ElementIndexScope::Set(삽입 순번) 뒤 키(Map 키는 Required 검사 없음 — WriteValueWire key)와 값(값
// struct는 u64 길이 + EncodeStructRecord, 그 밖은 WriteValueWire), Set은 원소(Required면 원소마다
// non-null) → End. 키·값은 배열 원소와 같이 다시 검증한다(enum 선언 값 — 비어 있지 않은
// 컨테이너마다 판정 집합 한 번 + 원소 수 × 조회 작업 선청구, Soft null 규칙·경로, ClassRef
// 등록·Base, Name pool·내용). 가변 키·값마다 Check()로 일찍 끊는다. 작업량은 out의 청구다. 호출자가
// owner를 encode 고정한 상태여야 한다(항목 버퍼가 owner 밖이다).
template <class Container>
Result<uint32> EncodeMapField(const Container& source, ArchiveWriter& out,
                              FieldEncodeContext& context) noexcept {
    using Traits = MapField<Container>;
    using Key = typename Traits::Key;
    using Value = typename Traits::Value;
    constexpr bool set = Traits::IsSet;
    constexpr bool structValue = !set && ReflectedStructField<Value>::Supported;
    static_assert(Traits::Supported);
    static_assert(set ? ValueKindCanBeSetElement(FieldValueKind<Key>())
                      : ValueKindCanBeMapKey(FieldValueKind<Key>()),
                  "Reflected map keys and set elements must be key kinds");
    static_assert(set || (ValueKindCanBeMapValue(FieldValueKind<Value>()) &&
                          FieldValueKind<Value>() != ValueKind::ObjectReference),
                  "Reflected map values must be single values or value structs");
    const FieldSchema& field = context.Field();
    if constexpr (structValue) {
        if (!MatchesStructView<Value>(context)) {
            return Result<uint32>::Failure(ErrorCode::TypeMismatch);
        }
    }

    // 필드별 상한은 저장(과 경로 쓰기)에서만 건다 — 넘친 컨테이너는 그 경로로 진단한 뒤 거부한다.
    const usize count = source.Count();
    if (count > field.maxElements && !context.IsRequiredCheck()) {
        if (auto reported =
                context.Report(FieldDiagnosticKind::ArrayTooLong, static_cast<int64>(count));
            !reported) {
            return Result<uint32>::Failure(reported.Error());
        }

        return Result<uint32>::Failure(ErrorCode::CapacityExceeded);
    }

    if (auto counted = context.AddElements(count); !counted) {
        return Result<uint32>::Failure(counted.Error());
    }

    const ValueType& keyType = set ? field.element : field.key;
    const EnumSchema* keyEnum = set ? context.FieldEnum() : context.FieldKeyEnum();
    auto keyWire = FieldWireType(keyType.kind, context.ReferenceWireType());
    if (!keyWire) {
        return keyWire;
    }

    const usize maxBytes = context.NestedRecordLimits().archive.maxTotalBytes;
    constexpr bool variable = IsSameV<Key, String> || IsSameV<Key, Name> ||
                              SoftReferenceField<Key>::Supported || IsSameV<Value, String> ||
                              IsSameV<Value, Name> || SoftReferenceField<Value>::Supported ||
                              structValue;

    // 키 enum·값 enum은 비어 있지 않은 컨테이너마다 판정 집합을 한 번 만들고 원소 수만큼의 조회
    // 작업을 먼저 청구한다(로드 두 단계와 같은 조건).
    Result<EnumValueSet> keySet = Result<EnumValueSet>::Failure(ErrorCode::Unsupported);
    if constexpr (ReflectedEnumField<Key>::Supported) {
        if (auto prepared = PrepareEnumElements(context, keyEnum, count, keySet); !prepared) {
            return Result<uint32>::Failure(prepared.Error());
        }
    }

    Result<EnumValueSet> valueSet = Result<EnumValueSet>::Failure(ErrorCode::Unsupported);
    if constexpr (!set && ReflectedEnumField<Value>::Supported) {
        if (auto prepared = PrepareEnumElements(context, context.FieldEnum(), count, valueSet);
            !prepared) {
            return Result<uint32>::Failure(prepared.Error());
        }
    }

    const EnumValueSet* keyLookup = keySet ? &keySet.Value() : nullptr;
    const EnumValueSet* valueLookup = valueSet ? &valueSet.Value() : nullptr;
    FieldValue keyValue(context.Scratch());
    FieldValue entryValue(context.Scratch());
    // 키 하나를 쓴다(Map 키는 Required 검사 없음, Set 원소는 필드의 Required를 원소마다 본다).
    const auto writeKey = [&](const Key& key) noexcept -> Result<void> {
        StringView text;
        if (auto copied = ElementWireValue(key, keyValue, text); !copied) {
            return copied;
        }

        return WriteValueWire(out, context, keyType, keyEnum, keyValue, text,
                              StaticReferenceNative<Key>(), keyLookup, !set);
    };

    if constexpr (set) {
        auto writer =
            SetWireWriter::Begin(out, keyWire.Value(), static_cast<uint32>(count), maxBytes,
                                 MaxArrayElements, ElementArrayShape(keyType.kind));
        if (!writer) {
            return Result<uint32>::Failure(writer.Error());
        }

        ElementIndexScope element(context);
        uint32 index = 0;
        for (const Key& key : source) {
            element.Set(index++);
            if (auto written = writeKey(key); !written) {
                return Result<uint32>::Failure(written.Error());
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

        return Result<uint32>::Success(static_cast<uint32>(WireType::Set));
    } else {
        uint32 valueWire = static_cast<uint32>(WireType::Record);
        if constexpr (!structValue) {
            auto wire = FieldWireType(field.element.kind, context.ReferenceWireType());
            if (!wire) {
                return wire;
            }

            valueWire = wire.Value();
        }

        auto writer = MapWireWriter::Begin(
            out, keyWire.Value(), valueWire, static_cast<uint32>(count), maxBytes, MaxArrayElements,
            ElementArrayShape(keyType.kind),
            structValue ? WireValueShape{} : ElementArrayShape(field.element.kind));
        if (!writer) {
            return Result<uint32>::Failure(writer.Error());
        }

        const StructView view = context.Current()->structure;
        ElementIndexScope element(context);
        uint32 index = 0;
        for (auto entry : source) {
            element.Set(index++);
            if (auto written = writeKey(entry.key); !written) {
                return Result<uint32>::Failure(written.Error());
            }

            if constexpr (structValue) {
                auto record = PrefixedRecordWriter::Begin(out, WireType::Record);
                if (!record) {
                    return Result<uint32>::Failure(record.Error());
                }

                auto wire = EncodeStructRecord(view, &entry.value, out, context);
                if (!wire) {
                    return wire;
                }

                if (wire.Value() != static_cast<uint32>(WireType::Record)) {
                    return Result<uint32>::Failure(ErrorCode::TypeMismatch);
                }

                if (auto ended = record.Value().End(); !ended) {
                    return Result<uint32>::Failure(ended.Error());
                }
            } else {
                StringView text;
                if (auto copied = ElementWireValue(entry.value, entryValue, text); !copied) {
                    return Result<uint32>::Failure(copied.Error());
                }

                if (auto written =
                        WriteValueWire(out, context, field.element, context.FieldEnum(), entryValue,
                                       text, StaticReferenceNative<Value>(), valueLookup);
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

        return Result<uint32>::Success(static_cast<uint32>(WireType::Map));
    }
}

// Map·Set 필드의 항목 열을 해석한다(DecodeMapField의 본문 — reader를 연 뒤, Finish 전). count는
// header의 항목 수, entries는 항목 reader다. applyWork에 2단계 몫(문자열 복사 2배 + weak 연결 수)을
// 쓴다.
template <class Container>
Result<void> DecodeMapEntries(ConstructionContext* construction, ObjectHandle owner,
                              Container* target, ArchiveReader& entries, usize count,
                              FieldDecodeContext& context, uint64& applyWork) noexcept {
    using Traits = MapField<Container>;
    using Key = typename Traits::Key;
    using Value = typename Traits::Value;
    using Probe = KeyProbe<Key>;
    constexpr bool set = Traits::IsSet;
    constexpr bool structValue = !set && ReflectedStructField<Value>::Supported;
    const FieldSchema& schema = context.Field();
    const ValueType& keyType = set ? schema.element : schema.key;
    const EnumSchema* keyEnum = set ? context.FieldEnum() : context.FieldKeyEnum();
    if (auto counted = context.AddElements(count); !counted) {
        return counted;
    }

    // 키 enum·값 enum은 비어 있지 않은 컨테이너마다 판정 집합을 한 번 만들고 원소 수만큼의 조회
    // 작업을 항목 읽기 전에 청구한다(1단계는 등록 enum과 정적 IsDeclared를 항목마다 모두 확인한다).
    Result<EnumValueSet> keySet = Result<EnumValueSet>::Failure(ErrorCode::Unsupported);
    if constexpr (ReflectedEnumField<Key>::Supported) {
        if (auto prepared = PrepareEnumElements(context, keyEnum, count, keySet); !prepared) {
            return prepared;
        }
    }

    Result<EnumValueSet> valueSet = Result<EnumValueSet>::Failure(ErrorCode::Unsupported);
    if constexpr (!set && ReflectedEnumField<Value>::Supported) {
        if (auto prepared = PrepareEnumElements(context, context.FieldEnum(), count, valueSet);
            !prepared) {
            return prepared;
        }
    }

    const EnumValueSet* keyLookup = keySet ? &keySet.Value() : nullptr;
    const EnumValueSet* valueLookup = valueSet ? &valueSet.Value() : nullptr;
    // 1단계(기록하는 단계)의 키 중복 판정 색인이다. 입력 byte를 빌리는 비교 키를 seed 해시(프로세스
    // seed — 상대가 색인 충돌을 고를 수 없다)로 담는다. 메모리는 ObservationScratch(1단계 전용 —
    // 바이트를 작업량으로 한 번 청구하고 단일 할당 한도가 없다: 색인이 wire보다 클 수 있으므로
    // 저장할 수 있었던 컨테이너는 이 한도로 막히지 않는다)이고 키 값 영역으로 줄인 count만큼 먼저
    // 잡는다(KeyDomainLimit — Set<bool> count 100만도 2칸). 2단계는 판정하지 않는다(1단계와 같은 키
    // — 키 자리는 값을 바꾸는 정책이 없다).
    const usize reserve = KeyDomainLimit<Key>(count, keyEnum);
    OrderedSet<typename Probe::Type, typename Probe::HashPolicy, typename Probe::EqualPolicy> seen(
        context.ObservationScratch());
    const bool recording = context.IsRecording();
    if (recording) {
        if (auto reserved = seen.Reserve(reserve); !reserved) {
            return reserved;
        }
    }

    // 적용·관찰 자리도 같은 상한으로 예약한다(넣는 키 수가 그 상한을 넘지 않으므로 값 struct 주소가
    // 고정된다).
    if (target) {
        target->Clear();
        if (auto reserved = target->Reserve(reserve); !reserved) {
            return reserved;
        }
    }

    usize textBytes = 0;
    usize references = 0;
    ElementIndexScope element(context);
    for (usize index = 0; index < count; ++index) {
        element.Set(static_cast<uint32>(index));
        ParsedValue key;
        if (auto read = ReadValueWire(entries, context, keyType, keyEnum, keyLookup,
                                      StaticReferenceNative<Key>(), key, true);
            !read) {
            return read;
        }

        // 키 자리는 미선언 enum을 정책과 무관하게 거부한다. 등록 enum이 정적 enum보다 넓어도
        // 2단계가 입력 때문에 실패하지 않게 정적 선언도 본다.
        if constexpr (ReflectedEnumField<Key>::Supported) {
            if (key.undeclared || !GeneratedType<Key>::IsDeclared(key.signedInteger)) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        }

        if (recording) {
            // 해시(키 byte)와 같은 해시 비교 하나의 몫이다.
            if (auto spent = context.Spend(uint64{key.text.ByteCount()} + 1); !spent) {
                return spent;
            }

            auto added = seen.Add(Probe::Make(key));
            if (!added) {
                return Result<void>::Failure(added.Error());
            }

            if (added.Value() == InsertOutcome::AlreadyPresent) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        }

        // 키 진단·수집(non-null 소프트 키)은 키 검증 직후 [f[i]]로 낸다.
        if (auto reported = ReportParsedValue(context, keyType, key); !reported) {
            return reported;
        }

        // Name 키·원소는 문자열을 복사하지 않는다(1단계는 기록, 2단계는 2a 토큰 — 항목마다 같은
        // 순서).
        if constexpr (IsSameV<Key, Name>) {
            if (auto resolved = ResolveParsedName(context, key); !resolved) {
                return resolved;
            }
        } else if (!TryAdd(textBytes, key.text.ByteCount(), textBytes)) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }

        if constexpr (set) {
            if (target) {
                auto made = MakeParsedValue<Key>(target->GetAllocator(), key);
                if (!made) {
                    return Result<void>::Failure(made.Error());
                }

                auto added = target->Add(Move(made.Value()));
                if (!added) {
                    return Result<void>::Failure(added.Error());
                }

                // 1단계가 중복을 거부했으므로 계약 위반이다.
                if (added.Value() == InsertOutcome::AlreadyPresent) {
                    return Result<void>::Failure(ErrorCode::InvalidArgument);
                }
            }
        } else if constexpr (structValue) {
            auto record = ReadPrefixedRecord(entries);
            if (!record) {
                return Result<void>::Failure(record.Error());
            }

            // 적용·관찰 자리면 키와 기본 값(S(Allocator&) 또는 S())을 먼저 넣고 그 자리에 해석한다
            // (Reserve했으므로 주소가 고정된다 — Record에 없는 멤버는 기본 값이다).
            Value* value = nullptr;
            if (target) {
                auto made = MakeParsedValue<Key>(target->GetAllocator(), key);
                if (!made) {
                    return Result<void>::Failure(made.Error());
                }

                Allocator& allocator = target->GetAllocator();
                auto added = target->FindOrAddWith(Move(made.Value()), [&allocator]() noexcept {
                    return Result<Value>::Success(MakeAllocatedValue<Value>(allocator));
                });
                if (!added) {
                    return Result<void>::Failure(added.Error());
                }

                if (added.Value().outcome == InsertOutcome::AlreadyPresent) {
                    return Result<void>::Failure(ErrorCode::InvalidArgument);
                }

                value = added.Value().value;
            }

            StructRecordSource source;
            source.record = record.Value();
            if (auto decoded = DecodeStructValue(context.Current()->structure, construction, owner,
                                                 value, source, context);
                !decoded) {
                return decoded;
            }
        } else {
            ParsedValue parsed;
            if (auto read = ReadValueWire(entries, context, schema.element, context.FieldEnum(),
                                          valueLookup, StaticReferenceNative<Value>(), parsed);
                !read) {
                return read;
            }

            // weak 값은 non-null마다 2단계의 연결 몫이 있다(배열과 같은 규칙).
            if constexpr (WeakReferenceField<Value>::Supported) {
                references += parsed.reference.isNull ? 0 : 1;
            }

            if constexpr (ReflectedEnumField<Value>::Supported) {
                if (auto resolved =
                        ResolveUndeclaredEnum(context, *context.FieldEnum(), valueLookup, parsed);
                    !resolved) {
                    return resolved;
                }

                if (!GeneratedType<Value>::IsDeclared(parsed.signedInteger)) {
                    return Result<void>::Failure(ErrorCode::InvalidArgument);
                }
            }

            if (auto reported = ReportParsedValue(context, schema.element, parsed); !reported) {
                return reported;
            }

            if constexpr (IsSameV<Value, Name>) {
                if (auto resolved = ResolveParsedName(context, parsed); !resolved) {
                    return resolved;
                }
            } else if (!TryAdd(textBytes, parsed.text.ByteCount(), textBytes)) {
                return Result<void>::Failure(ErrorCode::Overflow);
            }

            if (target) {
                auto madeKey = MakeParsedValue<Key>(target->GetAllocator(), key);
                if (!madeKey) {
                    return Result<void>::Failure(madeKey.Error());
                }

                auto madeValue = MakeParsedValue<Value>(target->GetAllocator(), parsed);
                if (!madeValue) {
                    return Result<void>::Failure(madeValue.Error());
                }

                auto added = target->Insert(Move(madeKey.Value()), Move(madeValue.Value()));
                if (!added) {
                    return Result<void>::Failure(added.Error());
                }

                if (added.Value() == InsertOutcome::AlreadyPresent) {
                    return Result<void>::Failure(ErrorCode::InvalidArgument);
                }
            }
        }
    }

    if (!TryMultiply(uint64{textBytes}, uint64{2}, applyWork) ||
        !TryAdd(applyWork, uint64{references}, applyWork)) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    return Result<void>::Success();
}

// 지금 위치(Map·Set 필드 — context.Field(), 호출자가 MatchesSchema를 확인했다)의 저장값 field를
// 해석한다. 세 방식은 배열과 같다: 검증(construction·target null — 항목을 할당하지 않는다), 관찰
// (construction null, target = 관찰 임시 값 안의 컨테이너 — 그 컨테이너의 allocator
// (ObservationScratch)로 채운다), 적용(둘 다 non-null — staged 객체 안, 객체 allocator). 1·2단계가
// 같은 reader 호출 순서를 실행한다: wire 확인(TypeMismatch) → 값 struct면 MatchesStructView
// (TypeMismatch) → 관찰 자리의 Name·weak 거부(InvalidArgument — 1단계에는 토큰·대상 handle이
// 없다) → MapWireReader·SetWireReader(maxCount = MaxArrayElements — 키·값 tag·count 사전 검사·
// count만큼 work 청구) → AddElements(count) → 항목마다 [키 읽기·검증(키 자리 규칙) → 1단계면 중복
// 판정 → 진단·수집 → 값(값 struct는 u64 길이 + Record를 OpenRecordBytes로 — 최상위 필드면 1단계가
// 2단계 몫을 선청구하고 2단계는 단독 Open, 1.2절 "뒤 단계 주의")] → Finish(trailing
// InvalidArgument) → ReserveApply 한 번(컨테이너 Reserve(count)의 저장소 byte
// Container::ReservedBytes + 항목 수 + 1
// + 2 × 문자열·경로 byte + weak 연결 수). 적용·관찰은 컨테이너를 비우고(Clear) 키 값 영역으로 줄인
// count만큼 예약한 뒤(KeyDomainLimit) 파일 순서대로 넣는다(삽입 순서 = 파일 순서). 로드는 필드별
// maxElements를 걸지 않는다. 2단계 실패는 할당뿐이다(입력 검증은 모두 1단계 — 중복 키는 1단계가
// 거부했으므로 2단계의 AlreadyPresent는 계약 위반 InvalidArgument다).
template <class Container>
Result<void> DecodeMapField(ConstructionContext* construction, ObjectHandle owner,
                            Container* target, const FieldView& field,
                            FieldDecodeContext& context) noexcept {
    using Traits = MapField<Container>;
    using Key = typename Traits::Key;
    using Value = typename Traits::Value;
    constexpr bool set = Traits::IsSet;
    constexpr bool structValue = !set && ReflectedStructField<Value>::Supported;
    static_assert(Traits::Supported);
    static_assert(set ? ValueKindCanBeSetElement(FieldValueKind<Key>())
                      : ValueKindCanBeMapKey(FieldValueKind<Key>()),
                  "Reflected map keys and set elements must be key kinds");
    static_assert(set || (ValueKindCanBeMapValue(FieldValueKind<Value>()) &&
                          FieldValueKind<Value>() != ValueKind::ObjectReference),
                  "Reflected map values must be single values or value structs");
    const FieldSchema& schema = context.Field();
    auto wire = FieldWireType(schema, context.ReferenceWireType());
    if (!wire) {
        return Result<void>::Failure(wire.Error());
    }

    if (field.wireType != wire.Value()) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    if constexpr (structValue) {
        if (!MatchesStructView<Value>(context)) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }
    }

    // Name 키·값은 1단계에서 토큰이 없고 weak 값은 대상 handle이 없으므로 관찰(1단계 임시 값
    // 채우기)할 수 없다 — 관찰 struct는 Name·weak 참조를 담지 않는다(직렬화기가 ObservedStructs에서
    // 거부한다, Docs/Design/ReflectionExtensions.ko.md 4.1·5.1절).
    constexpr bool unobservable =
        IsSameV<Key, Name> || IsSameV<Value, Name> || WeakReferenceField<Value>::Supported;
    if constexpr (unobservable) {
        if (target && context.IsRecording()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    const ValueType& keyType = set ? schema.element : schema.key;
    auto keyWire = FieldWireType(keyType.kind, context.ReferenceWireType());
    if (!keyWire) {
        return Result<void>::Failure(keyWire.Error());
    }

    auto opened = context.OpenValue(field);
    if (!opened) {
        return Result<void>::Failure(opened.Error());
    }

    uint64 applyWork = 0;
    usize count = 0;
    if constexpr (set) {
        auto reader = SetWireReader::Open(Move(opened.Value()), keyWire.Value(), MaxArrayElements,
                                          ElementArrayShape(keyType.kind));
        if (!reader) {
            return Result<void>::Failure(reader.Error());
        }

        count = reader.Value().Count();
        if (auto decoded = DecodeMapEntries(construction, owner, target, reader.Value().Elements(),
                                            count, context, applyWork);
            !decoded) {
            return decoded;
        }

        if (auto finished = reader.Value().Finish(); !finished) {
            return finished;
        }
    } else {
        uint32 valueWire = static_cast<uint32>(WireType::Record);
        if constexpr (!structValue) {
            auto elementWire = FieldWireType(schema.element.kind, context.ReferenceWireType());
            if (!elementWire) {
                return Result<void>::Failure(elementWire.Error());
            }

            valueWire = elementWire.Value();
        }

        auto reader = MapWireReader::Open(Move(opened.Value()), keyWire.Value(), valueWire,
                                          MaxArrayElements, ElementArrayShape(keyType.kind),
                                          structValue ? WireValueShape{}
                                                      : ElementArrayShape(schema.element.kind));
        if (!reader) {
            return Result<void>::Failure(reader.Error());
        }

        count = reader.Value().Count();
        if (auto decoded = DecodeMapEntries(construction, owner, target, reader.Value().Entries(),
                                            count, context, applyWork);
            !decoded) {
            return decoded;
        }

        if (auto finished = reader.Value().Finish(); !finished) {
            return finished;
        }
    }

    // 2단계 몫: 컨테이너 Reserve(count)의 저장소 byte(Container::ReservedBytes — 항목의 hash·정렬
    // padding과 2의 거듭제곱 용량을 담은 실제 할당 크기) + 항목 수 + 1 + 문자열 복사·weak 연결 몫.
    usize storage = 0;
    if (!Container::ReservedBytes(count, storage)) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    uint64 extra = 0;
    if (!TryAdd(uint64{storage}, applyWork, extra)) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    return ReserveArrayApply(context, count, 0, extra);
}

} // namespace rocks::detail
