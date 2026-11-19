#include "AssetTestSupport.h"

// 패키지 포맷 v1 시험이다(Docs/Design/AssetSystem.ko.md 5절, 14절 시나리오 11·12): 쿠킹·에디터
// 왕복, EditorOnly 제외, bulk(None·Zstd)·재번호, 결정적 byte·지문, 손상·버전 판정, SCC 한 트랜잭션
// 로드, 의존 누락·타입 불일치, Dependencies 대조, Required 저장 거부.
using namespace rocks;
using namespace rocks::asset_test;

namespace {

// 시험 장면이다. 패키지 A(id 1) → B(id 2) Hard, C(id 3) Soft, D(id 4) EditorOnly Hard.
struct Scene {
    explicit Scene(Allocator& allocator) noexcept
        : pixels(allocator), partData(allocator), sourceData(allocator) {
    }

    RootRef<Package> a;
    RootRef<Package> b;
    RootRef<Package> d;
    ObjectHandle aAsset{};
    ObjectHandle body{};
    ObjectHandle socket{};
    ObjectHandle bAsset{};
    ObjectHandle gear{};
    ObjectHandle dAsset{};
    Array<uint8> pixels;
    Array<uint8> partData;
    Array<uint8> sourceData;
    TestBulkSource bulk;
};

void Fill(Array<uint8>& bytes, usize count, uint8 seed) noexcept {
    (void)bytes.TryResize(count);
    for (usize index = 0; index < count; ++index) {
        // 압축이 되도록 반복이 많은 무늬다.
        bytes[index] = static_cast<uint8>(seed + (index / 64) % 7);
    }
}

bool BuildScene(ObjectRegistry& registry, Scene& scene) noexcept {
    const ObjectAccessContext access = registry.OwnerAccess();
    auto a = NewPackage(registry, access, FixedId(1), PackageKind::Editor);
    auto b = NewPackage(registry, access, FixedId(2), PackageKind::Editor);
    auto d = NewPackage(registry, access, FixedId(4), PackageKind::Editor);
    if (!a || !b || !d) {
        return false;
    }

    scene.a = Move(a.Value());
    scene.b = Move(b.Value());
    scene.d = Move(d.Value());
    scene.bAsset = Make<TestAsset>(registry, TestAssetType(), scene.b.Handle(), "");
    scene.gear = Make<TestPart>(registry, TestPartType(), scene.bAsset, "Gear");
    scene.dAsset = Make<TestAsset>(registry, TestAssetType(), scene.d.Handle(), "");
    scene.aAsset = Make<TestAsset>(registry, TestAssetType(), scene.a.Handle(), "");
    scene.body = Make<TestPart>(registry, TestPartType(), scene.aAsset, "Body");
    scene.socket = Make<TestPart>(registry, TestPartType(), scene.body, "Socket");
    const ObjectHandle temp = Make<TestScratch>(registry, TestScratchType(), scene.aAsset, "Temp");
    // 저장 불가 객체 아래의 저장 가능한 자식도 부분 트리째 빠진다(writer export 집합).
    const ObjectHandle tempChild = Make<TestPart>(registry, TestPartType(), temp, "Inner");
    if (scene.bAsset.IsNull() || scene.gear.IsNull() || scene.dAsset.IsNull() ||
        scene.aAsset.IsNull() || scene.body.IsNull() || scene.socket.IsNull() || temp.IsNull() ||
        tempChild.IsNull()) {
        return false;
    }

    Fill(scene.pixels, 5000, 3);
    Fill(scene.partData, 300, 9);
    Fill(scene.sourceData, 1000, 40);
    TestAsset* asset = Pointer<TestAsset>(registry, scene.aAsset);
    TestAsset* other = Pointer<TestAsset>(registry, scene.bAsset);
    TestPart* body = Pointer<TestPart>(registry, scene.body);
    TestPart* socket = Pointer<TestPart>(registry, scene.socket);
    if (asset == nullptr || other == nullptr || body == nullptr || socket == nullptr) {
        return false;
    }

    other->value = 20;
    asset->value = 10;
    body->index = 1;
    socket->index = 2;
    // 옛 bulkId는 일부러 연속이 아니다(writer가 1부터 다시 매긴다).
    asset->pixels = scene.bulk.Add(5, scene.pixels.View());
    asset->source = scene.bulk.Add(9, scene.sourceData.View());
    body->data = scene.bulk.Add(2, scene.partData.View());
    bool ok = asset->label.TryAssign("alpha").IsOk() &&
              asset->note.TryAssign("editor note").IsOk() &&
              asset->soft.TryAssign(FixedId(3), StringView()).IsOk();
    ok = ok && registry.SetReference(access, scene.aAsset, asset->hard, scene.bAsset).IsOk();
    ok = ok && registry.SetReference(access, scene.aAsset, asset->editorRef, scene.dAsset).IsOk();
    ok =
        ok && WithBorrow<TestAsset>(registry, scene.aAsset, [&](TestAsset& owner) {
            return registry.AddReference(access, scene.aAsset, owner.hardList, scene.gear).IsOk() &&
                   registry.AddReference(access, scene.aAsset, owner.hardList, ObjectHandle{})
                       .IsOk() &&
                   registry.AddReference(access, scene.aAsset, owner.hardList, scene.body).IsOk();
        });
    ok = ok && registry.SetReference(access, scene.body, body->sibling, scene.socket).IsOk();
    ok = ok && registry.SetReference(access, scene.socket, socket->sibling, scene.body).IsOk();
    return ok;
}

PackageWriteOptions WithBulk(PackageWriteOptions options, PackageBulkSource& source,
                             BulkCodec codec = BulkCodec::None) noexcept {
    options.bulkSource = &source;
    options.bulkCodec = codec;
    return options;
}

bool SpanEquals(Span<const uint8> bytes, const Array<uint8>& expected) noexcept {
    return SameBytes(bytes, expected.View());
}

// 쿠킹 B를 target에 로드하고 resolver에 넣는다.
bool LoadDependency(AssetEnv& env, Array<uint8>& bytes, RootRef<Object>& root, ObjectHandle& handle,
                    TestResolver& resolver, AssetId id) noexcept {
    Array<uint8> files[1] = {Move(bytes)};
    RootRef<Object> roots[1];
    ObjectHandle handles[1];
    if (!LoadAll(env, *env.target, files, PackageKind::Cooked, &resolver, roots, handles)) {
        return false;
    }

    root = Move(roots[0]);
    handle = handles[0];
    resolver.Add(id, handle);
    return true;
}

} // namespace

