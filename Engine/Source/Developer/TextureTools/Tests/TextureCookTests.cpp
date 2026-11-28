#include "../../AssetDatabase/Tests/AssetDatabaseTestSupport.h"

#include <AssetNugget/NuggetCache.h>
#include <AssetNugget/NuggetCacheSettings.h>
#include <AssetNugget/NuggetScc.h>
#include <AssetRuntime/MemoryBulkSource.h>
#include <AssetRuntime/Package.generated.h>
#include <AssetRuntime/Package.h>
#include <AssetRuntime/PackageLoad.h>
#include <AssetRuntime/PackageReader.h>
#include <Core/Hash/Sha256.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Text/NamePool.h>
#include <Meta/GeneratedType.h>
#include <Object/ConstructionContext.h>
#include <Object/ModuleRegistration.h>
#include <Object/ObjectRegistry.h>
#include <Object/ObjectRegistryOptions.h>
#include <Platform/File.h>
#include <Platform/Memory.h>
#include <TestSupport/NFailAllocator.h>
#include <Task/WorkerScheduler.h>
#include <TestSupport/Test.h>
#include <Texture/Texture.generated.h>
#include <TextureTools/TextureBlockEncode.h>
#include <TextureTools/TextureCook.h>

#include <stdio.h>
#include <string.h>

// 텍스처 쿠킹 처리기 시험이다(Docs/Design/AssetSystem 12.14절 — 14.1.101절 대조표): 너겟 머리
// 왕복·손상, 상주 꼬리 R 표·샘플러 필터 표, 차원 다섯의 쿠킹 패키지(EditorOnly 원본 없음·
// platformData 칸·bulk와 mip byte), 스트리밍 bulk·비스트리밍 조건, 샘플러 주소·Border·그룹 필터,
// norm16 대체(넘침값 자르기), 실패(계약 설정·빈 원본·쓰기 실패)와 에디터 객체 불변, 너겟(미적중
// → Put → 적중·SCC meta·키 변이·verify·손상 적중·Conflict 경고), 빌드 경고, 처리기 등록.
using namespace rocks;
namespace texture_cook_tests_file {

using graphics::PixelFormat;

using rocks::asset_database_test::TempProject;

namespace {

AssetId TextureId(uint64 seed) noexcept {
    return AssetId{Guid{0x5445585443000000ull, seed}};
}

// 길이 bytes의 결정적 무늬다.
Array<uint8> Pattern(Allocator& allocator, usize bytes, uint32 seed) noexcept {
    Array<uint8> out(allocator);
    CHECK(out.TryResize(bytes));
    for (usize index = 0; index < bytes; ++index) {
        out[index] = static_cast<uint8>((index * 31 + seed * 7 + (index >> 8)) & 0xFF);
    }

    return out;
}

// 실패하는 쓰기 sink다(쓰기 실패 뒤 되돌리기).
class FailingSink final : public PackageSink {
public:
    Result<void> Append(Span<const uint8>) noexcept override {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    Result<void> WriteAt(uint64, Span<const uint8>) noexcept override {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }
};

// 쿠킹 한 번의 출력이다.
struct CookOut {
    explicit CookOut(Allocator& allocator) noexcept : sink(allocator), warnings(allocator) {
    }

    // 경고 모음만 다른 Allocator로 둔다(경고 기록 실패 시험).
    CookOut(Allocator& allocator, Allocator& warningAllocator) noexcept
        : sink(allocator), warnings(warningAllocator) {
    }

    MemoryPackageSink sink;
    AssetDiagnostic failure;
    Array<CookDiagnostic> warnings;
};

// 쿠킹 패키지를 다시 읽은 결과다.
struct Loaded {
    RootRef<Object> root;
    ObjectHandle package{};
    ObjectHandle texture{};
};

// 텍스처 타입을 등록한 패키지 정책 Registry 둘(에디터·쿠킹 로드)과 에디터 패키지 하나다.
struct CookScene {
    CookScene() noexcept
        : types(allocator), registration(types, allocator), names(allocator), sourceBulk(allocator),
          profile(allocator), entry(allocator) {
        ready = RegisterTextureTypes(registration).IsOk() && names.Initialize().IsOk();
        RegistryOptions options;
        options.names = &names;
        options.policy.packageNativeType = NativeTypeKey<Package>();
        options.policy.rejectWorldOnly = true;
        auto made = UniquePtr<ObjectRegistry>::TryMake(allocator, allocator, types,
                                                       ContextId{0x5458434bull, 1}, options);
        auto other = UniquePtr<ObjectRegistry>::TryMake(allocator, allocator, types,
                                                        ContextId{0x5458434bull, 2}, options);
        if (made && other) {
            editor = Move(made.Value());
            cooked = Move(other.Value());
        }

        ready = ready && editor.Get() != nullptr && cooked.Get() != nullptr &&
                static_cast<bool>(profile.id.TryAssign("Windows"));
    }

    ~CookScene() noexcept {
        package.Reset();
        editor.Reset();
        cooked.Reset();
        CHECK(registration.Unregister());
    }

    ObjectAccessContext EditorAccess() const noexcept {
        return editor->OwnerAccess();
    }

    // 새 에디터 패키지 id에 type 텍스처(원본 format·x·y·slices, pixels)를 만든다.
    bool Make(uint64 id, TypeId type, TextureSourceFormat format, uint32 x, uint32 y, uint32 slices,
              Span<const uint8> pixels) noexcept {
        package.Reset();
        asset = TextureId(id);
        auto made = NewPackage(*editor, EditorAccess(), asset, PackageKind::Editor);
        if (!made) {
            return false;
        }

        package = Move(made.Value());
        auto created = editor->NewObject<Texture>(EditorAccess(), type, package.Handle(), "");
        if (!created) {
            return false;
        }

        texture = created.Value().Handle();
        auto ref = sourceBulk.AddCopy(pixels);
        if (!ref) {
            return false;
        }

        const BulkDataRef bulk = ref.Value();
        return Edit([&](Texture& value) noexcept {
            value.source.format = format;
            value.source.sizeX = x;
            value.source.sizeY = y;
            value.source.numSlices = slices;
            value.source.numMips = 1;
            value.source.numLayers = 1;
            value.source.bulk = bulk;
            value.srgb = 0;
            // G8 원본은 Grayscale로 둔다(R8 비압축 — mip byte를 그대로 본다, AssetSystem 12.17.5
            // 2).
            if (format == TextureSourceFormat::G8) {
                value.compressionSettings = TextureCompressionSettings::Grayscale;
            }

            return true;
        });
    }

    // 2D G8 x×y 무늬 텍스처다.
    bool MakeGray(uint64 id, uint32 x, uint32 y) noexcept {
        const Array<uint8> pixels = Pattern(allocator, usize{x} * y, static_cast<uint32>(id));
        return Make(id, TypeIdOf<Texture2D>(), TextureSourceFormat::G8, x, y, 1, pixels.View());
    }

    template <class Fn>
    bool Edit(Fn&& fn) noexcept {
        auto borrowed = editor->ResolveForWrite<Texture>(EditorAccess(), texture);
        if (!borrowed) {
            return false;
        }

        auto value = borrowed.Value().Get();
        return value && fn(*value.Value());
    }

    // 에디터 객체의 platformData가 기본값인지다(12.14절 9번 — 쿠킹 뒤 불변).
    bool EditorUntouched() noexcept {
        return Edit([](Texture& value) noexcept {
            const TexturePlatformData& data = value.platformData;
            return data.numMips == 0 && data.mips.IsEmpty() && data.pixelFormat == 0 &&
                   data.tailBulk.IsEmpty() && data.residentTailMips == 0 && data.sizeX == 0;
        });
    }

    // 처리기 안 병렬 압축 scheduler다(12.17.9 4 — null이면 직렬).
    WorkerScheduler* scheduler = nullptr;

    Result<void> Cook(const TextureCookProcessor& processor, CookOut& out,
                      NuggetCache* nuggets = nullptr, bool verify = false,
                      PackageSink* sink = nullptr, Allocator* scratch = nullptr) noexcept {
        entry.id = asset;
        CookContext context;
        context.allocator = scratch != nullptr ? scratch : &allocator;
        context.registry = editor.Get();
        context.access = EditorAccess();
        context.package = package.Handle();
        context.entry = &entry;
        context.profile = &profile;
        context.cookerVersion = 1;
        context.bulkSource = &sourceBulk;
        context.nuggets = nuggets;
        context.verifyNuggets = verify;
        context.scheduler = scheduler;
        context.scratch = scheduler != nullptr ? &allocator : nullptr;
        context.sink = sink != nullptr ? sink : &out.sink;
        context.failure = &out.failure;
        context.warnings = &out.warnings;
        return processor.Cook(context);
    }

    // 쿠킹 패키지 byte를 쿠킹 Registry에 로드한다.
    bool Load(Span<const uint8> bytes, Loaded& out) noexcept {
        Array<uint8> copy(allocator);
        if (!copy.TryAppend(bytes)) {
            return false;
        }

        PackageLoadOptions options;
        options.read.expectedKind = PackageKind::Cooked;
        auto made = UniquePtr<PackageLoadState>::TryMake(allocator, allocator, *cooked, types,
                                                         cooked->OwnerAccess(), options);
        if (!made) {
            return false;
        }

        UniquePtr<PackageLoadState> state = Move(made.Value());
        if (!state->Prepare(Move(copy), nullptr)) {
            return false;
        }

        ConstructionContext transaction(*cooked, cooked->OwnerAccess());
        PackageLoadState* pointers[1] = {state.Get()};
        ObjectHandle handles[1] = {};
        RootRef<Object> roots[1];
        if (!InstantiatePackages(transaction, Span<PackageLoadState* const>(pointers, 1), nullptr,
                                 Span<ObjectHandle>(handles), nullptr) ||
            !transaction.Commit(Span<ObjectHandle>(handles), Span<RootRef<Object>>(roots))) {
            return false;
        }

        out.root = Move(roots[0]);
        out.package = handles[0];
        auto child = cooked->FindChild(cooked->OwnerAccess(), out.package, "");
        out.texture = child ? child.Value() : ObjectHandle{};
        return !out.texture.IsNull();
    }

