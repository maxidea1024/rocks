#include "../../../Runtime/AssetRuntime/Tests/AssetFixtureValues.h"
#include "../../../Runtime/AssetRuntime/Tests/PackageEditing.h"
#include "AssetDatabaseTestSupport.h"
#include "EditorPackageSetTestSupport.h"

#include <AssetDatabase/AssetIndexCache.h>
#include <AssetDatabase/AssetService.h>
#include <AssetDatabase/ImportRecord.h>
#include <AssetDatabase/TemporaryFiles.h>
#include <AssetRuntime/AssetTypeHooks.h>
#include <AssetRuntime/PackageFormat.h>
#include <AssetRuntime/PackageTags.h>
#include <Core/Build/BuildInfo.h>
#include <Core/Encoding/ByteWriter.h>
#include <Core/Text/Unicode.h>
#include <TestSupport/NFailAllocator.h>
#include <string.h>

// 색인 캐시 시험이다(Docs/Design/AssetSystem.ko.md 7.1절 16 A2d1): 쓰기 → 읽기 왕복과 캐시를 쓴
// 검색의 같은 결과, 지문이 같으면 다시 읽지 않음(바꾼 캐시 값이 보인다)·다르면 다시 읽음, 진단이
// 있는 파일은 넣지 않음(같은 id 진단은 넣음), 키 축마다 무효, 신뢰하지 않는
// 입력(헤더·SHA-256·경계·의미 검사 각각 — 시험 쪽에서 형식을 따로 써서 만든다), 읽기 실패는 캐시
// 없음, 쓰기(잠금·IO 고장· NotDurable·할당 실패 — 임시 파일 남지 않음), 색인 읽기의 TypeRegistry
// 무시, 고아 임시 파일 정리 (모양·디렉터리·고장·잠금·할당 실패), 서비스의 캐시
// 사용·끄기·FlushCache.
using namespace rocks;
namespace asset_index_cache_tests_file {

using namespace rocks::asset_test;
using namespace rocks::asset_database_test;
using namespace rocks::editor_set_test;

namespace {

constexpr uint64 TaggedId = 2950;
constexpr uint64 BadTagsId = 2951;
// 캐시 헤더 배치(시험 쪽 독립 정의 — 구현과 같아야 한다).
constexpr usize HeaderBytes = 164;
constexpr usize CountAt = 120;
constexpr usize BodyBytesAt = 124;
constexpr usize BodyDigestAt = 132;
constexpr usize ReservedAt = 53;
constexpr usize NormalizesAt = 52;
constexpr usize DigestBytes = 32;
const PackageTagInput TaggedTags[] = {{"Kind", "Champion"}, {"Rank", "A"}};
const Guid TaggedImporter{0x1A, 0x2B};

// 캐시 장면이다: 루트는 디스크 임시 디렉터리(잠금), 파일은 그 루트에 놓인 메모리 파일 시스템
// (Content·Intermediate 둘 다), Content에 Base·Derived·Hero·Villain·Leaf·Tagged(태그·ImportRecord)·
// BadTags(Tags 진단 — 항목 있음)·Copy(Leaf와 같은 id — 같은 id 진단)·derived.RASSET(Derived와 같은
// 경로 키·같은 id — 같은 경로 진단)·Junk(읽기 실패), 잡은 잠금.
struct CacheScene {
    CacheScene(AssetEnv& env, Sources& sources, const char* tag) noexcept
        : root(tag), layout(env.allocator), files(env.allocator, Initialized(layout, root)),
          tagged(env.allocator), badTags(env.allocator) {
        ready = root.made && !layout.Root().IsEmpty() && files.Ready() && MakeExtra(env) &&
                files.AddDirectory("Content") && files.AddDirectory("Content/Sub") &&
                files.AddFile("Content/Base.rasset", sources.base.View()) &&
                files.AddFile("Content/Derived.rasset", sources.derived.View()) &&
                files.AddFile("Content/Hero.rasset", sources.hero.View()) &&
                files.AddFile("Content/Villain.rasset", sources.villain.View()) &&
                files.AddFile("Content/Sub/Leaf.rasset", sources.leaf.View()) &&
                files.AddFile("Content/Copy.rasset", sources.leaf.View()) &&
                files.AddFile("Content/Tagged.rasset", tagged.View()) &&
                files.AddFile("Content/BadTags.rasset", badTags.View()) &&
                files.AddFile("Content/Junk.rasset", JunkBytes()) &&
                files.AddFile("Content/derived.RASSET", sources.derived.View()) &&
                static_cast<bool>(lock.Acquire(layout, 0));
    }

    // layout을 root로 초기화하고 절대 루트를 준다(메모리 파일 시스템의 경로가 배치와 같게).
    static StringView Initialized(ProjectLayout& layout, const TempProject& root) noexcept {
        return layout.Initialize(root.Root()) ? layout.Root() : root.Root();
    }

    static Span<const uint8> JunkBytes() noexcept {
        static const uint8 junk[] = {1, 2, 3, 4};
        return Span<const uint8>(junk, sizeof(junk));
    }

    bool MakeExtra(AssetEnv& env) noexcept {
        ObjectRegistry& registry = *env.source;
        RootRef<Package> taggedPackage;
        RootRef<Package> badPackage;
        if (MakeAsset<TestAsset>(registry, TaggedId, taggedPackage).IsNull() ||
            MakeAsset<TestAsset>(registry, BadTagsId, badPackage).IsNull()) {
            return false;
        }

        auto tags = WritePackageTags(env.allocator, TaggedTags);
        const ImportSourceInput sourcesIn[] = {{"source", "", "Tagged.png", Sha256Digest{}, 1}};
        const ImportOutputInput outputs[] = {{"", FixedId(TaggedId), IdOf<TestAsset>()}};
        ImportRecordInput input;
        input.owner = FixedId(TaggedId);
        input.importerId = TaggedImporter;
        input.importerName = "TestImporter";
        input.importerVersion = 3;
        input.sources = sourcesIn;
        input.outputs = outputs;
        auto record = WriteImportRecord(env.allocator, FixedId(TaggedId), input);
        if (!tags || !record) {
            return false;
        }

        PackageWriteOptions taggedOptions = EditorOptions();
        taggedOptions.tags = tags.Value().View();
        taggedOptions.importRecord = record.Value().View();
        tagged = Write(env.allocator, registry, taggedPackage.Handle(), taggedOptions);
        const Array<uint8> broken = OneFieldRecord(env.allocator, PackageTagsSchema);
        PackageWriteOptions badOptions = EditorOptions();
        badOptions.tags = broken.View();
        badTags = Write(env.allocator, registry, badPackage.Handle(), badOptions);
        return !tagged.IsEmpty() && !badTags.IsEmpty();
    }

