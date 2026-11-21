#include <AssetDatabase/ImportRecord.h>
#include <AssetRuntime/PackageTags.h>
#include <Base/Endian.h>
#include <Core/Memory/SystemAllocator.h>
#include <Serialization/Record.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>

// ImportRecord 값 스키마 시험이다(Docs/Design/AssetSystem.ko.md 5.4절, 7.1절 15 — A2c2a): 소유·참조
// 기록 왕복과 손으로 만든 정규 배치(golden)와의 byte 일치, 모든 경계값 양성(문자열 상한·개수
// 상한·빈 root·빈 outputKey·점이 든 경로 성분), 쓰기 규칙 위반 전부, 읽기의 손상 전부(필드
// 없음·wire·크기· 모르는 필드·빈 목록·개수·항목 schema·버전·순서·중복·self 없음·options envelope),
// ImportOwner 대조의 모든 조합, 모든 할당 실패, 용량 초과.
using namespace rocks;
namespace import_record_tests_file {

namespace {

constexpr AssetId Self{Guid{0x10, 0}};
constexpr AssetId Other{Guid{0x10, 0x77}};
constexpr Guid Importer{0x494d50, 1};
constexpr TypeId TextureType{0x545950, 1};
constexpr TypeId MaterialType{0x545950, 2};
constexpr Guid OptionsSchema{0x4f5054, 1};
constexpr Guid OptionsField{0, 1};
constexpr uint32 OptionsValue = 0x1234;
constexpr usize LongText = 2048;

AssetId OutputAsset(uint64 index) noexcept {
    return AssetId{Guid{0x10, index}};
}

Sha256Digest Digest(uint8 seed) noexcept {
    Sha256Digest digest;
    for (usize index = 0; index < Sha256Digest::Bytes; ++index) {
        digest.bytes[index] = static_cast<uint8>(seed + index);
    }

    return digest;
}

// 'a'로 채운 count byte 문자열이다(count ≤ LongText).
StringView Repeated(usize count) noexcept {
    static char text[LongText];
    memset(text, 'a', sizeof(text));
    return StringView(text, count);
}

// 옵션 Record다(임포터 스키마 — U32 하나).
Array<uint8> MakeOptions(Allocator& allocator) noexcept {
    RecordBuilder builder(allocator, OptionsSchema, 1);
    uint8 value[4];
    StoreLittleEndian<uint32>(value, OptionsValue);
    if (!builder.AddField(OptionsField, WireType::U32, value)) {
        return Array<uint8>(allocator);
    }

    auto built = builder.Build();
    return built ? Move(built.Value()) : Array<uint8>(allocator);
}

// 쓰기 입력 장면이다: 원본 둘(source·px — 루트 있음/없음), 출력 둘(key "mat"·"" — 쓴 순서는 key
// 역순), options 있음. 시험은 이 값을 바꿔 규칙 하나씩 어긴다.
struct WriteScene {
    explicit WriteScene(Allocator& allocator) noexcept : options(MakeOptions(allocator)) {
        sources[0] = {"source", "Raw", "Textures/Rock.png", Digest(1), 4096};
        sources[1] = {"px", "", "Cube/px.png", Digest(2), 17};
        outputs[0] = {"mat", OutputAsset(5), MaterialType};
        outputs[1] = {"", Self, TextureType};
        input.owner = Self;
        input.importerId = Importer;
        input.importerName = "TextureImporter";
        input.importerVersion = 3;
        input.sources = sources;
        input.options = options.View();
        input.outputs = outputs;
    }

    Array<uint8> options;
    ImportSourceInput sources[2];
    ImportOutputInput outputs[2];
    ImportRecordInput input;
};

// 손으로 만드는 Record다(필드 id·wire·payload를 그대로 — 손상 장면과 golden 배치).
struct RawField {
    explicit RawField(Allocator& allocator) noexcept : payload(allocator) {
    }

    Guid id{};
    uint32 wire = 0;
    Array<uint8> payload;
};

class RawRecord {
public:
    RawRecord(Allocator& allocator, Guid schema, uint32 version) noexcept
        : allocator_(&allocator), fields_(allocator), schema(schema), version(version) {
    }

    RawRecord(RawRecord&&) noexcept = default;
    RawRecord& operator=(RawRecord&&) noexcept = default;

    // 같은 id가 있으면 자리를 지켜 바꾸고 없으면 끝에 더한다.
    void Set(Guid id, uint32 wire, Span<const uint8> payload) noexcept {
        RawField* field = nullptr;
        for (RawField& existing : fields_) {
            if (existing.id == id) {
                field = &existing;
            }
        }

        if (field == nullptr) {
            CHECK(fields_.TryPushBack(RawField(*allocator_)));
            field = &fields_[fields_.Size() - 1];
        }

        field->id = id;
        field->wire = wire;
        field->payload.Clear();
        CHECK(field->payload.TryAppend(payload));
    }

    void Set(Guid id, WireType wire, Span<const uint8> payload) noexcept {
        Set(id, static_cast<uint32>(wire), payload);
    }

    void SetText(Guid id, StringView text) noexcept {
        Set(id, WireType::String,
            Span<const uint8>(reinterpret_cast<const uint8*>(text.Data()), text.ByteCount()));
    }

    void SetGuid(Guid id, Guid value) noexcept {
        uint8 bytes[16];
        StoreLittleEndian<uint64>(bytes, value.high);
        StoreLittleEndian<uint64>(bytes + 8, value.low);
        Set(id, WireType::Guid, bytes);
    }

    void SetU32(Guid id, uint32 value) noexcept {
        uint8 bytes[4];
        StoreLittleEndian<uint32>(bytes, value);
        Set(id, WireType::U32, bytes);
    }

    void SetU64(Guid id, uint64 value) noexcept {
        uint8 bytes[8];
        StoreLittleEndian<uint64>(bytes, value);
        Set(id, WireType::U64, bytes);
    }

    void Remove(Guid id) noexcept {
        for (usize index = 0; index < fields_.Size(); ++index) {
            if (fields_[index].id == id) {
                CHECK(fields_.TryRemoveAt(index));
                return;
            }
        }
    }

