#pragma once

#include "../Private/TextureImportInternal.h"

#include <Compression/Compression.h>
#include <Core/Encoding/Crc32.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Memory/SystemAllocator.h>
#include <Meta/GeneratedType.h>
#include <Object/ModuleRegistration.h>
#include <Object/ObjectRegistry.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <Texture/Texture.generated.h>
#include <TextureTools/TextureImport.h>

// TextureTools 시험 지원이다(Docs/Design/AssetSystem 12.12절 — 14.1.97절 대조표): 텍스처 타입 등록,
// 가짜 임포트 문맥(ObjectRegistry 위 — 출력·bulk·경고·실패를 모은다), PNG·TGA·DDS 원본 생성기.
namespace rocks::texture_import_test {

// 텍스처 타입을 등록한 TypeRegistry와 객체 Registry다. 객체 Registry 할당자(임포터·재구성 hook이
// 쓰는 ObjectAllocator)는 objectAllocator를 거친다 — 할당 실패 전수 시험만 실패를 켠다.
struct TextureEnv {
    TextureEnv() noexcept
        : objectAllocator(allocator), types(allocator), registration(types, allocator),
          ready(RegisterTextureTypes(registration).IsOk()),
          objects(objectAllocator, types, ContextId{0x54584954ull, 1}) {
    }

    // 객체가 남아 있으면 타입 해제가 Busy이므로 객체 Registry를 먼저 닫는다.
    ~TextureEnv() noexcept {
        CHECK(objects.Shutdown(objects.OwnerAccess()));
        CHECK(registration.Unregister());
    }

    SystemAllocator allocator;
    test::NFailAllocator objectAllocator;
    TypeRegistry types;
    ModuleRegistration registration;
    bool ready = false;
    ObjectRegistry objects;
};

// 경고 하나다.
struct SeenWarning {
    AssetDiagnosticCode code = AssetDiagnosticCode::None;
    uint64 detail = 0;
};

// 임포터 시험 문맥이다. 새 임포트는 Output이 새 객체를, 재임포트는 existing을 준다.
class FakeImportContext final : public AssetImportContext {
public:
    FakeImportContext(TextureEnv& env, Span<const AssetImportSource> sources, const void* options,
                      ObjectHandle existing = {}) noexcept
        : bulk(env.allocator), warnings(env.allocator), env_(&env),
          access_(env.objects.OwnerAccess()), sources_(sources), options_(options),
          existing_(existing), roots_(env.allocator) {
    }

    Span<const AssetImportSource> Sources() const noexcept override {
        return sources_;
    }

    const void* Options() const noexcept override {
        return options_;
    }

    bool IsReimport() const noexcept override {
        return !existing_.IsNull();
    }

    ObjectRegistry& Registry() noexcept override {
        return env_->objects;
    }

    const ObjectAccessContext& Access() const noexcept override {
        return access_;
    }

    Result<ObjectHandle> Output(StringView key, TypeId type) noexcept override {
        if (!key.IsEmpty() || !output.IsNull()) {
            return Result<ObjectHandle>::Failure(ErrorCode::InvalidArgument);
        }

        outputType = type;
        if (!existing_.IsNull()) {
            output = existing_;
            return Result<ObjectHandle>::Success(output);
        }

        auto made = env_->objects.NewObject<Texture>(access_, type);
        if (!made) {
            return Result<ObjectHandle>::Failure(made.Error());
        }

        output = made.Value().Handle();
        if (auto pushed = roots_.TryPushBack(rocks::Move(made.Value())); !pushed) {
            return Result<ObjectHandle>::Failure(pushed.Error());
        }

        return Result<ObjectHandle>::Success(output);
    }

    Result<BulkDataRef> AddBulk(ObjectHandle target, Array<uint8>&& raw) noexcept override {
        if (target != output) {
            return Result<BulkDataRef>::Failure(ErrorCode::InvalidArgument);
        }

        BulkDataRef ref;
        ref.bulkId = 1;
        ref.rawBytes = raw.Size();
        ref.sha256 = Sha256::Hash(raw.View());
        bulk = rocks::Move(raw);
        return Result<BulkDataRef>::Success(ref);
    }

    Result<void> Warn(AssetDiagnosticCode code, uint64 detail) noexcept override {
        return warnings.TryPushBack(SeenWarning{code, detail});
    }

    Result<void> Fail(AssetDiagnosticCode code, ErrorCode error, uint64 detail) noexcept override {
        failCode = code;
        failError = error;
        failDetail = detail;
        return Result<void>::Failure(error);
    }