    TempProject root;
    ProjectLayout layout;
    MemoryFileSystem files;
    AssetDatabaseLock lock;
    Array<uint8> tagged;
    Array<uint8> badTags;
    bool ready = false;
};

PackageReadOptions ReadOptions() noexcept {
    PackageReadOptions options;
    options.expectedKind = PackageKind::Editor;
    options.engineVersion = TestEngineVersion;
    return options;
}

bool SameEntry(const AssetIndexEntry& left, const AssetIndexEntry& right) noexcept {
    if (!(left.id == right.id) || !(left.type == right.type) ||
        !(left.fingerprint == right.fingerprint) || left.fileBytes != right.fileBytes ||
        left.modifiedTicks != right.modifiedTicks || left.readOnly != right.readOnly ||
        left.path.View() != right.path.View() || left.key.View() != right.key.View() ||
        left.fileName.View() != right.fileName.View() || left.duplicateId != right.duplicateId ||
        left.duplicatePath != right.duplicatePath ||
        left.dependencies.Size() != right.dependencies.Size() ||
        left.tags.Size() != right.tags.Size() ||
        left.importRecord.present != right.importRecord.present ||
        !(left.importRecord.owner == right.importRecord.owner) ||
        !(left.importRecord.importerId == right.importRecord.importerId) ||
        left.importRecord.importerName.View() != right.importRecord.importerName.View() ||
        left.importRecord.importerVersion != right.importRecord.importerVersion ||
        left.importRecord.outputCount != right.importRecord.outputCount) {
        return false;
    }

    for (usize index = 0; index < left.dependencies.Size(); ++index) {
        const PackageDependencyEntry& a = left.dependencies[index];
        const PackageDependencyEntry& b = right.dependencies[index];
        if (!(a.asset == b.asset) || a.kind != b.kind || !(a.expectedType == b.expectedType) ||
            a.required != b.required) {
            return false;
        }
    }

    for (usize index = 0; index < left.tags.Size(); ++index) {
        if (left.tags[index].key.View() != right.tags[index].key.View() ||
            left.tags[index].value.View() != right.tags[index].value.View()) {
            return false;
        }
    }

    return true;
}

bool SameIndex(const AssetIndex& left, const AssetIndex& right) noexcept {
    if (left.Count() != right.Count() || left.Diagnostics().Size() != right.Diagnostics().Size()) {
        return false;
    }

    for (usize index = 0; index < left.Count(); ++index) {
        if (!SameEntry(left.At(index), right.At(index))) {
            return false;
        }
    }

    for (usize index = 0; index < left.Diagnostics().Size(); ++index) {
        const AssetIndexDiagnostic& a = left.Diagnostics()[index];
        const AssetIndexDiagnostic& b = right.Diagnostics()[index];
        if (a.code != b.code || a.error != b.error || a.fileName.View() != b.fileName.View()) {
            return false;
        }
    }

    return true;
}

// 본문 SHA-256(과 본문 byte 수)을 다시 쓴다(손상을 SHA-256 검사 뒤 단계에 닿게).
void Resign(Array<uint8>& bytes) noexcept {
    const uint64 body = bytes.Size() - HeaderBytes;
    StoreLittleEndian<uint64>(bytes.Data() + BodyBytesAt, body);
    const Sha256Digest digest =
        Sha256::Hash(Span<const uint8>(bytes.Data() + HeaderBytes, bytes.Size() - HeaderBytes));
    memcpy(bytes.Data() + BodyDigestAt, digest.bytes, DigestBytes);
}

// bytes 안 첫 needle을 replacement(같은 길이)로 바꾼다.
bool Patch(Array<uint8>& bytes, StringView needle, StringView replacement) noexcept {
    for (usize at = HeaderBytes; at + needle.ByteCount() <= bytes.Size(); ++at) {
        if (memcmp(bytes.Data() + at, needle.Data(), needle.ByteCount()) == 0) {
            memcpy(bytes.Data() + at, replacement.Data(), replacement.ByteCount());
            return true;
        }
    }

    return false;
}

// 시험 쪽 항목 작성기다(형식을 구현과 따로 쓴다 — 손상 모양을 정확히 만든다).
struct EntrySpec {
    StringView name = "A.rasset";
    uint8 readOnly = 0;
    uint32 dependencyCount = 0;
    uint32 dependencyKind = 1;
    uint8 required = 0;
    uint32 tagCount = 0;
    StringView tagKeys[2] = {"K", "L"};
    StringView tagValues[2] = {"v", "w"};
    uint8 present = 0;
    StringView importerName = "Imp";
    // 이름 길이를 부풀려 경계를 넘긴다.
    uint32 nameBytesOverride = 0;
    bool rawName = false;
    uint8 rawNameByte = 0;
    // 지문·id·타입(색인 재사용 시험이 실제 파일과 맞춘다 — null id·0 타입은 거부된다).
    Sha256Digest fingerprint{};
    Guid id{1, 1};
    Guid type{2, 2};
};

void WriteSpecText(ByteWriter& writer, StringView text) noexcept {
    writer.WriteU32LE(static_cast<uint32>(text.ByteCount()));
    writer.WriteBytes(
        Span<const uint8>(reinterpret_cast<const uint8*>(text.Data()), text.ByteCount()));
}

void WriteEntry(ByteWriter& writer, const EntrySpec& spec) noexcept {
    if (spec.rawName) {
        writer.WriteU32LE(1);
        writer.WriteU8(spec.rawNameByte);
    } else if (spec.nameBytesOverride != 0) {
        writer.WriteU32LE(spec.nameBytesOverride);
        writer.WriteBytes(Span<const uint8>(reinterpret_cast<const uint8*>(spec.name.Data()),
                                            spec.name.ByteCount()));
    } else {
        WriteSpecText(writer, spec.name);
    }

    writer.WriteBytes(Span<const uint8>(spec.fingerprint.bytes, DigestBytes));
    writer.WriteU64LE(spec.id.high);
    writer.WriteU64LE(spec.id.low);
    writer.WriteU64LE(spec.type.high);
    writer.WriteU64LE(spec.type.low);
    writer.WriteU8(spec.readOnly);
    writer.WriteU32LE(spec.dependencyCount);
    for (uint32 index = 0; index < spec.dependencyCount && index < 2; ++index) {
        writer.WriteZeros(16);
        writer.WriteU32LE(spec.dependencyKind);
        writer.WriteZeros(16);
        writer.WriteU8(spec.required);
    }

    writer.WriteU32LE(spec.tagCount);
    for (uint32 index = 0; index < spec.tagCount && index < 2; ++index) {
        WriteSpecText(writer, spec.tagKeys[index]);
        WriteSpecText(writer, spec.tagValues[index]);
    }

    writer.WriteU8(spec.present);
    if (spec.present != 0) {
        writer.WriteZeros(32);
        WriteSpecText(writer, spec.importerName);
        writer.WriteU32LE(1);
        writer.WriteU64LE(1);
    }
}

// key의 헤더 + entries 본문(서명함)을 만든다.
Array<uint8> MakeCache(Allocator& allocator, const AssetIndexCacheKey& key,
                       Span<const EntrySpec> entries, uint32 countOverride = 0) noexcept {
    Array<uint8> bytes(allocator);
    ByteWriter writer(bytes);
    writer.WriteU32LE(0x49414b52u);
    writer.WriteU32LE(key.formatVersion);
    writer.WriteU32LE(key.schemaVersion);
    writer.WriteU32LE(key.semanticVersion);
    writer.WriteBytes(Span<const uint8>(key.unicode.bytes, DigestBytes));
    writer.WriteU16LE(key.compatibleMajor);
    writer.WriteU16LE(key.compatibleMinor);
    writer.WriteU8(key.normalizesNames ? 1 : 0);
    writer.WriteZeros(3);
    writer.WriteBytes(Span<const uint8>(key.customVersions.bytes, DigestBytes));
    writer.WriteZeros(DigestBytes);
    writer.WriteU32LE(countOverride != 0 ? countOverride : static_cast<uint32>(entries.Size()));
    writer.WriteU64LE(0);
    writer.WriteZeros(DigestBytes);
    for (const EntrySpec& entry : entries) {
        WriteEntry(writer, entry);
    }

    CHECK(writer.Status());
    Resign(bytes);
    return bytes;
}

// count byte의 'k' 문자열이다.
String Filled(Allocator& allocator, usize count) noexcept {
    String text(allocator);
    for (usize index = 0; index < count; ++index) {
        CHECK(text.TryAppend("k"));
    }

    return text;
}

bool HasTemporary(MemoryFileSystem& files) noexcept {
    for (usize index = 0; index < files.NodeCount(); ++index) {
        if (files.NodeName(index).EndsWith(".tmp", StringComparison::CaseSensitive)) {
            return true;
        }
    }

    return false;
}

} // namespace

// 왕복과 재사용: 캐시 없는 검색을 캐시로 쓰고 읽으면 진단 없는 파일만 담긴다(Tags 진단의 BadTags·
// 읽기 실패 Junk는 없다, 같은 id 진단의 Copy·Sub/Leaf는 있다 — 게시가 다시 정한다). 항목 칸(id·
// 타입·readOnly·의존·태그·ImportRecord 요약·지문)이 색인과 같고 캐시를 쓴 검색은 캐시 없는 검색과
// 같다(진단 포함). 지문이 같은 파일은 다시 읽지 않는다(캐시 byte의 태그 값을 바꾸면 그 값이 보인다)
// 그리고 지문이 다르면 다시 읽는다.
TEST(AssetIndexCache_RoundTripsAndReuses) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    CacheScene scene(env, sources, "cache-round-trip");
    REQUIRE(scene.ready);
    const PackageReadOptions options = ReadOptions();
    AssetIndex plain(env.allocator, scene.layout);
    REQUIRE(plain.Rebuild(scene.files, options));
    REQUIRE(plain.Count() == 9);
    REQUIRE(WriteAssetIndexCache(scene.files, scene.layout, scene.lock, plain, options));
    CHECK(scene.files.Has("Intermediate/AssetIndex.cache") && !HasTemporary(scene.files));
    AssetIndexCache cache(env.allocator);
    auto loaded = cache.Load(scene.files, scene.layout, options);
    REQUIRE(loaded && loaded.Value());
    CHECK(cache.Count() == 8);
    CHECK(cache.ToolBuildId() == build::ToolBuildId());
    for (usize index = 0; index < plain.Count(); ++index) {
        const AssetIndexEntry& entry = plain.At(index);
        const AssetIndexCacheEntry* cached = cache.Find(entry.fileName.View(), entry.fingerprint);
        if (entry.fileName.View() == "BadTags.rasset") {
            CHECK(cached == nullptr);
            continue;
        }

        REQUIRE(cached != nullptr);
        CHECK(cached->id == entry.id && cached->type == entry.type &&
              cached->readOnly == entry.readOnly &&
              cached->dependencies.Size() == entry.dependencies.Size() &&
              cached->tags.Size() == entry.tags.Size() &&
              cached->importRecord.present == entry.importRecord.present &&
              cached->importRecord.importerName.View() == entry.importRecord.importerName.View());
        CHECK(cache.Find(entry.fileName.View(), PackageFingerprint{}) == nullptr);
    }

