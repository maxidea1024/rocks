#include <Core/Memory/SystemAllocator.h>
#include <Serialization/RecordList.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>
#include <Platform/Thread.h>

using namespace rocks;

// RecordList(WireType::RecordList)의 형식·한도·예산 공유·실패 원자성
// 시험이다(CoreDataFoundations.ko.md 4절).
namespace {

constexpr Guid ParentSchema{11, 1};
constexpr Guid ItemSchema{11, 2};
constexpr Guid ItemsField{11, 3};
constexpr Guid ValueField{11, 4};
constexpr Guid ChildrenField{11, 5};

// 값 필드 하나(u64)를 가진 항목 Record.
Result<Array<uint8>> BuildItem(Allocator& allocator, uint64 value,
                               Guid schema = ItemSchema) noexcept {
    ArchiveWriter payload(allocator);
    if (auto written = payload.WriteU64(value); !written) {
        return Result<Array<uint8>>::Failure(written.Error());
    }

    RecordBuilder builder(allocator, schema, 1);
    if (auto added = builder.AddField(ValueField, WireType::U64, payload.View()); !added) {
        return Result<Array<uint8>>::Failure(added.Error());
    }

    return builder.Build();
}

Result<uint64> ReadValue(RecordReader& item) noexcept {
    auto field = item.Find(ValueField);
    if (!field) {
        return Result<uint64>::Failure(field.Error());
    }

    auto archive = item.OpenValue(field.Value()); // 트리 예산으로 값을 읽는다
    if (!archive) {
        return Result<uint64>::Failure(archive.Error());
    }

    return archive.Value().ReadU64();
}

// 목록 payload를 직접 조립한다: u32 count, 그 뒤 (u64 length, bytes)들.
void AppendU32(Array<uint8>& out, uint32 value) noexcept {
    for (int index = 0; index < 4; ++index) {
        static_cast<void>(out.TryPushBack(static_cast<uint8>(value >> (8 * index))));
    }
}

void AppendU64(Array<uint8>& out, uint64 value) noexcept {
    for (int index = 0; index < 8; ++index) {
        static_cast<void>(out.TryPushBack(static_cast<uint8>(value >> (8 * index))));
    }
}

void AppendBytes(Array<uint8>& out, Span<const uint8> bytes) noexcept {
    static_cast<void>(out.TryAppend(bytes));
}

bool SameBytes(Span<const uint8> left, Span<const uint8> right) noexcept {
    return left.Size() == right.Size() &&
           (left.Size() == 0 || platform::MemoryEquals(left.Data(), right.Data(), left.Size()));
}

// 전체 크기가 정확히 totalBytes인 항목 Record(Bytes 필드 하나). 헤더 44 + TLV 28 + 길이 8 + 데이터.
Result<Array<uint8>> BuildSizedItem(Allocator& allocator, usize totalBytes) noexcept {
    constexpr usize Overhead = 44 + 28 + 8;
    if (totalBytes < Overhead) {
        return Result<Array<uint8>>::Failure(ErrorCode::InvalidArgument);
    }

    Array<uint8> data(allocator);
    auto filled = data.TryAppendUninitialized(totalBytes - Overhead);
    if (!filled) {
        return Result<Array<uint8>>::Failure(filled.Error());
    }

    platform::MemorySet(data.Data(), 0x5A, data.Size());
    ArchiveWriter payload(allocator);
    if (auto written = payload.WriteBytes(data.View()); !written) {
        return Result<Array<uint8>>::Failure(written.Error());
    }

    RecordBuilder builder(allocator, ItemSchema, 1);
    if (auto added = builder.AddField(ValueField, WireType::Bytes, payload.View()); !added) {
        return Result<Array<uint8>>::Failure(added.Error());
    }

    return builder.Build();
}

// builder를 부모 Record에 쓰고 다시 열어 목록 필드 payload를 돌려준다(parentBytes를 빌린다).
Result<Span<const uint8>> FinishAndFind(Allocator& allocator, RecordListBuilder& builder,
                                        Array<uint8>& parentBytes) noexcept {
    RecordBuilder parent(allocator, ParentSchema, 1);
    if (auto finished = builder.Finish(parent, ItemsField); !finished) {
        return Result<Span<const uint8>>::Failure(finished.Error());
    }

    auto built = parent.Build();
    if (!built) {
        return Result<Span<const uint8>>::Failure(built.Error());
    }

    parentBytes = Move(built.Value());
    auto reader = RecordReader::Open(allocator, parentBytes.View(), ParentSchema, 1, 1);
    if (!reader) {
        return Result<Span<const uint8>>::Failure(reader.Error());
    }

    auto field = reader.Value().Find(ItemsField);
    if (!field) {
        return Result<Span<const uint8>>::Failure(field.Error());
    }

    return Result<Span<const uint8>>::Success(field.Value().payload);
}

} // namespace