    Result<void> AddTag(StringView, StringView) noexcept override {
        return Result<void>::Success();
    }

    Span<const ImportTag> PreviousTags() const noexcept override {
        return {};
    }

    Result<ObjectHandle> OpenAsset(AssetId) noexcept override {
        return Result<ObjectHandle>::Failure(ErrorCode::NotFound);
    }

    usize GroupSize() const noexcept override {
        return 1;
    }

    usize GroupIndex() const noexcept override {
        return 0;
    }

    AssetId MemberAsset(usize) const noexcept override {
        return {};
    }

    // 출력 텍스처다(없으면 null).
    Texture* Imported() noexcept {
        auto borrowed = env_->objects.Resolve<Texture>(access_, output);
        if (!borrowed) {
            return nullptr;
        }

        auto value = borrowed.Value().Get();
        return value ? value.Value() : nullptr;
    }

    ObjectHandle output{};
    TypeId outputType{};
    Array<uint8> bulk;
    Array<SeenWarning> warnings;
    AssetDiagnosticCode failCode = AssetDiagnosticCode::None;
    ErrorCode failError = ErrorCode::None;
    uint64 failDetail = 0;

private:
    TextureEnv* env_;
    ObjectAccessContext access_;
    Span<const AssetImportSource> sources_;
    const void* options_;
    ObjectHandle existing_;
    Array<RootRef<Texture>> roots_;
};

// 파일 byte 생성기다.
struct FileBytes {
    explicit FileBytes(Allocator& allocator) noexcept : data(allocator) {
    }

    void U8(uint32 value) noexcept {
        CHECK(data.TryPushBack(static_cast<uint8>(value)));
    }

    void Le16(uint32 value) noexcept {
        U8(value);
        U8(value >> 8);
    }

    void Le32(uint32 value) noexcept {
        Le16(value);
        Le16(value >> 16);
    }

    void Be32(uint32 value) noexcept {
        U8(value >> 24);
        U8(value >> 16);
        U8(value >> 8);
        U8(value);
    }

    void Append(Span<const uint8> bytes) noexcept {
        CHECK(data.TryAppend(bytes));
    }