    CHECK(cache.Find("Missing.rasset", PackageFingerprint{}) == nullptr);
    AssetIndex reused(env.allocator, scene.layout);
    REQUIRE(reused.Rebuild(scene.files, options, &cache));
    CHECK(SameIndex(plain, reused));

    // 재사용 증거: 캐시의 Tagged 태그 값을 바꾸면(같은 지문) 그 값이 보인다.
    Array<uint8> bytes(env.allocator);
    REQUIRE(EncodeAssetIndexCache(plain, MakeAssetIndexCacheKey(options), bytes));
    REQUIRE(Patch(bytes, "Champion", "Champixn"));
    Resign(bytes);
    AssetIndexCache patched(env.allocator);
    auto parsed = patched.Parse(bytes.View(), MakeAssetIndexCacheKey(options));
    REQUIRE(parsed && parsed.Value());
    AssetIndex fromPatched(env.allocator, scene.layout);
    REQUIRE(fromPatched.Rebuild(scene.files, options, &patched));
    Array<const AssetIndexEntry*> hexo(env.allocator);
    REQUIRE(fromPatched.FindByTag("Kind", "Champixn", hexo));
    CHECK(hexo.Size() == 1);

    // 지문이 다르면(파일 내용 교체) 다시 읽는다.
    scene.files.Remove("Content/Tagged.rasset");
    REQUIRE(scene.files.AddFile("Content/Tagged.rasset", sources.villain.View()));
    AssetIndex changed(env.allocator, scene.layout);
    REQUIRE(changed.Rebuild(scene.files, options, &patched));
    hexo.Clear();
    REQUIRE(changed.FindByTag("Kind", "Champixn", hexo));
    CHECK(hexo.IsEmpty());
}