// 쿠킹 왕복: 값·Hard·Soft·하위 객체·bulk(Zstd)가 돌아오고 EditorOnly 필드·저장 불가 객체는 빠지며,
// 다시 쓰면 같은 byte다(AS 14절 11).
TEST(AssetPackage_CookedRoundTrip) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Scene scene(env.allocator);
    REQUIRE(BuildScene(*env.source, scene));
    Array<uint8> aBytes = Write(env.allocator, *env.source, scene.a.Handle(),
                                WithBulk(CookedOptions(), scene.bulk, BulkCodec::Zstd));
    Array<uint8> bBytes = Write(env.allocator, *env.source, scene.b.Handle(),
                                WithBulk(CookedOptions(), scene.bulk, BulkCodec::Zstd));
    REQUIRE(!aBytes.IsEmpty() && !bBytes.IsEmpty());
    const Array<uint8> aOriginal = Copy(env.allocator, aBytes.View());

    // 헤더·표 검증과 내용 해석.
    PackageReadOptions read;
    read.types = &env.types;
    auto contents = ReadPackage(env.allocator, aOriginal.View(), read);
    REQUIRE(contents);
    CHECK(contents.Value().header.assetId == FixedId(1));
    CHECK(contents.Value().header.kind == PackageKind::Cooked);
    CHECK(contents.Value().header.bulkAlignment == PackageDefaultCookedBulkAlignment);
    CHECK(contents.Value().summary.profileId.View() == StringView("Windows"));
    CHECK(contents.Value().summary.cookerVersion == 7);
    // export: 대표 에셋, Body, Body.Socket(Temp는 저장 불가라 그 자식 Temp.Inner와 함께 없다).
    REQUIRE(contents.Value().exports.Size() == 3);
    CHECK(contents.Value().exports[1].name == StringView("Body"));
    CHECK(contents.Value().exports[2].name == StringView("Socket"));
    CHECK(contents.Value().exports[2].outerIndex == 2);
    // import: B의 대표 에셋("")과 Gear. 쿠킹은 EditorOnly 참조(D)를 쓰지 않는다.
    REQUIRE(contents.Value().imports.Size() == 2);
    CHECK(contents.Value().imports[0].asset == FixedId(2));
    CHECK(contents.Value().imports[0].name == StringView(""));
    CHECK(contents.Value().imports[1].name == StringView("Gear"));
    // 의존: (B, Hard, TestAsset), (C, Soft).
    REQUIRE(contents.Value().dependencies.Size() == 2);
    CHECK(contents.Value().dependencies[0].asset == FixedId(2));
    CHECK(contents.Value().dependencies[0].kind == PackageDependencyKind::Hard);
    CHECK(contents.Value().dependencies[0].expectedType == TestAssetType());
    CHECK(contents.Value().dependencies[1].asset == FixedId(3));
    CHECK(contents.Value().dependencies[1].kind == PackageDependencyKind::Soft);
    // bulk: pixels·Body.data 두 entry(EditorOnly source 제외), Zstd로 줄었다.
    REQUIRE(contents.Value().bulkEntries.Size() == 2);
    CHECK(contents.Value().bulkEntries[0].codec == BulkCodec::Zstd);
    CHECK(contents.Value().bulkEntries[0].rawBytes == 5000);
    CHECK(contents.Value().bulkEntries[1].rawBytes == 300);
    CHECK(contents.Value().bulkEntries[1].offset % PackageDefaultCookedBulkAlignment == 0);

    TestResolver resolver;
    RootRef<Object> bRoot;
    ObjectHandle bPackage{};
    REQUIRE(LoadDependency(env, bBytes, bRoot, bPackage, resolver, FixedId(2)));
    Array<uint8> files[1] = {Move(aBytes)};
    RootRef<Object> roots[1];
    ObjectHandle handles[1];
    AssetDiagnostic failure;
    REQUIRE(
        LoadAll(env, *env.target, files, PackageKind::Cooked, &resolver, roots, handles, &failure));

    ObjectRegistry& target = *env.target;
    const ObjectAccessContext access = target.OwnerAccess();
    Package* package = Pointer<Package>(target, handles[0]);
    REQUIRE(package != nullptr);
    CHECK(package->Id() == FixedId(1));
    CHECK(package->Kind() == PackageKind::Cooked);
    auto assetHandle = target.FindObject(access, handles[0], StringView());
    REQUIRE(assetHandle);
    TestAsset* asset = Pointer<TestAsset>(target, assetHandle.Value());
    REQUIRE(asset != nullptr);
    CHECK(asset->Id() == FixedId(1));
    CHECK(asset->value == 10);
    CHECK(asset->label.View() == StringView("alpha"));
    CHECK(asset->note.ByteCount() == 0);
    CHECK(asset->source.IsEmpty());
    CHECK(asset->editorRef.Handle().IsNull());
    CHECK(asset->soft.asset == FixedId(3));
    auto bAsset = target.FindObject(access, bPackage, StringView());
    auto gear = target.FindObject(access, bPackage, "Gear");
    auto body = target.FindObject(access, handles[0], "Body");
    auto socket = target.FindObject(access, handles[0], "Body.Socket");
    REQUIRE(bAsset && gear && body && socket);
    CHECK(asset->hard.Handle() == bAsset.Value());
    REQUIRE(asset->hardList.Size() == 3);
    CHECK(asset->hardList[0] == gear.Value());
    CHECK(asset->hardList[1].IsNull());
    CHECK(asset->hardList[2] == body.Value());
    CHECK(!target.FindObject(access, handles[0], "Temp"));
    CHECK(!target.FindObject(access, handles[0], "Temp.Inner"));
    TestPart* bodyPart = Pointer<TestPart>(target, body.Value());
    TestPart* socketPart = Pointer<TestPart>(target, socket.Value());
    REQUIRE(bodyPart != nullptr && socketPart != nullptr);
    CHECK(bodyPart->sibling.Handle() == socket.Value());
    CHECK(socketPart->sibling.Handle() == body.Value());
    CHECK(socketPart->index == 2);
    // bulk는 새 id(1·2)이고 byte가 같다.
    CHECK(asset->pixels.bulkId == 1);
    CHECK(bodyPart->data.bulkId == 2);
    CHECK(package->BulkCount() == 2);
    auto pixels = package->BulkBytes(asset->pixels);
    auto data = package->BulkBytes(bodyPart->data);
    REQUIRE(pixels && data);
    CHECK(SpanEquals(pixels.Value(), scene.pixels));
    CHECK(SpanEquals(data.Value(), scene.partData));
    CHECK(!package->BulkBytes(BulkDataRef{}));
    CHECK(package->BulkBytes(BulkDataRef{3, 1, {}}).Error() == ErrorCode::NotFound);
    CHECK(package->BulkBytes(BulkDataRef{1, 1, {}}).Error() == ErrorCode::TypeMismatch);

    // 로드한 상태를 다시 쓰면 같은 byte다(재번호·정렬·패딩이 결정적).
    PackageBulkReader reader(target, handles[0]);
    Array<uint8> again = Write(env.allocator, target, handles[0],
                               WithBulk(CookedOptions(), reader, BulkCodec::Zstd));
    CHECK(SameBytes(again.View(), aOriginal.View()));
}