    // 로드한 텍스처다.
    const Texture* LoadedTexture(const Loaded& loaded) noexcept {
        auto borrowed = cooked->Resolve<Texture>(cooked->OwnerAccess(), loaded.texture);
        if (!borrowed) {
            return nullptr;
        }

        auto value = borrowed.Value().Get();
        return value ? value.Value() : nullptr;
    }

    // 로드한 패키지의 bulk byte다.
    Span<const uint8> LoadedBulk(const Loaded& loaded, const BulkDataRef& ref) noexcept {
        auto borrowed = cooked->Resolve<Package>(cooked->OwnerAccess(), loaded.package);
        if (!borrowed) {
            return {};
        }

        auto value = borrowed.Value().Get();
        if (!value) {
            return {};
        }

        auto bytes = value.Value()->BulkBytes(ref);
        return bytes ? bytes.Value() : Span<const uint8>{};
    }

    SystemAllocator allocator;
    TypeRegistry types;
    ModuleRegistration registration;
    NamePool names;
    UniquePtr<ObjectRegistry> editor;
    UniquePtr<ObjectRegistry> cooked;
    RootRef<Package> package;
    ObjectHandle texture{};
    AssetId asset{};
    MemoryBulkSource sourceBulk;
    TargetProfile profile;
    AssetIndexEntry entry;
    bool ready = false;
};

// 쿠킹 패키지의 bulk 항목 수다(읽기 실패면 NumericLimits 최댓값).
usize BulkEntryCount(Allocator& allocator, Span<const uint8> bytes) noexcept {
    PackageReadOptions read;
    read.expectedKind = PackageKind::Cooked;
    auto contents = ReadPackage(allocator, bytes, read, nullptr);
    return contents ? contents.Value().bulkEntries.Size() : NumericLimits<usize>::Max();
}

// 쿠킹 패키지의 bulk 시작 오프셋이 모두 alignment 배수인지 본다(읽기 실패면 거짓).
bool BulkOffsetsAligned(Allocator& allocator, Span<const uint8> bytes, uint64 alignment) noexcept {
    PackageReadOptions read;
    read.expectedKind = PackageKind::Cooked;
    auto contents = ReadPackage(allocator, bytes, read, nullptr);
    if (!contents) {
        return false;
    }

    for (const PackageBulkEntry& bulk : contents.Value().bulkEntries) {
        if (bulk.offset % alignment != 0) {
            return false;
        }
    }

    return true;
}

// 쿠킹·로드해 platformData를 돌려준다(실패하면 null).
const TexturePlatformData* CookAndLoad(CookScene& scene, const TextureCookProcessor& processor,
                                       CookOut& out, Loaded& loaded) noexcept {
    if (!scene.Cook(processor, out) || !scene.Load(out.sink.Bytes(), loaded)) {
        return nullptr;
    }

    const Texture* texture = scene.LoadedTexture(loaded);
    return texture != nullptr ? &texture->platformData : nullptr;
}

// 시험 캐시(실제 디스크 — 임시 프로젝트의 Intermediate/AssetNuggets)다.
struct NuggetScene {
    explicit NuggetScene(Allocator& sceneAllocator, const char* tag) noexcept
        : allocator(sceneAllocator), project(tag), settings(sceneAllocator), root(sceneAllocator) {
        ready = project.made &&
                static_cast<bool>(settings.localPath.TryAssign(NuggetDefaultLocalPath)) &&
                static_cast<bool>(ResolveNuggetLocalPath(settings, project.Root(), root)) &&
                Reopen();
    }

    // 새 캐시 객체로 연다(Memory 계층이 비어 디스크만 본다).
    bool Reopen() noexcept {
        cache.Reset();
        auto made = UniquePtr<NuggetCache>::TryMake(allocator, allocator,
                                                    platform::PhysicalFileSystem(), clock);
        if (!made) {
            return false;
        }

        cache = Move(made.Value());
        return static_cast<bool>(cache->Open(settings, project.Root()));
    }

    // key 항목 파일을 지운다.
    bool Remove(const Sha256Digest& key) noexcept {
        char hex[Sha256Digest::HexBytes + 1];
        key.ToHex(hex);
        String path(allocator);
        return path.TryAppend(root.View()) && path.TryAppend("/") &&
               path.TryAppend(StringView(hex, 2)) && path.TryAppend("/") &&
               path.TryAppend(StringView(hex, Sha256Digest::HexBytes)) && path.TryAppend(".rnug") &&
               static_cast<bool>(platform::PhysicalFileSystem().Delete(path.View().Data(),
                                                                       path.View().ByteCount()));
    }

    // 항목을 values로 바꿔 쓴다(지우고 새 캐시로 Put — 같은 키 다른 값의 적중을 만든다).
    bool Replace(const Sha256Digest& key, AssetId member, Array<NuggetValue>& values) noexcept {
        if (!Remove(key) || !Reopen()) {
            return false;
        }

        NuggetCachePut put;
        put.bucket = NuggetBucketTexture;
        put.key = key;
        put.values = values.View();
        const AssetId members[1] = {member};
        const NuggetCachePutResult stored = cache->PutScc(Span<const AssetId>(members), put);
        return stored.status == NuggetStatus::Stored && Reopen();
    }