// 키: 패치 버전만 다르면 같은 키, 주·부·customVersions(값·개수)·이름 정규화 플래그가 다르면 다른
// 키. 키 칸 하나라도 다른 캐시는 없는 것과 같다(형식·항목 스키마·판정
// 의미·Unicode·주·부·customVersions· 정규화 — 칸마다).
TEST(AssetIndexCache_KeyAxes) {
    AssetEnv env;
    REQUIRE(env.Ready());
    PackageReadOptions options = ReadOptions();
    const AssetIndexCacheKey base = MakeAssetIndexCacheKey(options);
    const StringView unicode = UnicodeVersionText();
    CHECK(base.unicode ==
          Sha256::Hash(Span<const uint8>(reinterpret_cast<const uint8*>(unicode.Data()),
                                         unicode.ByteCount())));
    CHECK(base.formatVersion == AssetIndexCacheFormatVersion &&
          base.schemaVersion == AssetIndexCacheSchemaVersion &&
          base.semanticVersion == AssetIndexCacheSemanticVersion &&
          base.normalizesNames == FileSystemNormalizesNames);
    PackageReadOptions patch = options;
    patch.engineVersion.patchVersion = static_cast<uint16>(patch.engineVersion.patchVersion + 1);
    CHECK(MakeAssetIndexCacheKey(patch) == base);
    // 엔진 축을 끈 실행(빈 엔진 버전 — 주·부 0)은 켠 실행과 다른 키다(주·부 칸이 가른다). 빈 값은
    // 패치와 상관없이 하나다(IsEmpty는 주·부만 본다).
    PackageReadOptions noEngine = options;
    noEngine.engineVersion = EngineVersion{};
    CHECK(!(MakeAssetIndexCacheKey(noEngine) == base));
    PackageReadOptions emptyPatch = options;
    emptyPatch.engineVersion = EngineVersion{0, 0, 1};
    CHECK(emptyPatch.engineVersion.IsEmpty() &&
          MakeAssetIndexCacheKey(emptyPatch) == MakeAssetIndexCacheKey(noEngine));
    PackageReadOptions minor = options;
    minor.engineVersion.minorVersion = static_cast<uint16>(minor.engineVersion.minorVersion + 1);
    CHECK(!(MakeAssetIndexCacheKey(minor) == base));
    PackageReadOptions major = options;
    major.engineVersion.majorVersion = static_cast<uint16>(major.engineVersion.majorVersion + 1);
    CHECK(!(MakeAssetIndexCacheKey(major) == base));
    const PackageCustomVersion one[] = {{Guid{1, 2}, 3}};
    const PackageCustomVersion other[] = {{Guid{1, 2}, 4}};
    const PackageCustomVersion two[] = {{Guid{1, 2}, 3}, {Guid{5, 6}, 1}};
    PackageReadOptions custom = options;
    custom.customVersions = one;
    const AssetIndexCacheKey oneKey = MakeAssetIndexCacheKey(custom);
    CHECK(!(oneKey == base));
    custom.customVersions = other;
    CHECK(!(MakeAssetIndexCacheKey(custom) == oneKey));
    custom.customVersions = two;
    CHECK(!(MakeAssetIndexCacheKey(custom) == oneKey));

    const EntrySpec entry;
    const Array<uint8> bytes = MakeCache(env.allocator, base, Span<const EntrySpec>(&entry, 1));
    AssetIndexCache cache(env.allocator);
    auto accepted = cache.Parse(bytes.View(), base);
    REQUIRE(accepted && accepted.Value());
    CHECK(cache.Count() == 1);
    AssetIndexCacheKey keys[8] = {base, base, base, base, base, base, base, base};
    ++keys[0].formatVersion;
    ++keys[1].schemaVersion;
    ++keys[2].semanticVersion;
    keys[3].unicode.bytes[0] ^= 1;
    ++keys[4].compatibleMajor;
    ++keys[5].compatibleMinor;
    keys[6].customVersions.bytes[0] ^= 1;
    keys[7].normalizesNames = !keys[7].normalizesNames;
    for (const AssetIndexCacheKey& key : keys) {
        auto rejected = cache.Parse(bytes.View(), key);
        CHECK(rejected && !rejected.Value() && cache.Count() == 0);
        // 그 키로 쓴 캐시는 기준 키로 읽히지 않는다(헤더의 칸이 대조된다).
        const Array<uint8> written =
            MakeCache(env.allocator, key, Span<const EntrySpec>(&entry, 1));
        auto mismatched = cache.Parse(written.View(), base);
        CHECK(mismatched && !mismatched.Value());
    }
}

// 신뢰하지 않는 입력: 헤더(짧음·magic·예약 byte·정규화 칸 2·본문 byte 불일치·SHA-256 불일치·항목 수
// 상한·크기 상한), 항목(이름 UTF-8·빈 이름·.rasset 아님·이름 길이 경계·같은 이름·순서·readOnly 2·
// 의존 수 경계·의존 종류 0·6·required 2·태그 수 경계·빈 태그 키·태그 키·값 길이 상한·태그 순서·같은
// 태그·null id·0 타입·요약 표지 2·
// 요약 이름 UTF-8·남은 byte·잘림)의 위반은 모두 캐시 없음(비운 채 false)이다. 올바른 여러 항목은
// 받는다(경계 값 — 태그 둘·의존 둘·요약 있음·readOnly 1).
TEST(AssetIndexCache_RejectsUntrustedInput) {
    AssetEnv env;
    REQUIRE(env.Ready());
    const AssetIndexCacheKey key = MakeAssetIndexCacheKey(ReadOptions());
    AssetIndexCache cache(env.allocator);
    auto rejects = [&](const Array<uint8>& bytes) noexcept {
        auto parsed = cache.Parse(bytes.View(), key);
        return parsed && !parsed.Value() && cache.Count() == 0;
    };

    EntrySpec full;
    full.readOnly = 1;
    full.dependencyCount = 2;
    full.dependencyKind = static_cast<uint32>(PackageDependencyKind::ImportOwner);
    full.required = 1;
    full.tagCount = 2;
    full.present = 1;
    EntrySpec second;
    second.name = "B.rasset";
    const EntrySpec valid[] = {full, second};
    const Array<uint8> good = MakeCache(env.allocator, key, valid);
    auto accepted = cache.Parse(good.View(), key);
    REQUIRE(accepted && accepted.Value());
    REQUIRE(cache.Count() == 2);
    CHECK(
        cache.At(0).readOnly && cache.At(0).dependencies.Size() == 2 &&
        cache.At(0).dependencies[1].kind == PackageDependencyKind::ImportOwner &&
        cache.At(0).dependencies[0].required && cache.At(0).tags.Size() == 2 &&
        cache.At(0).importRecord.present && cache.At(0).importRecord.importerName.View() == "Imp" &&
        cache.At(0).importRecord.importerVersion == 1 && cache.At(0).importRecord.outputCount == 1);

    // 헤더.
    {
        Array<uint8> shortHeader(env.allocator);
        REQUIRE(shortHeader.TryAppend(Span<const uint8>(good.Data(), HeaderBytes - 1)));
        CHECK(rejects(shortHeader));
        Array<uint8> magic(env.allocator);
        REQUIRE(magic.TryAppend(good.View()));
        magic[0] ^= 1;
        CHECK(rejects(magic));
        Array<uint8> reserved(env.allocator);
        REQUIRE(reserved.TryAppend(good.View()));
        reserved[ReservedAt] = 1;
        CHECK(rejects(reserved));
        Array<uint8> normalizes(env.allocator);
        REQUIRE(normalizes.TryAppend(good.View()));
        normalizes[NormalizesAt] = 2;
        CHECK(rejects(normalizes));
        Array<uint8> bodyBytes(env.allocator);
        REQUIRE(bodyBytes.TryAppend(good.View()));
        StoreLittleEndian<uint64>(bodyBytes.Data() + BodyBytesAt, good.Size() - HeaderBytes + 1);
        CHECK(rejects(bodyBytes));
        Array<uint8> digest(env.allocator);
        REQUIRE(digest.TryAppend(good.View()));
        digest[digest.Size() - 1] ^= 1;
        CHECK(rejects(digest));
        Array<uint8> count(env.allocator);
        REQUIRE(count.TryAppend(good.View()));
        StoreLittleEndian<uint32>(count.Data() + CountAt, 0xffffffffu);
        CHECK(rejects(count));
        Array<uint8> fewer(env.allocator);
        REQUIRE(fewer.TryAppend(good.View()));
        StoreLittleEndian<uint32>(fewer.Data() + CountAt, 1);
        CHECK(rejects(fewer));
        // 크기 상한(넘는 크기는 내용을 보지 않는다).
        auto huge = cache.Parse(Span<const uint8>(good.Data(), MaxAssetIndexCacheBytes + 1), key);
        CHECK(huge && !huge.Value());
    }

    // 항목.
    struct Case {
        EntrySpec spec;
        const char* label;
    };
    Case cases[] = {{EntrySpec{}, "raw name"},
                    {EntrySpec{}, "empty name"},
                    {EntrySpec{}, "not rasset"},
                    {EntrySpec{}, "name too long"},
                    {EntrySpec{}, "readOnly 2"},
                    {EntrySpec{}, "dependency count"},
                    {EntrySpec{}, "kind 0"},
                    {EntrySpec{}, "kind 6"},
                    {EntrySpec{}, "required 2"},
                    {EntrySpec{}, "tag count"},
                    {EntrySpec{}, "empty tag key"},
                    {EntrySpec{}, "tag order"},
                    {EntrySpec{}, "same tag"},
                    {EntrySpec{}, "present 2"},
                    {EntrySpec{}, "importer name"},
                    {EntrySpec{}, "null id"},
                    {EntrySpec{}, "zero type"},
                    {EntrySpec{}, "tag key too long"},
                    {EntrySpec{}, "tag value too long"}};
    cases[0].spec.rawName = true;
    cases[0].spec.rawNameByte = 0xff;
    cases[1].spec.name = "";
    cases[2].spec.name = "A.txt";
    cases[3].spec.nameBytesOverride = 1000;
    cases[4].spec.readOnly = 2;
    cases[5].spec.dependencyCount = 1000000;
    cases[6].spec.dependencyCount = 1;
    cases[6].spec.dependencyKind = 0;
    cases[7].spec.dependencyCount = 1;
    cases[7].spec.dependencyKind = 6;
    cases[8].spec.dependencyCount = 1;
    cases[8].spec.required = 2;
    cases[9].spec.tagCount = 1000000;
    cases[10].spec.tagCount = 1;
    cases[10].spec.tagKeys[0] = "";
    cases[11].spec.tagCount = 2;
    cases[11].spec.tagKeys[0] = "Z";
    cases[12].spec.tagCount = 2;
    cases[12].spec.tagKeys[1] = "K";
    cases[12].spec.tagValues[1] = "v";
    cases[13].spec.present = 2;
    cases[14].spec.present = 1;
    cases[14].spec.importerName = "\xff";
    cases[15].spec.id = Guid{};
    cases[16].spec.type = Guid{};
    const String longKey = Filled(env.allocator, MaxPackageTagKeyBytes + 1);
    const String longValue = Filled(env.allocator, PackageMaxStringBytes + 1);
    const String maxKey = Filled(env.allocator, MaxPackageTagKeyBytes);
    cases[17].spec.tagCount = 1;
    cases[17].spec.tagKeys[0] = longKey.View();
    cases[18].spec.tagCount = 1;
    cases[18].spec.tagValues[0] = longValue.View();
    for (const Case& item : cases) {
        const Array<uint8> bytes =
            MakeCache(env.allocator, key, Span<const EntrySpec>(&item.spec, 1));
        CHECK(rejects(bytes));
    }

    // 키 길이 상한 그대로는 받는다.
    EntrySpec maxTag;
    maxTag.tagCount = 1;
    maxTag.tagKeys[0] = maxKey.View();
    auto acceptedMax =
        cache.Parse(MakeCache(env.allocator, key, Span<const EntrySpec>(&maxTag, 1)).View(), key);
    CHECK(acceptedMax && acceptedMax.Value());

    // 같은 이름·역순 이름·남은 byte·잘린 항목.
    const EntrySpec same[] = {EntrySpec{}, EntrySpec{}};
    CHECK(rejects(MakeCache(env.allocator, key, same)));
    const EntrySpec reversed[] = {second, EntrySpec{}};
    CHECK(rejects(MakeCache(env.allocator, key, reversed)));
    Array<uint8> trailing = MakeCache(env.allocator, key, Span<const EntrySpec>(&full, 1));
    REQUIRE(trailing.TryPushBack(0));
    Resign(trailing);
    CHECK(rejects(trailing));
    Array<uint8> truncated = MakeCache(env.allocator, key, Span<const EntrySpec>(&full, 1));
    REQUIRE(truncated.TryResize(truncated.Size() - 1));
    Resign(truncated);
    CHECK(rejects(truncated));
    // 잘린 항목: 모든 칸을 가진 항목을 길이마다 자르면(서명은 다시 한다) 어느 칸에서 잘려도
    // 거부한다.
    EntrySpec longName = full;
    longName.name = "AVeryLongPackageNameSoTruncationReachesEveryField0123456789.rasset";
    // 긴 태그 키·값(태그 수 상한 검사를 지나 키·값 읽기에서 잘리게).
    longName.tagKeys[0] = "AVeryLongTagKey0123456789";
    longName.tagKeys[1] = "BVeryLongTagKey0123456789";
    longName.tagValues[0] = "AVeryLongTagValue0123456789";
    longName.tagValues[1] = "BVeryLongTagValue0123456789";
    const Array<uint8> whole = MakeCache(env.allocator, key, Span<const EntrySpec>(&longName, 1));
    for (usize length = HeaderBytes; length < whole.Size(); ++length) {
        Array<uint8> cut(env.allocator);
        REQUIRE(cut.TryAppend(Span<const uint8>(whole.Data(), length)));
        Resign(cut);
        CHECK(rejects(cut));
    }

    // 항목 수가 본문보다 많다(마지막 항목이 없다).
    CHECK(rejects(MakeCache(env.allocator, key, Span<const EntrySpec>(&full, 1), 2)));
}