// 에디터 왕복: EditorOnly 필드·참조·bulk가 남고 의존에 EditorOnly 종류가 생기며, bulk entry의
// EditorOnly 표시와 엔진 버전이 기록된다.
TEST(AssetPackage_EditorRoundTripKeepsEditorOnly) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Scene scene(env.allocator);
    REQUIRE(BuildScene(*env.source, scene));
    Array<uint8> aBytes =
        Write(env.allocator, *env.source, scene.a.Handle(), WithBulk(EditorOptions(), scene.bulk));
    Array<uint8> bBytes =
        Write(env.allocator, *env.source, scene.b.Handle(), WithBulk(EditorOptions(), scene.bulk));
    Array<uint8> dBytes =
        Write(env.allocator, *env.source, scene.d.Handle(), WithBulk(EditorOptions(), scene.bulk));
    REQUIRE(!aBytes.IsEmpty() && !bBytes.IsEmpty() && !dBytes.IsEmpty());

    PackageReadOptions read;
    read.expectedKind = PackageKind::Editor;
    read.types = &env.types;
    read.engineVersion = TestEngineVersion;
    auto contents = ReadPackage(env.allocator, aBytes.View(), read);
    REQUIRE(contents);
    CHECK(contents.Value().header.bulkAlignment == PackageEditorBulkAlignment);
    CHECK(contents.Value().summary.savedBy == TestEngineVersion);
    CHECK(contents.Value().summary.compatibleWith == (EngineVersion{1, 2, 0}));
    CHECK(!contents.Value().readOnly);
    // 의존: (B Hard), (C Soft), (D EditorOnly).
    REQUIRE(contents.Value().dependencies.Size() == 3);
    CHECK(contents.Value().dependencies[2].asset == FixedId(4));
    CHECK(contents.Value().dependencies[2].kind == PackageDependencyKind::EditorOnly);
    // bulk: pixels(1), source(2, EditorOnly), Body.data(3).
    REQUIRE(contents.Value().bulkEntries.Size() == 3);
    CHECK(contents.Value().bulkEntries[0].flags == 0);
    CHECK(contents.Value().bulkEntries[1].flags == static_cast<uint32>(BulkEntryFlags::EditorOnly));
    CHECK(contents.Value().bulkEntries[1].rawBytes == 1000);

    // 더 새 엔진이 저장한 패키지(compatibleWith 주·부가 더 새다)는 NewerVersion이다.
    read.engineVersion = EngineVersion{1, 1, 9};
    AssetDiagnostic newer;
    CHECK(!ReadPackage(env.allocator, aBytes.View(), read, &newer));
    CHECK(newer.code == AssetDiagnosticCode::NewerVersion);

    Array<uint8> files[3] = {Move(aBytes), Move(bBytes), Move(dBytes)};
    RootRef<Object> roots[3];
    ObjectHandle handles[3];
    REQUIRE(LoadAll(env, *env.target, files, PackageKind::Editor, nullptr, roots, handles));
    ObjectRegistry& target = *env.target;
    auto assetHandle = target.FindObject(target.OwnerAccess(), handles[0], StringView());
    auto dAsset = target.FindObject(target.OwnerAccess(), handles[2], StringView());
    REQUIRE(assetHandle && dAsset);
    TestAsset* asset = Pointer<TestAsset>(target, assetHandle.Value());
    REQUIRE(asset != nullptr);
    CHECK(asset->note.View() == StringView("editor note"));
    CHECK(asset->editorRef.Handle() == dAsset.Value());
    CHECK(asset->source.bulkId == 2);
    Package* package = Pointer<Package>(target, handles[0]);
    REQUIRE(package != nullptr);
    CHECK(package->Kind() == PackageKind::Editor);
    auto source = package->BulkBytes(asset->source);
    REQUIRE(source);
    CHECK(SpanEquals(source.Value(), scene.sourceData));

    // 쿠킹을 기대하는 reader는 에디터 패키지를 UnsupportedVersion(다시 쿠킹)으로 거부한다.
    Array<uint8> editor =
        Write(env.allocator, *env.source, scene.b.Handle(), WithBulk(EditorOptions(), scene.bulk));
    PackageReadOptions cooked;
    AssetDiagnostic wrongKind;
    CHECK(!ReadPackage(env.allocator, editor.View(), cooked, &wrongKind));
    CHECK(wrongKind.code == AssetDiagnosticCode::UnsupportedVersion);
}