    Array<uint8> data;
};

// PNG 색 형식이다(IHDR color type).
enum class PngColor : uint8 {
    Gray = 0,
    Rgb = 2,
    GrayAlpha = 4,
    Rgba = 6,
};

inline void PngChunk(FileBytes& file, const char* type, Span<const uint8> data) noexcept {
    file.Be32(static_cast<uint32>(data.Size()));
    const usize start = file.data.Size();
    for (const char* c = type; *c != '\0'; ++c) {
        file.U8(static_cast<uint8>(*c));
    }

    file.Append(data);
    file.Be32(Crc32(file.data.View().Slice(start, data.Size() + 4)));
}

// PNG 하나를 만든다(texels는 행 순서의 채널 값 — 16비트는 값 그대로 넘기고 big-endian으로 쓴다,
// 행마다 필터 0). gamma가 0이 아니면 gAMA chunk(그 값 — 100000 = 1.0)를 넣는다.
inline Array<uint8> MakePng(Allocator& allocator, uint32 width, uint32 height, PngColor color,
                            uint32 depth, Span<const uint32> texels, uint32 gamma = 0) noexcept {
    const uint32 channels = color == PngColor::Gray        ? 1
                            : color == PngColor::GrayAlpha ? 2
                            : color == PngColor::Rgb       ? 3
                                                           : 4;
    FileBytes file(allocator);
    const uint8 signature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    file.Append(signature);
    FileBytes header(allocator);
    header.Be32(width);
    header.Be32(height);
    header.U8(depth);
    header.U8(static_cast<uint32>(color));
    header.U8(0);
    header.U8(0);
    header.U8(0);
    PngChunk(file, "IHDR", header.data.View());
    if (gamma != 0) {
        FileBytes chunk(allocator);
        chunk.Be32(gamma);
        PngChunk(file, "gAMA", chunk.data.View());
    }

    FileBytes raw(allocator);
    CHECK(texels.Size() == usize{width} * height * channels);
    for (uint32 y = 0; y < height; ++y) {
        raw.U8(0);
        for (uint32 x = 0; x < width * channels; ++x) {
            const uint32 value = texels[usize{y} * width * channels + x];
            if (depth == 16) {
                raw.U8(value >> 8);
            }

            raw.U8(value);
        }
    }

    Array<uint8> compressed(allocator);
    auto settings = CompressionSettings::TryDefault(CompressionFormat::Zlib);
    CHECK(settings);
    if (settings) {
        CHECK(TryCompressAppend(settings.Value(), raw.data.View(), compressed));
    }

    PngChunk(file, "IDAT", compressed.View());
    PngChunk(file, "IEND", {});
    return rocks::Move(file.data);
}

// 8비트 TGA 하나를 만든다: type 3(회색, 색표 없음) 또는 type 1(색표 — palette는 BGR 3 byte씩).
inline Array<uint8> MakeTga8(Allocator& allocator, uint32 type, uint32 width, uint32 height,
                             Span<const uint8> indices, Span<const uint8> palette = {}) noexcept {
    FileBytes file(allocator);
    const bool mapped = type == 1;
    file.U8(0);
    file.U8(mapped ? 1 : 0);
    file.U8(type);
    file.Le16(0);
    file.Le16(mapped ? static_cast<uint32>(palette.Size() / 3) : 0);
    file.U8(mapped ? 24 : 0);
    file.Le16(0);
    file.Le16(0);
    file.Le16(width);
    file.Le16(height);
    file.U8(8);
    // 위에서 아래 순서(descriptor bit 5).
    file.U8(0x20);
    file.Append(palette);
    file.Append(indices);
    return rocks::Move(file.data);
}

// DXGI 형식·resource dimension 번호다(DDS DX10 머리).
inline constexpr uint32 DxgiRgba8 = 28;
inline constexpr uint32 DxgiRgba8Srgb = 29;
inline constexpr uint32 DxgiRgba16F = 10;
inline constexpr uint32 DdsTexture2D = 3;
inline constexpr uint32 DdsTexture3D = 4;
inline constexpr uint32 DdsMiscCube = 4;

// DX10 DDS 하나를 만든다(mip 1개 — data는 slice를 이은 byte).
inline Array<uint8> MakeDds(Allocator& allocator, uint32 width, uint32 height, uint32 depth,
                            uint32 dxgi, uint32 resourceDimension, uint32 misc, uint32 arraySize,
                            Span<const uint8> data) noexcept {
    FileBytes file(allocator);
    file.Le32(0x20534444u);
    file.Le32(124);
    file.Le32(0);
    file.Le32(height);
    file.Le32(width);
    file.Le32(0);
    file.Le32(depth);
    file.Le32(1);
    for (uint32 index = 0; index < 11; ++index) {
        file.Le32(0);
    }

    file.Le32(32);
    file.Le32(0x4);
    file.Le32(0x30315844u);
    for (uint32 index = 0; index < 5; ++index) {
        file.Le32(0);
    }

    file.Le32(0);
    file.Le32(resourceDimension == DdsTexture3D ? 0x200000u : 0);
    for (uint32 index = 0; index < 3; ++index) {
        file.Le32(0);
    }

    file.Le32(dxgi);
    file.Le32(resourceDimension);
    file.Le32(misc);
    file.Le32(arraySize);
    file.Le32(0);
    file.Append(data);
    return rocks::Move(file.data);
}

// RGBA8 texel 목록을 채널 목록으로 편다(r, g, b, a 0–255).
struct Rgba {
    uint32 r = 0;
    uint32 g = 0;
    uint32 b = 0;
    uint32 a = 255;
};

inline Array<uint32> Channels(Allocator& allocator, Span<const Rgba> texels) noexcept {
    Array<uint32> out(allocator);
    for (const Rgba& texel : texels) {
        CHECK(out.TryPushBack(texel.r) && out.TryPushBack(texel.g) && out.TryPushBack(texel.b) &&
              out.TryPushBack(texel.a));
    }

    return out;
}

// 단색 RGBA8 PNG(width × height)다.
inline Array<uint8> SolidPng(Allocator& allocator, uint32 width, uint32 height,
                             Rgba color) noexcept {
    Array<uint32> texels(allocator);
    for (uint32 index = 0; index < width * height; ++index) {
        CHECK(texels.TryPushBack(color.r) && texels.TryPushBack(color.g) &&
              texels.TryPushBack(color.b) && texels.TryPushBack(color.a));
    }

    return MakePng(allocator, width, height, PngColor::Rgba, 8, texels.View());
}

inline Span<const uint8> View(const Array<uint8>& bytes) noexcept {
    return bytes.View();
}

} // namespace rocks::texture_import_test