// 읽기: 파일 없음·디렉터리·열기 실패·읽기 실패는 캐시 없음(false)이고, 할당 실패는 OutOfMemory다
// (그 밖은 성공 — 크기 0 캐시도 캐시 없음).
TEST(AssetIndexCache_LoadFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    CacheScene scene(env, sources, "cache-load");
    REQUIRE(scene.ready);
    const PackageReadOptions options = ReadOptions();
    AssetIndexCache cache(env.allocator);
    auto missing = cache.Load(scene.files, scene.layout, options);
    CHECK(missing && !missing.Value());
    REQUIRE(scene.files.AddDirectory("Intermediate/AssetIndex.cache"));
    auto directory = cache.Load(scene.files, scene.layout, options);
    CHECK(directory && !directory.Value());
    scene.files.Remove("Intermediate/AssetIndex.cache");
    REQUIRE(scene.files.AddFile("Intermediate/AssetIndex.cache", Span<const uint8>()));
    auto empty = cache.Load(scene.files, scene.layout, options);
    CHECK(empty && !empty.Value());
    scene.files.Remove("Intermediate/AssetIndex.cache");

    AssetIndex index(env.allocator, scene.layout);
    REQUIRE(index.Rebuild(scene.files, options));
    REQUIRE(WriteAssetIndexCache(scene.files, scene.layout, scene.lock, index, options));
    Array<uint8> bytes(env.allocator);
    REQUIRE(bytes.TryAppend(scene.files.Bytes("Intermediate/AssetIndex.cache")));
    scene.files.Remove("Intermediate/AssetIndex.cache");
    const MemoryFault faults[] = {MemoryFault::OpenMissing, MemoryFault::OpenBusy,
                                  MemoryFault::Read};
    for (MemoryFault fault : faults) {
        REQUIRE(scene.files.AddFile("Intermediate/AssetIndex.cache", bytes.View(), fault));
        auto failed = cache.Load(scene.files, scene.layout, options);
        CHECK(failed && !failed.Value() && cache.Count() == 0);
        scene.files.Remove("Intermediate/AssetIndex.cache");
    }

    REQUIRE(scene.files.AddFile("Intermediate/AssetIndex.cache", bytes.View()));
    test::NFailAllocator failing(env.allocator);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        AssetIndexCache attempted(failing);
        failing.FailOnAttempt(attempt);
        auto loaded = attempted.Load(scene.files, scene.layout, options);
        const bool triggered = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!triggered) {
            CHECK(loaded && loaded.Value() && attempted.Count() == 8);
            break;
        }

        CHECK(!loaded && loaded.Error() == ErrorCode::OutOfMemory && attempted.Count() == 0);
        ++failures;
    }

    CHECK(failures > 0);
}