    Array<uint8> Build() const noexcept {
        RecordLimits limits;
        limits.archive.maxTotalBytes = 64 * 1024 * 1024;
        limits.archive.maxValueBytes = 32 * 1024 * 1024;
        RecordBuilder builder(*allocator_, schema, version, limits);
        for (const RawField& field : fields_) {
            CHECK(builder.AddField(field.id, field.wire, field.payload.View()));
        }

        auto built = builder.Build();
        CHECK(built);
        return built ? Move(built.Value()) : Array<uint8>(*allocator_);
    }

private:
    Allocator* allocator_;
    Array<RawField> fields_;

public:
    Guid schema;
    uint32 version;
};

RawRecord SourceItem(Allocator& allocator, StringView role, StringView root, StringView path,
                     uint8 seed, uint64 bytes) noexcept {
    RawRecord item(allocator, ImportSourceSchema, ImportRecordVersion);
    item.SetText(ImportSourceRoleField, role);
    item.SetText(ImportSourceRootField, root);
    item.SetText(ImportSourcePathField, path);
    item.Set(ImportSourceSha256Field, WireType::Bytes, Digest(seed).bytes);
    item.SetU64(ImportSourceBytesField, bytes);
    return item;
}

RawRecord OutputItem(Allocator& allocator, StringView key, AssetId asset, TypeId type) noexcept {
    RawRecord item(allocator, ImportOutputSchema, ImportRecordVersion);
    item.SetText(ImportOutputKeyField, key);
    item.SetGuid(ImportOutputAssetField, asset.value);
    item.SetGuid(ImportOutputTypeField, type);
    return item;
}

// RecordList payload(u32 count + count × (u64 length + Record))다.
Array<uint8> ListPayload(Allocator& allocator, const Array<RawRecord>& items) noexcept {
    Array<uint8> payload(allocator);
    uint8 count[4];
    StoreLittleEndian<uint32>(count, static_cast<uint32>(items.Size()));
    CHECK(payload.TryAppend(Span<const uint8>(count)));
    for (const RawRecord& item : items) {
        const Array<uint8> built = item.Build();
        uint8 length[8];
        StoreLittleEndian<uint64>(length, built.Size());
        CHECK(payload.TryAppend(Span<const uint8>(length)));
        CHECK(payload.TryAppend(built.View()));
    }

    return payload;
}

// 손으로 만드는 기록 장면이다. 기본값은 WriteScene과 같은 소유 기록의 정규 배치다(출력은 key 순).
struct RawScene {
    explicit RawScene(Allocator& allocator) noexcept
        : allocator(allocator), top(allocator, ImportRecordSchema, ImportRecordVersion),
          sources(allocator), outputs(allocator), options(MakeOptions(allocator)) {
        top.SetGuid(ImportRecordImporterIdField, Importer);
        top.SetText(ImportRecordImporterNameField, "TextureImporter");
        top.SetU32(ImportRecordImporterVersionField, 3);
        CHECK(sources.TryPushBack(
            SourceItem(allocator, "source", "Raw", "Textures/Rock.png", 1, 4096)));
        CHECK(sources.TryPushBack(SourceItem(allocator, "px", "", "Cube/px.png", 2, 17)));
        CHECK(outputs.TryPushBack(OutputItem(allocator, "", Self, TextureType)));
        CHECK(outputs.TryPushBack(OutputItem(allocator, "mat", OutputAsset(5), MaterialType)));
    }

    // 참조 기록으로 바꾼다(owner = Other, 빈 임포터, 목록·옵션 없음).
    void MakeReference() noexcept {
        top.SetGuid(ImportRecordImporterIdField, Guid{});
        top.SetText(ImportRecordImporterNameField, "");
        top.SetU32(ImportRecordImporterVersionField, 0);
        writeSources = false;
        writeOptions = false;
        writeOutputs = false;
        owner = Other;
    }

    Array<uint8> Build() noexcept {
        if (writeSources) {
            const Array<uint8> list = ListPayload(allocator, sources);
            top.Set(ImportRecordSourcesField, WireType::RecordList, list.View());
        }

        if (writeOptions) {
            top.Set(ImportRecordOptionsField, WireType::Record, options.View());
        }

        if (writeOutputs) {
            const Array<uint8> list = ListPayload(allocator, outputs);
            top.Set(ImportRecordOutputsField, WireType::RecordList, list.View());
        }

        top.SetGuid(ImportRecordOwnerField, owner.value);
        return top.Build();
    }