// 같은 상태를 두 번 쓰면 같은 byte이고, writer의 지문은 reader의 지문과 같다.
TEST(AssetPackage_DeterministicBytesAndFingerprint) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Scene scene(env.allocator);
    REQUIRE(BuildScene(*env.source, scene));
    const BulkCodec codecs[] = {BulkCodec::None, BulkCodec::Zstd};
    for (BulkCodec codec : codecs) {
        MemoryPackageSink first(env.allocator);
        MemoryPackageSink second(env.allocator);
        const PackageWriteOptions options = WithBulk(CookedOptions(), scene.bulk, codec);
        auto one = WritePackage(env.allocator, *env.source, env.source->OwnerAccess(),
                                scene.a.Handle(), options, first);
        auto two = WritePackage(env.allocator, *env.source, env.source->OwnerAccess(),
                                scene.a.Handle(), options, second);
        REQUIRE(one && two);
        CHECK(SameBytes(first.Bytes(), second.Bytes()));
        CHECK(one.Value().fileBytes == first.Bytes().Size());
        REQUIRE(one.Value().bulkRemap.Size() == 2);
        CHECK(one.Value().bulkRemap[0].oldId == 5);
        CHECK(one.Value().bulkRemap[0].newId == 1);
        CHECK(one.Value().bulkRemap[1].oldId == 2);
        CHECK(one.Value().bulkRemap[1].newId == 2);
        auto contents = ReadPackage(env.allocator, first.Bytes(), PackageReadOptions{});
        REQUIRE(contents);
        CHECK(contents.Value().fingerprint == one.Value().fingerprint);
        CHECK(contents.Value().bulkEntries[0].codec == codec);
    }
}