// 쓰기: 이 프로젝트의 잡은 잠금이 아니면 InvalidArgument(아무것도 안 씀), 디렉터리 만들기·만들기·
// 쓰기·Flush·Close·교체 실패는 그 오류이고 임시 파일이 남지 않으며 이전 캐시가 그대로다. 교체
// NotDurable은 성공이다. 할당 실패는 OutOfMemory이고 아무것도 안 쓴다.
TEST(AssetIndexCache_WriteFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    CacheScene scene(env, sources, "cache-write");
    REQUIRE(scene.ready);
    const PackageReadOptions options = ReadOptions();
    AssetIndex index(env.allocator, scene.layout);
    REQUIRE(index.Rebuild(scene.files, options));
    AssetDatabaseLock idle;
    CHECK(WriteAssetIndexCache(scene.files, scene.layout, idle, index, options).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(!scene.files.Has("Intermediate"));
    scene.files.faults.makeDirectory = MemoryBusy;
    CHECK(!WriteAssetIndexCache(scene.files, scene.layout, scene.lock, index, options));
    scene.files.faults.makeDirectory = {};
    REQUIRE(WriteAssetIndexCache(scene.files, scene.layout, scene.lock, index, options));
    Array<uint8> before(env.allocator);
    REQUIRE(before.TryAppend(scene.files.Bytes("Intermediate/AssetIndex.cache")));
    AssetIndex smaller(env.allocator, scene.layout);
    scene.files.Remove("Content/Tagged.rasset");
    REQUIRE(smaller.Rebuild(scene.files, options));

    auto unchanged = [&]() noexcept {
        const Span<const uint8> now = scene.files.Bytes("Intermediate/AssetIndex.cache");
        return now.Size() == before.Size() && memcmp(now.Data(), before.Data(), now.Size()) == 0 &&
               !HasTemporary(scene.files);
    };
    scene.files.faults.create = MemoryBusy;
    CHECK(!WriteAssetIndexCache(scene.files, scene.layout, scene.lock, smaller, options));
    scene.files.faults.create = {};
    CHECK(unchanged());
    scene.files.faults.failWriteCall = scene.files.WriteCalls() + 1;
    CHECK(!WriteAssetIndexCache(scene.files, scene.layout, scene.lock, smaller, options));
    scene.files.faults.failWriteCall = 0;
    CHECK(unchanged());
    scene.files.faults.flush = MemoryBusy;
    CHECK(!WriteAssetIndexCache(scene.files, scene.layout, scene.lock, smaller, options));
    scene.files.faults.flush = {};
    CHECK(unchanged());
    scene.files.faults.close = MemoryBusy;
    CHECK(!WriteAssetIndexCache(scene.files, scene.layout, scene.lock, smaller, options));
    scene.files.faults.close = {};
    CHECK(unchanged());
    scene.files.faults.replace = MemoryBusy;
    CHECK(!WriteAssetIndexCache(scene.files, scene.layout, scene.lock, smaller, options));
    scene.files.faults.replace = {};
    CHECK(unchanged());
    scene.files.faults.replace = MemoryNotDurable;
    CHECK(WriteAssetIndexCache(scene.files, scene.layout, scene.lock, smaller, options));
    scene.files.faults.replace = {};
    CHECK(!unchanged() && !HasTemporary(scene.files));

    test::NFailAllocator failing(env.allocator);
    ProjectLayout failingLayout(failing);
    REQUIRE(failingLayout.Initialize(scene.root.Root()));
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        scene.files.Remove("Intermediate/AssetIndex.cache");
        failing.FailOnAttempt(attempt);
        auto written =
            WriteAssetIndexCache(scene.files, failingLayout, scene.lock, smaller, options);
        const bool triggered = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!triggered) {
            CHECK(written && scene.files.Has("Intermediate/AssetIndex.cache"));
            break;
        }

        CHECK(!written && written.Error() == ErrorCode::OutOfMemory &&
              !scene.files.Has("Intermediate/AssetIndex.cache") && !HasTemporary(scene.files));
        ++failures;
    }

    CHECK(failures > 0);
}

// 재사용의 가장자리: 빈 캐시는 캐시 없음과 같은 결과다. 캐시에 있는 파일이 열 때 사라졌으면(열기
// 없음)·지문을 읽을 수 없으면(내용이 손상) 부분 읽기가 같은 진단을 만든다. 캐시 이름이 논리 경로
// 규칙을 어기면(조작한 캐시 — 같은 지문) 쓰지 않고 부분 읽기가 InvalidPath를 만든다. 조작한 캐시의
// 값(readOnly·required 의존)은 그대로 색인에 들어가고 다시 인코딩하면 남는다. 캐시를 쓰는 검색의
// 할당 실패는 OutOfMemory이고 색인은 비고, 실패가 없어지면 캐시 없는 검색과 같다.
TEST(AssetIndexCache_ReuseEdges) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    CacheScene scene(env, sources, "cache-reuse-edges");
    REQUIRE(scene.ready);
    const PackageReadOptions options = ReadOptions();
    const AssetIndexCacheKey key = MakeAssetIndexCacheKey(options);
    AssetIndex plain(env.allocator, scene.layout);
    REQUIRE(plain.Rebuild(scene.files, options));
    Array<uint8> bytes(env.allocator);
    REQUIRE(EncodeAssetIndexCache(plain, key, bytes));
    AssetIndexCache cache(env.allocator);
    REQUIRE(cache.Parse(bytes.View(), key));
    auto same = [&](const AssetIndexCache* used) noexcept {
        AssetIndex without(env.allocator, scene.layout);
        AssetIndex with(env.allocator, scene.layout);
        return without.Rebuild(scene.files, options) && with.Rebuild(scene.files, options, used) &&
               SameIndex(without, with);
    };

    const Array<uint8> empty = MakeCache(env.allocator, key, Span<const EntrySpec>());
    AssetIndexCache none(env.allocator);
    auto parsedEmpty = none.Parse(empty.View(), key);
    REQUIRE(parsedEmpty && parsedEmpty.Value() && none.Count() == 0);
    CHECK(same(&none));

    // 열기 없음·지문 손상.
    scene.files.Remove("Content/Base.rasset");
    REQUIRE(
        scene.files.AddFile("Content/Base.rasset", sources.base.View(), MemoryFault::OpenMissing));
    CHECK(same(&cache));
    scene.files.Remove("Content/Base.rasset");
    REQUIRE(scene.files.AddFile("Content/Base.rasset", CacheScene::JunkBytes()));
    CHECK(same(&cache));
    scene.files.Remove("Content/Base.rasset");
    REQUIRE(scene.files.AddFile("Content/Base.rasset", sources.base.View()));

    // 조작한 캐시: 규칙을 어긴 이름(같은 지문)과 readOnly·required 의존 값.
    const AssetIndexEntry* base = nullptr;
    for (usize index = 0; index < plain.Count(); ++index) {
        if (plain.At(index).fileName.View() == "Base.rasset") {
            base = &plain.At(index);
        }
    }

    REQUIRE(base != nullptr);
    REQUIRE(scene.files.AddFile("Content/Bad Name.rasset", sources.base.View()));
    EntrySpec badName;
    badName.name = "Bad Name.rasset";
    badName.fingerprint = base->fingerprint.digest;
    badName.id = base->id.value;
    badName.type = base->type;
    EntrySpec forged;
    forged.name = "Base.rasset";
    forged.fingerprint = base->fingerprint.digest;
    forged.id = base->id.value;
    forged.type = base->type;
    forged.readOnly = 1;
    forged.dependencyCount = 1;
    forged.required = 1;
    const EntrySpec crafted[] = {badName, forged};
    const Array<uint8> forgedBytes = MakeCache(env.allocator, key, crafted);
    AssetIndexCache forgedCache(env.allocator);
    REQUIRE(forgedCache.Parse(forgedBytes.View(), key));
    AssetIndex fromForged(env.allocator, scene.layout);
    REQUIRE(fromForged.Rebuild(scene.files, options, &forgedCache));
    bool invalidPath = false;
    for (const AssetIndexDiagnostic& diagnostic : fromForged.Diagnostics()) {
        invalidPath = invalidPath || (diagnostic.code == AssetDiagnosticCode::InvalidPath &&
                                      diagnostic.fileName.View() == "Bad Name.rasset");
    }

    CHECK(invalidPath);
    Array<uint8> again(env.allocator);
    REQUIRE(EncodeAssetIndexCache(fromForged, key, again));
    AssetIndexCache reread(env.allocator);
    REQUIRE(reread.Parse(again.View(), key));
    const AssetIndexCacheEntry* forgedEntry = reread.Find("Base.rasset", base->fingerprint);
    REQUIRE(forgedEntry != nullptr);
    CHECK(forgedEntry->readOnly && forgedEntry->dependencies.Size() == 1 &&
          forgedEntry->dependencies[0].required);
    scene.files.Remove("Content/Bad Name.rasset");

    // 할당 실패.
    test::NFailAllocator failing(env.allocator);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        AssetIndex index(failing, scene.layout);
        failing.FailOnAttempt(attempt);
        auto rebuilt = index.Rebuild(scene.files, options, &cache);
        const bool triggered = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!triggered) {
            REQUIRE(rebuilt);
            CHECK(SameIndex(plain, index));
            break;
        }

        CHECK(!rebuilt && rebuilt.Error() == ErrorCode::OutOfMemory && index.Count() == 0);
        ++failures;
    }

    CHECK(failures > 0);
}