    Allocator& allocator;
    TempProject project;
    NuggetCacheSettings settings;
    String root;
    NuggetSystemClock clock;
    UniquePtr<NuggetCache> cache;
    bool ready = false;
};

// 적중 값의 사본이다.
Array<NuggetValue> CopyValues(Allocator& allocator, const NuggetCacheGetResult& got) noexcept {
    Array<NuggetValue> out(allocator);
    for (const NuggetValue& value : got.values) {
        NuggetValue copy(allocator);
        copy.id = value.id;
        CHECK(copy.bytes.TryAppend(value.bytes.View()));
        CHECK(out.TryPushBack(Move(copy)));
    }

    return out;
}

bool HasWarning(const Array<CookDiagnostic>& warnings, AssetDiagnosticCode code) noexcept {
    for (const CookDiagnostic& warning : warnings) {
        if (warning.code == code && warning.warning) {
            return true;
        }
    }

    return false;
}

} // namespace

// 너겟 머리: 왕복(형식·srgb·swizzle·slice·mip 크기와 byte 수), 쓰기 거부(mip 0개·상한 초과),
// 읽기 거부(짧음·버전·PixelFormat 0과 모르는 번호·srgb 2·swizzle 범위·mip 0개·상한·크기 불일치).
TEST(TextureCook_NuggetHeader) {
    SystemAllocator allocator;
    TextureBuildOutputInfo info;
    info.pixelFormat = graphics::PixelFormat::RGBA8Unorm;
    info.srgb = true;
    info.swizzle[3] = TextureSwizzle::One;
    const uint8 bytes[16] = {};
    const TextureCookMip mips[2] = {{2, 2, 1, Span<const uint8>(bytes, 16)},
                                    {1, 1, 1, Span<const uint8>(bytes, 4)}};
    Array<uint8> header(allocator);
    REQUIRE(WriteTextureNuggetHeader(info, 6, Span<const TextureCookMip>(mips), header));
    CHECK(header.Size() == 21 + 2 * 20);
    TextureNuggetHeader read(allocator);
    REQUIRE(ReadTextureNuggetHeader(header.View(), read));
    CHECK(read.output.pixelFormat == graphics::PixelFormat::RGBA8Unorm && read.output.srgb &&
          read.output.swizzle[0] == TextureSwizzle::R &&
          read.output.swizzle[3] == TextureSwizzle::One && read.numSlices == 6);
    REQUIRE(read.mips.Size() == 2);
    CHECK(read.mips[0].sizeX == 2 && read.mips[0].byteSize == 16 && read.mips[1].sizeY == 1 &&
          read.mips[1].byteSize == 4);

    Array<uint8> rejected(allocator);
    CHECK(WriteTextureNuggetHeader(info, 1, {}, rejected).Error() == ErrorCode::InvalidArgument);
    TextureCookMip many[TextureNuggetMaxMips + 1] = {};
    CHECK(WriteTextureNuggetHeader(info, 1, Span<const TextureCookMip>(many), rejected).Error() ==
          ErrorCode::InvalidArgument);

    struct Corrupt {
        usize at;
        uint8 value;
    };
    // 버전(0), PixelFormat(4 — 0과 큰 번호), srgb(8), swizzle(9), mip 수(17).
    const Corrupt corrupts[] = {{0, 2}, {4, 0}, {5, 0xFF}, {8, 2}, {9, 6}, {17, 0}, {17, 3}};
    for (const Corrupt& item : corrupts) {
        Array<uint8> broken(allocator);
        REQUIRE(broken.TryAppend(header.View()));
        if (item.at == 17 && item.value == 0) {
            broken[17] = 0;
        } else {
            broken[item.at] = item.value;
        }

        CHECK(ReadTextureNuggetHeader(broken.View(), read).Error() == ErrorCode::InvalidArgument);
    }

    // 상한을 넘는 mip 수, 짧은·긴 byte.
    Array<uint8> broken(allocator);
    REQUIRE(broken.TryAppend(header.View()));
    broken[17] = static_cast<uint8>(TextureNuggetMaxMips + 1);
    CHECK(ReadTextureNuggetHeader(broken.View(), read).Error() == ErrorCode::InvalidArgument);
    CHECK(ReadTextureNuggetHeader(header.View().Slice(0, 20), read).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(ReadTextureNuggetHeader(header.View().Slice(0, header.Size() - 1), read).Error() ==
          ErrorCode::InvalidArgument);
    REQUIRE(broken.TryResize(0) && broken.TryAppend(header.View()) && broken.TryPushBack(0));
    CHECK(ReadTextureNuggetHeader(broken.View(), read).Error() == ErrorCode::InvalidArgument);
}

// 상주 꼬리 R(12.5.1절 2번 — 1×1·4×4·8×8·256×256·2048×2048·2048×64의 N)과 샘플러 필터 표(UE
// SetupGroup 조합·텍스처 filter).
TEST(TextureCook_ResidentTailAndFilter) {
    struct Row {
        uint32 numMips;
        uint32 streaming;
        uint32 nonStreaming;
    };
    const Row rows[] = {{0, 0, 0}, {1, 1, 1}, {3, 3, 3}, {4, 4, 4},
                        {7, 7, 7}, {8, 7, 8}, {9, 7, 9}, {12, 7, 12}};
    for (const Row& row : rows) {
        CHECK(TextureResidentTailMips(row.numMips, true) == row.streaming);
        CHECK(TextureResidentTailMips(row.numMips, false) == row.nonStreaming);
    }

    struct FilterRow {
        TextureLodMinMagFilter minMag;
        TextureLodMipFilter mip;
        TextureSamplerFilter expected;
    };
    const FilterRow groups[] = {
        {TextureLodMinMagFilter::Point, TextureLodMipFilter::Point, TextureSamplerFilter::Point},
        {TextureLodMinMagFilter::Point, TextureLodMipFilter::Linear, TextureSamplerFilter::Point},
        {TextureLodMinMagFilter::Linear, TextureLodMipFilter::Point,
         TextureSamplerFilter::Bilinear},
        {TextureLodMinMagFilter::Linear, TextureLodMipFilter::Linear,
         TextureSamplerFilter::Trilinear},
        {TextureLodMinMagFilter::Aniso, TextureLodMipFilter::Point,
         TextureSamplerFilter::AnisotropicPoint},
        {TextureLodMinMagFilter::Aniso, TextureLodMipFilter::Linear,
         TextureSamplerFilter::AnisotropicLinear},
    };
    for (const FilterRow& row : groups) {
        TextureLodGroup group;
        group.minMagFilter = row.minMag;
        group.mipFilter = row.mip;
        CHECK(TextureCookSamplerFilter(TextureFilter::Default, group) == row.expected);
        // 텍스처 filter가 Default가 아니면 그룹을 보지 않는다.
        CHECK(TextureCookSamplerFilter(TextureFilter::Nearest, group) ==
              TextureSamplerFilter::Point);
        CHECK(TextureCookSamplerFilter(TextureFilter::Bilinear, group) ==
              TextureSamplerFilter::Bilinear);
        CHECK(TextureCookSamplerFilter(TextureFilter::Trilinear, group) ==
              TextureSamplerFilter::Trilinear);
    }
}

// 차원 다섯(12.14절 시험): 쿠킹 패키지를 다시 읽어 platformData 칸(차원·형식·크기·mip 수·slice·
// R·꼬리 오프셋), bulk 수(꼬리 하나 — EditorOnly 원본 없음), 꼬리 byte 크기, 로드한 원본 ref가 빔,
// 에디터 객체 불변을 본다.
TEST(TextureCook_Dimensions) {
    CookScene scene;
    REQUIRE(scene.ready);
    TextureCookProcessor processor;
    struct Case {
        TypeId type;
        TextureDimension dimension;
        uint32 x;
        uint32 y;
        uint32 slices;
        uint32 numMips;
        uint32 numSlices;
        uint32 sizeZ;
        uint64 tailBytes;
    };
    const Case cases[] = {
        // 꼬리 byte는 mip마다 16 byte 정렬 시작을 넣은 크기다(64·16·4·1 → 0·64·80·96 → 97).
        {TypeIdOf<Texture2D>(), TextureDimension::Texture2D, 8, 8, 1, 4, 1, 1, 96 + 1},
        {TypeIdOf<TextureCube>(), TextureDimension::TextureCube, 4, 4, 6, 3, 6, 1, 128 + 6},
        {TypeIdOf<Texture2DArray>(), TextureDimension::Texture2DArray, 4, 4, 2, 3, 2, 1, 48 + 2},
        {TypeIdOf<TextureCubeArray>(), TextureDimension::TextureCubeArray, 4, 4, 12, 3, 12, 1,
         12 * 21},
        {TypeIdOf<VolumeTexture>(), TextureDimension::Volume, 4, 4, 4, 3, 1, 4, 80 + 1},
    };
    uint64 id = 1;
    for (const Case& item : cases) {
        const Array<uint8> pixels =
            Pattern(scene.allocator, usize{item.x} * item.y * item.slices, static_cast<uint32>(id));
        REQUIRE(scene.Make(id++, item.type, TextureSourceFormat::G8, item.x, item.y, item.slices,
                           pixels.View()));
        CookOut out(scene.allocator);
        Loaded loaded;
        const TexturePlatformData* data = CookAndLoad(scene, processor, out, loaded);
        REQUIRE(data != nullptr);
        CHECK(data->dimension == item.dimension &&
              data->pixelFormat == static_cast<uint8>(graphics::PixelFormat::R8Unorm) &&
              !data->srgb && data->swizzleG == TextureSwizzle::R &&
              data->swizzleA == TextureSwizzle::R);
        CHECK(data->sizeX == item.x && data->sizeY == item.y && data->sizeZ == item.sizeZ &&
              data->numMips == item.numMips && data->numSlices == item.numSlices &&
              data->residentTailMips == item.numMips && data->mips.Size() == item.numMips);
        uint64 offset = 0;
        for (const TextureMipData& mip : data->mips) {
            offset = (offset + TextureTailMipAlignment - 1) & ~(TextureTailMipAlignment - 1);
            CHECK(mip.bulk.IsEmpty() && mip.tailOffset == offset);
            offset += mip.byteSize;
        }

        CHECK(offset == item.tailBytes && data->tailBulk.rawBytes == item.tailBytes &&
              scene.LoadedBulk(loaded, data->tailBulk).Size() == item.tailBytes);
        CHECK(BulkEntryCount(scene.allocator, out.sink.Bytes()) == 1);
        const Texture* texture = scene.LoadedTexture(loaded);
        REQUIRE(texture != nullptr);
        CHECK(texture->source.bulk.IsEmpty() && texture->source.sizeX == 0);
        CHECK(scene.EditorUntouched() && out.warnings.IsEmpty());
    }

    CHECK(processor.Builds() == 5);
}

// 스트리밍(12.5.1절): 256×256은 R = 7·스트리밍 mip 둘이 각각 bulk(크기·SHA-256이 mip byte와 같음)·
// 꼬리 오프셋, R 표의 큰 행(2048×2048·2048×64), 비스트리밍 조건마다 R = N·스트리밍 bulk 없음
// (NeverStream·UI 그룹·NoMipmaps·CPU·비 2의 거듭제곱 X·Cube·Volume Z — Z가 2의 거듭제곱이면
// 스트리밍).
TEST(TextureCook_Streaming) {
    CookScene scene;
    REQUIRE(scene.ready);
    TextureCookProcessor processor;
    {
        REQUIRE(scene.MakeGray(1, 256, 256));
        CookOut out(scene.allocator);
        Loaded loaded;
        const TexturePlatformData* data = CookAndLoad(scene, processor, out, loaded);
        REQUIRE(data != nullptr && data->numMips == 9 && data->residentTailMips == 7);
        CHECK(data->mips[0].bulk.rawBytes == 65536 && data->mips[1].bulk.rawBytes == 16384);
        for (uint32 index = 0; index < 2; ++index) {
            const Span<const uint8> bytes = scene.LoadedBulk(loaded, data->mips[index].bulk);
            CHECK(bytes.Size() == data->mips[index].byteSize &&
                  Sha256::Hash(bytes) == data->mips[index].bulk.sha256);
        }

        uint64 offset = 0;
        for (uint32 index = 2; index < 9; ++index) {
            offset = (offset + TextureTailMipAlignment - 1) & ~(TextureTailMipAlignment - 1);
            CHECK(data->mips[index].bulk.IsEmpty() && data->mips[index].tailOffset == offset);
            offset += data->mips[index].byteSize;
        }

        // 4096·1024·256·64·16·4·1 → 시작 0·4096·5120·5376·5440·5456·5472(16 byte 정렬) → 5473.
        CHECK(offset == 5472 + 1 && data->tailBulk.rawBytes == offset);
        CHECK(BulkEntryCount(scene.allocator, out.sink.Bytes()) == 3);
        CHECK(BulkOffsetsAligned(scene.allocator, out.sink.Bytes(), TextureStreamingBulkAlignment));
    }

    // 프로파일 정렬이 4 KiB보다 작아도 텍스처는 4 KiB다(렌더 요구 — 12.14절 A5j3).
    {
        constexpr uint32 SmallAlignment = 16;
        scene.profile.bulkAlignment = SmallAlignment;
        REQUIRE(scene.MakeGray(2, 256, 256));
        CookOut out(scene.allocator);
        Loaded loaded;
        REQUIRE(CookAndLoad(scene, processor, out, loaded) != nullptr);
        CHECK(BulkEntryCount(scene.allocator, out.sink.Bytes()) == 3 &&
              BulkOffsetsAligned(scene.allocator, out.sink.Bytes(), TextureStreamingBulkAlignment));
        scene.profile.bulkAlignment = 0;
    }

    struct Size {
        uint32 x;
        uint32 y;
        uint32 numMips;
        uint32 resident;
    };
    const Size sizes[] = {
        {1, 1, 1, 1}, {4, 4, 3, 3}, {8, 8, 4, 4}, {2048, 2048, 12, 7}, {2048, 64, 12, 7}};
    uint64 id = 10;
    for (const Size& size : sizes) {
        REQUIRE(scene.MakeGray(id++, size.x, size.y));
        CookOut out(scene.allocator);
        Loaded loaded;
        const TexturePlatformData* data = CookAndLoad(scene, processor, out, loaded);
        REQUIRE(data != nullptr);
        CHECK(data->numMips == size.numMips && data->residentTailMips == size.resident);
        CHECK(BulkEntryCount(scene.allocator, out.sink.Bytes()) ==
              size.numMips - size.resident + 1);
    }

    // 비스트리밍 조건(256×256 기준 — 2D 편집 하나씩).
    enum class Condition {
        NeverStream,
        UiGroup,
        NoMipmaps,
        Cpu
    };
    const Condition conditions[] = {Condition::NeverStream, Condition::UiGroup,
                                    Condition::NoMipmaps, Condition::Cpu};
    for (Condition condition : conditions) {
        REQUIRE(scene.MakeGray(id++, 256, 256));
        REQUIRE(scene.Edit([&](Texture& texture) noexcept {
            switch (condition) {
            case Condition::NeverStream:
                texture.neverStream = 1;
                break;
            case Condition::UiGroup:
                texture.lodGroup = TextureGroup::UI;
                break;
            case Condition::NoMipmaps:
                texture.mipGenSettings = TextureMipGenSettings::NoMipmaps;
                break;
            case Condition::Cpu:
                texture.availability = TextureAvailability::CPU;
                break;
            }

            return true;
        }));
        CookOut out(scene.allocator);
        Loaded loaded;
        const TexturePlatformData* data = CookAndLoad(scene, processor, out, loaded);
        REQUIRE(data != nullptr);
        CHECK(data->residentTailMips == data->numMips &&
              BulkEntryCount(scene.allocator, out.sink.Bytes()) == 1);
        // CPU availability는 Texture2D에서 NoMipmaps가 된다(UE TextureDerivedData.cpp 1278–1285).
        CHECK(data->numMips ==
              (condition == Condition::NoMipmaps || condition == Condition::Cpu ? 1u : 9u));
    }

    // 비 2의 거듭제곱 X(256×192 — N 9).
    {
        REQUIRE(scene.MakeGray(id++, 192, 256));
        CookOut out(scene.allocator);
        Loaded loaded;
        const TexturePlatformData* data = CookAndLoad(scene, processor, out, loaded);
        REQUIRE(data != nullptr);
        CHECK(data->numMips == 9 && data->residentTailMips == 9);
    }

    // Cube 256(6면)·Volume 256×256×3(Z 비 2의 거듭제곱)·256×256×2(스트리밍).
    struct Shape {
        TypeId type;
        uint32 slices;
        uint32 resident;
    };
    const Shape shapes[] = {{TypeIdOf<TextureCube>(), 6, 9},
                            {TypeIdOf<VolumeTexture>(), 3, 9},
                            {TypeIdOf<VolumeTexture>(), 2, 7}};
    for (const Shape& shape : shapes) {
        const Array<uint8> pixels = Pattern(scene.allocator, usize{256} * 256 * shape.slices, 3);
        REQUIRE(scene.Make(id++, shape.type, TextureSourceFormat::G8, 256, 256, shape.slices,
                           pixels.View()));
        CookOut out(scene.allocator);
        Loaded loaded;
        const TexturePlatformData* data = CookAndLoad(scene, processor, out, loaded);
        REQUIRE(data != nullptr);
        CHECK(data->numMips == 9 && data->residentTailMips == shape.resident);
    }
}

// 샘플러(12.14절 8번): Texture2D 주소 X·Y·W Wrap·Border, 그룹 필터 조합·maxAniso, 텍스처 filter,
// 2DArray Z, Volume addressMode, Cube Clamp·TransparentBlack. norm16 대체: R16·RG16·RGBA16 Unorm →
// 16F, 확대 재표본의 넘침값을 [0, 1]로 자른다.
TEST(TextureCook_SamplerAndNorm16) {
    CookScene scene;
    REQUIRE(scene.ready);
    TextureCookProcessor processor;
    TextureLodGroup world;
    world.minMagFilter = TextureLodMinMagFilter::Linear;
    world.mipFilter = TextureLodMipFilter::Linear;
    world.maxAniso = 8;
    REQUIRE(scene.profile.textureLodGroups.InsertOrAssign(TextureGroup::World, world));
    {
        REQUIRE(scene.MakeGray(1, 4, 4));
        REQUIRE(scene.Edit([](Texture& texture) noexcept {
            auto& typed = static_cast<Texture2D&>(texture);
            typed.addressX = TextureAddress::Clamp;
            typed.addressY = TextureAddress::Mirror;
            typed.borderColor = TextureBorderColor::OpaqueWhite;
            return true;
        }));
        CookOut out(scene.allocator);
        Loaded loaded;
        const TexturePlatformData* data = CookAndLoad(scene, processor, out, loaded);
        REQUIRE(data != nullptr);
        const TextureSamplerSettings& sampler = data->sampler;
        CHECK(sampler.filter == TextureSamplerFilter::Trilinear && sampler.maxAniso == 8 &&
              sampler.addressU == TextureAddress::Clamp &&
              sampler.addressV == TextureAddress::Mirror &&
              sampler.addressW == TextureAddress::Wrap &&
              sampler.borderColor == TextureBorderColor::OpaqueWhite);
    }

    {
        REQUIRE(scene.MakeGray(2, 4, 4));
        REQUIRE(scene.Edit([](Texture& texture) noexcept {
            texture.filter = TextureFilter::Nearest;
            texture.lodGroup = TextureGroup::Character;
            return true;
        }));
        CookOut out(scene.allocator);
        Loaded loaded;
        const TexturePlatformData* data = CookAndLoad(scene, processor, out, loaded);
        REQUIRE(data != nullptr);
        // Character는 표에 없어 기본 그룹(maxAniso 0)이다.
        CHECK(data->sampler.filter == TextureSamplerFilter::Point && data->sampler.maxAniso == 0);
    }

    struct Shape {
        uint64 id;
        TypeId type;
        uint32 slices;
        TextureAddress u;
        TextureAddress w;
        TextureBorderColor border;
    };
    const Shape shapes[] = {
        {3, TypeIdOf<Texture2DArray>(), 2, TextureAddress::Wrap, TextureAddress::Mirror,
         TextureBorderColor::OpaqueBlack},
        {4, TypeIdOf<VolumeTexture>(), 2, TextureAddress::Clamp, TextureAddress::Clamp,
         TextureBorderColor::OpaqueBlack},
        {5, TypeIdOf<TextureCube>(), 6, TextureAddress::Clamp, TextureAddress::Clamp,
         TextureBorderColor::TransparentBlack},
        {6, TypeIdOf<TextureCubeArray>(), 6, TextureAddress::Clamp, TextureAddress::Clamp,
         TextureBorderColor::TransparentBlack},
    };
    for (const Shape& shape : shapes) {
        const Array<uint8> pixels = Pattern(scene.allocator, usize{4} * 4 * shape.slices, 5);
        REQUIRE(scene.Make(shape.id, shape.type, TextureSourceFormat::G8, 4, 4, shape.slices,
                           pixels.View()));
        REQUIRE(scene.Edit([&](Texture& texture) noexcept {
            if (shape.type == TypeIdOf<Texture2DArray>()) {
                auto& typed = static_cast<Texture2DArray&>(texture);
                typed.addressZ = TextureAddress::Mirror;
                typed.borderColor = TextureBorderColor::OpaqueBlack;
            } else if (shape.type == TypeIdOf<VolumeTexture>()) {
                auto& typed = static_cast<VolumeTexture&>(texture);
                typed.addressMode = TextureAddress::Clamp;
                typed.borderColor = TextureBorderColor::OpaqueBlack;
            }

            return true;
        }));
        CookOut out(scene.allocator);
        Loaded loaded;
        const TexturePlatformData* data = CookAndLoad(scene, processor, out, loaded);
        REQUIRE(data != nullptr);
        CHECK(data->sampler.addressU == shape.u && data->sampler.addressV == shape.u &&
              data->sampler.addressW == shape.w && data->sampler.borderColor == shape.border);
    }

    // norm16 대체.
    struct Norm16 {
        TextureSourceFormat format;
        uint32 channels;
        graphics::PixelFormat supported;
        graphics::PixelFormat fallback;
    };
    // norm16이 형식을 바꾸는 것은 G16 원본의 Grayscale·Displacementmap뿐이다(UE G16 — 그 밖의
    // 16비트 원본은 압축하거나 BGRA8로 낸다, 12.17.5 2).
    const Norm16 formats[] = {
        {TextureSourceFormat::G16, 1, graphics::PixelFormat::R16Unorm,
         graphics::PixelFormat::R16Float},
    };
    uint64 id = 20;
    for (const Norm16& item : formats) {
        // 열마다 0·65535를 번갈아 둔 4×4를 8×8로 확대한다(Catmull-Rom 넘침).
        Array<uint8> pixels(scene.allocator);
        REQUIRE(pixels.TryResize(usize{4} * 4 * item.channels * 2));
        for (usize index = 0; index < pixels.Size() / 2; ++index) {
            const uint16 value = ((index / item.channels) % 2) != 0 ? 0xFFFF : 0;
            pixels[index * 2] = static_cast<uint8>(value);
            pixels[index * 2 + 1] = static_cast<uint8>(value >> 8);
        }

        const bool supported[] = {true, false};
        for (bool norm16 : supported) {
            scene.profile.textureNorm16Supported = norm16;
            REQUIRE(scene.Make(id++, TypeIdOf<Texture2D>(), item.format, 4, 4, 1, pixels.View()));
            REQUIRE(scene.Edit([](Texture& texture) noexcept {
                texture.compressionSettings = TextureCompressionSettings::Grayscale;
                texture.powerOfTwoMode = TexturePowerOfTwoSetting::ResizeToSpecificResolution;
                texture.resizeDuringBuildX = 8;
                texture.resizeDuringBuildY = 8;
                return true;
            }));
            CookOut out(scene.allocator);
            Loaded loaded;
            const TexturePlatformData* data = CookAndLoad(scene, processor, out, loaded);
            REQUIRE(data != nullptr);
            CHECK(data->pixelFormat == static_cast<uint8>(norm16 ? item.supported : item.fallback));
            if (!norm16) {
                // 16F 값은 모두 [0, 1](부호 bit 없음·1.0 = 0x3C00 이하)이다.
                const Span<const uint8> tail = scene.LoadedBulk(loaded, data->tailBulk);
                REQUIRE(tail.Size() == data->tailBulk.rawBytes && tail.Size() % 2 == 0);
                bool inRange = true;
                for (usize at = 0; at + 1 < tail.Size(); at += 2) {
                    const uint16 half = static_cast<uint16>(tail[at] | (tail[at + 1] << 8));
                    inRange = inRange && half <= 0x3C00;
                }

                CHECK(inRange);
            }
        }
    }

    scene.profile.textureNorm16Supported = true;
}

// 실패(12.14절 2·4·9번): 계약 설정(OnlyFirstMip — UnsupportedSetting·Unsupported·detail = 이슈 수),
// 빈 원본 InvalidSource, 쓰기 실패(sink 오류 그대로) — 모두 에디터 객체 불변. 두 프로파일 연속
// 쿠킹(norm16 다름)이 서로 값을 남기지 않는다. 대표 에셋이 텍스처가 아니면 TypeMismatch.
TEST(TextureCook_FailuresKeepEditorObject) {
    CookScene scene;
    REQUIRE(scene.ready);
    TextureCookProcessor processor;
    REQUIRE(scene.MakeGray(1, 4, 4));
    REQUIRE(scene.Edit([](Texture& texture) noexcept {
        texture.mipLoadOptions = TextureMipLoadOptions::OnlyFirstMip;
        return true;
    }));
    {
        CookOut out(scene.allocator);
        auto cooked = scene.Cook(processor, out);
        CHECK(!cooked && cooked.Error() == ErrorCode::Unsupported &&
              out.failure.code == AssetDiagnosticCode::UnsupportedSetting &&
              out.failure.detail >= 1 && out.failure.asset == scene.asset);
        CHECK(scene.EditorUntouched() && processor.Builds() == 0 && out.sink.Bytes().IsEmpty());
    }

    REQUIRE(scene.MakeGray(2, 4, 4));
    REQUIRE(scene.Edit([](Texture& texture) noexcept {
        texture.source.bulk = BulkDataRef{};
        return true;
    }));
    {
        CookOut out(scene.allocator);
        auto cooked = scene.Cook(processor, out);
        CHECK(!cooked && cooked.Error() == ErrorCode::InvalidArgument &&
              out.failure.code == AssetDiagnosticCode::InvalidSource);
        CHECK(scene.EditorUntouched() && processor.Builds() == 0);
    }

    REQUIRE(scene.MakeGray(3, 256, 256));
    {
        CookOut out(scene.allocator);
        FailingSink failing;
        auto cooked = scene.Cook(processor, out, nullptr, false, &failing);
        CHECK(!cooked && cooked.Error() == ErrorCode::CapacityExceeded);
        CHECK(scene.EditorUntouched() && processor.Builds() == 1);
    }

    // 두 프로파일 연속(G16 — Windows R16Unorm, Android R16Float).
    {
        const Array<uint8> pixels = Pattern(scene.allocator, 4 * 4 * 2, 9);
        REQUIRE(
            scene.Make(4, TypeIdOf<Texture2D>(), TextureSourceFormat::G16, 4, 4, 1, pixels.View()));
        REQUIRE(scene.Edit([](Texture& texture) noexcept {
            texture.compressionSettings = TextureCompressionSettings::Grayscale;
            return true;
        }));
        CookOut windows(scene.allocator);
        REQUIRE(scene.Cook(processor, windows));
        CHECK(scene.EditorUntouched());
        scene.profile.textureNorm16Supported = false;
        REQUIRE(scene.profile.id.TryAssign("Android"));
        CookOut android(scene.allocator);
        REQUIRE(scene.Cook(processor, android));
        CHECK(scene.EditorUntouched());
        Loaded first;
        REQUIRE(scene.Load(windows.sink.Bytes(), first));
        const Texture* texture = scene.LoadedTexture(first);
        REQUIRE(texture != nullptr);
        CHECK(texture->platformData.pixelFormat ==
              static_cast<uint8>(graphics::PixelFormat::R16Unorm));
        first.root.Reset();
        CHECK(!(windows.sink.Bytes().Size() == android.sink.Bytes().Size() &&
                platform::MemoryEquals(windows.sink.Bytes().Data(), android.sink.Bytes().Data(),
                                       windows.sink.Bytes().Size())));
        scene.profile.textureNorm16Supported = true;
        REQUIRE(scene.profile.id.TryAssign("Windows"));
    }

    // 대표 에셋이 텍스처가 아닌 패키지는 없다(Texture 표 등록) — 대표 자식이 없으면 찾기 오류다.
    {
        auto made =
            NewPackage(*scene.editor, scene.EditorAccess(), TextureId(99), PackageKind::Editor);
        REQUIRE(made);
        scene.package = Move(made.Value());
        scene.asset = TextureId(99);
        CookOut out(scene.allocator);
        CHECK(!scene.Cook(processor, out));
    }
}

// 할당 실패 전수(14.1.101 — 빌드 때 더한 것): 쿠킹 문맥 Allocator의 n번째 할당이 실패하면 쿠킹은
// OutOfMemory(진단 None)이고 에디터 객체는 그대로다. 실패가 없으면(시도가 다 쓰이면) 기준과 같은
// byte다. 너겟 캐시가 있는 쿠킹(미적중 → Put, 적중 → Get)도 같다.
TEST(TextureCook_AllocationFailures) {
    CookScene scene;
    REQUIRE(scene.ready);
    TextureCookProcessor processor;
    REQUIRE(scene.MakeGray(1, 64, 32));
    CookOut reference(scene.allocator);
    REQUIRE(scene.Cook(processor, reference));
    const auto sweep = [&](NuggetCache* nuggets) noexcept {
        uint32 failures = 0;
        for (uint64 attempt = 1;; ++attempt) {
            test::NFailAllocator failing(scene.allocator);
            failing.FailOnAttempt(attempt);
            CookOut out(scene.allocator);
            auto cooked = scene.Cook(processor, out, nuggets, false, nullptr, &failing);
            const bool exhausted = failing.Attempts() < attempt;
            failing.DisableFailure();
            if (exhausted) {
                CHECK(cooked);
                CHECK(out.sink.Bytes().Size() == reference.sink.Bytes().Size() &&
                      platform::MemoryEquals(out.sink.Bytes().Data(), reference.sink.Bytes().Data(),
                                             reference.sink.Bytes().Size()));
                break;
            }

            if (cooked) {
                continue;
            }

            ++failures;
            CHECK(cooked.Error() == ErrorCode::OutOfMemory &&
                  out.failure.code == AssetDiagnosticCode::None);
            CHECK(scene.EditorUntouched());
        }

        return failures;
    };
    CHECK(sweep(nullptr) > 0);
    NuggetScene nuggets(scene.allocator, "texture-cook-allocation");
    REQUIRE(nuggets.ready);
    // 첫 sweep의 성공 시도가 너겟을 Put하므로 둘째 sweep은 적중(Get) 경로다.
    CHECK(sweep(nuggets.cache.Get()) > 0);
    CHECK(sweep(nuggets.cache.Get()) > 0);
}

// 빌드 경고(12.14절 10번): SrgbIgnored(Masks + SRGB)·PreserveBorderWithWrap가 대상 id의 경고로
// 남고 쿠킹은 성공한다. 너겟 적중이어도 같은 경고다.
TEST(TextureCook_Warnings) {
    CookScene scene;
    REQUIRE(scene.ready);
    NuggetScene nuggets(scene.allocator, "texture-cook-warnings");
    REQUIRE(nuggets.ready);
    TextureCookProcessor processor;
    const Array<uint8> pixels = Pattern(scene.allocator, 8 * 8 * 4, 2);
    REQUIRE(
        scene.Make(1, TypeIdOf<Texture2D>(), TextureSourceFormat::RGBA8, 8, 8, 1, pixels.View()));
    REQUIRE(scene.Edit([](Texture& texture) noexcept {
        texture.srgb = 1;
        texture.compressionSettings = TextureCompressionSettings::Masks;
        texture.preserveBorder = 1;
        return true;
    }));
    for (int round = 0; round < 2; ++round) {
        CookOut out(scene.allocator);
        REQUIRE(scene.Cook(processor, out, nuggets.cache.Get()));
        CHECK(out.warnings.Size() == 2 &&
              HasWarning(out.warnings, AssetDiagnosticCode::SrgbIgnored) &&
              HasWarning(out.warnings, AssetDiagnosticCode::PreserveBorderWithWrap));
        for (const CookDiagnostic& warning : out.warnings) {
            CHECK(warning.asset == scene.asset);
        }
    }

    CHECK(processor.Builds() == 1);
}

// 경고 기록 실패(14.1.119 후속 2번): 경고 모음(호출자 Allocator)의 n번째 할당이 실패하면 쿠킹은
// OutOfMemory이고 실패 진단은 None이다. 너겟 미적중·적중이 같다. 실패가 없으면 경고 둘이다.
TEST(TextureCook_WarningAllocationFailures) {
    CookScene scene;
    REQUIRE(scene.ready);
    NuggetScene nuggets(scene.allocator, "texture-cook-warning-failures");
    REQUIRE(nuggets.ready);
    TextureCookProcessor processor;
    const Array<uint8> pixels = Pattern(scene.allocator, 8 * 8 * 4, 2);
    REQUIRE(
        scene.Make(1, TypeIdOf<Texture2D>(), TextureSourceFormat::RGBA8, 8, 8, 1, pixels.View()));
    REQUIRE(scene.Edit([](Texture& texture) noexcept {
        texture.srgb = 1;
        texture.compressionSettings = TextureCompressionSettings::Masks;
        texture.preserveBorder = 1;
        return true;
    }));
    for (int round = 0; round < 2; ++round) {
        uint32 failures = 0;
        for (uint64 attempt = 1;; ++attempt) {
            test::NFailAllocator failing(scene.allocator);
            failing.FailOnAttempt(attempt);
            CookOut out(scene.allocator, failing);
            auto cooked = scene.Cook(processor, out, nuggets.cache.Get());
            const bool exhausted = failing.Attempts() < attempt;
            failing.DisableFailure();
            if (exhausted) {
                CHECK(cooked && out.warnings.Size() == 2);
                break;
            }

            ++failures;
            CHECK(!cooked && cooked.Error() == ErrorCode::OutOfMemory &&
                  out.failure.code == AssetDiagnosticCode::None);
        }

        CHECK(failures > 0);
    }
}

// 너겟(12.14절 5·6번): 미적중 → Put(SCC meta 구성원 0·ValueId 0..N) → 같은 키 적중(빌드 없음·같은
// byte), 키 밖 입력(샘플러 filter·neverStream·같은 값의 그룹·프로파일 id·주소·8비트 원본의 norm16 —
// 적중), 키 변이(LODBias·압축 기록·원본·16비트 원본의 norm16 — 미적중), verify 일치(빌드하고 Put
// 없음), 손상 적중(mip byte — verify NuggetMismatch·아니면 그대로 씀, 머리·값 수·크기 사슬·잘린
// 사슬·형식 — 미적중·Reject·다시 Put, 다음 쿠킹 적중).
TEST(TextureCook_Nuggets) {
    CookScene scene;
    REQUIRE(scene.ready);
    NuggetScene nuggets(scene.allocator, "texture-cook-nuggets");
    REQUIRE(nuggets.ready);
    TextureCookProcessor processor;
    REQUIRE(scene.MakeGray(1, 16, 16));
    CookOut first(scene.allocator);
    REQUIRE(scene.Cook(processor, first, nuggets.cache.Get()));
    CHECK(processor.Builds() == 1 && nuggets.cache->Stats().stored == 1);
    const Sha256Digest key = processor.LastNuggetKey();
    {
        NuggetCacheGetResult record = nuggets.cache->Get(NuggetCacheGet{NuggetBucketTexture, key});
        REQUIRE(record.status == NuggetStatus::Hit);
        auto member = FindNuggetSccMember(record.record.meta.View(), scene.asset);
        CHECK(member && member.Value() == 0);
        REQUIRE(record.values.Size() == 5 + 1);
        for (usize index = 0; index < record.values.Size(); ++index) {
            CHECK(record.values[index].id == NuggetSccValueId(0, static_cast<uint32>(index)));
        }
    }

    // 같은 키 적중(새 캐시 객체 — 디스크).
    REQUIRE(nuggets.Reopen());
    {
        CookOut again(scene.allocator);
        REQUIRE(scene.Cook(processor, again, nuggets.cache.Get()));
        CHECK(processor.Builds() == 1 && processor.LastNuggetKey() == key &&
              again.sink.Bytes().Size() == first.sink.Bytes().Size() &&
              platform::MemoryEquals(again.sink.Bytes().Data(), first.sink.Bytes().Data(),
                                     first.sink.Bytes().Size()));
    }

    // 키 밖 입력은 적중이다.
    enum class Outside {
        Filter,
        NeverStream,
        Group,
        ProfileId,
        Norm16
    };
    const Outside outside[] = {Outside::Filter, Outside::NeverStream, Outside::Group,
                               Outside::ProfileId, Outside::Norm16};
    for (Outside change : outside) {
        if (change == Outside::ProfileId) {
            // 출력이 같은 다른 플랫폼(Mac — 검증 끔 대상의 BC 표가 같다)은 너겟을 같이 쓴다.
            REQUIRE(scene.profile.id.TryAssign("Mac"));
        } else if (change == Outside::Norm16) {
            // 8비트 원본은 norm16과 무관하다(키 정규화 — Windows·Android가 너겟을 함께 쓴다).
            scene.profile.textureNorm16Supported = false;
        } else {
            REQUIRE(scene.Edit([&](Texture& texture) noexcept {
                if (change == Outside::Filter) {
                    texture.filter = TextureFilter::Nearest;
                } else if (change == Outside::NeverStream) {
                    texture.neverStream = 1;
                } else {
                    texture.lodGroup = TextureGroup::Character;
                }

                return true;
            }));
        }

        CookOut out(scene.allocator);
        REQUIRE(scene.Cook(processor, out, nuggets.cache.Get()));
        CHECK(processor.Builds() == 1 && processor.LastNuggetKey() == key);
    }

    // 키 변이는 미적중이다(서로 다른 키).
    Array<Sha256Digest> seen(scene.allocator);
    REQUIRE(seen.TryPushBack(key));
    enum class Inside {
        LodBias,
        CacheId,
        NoAlpha,
        Address,
        Source
    };
    const Inside inside[] = {Inside::LodBias, Inside::CacheId, Inside::NoAlpha, Inside::Address,
                             Inside::Source};
    uint64 builds = processor.Builds();
    for (Inside change : inside) {
        if (change == Inside::Source) {
            const Array<uint8> other = Pattern(scene.allocator, 16 * 16, 77);
            auto ref = scene.sourceBulk.AddCopy(other.View());
            REQUIRE(ref);
            const BulkDataRef bulk = ref.Value();
            REQUIRE(scene.Edit([&](Texture& texture) noexcept {
                texture.source.bulk = bulk;
                return true;
            }));
        } else {
            REQUIRE(scene.Edit([&](Texture& texture) noexcept {
                if (change == Inside::LodBias) {
                    texture.lodBias = 1;
                } else if (change == Inside::CacheId) {
                    texture.compressionCacheId = Guid{0xCAC4E, 1};
                } else if (change == Inside::Address) {
                    // 주소는 커널 mip의 원본 밖 처리를 바꾼다(12.18 TP1 — 키에 든다).
                    static_cast<Texture2D&>(texture).addressX = TextureAddress::Clamp;
                    static_cast<Texture2D&>(texture).addressY = TextureAddress::Mirror;
                } else {
                    texture.compressionNoAlpha = 1;
                }

                return true;
            }));
        }

        CookOut out(scene.allocator);
        REQUIRE(scene.Cook(processor, out, nuggets.cache.Get()));
        CHECK(processor.Builds() == ++builds);
        bool distinct = true;
        for (const Sha256Digest& other : seen) {
            distinct = distinct && !(other == processor.LastNuggetKey());
        }

        CHECK(distinct);
        REQUIRE(seen.TryPushBack(processor.LastNuggetKey()));
    }

    // G16 Grayscale은 norm16이 출력을 바꾸므로(R16Unorm·R16Float) 키가 다르다.
    {
        const Array<uint8> pixels = Pattern(scene.allocator, 4 * 4 * 2, 13);
        REQUIRE(
            scene.Make(3, TypeIdOf<Texture2D>(), TextureSourceFormat::G16, 4, 4, 1, pixels.View()));
        REQUIRE(scene.Edit([](Texture& texture) noexcept {
            texture.compressionSettings = TextureCompressionSettings::Grayscale;
            return true;
        }));
        scene.profile.textureNorm16Supported = true;
        CookOut supported(scene.allocator);
        REQUIRE(scene.Cook(processor, supported, nuggets.cache.Get()));
        const Sha256Digest unorm = processor.LastNuggetKey();
        scene.profile.textureNorm16Supported = false;
        CookOut fallback(scene.allocator);
        REQUIRE(scene.Cook(processor, fallback, nuggets.cache.Get()));
        builds += 2;
        CHECK(processor.Builds() == builds && !(processor.LastNuggetKey() == unorm));
    }

    // 처음 상태로 되돌린 새 텍스처(같은 id 패키지는 다시 만들 수 없어 처음 키를 다시 쓰려면 같은
    // 값의 새 패키지 — 여기서는 새 id로 verify와 손상을 본다).
    scene.profile.textureNorm16Supported = true;
    REQUIRE(scene.profile.id.TryAssign("Windows"));
    REQUIRE(scene.MakeGray(2, 16, 16));
    CookOut base(scene.allocator);
    REQUIRE(scene.Cook(processor, base, nuggets.cache.Get()));
    builds = processor.Builds();
    const Sha256Digest baseKey = processor.LastNuggetKey();
    const uint64 stored = nuggets.cache->Stats().stored;
    {
        CookOut verified(scene.allocator);
        REQUIRE(scene.Cook(processor, verified, nuggets.cache.Get(), true));
        CHECK(processor.Builds() == ++builds && nuggets.cache->Stats().stored == stored);
    }

    NuggetCacheGetResult good =
        nuggets.cache->GetSccMember(NuggetBucketTexture, baseKey, scene.asset);
    REQUIRE(good.status == NuggetStatus::Hit && good.values.Size() == 6);

    // mip byte 하나만 다름: 일반 쿠킹은 적중을 그대로 쓰고 verify는 NuggetMismatch다.
    {
        Array<NuggetValue> values = CopyValues(scene.allocator, good);
        values[1].bytes[0] = static_cast<uint8>(values[1].bytes[0] ^ 0xFF);
        REQUIRE(nuggets.Replace(baseKey, scene.asset, values));
        CookOut trusted(scene.allocator);
        REQUIRE(scene.Cook(processor, trusted, nuggets.cache.Get()));
        CHECK(processor.Builds() == builds);
        CookOut verified(scene.allocator);
        auto mismatch = scene.Cook(processor, verified, nuggets.cache.Get(), true);
        CHECK(!mismatch && mismatch.Error() == ErrorCode::InvalidArgument &&
              verified.failure.code == AssetDiagnosticCode::NuggetMismatch);
        CHECK(processor.Builds() == ++builds && scene.EditorUntouched());
    }

    // 미적중으로 다루는 손상: 머리 버전·값 하나 빠짐·크기 사슬·잘린 사슬(머리도 맞춘 — 사슬
    // 끝)·형식 규칙. 처리기가 Record를 Reject하므로 다시 만든 값의 Put이 저장되고(Conflict 없음)
    // 다음 쿠킹은 적중이다.
    enum class Broken {
        Header,
        MissingValue,
        Chain,
        Truncated,
        Format
    };
    const Broken broken[] = {Broken::Header, Broken::MissingValue, Broken::Chain, Broken::Truncated,
                             Broken::Format};
    for (Broken kind : broken) {
        Array<NuggetValue> values = CopyValues(scene.allocator, good);
        Array<uint8>& header = values[0].bytes;
        switch (kind) {
        case Broken::Header:
            header[0] = 9;
            break;
        case Broken::MissingValue:
            (void)values.TryPopBack();
            break;
        case Broken::Chain:
            // mip 1의 sizeX(머리 21 + 20 byte 뒤)를 바꾼다.
            header[21 + 20] = static_cast<uint8>(header[21 + 20] + 1);
            break;
        case Broken::Truncated:
            // 마지막 mip(1×1)을 값과 머리 모두에서 뺀다(mip 수 byte 17, 항목 20 byte).
            (void)values.TryPopBack();
            header[17] = static_cast<uint8>(header[17] - 1);
            REQUIRE(header.TryResize(header.Size() - 20));
            break;
        case Broken::Format:
            header[4] = static_cast<uint8>(graphics::PixelFormat::R16Unorm);
            break;
        }

        REQUIRE(nuggets.Replace(baseKey, scene.asset, values));
        CookOut out(scene.allocator);
        REQUIRE(scene.Cook(processor, out, nuggets.cache.Get()));
        CHECK(processor.Builds() == ++builds);
        CHECK(!HasWarning(out.warnings, AssetDiagnosticCode::NuggetConflict) &&
              nuggets.cache->Stats().rejected == 1);
        CHECK(out.sink.Bytes().Size() == base.sink.Bytes().Size() &&
              platform::MemoryEquals(out.sink.Bytes().Data(), base.sink.Bytes().Data(),
                                     base.sink.Bytes().Size()));
        REQUIRE(nuggets.Reopen());
        CookOut again(scene.allocator);
        REQUIRE(scene.Cook(processor, again, nuggets.cache.Get()));
        CHECK(processor.Builds() == builds);
    }
}

// 형식 대상(12.17.5 4): 프로필 id의 플랫폼과 graphicsBackends로 형식을 고른다. AutoDXT의 알파
// 판정은 너겟 적중에서도 같은 형식(머리 형식이 맞는 후보)이다. Android OpenGL은 ASTC(12.17.8 6 —
// 경고 없음)이고, 비압축 BGRA8 설정(VectorDisplacementmap)은 BGRA8 보장이 없어 RGBA8 대체 +
// PlatformFormatFallback(setting = 바꾸기 전 형식 번호)이다 — 적중·미적중 경고가 같고 키가
// Windows와 다르다. 모르는 플랫폼 id·backend 이름은 InvalidSetting이다.
TEST(TextureCook_FormatTargets) {
    CookScene scene;
    REQUIRE(scene.ready);
    NuggetScene nuggets(scene.allocator, "texture-cook-targets");
    REQUIRE(nuggets.ready);
    TextureCookProcessor processor;
    Array<uint8> pixels = Pattern(scene.allocator, 8 * 8 * 4, 5);
    for (usize index = 3; index < pixels.Size(); index += 4) {
        pixels[index] = 255;
    }

    pixels[3] = 100;
    REQUIRE(
        scene.Make(1, TypeIdOf<Texture2D>(), TextureSourceFormat::RGBA8, 8, 8, 1, pixels.View()));
    const auto cookAndFormat = [&](CookOut& out) noexcept {
        if (!scene.Cook(processor, out, nuggets.cache.Get())) {
            return graphics::PixelFormat::Unknown;
        }

        Loaded loaded;
        if (!scene.Load(out.sink.Bytes(), loaded)) {
            return graphics::PixelFormat::Unknown;
        }

        const Texture* texture = scene.LoadedTexture(loaded);
        const auto format = texture != nullptr
                                ? graphics::PixelFormatFromNumber(texture->platformData.pixelFormat)
                                : Result<graphics::PixelFormat>::Failure(ErrorCode::NotFound);
        return format ? format.Value() : graphics::PixelFormat::Unknown;
    };
    // Windows(검증 끔): 알파가 있어 BC3 — 둘째는 적중(빌드 한 번).
    for (int round = 0; round < 2; ++round) {
        CookOut out(scene.allocator);
        CHECK(cookAndFormat(out) == graphics::PixelFormat::BC3Unorm);
        CHECK(out.warnings.IsEmpty());
    }

    CHECK(processor.Builds() == 1);
    const Sha256Digest windows = processor.LastNuggetKey();
    // Android OpenGL.
    REQUIRE(scene.profile.id.TryAssign("Android"));
    String gl(scene.allocator);
    REQUIRE(gl.TryAssign("OpenGL"));
    REQUIRE(scene.profile.graphicsBackends.TryPushBack(Move(gl)));
    for (int round = 0; round < 2; ++round) {
        CookOut out(scene.allocator);
        CHECK(cookAndFormat(out) == graphics::PixelFormat::ASTC6x6Unorm);
        CHECK(out.warnings.IsEmpty());
    }

    CHECK(processor.Builds() == 2 && !(processor.LastNuggetKey() == windows));
    REQUIRE(scene.Edit([](Texture& texture) noexcept {
        texture.compressionSettings = TextureCompressionSettings::VectorDisplacementmap;
        return true;
    }));
    for (int round = 0; round < 2; ++round) {
        CookOut out(scene.allocator);
        CHECK(cookAndFormat(out) == graphics::PixelFormat::RGBA8Unorm);
        bool fallback = false;
        for (const CookDiagnostic& warning : out.warnings) {
            fallback = fallback || (warning.code == AssetDiagnosticCode::PlatformFormatFallback &&
                                    warning.setting == graphics::PixelFormatNumber(
                                                           graphics::PixelFormat::BGRA8Unorm) &&
                                    warning.asset == scene.asset);
        }

        CHECK(fallback && out.warnings.Size() == 1);
    }

    CHECK(processor.Builds() == 3);
    // 모르는 backend 이름·플랫폼 id.
    REQUIRE(scene.profile.graphicsBackends[0].TryAssign("Glide"));
    {
        CookOut out(scene.allocator);
        CHECK(!scene.Cook(processor, out, nuggets.cache.Get()));
        CHECK(out.failure.code == AssetDiagnosticCode::InvalidSetting &&
              out.failure.error == ErrorCode::InvalidArgument);
    }

    scene.profile.graphicsBackends.Clear();
    REQUIRE(scene.profile.id.TryAssign("Other"));
    {
        CookOut out(scene.allocator);
        CHECK(!scene.Cook(processor, out, nuggets.cache.Get()));
        CHECK(out.failure.code == AssetDiagnosticCode::InvalidSetting);
    }

    REQUIRE(scene.profile.id.TryAssign("Windows"));
}

// 등록: null state InvalidArgument, Texture 파생 다섯이 이 처리기를 찾고 다른 타입은 fallback이다,
// 두 번 등록 AlreadyExists.
// BC6H·BC7 쿠킹(12.17.6 6·9): BC7 알파 원본은 BC7UnormSrgb이고 둘째 쿠킹은 적중(빌드 한 번 — 같은
// 출력 byte)이다. ForceAlpha를 켜면 키가 바뀐다(알파 판정이 프로필을 바꾼다 — 설정이 키에 있다).
// HDR_Compressed는 BC6HUfloat이다. ISPC 커널이 없는 대상(Android)은 건너뛴다.
TEST(TextureCook_IspcFormats) {
    if (IspcBcEncoderIdentity().IsEmpty()) {
        return;
    }

    CookScene scene;
    REQUIRE(scene.ready);
    NuggetScene nuggets(scene.allocator, "texture-cook-ispc");
    REQUIRE(nuggets.ready);
    TextureCookProcessor processor;
    Array<uint8> pixels = Pattern(scene.allocator, 8 * 8 * 4, 9);
    REQUIRE(
        scene.Make(1, TypeIdOf<Texture2D>(), TextureSourceFormat::RGBA8, 8, 8, 1, pixels.View()));
    REQUIRE(scene.Edit([](Texture& texture) noexcept {
        texture.srgb = 1;
        texture.compressionSettings = TextureCompressionSettings::BC7;
        return true;
    }));
    const auto cooked = [&](CookOut& out, graphics::PixelFormat& format) noexcept {
        Loaded loaded;
        if (!scene.Cook(processor, out, nuggets.cache.Get()) ||
            !scene.Load(out.sink.Bytes(), loaded)) {
            return false;
        }

        const Texture* texture = scene.LoadedTexture(loaded);
        if (texture == nullptr) {
            return false;
        }

        auto number = graphics::PixelFormatFromNumber(texture->platformData.pixelFormat);
        format = number ? number.Value() : graphics::PixelFormat::Unknown;
        return true;
    };
    CookOut first(scene.allocator);
    CookOut second(scene.allocator);
    graphics::PixelFormat format = graphics::PixelFormat::Unknown;
    REQUIRE(cooked(first, format));
    CHECK(format == graphics::PixelFormat::BC7UnormSrgb && first.warnings.IsEmpty());
    REQUIRE(cooked(second, format));
    CHECK(format == graphics::PixelFormat::BC7UnormSrgb && second.warnings.IsEmpty());
    CHECK(processor.Builds() == 1);
    CHECK(first.sink.Bytes().Size() == second.sink.Bytes().Size() &&
          memcmp(first.sink.Bytes().Data(), second.sink.Bytes().Data(),
                 first.sink.Bytes().Size()) == 0);
    const Sha256Digest bc7 = processor.LastNuggetKey();
    REQUIRE(scene.Edit([](Texture& texture) noexcept {
        texture.compressionForceAlpha = 1;
        return true;
    }));
    CookOut forced(scene.allocator);
    REQUIRE(cooked(forced, format));
    CHECK(format == graphics::PixelFormat::BC7UnormSrgb && processor.Builds() == 2 &&
          !(processor.LastNuggetKey() == bc7));
    REQUIRE(scene.Edit([](Texture& texture) noexcept {
        texture.compressionForceAlpha = 0;
        // HDR류는 sRGB를 막으므로(켜 두면 SrgbIgnored 경고) 끈다.
        texture.srgb = 0;
        texture.compressionSettings = TextureCompressionSettings::HDR_Compressed;
        return true;
    }));
    CookOut hdr(scene.allocator);
    REQUIRE(cooked(hdr, format));
    CHECK(format == graphics::PixelFormat::BC6HUfloat && hdr.warnings.IsEmpty());
}

// Android ASTC·EAC 쿠킹(12.17.8 6–9): 압축 품질이 ASTC 블록 크기를 정하고 키를 바꾼다(데스크톱은
// 품질이 키를 바꾸지 않아 적중이다), 같은 입력은 같은 byte, Alpha 설정은 EAC R11이다.
TEST(TextureCook_AndroidAstcAndEac) {
    CookScene scene;
    REQUIRE(scene.ready);
    NuggetScene nuggets(scene.allocator, "texture-cook-android");
    REQUIRE(nuggets.ready);
    TextureCookProcessor processor;
    Array<uint8> pixels = Pattern(scene.allocator, 16 * 16 * 4, 12);
    REQUIRE(
        scene.Make(1, TypeIdOf<Texture2D>(), TextureSourceFormat::RGBA8, 16, 16, 1, pixels.View()));
    REQUIRE(scene.Edit([](Texture& texture) noexcept {
        texture.srgb = 1;
        texture.compressionNoAlpha = 1;
        return true;
    }));
    const auto cooked = [&](CookOut& out, graphics::PixelFormat& format) noexcept {
        Loaded loaded;
        if (!scene.Cook(processor, out, nuggets.cache.Get()) ||
            !scene.Load(out.sink.Bytes(), loaded)) {
            return false;
        }

        const Texture* texture = scene.LoadedTexture(loaded);
        if (texture == nullptr) {
            return false;
        }

        auto number = graphics::PixelFormatFromNumber(texture->platformData.pixelFormat);
        format = number ? number.Value() : graphics::PixelFormat::Unknown;
        return true;
    };
    graphics::PixelFormat format = graphics::PixelFormat::Unknown;
    // Windows: 품질을 바꿔도 같은 키(적중).
    CookOut windows(scene.allocator);
    REQUIRE(cooked(windows, format));
    CHECK(format == graphics::PixelFormat::BC1UnormSrgb && processor.Builds() == 1);
    const Sha256Digest windowsKey = processor.LastNuggetKey();
    REQUIRE(scene.Edit([](Texture& texture) noexcept {
        texture.compressionQuality = TextureCompressionQuality::Highest;
        return true;
    }));
    CookOut windowsHighest(scene.allocator);
    REQUIRE(cooked(windowsHighest, format));
    CHECK(format == graphics::PixelFormat::BC1UnormSrgb && processor.Builds() == 1 &&
          processor.LastNuggetKey() == windowsKey);
    // Android: Highest → 4×4, Default → 6×6(키가 다르다), 같은 설정 다시는 적중·같은 byte.
    REQUIRE(scene.profile.id.TryAssign("Android"));
    CookOut highest(scene.allocator);
    REQUIRE(cooked(highest, format));
    CHECK(format == graphics::PixelFormat::ASTC4x4UnormSrgb && highest.warnings.IsEmpty());
    CHECK(processor.Builds() == 2);
    const Sha256Digest highestKey = processor.LastNuggetKey();
    REQUIRE(scene.Edit([](Texture& texture) noexcept {
        texture.compressionQuality = TextureCompressionQuality::Default;
        return true;
    }));
    CookOut first(scene.allocator);
    CookOut second(scene.allocator);
    REQUIRE(cooked(first, format));
    CHECK(format == graphics::PixelFormat::ASTC6x6UnormSrgb && processor.Builds() == 3 &&
          !(processor.LastNuggetKey() == highestKey));
    REQUIRE(cooked(second, format));
    CHECK(format == graphics::PixelFormat::ASTC6x6UnormSrgb && processor.Builds() == 3);
    CHECK(first.sink.Bytes().Size() == second.sink.Bytes().Size() &&
          memcmp(first.sink.Bytes().Data(), second.sink.Bytes().Data(),
                 first.sink.Bytes().Size()) == 0);
    // Alpha 설정 → EAC R11(선형).
    REQUIRE(scene.Edit([](Texture& texture) noexcept {
        texture.srgb = 0;
        texture.compressionSettings = TextureCompressionSettings::Alpha;
        return true;
    }));
    CookOut alpha(scene.allocator);
    REQUIRE(cooked(alpha, format));
    CHECK(format == graphics::PixelFormat::EACR11Unorm && alpha.warnings.IsEmpty());
}

// 병렬 압축 쿠킹(12.17.9 4·5): scheduler가 있어도 쿠킹 byte·너겟 키가 직렬과 같고(실행 방법은 키에
// 넣지 않는다 — 직렬 너겟을 그대로 적중), verify(다시 만들어 byte 대조)가 통과한다. Windows BC와
// Android ASTC·EAC 셋을 본다.
TEST(TextureCook_ParallelMatchesSerial) {
    CookScene scene;
    REQUIRE(scene.ready);
    WorkerScheduler workers(scene.allocator);
    WorkerSchedulerConfig config;
    config.workerCount = 4;
    REQUIRE(workers.Start(config));
    TextureCookProcessor processor;
    Array<uint8> pixels = Pattern(scene.allocator, 64 * 64 * 4, 21);
    REQUIRE(
        scene.Make(1, TypeIdOf<Texture2D>(), TextureSourceFormat::RGBA8, 64, 64, 1, pixels.View()));
    struct Case {
        const char* profile;
        TextureCompressionSettings compression;
    };
    const Case cases[] = {{"Windows", TextureCompressionSettings::Default},
                          {"Android", TextureCompressionSettings::Default},
                          {"Android", TextureCompressionSettings::Alpha}};
    usize index = 0;
    for (const Case& one : cases) {
        REQUIRE(scene.profile.id.TryAssign(one.profile));
        const TextureCompressionSettings compression = one.compression;
        REQUIRE(scene.Edit([compression](Texture& texture) noexcept {
            texture.compressionSettings = compression;
            texture.srgb = compression == TextureCompressionSettings::Alpha ? 0 : 1;
            return true;
        }));
        char tag[48];
        snprintf(tag, sizeof(tag), "texture-cook-parallel-%u", static_cast<unsigned>(index++));
        NuggetScene nuggets(scene.allocator, tag);
        REQUIRE(nuggets.ready);
        scene.scheduler = nullptr;
        CookOut serial(scene.allocator);
        REQUIRE(scene.Cook(processor, serial, nuggets.cache.Get()));
        const Sha256Digest serialKey = processor.LastNuggetKey();
        const uint64 builds = processor.Builds();
        // 새 캐시에서 병렬로 다시 만든다(같은 키·같은 byte).
        char freshTag[48];
        snprintf(freshTag, sizeof(freshTag), "texture-cook-parallel-fresh-%u",
                 static_cast<unsigned>(index));
        NuggetScene fresh(scene.allocator, freshTag);
        REQUIRE(fresh.ready);
        scene.scheduler = &workers;
        CookOut parallel(scene.allocator);
        REQUIRE(scene.Cook(processor, parallel, fresh.cache.Get()));
        CHECK(processor.Builds() == builds + 1);
        CHECK(processor.LastNuggetKey() == serialKey);
        CHECK(parallel.sink.Bytes().Size() == serial.sink.Bytes().Size() &&
              memcmp(parallel.sink.Bytes().Data(), serial.sink.Bytes().Data(),
                     serial.sink.Bytes().Size()) == 0);
        // verify: 직렬로 만든 너겟을 병렬로 다시 만들어 byte 대조한다.
        CookOut verified(scene.allocator);
        REQUIRE(scene.Cook(processor, verified, nuggets.cache.Get(), true));
        CHECK(verified.failure.code == AssetDiagnosticCode::None);
        scene.scheduler = nullptr;
    }

    REQUIRE(workers.Shutdown());
}

TEST(TextureCook_Registration) {
    CookScene scene;
    REQUIRE(scene.ready);
    TextureCookProcessor processor;
    DefaultCookProcessor fallback;
    CookProcessorTable table(scene.allocator, scene.types, fallback);
    CHECK(RegisterTextureCookProcessors(nullptr, table).Error() == ErrorCode::InvalidArgument);
    REQUIRE(RegisterTextureCookProcessors(&processor, table));
    CHECK(RegisterTextureCookProcessors(&processor, table).Error() == ErrorCode::AlreadyExists);
    const TypeId textures[] = {TypeIdOf<Texture2D>(), TypeIdOf<TextureCube>(),
                               TypeIdOf<Texture2DArray>(), TypeIdOf<TextureCubeArray>(),
                               TypeIdOf<VolumeTexture>()};
    for (TypeId type : textures) {
        auto found = table.Find(type);
        CHECK(found && found.Value() == &processor);
    }

    auto other = table.Find(TypeIdOf<Package>());
    CHECK(other && other.Value() == &fallback);
    CHECK(processor.Identity().processor == TextureCookProcessorId.processor &&
          processor.Identity().version == TextureCookProcessorId.version);
}

} // namespace texture_cook_tests_file