// 손상 판정(AS 5.1절 검증 순서, 14절 12): magic·headerCrc·flags·formatVersion·fileBytes·표 CRC·섹션
// CRC·패딩·섹션 순서·겹침·bulk entry CRC·Exports 내용.
TEST(AssetPackage_CorruptionAndVersionChecks) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Scene scene(env.allocator);
    REQUIRE(BuildScene(*env.source, scene));
    const Array<uint8> valid =
        Write(env.allocator, *env.source, scene.a.Handle(), WithBulk(CookedOptions(), scene.bulk));
    REQUIRE(!valid.IsEmpty());
    REQUIRE(ReadPackage(env.allocator, valid.View(), PackageReadOptions{}));

    auto expect = [&](Array<uint8>& bytes, AssetDiagnosticCode code, ErrorCode error,
                      PackageKind kind = PackageKind::Cooked) {
        PackageReadOptions options;
        options.expectedKind = kind;
        AssetDiagnostic failure;
        auto read = ReadPackage(env.allocator, bytes.View(), options, &failure);
        CHECK(!read);
        CHECK(failure.code == code);
        CHECK(!read && read.Error() == error);
    };

    {
        Array<uint8> bytes = Copy(env.allocator, valid.View());
        bytes[0] = 'X';
        expect(bytes, AssetDiagnosticCode::CorruptPackage, ErrorCode::InvalidArgument);
    }

    {
        Array<uint8> bytes = Copy(env.allocator, valid.View());
        bytes[33] ^= 1; // assetId — headerCrc가 잡는다
        expect(bytes, AssetDiagnosticCode::CorruptPackage, ErrorCode::InvalidArgument);
    }

    {
        Array<uint8> bytes = Copy(env.allocator, valid.View());
        StoreLittleEndian<uint32>(bytes.Data() + 8, 3); // Editor | Cooked
        FixHeaderCrc(bytes);
        expect(bytes, AssetDiagnosticCode::CorruptPackage, ErrorCode::InvalidArgument);
    }

    {
        // 더 새 formatVersion: 쿠킹 UnsupportedVersion, 에디터 NewerVersion.
        Array<uint8> bytes = Copy(env.allocator, valid.View());
        StoreLittleEndian<uint16>(bytes.Data() + 4, PackageFormatVersion + 1);
        FixHeaderCrc(bytes);
        expect(bytes, AssetDiagnosticCode::UnsupportedVersion, ErrorCode::Unsupported);
        StoreLittleEndian<uint32>(bytes.Data() + 8, static_cast<uint32>(PackageKind::Editor));
        FixHeaderCrc(bytes);
        expect(bytes, AssetDiagnosticCode::NewerVersion, ErrorCode::Unsupported,
               PackageKind::Editor);
        StoreLittleEndian<uint16>(bytes.Data() + 4, 0);
        FixHeaderCrc(bytes);
        expect(bytes, AssetDiagnosticCode::UnsupportedVersion, ErrorCode::Unsupported,
               PackageKind::Editor);
    }

    {
        // 잘린 파일(Stat 크기 ≠ fileBytes).
        Array<uint8> bytes = Copy(env.allocator, valid.View());
        (void)bytes.TryResize(bytes.Size() - 1);
        expect(bytes, AssetDiagnosticCode::CorruptPackage, ErrorCode::InvalidArgument);
    }

    {
        // 섹션 표 CRC.
        Array<uint8> bytes = Copy(env.allocator, valid.View());
        bytes[bytes.Size() - 1] ^= 0x10;
        expect(bytes, AssetDiagnosticCode::CorruptPackage, ErrorCode::InvalidArgument);
    }

    {
        // Exports 섹션 byte 하나(섹션 CRC).
        Array<uint8> bytes = Copy(env.allocator, valid.View());
        usize row = 0;
        uint64 offset = 0;
        uint64 size = 0;
        REQUIRE(FindSection(bytes.View(), PackageSectionKind::Exports, row, offset, size));
        bytes[static_cast<usize>(offset)] ^= 1;
        expect(bytes, AssetDiagnosticCode::CorruptPackage, ErrorCode::InvalidArgument);
    }

    {
        // 패딩(Bulk 섹션 앞 정렬 공간)의 0이 아닌 byte — CRC 밖이지만 거부한다.
        Array<uint8> bytes = Copy(env.allocator, valid.View());
        usize row = 0;
        uint64 offset = 0;
        uint64 size = 0;
        REQUIRE(FindSection(bytes.View(), PackageSectionKind::Bulk, row, offset, size));
        bytes[static_cast<usize>(offset) - 1] = 0xAA;
        expect(bytes, AssetDiagnosticCode::CorruptPackage, ErrorCode::InvalidArgument);
    }

    {
        // 섹션 순서: Summary와 Exports의 kind를 바꾼다(표 CRC는 다시 맞춘다).
        Array<uint8> bytes = Copy(env.allocator, valid.View());
        const usize table = static_cast<usize>(LoadLittleEndian<uint64>(bytes.Data() + 16));
        StoreLittleEndian<uint32>(bytes.Data() + table, 2);
        StoreLittleEndian<uint32>(bytes.Data() + table + PackageSectionEntryBytes, 1);
        FixTableCrc(bytes);
        expect(bytes, AssetDiagnosticCode::CorruptPackage, ErrorCode::InvalidArgument);
    }

    {
        // 겹침: Exports 크기를 다음 섹션까지 늘린다.
        Array<uint8> bytes = Copy(env.allocator, valid.View());
        usize row = 0;
        uint64 offset = 0;
        uint64 size = 0;
        REQUIRE(FindSection(bytes.View(), PackageSectionKind::Exports, row, offset, size));
        StoreLittleEndian<uint64>(bytes.Data() + row + 16, size + 64);
        FixTableCrc(bytes);
        expect(bytes, AssetDiagnosticCode::CorruptPackage, ErrorCode::InvalidArgument);
    }

    {
        // bulk entry 저장 byte(entry CRC).
        Array<uint8> bytes = Copy(env.allocator, valid.View());
        usize row = 0;
        uint64 offset = 0;
        uint64 size = 0;
        REQUIRE(FindSection(bytes.View(), PackageSectionKind::Bulk, row, offset, size));
        bytes[static_cast<usize>(offset) + 10] ^= 0xFF;
        expect(bytes, AssetDiagnosticCode::CorruptPackage, ErrorCode::InvalidArgument);
    }

    {
        // Exports 내용(섹션 CRC는 맞춤): export 1의 Primary 표시를 지운다.
        Array<uint8> bytes = Copy(env.allocator, valid.View());
        usize row = 0;
        uint64 offset = 0;
        uint64 size = 0;
        REQUIRE(FindSection(bytes.View(), PackageSectionKind::Exports, row, offset, size));
        // count(4) + name(u64 길이 0) + type(16) + outer(4) → flags.
        const usize flags = static_cast<usize>(offset) + 4 + 8 + 16 + 4;
        CHECK(LoadLittleEndian<uint32>(bytes.Data() + flags) == 1);
        StoreLittleEndian<uint32>(bytes.Data() + flags, 0);
        FixSectionCrc(bytes, PackageSectionKind::Exports);
        expect(bytes, AssetDiagnosticCode::CorruptPackage, ErrorCode::InvalidArgument);
        // 정의되지 않은(v1 예약) EditorOnly 표시도 거부한다.
        StoreLittleEndian<uint32>(bytes.Data() + flags, 1 | 2);
        FixSectionCrc(bytes, PackageSectionKind::Exports);
        expect(bytes, AssetDiagnosticCode::CorruptPackage, ErrorCode::InvalidArgument);
    }

    {
        // 쿠킹 커스텀 버전 불일치는 UnsupportedVersion, 에디터의 더 새 커스텀 버전은 읽기 전용이다.
        const PackageCustomVersion stored[] = {{Guid{1, 2}, 3}};
        PackageWriteOptions options = WithBulk(CookedOptions(), scene.bulk);
        options.customVersions = stored;
        Array<uint8> bytes = Write(env.allocator, *env.source, scene.a.Handle(), options);
        REQUIRE(!bytes.IsEmpty());
        expect(bytes, AssetDiagnosticCode::UnsupportedVersion, ErrorCode::Unsupported);
        PackageReadOptions same;
        same.customVersions = stored;
        CHECK(ReadPackage(env.allocator, bytes.View(), same));

        PackageWriteOptions editorOptions = WithBulk(EditorOptions(), scene.bulk);
        editorOptions.customVersions = stored;
        Array<uint8> editor = Write(env.allocator, *env.source, scene.a.Handle(), editorOptions);
        PackageReadOptions older;
        older.expectedKind = PackageKind::Editor;
        const PackageCustomVersion current[] = {{Guid{1, 2}, 2}};
        older.customVersions = current;
        auto opened = ReadPackage(env.allocator, editor.View(), older);
        REQUIRE(opened);
        CHECK(opened.Value().readOnly);
    }
}