    Allocator& allocator;
    RawRecord top;
    Array<RawRecord> sources;
    Array<RawRecord> outputs;
    Array<uint8> options;
    AssetId owner = Self;
    bool writeSources = true;
    bool writeOptions = true;
    bool writeOutputs = true;
};

bool SameText(const String& text, StringView expected) noexcept {
    return StringView::CompareBytes(text.View(), expected) == 0;
}

} // namespace

// 왕복과 배치(7.1절 15): 소유 기록을 쓰면 손으로 만든 정규 배치와 byte가 같고(출력은 key 순으로
// 정렬된다), 읽으면 모든 값이 돌아온다(원본은 쓴 순서). options 없는 소유 기록·참조 기록도 정규
// 배치와 같고 왕복한다. 소유 기록을 다른 self로 읽으면 참조 기록 규칙 위반, 참조 기록을 owner로
// 읽으면 소유 기록 규칙 위반이다.
TEST(ImportRecord_RoundTripAndLayout) {
    SystemAllocator allocator;
    {
        WriteScene scene(allocator);
        REQUIRE(!scene.options.IsEmpty());
        auto written = WriteImportRecord(allocator, Self, scene.input);
        REQUIRE(written);
        RawScene raw(allocator);
        const Array<uint8> golden = raw.Build();
        CHECK(written.Value().Size() == golden.Size() &&
              memcmp(written.Value().Data(), golden.Data(), golden.Size()) == 0);

        auto read = ReadImportRecord(allocator, Self, written.Value().View());
        REQUIRE(read);
        const ImportRecord& record = read.Value();
        CHECK(record.OwnedBy(Self));
        CHECK(record.owner == Self);
        CHECK(record.importerId == Importer);
        CHECK(SameText(record.importerName, "TextureImporter"));
        CHECK(record.importerVersion == 3);
        REQUIRE(record.sources.Size() == 2);
        CHECK(SameText(record.sources[0].role, "source"));
        CHECK(SameText(record.sources[0].root, "Raw"));
        CHECK(SameText(record.sources[0].relativePath, "Textures/Rock.png"));
        CHECK(record.sources[0].sha256 == Digest(1));
        CHECK(record.sources[0].bytes == 4096);
        CHECK(SameText(record.sources[1].role, "px"));
        CHECK(record.sources[1].root.IsEmpty());
        CHECK(SameText(record.sources[1].relativePath, "Cube/px.png"));
        CHECK(record.sources[1].sha256 == Digest(2));
        CHECK(record.sources[1].bytes == 17);
        CHECK(record.options.Size() == scene.options.Size() &&
              memcmp(record.options.Data(), scene.options.Data(), scene.options.Size()) == 0);
        REQUIRE(record.outputs.Size() == 2);
        CHECK(record.outputs[0].outputKey.IsEmpty());
        CHECK(record.outputs[0].asset == Self);
        CHECK(record.outputs[0].type == TextureType);
        CHECK(SameText(record.outputs[1].outputKey, "mat"));
        CHECK(record.outputs[1].asset == OutputAsset(5));
        CHECK(record.outputs[1].type == MaterialType);

        // 소유 기록을 다른 self로 읽으면 참조 기록 규칙(임포터 비어 있음) 위반이다.
        CHECK(ReadImportRecord(allocator, Other, written.Value().View()).Error() ==
              ErrorCode::InvalidArgument);
    }

    {
        WriteScene scene(allocator);
        scene.input.options = {};
        auto written = WriteImportRecord(allocator, Self, scene.input);
        REQUIRE(written);
        RawScene raw(allocator);
        raw.writeOptions = false;
        const Array<uint8> golden = raw.Build();
        CHECK(written.Value().Size() == golden.Size() &&
              memcmp(written.Value().Data(), golden.Data(), golden.Size()) == 0);
        auto read = ReadImportRecord(allocator, Self, written.Value().View());
        REQUIRE(read);
        CHECK(read.Value().options.IsEmpty());
        CHECK(read.Value().sources.Size() == 2 && read.Value().outputs.Size() == 2);
    }

    {
        ImportRecordInput input;
        input.owner = Other;
        auto written = WriteImportRecord(allocator, Self, input);
        REQUIRE(written);
        RawScene raw(allocator);
        raw.MakeReference();
        const Array<uint8> golden = raw.Build();
        CHECK(written.Value().Size() == golden.Size() &&
              memcmp(written.Value().Data(), golden.Data(), golden.Size()) == 0);
        auto read = ReadImportRecord(allocator, Self, written.Value().View());
        REQUIRE(read);
        CHECK(!read.Value().OwnedBy(Self));
        CHECK(read.Value().owner == Other);
        CHECK(read.Value().importerId == Guid{});
        CHECK(read.Value().importerName.IsEmpty());
        CHECK(read.Value().importerVersion == 0);
        CHECK(read.Value().sources.IsEmpty() && read.Value().outputs.IsEmpty() &&
              read.Value().options.IsEmpty());

        // 참조 기록을 owner 자신으로 읽으면 소유 기록 규칙(임포터·목록 있음) 위반이다.
        CHECK(ReadImportRecord(allocator, Other, written.Value().View()).Error() ==
              ErrorCode::InvalidArgument);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 경계값 양성(7.1절 15): 이름 256 byte, role 64, root 256·빈 값, relativePath 1024·점이 든 성분
// (".hidden"·"..."·"a.")·한 성분, outputKey 64·빈 값, 원본 256개, 출력 1024개(self가 맨 끝, 쓴
// 순서는 key 역순)가 모두 쓰이고 같은 값으로 읽힌다.
TEST(ImportRecord_BoundaryValues) {
    SystemAllocator allocator;
    {
        WriteScene scene(allocator);
        scene.input.importerName = Repeated(MaxImporterNameBytes);
        scene.sources[0] = {Repeated(MaxImportRoleBytes), Repeated(MaxImportRootBytes),
                            Repeated(MaxImportRelativePathBytes), Digest(3), 0};
        scene.sources[1] = {"x", "", ".hidden/.../a./b", Digest(4), ~uint64{0}};
        scene.outputs[0] = {Repeated(MaxImportOutputKeyBytes), OutputAsset(9), MaterialType};
        auto written = WriteImportRecord(allocator, Self, scene.input);
        REQUIRE(written);
        auto read = ReadImportRecord(allocator, Self, written.Value().View());
        REQUIRE(read);
        CHECK(read.Value().importerName.ByteCount() == MaxImporterNameBytes);
        CHECK(read.Value().sources[0].role.ByteCount() == MaxImportRoleBytes);
        CHECK(read.Value().sources[0].root.ByteCount() == MaxImportRootBytes);
        CHECK(read.Value().sources[0].relativePath.ByteCount() == MaxImportRelativePathBytes);
        CHECK(read.Value().sources[0].bytes == 0);
        CHECK(SameText(read.Value().sources[1].relativePath, ".hidden/.../a./b"));
        CHECK(read.Value().sources[1].bytes == ~uint64{0});
        CHECK(read.Value().outputs[1].outputKey.ByteCount() == MaxImportOutputKeyBytes);
    }

    {
        // 원본 256개(role "r0".."r255")·출력 1024개(key "k0000".."k1023", 역순으로 준다).
        static char roles[MaxImportSources][8];
        static char keys[MaxImportOutputs][8];
        static ImportSourceInput sources[MaxImportSources];
        static ImportOutputInput outputs[MaxImportOutputs];
        for (usize index = 0; index < MaxImportSources; ++index) {
            const int count =
                snprintf(roles[index], sizeof(roles[index]), "r%u", static_cast<unsigned>(index));
            sources[index] = {StringView(roles[index], static_cast<usize>(count)), "", "a",
                              Digest(static_cast<uint8>(index)), index};
        }

        for (usize index = 0; index < MaxImportOutputs; ++index) {
            const usize at = MaxImportOutputs - 1 - index;
            snprintf(keys[at], sizeof(keys[at]), "k%04u", static_cast<unsigned>(at));
            outputs[index] = {StringView(keys[at], 5),
                              at == MaxImportOutputs - 1 ? Self : OutputAsset(1000 + at),
                              TextureType};
        }

        WriteScene scene(allocator);
        scene.input.sources = sources;
        scene.input.outputs = outputs;
        auto written = WriteImportRecord(allocator, Self, scene.input);
        REQUIRE(written);
        auto read = ReadImportRecord(allocator, Self, written.Value().View());
        REQUIRE(read);
        REQUIRE(read.Value().sources.Size() == MaxImportSources);
        REQUIRE(read.Value().outputs.Size() == MaxImportOutputs);
        CHECK(SameText(read.Value().sources[MaxImportSources - 1].role, "r255"));
        CHECK(SameText(read.Value().outputs[0].outputKey, "k0000"));
        CHECK(read.Value().outputs[0].asset == OutputAsset(1000));
        CHECK(read.Value().outputs[MaxImportOutputs - 1].asset == Self);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

namespace {

// 쓰기 규칙 위반 하나다.
enum class WriteBreak {
    NullSelf,
    NullOwner,
    NullImporter,
    EmptyName,
    LongName,
    BadUtf8Name,
    NulName,
    NoSources,
    TooManySources,
    NoOutputs,
    TooManyOutputs,
    OptionsNotRecord,
    EmptyRole,
    LongRole,
    NulRole,
    LongRoot,
    SlashRoot,
    BackslashRoot,
    BadUtf8Root,
    EmptyPath,
    LongPath,
    LeadingSlash,
    TrailingSlash,
    DoubleSlash,
    DotComponent,
    DotDotComponent,
    InnerDotDot,
    BackslashPath,
    BadUtf8Path,
    DuplicateRole,
    LongKey,
    BadUtf8Key,
    NullOutputAsset,
    NullOutputType,
    DuplicateKey,
    DuplicateAsset,
    SelfMissing,
    ReferenceImporter,
    ReferenceName,
    ReferenceVersion,
    ReferenceSources,
    ReferenceOptions,
    ReferenceOutputs
};

constexpr WriteBreak AllWriteBreaks[] = {
    WriteBreak::NullSelf,         WriteBreak::NullOwner,         WriteBreak::NullImporter,
    WriteBreak::EmptyName,        WriteBreak::LongName,          WriteBreak::BadUtf8Name,
    WriteBreak::NulName,          WriteBreak::NoSources,         WriteBreak::TooManySources,
    WriteBreak::NoOutputs,        WriteBreak::TooManyOutputs,    WriteBreak::OptionsNotRecord,
    WriteBreak::EmptyRole,        WriteBreak::LongRole,          WriteBreak::NulRole,
    WriteBreak::LongRoot,         WriteBreak::SlashRoot,         WriteBreak::BackslashRoot,
    WriteBreak::BadUtf8Root,      WriteBreak::EmptyPath,         WriteBreak::LongPath,
    WriteBreak::LeadingSlash,     WriteBreak::TrailingSlash,     WriteBreak::DoubleSlash,
    WriteBreak::DotComponent,     WriteBreak::DotDotComponent,   WriteBreak::InnerDotDot,
    WriteBreak::BackslashPath,    WriteBreak::BadUtf8Path,       WriteBreak::DuplicateRole,
    WriteBreak::LongKey,          WriteBreak::BadUtf8Key,        WriteBreak::NullOutputAsset,
    WriteBreak::NullOutputType,   WriteBreak::DuplicateKey,      WriteBreak::DuplicateAsset,
    WriteBreak::SelfMissing,      WriteBreak::ReferenceImporter, WriteBreak::ReferenceName,
    WriteBreak::ReferenceVersion, WriteBreak::ReferenceSources,  WriteBreak::ReferenceOptions,
    WriteBreak::ReferenceOutputs};

const uint8 NotRecord[] = {1, 2, 3};
ImportSourceInput ManySources[MaxImportSources + 1];
ImportOutputInput ManyOutputs[MaxImportOutputs + 1];

// scene을 규칙 하나를 어기게 바꾸고 쓸 self를 돌려준다.
AssetId Break(WriteScene& scene, WriteBreak kind) noexcept {
    ImportRecordInput& input = scene.input;
    ImportSourceInput& source = scene.sources[0];
    ImportOutputInput& output = scene.outputs[0];
    switch (kind) {
    case WriteBreak::NullSelf:
        return AssetId{};
    case WriteBreak::NullOwner:
        input.owner = AssetId{};
        break;
    case WriteBreak::NullImporter:
        input.importerId = Guid{};
        break;
    case WriteBreak::EmptyName:
        input.importerName = "";
        break;
    case WriteBreak::LongName:
        input.importerName = Repeated(MaxImporterNameBytes + 1);
        break;
    case WriteBreak::BadUtf8Name:
        input.importerName = "\xff";
        break;
    case WriteBreak::NulName:
        input.importerName = StringView("a\0b", 3);
        break;
    case WriteBreak::NoSources:
        input.sources = {};
        break;
    case WriteBreak::TooManySources:
        input.sources = ManySources;
        break;
    case WriteBreak::NoOutputs:
        input.outputs = {};
        break;
    case WriteBreak::TooManyOutputs:
        input.outputs = ManyOutputs;
        break;
    case WriteBreak::OptionsNotRecord:
        input.options = NotRecord;
        break;
    case WriteBreak::EmptyRole:
        source.role = "";
        break;
    case WriteBreak::LongRole:
        source.role = Repeated(MaxImportRoleBytes + 1);
        break;
    case WriteBreak::NulRole:
        source.role = StringView("\0", 1);
        break;
    case WriteBreak::LongRoot:
        source.root = Repeated(MaxImportRootBytes + 1);
        break;
    case WriteBreak::SlashRoot:
        source.root = "a/b";
        break;
    case WriteBreak::BackslashRoot:
        source.root = "a\\b";
        break;
    case WriteBreak::BadUtf8Root:
        source.root = "\xc0";
        break;
    case WriteBreak::EmptyPath:
        source.relativePath = "";
        break;
    case WriteBreak::LongPath:
        source.relativePath = Repeated(MaxImportRelativePathBytes + 1);
        break;
    case WriteBreak::LeadingSlash:
        source.relativePath = "/a";
        break;
    case WriteBreak::TrailingSlash:
        source.relativePath = "a/";
        break;
    case WriteBreak::DoubleSlash:
        source.relativePath = "a//b";
        break;
    case WriteBreak::DotComponent:
        source.relativePath = "./a";
        break;
    case WriteBreak::DotDotComponent:
        source.relativePath = "..";
        break;
    case WriteBreak::InnerDotDot:
        source.relativePath = "a/../b";
        break;
    case WriteBreak::BackslashPath:
        source.relativePath = "a\\b";
        break;
    case WriteBreak::BadUtf8Path:
        source.relativePath = "a/\xff";
        break;
    case WriteBreak::DuplicateRole:
        scene.sources[1].role = source.role;
        break;
    case WriteBreak::LongKey:
        output.outputKey = Repeated(MaxImportOutputKeyBytes + 1);
        break;
    case WriteBreak::BadUtf8Key:
        output.outputKey = "\xfe";
        break;
    case WriteBreak::NullOutputAsset:
        output.asset = AssetId{};
        break;
    case WriteBreak::NullOutputType:
        output.type = TypeId{};
        break;
    case WriteBreak::DuplicateKey:
        output.outputKey = "";
        break;
    case WriteBreak::DuplicateAsset:
        output.asset = Self;
        break;
    case WriteBreak::SelfMissing:
        scene.outputs[1].asset = OutputAsset(6);
        break;
    case WriteBreak::ReferenceImporter:
    case WriteBreak::ReferenceName:
    case WriteBreak::ReferenceVersion:
    case WriteBreak::ReferenceSources:
    case WriteBreak::ReferenceOptions:
    case WriteBreak::ReferenceOutputs: {
        // 참조 기록에서 한 칸만 남긴다.
        const ImportRecordInput full = input;
        input = ImportRecordInput{};
        input.owner = Other;
        if (kind == WriteBreak::ReferenceImporter) {
            input.importerId = full.importerId;
        } else if (kind == WriteBreak::ReferenceName) {
            input.importerName = full.importerName;
        } else if (kind == WriteBreak::ReferenceVersion) {
            input.importerVersion = full.importerVersion;
        } else if (kind == WriteBreak::ReferenceSources) {
            input.sources = full.sources;
        } else if (kind == WriteBreak::ReferenceOptions) {
            input.options = full.options;
        } else {
            input.outputs = full.outputs;
        }

        break;
    }
    }

    return Self;
}

} // namespace

// 쓰기 규칙(7.1절 15): 규칙 하나만 어긴 입력마다 InvalidArgument다(self·owner null, 소유 기록의
// 임포터·이름·개수·options envelope·원본 문자열·경로 형태·role 중복·출력 key·asset·type·중복·self
// 없음, 참조 기록의 각 칸). 바꾸지 않은 장면은 쓰인다.
TEST(ImportRecord_WriteRejects) {
    SystemAllocator allocator;
    for (usize index = 0; index <= MaxImportSources; ++index) {
        ManySources[index] = {"r", "", "a", Digest(0), 0};
    }

    for (usize index = 0; index <= MaxImportOutputs; ++index) {
        ManyOutputs[index] = {"", OutputAsset(index), TextureType};
    }

    {
        WriteScene scene(allocator);
        CHECK(WriteImportRecord(allocator, Self, scene.input));
    }

    for (WriteBreak kind : AllWriteBreaks) {
        WriteScene scene(allocator);
        const AssetId self = Break(scene, kind);
        auto written = WriteImportRecord(allocator, self, scene.input);
        CHECK(!written);
        if (written.Error() != ErrorCode::InvalidArgument) {
            printf("  write break %u: error %u\n", static_cast<unsigned>(kind),
                   static_cast<unsigned>(written.Error()));
            CHECK(false);
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

namespace {

// 읽기 손상 하나다.
enum class ReadBreak {
    MissingImporter,
    MissingName,
    MissingVersion,
    MissingOwner,
    ImporterWire,
    ImporterSize,
    NameWire,
    VersionWire,
    VersionSize,
    OwnerWire,
    NullOwner,
    UnknownField,
    NullImporter,
    EmptyName,
    LongName,
    NulName,
    MissingSources,
    MissingOutputs,
    SourcesWire,
    OutputsWire,
    OptionsWire,
    OptionsEmpty,
    OptionsHeader,
    SourcesEmpty,
    SourcesTooMany,
    SourcesTruncated,
    SourceSchema,
    SourceMissingRole,
    SourceMissingRoot,
    SourceMissingPath,
    SourceMissingSha,
    SourceMissingBytes,
    SourceExtraField,
    SourceRoleWire,
    SourceRootWire,
    SourcePathWire,
    SourceShaWire,
    SourceShaSize,
    SourceBytesWire,
    SourceBytesSize,
    SourceEmptyRole,
    SourceRootSlash,
    SourceDotDot,
    SourceDuplicateRole,
    OutputsEmpty,
    OutputsTooMany,
    OutputSchema,
    OutputMissingKey,
    OutputMissingAsset,
    OutputMissingType,
    OutputExtraField,
    OutputKeyWire,
    OutputAssetSize,
    OutputTypeWire,
    OutputLongKey,
    OutputNullAsset,
    OutputNullType,
    OutputOrder,
    OutputDuplicateKey,
    OutputDuplicateAsset,
    OutputSelfMissing,
    ReferenceImporter,
    ReferenceName,
    ReferenceVersion,
    ReferenceSources,
    ReferenceOptions,
    ReferenceOutputs,
    TopVersion,
    SourceVersion,
    OutputVersion
};

struct ReadCase {
    ReadBreak kind;
    ErrorCode error;
};

constexpr ErrorCode Invalid = ErrorCode::InvalidArgument;
constexpr ErrorCode Newer = ErrorCode::Unsupported;

constexpr ReadCase AllReadCases[] = {
    {ReadBreak::MissingImporter, Invalid},    {ReadBreak::MissingName, Invalid},
    {ReadBreak::MissingVersion, Invalid},     {ReadBreak::MissingOwner, Invalid},
    {ReadBreak::ImporterWire, Invalid},       {ReadBreak::ImporterSize, Invalid},
    {ReadBreak::NameWire, Invalid},           {ReadBreak::VersionWire, Invalid},
    {ReadBreak::VersionSize, Invalid},        {ReadBreak::OwnerWire, Invalid},
    {ReadBreak::NullOwner, Invalid},          {ReadBreak::UnknownField, Invalid},
    {ReadBreak::NullImporter, Invalid},       {ReadBreak::EmptyName, Invalid},
    {ReadBreak::LongName, Invalid},           {ReadBreak::NulName, Invalid},
    {ReadBreak::MissingSources, Invalid},     {ReadBreak::MissingOutputs, Invalid},
    {ReadBreak::SourcesWire, Invalid},        {ReadBreak::OutputsWire, Invalid},
    {ReadBreak::OptionsWire, Invalid},        {ReadBreak::OptionsEmpty, Invalid},
    {ReadBreak::OptionsHeader, Invalid},      {ReadBreak::SourcesEmpty, Invalid},
    {ReadBreak::SourcesTooMany, Invalid},     {ReadBreak::SourcesTruncated, Invalid},
    {ReadBreak::SourceSchema, Invalid},       {ReadBreak::SourceMissingRole, Invalid},
    {ReadBreak::SourceMissingRoot, Invalid},  {ReadBreak::SourceMissingPath, Invalid},
    {ReadBreak::SourceMissingSha, Invalid},   {ReadBreak::SourceMissingBytes, Invalid},
    {ReadBreak::SourceExtraField, Invalid},   {ReadBreak::SourceRoleWire, Invalid},
    {ReadBreak::SourceRootWire, Invalid},     {ReadBreak::SourcePathWire, Invalid},
    {ReadBreak::SourceShaWire, Invalid},      {ReadBreak::SourceShaSize, Invalid},
    {ReadBreak::SourceBytesWire, Invalid},    {ReadBreak::SourceBytesSize, Invalid},
    {ReadBreak::SourceEmptyRole, Invalid},    {ReadBreak::SourceRootSlash, Invalid},
    {ReadBreak::SourceDotDot, Invalid},       {ReadBreak::SourceDuplicateRole, Invalid},
    {ReadBreak::OutputsEmpty, Invalid},       {ReadBreak::OutputsTooMany, Invalid},
    {ReadBreak::OutputSchema, Invalid},       {ReadBreak::OutputMissingKey, Invalid},
    {ReadBreak::OutputMissingAsset, Invalid}, {ReadBreak::OutputMissingType, Invalid},
    {ReadBreak::OutputExtraField, Invalid},   {ReadBreak::OutputKeyWire, Invalid},
    {ReadBreak::OutputAssetSize, Invalid},    {ReadBreak::OutputTypeWire, Invalid},
    {ReadBreak::OutputLongKey, Invalid},      {ReadBreak::OutputNullAsset, Invalid},
    {ReadBreak::OutputNullType, Invalid},     {ReadBreak::OutputOrder, Invalid},
    {ReadBreak::OutputDuplicateKey, Invalid}, {ReadBreak::OutputDuplicateAsset, Invalid},
    {ReadBreak::OutputSelfMissing, Invalid},  {ReadBreak::ReferenceImporter, Invalid},
    {ReadBreak::ReferenceName, Invalid},      {ReadBreak::ReferenceVersion, Invalid},
    {ReadBreak::ReferenceSources, Invalid},   {ReadBreak::ReferenceOptions, Invalid},
    {ReadBreak::ReferenceOutputs, Invalid},   {ReadBreak::TopVersion, Newer},
    {ReadBreak::SourceVersion, Newer},        {ReadBreak::OutputVersion, Newer}};

const uint8 ShortGuid[15] = {};
const uint8 FourBytes[4] = {1, 0, 0, 0};
const uint8 EightBytes[8] = {1, 0, 0, 0, 0, 0, 0, 0};

// 손상 하나를 장면에 넣고 기록 byte를 만든다.
Array<uint8> BuildBroken(RawScene& scene, ReadBreak kind) noexcept {
    RawRecord& top = scene.top;
    RawRecord& source = scene.sources[0];
    RawRecord& output = scene.outputs[1];
    switch (kind) {
    case ReadBreak::MissingImporter:
        top.Remove(ImportRecordImporterIdField);
        break;
    case ReadBreak::MissingName:
        top.Remove(ImportRecordImporterNameField);
        break;
    case ReadBreak::MissingVersion:
        top.Remove(ImportRecordImporterVersionField);
        break;
    case ReadBreak::MissingOwner: {
        // Build가 owner를 다시 쓰므로 만든 뒤 owner 없는 Record를 다시 만든다.
        scene.Build();
        top.Remove(ImportRecordOwnerField);
        return top.Build();
    }
    case ReadBreak::ImporterWire:
        top.SetText(ImportRecordImporterIdField, "0123456789abcdef");
        break;
    case ReadBreak::ImporterSize:
        top.Set(ImportRecordImporterIdField, WireType::Guid, ShortGuid);
        break;
    case ReadBreak::NameWire:
        top.Set(ImportRecordImporterNameField, WireType::Bytes, FourBytes);
        break;
    case ReadBreak::VersionWire:
        top.Set(ImportRecordImporterVersionField, WireType::I64, EightBytes);
        break;
    case ReadBreak::VersionSize:
        top.Set(ImportRecordImporterVersionField, WireType::U32, EightBytes);
        break;
    case ReadBreak::OwnerWire: {
        scene.Build();
        top.SetU32(ImportRecordOwnerField, 1);
        return top.Build();
    }
    case ReadBreak::NullOwner:
        scene.owner = AssetId{};
        break;
    case ReadBreak::UnknownField:
        top.SetU32(Guid{0, 99}, 1);
        break;
    case ReadBreak::NullImporter:
        top.SetGuid(ImportRecordImporterIdField, Guid{});
        break;
    case ReadBreak::EmptyName:
        top.SetText(ImportRecordImporterNameField, "");
        break;
    case ReadBreak::LongName:
        top.SetText(ImportRecordImporterNameField, Repeated(MaxImporterNameBytes + 1));
        break;
    case ReadBreak::NulName:
        top.SetText(ImportRecordImporterNameField, StringView("\0", 1));
        break;
    case ReadBreak::MissingSources:
        scene.writeSources = false;
        break;
    case ReadBreak::MissingOutputs:
        scene.writeOutputs = false;
        break;
    case ReadBreak::SourcesWire: {
        scene.Build();
        top.Set(ImportRecordSourcesField, WireType::Bytes, FourBytes);
        return top.Build();
    }
    case ReadBreak::OutputsWire: {
        scene.Build();
        top.Set(ImportRecordOutputsField, WireType::Record, scene.options.View());
        return top.Build();
    }
    case ReadBreak::OptionsWire: {
        scene.Build();
        top.Set(ImportRecordOptionsField, WireType::Bytes, scene.options.View());
        return top.Build();
    }
    case ReadBreak::OptionsEmpty:
        scene.options.Clear();
        break;
    case ReadBreak::OptionsHeader:
        scene.options.Clear();
        CHECK(scene.options.TryAppend(Span<const uint8>(EightBytes)));
        break;
    case ReadBreak::SourcesEmpty:
        scene.sources.Clear();
        break;
    case ReadBreak::SourcesTooMany:
        for (usize index = scene.sources.Size(); index <= MaxImportSources; ++index) {
            char role[8];
            const int count = snprintf(role, sizeof(role), "r%u", static_cast<unsigned>(index));
            CHECK(scene.sources.TryPushBack(SourceItem(
                scene.allocator, StringView(role, static_cast<usize>(count)), "", "a", 0, 0)));
        }

        break;
    case ReadBreak::SourcesTruncated: {
        // count는 2인데 항목 길이가 남은 byte를 넘는다.
        scene.Build();
        const Array<uint8> list = ListPayload(scene.allocator, scene.sources);
        top.Set(ImportRecordSourcesField, WireType::RecordList, list.View().Left(list.Size() - 1));
        return top.Build();
    }
    case ReadBreak::SourceSchema:
        source.schema = ImportOutputSchema;
        break;
    case ReadBreak::SourceMissingRole:
        source.Remove(ImportSourceRoleField);
        break;
    case ReadBreak::SourceMissingRoot:
        source.Remove(ImportSourceRootField);
        break;
    case ReadBreak::SourceMissingPath:
        source.Remove(ImportSourcePathField);
        break;
    case ReadBreak::SourceMissingSha:
        source.Remove(ImportSourceSha256Field);
        break;
    case ReadBreak::SourceMissingBytes:
        source.Remove(ImportSourceBytesField);
        break;
    case ReadBreak::SourceExtraField:
        source.SetU32(Guid{0, 6}, 1);
        break;
    case ReadBreak::SourceRoleWire:
        source.Set(ImportSourceRoleField, WireType::Bytes, FourBytes);
        break;
    case ReadBreak::SourceRootWire:
        source.Set(ImportSourceRootField, WireType::Bytes, FourBytes);
        break;
    case ReadBreak::SourcePathWire:
        source.Set(ImportSourcePathField, WireType::Bytes, FourBytes);
        break;
    case ReadBreak::SourceShaWire:
        source.Set(ImportSourceSha256Field, WireType::String, Digest(1).bytes);
        break;
    case ReadBreak::SourceShaSize:
        source.Set(ImportSourceSha256Field, WireType::Bytes,
                   Span<const uint8>(Digest(1).bytes).Left(Sha256Digest::Bytes - 1));
        break;
    case ReadBreak::SourceBytesWire:
        source.Set(ImportSourceBytesField, WireType::I64, EightBytes);
        break;
    case ReadBreak::SourceBytesSize:
        source.Set(ImportSourceBytesField, WireType::U64, FourBytes);
        break;
    case ReadBreak::SourceEmptyRole:
        source.SetText(ImportSourceRoleField, "");
        break;
    case ReadBreak::SourceRootSlash:
        source.SetText(ImportSourceRootField, "a/b");
        break;
    case ReadBreak::SourceDotDot:
        source.SetText(ImportSourcePathField, "a/..");
        break;
    case ReadBreak::SourceDuplicateRole:
        scene.sources[1].SetText(ImportSourceRoleField, "source");
        break;
    case ReadBreak::OutputsEmpty:
        scene.outputs.Clear();
        break;
    case ReadBreak::OutputsTooMany:
        for (usize index = scene.outputs.Size(); index <= MaxImportOutputs; ++index) {
            char key[8];
            snprintf(key, sizeof(key), "z%04u", static_cast<unsigned>(index));
            CHECK(scene.outputs.TryPushBack(OutputItem(scene.allocator, StringView(key, 5),
                                                       OutputAsset(100 + index), TextureType)));
        }

        break;
    case ReadBreak::OutputSchema:
        output.schema = ImportSourceSchema;
        break;
    case ReadBreak::OutputMissingKey:
        output.Remove(ImportOutputKeyField);
        break;
    case ReadBreak::OutputMissingAsset:
        output.Remove(ImportOutputAssetField);
        break;
    case ReadBreak::OutputMissingType:
        output.Remove(ImportOutputTypeField);
        break;
    case ReadBreak::OutputExtraField:
        output.SetU32(Guid{0, 4}, 1);
        break;
    case ReadBreak::OutputKeyWire:
        output.Set(ImportOutputKeyField, WireType::Bytes, FourBytes);
        break;
    case ReadBreak::OutputAssetSize:
        output.Set(ImportOutputAssetField, WireType::Guid, ShortGuid);
        break;
    case ReadBreak::OutputTypeWire:
        output.Set(ImportOutputTypeField, WireType::Bytes,
                   Span<const uint8>(Digest(0).bytes).Left(16));
        break;
    case ReadBreak::OutputLongKey:
        output.SetText(ImportOutputKeyField, Repeated(MaxImportOutputKeyBytes + 1));
        break;
    case ReadBreak::OutputNullAsset:
        output.SetGuid(ImportOutputAssetField, Guid{});
        break;
    case ReadBreak::OutputNullType:
        output.SetGuid(ImportOutputTypeField, Guid{});
        break;
    case ReadBreak::OutputOrder: {
        RawRecord first = Move(scene.outputs[0]);
        scene.outputs[0] = Move(scene.outputs[1]);
        scene.outputs[1] = Move(first);
        break;
    }
    case ReadBreak::OutputDuplicateKey:
        output.SetText(ImportOutputKeyField, "");
        break;
    case ReadBreak::OutputDuplicateAsset:
        output.SetGuid(ImportOutputAssetField, Self.value);
        break;
    case ReadBreak::OutputSelfMissing:
        scene.outputs[0].SetGuid(ImportOutputAssetField, OutputAsset(6).value);
        break;
    case ReadBreak::ReferenceImporter:
    case ReadBreak::ReferenceName:
    case ReadBreak::ReferenceVersion:
    case ReadBreak::ReferenceSources:
    case ReadBreak::ReferenceOptions:
    case ReadBreak::ReferenceOutputs:
        scene.MakeReference();
        if (kind == ReadBreak::ReferenceImporter) {
            top.SetGuid(ImportRecordImporterIdField, Importer);
        } else if (kind == ReadBreak::ReferenceName) {
            top.SetText(ImportRecordImporterNameField, "x");
        } else if (kind == ReadBreak::ReferenceVersion) {
            top.SetU32(ImportRecordImporterVersionField, 1);
        } else if (kind == ReadBreak::ReferenceSources) {
            scene.writeSources = true;
        } else if (kind == ReadBreak::ReferenceOptions) {
            scene.writeOptions = true;
        } else {
            scene.writeOutputs = true;
        }

        break;
    case ReadBreak::TopVersion:
        top.version = ImportRecordVersion + 1;
        break;
    case ReadBreak::SourceVersion:
        source.version = ImportRecordVersion + 1;
        break;
    case ReadBreak::OutputVersion:
        output.version = ImportRecordVersion + 1;
        break;
    }

    return scene.Build();
}

} // namespace

// 읽기 손상(7.1절 15): 손상 하나마다 기대 오류다 — 형식·규칙 위반은 InvalidArgument, 기록·항목의
// 더 새 버전은 Unsupported. 그 밖의 입력: 빈 byte·null self는 InvalidArgument, header가 아닌 byte는
// InvalidArgument, 더 새 envelope 버전·다른 스키마(Tags)는 Unsupported. 바꾸지 않은 장면은 읽힌다.
TEST(ImportRecord_ReadRejects) {
    SystemAllocator allocator;
    {
        RawScene scene(allocator);
        const Array<uint8> bytes = scene.Build();
        CHECK(ReadImportRecord(allocator, Self, bytes.View()));
        CHECK(ReadImportRecord(allocator, AssetId{}, bytes.View()).Error() == Invalid);
        CHECK(ReadImportRecord(allocator, Self, {}).Error() == Invalid);
        CHECK(ReadImportRecord(allocator, Self, Span<const uint8>(EightBytes)).Error() == Invalid);
        Array<uint8> envelope(allocator);
        REQUIRE(envelope.TryAppend(bytes.View()));
        StoreLittleEndian<uint32>(envelope.Data() + 8, 2);
        CHECK(ReadImportRecord(allocator, Self, envelope.View()).Error() == Newer);
        const PackageTagInput tag[] = {{"k", "v"}};
        auto tags = WritePackageTags(allocator, tag);
        REQUIRE(tags);
        CHECK(ReadImportRecord(allocator, Self, tags.Value().View()).Error() == Newer);
    }

    for (const ReadCase& one : AllReadCases) {
        RawScene scene(allocator);
        const Array<uint8> bytes = BuildBroken(scene, one.kind);
        auto read = ReadImportRecord(allocator, Self, bytes.View());
        if (read || read.Error() != one.error) {
            printf("  read break %u: %s error %u\n", static_cast<unsigned>(one.kind),
                   read ? "accepted" : "rejected", read ? 0u : static_cast<unsigned>(read.Error()));
            CHECK(false);
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// ImportOwner 대조(7.1절 7·15): 기록 없음·소유 기록은 ImportOwner 항목이 없어야 하고, 참조 기록은
// owner를 가리키는 항목이 정확히 하나여야 한다. 다른 종류 항목은 세지 않는다. self null은
// InvalidArgument다.
TEST(ImportRecord_CheckImportOwner) {
    SystemAllocator allocator;
    ImportRecord owned(allocator);
    owned.owner = Self;
    ImportRecord reference(allocator);
    reference.owner = Other;
    const AssetId third{Guid{0x10, 0x88}};
    using K = PackageDependencyKind;
    const PackageDependencyEntry none[] = {{Other, K::Hard, {}, true},
                                           {third, K::Build, {}, false}};
    const PackageDependencyEntry toOwner[] = {{Other, K::Hard, {}, true},
                                              {Other, K::ImportOwner, {}, false}};
    const PackageDependencyEntry toThird[] = {{third, K::ImportOwner, {}, false}};
    const PackageDependencyEntry two[] = {{Other, K::ImportOwner, {}, false},
                                          {third, K::ImportOwner, {}, false}};
    struct Case {
        const ImportRecord* record;
        Span<const PackageDependencyEntry> dependencies;
        bool ok;
    };
    const Case cases[] = {
        {nullptr, {}, true},       {nullptr, none, true},        {nullptr, toOwner, false},
        {&owned, none, true},      {&owned, toOwner, false},     {&reference, toOwner, true},
        {&reference, none, false}, {&reference, toThird, false}, {&reference, two, false},
        {&reference, {}, false},
    };
    for (const Case& one : cases) {
        auto checked = CheckImportOwner(Self, one.record, one.dependencies);
        CHECK(static_cast<bool>(checked) == one.ok);
        if (!checked) {
            CHECK(checked.Error() == ErrorCode::InvalidArgument);
        }
    }

    CHECK(CheckImportOwner(AssetId{}, nullptr, {}).Error() == ErrorCode::InvalidArgument);
}

// 할당 실패와 용량(7.1절 15): 소유 기록 쓰기·읽기와 참조 기록 쓰기·읽기의 모든 할당 시도를 하나씩
// 실패시키면 OutOfMemory이고 누수가 없다. options가 Record 값 한도(4 MiB)를 넘으면 쓰기는
// CapacityExceeded다.
TEST(ImportRecord_AllocationAndCapacity) {
    SystemAllocator allocator;
    {
        test::NFailAllocator failing(allocator);
        WriteScene scene(allocator);
        ImportRecordInput reference;
        reference.owner = Other;
        const ImportRecordInput* inputs[] = {&scene.input, &reference};
        for (const ImportRecordInput* input : inputs) {
            auto written = WriteImportRecord(allocator, Self, *input);
            REQUIRE(written);
            usize writeFailures = 0;
            usize readFailures = 0;
            for (uint64 attempt = 1;; ++attempt) {
                failing.FailOnAttempt(attempt);
                auto result = WriteImportRecord(failing, Self, *input);
                if (failing.Attempts() < attempt) {
                    CHECK(result);
                    break;
                }

                CHECK(!result && result.Error() == ErrorCode::OutOfMemory);
                ++writeFailures;
            }

            for (uint64 attempt = 1;; ++attempt) {
                failing.FailOnAttempt(attempt);
                auto result = ReadImportRecord(failing, Self, written.Value().View());
                if (failing.Attempts() < attempt) {
                    CHECK(result);
                    break;
                }

                CHECK(!result && result.Error() == ErrorCode::OutOfMemory);
                ++readFailures;
            }

            failing.DisableFailure();
            CHECK(writeFailures > 0 && readFailures > 0);
        }
    }

    {
        // 4 MiB + 1 byte 값 하나를 가진 옵션 Record(큰 한도로 만든다).
        RecordLimits limits;
        limits.archive.maxTotalBytes = 16 * 1024 * 1024;
        limits.archive.maxValueBytes = 8 * 1024 * 1024;
        RecordBuilder big(allocator, OptionsSchema, 1, limits);
        Array<uint8> payload(allocator);
        REQUIRE(payload.TryResize(4 * 1024 * 1024 + 1));
        REQUIRE(big.AddField(OptionsField, WireType::Bytes, payload.View()));
        auto options = big.Build();
        REQUIRE(options);
        WriteScene scene(allocator);
        scene.input.options = options.Value().View();
        CHECK(WriteImportRecord(allocator, Self, scene.input).Error() ==
              ErrorCode::CapacityExceeded);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace import_record_tests_file