TEST(RecordList_RoundTripEmptyAndNested) {
    SystemAllocator allocator;
    // 항목 3개를 부모 Record의 목록 필드로 쓰고 다시 읽는다.
    RecordListBuilder list(allocator);
    for (uint64 value = 10; value < 13; ++value) {
        auto item = BuildItem(allocator, value);
        REQUIRE(item && list.AddItem(item.Value().View()));
    }

    CHECK(list.ItemCount() == 3);
    RecordBuilder parent(allocator, ParentSchema, 1);
    REQUIRE(list.Finish(parent, ItemsField));
    auto parentBytes = parent.Build();
    REQUIRE(parentBytes);
    auto parentReader =
        RecordReader::Open(allocator, parentBytes.Value().View(), ParentSchema, 1, 1);
    REQUIRE(parentReader);
    auto field = parentReader.Value().Find(ItemsField);
    REQUIRE(field && field.Value().wireType == static_cast<uint32>(WireType::RecordList));
    auto items = RecordListReader::Open(allocator, field.Value().payload, ItemSchema, 1, 1);
    REQUIRE(items && items.Value().ItemCount() == 3);
    for (usize index = 0; index < 3; ++index) {
        auto item = items.Value().OpenItem(index);
        REQUIRE(item);
        auto value = ReadValue(item.Value());
        CHECK(value && value.Value() == 10 + index);
    }

    CHECK(items.Value().OpenItem(3).Error() == ErrorCode::InvalidArgument);
    // 항목 schema·버전 검사는 OpenItem이 한다.
    auto wrongSchema = RecordListReader::Open(allocator, field.Value().payload, ParentSchema, 1, 1);
    REQUIRE(wrongSchema);
    CHECK(wrongSchema.Value().OpenItem(0).Error() == ErrorCode::TypeMismatch);
    auto wrongVersion = RecordListReader::Open(allocator, field.Value().payload, ItemSchema, 2, 3);
    REQUIRE(wrongVersion);
    CHECK(wrongVersion.Value().OpenItem(0).Error() == ErrorCode::Unsupported);

    // 빈 목록도 count 0으로 기록한다(payload 4 byte).
    RecordListBuilder empty(allocator);
    RecordBuilder emptyParent(allocator, ParentSchema, 1);
    REQUIRE(empty.Finish(emptyParent, ItemsField));
    auto emptyBytes = emptyParent.Build();
    REQUIRE(emptyBytes);
    auto emptyReader = RecordReader::Open(allocator, emptyBytes.Value().View(), ParentSchema, 1, 1);
    REQUIRE(emptyReader);
    auto emptyField = emptyReader.Value().Find(ItemsField);
    REQUIRE(emptyField && emptyField.Value().payload.Size() == 4);
    auto emptyItems =
        RecordListReader::Open(allocator, emptyField.Value().payload, ItemSchema, 1, 1);
    CHECK(emptyItems && emptyItems.Value().ItemCount() == 0);

    // 중첩: 항목 Record가 자기 목록 필드를 갖는다.
    RecordListBuilder inner(allocator);
    auto leaf = BuildItem(allocator, 99);
    REQUIRE(leaf && inner.AddItem(leaf.Value().View()));
    RecordBuilder branch(allocator, ItemSchema, 1);
    REQUIRE(inner.Finish(branch, ChildrenField));
    auto branchBytes = branch.Build();
    REQUIRE(branchBytes);
    RecordListBuilder outer(allocator);
    REQUIRE(outer.AddItem(branchBytes.Value().View()));
    RecordBuilder root(allocator, ParentSchema, 1);
    REQUIRE(outer.Finish(root, ItemsField));
    auto rootBytes = root.Build();
    REQUIRE(rootBytes);
    auto rootReader = RecordReader::Open(allocator, rootBytes.Value().View(), ParentSchema, 1, 1);
    REQUIRE(rootReader);
    auto outerField = rootReader.Value().Find(ItemsField);
    REQUIRE(outerField);
    auto outerItems =
        RecordListReader::Open(allocator, outerField.Value().payload, ItemSchema, 1, 1);
    REQUIRE(outerItems);
    auto branchReader = outerItems.Value().OpenItem(0);
    REQUIRE(branchReader);
    auto children = branchReader.Value().Find(ChildrenField);
    REQUIRE(children && children.Value().wireType == static_cast<uint32>(WireType::RecordList));
    auto innerItems = RecordListReader::Open(allocator, children.Value().payload, ItemSchema, 1, 1);
    REQUIRE(innerItems);
    auto leafReader = innerItems.Value().OpenItem(0);
    REQUIRE(leafReader);
    auto leafValue = ReadValue(leafReader.Value());
    CHECK(leafValue && leafValue.Value() == 99);
}

