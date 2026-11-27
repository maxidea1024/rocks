#include <Core/Memory/SystemAllocator.h>
#include <Serialization/InstancedStructWire.h>
#include <Serialization/MapWire.h>
#include <Serialization/OptionalWire.h>
#include <Serialization/SetWire.h>
#include <Serialization/TextWire.h>
#include <Serialization/ValueArray.h>
#include <TestSupport/Test.h>
#include <Platform/Memory.h>

using namespace rocks;
namespace wire_mutation_tests_file {

// wire 14–18을 섞은 Record 트리의 손상 입력 시험이다: 모든 byte를 바꾼 입력을 끝까지 읽어도 정해진
// 오류 코드로만 끝나고(경계 밖 읽기·넘침이 없다 — Asan 체인이 확인한다) 예산 한도 안에 머문다. 중첩
// Record 내용(Optional 내부·Map 값)은 깊이만큼 다시 청구되지 않는다(Docs/Design/
// ReflectionExtensions.ko.md 1.2절 — 리플렉션 B9a3).
namespace {

constexpr Guid TopSchema{18, 1};
constexpr Guid InnerSchema{18, 2};
constexpr Guid OptionalField{18, 3};
constexpr Guid MapField{18, 4};
constexpr Guid ArrayField{18, 5};
constexpr Guid TextMapField{18, 6};
constexpr Guid UnsetField{18, 7};
constexpr Guid SetMember{18, 8};
constexpr Guid TextMember{18, 9};
constexpr Guid OptionalMember{18, 10};
constexpr Guid LeafMember{18, 11};
constexpr Guid InstancedField{18, 12};
constexpr uint32 MaxCount = 64;

constexpr uint32 Wire(WireType type) noexcept {
    return static_cast<uint32>(type);
}

// 값 하나를 다 읽었는지 본다(남은 byte는 CorruptData).
Result<void> AtEnd(const ArchiveReader& reader) noexcept {
    return reader.AtEnd() ? Result<void>::Success() : Result<void>::Failure(ErrorCode::CorruptData);
}

// 안쪽 struct Record: Set<String>, Text(Localized), Optional<U32>.
Result<Array<uint8>> BuildInner(Allocator& allocator, uint32 seed) noexcept {
    ArchiveWriter set(allocator);
    auto setWriter = SetWireWriter::Begin(set, Wire(WireType::String), 2, 1024);
    if (!setWriter || !set.WriteString("x") ||
        !set.WriteString(seed == 0 ? StringView("zero") : StringView("seed")) ||
        !setWriter.Value().End()) {
        return Result<Array<uint8>>::Failure(ErrorCode::InvalidArgument);
    }

    ArchiveWriter text(allocator);
    TextWireValue localized;
    localized.form = TextWireForm::Localized;
    localized.textId = Guid{seed + 1, 2};
    localized.sourceRevision = seed + 1;
    localized.source = "문구";
    if (!WriteTextWire(text, localized)) {
        return Result<Array<uint8>>::Failure(ErrorCode::InvalidArgument);
    }

    ArchiveWriter optional(allocator);
    auto optionalWriter = OptionalWireWriter::BeginSet(optional, Wire(WireType::U32));
    if (!optionalWriter || !optional.WriteU32(seed) || !optionalWriter.Value().End()) {
        return Result<Array<uint8>>::Failure(ErrorCode::InvalidArgument);
    }

    RecordBuilder builder(allocator, InnerSchema, 1);
    if (!builder.AddField(SetMember, WireType::Set, set.View()) ||
        !builder.AddField(TextMember, WireType::Text, text.View()) ||
        !builder.AddField(OptionalMember, WireType::Optional, optional.View())) {
        return Result<Array<uint8>>::Failure(ErrorCode::InvalidArgument);
    }

    return builder.Build();
}

// 바깥 Record: Optional<struct>, Map<String, struct>, Array<InstancedStruct>, Map<U32, Text>,
// Optional<U64> unset, 필드 자리 InstancedStruct.
Result<Array<uint8>> BuildFixture(Allocator& allocator) noexcept {
    auto first = BuildInner(allocator, 0);
    auto second = BuildInner(allocator, 7);
    if (!first || !second) {
        return Result<Array<uint8>>::Failure(ErrorCode::InvalidArgument);
    }

    ArchiveWriter optional(allocator);
    auto optionalWriter = OptionalWireWriter::BeginSet(optional, Wire(WireType::Record));
    if (!optionalWriter || !optional.WriteRaw(first.Value().View()) ||
        !optionalWriter.Value().End()) {
        return Result<Array<uint8>>::Failure(ErrorCode::InvalidArgument);
    }

    ArchiveWriter map(allocator);
    auto mapWriter =
        MapWireWriter::Begin(map, Wire(WireType::String), Wire(WireType::Record), 2, 4096);
    if (!mapWriter) {
        return Result<Array<uint8>>::Failure(ErrorCode::InvalidArgument);
    }

    for (uint32 index = 0; index < 2; ++index) {
        if (!map.WriteString(index == 0 ? StringView("a") : StringView("bb"))) {
            return Result<Array<uint8>>::Failure(ErrorCode::InvalidArgument);
        }

        auto value = PrefixedRecordWriter::Begin(map, Wire(WireType::Record));
        if (!value || !map.WriteRaw(index == 0 ? first.Value().View() : second.Value().View()) ||
            !value.Value().End()) {
            return Result<Array<uint8>>::Failure(ErrorCode::InvalidArgument);
        }
    }

    if (!mapWriter.Value().End()) {
        return Result<Array<uint8>>::Failure(ErrorCode::InvalidArgument);
    }

    ArchiveWriter array(allocator);
    auto arrayWriter = ValueArrayWriter::Begin(array, Wire(WireType::InstancedStruct), 2, 4096);
    auto empty = PrefixedRecordWriter::Begin(array, Wire(WireType::InstancedStruct));
    if (!arrayWriter || !empty || !empty.Value().End()) {
        return Result<Array<uint8>>::Failure(ErrorCode::InvalidArgument);
    }

    auto filled = PrefixedRecordWriter::Begin(array, Wire(WireType::InstancedStruct));
    if (!filled || !array.WriteRaw(second.Value().View()) || !filled.Value().End() ||
        !arrayWriter.Value().End()) {
        return Result<Array<uint8>>::Failure(ErrorCode::InvalidArgument);
    }

    ArchiveWriter texts(allocator);
    auto textWriter =
        MapWireWriter::Begin(texts, Wire(WireType::U32), Wire(WireType::Text), 2, 4096);
    TextWireValue invariant;
    invariant.form = TextWireForm::Invariant;
    invariant.source = "inv";
    if (!textWriter || !texts.WriteU32(1) || !WriteTextWire(texts, TextWireValue{}) ||
        !texts.WriteU32(2) || !WriteTextWire(texts, invariant) || !textWriter.Value().End()) {
        return Result<Array<uint8>>::Failure(ErrorCode::InvalidArgument);
    }

    ArchiveWriter unset(allocator);
    if (!OptionalWireWriter::WriteUnset(unset)) {
        return Result<Array<uint8>>::Failure(ErrorCode::InvalidArgument);
    }

    RecordBuilder builder(allocator, TopSchema, 1);
    if (!builder.AddField(OptionalField, WireType::Optional, optional.View()) ||
        !builder.AddField(MapField, WireType::Map, map.View()) ||
        !builder.AddField(ArrayField, WireType::ValueArray, array.View()) ||
        !builder.AddField(TextMapField, WireType::Map, texts.View()) ||
        !builder.AddField(UnsetField, WireType::Optional, unset.View()) ||
        !builder.AddField(InstancedField, WireType::InstancedStruct, second.Value().View())) {
        return Result<Array<uint8>>::Failure(ErrorCode::InvalidArgument);
    }

    return builder.Build();
}

// 안쪽 struct Record의 모든 필드를 읽는다.
Result<void> ReadInner(RecordReader& inner) noexcept {
    while (true) {
        auto field = inner.Next();
        if (!field) {
            return field.Error() == ErrorCode::NotFound ? Result<void>::Success()
                                                        : Result<void>::Failure(field.Error());
        }

        const FieldView& view = field.Value();
        if (view.id == SetMember) {
            auto set = SetWireReader::OpenField(inner, view, Wire(WireType::String), MaxCount);
            if (!set) {
                return Result<void>::Failure(set.Error());
            }

            for (uint32 index = 0; index < set.Value().Count(); ++index) {
                if (auto element = set.Value().Elements().ReadStringView(); !element) {
                    return Result<void>::Failure(element.Error());
                }
            }

            if (auto finished = set.Value().Finish(); !finished) {
                return finished;
            }
        } else if (view.id == TextMember) {
            if (view.wireType != Wire(WireType::Text)) {
                return Result<void>::Failure(ErrorCode::TypeMismatch);
            }

            auto value = inner.OpenValue(view);
            if (!value) {
                return Result<void>::Failure(value.Error());
            }

            if (auto text = ReadTextWire(value.Value()); !text) {
                return Result<void>::Failure(text.Error());
            }

            if (auto finished = AtEnd(value.Value()); !finished) {
                return finished;
            }
        } else if (view.id == OptionalMember) {
            auto optional = OptionalWireReader::OpenField(inner, view, Wire(WireType::U32));
            if (!optional) {
                return Result<void>::Failure(optional.Error());
            }

            if (optional.Value().IsSet()) {
                if (auto number = optional.Value().Inner().ReadU32(); !number) {
                    return Result<void>::Failure(number.Error());
                }
            }

            if (auto finished = optional.Value().Finish(); !finished) {
                return finished;
            }
        } else {
            return Result<void>::Failure(ErrorCode::NotFound);
        }
    }
}

// parent 입력 안의 struct Record(record)를 같은 예산으로 열어 모두 읽는다.
Result<void> ReadNested(Allocator& allocator, RecordReader& parent, Span<const uint8> record,
                        Guid schema) noexcept {
    if (schema != InnerSchema) {
        return Result<void>::Failure(
            ErrorCode::NotFound); // 모르는 struct(codec은 TypeRegistry로 본다)
    }

    auto inner = parent.OpenNestedRecord(allocator, record, schema, 1, 1, 16);
    if (!inner) {
        return Result<void>::Failure(inner.Error());
    }

    return ReadInner(inner.Value());
}

// 바깥 Record의 모든 필드를 읽는다. 첫 오류 코드를 돌려준다.
Result<void> ReadTopFields(Allocator& allocator, RecordReader& top) noexcept {
    while (true) {
        auto field = top.Next();
        if (!field) {
            return field.Error() == ErrorCode::NotFound ? Result<void>::Success()
                                                        : Result<void>::Failure(field.Error());
        }

        const FieldView& view = field.Value();
        if (view.id == OptionalField) {
            auto optional = OptionalWireReader::OpenField(top, view, Wire(WireType::Record));
            if (!optional) {
                return Result<void>::Failure(optional.Error());
            }

            if (optional.Value().IsSet()) {
                auto read = ReadNested(allocator, top, optional.Value().RecordBytes(), InnerSchema);
                if (!read) {
                    return read;
                }
            }

            if (auto finished = optional.Value().Finish(); !finished) {
                return finished;
            }
        } else if (view.id == MapField) {
            auto map = MapWireReader::OpenField(top, view, Wire(WireType::String),
                                                Wire(WireType::Record), MaxCount);
            if (!map) {
                return Result<void>::Failure(map.Error());
            }

            for (uint32 index = 0; index < map.Value().Count(); ++index) {
                if (auto key = map.Value().Entries().ReadStringView(); !key) {
                    return Result<void>::Failure(key.Error());
                }

                auto value = ReadPrefixedRecord(map.Value().Entries());
                if (!value) {
                    return Result<void>::Failure(value.Error());
                }

                if (auto read = ReadNested(allocator, top, value.Value(), InnerSchema); !read) {
                    return read;
                }
            }

            if (auto finished = map.Value().Finish(); !finished) {
                return finished;
            }
        } else if (view.id == ArrayField) {
            auto array =
                ValueArrayReader::OpenField(top, view, Wire(WireType::InstancedStruct), MaxCount);
            if (!array) {
                return Result<void>::Failure(array.Error());
            }

            for (uint32 index = 0; index < array.Value().Count(); ++index) {
                auto content = ReadInstancedStructElement(array.Value().Elements());
                if (!content) {
                    return Result<void>::Failure(content.Error());
                }

                if (content.Value().IsEmpty()) {
                    continue;
                }

                auto read =
                    ReadNested(allocator, top, content.Value().record, content.Value().schema);
                if (!read) {
                    return read;
                }
            }

            if (auto finished = array.Value().Finish(); !finished) {
                return finished;
            }
        } else if (view.id == TextMapField) {
            auto map = MapWireReader::OpenField(top, view, Wire(WireType::U32),
                                                Wire(WireType::Text), MaxCount);
            if (!map) {
                return Result<void>::Failure(map.Error());
            }

            for (uint32 index = 0; index < map.Value().Count(); ++index) {
                if (auto key = map.Value().Entries().ReadU32(); !key) {
                    return Result<void>::Failure(key.Error());
                }

                if (auto text = ReadTextWire(map.Value().Entries()); !text) {
                    return Result<void>::Failure(text.Error());
                }
            }

            if (auto finished = map.Value().Finish(); !finished) {
                return finished;
            }
        } else if (view.id == InstancedField) {
            if (view.wireType != Wire(WireType::InstancedStruct)) {
                return Result<void>::Failure(ErrorCode::TypeMismatch);
            }

            auto value = top.OpenValue(view);
            if (!value) {
                return Result<void>::Failure(value.Error());
            }

            auto content = ReadInstancedStructField(value.Value());
            if (!content) {
                return Result<void>::Failure(content.Error());
            }

            if (!content.Value().IsEmpty()) {
                auto read =
                    ReadNested(allocator, top, content.Value().record, content.Value().schema);
                if (!read) {
                    return read;
                }
            }

            if (auto finished = AtEnd(value.Value()); !finished) {
                return finished;
            }
        } else if (view.id == UnsetField) {
            auto optional = OptionalWireReader::OpenField(top, view, Wire(WireType::U64));
            if (!optional) {
                return Result<void>::Failure(optional.Error());
            }

            if (optional.Value().IsSet()) {
                if (auto number = optional.Value().Inner().ReadU64(); !number) {
                    return Result<void>::Failure(number.Error());
                }
            }

            if (auto finished = optional.Value().Finish(); !finished) {
                return finished;
            }
        } else {
            return Result<void>::Failure(ErrorCode::NotFound);
        }
    }
}

ErrorCode ReadAll(Allocator& allocator, Span<const uint8> bytes, RecordLimits limits,
                  uint64& workUsed) noexcept {
    workUsed = 0;
    auto top = RecordReader::Open(allocator, bytes, TopSchema, 1, 1, limits);
    if (!top) {
        return top.Error();
    }

    auto read = ReadTopFields(allocator, top.Value());
    workUsed = top.Value().WorkUsed();
    return read ? ErrorCode::None : read.Error();
}

bool IsFormatError(ErrorCode code) noexcept {
    // 손상 입력은 CorruptData다(AssetSystem 19장 — 중복 id·키 포함).
    return code == ErrorCode::None || code == ErrorCode::CorruptData ||
           code == ErrorCode::TypeMismatch || code == ErrorCode::CapacityExceeded ||
           code == ErrorCode::Unsupported || code == ErrorCode::NotFound;
}

} // namespace

TEST(WireFormats_EveryByteMutationEndsInFormatErrorWithinBudget) {
    SystemAllocator allocator;
    auto fixture = BuildFixture(allocator);
    REQUIRE(fixture);
    // 예산 한도는 정상 입력의 작업량보다 넉넉히 크게 둔다. 그래서 한도에 닿으면(사용량이 한도로
    // 포화) 손상 입력이 작업량을 불린 것이다 — 아래 단언은 포화가 없고 작업량이 정상 입력의 상수 배
    // 안임을 본다.
    RecordLimits limits;
    limits.archive.maxWork = 1024 * 1024;
    uint64 baseline = 0;
    REQUIRE(ReadAll(allocator, fixture.Value().View(), limits, baseline) == ErrorCode::None);
    CHECK(baseline > fixture.Value().Size() && baseline < limits.archive.maxWork / 16);
    uint64 work = 0;
    uint64 largest = 0;

    Array<uint8> mutated(allocator);
    REQUIRE(mutated.TryAppend(fixture.Value().View()));
    usize accepted = 0;
    usize rejected = 0;
    for (usize offset = 0; offset < mutated.Size(); ++offset) {
        const uint8 original = mutated[offset];
        const uint8 replacements[] = {0, 0xff, static_cast<uint8>(original ^ 1),
                                      static_cast<uint8>(original + 1), 0x80};
        for (uint8 replacement : replacements) {
            if (replacement == original) {
                continue;
            }

            mutated[offset] = replacement;
            const ErrorCode result = ReadAll(allocator, mutated.View(), limits, work);
            CHECK(IsFormatError(result));
            CHECK(work < limits.archive.maxWork);
            largest = work > largest ? work : largest;
            if (result == ErrorCode::None) {
                ++accepted;
            } else {
                ++rejected;
            }
        }

        mutated[offset] = original;
    }

    CHECK(largest <= 2 * baseline);

    // 구조 byte를 바꾸면 거의 모두 거부되고, 문자열·값 byte처럼 형식을 깨지 않는 변경만 받는다.
    CHECK(rejected > accepted);
    CHECK(accepted > 0);
}

namespace {

// depth단 중첩: 가장 안쪽은 bytes 길이 Bytes 멤버의 Record, 바깥 단은 홀수면 Optional<struct>,
// 짝수면 Map<U32, struct> 한 항목으로 안쪽을 담는다(모든 단의 schema는 InnerSchema).
Result<Array<uint8>> BuildDeep(Allocator& allocator, usize depth, usize bytes) noexcept {
    Array<uint8> filler(allocator);
    if (!filler.TryAppendUninitialized(bytes)) {
        return Result<Array<uint8>>::Failure(ErrorCode::OutOfMemory);
    }

    platform::MemorySet(filler.Data(), 0x6b, filler.Size());
    ArchiveWriter leafValue(allocator);
    RecordBuilder leaf(allocator, InnerSchema, 1);
    if (!leafValue.WriteBytes(filler.View()) ||
        !leaf.AddField(LeafMember, WireType::Bytes, leafValue.View())) {
        return Result<Array<uint8>>::Failure(ErrorCode::InvalidArgument);
    }

    auto current = leaf.Build();
    if (!current) {
        return current;
    }

    for (usize level = 1; level < depth; ++level) {
        ArchiveWriter value(allocator);
        RecordBuilder outer(allocator, InnerSchema, 1);
        if (level % 2 == 1) {
            auto set = OptionalWireWriter::BeginSet(value, Wire(WireType::Record));
            if (!set || !value.WriteRaw(current.Value().View()) || !set.Value().End() ||
                !outer.AddField(OptionalMember, WireType::Optional, value.View())) {
                return Result<Array<uint8>>::Failure(ErrorCode::InvalidArgument);
            }
        } else {
            auto map = MapWireWriter::Begin(value, Wire(WireType::U32), Wire(WireType::Record), 1,
                                            4 * 1024 * 1024);
            if (!map || !value.WriteU32(static_cast<uint32>(level))) {
                return Result<Array<uint8>>::Failure(ErrorCode::InvalidArgument);
            }

            auto prefixed = PrefixedRecordWriter::Begin(value, Wire(WireType::Record));
            if (!prefixed || !value.WriteRaw(current.Value().View()) || !prefixed.Value().End() ||
                !map.Value().End() || !outer.AddField(SetMember, WireType::Map, value.View())) {
                return Result<Array<uint8>>::Failure(ErrorCode::InvalidArgument);
            }
        }

        auto built = outer.Build();
        if (!built) {
            return built;
        }

        current = Move(built);
    }

    return current;
}

} // namespace

TEST(WireFormats_NestedRecordContentIsChargedOnce) {
    SystemAllocator allocator;
    constexpr usize Depth = 12;
    constexpr usize Bytes = 64 * 1024;
    auto deep = BuildDeep(allocator, Depth, Bytes);
    REQUIRE(deep);
    auto top = RecordReader::Open(allocator, deep.Value().View(), InnerSchema, 1, 1);
    REQUIRE(top);
    const uint64 afterOpen = top.Value().WorkUsed();
    CHECK(afterOpen >= Bytes);

    Array<RecordReader> chain(allocator);
    REQUIRE(chain.TryReserve(Depth));
    RecordReader* current = &top.Value();
    for (usize level = Depth - 1; level >= 1; --level) {
        auto field = current->Next();
        REQUIRE(field);
        Span<const uint8> record;
        if (level % 2 == 1) {
            auto optional =
                OptionalWireReader::OpenField(*current, field.Value(), Wire(WireType::Record));
            REQUIRE(optional && optional.Value().IsSet() && optional.Value().Finish());
            record = optional.Value().RecordBytes();
        } else {
            auto map = MapWireReader::OpenField(*current, field.Value(), Wire(WireType::U32),
                                                Wire(WireType::Record), 1);
            REQUIRE(map && map.Value().Count() == 1);
            REQUIRE(map.Value().Entries().ReadU32());
            auto value = ReadPrefixedRecord(map.Value().Entries());
            REQUIRE(value && map.Value().Finish());
            record = value.Value();
        }

        auto nested = current->OpenNestedRecord(allocator, record, InnerSchema, 1, 1, 4);
        REQUIRE(nested);
        REQUIRE(chain.TryPushBack(Move(nested.Value())));
        current = &chain[chain.Size() - 1];
    }

    // 11단을 더 열어도 64 KiB 내용을 다시 청구하지 않는다(단마다 색인·머리 비용만).
    CHECK(current->WorkUsed() - afterOpen < 8192);
    auto leaf = current->Next();
    REQUIRE(leaf && leaf.Value().id == LeafMember);
    auto value = current->OpenValue(leaf.Value());
    REQUIRE(value);
    auto read = value.Value().ReadBytes();
    CHECK(read && read.Value().Size() == Bytes);
}
} // namespace wire_mutation_tests_file