// 색인 읽기는 TypeRegistry를 쓰지 않는다(7.1절 16): options.types가 있어도 결과가 없을 때와 같다.
TEST(AssetIndexCache_IndexIgnoresTypes) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    CacheScene scene(env, sources, "cache-types");
    REQUIRE(scene.ready);
    PackageReadOptions options = ReadOptions();
    AssetIndex plain(env.allocator, scene.layout);
    REQUIRE(plain.Rebuild(scene.files, options));
    options.types = &env.types;
    AssetIndex typed(env.allocator, scene.layout);
    REQUIRE(typed.Rebuild(scene.files, options));
    CHECK(SameIndex(plain, typed));
}

// 고아 임시 파일 정리: Content(하위 디렉터리 포함)의 `<이름>.rasset.<guid>.tmp`와 Intermediate 바로
// 아래 `AssetIndex.cache.<guid>.tmp`만 지운다 — 대문자 guid·다른 확장자·빈 이름·Content의 캐시
// 임시 이름·Intermediate의 다른 캐시 이름·Intermediate 하위 디렉터리·링크·디렉터리는 남긴다. 지울
// 수 없는 파일·열거할 수 없는 하위 디렉터리는 실패 목록이고 정리는 계속한다. 잠금 아님
// InvalidArgument, Content가 파일이면 그 오류, 없는 디렉터리는 비어 있음, 할당 실패는
// OutOfMemory다.
TEST(AssetIndexCache_DeletesTemporaryOrphans) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    const uint8 one[] = {1};
    const StringView deleted[] = {
        "Content/A.rasset.0123abcd-0123-4567-89ab-0123456789ab.tmp",
        "Content/Sub/B.rasset.0123abcd-0123-4567-89ab-0123456789ab.tmp",
        "Intermediate/AssetIndex.cache.0123abcd-0123-4567-89ab-0123456789ab.tmp"};
    const StringView kept[] = {
        "Content/C.rasset.0123ABCD-0123-4567-89ab-0123456789ab.tmp",
        "Content/D.txt.0123abcd-0123-4567-89ab-0123456789ab.tmp",
        "Content/.rasset.0123abcd-0123-4567-89ab-0123456789ab.tmp",
        "Content/AssetIndex.cache.0123abcd-0123-4567-89ab-0123456789ab.tmp",
        "Content/E.rasset.0123abcd-0123-4567-89ab-0123456789ab.TMP",
        "Intermediate/Other.cache.0123abcd-0123-4567-89ab-0123456789ab.tmp",
        "Intermediate/Sub/AssetIndex.cache.0123abcd-0123-4567-89ab-0123456789ab.tmp",
        "Intermediate/AssetDatabase.lock"};
    CacheScene scene(env, sources, "cache-orphans");
    REQUIRE(scene.ready);
    REQUIRE(scene.files.AddDirectory("Intermediate") &&
            scene.files.AddDirectory("Intermediate/Sub"));
    for (StringView name : deleted) {
        REQUIRE(scene.files.AddFile(name, Span<const uint8>(one, 1)));
    }

    for (StringView name : kept) {
        REQUIRE(scene.files.AddFile(name, Span<const uint8>(one, 1)));
    }

    REQUIRE(scene.files.AddFile("Content/Link.rasset.0123abcd-0123-4567-89ab-0123456789ab.tmp",
                                Span<const uint8>(one, 1), MemoryFault::None,
                                platform::FileType::File, true));
    REQUIRE(
        scene.files.AddDirectory("Content/Dir.rasset.0123abcd-0123-4567-89ab-0123456789ab.tmp"));
    REQUIRE(scene.files.AddFile(
        StringView("Content/\xff.rasset.0123abcd-0123-4567-89ab-0123456789ab.tmp"),
        Span<const uint8>(one, 1)));
    REQUIRE(scene.files.AddFile("Content/Device.rasset.0123abcd-0123-4567-89ab-0123456789ab.tmp",
                                Span<const uint8>(one, 1), MemoryFault::None,
                                platform::FileType::Other));
    usize removed = 0;
    Array<TemporaryFileFailure> failures(env.allocator);
    REQUIRE(DeleteTemporaryOrphans(scene.files, scene.layout, scene.lock, removed, failures));
    CHECK(removed == 3 && failures.IsEmpty());
    for (StringView name : deleted) {
        CHECK(!scene.files.Has(name));
    }

    for (StringView name : kept) {
        CHECK(scene.files.Has(name));
    }

    CHECK(scene.files.Has("Content/Link.rasset.0123abcd-0123-4567-89ab-0123456789ab.tmp") &&
          scene.files.Has("Content/Dir.rasset.0123abcd-0123-4567-89ab-0123456789ab.tmp") &&
          scene.files.Has("Content/Device.rasset.0123abcd-0123-4567-89ab-0123456789ab.tmp"));

    // 지울 때 이미 사라졌으면(없음) 지운 것으로 센다.
    REQUIRE(scene.files.AddFile(deleted[0], Span<const uint8>(one, 1)));
    scene.files.faults.remove = MemoryMissing;
    removed = 0;
    REQUIRE(DeleteTemporaryOrphans(scene.files, scene.layout, scene.lock, removed, failures));
    scene.files.faults.remove = {};
    CHECK(removed == 1 && failures.IsEmpty());
    scene.files.Remove(deleted[0]);

    // 지울 수 없음·열거할 수 없는 하위 디렉터리.
    REQUIRE(scene.files.AddFile(deleted[0], Span<const uint8>(one, 1)));
    REQUIRE(scene.files.AddDirectory("Content/Locked", false, MemoryFault::Enumerate));
    scene.files.faults.remove = MemoryBusy;
    removed = 0;
    REQUIRE(DeleteTemporaryOrphans(scene.files, scene.layout, scene.lock, removed, failures));
    scene.files.faults.remove = {};
    CHECK(removed == 0 && failures.Size() == 2);
    bool sawFile = false;
    bool sawDirectory = false;
    for (const TemporaryFileFailure& failure : failures) {
        sawFile =
            sawFile || (failure.path.View() == deleted[0] && failure.error == ErrorCode::Busy);
        sawDirectory = sawDirectory || failure.path.View() == "Content/Locked";
    }

    CHECK(sawFile && sawDirectory);

    // 잠금 아님·Content가 파일·없는 디렉터리.
    AssetDatabaseLock idle;
    CHECK(DeleteTemporaryOrphans(scene.files, scene.layout, idle, removed, failures).Error() ==
          ErrorCode::InvalidArgument);
    {
        CacheScene bare(env, sources, "cache-orphans-bare");
        REQUIRE(bare.ready);
        bare.files.Remove("Intermediate");
        usize none = 0;
        Array<TemporaryFileFailure> nothing(env.allocator);
        CHECK(DeleteTemporaryOrphans(bare.files, bare.layout, bare.lock, none, nothing));
        CHECK(none == 0 && nothing.IsEmpty());
    }

    {
        TempProject root("cache-orphans-file");
        ProjectLayout layout(env.allocator);
        MemoryFileSystem files(env.allocator, CacheScene::Initialized(layout, root));
        AssetDatabaseLock lock;
        REQUIRE(root.made && !layout.Root().IsEmpty() && files.Ready() &&
                files.AddFile("Content", Span<const uint8>(one, 1)) && lock.Acquire(layout, 0));
        usize none = 0;
        Array<TemporaryFileFailure> nothing(env.allocator);
        CHECK(!DeleteTemporaryOrphans(files, layout, lock, none, nothing));
    }

    // 할당 실패(실패 기록이 생기는 장면 — 지울 수 없는 파일과 열거할 수 없는 디렉터리, 걷기 목록이
    // 늘어나는 하위 디렉터리 여럿).
    const StringView extraDirectories[] = {"Content/D0", "Content/D1", "Content/D2", "Content/D3",
                                           "Content/D4", "Content/D5", "Content/D6", "Content/D7"};
    for (StringView directory : extraDirectories) {
        REQUIRE(scene.files.AddDirectory(directory));
    }

    test::NFailAllocator failing(env.allocator);
    uint32 failuresSeen = 0;
    scene.files.faults.remove = MemoryBusy;
    for (uint64 attempt = 1;; ++attempt) {
        // 시도마다 같은 상태(고아 하나)에서 시작한다(중복 노드를 만들지 않는다).
        if (!scene.files.Has(deleted[0])) {
            REQUIRE(scene.files.AddFile(deleted[0], Span<const uint8>(one, 1)));
        }
        Array<TemporaryFileFailure> failed(failing);
        usize count = 0;
        failing.FailOnAttempt(attempt);
        auto cleaned = DeleteTemporaryOrphans(scene.files, scene.layout, scene.lock, count, failed);
        const bool triggered = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!triggered) {
            CHECK(cleaned && count == 0 && failed.Size() == 2);
            break;
        }

        CHECK(!cleaned && cleaned.Error() == ErrorCode::OutOfMemory);
        ++failuresSeen;
    }

    scene.files.faults.remove = {};

    CHECK(failuresSeen > 0);
}

