#include <AssetNugget/NuggetRecord.h>
#include <Core/Memory/SystemAllocator.h>
#include <Serialization/Record.h>
#include <Serialization/RecordList.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// 에셋 너겟 Record 시험이다(Docs/Design/AssetSystem.ko.md 9.1절 결정 5 — Rockpile 5.2·5.3절):
// 왕복, bucket·namespace 규칙 경계, 정규 형식 규칙마다 거부, TLV 형식 변조(schema·버전·필드 순서·
// wire·추가 필드·크기), 1 MiB 한도, 논리 digest(저장 표현 제외·논리 칸 포함), 소비 입력 순서, 할당
// 실패.
using namespace rocks;
namespace nugget_record_tests_file {

namespace {

Sha256Digest Digest(uint8 seed) noexcept {
    Sha256Digest digest;
    for (usize index = 0; index < Sha256Digest::Bytes; ++index) {
        digest.bytes[index] = static_cast<uint8>(seed + index);
    }

    return digest;
}

NuggetValueRow IdentityRow(uint32 id, uint64 size, uint8 seed) noexcept {
    NuggetValueRow row;
    row.id = id;
    row.rawSize = size;
    row.storedSize = size;
    row.rawHash = Digest(seed);
    row.contentHash = row.rawHash;
    return row;
}

// 값 셋(Identity 작은 값·Rcbk·1 MiB 이상 Identity)과 입력 둘·meta를 가진 Record다.
NuggetRecord Sample(Allocator& allocator) noexcept {
    NuggetRecord record(allocator);
    CHECK(record.nameSpace.TryAssign(NuggetNamespace) &&
          record.bucket.TryAssign(NuggetBucketTexture));
    record.key = Digest(1);
    NuggetValueRow rcbk;
    rcbk.id = 2;
    rcbk.encoding = NuggetEncoding::Rcbk;
    rcbk.rawSize = 100;
    rcbk.storedSize = 80;
    rcbk.indexLength = 56;
    rcbk.rawHash = Digest(10);
    rcbk.contentHash = Digest(11);
    rcbk.indexHash = Digest(12);
    NuggetValueRow large = IdentityRow(7, NuggetChunkBytes, 20);
    large.chunkTableHash = Digest(21);
    CHECK(record.values.TryPushBack(IdentityRow(0, 5, 30)) && record.values.TryPushBack(rcbk) &&
          record.values.TryPushBack(large));
    NuggetInput first(allocator);
    NuggetInput second(allocator);
    CHECK(first.bucket.TryAssign("scc") && second.bucket.TryAssign("texture"));
    first.key = Digest(40);
    first.digest = Digest(41);
    second.key = Digest(42);
    second.digest = Digest(43);
    CHECK(record.inputs.TryPushBack(Move(first)) && record.inputs.TryPushBack(Move(second)));
    const uint8 meta[] = {1, 2, 3};
    CHECK(record.meta.TryAppend(meta));
    return record;
}

bool SameRecord(const NuggetRecord& left, const NuggetRecord& right) noexcept {
    if (left.nameSpace.View() != right.nameSpace.View() ||
        left.bucket.View() != right.bucket.View() || !(left.key == right.key) ||
        left.values.Size() != right.values.Size() || left.inputs.Size() != right.inputs.Size() ||
        left.meta.Size() != right.meta.Size()) {
        return false;
    }

    for (usize index = 0; index < left.values.Size(); ++index) {
        const NuggetValueRow& a = left.values[index];
        const NuggetValueRow& b = right.values[index];
        if (a.id != b.id || a.encoding != b.encoding || a.rawSize != b.rawSize ||
            a.storedSize != b.storedSize || a.indexLength != b.indexLength ||
            !(a.rawHash == b.rawHash) || !(a.contentHash == b.contentHash) ||
            !(a.indexHash == b.indexHash) || !(a.chunkTableHash == b.chunkTableHash)) {
            return false;
        }
    }

    for (usize index = 0; index < left.inputs.Size(); ++index) {
        if (CompareNuggetInputs(left.inputs[index], right.inputs[index]) != 0) {
            return false;
        }
    }

    for (usize index = 0; index < left.meta.Size(); ++index) {
        if (left.meta[index] != right.meta[index]) {
            return false;
        }
    }

    return true;
}

} // namespace

// 왕복(TLV byte → 같은 Record), 결정적 byte, 빈 값·입력·meta도 된다.
TEST(NuggetRecord_RoundTrip) {
    SystemAllocator allocator;
    const NuggetRecord record = Sample(allocator);
    auto bytes = WriteNuggetRecord(allocator, record);
    REQUIRE(bytes);
    auto again = WriteNuggetRecord(allocator, record);
    REQUIRE(again && again.Value().Size() == bytes.Value().Size());
    for (usize index = 0; index < bytes.Value().Size(); ++index) {
        CHECK(again.Value()[index] == bytes.Value()[index]);
    }

    auto back = ReadNuggetRecord(allocator, bytes.Value().View());
    REQUIRE(back);
    CHECK(SameRecord(back.Value(), record));

    NuggetRecord empty(allocator);
    REQUIRE(empty.nameSpace.TryAssign(NuggetNamespace) && empty.bucket.TryAssign("shader"));
    auto emptyBytes = WriteNuggetRecord(allocator, empty);
    REQUIRE(emptyBytes);
    auto emptyBack = ReadNuggetRecord(allocator, emptyBytes.Value().View());
    REQUIRE(emptyBack);
    CHECK(SameRecord(emptyBack.Value(), empty));
}

// bucket(`[a-z][a-z0-9_]{0,63}`)·namespace(`[a-z0-9][a-z0-9._-]{0,63}`) 경계.
TEST(NuggetRecord_NameRules) {
    char longest[NuggetNameMaxBytes + 1] = {};
    for (usize index = 0; index < NuggetNameMaxBytes + 1; ++index) {
        longest[index] = 'a';
    }

    CHECK(IsValidNuggetBucket("texture") && IsValidNuggetBucket("a") &&
          IsValidNuggetBucket("a_1") && IsValidNuggetBucket(StringView(longest, 64)));
    CHECK(!IsValidNuggetBucket("") && !IsValidNuggetBucket("1a") && !IsValidNuggetBucket("_a") &&
          !IsValidNuggetBucket("Tex") && !IsValidNuggetBucket("a-b") &&
          !IsValidNuggetBucket("a.b") && !IsValidNuggetBucket(StringView(longest, 65)) &&
          !IsValidNuggetBucket("a{") && !IsValidNuggetBucket("{"));
    CHECK(IsValidNuggetNamespace(NuggetNamespace) && IsValidNuggetNamespace("1x") &&
          IsValidNuggetNamespace("a-b_c.d9") && IsValidNuggetNamespace(StringView(longest, 64)));
    CHECK(!IsValidNuggetNamespace("") && !IsValidNuggetNamespace(".a") &&
          !IsValidNuggetNamespace("-a") && !IsValidNuggetNamespace("A") &&
          !IsValidNuggetNamespace("a/b") && !IsValidNuggetNamespace(StringView(longest, 65)) &&
          !IsValidNuggetNamespace("a~") && !IsValidNuggetNamespace("~") &&
          !IsValidNuggetNamespace("a:"));
}

// 정규 형식 규칙마다 Validate·Write가 InvalidArgument다.
TEST(NuggetRecord_CanonicalRules) {
    SystemAllocator allocator;
    CHECK(ValidateNuggetRecord(Sample(allocator)));
    using Break = void (*)(NuggetRecord&) noexcept;
    const Break breaks[] = {
        [](NuggetRecord& r) noexcept { CHECK(r.nameSpace.TryAssign("Bad")); },
        [](NuggetRecord& r) noexcept { CHECK(r.bucket.TryAssign("1bad")); },
        [](NuggetRecord& r) noexcept { CHECK(r.meta.TryResize(NuggetMetaMaxBytes + 1)); },
        // 값 순서·중복.
        [](NuggetRecord& r) noexcept { r.values[1].id = 0; },
        [](NuggetRecord& r) noexcept { r.values[2].id = 1; },
        // 모르는 encoding.
        [](NuggetRecord& r) noexcept { r.values[0].encoding = static_cast<NuggetEncoding>(3); },
        // 크기 상한.
        [](NuggetRecord& r) noexcept { r.values[1].rawSize = NuggetValueMaxBytes + 1; },
        [](NuggetRecord& r) noexcept { r.values[1].storedSize = NuggetValueMaxBytes + 1; },
        // Rcbk가 아닌 값의 색인.
        [](NuggetRecord& r) noexcept { r.values[0].indexLength = 1; },
        [](NuggetRecord& r) noexcept { r.values[0].indexHash.bytes[0] = 1; },
        // Identity의 크기·해시 불일치.
        [](NuggetRecord& r) noexcept { r.values[0].storedSize = 4; },
        [](NuggetRecord& r) noexcept { r.values[0].contentHash.bytes[0] ^= 1; },
        // 색인이 저장 크기보다 큼, Rcbk 색인 규칙(48 + 8n·IndexHash 있음).
        [](NuggetRecord& r) noexcept { r.values[1].indexLength = 81; },
        [](NuggetRecord& r) noexcept { r.values[1].indexLength = 40; },
        [](NuggetRecord& r) noexcept { r.values[1].indexLength = 52; },
        [](NuggetRecord& r) noexcept { r.values[1].indexHash = Sha256Digest{}; },
        // 조각 해시 표: 1 MiB 미만에 표 해시, 1 MiB 이상에 없음.
        [](NuggetRecord& r) noexcept { r.values[0].chunkTableHash.bytes[0] = 1; },
        [](NuggetRecord& r) noexcept { r.values[2].chunkTableHash = Sha256Digest{}; },
        // 입력 bucket·순서·중복.
        [](NuggetRecord& r) noexcept { CHECK(r.inputs[0].bucket.TryAssign("Bad")); },
        [](NuggetRecord& r) noexcept { CHECK(r.inputs[1].bucket.TryAssign("a")); },
        [](NuggetRecord& r) noexcept {
            CHECK(r.inputs[1].bucket.TryAssign("scc"));
            r.inputs[1].key = r.inputs[0].key;
            r.inputs[1].digest = r.inputs[0].digest;
        }};
    for (Break mutate : breaks) {
        NuggetRecord record = Sample(allocator);
        mutate(record);
        CHECK(ValidateNuggetRecord(record).Error() == ErrorCode::InvalidArgument);
        CHECK(WriteNuggetRecord(allocator, record).Error() == ErrorCode::InvalidArgument);
    }

    // 1 MiB 넘는 Record는 CapacityExceeded다(행 6,554개 × 160 byte).
    NuggetRecord big(allocator);
    REQUIRE(big.nameSpace.TryAssign(NuggetNamespace) && big.bucket.TryAssign("texture"));
    constexpr uint32 Rows = NuggetRecordMaxBytes / NuggetValueRowBytes + 1;
    REQUIRE(big.values.TryReserve(Rows));
    for (uint32 index = 0; index < Rows; ++index) {
        REQUIRE(big.values.TryPushBack(IdentityRow(index, 1, 0)));
    }

    CHECK(WriteNuggetRecord(allocator, big).Error() == ErrorCode::CapacityExceeded);
}

namespace {

// 필드 목록을 그대로 쓴 TLV다(형식 변조 시험).
struct RawField {
    Guid id;
    WireType wire;
    Span<const uint8> payload;
};

Array<uint8> RawRecord(Allocator& allocator, Guid schema, uint32 version,
                       Span<const RawField> fields) noexcept {
    RecordBuilder builder(allocator, schema, version);
    for (const RawField& field : fields) {
        CHECK(builder.AddField(field.id, field.wire, field.payload));
    }

    auto built = builder.Build();
    CHECK(built);
    return built ? Move(built.Value()) : Array<uint8>(allocator);
}

Span<const uint8> Text(StringView text) noexcept {
    return Span<const uint8>(reinterpret_cast<const uint8*>(text.Data()), text.ByteCount());
}

} // namespace

// TLV 형식 변조: 다른 schema·Record 아님 InvalidArgument, 더 새 버전 Unsupported, 필드 빠짐·순서·
// wire·추가 필드·key 크기·행 길이·reserved·입력 항목 형식, 1 MiB 넘는 입력 CapacityExceeded.
TEST(NuggetRecord_FormatTampering) {
    SystemAllocator allocator;
    const uint8 key[Sha256Digest::Bytes] = {};
    Array<uint8> emptyList(allocator);
    {
        RecordBuilder holder(allocator, Guid{9, 9}, 1);
        RecordListBuilder list(allocator);
        REQUIRE(list.Finish(holder, Guid{0, 1}));
        auto built = holder.Build();
        REQUIRE(built);
        auto reader = RecordReader::Open(allocator, built.Value().View(), Guid{9, 9}, 1, 1);
        REQUIRE(reader);
        auto field = reader.Value().Find(Guid{0, 1});
        REQUIRE(field);
        REQUIRE(emptyList.TryAppend(field.Value().payload));
    }

    const RawField good[] = {{Guid{0, 1}, WireType::String, Text("rocks.nuggets")},
                             {Guid{0, 2}, WireType::String, Text("texture")},
                             {Guid{0, 3}, WireType::Bytes, key},
                             {Guid{0, 4}, WireType::Bytes, {}},
                             {Guid{0, 5}, WireType::RecordList, emptyList.View()},
                             {Guid{0, 6}, WireType::Bytes, {}}};
    {
        const Array<uint8> bytes =
            RawRecord(allocator, NuggetRecordSchema, NuggetRecordSchemaVersion, good);
        CHECK(ReadNuggetRecord(allocator, bytes.View()));
    }

    CHECK(ReadNuggetRecord(allocator, RawRecord(allocator, Guid{1, 1}, 1, good).View()).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(ReadNuggetRecord(allocator, RawRecord(allocator, NuggetRecordSchema, 2, good).View())
              .Error() == ErrorCode::Unsupported);
    const uint8 garbage[] = {1, 2, 3};
    CHECK(ReadNuggetRecord(allocator, garbage).Error() == ErrorCode::InvalidArgument);

    // 필드 빠짐(마지막)·순서 바꿈·wire 바꿈·추가 필드.
    CHECK(ReadNuggetRecord(
              allocator,
              RawRecord(allocator, NuggetRecordSchema, 1, Span<const RawField>(good, 5)).View())
              .Error() == ErrorCode::InvalidArgument);
    {
        RawField swapped[6] = {good[1], good[0], good[2], good[3], good[4], good[5]};
        CHECK(
            ReadNuggetRecord(allocator, RawRecord(allocator, NuggetRecordSchema, 1, swapped).View())
                .Error() == ErrorCode::InvalidArgument);
    }

    for (usize index = 0; index < 6; ++index) {
        RawField changed[6] = {good[0], good[1], good[2], good[3], good[4], good[5]};
        changed[index].wire =
            changed[index].wire == WireType::Bytes ? WireType::String : WireType::Bytes;
        CHECK(
            ReadNuggetRecord(allocator, RawRecord(allocator, NuggetRecordSchema, 1, changed).View())
                .Error() == ErrorCode::InvalidArgument);
    }

    {
        RawField extra[7] = {good[0],
                             good[1],
                             good[2],
                             good[3],
                             good[4],
                             good[5],
                             {Guid{0, 7}, WireType::Bytes, {}}};
        CHECK(ReadNuggetRecord(allocator, RawRecord(allocator, NuggetRecordSchema, 1, extra).View())
                  .Error() == ErrorCode::InvalidArgument);
    }

    // key 크기·행 길이(160 배수 아님)·reserved·정규 형식(읽은 뒤 Validate).
    {
        RawField shortKey[6] = {good[0], good[1], {Guid{0, 3}, WireType::Bytes, Text("k")},
                                good[3], good[4], good[5]};
        CHECK(ReadNuggetRecord(allocator,
                               RawRecord(allocator, NuggetRecordSchema, 1, shortKey).View())
                  .Error() == ErrorCode::InvalidArgument);
    }

    uint8 row[NuggetValueRowBytes] = {};
    {
        RawField oddRows[6] = {good[0], good[1],
                               good[2], {Guid{0, 4}, WireType::Bytes, Span<const uint8>(row, 159)},
                               good[4], good[5]};
        CHECK(
            ReadNuggetRecord(allocator, RawRecord(allocator, NuggetRecordSchema, 1, oddRows).View())
                .Error() == ErrorCode::InvalidArgument);
    }

    for (usize at = 5; at < 8; ++at) {
        uint8 reserved[NuggetValueRowBytes] = {};
        reserved[at] = 1;
        RawField rows[6] = {good[0], good[1], good[2], {Guid{0, 4}, WireType::Bytes, reserved},
                            good[4], good[5]};
        CHECK(ReadNuggetRecord(allocator, RawRecord(allocator, NuggetRecordSchema, 1, rows).View())
                  .Error() == ErrorCode::InvalidArgument);
    }

    {
        RawField badName[6] = {{Guid{0, 1}, WireType::String, Text("BAD")},
                               good[1],
                               good[2],
                               good[3],
                               good[4],
                               good[5]};
        CHECK(
            ReadNuggetRecord(allocator, RawRecord(allocator, NuggetRecordSchema, 1, badName).View())
                .Error() == ErrorCode::InvalidArgument);
    }

    // 입력 항목: 다른 schema·필드 빠짐·key 크기.
    auto inputList = [&](Guid schema, Span<const RawField> fields, Array<uint8>& out) noexcept {
        const Array<uint8> item = RawRecord(allocator, schema, 1, fields);
        RecordBuilder holder(allocator, Guid{9, 9}, 1);
        RecordListBuilder list(allocator);
        CHECK(list.AddItem(item.View()) && list.Finish(holder, Guid{0, 1}));
        auto built = holder.Build();
        CHECK(built);
        auto reader = RecordReader::Open(allocator, built.Value().View(), Guid{9, 9}, 1, 1);
        CHECK(reader);
        auto field = reader.Value().Find(Guid{0, 1});
        CHECK(field && out.TryAppend(field.Value().payload));
    };
    const uint8 digest[Sha256Digest::Bytes] = {};
    const RawField inputGood[] = {{Guid{0, 1}, WireType::String, Text("scc")},
                                  {Guid{0, 2}, WireType::Bytes, digest},
                                  {Guid{0, 3}, WireType::Bytes, digest}};
    const RawField inputShort[] = {{Guid{0, 1}, WireType::String, Text("scc")},
                                   {Guid{0, 2}, WireType::Bytes, Text("x")},
                                   {Guid{0, 3}, WireType::Bytes, digest}};
    const RawField inputShortDigest[] = {{Guid{0, 1}, WireType::String, Text("scc")},
                                         {Guid{0, 2}, WireType::Bytes, digest},
                                         {Guid{0, 3}, WireType::Bytes, Text("y")}};
    const RawField inputExtra[] = {
        inputGood[0], inputGood[1], inputGood[2], {Guid{0, 4}, WireType::Bytes, {}}};
    struct Case {
        Guid schema;
        Span<const RawField> fields;
        bool ok;
    };
    const Case cases[] = {{NuggetInputSchema, inputGood, true},
                          {Guid{5, 5}, inputGood, false},
                          {NuggetInputSchema, Span<const RawField>(inputGood, 2), false},
                          {NuggetInputSchema, inputShort, false},
                          {NuggetInputSchema, inputShortDigest, false},
                          {NuggetInputSchema, inputExtra, false}};
    for (const Case& item : cases) {
        Array<uint8> list(allocator);
        inputList(item.schema, item.fields, list);
        RawField fields[6] = {
            good[0], good[1], good[2], good[3], {Guid{0, 5}, WireType::RecordList, list.View()},
            good[5]};
        auto read =
            ReadNuggetRecord(allocator, RawRecord(allocator, NuggetRecordSchema, 1, fields).View());
        CHECK(item.ok ? static_cast<bool>(read) : read.Error() == ErrorCode::InvalidArgument);
    }

    // 1 MiB 넘는 입력.
    Array<uint8> huge(allocator);
    REQUIRE(huge.TryResize(NuggetRecordMaxBytes + 1));
    CHECK(ReadNuggetRecord(allocator, huge.View()).Error() == ErrorCode::CapacityExceeded);
}

// 논리 digest: 저장 표현(encoding·storedSize·ContentHash·IndexHash·ChunkTableHash·indexLength)은
// 넣지 않고, 논리 칸(namespace·bucket·key·ValueId·rawSize·RawHash·입력·meta)은 하나씩 바꾸면
// 바뀐다.
TEST(NuggetRecord_LogicalDigest) {
    SystemAllocator allocator;
    const Sha256Digest base = NuggetRecordDigest(Sample(allocator));
    using Edit = void (*)(NuggetRecord&) noexcept;
    const Edit representation[] = {
        [](NuggetRecord& r) noexcept { r.values[1].encoding = NuggetEncoding::Codec; },
        [](NuggetRecord& r) noexcept { r.values[1].storedSize = 90; },
        [](NuggetRecord& r) noexcept { r.values[1].contentHash = Digest(99); },
        [](NuggetRecord& r) noexcept { r.values[1].indexHash = Digest(98); },
        [](NuggetRecord& r) noexcept { r.values[1].indexLength = 60; },
        [](NuggetRecord& r) noexcept { r.values[2].chunkTableHash = Digest(97); }};
    for (Edit edit : representation) {
        NuggetRecord record = Sample(allocator);
        edit(record);
        CHECK(NuggetRecordDigest(record) == base);
    }

    const Edit logical[] = {
        [](NuggetRecord& r) noexcept { CHECK(r.nameSpace.TryAssign("other")); },
        [](NuggetRecord& r) noexcept { CHECK(r.bucket.TryAssign("shader")); },
        [](NuggetRecord& r) noexcept { r.key = Digest(77); },
        [](NuggetRecord& r) noexcept { r.values[2].id = 8; },
        [](NuggetRecord& r) noexcept { r.values[0].rawSize = 6; },
        [](NuggetRecord& r) noexcept { r.values[0].rawHash = Digest(76); },
        [](NuggetRecord& r) noexcept { (void)r.values.TryPopBack(); },
        [](NuggetRecord& r) noexcept { r.inputs[0].digest = Digest(75); },
        [](NuggetRecord& r) noexcept { r.inputs[0].key = Digest(74); },
        [](NuggetRecord& r) noexcept { CHECK(r.inputs[0].bucket.TryAssign("sc")); },
        [](NuggetRecord& r) noexcept { (void)r.inputs.TryPopBack(); },
        [](NuggetRecord& r) noexcept { r.meta[0] = 9; },
        [](NuggetRecord& r) noexcept { r.meta.Clear(); }};
    for (Edit edit : logical) {
        NuggetRecord record = Sample(allocator);
        edit(record);
        CHECK(!(NuggetRecordDigest(record) == base));
    }

    // 소비 입력 순서: 길이 접두(짧은 bucket 먼저) → bucket → key → digest.
    NuggetInput a(allocator);
    NuggetInput b(allocator);
    REQUIRE(a.bucket.TryAssign("zz") && b.bucket.TryAssign("aaa"));
    CHECK(CompareNuggetInputs(a, b) < 0 && CompareNuggetInputs(b, a) > 0);
    // 같은 길이 bucket은 내용으로.
    REQUIRE(b.bucket.TryAssign("za"));
    CHECK(CompareNuggetInputs(a, b) > 0);
    REQUIRE(b.bucket.TryAssign("zz"));
    a.key = Digest(1);
    b.key = Digest(2);
    CHECK(CompareNuggetInputs(a, b) < 0);
    b.key = a.key;
    a.digest = Digest(3);
    b.digest = Digest(2);
    CHECK(CompareNuggetInputs(a, b) > 0);
    b.digest = a.digest;
    CHECK(CompareNuggetInputs(a, b) == 0);
}

// 할당 실패(Write·Read): 모두 OutOfMemory이고 실패 없는 결과는 기준과 같다.
TEST(NuggetRecord_AllocationFailures) {
    SystemAllocator system;
    const NuggetRecord record = Sample(system);
    auto expected = WriteNuggetRecord(system, record);
    REQUIRE(expected);
    test::NFailAllocator failing(system);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        failing.FailOnAttempt(attempt);
        auto bytes = WriteNuggetRecord(failing, record);
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            REQUIRE(bytes && bytes.Value().Size() == expected.Value().Size());
            for (usize index = 0; index < bytes.Value().Size(); ++index) {
                CHECK(bytes.Value()[index] == expected.Value()[index]);
            }

            break;
        }

        CHECK(bytes.Error() == ErrorCode::OutOfMemory);
        ++failures;
    }