// 하드 순환 A ↔ B를 한 트랜잭션(allocate-all → fixup-all → publish)으로 로드한다(AS 11.3절).
TEST(AssetPackage_HardCycleLoadsInOneTransaction) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ObjectRegistry& source = *env.source;
    const ObjectAccessContext access = source.OwnerAccess();
    auto a = NewPackage(source, access, FixedId(11), PackageKind::Cooked);
    auto b = NewPackage(source, access, FixedId(12), PackageKind::Cooked);
    REQUIRE(a && b);
    const ObjectHandle aAsset = Make<TestAsset>(source, TestAssetType(), a.Value().Handle(), "");
    const ObjectHandle bAsset = Make<TestAsset>(source, TestAssetType(), b.Value().Handle(), "");
    REQUIRE(!aAsset.IsNull() && !bAsset.IsNull());
    REQUIRE(source.SetReference(access, aAsset, Pointer<TestAsset>(source, aAsset)->hard, bAsset));
    REQUIRE(source.SetReference(access, bAsset, Pointer<TestAsset>(source, bAsset)->hard, aAsset));
    Pointer<TestAsset>(source, aAsset)->value = 1;
    Pointer<TestAsset>(source, bAsset)->value = 2;
    Array<uint8> files[2] = {Write(env.allocator, source, a.Value().Handle(), CookedOptions()),
                             Write(env.allocator, source, b.Value().Handle(), CookedOptions())};
    REQUIRE(!files[0].IsEmpty() && !files[1].IsEmpty());

    // 하나만 올리면 상대가 없어 MissingDependency다.
    {
        Array<uint8> single[1] = {Copy(env.allocator, files[0].View())};
        RootRef<Object> roots[1];
        ObjectHandle handles[1];
        AssetDiagnostic failure;
        CHECK(!LoadAll(env, *env.target, single, PackageKind::Cooked, nullptr, roots, handles,
                       &failure));
        CHECK(failure.code == AssetDiagnosticCode::MissingDependency);
        CHECK(failure.related == FixedId(12));
        CHECK(failure.error == ErrorCode::NotFound);
        CHECK(env.target->Stats().liveObjects == 0);
    }

    RootRef<Object> roots[2];
    ObjectHandle handles[2];
    REQUIRE(LoadAll(env, *env.target, files, PackageKind::Cooked, nullptr, roots, handles));
    ObjectRegistry& target = *env.target;
    auto loadedA = target.FindObject(target.OwnerAccess(), handles[0], StringView());
    auto loadedB = target.FindObject(target.OwnerAccess(), handles[1], StringView());
    REQUIRE(loadedA && loadedB);
    CHECK(Pointer<TestAsset>(target, loadedA.Value())->hard.Handle() == loadedB.Value());
    CHECK(Pointer<TestAsset>(target, loadedB.Value())->hard.Handle() == loadedA.Value());
    CHECK(Pointer<TestAsset>(target, loadedB.Value())->value == 2);
}