// 서비스: 캐시를 읽는다(바꾼 캐시 값이 보인다 — 재사용), useCache가 거짓이면 읽지 않는다,
// FlushCache는 Initialize 전 InvalidArgument·잠금 아님 InvalidArgument·성공하면 다시 읽힌다(캐시
// 파일 시스템을 따로 주어도 같다). 캐시를 읽는 Initialize의 할당 실패는 OutOfMemory다.
TEST(AssetIndexCache_ServiceUsesCache) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    CacheScene scene(env, sources, "cache-service");
    REQUIRE(scene.ready);
    const PackageReadOptions options = ReadOptions();
    AssetIndex plain(env.allocator, scene.layout);
    REQUIRE(plain.Rebuild(scene.files, options));
    Array<uint8> bytes(env.allocator);
    REQUIRE(EncodeAssetIndexCache(plain, MakeAssetIndexCacheKey(options), bytes));
    REQUIRE(Patch(bytes, "Champion", "Champixn"));
    Resign(bytes);
    REQUIRE(scene.files.AddFile("Intermediate/AssetIndex.cache", bytes.View()));
    AssetTypeHookTable hooks(env.allocator, env.types);
    AssetServiceConfig config;
    config.packages.types = &env.types;
    config.packages.files = &scene.files;
    config.packages.hooks = &hooks;
    config.packages.read = options;
    config.packages.registryContext = ContextId{0x43414348ull, 1};
    config.packages.registryOptions.names = &env.names;
    auto tagged = [&](AssetService& service, StringView value) noexcept {
        Array<const AssetIndexEntry*> found(env.allocator);
        return service.Index().FindByTag("Kind", value, found) && found.Size() == 1;
    };
    {
        AssetService service(env.allocator, scene.layout, config);
        CHECK(service.FlushCache(scene.lock).Error() == ErrorCode::InvalidArgument);
        REQUIRE(service.Initialize());
        CHECK(tagged(service, "Champixn"));
        AssetDatabaseLock idle;
        CHECK(service.FlushCache(idle).Error() == ErrorCode::InvalidArgument);
    }

    config.useCache = false;
    {
        AssetService service(env.allocator, scene.layout, config);
        REQUIRE(service.Initialize());
        CHECK(tagged(service, "Champion"));
        REQUIRE(service.FlushCache(scene.lock));
    }

    config.useCache = true;
    config.cacheFiles = &scene.files;
    AssetService again(env.allocator, scene.layout, config);
    REQUIRE(again.Initialize());
    CHECK(tagged(again, "Champion"));

    // 캐시를 읽는 Initialize의 할당 실패.
    test::NFailAllocator failing(env.allocator);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        AssetService attempted(failing, scene.layout, config);
        failing.FailOnAttempt(attempt);
        auto initialized = attempted.Initialize();
        const bool triggered = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!triggered) {
            REQUIRE(initialized);
            break;
        }

        CHECK(!initialized && initialized.Error() == ErrorCode::OutOfMemory);
        ++failures;
    }

    CHECK(failures > 0);
}

} // namespace asset_index_cache_tests_file