TEST(RecordList_MalformedPayloads) {
    SystemAllocator allocator;
    auto item = BuildItem(allocator, 7);
    REQUIRE(item);
    const Span<const uint8> record = item.Value().View();
    auto openPayload = [&allocator](const Array<uint8>& payload) noexcept {
        auto opened = RecordListReader::Open(allocator, payload.View(), ItemSchema, 1, 1);
        return opened ? ErrorCode::None : opened.Error();
    };

    Array<uint8> valid(allocator);
    AppendU32(valid, 1);
    AppendU64(valid, record.Size());
    AppendBytes(valid, record);
    CHECK(openPayload(valid) == ErrorCode::None);

    // count가 남은 byte로 담을 수 없을 만큼 크면 할당 전에 InvalidArgument다.
    Array<uint8> countTooLarge(allocator);
    AppendU32(countTooLarge, 1000);
    AppendU64(countTooLarge, record.Size());
    AppendBytes(countTooLarge, record);
    CHECK(openPayload(countTooLarge) == ErrorCode::InvalidArgument);

    // 잘린 count·length·항목.
    Array<uint8> truncated(allocator);
    AppendU32(truncated, 1);
    AppendU64(truncated, record.Size());
    AppendBytes(truncated, Span<const uint8>(record.Data(), record.Size() - 1));
    CHECK(openPayload(truncated) == ErrorCode::InvalidArgument);
    Array<uint8> shortCount(allocator);
    AppendBytes(shortCount, Span<const uint8>(valid.Data(), 3));
    CHECK(openPayload(shortCount) == ErrorCode::InvalidArgument);

    // 남은 byte.
    Array<uint8> trailing(allocator);
    AppendBytes(trailing, valid.View());
    static_cast<void>(trailing.TryPushBack(0));
    CHECK(openPayload(trailing) == ErrorCode::InvalidArgument);

    // length가 중첩 Record 길이와 다르다(뒤에 여분 byte를 붙이고 length에 포함).
    Array<uint8> lengthMismatch(allocator);
    AppendU32(lengthMismatch, 1);
    AppendU64(lengthMismatch, record.Size() + 1);
    AppendBytes(lengthMismatch, record);
    static_cast<void>(lengthMismatch.TryPushBack(0));
    CHECK(openPayload(lengthMismatch) == ErrorCode::InvalidArgument);

    // 헤더보다 짧은 항목·잘못된 magic.
    Array<uint8> tiny(allocator);
    AppendU32(tiny, 1);
    AppendU64(tiny, 40);
    AppendBytes(tiny, Span<const uint8>(record.Data(), 40));
    AppendBytes(tiny, Span<const uint8>(record.Data(), 12)); // 최소 크기 검사를 통과할 만큼 채운다
    CHECK(openPayload(tiny) == ErrorCode::InvalidArgument);
    Array<uint8> badMagic(allocator);
    AppendBytes(badMagic, valid.View());
    badMagic[12] = 'X';
    CHECK(openPayload(badMagic) == ErrorCode::InvalidArgument);

    // builder도 envelope가 아닌 입력을 거부한다.
    RecordListBuilder builder(allocator);
    CHECK(builder.AddItem(Span<const uint8>(record.Data(), record.Size() - 1)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(builder.AddItem(Span<const uint8>(record.Data(), 10)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(builder.ItemCount() == 0);
}

TEST(RecordList_LimitsAndSharedBudget) {
    SystemAllocator allocator;
    auto item = BuildItem(allocator, 1);
    REQUIRE(item);
    // 항목 수·항목 크기 한도(builder와 reader 모두).
    RecordListLimits limits;
    limits.maxItems = 2;
    RecordListBuilder builder(allocator, limits);
    REQUIRE(builder.AddItem(item.Value().View()) && builder.AddItem(item.Value().View()));
    CHECK(builder.AddItem(item.Value().View()).Error() == ErrorCode::CapacityExceeded);
    RecordListLimits small;
    small.maxItemBytes = item.Value().Size() - 1;
    RecordListBuilder smallBuilder(allocator, small);
    CHECK(smallBuilder.AddItem(item.Value().View()).Error() == ErrorCode::CapacityExceeded);

    RecordListBuilder three(allocator);
    for (int index = 0; index < 3; ++index) {
        REQUIRE(three.AddItem(item.Value().View()));
    }

    RecordBuilder parent(allocator, ParentSchema, 1);
    REQUIRE(three.Finish(parent, ItemsField));
    auto parentBytes = parent.Build();
    REQUIRE(parentBytes);
    auto parentReader =
        RecordReader::Open(allocator, parentBytes.Value().View(), ParentSchema, 1, 1);
    REQUIRE(parentReader);
    const Span<const uint8> payload = parentReader.Value().Find(ItemsField).Value().payload;
    CHECK(RecordListReader::Open(allocator, payload, ItemSchema, 1, 1, {}, limits).Error() ==
          ErrorCode::CapacityExceeded);
    CHECK(RecordListReader::Open(allocator, payload, ItemSchema, 1, 1, {}, small).Error() ==
          ErrorCode::CapacityExceeded);

    // 예산 공유: 목록 파싱부터 청구하고, 항목 reader를 열 때마다·항목 reader가 조회할 때마다 같은
    // 누적 operation·work가 는다(새 예산을 받지 않는다).
    auto measured = RecordListReader::Open(allocator, payload, ItemSchema, 1, 1);
    REQUIRE(measured);
    const uint64 listOperations = measured.Value().OperationsUsed();
    const uint64 listWork = measured.Value().WorkUsed();
    CHECK(listOperations > 0 && listWork > 0);
    auto first = measured.Value().OpenItem(0);
    REQUIRE(first);
    const uint64 perItem = measured.Value().OperationsUsed() - listOperations;
    const uint64 perItemWork = measured.Value().WorkUsed() - listWork;
    CHECK(perItem > 0 && perItemWork > 0);
    const uint64 beforeFind = measured.Value().OperationsUsed();
    const uint64 beforeFindWork = measured.Value().WorkUsed();
    REQUIRE(first.Value().Find(ValueField));
    CHECK(measured.Value().OperationsUsed() > beforeFind &&
          measured.Value().WorkUsed() > beforeFindWork);
    CHECK(first.Value().OperationsUsed() == measured.Value().OperationsUsed());
    // 범위 밖 OpenItem도 operation을 소비한다.
    const uint64 beforeMiss = measured.Value().OperationsUsed();
    CHECK(measured.Value().OpenItem(99).Error() == ErrorCode::InvalidArgument);
    CHECK(measured.Value().OperationsUsed() == beforeMiss + 1);

    // operation 한도: 한 항목씩은 충분한 한도라도 누적이 넘으면 CapacityExceeded다.
    RecordLimits operationBudget;
    operationBudget.archive.maxOperations = listOperations + (perItem + 1) * 2;
    auto limited = RecordListReader::Open(allocator, payload, ItemSchema, 1, 1, operationBudget);
    REQUIRE(limited);
    CHECK(limited.Value().OpenItem(0) && limited.Value().OpenItem(1));
    CHECK(limited.Value().OpenItem(2).Error() == ErrorCode::CapacityExceeded);
    auto alone =
        RecordReader::Open(allocator, item.Value().View(), ItemSchema, 1, 1, operationBudget);
    CHECK(alone); // 공유가 없으면 통과했을 한도다
    // work 한도도 같다.
    RecordLimits workBudget;
    workBudget.archive.maxWork = listWork + perItemWork * 2;
    auto workLimited = RecordListReader::Open(allocator, payload, ItemSchema, 1, 1, workBudget);
    REQUIRE(workLimited);
    CHECK(workLimited.Value().OpenItem(0) && workLimited.Value().OpenItem(1));
    CHECK(workLimited.Value().OpenItem(2).Error() == ErrorCode::CapacityExceeded);
}

TEST(RecordList_LimitBoundaries) {
    SystemAllocator allocator;
    auto item = BuildItem(allocator, 1);
    REQUIRE(item);
    const Span<const uint8> record = item.Value().View();
    const usize size = record.Size();
    auto addResult = [&](RecordListLimits limits, int count) noexcept {
        RecordListBuilder builder(allocator, limits);
        for (int index = 0; index < count - 1; ++index) {
            if (!builder.AddItem(record)) {
                return ErrorCode::DependencyFailed; // 준비 단계 실패(검사 대상과 구분)
            }
        }

        auto added = builder.AddItem(record);
        return added ? ErrorCode::None : added.Error();
    };

    // builder: 각 한도와 정확히 같으면 통과하고 한 byte·한 개 넘으면 CapacityExceeded다.
    RecordListLimits itemBytes;
    itemBytes.maxItemBytes = size;
    CHECK(addResult(itemBytes, 1) == ErrorCode::None);
    itemBytes.maxItemBytes = size - 1;
    CHECK(addResult(itemBytes, 1) == ErrorCode::CapacityExceeded);
    RecordListLimits payloadBytes;
    payloadBytes.maxPayloadBytes = 4 + 8 + size;
    CHECK(addResult(payloadBytes, 1) == ErrorCode::None);
    CHECK(addResult(payloadBytes, 2) == ErrorCode::CapacityExceeded);
    payloadBytes.maxPayloadBytes = 4 + 8 + size - 1;
    CHECK(addResult(payloadBytes, 1) == ErrorCode::CapacityExceeded);
    payloadBytes.maxPayloadBytes = 0; // count 자리도 없는 한도(감산이 넘치지 않는다)
    CHECK(addResult(payloadBytes, 1) == ErrorCode::CapacityExceeded);
    // 그 한도로는 빈 목록(count 4 byte)도 쓰지 않는다(reader도 거부한다).
    RecordListBuilder emptyTooSmall(allocator, payloadBytes);
    RecordBuilder emptyParent(allocator, ParentSchema, 1);
    CHECK(emptyTooSmall.Finish(emptyParent, ItemsField).Error() == ErrorCode::CapacityExceeded);
    CHECK(emptyParent.FieldCount() == 0);
    const uint8 zeroCount[4] = {0, 0, 0, 0};
    CHECK(RecordListReader::Open(allocator, Span<const uint8>(zeroCount, 4), ItemSchema, 1, 1, {},
                                 payloadBytes)
              .Error() == ErrorCode::CapacityExceeded);
    RecordListLimits noItems;
    noItems.maxItems = 0;
    CHECK(addResult(noItems, 1) == ErrorCode::CapacityExceeded);

    // reader: 같은 경계를 payload에 적용한다.
    RecordListBuilder three(allocator);
    for (int index = 0; index < 3; ++index) {
        REQUIRE(three.AddItem(record));
    }

    Array<uint8> parentBytes(allocator);
    auto payload = FinishAndFind(allocator, three, parentBytes);
    REQUIRE(payload && payload.Value().Size() == 4 + 3 * (8 + size));
    auto openResult = [&](RecordListLimits limits) noexcept {
        auto opened =
            RecordListReader::Open(allocator, payload.Value(), ItemSchema, 1, 1, {}, limits);
        return opened ? ErrorCode::None : opened.Error();
    };

    RecordListLimits readerLimits;
    readerLimits.maxItems = 3;
    CHECK(openResult(readerLimits) == ErrorCode::None);
    readerLimits.maxItems = 2;
    CHECK(openResult(readerLimits) == ErrorCode::CapacityExceeded);
    readerLimits = {};
    readerLimits.maxItemBytes = size;
    CHECK(openResult(readerLimits) == ErrorCode::None);
    readerLimits.maxItemBytes = size - 1;
    CHECK(openResult(readerLimits) == ErrorCode::CapacityExceeded);
    readerLimits = {};
    readerLimits.maxPayloadBytes = payload.Value().Size();
    CHECK(openResult(readerLimits) == ErrorCode::None);
    readerLimits.maxPayloadBytes = payload.Value().Size() - 1;
    CHECK(openResult(readerLimits) == ErrorCode::CapacityExceeded);
}

TEST(RecordList_DefaultLimitsCompose) {
    // 기본 한도로 만든 가장 큰 항목(4 MiB - 12) 하나짜리 목록은 기본 한도의 부모 Record에 들어가고,
    // 기본 한도로 끝까지(부모 → 목록 → 항목 → 값) 다시 읽힌다.
    SystemAllocator allocator;
    const usize largest = RecordListLimits{}.maxItemBytes;
    CHECK(RecordListLimits{}.maxPayloadBytes == ArchiveLimits{}.maxValueBytes);
    CHECK(largest == RecordListLimits{}.maxPayloadBytes - 12);
    auto item = BuildSizedItem(allocator, largest);
    REQUIRE(item && item.Value().Size() == largest);
    RecordListBuilder builder(allocator);
    REQUIRE(builder.AddItem(item.Value().View()));
    Array<uint8> parentBytes(allocator);
    REQUIRE(FinishAndFind(allocator, builder, parentBytes));
    auto parent = RecordReader::Open(allocator, parentBytes.View(), ParentSchema, 1, 1);
    REQUIRE(parent);
    auto field = parent.Value().Find(ItemsField);
    REQUIRE(field && field.Value().payload.Size() == ArchiveLimits{}.maxValueBytes);
    auto items = RecordListReader::OpenField(parent.Value(), field.Value(), ItemSchema, 1, 1);
    REQUIRE(items && items.Value().ItemCount() == 1);
    auto opened = items.Value().OpenItem(0);
    REQUIRE(opened);
    auto valueField = opened.Value().Find(ValueField);
    REQUIRE(valueField);
    auto value = opened.Value().OpenValue(valueField.Value());
    REQUIRE(value);
    auto bytes = value.Value().ReadBytes();
    CHECK(bytes && bytes.Value().Size() == largest - 80 && value.Value().AtEnd());

    // 한 byte 더 큰 항목은 기본 builder 한도에서 거부된다.
    auto tooLarge = BuildSizedItem(allocator, largest + 1);
    REQUIRE(tooLarge);
    RecordListBuilder rejecting(allocator);
    CHECK(rejecting.AddItem(tooLarge.Value().View()).Error() == ErrorCode::CapacityExceeded);
}

TEST(RecordList_CountPrechecksRunBeforeAllocationAndParsing) {
    SystemAllocator backing;
    auto item = BuildItem(backing, 2);
    REQUIRE(item);
    // count가 uint32 최대이고 maxItems·예산을 모두 풀어도, 남은 byte / 52 검사가 index 할당 전에
    // InvalidArgument로 끊는다(할당은 공유 예산 하나뿐). 예산을 풀어 두므로 work 청구가 대신 끊어
    // 주는 경우와 구분된다.
    Array<uint8> huge(backing);
    AppendU32(huge, 0xFFFFFFFFu);
    AppendU64(huge, item.Value().Size());
    AppendBytes(huge, item.Value().View());
    RecordListLimits unlimitedCount;
    unlimitedCount.maxItems = 0xFFFFFFFFu;
    RecordLimits unlimitedBudget;
    unlimitedBudget.archive.maxOperations = ~0ull;
    unlimitedBudget.archive.maxWork = ~0ull;
    test::NFailAllocator counting(backing);
    counting.DisableFailure();
    auto opened = RecordListReader::Open(counting, huge.View(), ItemSchema, 1, 1, unlimitedBudget,
                                         unlimitedCount);
    CHECK(!opened && opened.Error() == ErrorCode::InvalidArgument);
    CHECK(counting.Attempts() == 1);

    // count만큼의 work를 항목 index 할당·파싱 전에 청구한다: 0으로 채운 1000 × 52 byte는 첫
    // length에서 InvalidArgument가 될 입력이지만, work 한도가 count보다 작으면 먼저
    // CapacityExceeded이며 index는 할당하지 않는다.
    Array<uint8> junk(backing);
    AppendU32(junk, 1000);
    REQUIRE(junk.TryAppendUninitialized(1000 * 52));
    platform::MemorySet(junk.Data() + 4, 0, 1000 * 52);
    CHECK(RecordListReader::Open(backing, junk.View(), ItemSchema, 1, 1).Error() ==
          ErrorCode::InvalidArgument);
    RecordLimits smallWork;
    smallWork.archive.maxWork = 500;
    counting.DisableFailure();
    CHECK(RecordListReader::Open(counting, junk.View(), ItemSchema, 1, 1, smallWork).Error() ==
          ErrorCode::CapacityExceeded);
    CHECK(counting.Attempts() == 1);
}

TEST(RecordList_BuilderPayloadMatchesFormat) {
    // builder 출력은 문서화된 형식을 손으로 조립한 bytes와 byte 단위로 같다.
    SystemAllocator allocator;
    auto first = BuildItem(allocator, 21);
    auto second = BuildSizedItem(allocator, 97);
    REQUIRE(first && second);
    RecordListBuilder builder(allocator);
    REQUIRE(builder.AddItem(first.Value().View()) && builder.AddItem(second.Value().View()));
    Array<uint8> expected(allocator);
    AppendU32(expected, 2);
    AppendU64(expected, first.Value().Size());
    AppendBytes(expected, first.Value().View());
    AppendU64(expected, second.Value().Size());
    AppendBytes(expected, second.Value().View());
    Array<uint8> parentBytes(allocator);
    auto payload = FinishAndFind(allocator, builder, parentBytes);
    REQUIRE(payload);
    CHECK(SameBytes(payload.Value(), expected.View()));
    // Finish 뒤에도 항목이 남아 같은 bytes를 다시 쓴다.
    Array<uint8> againBytes(allocator);
    auto again = FinishAndFind(allocator, builder, againBytes);
    REQUIRE(again);
    CHECK(SameBytes(again.Value(), expected.View()));

    // 빈 목록은 count 0 하나다.
    RecordListBuilder empty(allocator);
    const uint8 zero[4] = {0, 0, 0, 0};
    Array<uint8> emptyBytes(allocator);
    auto emptyPayload = FinishAndFind(allocator, empty, emptyBytes);
    REQUIRE(emptyPayload);
    CHECK(SameBytes(emptyPayload.Value(), Span<const uint8>(zero, 4)));
}

TEST(RecordList_BudgetOwnershipSurvivesMovesAndDestruction) {
    SystemAllocator allocator;
    auto item = BuildItem(allocator, 3);
    REQUIRE(item);
    RecordListBuilder builder(allocator);
    REQUIRE(builder.AddItem(item.Value().View()));
    RecordBuilder parent(allocator, ParentSchema, 1);
    REQUIRE(builder.Finish(parent, ItemsField));
    auto parentBytes = parent.Build();
    REQUIRE(parentBytes);
    auto parentReader =
        RecordReader::Open(allocator, parentBytes.Value().View(), ParentSchema, 1, 1);
    REQUIRE(parentReader);
    const Span<const uint8> payload = parentReader.Value().Find(ItemsField).Value().payload;

    // 이동 전에 연 항목 reader가 이동 생성·이동 대입 뒤의 목록과 같은 예산을 쓴다(값 복사가
    // 아니다).
    auto list = RecordListReader::Open(allocator, payload, ItemSchema, 1, 1);
    REQUIRE(list);
    auto opened = list.Value().OpenItem(0);
    REQUIRE(opened);
    RecordListReader constructed(Move(list.Value()));
    uint64 before = constructed.OperationsUsed();
    REQUIRE(opened.Value().Find(ValueField));
    CHECK(constructed.OperationsUsed() > before);
    auto other = RecordListReader::Open(allocator, payload, ItemSchema, 1, 1);
    REQUIRE(other);
    RecordListReader assigned(Move(other.Value()));
    assigned = Move(constructed);
    before = assigned.OperationsUsed();
    REQUIRE(opened.Value().Find(ValueField));
    CHECK(assigned.OperationsUsed() > before);
    // 이동된 목록은 빈 reader다.
    CHECK(constructed.ItemCount() == 0 &&
          constructed.OpenItem(0).Error() == ErrorCode::InvalidArgument);

    // 목록 reader가 먼저 파괴돼도 항목 reader는 안전하다(예산은 참조 계수로 공유된다).
    RecordReader survivor = [&]() noexcept {
        auto scoped = RecordListReader::Open(allocator, payload, ItemSchema, 1, 1);
        auto opened = scoped.Value().OpenItem(0);
        return Move(opened.Value());
    }();
    auto value = ReadValue(survivor);
    CHECK(value && value.Value() == 3);

    // 이동된 ArchiveReader 원본은 빈 입력이다(남은 예산으로 같은 입력을 다시 읽지 못한다).
    auto archive = ArchiveReader::Create(item.Value().View());
    REQUIRE(archive);
    ArchiveReader target = Move(archive.Value());
    CHECK(archive.Value().Remaining() == 0 && target.Remaining() == item.Value().Size());
}

TEST(RecordList_TreeSharesOneBudget) {
    SystemAllocator allocator;
    // 부모 → 목록(OpenField) → 항목 → 중첩 목록(OpenField) → 값(OpenValue)이 모두 부모 Record의
    // 예산이다.
    RecordListBuilder inner(allocator);
    for (uint64 value = 0; value < 4; ++value) {
        auto leaf = BuildItem(allocator, value);
        REQUIRE(leaf && inner.AddItem(leaf.Value().View()));
    }

    RecordListBuilder outer(allocator);
    for (int branchIndex = 0; branchIndex < 4; ++branchIndex) {
        RecordBuilder branch(allocator, ItemSchema, 1);
        REQUIRE(inner.Finish(branch, ChildrenField));
        auto branchBytes = branch.Build();
        REQUIRE(branchBytes && outer.AddItem(branchBytes.Value().View()));
    }

    RecordBuilder root(allocator, ParentSchema, 1);
    REQUIRE(outer.Finish(root, ItemsField));
    auto rootBytes = root.Build();
    REQUIRE(rootBytes);

    // 순회 한 번에 드는 예산을 잰다.
    auto traverse = [&](RecordReader& reader, uint64& leaves) noexcept -> Result<void> {
        auto field = reader.Find(ItemsField);
        if (!field) {
            return Result<void>::Failure(field.Error());
        }

        auto branches = RecordListReader::OpenField(reader, field.Value(), ItemSchema, 1, 1);
        if (!branches) {
            return Result<void>::Failure(branches.Error());
        }

        for (usize index = 0; index < branches.Value().ItemCount(); ++index) {
            auto branch = branches.Value().OpenItem(index);
            if (!branch) {
                return Result<void>::Failure(branch.Error());
            }

            auto children = branch.Value().Find(ChildrenField);
            if (!children) {
                return Result<void>::Failure(children.Error());
            }

            auto leafList =
                RecordListReader::OpenField(branch.Value(), children.Value(), ItemSchema, 1, 1);
            if (!leafList) {
                return Result<void>::Failure(leafList.Error());
            }

            for (usize leafIndex = 0; leafIndex < leafList.Value().ItemCount(); ++leafIndex) {
                auto leaf = leafList.Value().OpenItem(leafIndex);
                if (!leaf) {
                    return Result<void>::Failure(leaf.Error());
                }

                auto valueField = leaf.Value().Find(ValueField);
                if (!valueField) {
                    return Result<void>::Failure(valueField.Error());
                }

                auto value = leaf.Value().OpenValue(valueField.Value());
                if (!value) {
                    return Result<void>::Failure(value.Error());
                }

                auto read = value.Value().ReadU64();
                if (!read) {
                    return Result<void>::Failure(read.Error());
                }

                ++leaves;
            }
        }

        return Result<void>::Success();
    };

    auto full = RecordReader::Open(allocator, rootBytes.Value().View(), ParentSchema, 1, 1);
    REQUIRE(full);
    const uint64 openOperations = full.Value().OperationsUsed();
    uint64 leaves = 0;
    REQUIRE(traverse(full.Value(), leaves));
    CHECK(leaves == 16);
    const uint64 treeOperations = full.Value().OperationsUsed();
    CHECK(treeOperations >
          openOperations + 16 * 3); // 중첩 목록·항목·값이 모두 부모 예산에 청구됐다

    // 트리 전체 사용량보다 작은 한도로는 순회가 끝나지 않는다(각 단계가 새 예산을 받으면 통과했을
    // 것이다).
    RecordLimits tight;
    tight.archive.maxOperations = treeOperations - 1;
    auto limited =
        RecordReader::Open(allocator, rootBytes.Value().View(), ParentSchema, 1, 1, tight);
    REQUIRE(limited);
    uint64 limitedLeaves = 0;
    auto result = traverse(limited.Value(), limitedLeaves);
    CHECK(!result && result.Error() == ErrorCode::CapacityExceeded && limitedLeaves < 16);

    // 잘못된 사용: RecordList가 아닌 필드, 이 Record 밖의 범위.
    ArchiveWriter scalar(allocator);
    REQUIRE(scalar.WriteU32(1));
    RecordBuilder plain(allocator, ParentSchema, 1);
    REQUIRE(plain.AddField(ValueField, WireType::U32, scalar.View()));
    auto plainBytes = plain.Build();
    REQUIRE(plainBytes);
    auto plainReader = RecordReader::Open(allocator, plainBytes.Value().View(), ParentSchema, 1, 1);
    REQUIRE(plainReader);
    auto plainField = plainReader.Value().Find(ValueField);
    REQUIRE(plainField);
    CHECK(RecordListReader::OpenField(plainReader.Value(), plainField.Value(), ItemSchema, 1, 1)
              .Error() == ErrorCode::TypeMismatch);
    FieldView foreign = plainField.Value();
    foreign.payload = scalar.View();
    CHECK(plainReader.Value().OpenValue(foreign).Error() == ErrorCode::InvalidArgument);
    CHECK(plainReader.Value().OpenValue(plainField.Value()));
}

TEST(RecordList_OpenFieldValidationChargesAndLimitPropagation) {
    SystemAllocator allocator;
    auto item = BuildItem(allocator, 4);
    REQUIRE(item);
    RecordListBuilder builder(allocator);
    REQUIRE(builder.AddItem(item.Value().View()));
    Array<uint8> parentBytes(allocator);
    REQUIRE(FinishAndFind(allocator, builder, parentBytes));
    auto parent = RecordReader::Open(allocator, parentBytes.View(), ParentSchema, 1, 1);
    REQUIRE(parent);
    auto field = parent.Value().Find(ItemsField);
    REQUIRE(field);

    // 인자 검증: 0 schema, 최소 버전 0, max < min은 InvalidArgument다(Open·OpenField 모두).
    const Span<const uint8> payload = field.Value().payload;
    CHECK(RecordListReader::Open(allocator, payload, Guid{}, 1, 1).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(RecordListReader::Open(allocator, payload, ItemSchema, 0, 1).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(RecordListReader::Open(allocator, payload, ItemSchema, 2, 1).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(RecordListReader::OpenField(parent.Value(), field.Value(), Guid{}, 1, 1).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(RecordListReader::OpenField(parent.Value(), field.Value(), ItemSchema, 0, 1).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(RecordListReader::OpenField(parent.Value(), field.Value(), ItemSchema, 2, 1).Error() ==
          ErrorCode::InvalidArgument);

    // 이 Record 밖의 범위(같은 bytes의 다른 사본)는 InvalidArgument이고, 실패한 호출도 operation을
    // 하나 소비한다(OpenField·OpenValue 모두).
    Array<uint8> copy(allocator);
    REQUIRE(copy.TryAppend(parentBytes.View()));
    FieldView foreign = field.Value();
    foreign.payload =
        Span<const uint8>(copy.Data() + (payload.Data() - parentBytes.Data()), payload.Size());
    uint64 before = parent.Value().OperationsUsed();
    CHECK(RecordListReader::OpenField(parent.Value(), foreign, ItemSchema, 1, 1).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(parent.Value().OperationsUsed() == before + 1);
    before = parent.Value().OperationsUsed();
    CHECK(parent.Value().OpenValue(foreign).Error() == ErrorCode::InvalidArgument);
    CHECK(parent.Value().OperationsUsed() == before + 1);
    FieldView mistyped = field.Value();
    mistyped.wireType = static_cast<uint32>(WireType::Bytes);
    before = parent.Value().OperationsUsed();
    CHECK(RecordListReader::OpenField(parent.Value(), mistyped, ItemSchema, 1, 1).Error() ==
          ErrorCode::TypeMismatch);
    CHECK(parent.Value().OperationsUsed() == before + 1);

    // 부모 Record의 한도가 중첩 목록·항목에 전파된다: 항목 필드 2개는 부모 maxFields 1을 넘는다.
    RecordBuilder twoFields(allocator, ItemSchema, 1);
    REQUIRE(twoFields.AddField(ValueField, WireType::Bytes, {}));
    REQUIRE(twoFields.AddField(ChildrenField, WireType::Bytes, {}));
    auto wide = twoFields.Build();
    REQUIRE(wide);
    RecordListBuilder wideList(allocator);
    REQUIRE(wideList.AddItem(wide.Value().View()));
    Array<uint8> wideParentBytes(allocator);
    REQUIRE(FinishAndFind(allocator, wideList, wideParentBytes));
    RecordLimits narrow;
    narrow.maxFields = 1;
    auto narrowParent =
        RecordReader::Open(allocator, wideParentBytes.View(), ParentSchema, 1, 1, narrow);
    REQUIRE(narrowParent);
    auto wideField = narrowParent.Value().Find(ItemsField);
    REQUIRE(wideField);
    auto inherited =
        RecordListReader::OpenField(narrowParent.Value(), wideField.Value(), ItemSchema, 1, 1);
    REQUIRE(inherited);
    CHECK(inherited.Value().OpenItem(0).Error() == ErrorCode::CapacityExceeded);
    auto standalone =
        RecordListReader::Open(allocator, wideField.Value().payload, ItemSchema, 1, 1);
    REQUIRE(standalone);
    CHECK(standalone.Value().OpenItem(0)); // 기본 한도로는 열린다

    // 이동된 parent·reader는 빈 입력이다.
    RecordReader moved(Move(parent.Value()));
    CHECK(RecordListReader::OpenField(parent.Value(), field.Value(), ItemSchema, 1, 1).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(parent.Value().OpenValue(field.Value()).Error() == ErrorCode::InvalidArgument);
    CHECK(RecordListReader::OpenField(moved, field.Value(), ItemSchema, 1, 1));
}

TEST(RecordList_SaturatedBudgetFailsSiblingReaders) {
    // 한 항목에서 work 한도를 넘으면 공유 사용량이 한도로 포화되어, 남은 예산으로는 충분했을 형제
    // 항목의 작은 호출도 실패한다.
    SystemAllocator allocator;
    auto big = BuildSizedItem(allocator, 80 + 1000);
    auto small = BuildItem(allocator, 6);
    REQUIRE(big && small);
    RecordListBuilder builder(allocator);
    REQUIRE(builder.AddItem(big.Value().View()) && builder.AddItem(small.Value().View()));
    Array<uint8> parentBytes(allocator);
    auto payload = FinishAndFind(allocator, builder, parentBytes);
    REQUIRE(payload);

    // 값 읽기 직전까지의 사용량을 잰다(같은 순서는 같은 사용량이다).
    uint64 prepared = 0;
    {
        auto list = RecordListReader::Open(allocator, payload.Value(), ItemSchema, 1, 1);
        REQUIRE(list);
        auto first = list.Value().OpenItem(0);
        auto second = list.Value().OpenItem(1);
        REQUIRE(first && second);
        auto field = first.Value().Find(ValueField);
        REQUIRE(field && first.Value().OpenValue(field.Value()));
        prepared = list.Value().WorkUsed();
    }

    RecordLimits limits;
    limits.archive.maxWork = prepared + 500; // 1000 byte 값은 못 읽지만 작은 Find에는 충분하다
    auto list = RecordListReader::Open(allocator, payload.Value(), ItemSchema, 1, 1, limits);
    REQUIRE(list);
    auto first = list.Value().OpenItem(0);
    auto second = list.Value().OpenItem(1);
    REQUIRE(first && second);
    auto field = first.Value().Find(ValueField);
    REQUIRE(field);
    auto value = first.Value().OpenValue(field.Value());
    REQUIRE(value);
    CHECK(list.Value().WorkUsed() == prepared);
    CHECK(value.Value().ReadBytes().Error() == ErrorCode::CapacityExceeded);
    CHECK(list.Value().WorkUsed() == limits.archive.maxWork); // 포화
    CHECK(second.Value().Find(ValueField).Error() == ErrorCode::CapacityExceeded);
    CHECK(list.Value().OpenItem(1).Error() == ErrorCode::CapacityExceeded);
}

namespace {

struct FindWork {
    RecordReader* reader;
    uint32 calls;
    bool ok;
};

void RunFinds(void* raw) noexcept {
    auto& work = *static_cast<FindWork*>(raw);
    work.ok = true;
    for (uint32 call = 0; call < work.calls; ++call) {
        work.ok = work.ok && static_cast<bool>(work.reader->Find(ValueField));
    }
}

} // namespace

TEST(RecordList_SharedBudgetIsThreadSafe) {
    // 같은 예산을 공유하는 서로 다른 항목 reader를 두 스레드에서 쓰면 사용량이 정확히
    // 합산된다(원자적 누적).
    SystemAllocator allocator;
    auto item = BuildItem(allocator, 8);
    REQUIRE(item);
    RecordListBuilder builder(allocator);
    REQUIRE(builder.AddItem(item.Value().View()) && builder.AddItem(item.Value().View()));
    RecordBuilder parent(allocator, ParentSchema, 1);
    REQUIRE(builder.Finish(parent, ItemsField));
    auto parentBytes = parent.Build();
    REQUIRE(parentBytes);
    auto parentReader =
        RecordReader::Open(allocator, parentBytes.Value().View(), ParentSchema, 1, 1);
    REQUIRE(parentReader);
    RecordLimits generous;
    generous.archive.maxOperations = ~0ull;
    generous.archive.maxWork = ~0ull;
    auto list =
        RecordListReader::Open(allocator, parentReader.Value().Find(ItemsField).Value().payload,
                               ItemSchema, 1, 1, generous);
    REQUIRE(list);
    auto first = list.Value().OpenItem(0);
    auto second = list.Value().OpenItem(1);
    REQUIRE(first && second);
    const uint64 before = list.Value().OperationsUsed();
    constexpr uint32 Calls = 200000;
    FindWork works[2] = {{&first.Value(), Calls, false}, {&second.Value(), Calls, false}};
    platform::Thread threads[2];
    for (int index = 0; index < 2; ++index) {
        REQUIRE(threads[index].Start(RunFinds, &works[index]) == platform::ThreadStatus::Success);
    }

    for (platform::Thread& thread : threads) {
        if (thread.WaitForCompletion(30000) != platform::WaitResult::Signaled ||
            thread.Join() != platform::ThreadStatus::Success) {
            platform::Fatal("RecordList budget thread did not finish");
        }
    }

    CHECK(works[0].ok && works[1].ok);
    CHECK(list.Value().OperationsUsed() == before + 2ull * Calls);
}

TEST(RecordList_FailureAtomicity) {
    SystemAllocator backing;
    auto item = BuildItem(backing, 5);
    REQUIRE(item);
    // 부모 AddField 실패(중복 id): 부모와 builder는 그대로이고 다른 부모에 다시 쓸 수 있다.
    RecordListBuilder builder(backing);
    REQUIRE(builder.AddItem(item.Value().View()));
    RecordBuilder parent(backing, ParentSchema, 1);
    ArchiveWriter unrelated(backing);
    REQUIRE(unrelated.WriteU32(1));
    REQUIRE(parent.AddField(ItemsField, WireType::U32, unrelated.View()));
    CHECK(builder.Finish(parent, ItemsField).Error() == ErrorCode::AlreadyExists);
    CHECK(parent.FieldCount() == 1 && builder.ItemCount() == 1);
    RecordBuilder other(backing, ParentSchema, 1);
    CHECK(builder.Finish(other, ItemsField) && other.FieldCount() == 1);

    // 모든 할당 지점 실패: AddItem·Finish·Open이 실패하면 상태가 바뀌지 않고, 실패 없는 실행과 같은
    // 결과를 낸다.
    // 실패는 모두 OutOfMemory이고, 반복마다 할당을 남기지 않는다.
    test::NFailAllocator failing(backing);
    const uint64 baseline = backing.Stats().liveAllocations;
    uint32 pipelineFailures = 0;
    for (uint64 attempt = 1; attempt < 64; ++attempt) {
        failing.FailOnAttempt(attempt);
        // 각 반복은 lambda 안에서 끝나 모든 지역 객체가 파괴된 뒤 누수를 본다.
        [&]() noexcept {
            auto failed = [&](ErrorCode error) noexcept {
                ++pipelineFailures;
                CHECK(error == ErrorCode::OutOfMemory);
            };

            RecordListBuilder list(failing);
            auto added = list.AddItem(item.Value().View());
            CHECK(list.ItemCount() == (added ? 1u : 0u));
            if (!added) {
                failed(added.Error());
                return;
            }

            RecordBuilder target(failing, ParentSchema, 1);
            auto finished = list.Finish(target, ItemsField);
            CHECK(target.FieldCount() == (finished ? 1u : 0u));
            if (!finished) {
                failed(finished.Error());
                return;
            }

            auto bytes = target.Build();
            if (!bytes) {
                failed(bytes.Error());
                return;
            }

            auto reader = RecordReader::Open(failing, bytes.Value().View(), ParentSchema, 1, 1);
            if (!reader) {
                failed(reader.Error());
                return;
            }

            auto field = reader.Value().Find(ItemsField);
            REQUIRE(field);
            auto items =
                RecordListReader::OpenField(reader.Value(), field.Value(), ItemSchema, 1, 1);
            if (!items) {
                failed(items.Error());
                return;
            }

            CHECK(items.Value().ItemCount() == 1);
            auto opened = items.Value().OpenItem(0);
            if (!opened) {
                failed(opened.Error());
                return;
            }

            auto value = ReadValue(opened.Value());
            if (!value) {
                failed(value.Error());
                return;
            }

            CHECK(value.Value() == 5);
        }();

        CHECK(backing.Stats().liveAllocations == baseline);
    }

    failing.DisableFailure();
    CHECK(pipelineFailures > 0);

    // 여러 항목: 어느 할당이 실패해도 그 호출만 실패하고 상태는 그대로라, 실패한 호출을 다시 하면
    // 실패 없는 실행과 byte 단위로 같은 Record가 나온다(부분 항목이 남으면 bytes가 달라진다).
    Array<uint8> itemBytes[4] = {Array<uint8>(backing), Array<uint8>(backing),
                                 Array<uint8>(backing), Array<uint8>(backing)};
    for (usize index = 0; index < 4; ++index) {
        auto built = BuildSizedItem(backing, 80 + 37 * (index + 1));
        REQUIRE(built);
        itemBytes[index] = Move(built.Value());
    }

    // 단계별 실패 횟수: [AddItem, Finish, Build].
    auto buildAll = [&](Allocator& allocator,
                        uint32(&failures)[3]) noexcept -> Result<Array<uint8>> {
        RecordListBuilder list(allocator);
        for (usize index = 0; index < 4; ++index) {
            auto added = list.AddItem(itemBytes[index].View());
            if (!added) {
                ++failures[0];
                if (added.Error() != ErrorCode::OutOfMemory || list.ItemCount() != index) {
                    return Result<Array<uint8>>::Failure(ErrorCode::DependencyFailed); // 상태 변경
                }

                if (auto retried = list.AddItem(itemBytes[index].View()); !retried) {
                    return Result<Array<uint8>>::Failure(retried.Error());
                }
            }
        }

        RecordBuilder target(allocator, ParentSchema, 1);
        if (auto finished = list.Finish(target, ItemsField); !finished) {
            ++failures[1];
            if (finished.Error() != ErrorCode::OutOfMemory || target.FieldCount() != 0 ||
                list.ItemCount() != 4) {
                return Result<Array<uint8>>::Failure(ErrorCode::DependencyFailed); // 상태 변경
            }

            if (auto retried = list.Finish(target, ItemsField); !retried) {
                return Result<Array<uint8>>::Failure(retried.Error());
            }
        }

        auto built = target.Build();
        if (!built) {
            ++failures[2];
            if (built.Error() != ErrorCode::OutOfMemory || target.FieldCount() != 1) {
                return Result<Array<uint8>>::Failure(ErrorCode::DependencyFailed); // 상태 변경
            }

            return target.Build();
        }

        return built;
    };

    uint32 cleanFailures[3] = {};
    auto clean = buildAll(backing, cleanFailures);
    REQUIRE(clean && cleanFailures[0] + cleanFailures[1] + cleanFailures[2] == 0);
    const uint64 settled = backing.Stats().liveAllocations; // 항목 bytes와 clean 결과까지
    uint32 injected[3] = {};
    for (uint64 attempt = 1; attempt < 64; ++attempt) {
        failing.FailOnAttempt(attempt);
        uint32 failures[3] = {};
        {
            auto result = buildAll(failing, failures);
            REQUIRE(result);
            CHECK(failures[0] + failures[1] + failures[2] <= 1);
            CHECK(SameBytes(result.Value().View(), clean.Value().View()));
        }

        for (int stage = 0; stage < 3; ++stage) {
            injected[stage] += failures[stage];
        }

        CHECK(backing.Stats().liveAllocations == settled);
    }

    failing.DisableFailure();
    // 각 단계의 할당 지점을 실제로 실패시켰다(AddItem은 항목 4개 동안 buffer가 여러 번 자란다).
    CHECK(injected[0] >= 2 && injected[1] >= 1 && injected[2] >= 1);
}