// import 대상의 실제 타입이 선언 타입의 파생이 아니면 TypeMismatch다. Dependencies 내용이 디코딩
// 결과와 다르면 CorruptPackage다.
TEST(AssetPackage_ImportTypeMismatchAndDependencyCrossCheck) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ObjectRegistry& source = *env.source;
    const ObjectAccessContext access = source.OwnerAccess();
    auto a = NewPackage(source, access, FixedId(21), PackageKind::Cooked);
    auto b = NewPackage(source, access, FixedId(22), PackageKind::Cooked);
    REQUIRE(a && b);
    const ObjectHandle aAsset = Make<TestAsset>(source, TestAssetType(), a.Value().Handle(), "");
    const ObjectHandle bAsset = Make<TestAsset>(source, TestAssetType(), b.Value().Handle(), "");
    REQUIRE(source.SetReference(access, aAsset, Pointer<TestAsset>(source, aAsset)->hard, bAsset));
    const Array<uint8> aBytes = Write(env.allocator, source, a.Value().Handle(), CookedOptions());
    REQUIRE(!aBytes.IsEmpty());

    // 같은 id(22)로 다른 타입의 대표 에셋(TestRequiredAsset — TestAsset 파생 아님)을 만든다.
    auto other = NewPackage(source, access, FixedId(22), PackageKind::Cooked);
    REQUIRE(other);
    const ObjectHandle wrong =
        Make<TestRequiredAsset>(source, TestRequiredType(), other.Value().Handle(), "");
    REQUIRE(source.SetReference(access, wrong, Pointer<TestRequiredAsset>(source, wrong)->target,
                                aAsset));
    Array<uint8> wrongBytes = Write(env.allocator, source, other.Value().Handle(), CookedOptions());
    REQUIRE(!wrongBytes.IsEmpty());
    {
        // B'는 A를 하드 참조하므로 둘을 같은 묶음으로 올린다.
        Array<uint8> files[2] = {Copy(env.allocator, aBytes.View()), Move(wrongBytes)};
        RootRef<Object> roots[2];
        ObjectHandle handles[2];
        AssetDiagnostic failure;
        CHECK(!LoadAll(env, *env.target, files, PackageKind::Cooked, nullptr, roots, handles,
                       &failure));
        CHECK(failure.code == AssetDiagnosticCode::TypeMismatch);
        CHECK(failure.asset == FixedId(21));
        CHECK(failure.related == FixedId(22));
    }

    {
        // Dependencies의 required 표시를 바꾸면(섹션 CRC는 맞춤) 디코딩 결과와 달라
        // CorruptPackage다.
        Array<uint8> bytes = Copy(env.allocator, aBytes.View());
        usize row = 0;
        uint64 offset = 0;
        uint64 size = 0;
        REQUIRE(FindSection(bytes.View(), PackageSectionKind::Dependencies, row, offset, size));
        // count(4) + asset(16) + kind(4) + expected(16) → flags.
        const usize flags = static_cast<usize>(offset) + 4 + 16 + 4 + 16;
        StoreLittleEndian<uint32>(bytes.Data() + flags, 1);
        FixSectionCrc(bytes, PackageSectionKind::Dependencies);
        PackageLoadState state(env.allocator, *env.target, env.types, env.target->OwnerAccess(),
                               LoadOptions(PackageKind::Cooked));
        AssetDiagnostic failure;
        CHECK(!state.Prepare(Move(bytes), &failure));
        CHECK(failure.code == AssetDiagnosticCode::CorruptPackage);
        CHECK(failure.section == static_cast<uint32>(PackageSectionKind::Dependencies));
        // Prepare는 한 번만 한다.
        CHECK(state.Prepare(Copy(env.allocator, aBytes.View())).Error() ==
              ErrorCode::InvalidArgument);
    }
}