    for (uint64 attempt = 1;; ++attempt) {
        failing.FailOnAttempt(attempt);
        auto read = ReadNuggetRecord(failing, expected.Value().View());
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            REQUIRE(read);
            CHECK(SameRecord(read.Value(), record));
            break;
        }

        CHECK(read.Error() == ErrorCode::OutOfMemory);
        ++failures;
    }

    CHECK(failures > 1);
}

// TLV byte를 하나씩 뒤집으면 읽기가 거부하거나 다른 Record가 된다(같은 Record를 돌려주지 않는다).
TEST(NuggetRecord_EveryByteFlip) {
    SystemAllocator allocator;
    const NuggetRecord record = Sample(allocator);
    auto bytes = WriteNuggetRecord(allocator, record);
    REQUIRE(bytes);
    uint32 rejected = 0;
    for (usize at = 0; at < bytes.Value().Size(); ++at) {
        Array<uint8> copy(allocator);
        REQUIRE(copy.TryAppend(bytes.Value().View()));
        copy[at] ^= 0x01;
        auto read = ReadNuggetRecord(allocator, copy.View());
        if (!read) {
            CHECK(read.Error() == ErrorCode::InvalidArgument ||
                  read.Error() == ErrorCode::Unsupported);
            ++rejected;
            continue;
        }

        CHECK(!SameRecord(read.Value(), record));
    }

    CHECK(rejected > 0);
}

} // namespace nugget_record_tests_file