// 쓰기 거부: Required null(쿠킹·에디터 저장), bulk 원본 불일치(CorruptPackage), 대표 에셋 없음,
// 잘못된 옵션.
TEST(AssetPackage_WriterRejections) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ObjectRegistry& source = *env.source;
    const ObjectAccessContext access = source.OwnerAccess();
    auto required = NewPackage(source, access, FixedId(31), PackageKind::Cooked);
    REQUIRE(required);
    REQUIRE(!Make<TestRequiredAsset>(source, TestRequiredType(), required.Value().Handle(), "")
                 .IsNull());
    MemoryPackageSink sink(env.allocator);
    CHECK(WritePackage(env.allocator, source, access, required.Value().Handle(), CookedOptions(),
                       sink)
              .Error() == ErrorCode::InvalidArgument);

    // 대표 에셋이 없는 패키지.
    auto empty = NewPackage(source, access, FixedId(32), PackageKind::Cooked);
    REQUIRE(empty);
    CHECK(WritePackage(env.allocator, source, access, empty.Value().Handle(), CookedOptions(), sink)
              .Error() == ErrorCode::InvalidArgument);

    // bulk 원본이 ref와 다른 byte를 주면 CorruptPackage다(손상이 새 CRC로 세탁되지 않게).
    auto bulky = NewPackage(source, access, FixedId(33), PackageKind::Cooked);
    REQUIRE(bulky);
    const ObjectHandle asset = Make<TestAsset>(source, TestAssetType(), bulky.Value().Handle(), "");
    REQUIRE(!asset.IsNull());
    const uint8 good[4] = {1, 2, 3, 4};
    const uint8 bad[4] = {1, 2, 3, 5};
    TestBulkSource real;
    Pointer<TestAsset>(source, asset)->pixels = real.Add(1, Span<const uint8>(good, 4));
    TestBulkSource tampered;
    (void)tampered.Add(1, Span<const uint8>(bad, 4));
    AssetDiagnostic failure;
    MemoryPackageSink out(env.allocator);
    CHECK(!WritePackage(env.allocator, source, access, bulky.Value().Handle(),
                        WithBulk(CookedOptions(), tampered), out, &failure));
    CHECK(failure.code == AssetDiagnosticCode::CorruptPackage);
    // bulk가 있는데 source가 없으면 InvalidArgument다.
    CHECK(WritePackage(env.allocator, source, access, bulky.Value().Handle(), CookedOptions(), out)
              .Error() == ErrorCode::InvalidArgument);

    // 잘못된 옵션: 쿠킹의 빈 프로파일, 에디터의 빈 엔진 버전, 정렬, 쿠킹의 Build 의존.
    PackageWriteOptions options = WithBulk(CookedOptions(), real);
    options.profileId = StringView();
    CHECK(
        WritePackage(env.allocator, source, access, bulky.Value().Handle(), options, out).Error() ==
        ErrorCode::InvalidArgument);
    options = WithBulk(EditorOptions(), real);
    options.engineVersion = EngineVersion{};
    CHECK(
        WritePackage(env.allocator, source, access, bulky.Value().Handle(), options, out).Error() ==
        ErrorCode::InvalidArgument);
    options = WithBulk(CookedOptions(), real);
    options.bulkAlignment = 48;
    CHECK(
        WritePackage(env.allocator, source, access, bulky.Value().Handle(), options, out).Error() ==
        ErrorCode::InvalidArgument);
    const PackageDependencyEntry build[] = {
        {FixedId(40), PackageDependencyKind::Build, TypeId{}, false}};
    options = WithBulk(CookedOptions(), real);
    options.extraDependencies = build;
    CHECK(
        WritePackage(env.allocator, source, access, bulky.Value().Handle(), options, out).Error() ==
        ErrorCode::InvalidArgument);

    // 에디터 패키지의 Build 의존은 기록되고 로드 대조에서 빠진다.
    options = WithBulk(EditorOptions(), real);
    options.extraDependencies = build;
    Array<uint8> editor = Write(env.allocator, source, bulky.Value().Handle(), options);
    REQUIRE(!editor.IsEmpty());
    PackageLoadState state(env.allocator, *env.target, env.types, env.target->OwnerAccess(),
                           LoadOptions(PackageKind::Editor));
    REQUIRE(state.Prepare(Move(editor)));
    REQUIRE(state.Contents().dependencies.Size() == 1);
    CHECK(state.Contents().dependencies[0].kind == PackageDependencyKind::Build);
}
