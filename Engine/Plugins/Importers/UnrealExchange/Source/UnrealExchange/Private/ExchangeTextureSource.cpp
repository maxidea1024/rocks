#include <ImageCodec/ImageDecode.h>
#include <Platform/Memory.h>
#include <UnrealExchange/ExchangeTextureSource.h>

// 교환 텍스처 원본 변환이다(Docs/Design/UnrealExchange 4.6절 2번, 4.2절 형식 대응).

namespace rocks {

namespace exchange_texture_source {

// UE 원본 형식 하나를 Rocks 형식으로 옮기는 방법이다.
enum class Conversion : uint8 {
    Copy,
    // BGRA8 → RGBA8(texel마다 B·R 교환).
    SwapBlueRed,
    // BGRE8 → RGBA32F(UE FColor::FromRGBE).
    DecodeRgbe
};

// 4.2절 표 한 행과 그 blob 배치(4.5절 2번)다.
struct FormatEntry {
    StringView name;
    ExchangeBlobLayout layout;
    uint32 components;
    TextureSourceFormat format;
    Conversion conversion;
};

constexpr FormatEntry Formats[] = {
    {"G8", ExchangeBlobLayout::U8, 1, TextureSourceFormat::G8, Conversion::Copy},
    {"BGRA8", ExchangeBlobLayout::U8, 4, TextureSourceFormat::RGBA8, Conversion::SwapBlueRed},
    {"BGRE8", ExchangeBlobLayout::U8, 4, TextureSourceFormat::RGBA32F, Conversion::DecodeRgbe},
    {"RGBA16", ExchangeBlobLayout::U16, 4, TextureSourceFormat::RGBA16, Conversion::Copy},
    {"RGBA16F", ExchangeBlobLayout::F16, 4, TextureSourceFormat::RGBA16F, Conversion::Copy},
    {"G16", ExchangeBlobLayout::U16, 1, TextureSourceFormat::G16, Conversion::Copy},
    {"RGBA32F", ExchangeBlobLayout::F32, 4, TextureSourceFormat::RGBA32F, Conversion::Copy},
    {"R16F", ExchangeBlobLayout::F16, 1, TextureSourceFormat::R16F, Conversion::Copy},
    {"R32F", ExchangeBlobLayout::F32, 1, TextureSourceFormat::R32F, Conversion::Copy},
};

// layers[].gammaSpace 이름(ExchangeGammaSpace 순)이다.
constexpr StringView GammaNames[] = {"Linear", "Pow22", "sRGB"};

// 지원하는 층·블록 수(4.1절 — 둘 이상은 UnsupportedSourceLayout)와 큐브 면 수다.
constexpr uint64 SupportedLayers = 1;
constexpr uint64 SupportedBlocks = 1;
constexpr uint32 CubeFaces = 6;

// RGBE 해제 상수다(UE FColor::FromRGBE — 2^(e − (128 + 8)), 성분 + 0.5).
constexpr int32 RgbeExponentBias = 136;
constexpr float RgbeComponentBias = 0.5f;
// texel 하나의 byte 수다(BGRA8·BGRE8 u8×4, RGBA32F f32×4).
constexpr usize Rgba8TexelBytes = 4;
constexpr usize Rgba32FTexelBytes = 16;
// double 지수 bias와 가수 비트 수다(2의 거듭제곱을 비트로 만든다).
constexpr int64 DoubleExponentBias = 1023;
constexpr uint32 DoubleMantissaBits = 52;

Result<void> Reject(ExchangeTextureFailure& failure, AssetDiagnosticCode code,
                    ErrorCode error) noexcept {
    failure.code = code;
    failure.error = error;
    return Result<void>::Failure(error);
}

Result<void> Corrupt(ExchangeTextureFailure& failure) noexcept {
    return Reject(failure, AssetDiagnosticCode::CorruptBundle, ErrorCode::CorruptData);
}

bool ReadUnsigned(JsonValue object, StringView key, uint64& out) noexcept {
    const JsonValue value = object.Find(key);
    if (!value.IsValid() || !value.IsNumber()) {
        return false;
    }

    auto number = value.TryGetUInt64();
    if (!number) {
        return false;
    }

    out = number.Value();
    return true;
}

bool ReadString(JsonValue object, StringView key, StringView& out) noexcept {
    const JsonValue value = object.Find(key);
    if (!value.IsValid() || !value.IsString()) {
        return false;
    }

    out = value.GetString();
    return true;
}

const FormatEntry* FindFormat(StringView name) noexcept {
    for (const FormatEntry& entry : Formats) {
        if (entry.name == name) {
            return &entry;
        }
    }

    return nullptr;
}

// 클래스의 slice 수 규칙이다(4.6절 2번 5).
bool SlicesMatchClass(ExchangeClass type, uint64 slices) noexcept {
    switch (type) {
    case ExchangeClass::Texture2D:
        return slices == 1;
    case ExchangeClass::TextureCube:
        return slices == CubeFaces;
    case ExchangeClass::TextureCubeArray:
        return slices >= CubeFaces && slices % CubeFaces == 0;
    case ExchangeClass::Texture2DArray:
    case ExchangeClass::VolumeTexture:
        return slices >= 1;
    default:
        return false;
    }
}

// mip 0 층 0의 blob 참조를 asset.blobs 번호로 찾는다(배치·성분·count가 형식과 맞아야 한다).
bool FindPixelBlob(const ExchangeAsset& asset, JsonValue reference, const FormatEntry& format,
                   uint64 texels, usize& index) noexcept {
    StringView hex;
    StringView layout;
    uint64 components = 0;
    uint64 count = 0;
    if (!reference.IsObject() || !ReadString(reference, "blob", hex) ||
        !ReadString(reference, "layout", layout) ||
        !ReadUnsigned(reference, "components", components) ||
        !ReadUnsigned(reference, "count", count)) {
        return false;
    }

    if (layout != StringView(ExchangeBlobLayoutName(format.layout)) ||
        components != format.components || count != texels) {
        return false;
    }

    for (index = 0; index < asset.blobs.Size(); ++index) {
        const ExchangeBlobRef& blob = asset.blobs[index];
        char text[Sha256Digest::HexBytes + 1];
        blob.digest.ToHex(text);
        if (StringView(text, Sha256Digest::HexBytes) == hex && blob.layout == format.layout &&
            blob.components == format.components && blob.count == texels) {
            return true;
        }
    }

    return false;
}

// 2^exponent를 float로 만든다(UE ldexpf(1, e) — 지수 0은 따로 처리하므로 e는 [−135, 119]이고 float
// 정상·비정상 범위 안에서 정확하다).
float PowerOfTwo(int32 exponent) noexcept {
    const uint64 bits = static_cast<uint64>(exponent + DoubleExponentBias) << DoubleMantissaBits;
    return static_cast<float>(platform::BitCast<double>(bits));
}

void DecodeRgbe(Span<const uint8> source, uint8* out) noexcept {
    const usize texels = source.Size() / Rgba8TexelBytes;
    for (usize texel = 0; texel < texels; ++texel) {
        const uint8* in = source.Data() + texel * Rgba8TexelBytes;
        // UE 저장 순서 B·G·R·E다.
        const uint8 exponent = in[3];
        float rgba[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        if (exponent != 0) {
            const float scale = PowerOfTwo(static_cast<int32>(exponent) - RgbeExponentBias);
            rgba[0] = (static_cast<float>(in[2]) + RgbeComponentBias) * scale;
            rgba[1] = (static_cast<float>(in[1]) + RgbeComponentBias) * scale;
            rgba[2] = (static_cast<float>(in[0]) + RgbeComponentBias) * scale;
        }

        platform::MemoryCopy(out + texel * Rgba32FTexelBytes, rgba, sizeof(rgba));
    }
}

void SwapBlueRed(Span<const uint8> source, uint8* out) noexcept {
    const usize texels = source.Size() / Rgba8TexelBytes;
    for (usize texel = 0; texel < texels; ++texel) {
        const uint8* in = source.Data() + texel * Rgba8TexelBytes;
        uint8* to = out + texel * Rgba8TexelBytes;
        to[0] = in[2];
        to[1] = in[1];
        to[2] = in[0];
        to[3] = in[3];
    }
}

Result<void> ConvertInto(const ExchangeAsset& asset, Span<const Span<const uint8>> blobBytes,
                         ExchangeTextureSource& out, ExchangeTextureFailure& failure) noexcept {
    // 텍스처 클래스가 아니면 호출 계약 위반이다.
    if (!IsExchangeTextureClass(asset.exchangeClass)) {
        return Reject(failure, AssetDiagnosticCode::InvalidSource, ErrorCode::InvalidArgument);
    }

    const JsonValue source = asset.data.IsObject() ? asset.data.Find("source") : JsonValue();
    uint64 layers = 0;
    uint64 blocks = 0;
    const JsonValue longLat = source.IsObject() ? source.Find("longLat") : JsonValue();
    if (!source.IsObject() || !ReadUnsigned(source, "numLayers", layers) ||
        !ReadUnsigned(source, "numBlocks", blocks) || !longLat.IsValid() || !longLat.IsBool()) {
        return Corrupt(failure);
    }

    // 2. 다층·UDIM·위도경도 큐브(4.1절).
    if (layers != SupportedLayers || blocks != SupportedBlocks || longLat.GetBool()) {
        return Reject(failure, AssetDiagnosticCode::UnsupportedSourceLayout,
                      ErrorCode::Unsupported);
    }

    // 3. 층 0 형식과 감마.
    StringView formatName;
    const JsonValue layerList = source.Find("layers");
    if (!ReadString(source, "format", formatName) || !layerList.IsArray() ||
        layerList.Size() != layers) {
        return Corrupt(failure);
    }

    const JsonValue layer = layerList.At(0);
    StringView layerFormat;
    StringView gammaName;
    const FormatEntry* format = FindFormat(formatName);
    if (format == nullptr || !layer.IsObject() || !ReadString(layer, "format", layerFormat) ||
        layerFormat != formatName || !ReadString(layer, "gammaSpace", gammaName)) {
        return Corrupt(failure);
    }

    bool gammaKnown = false;
    for (usize index = 0; index < sizeof(GammaNames) / sizeof(GammaNames[0]); ++index) {
        if (GammaNames[index] == gammaName) {
            out.gamma = static_cast<ExchangeGammaSpace>(index);
            gammaKnown = true;
        }
    }

    if (!gammaKnown) {
        return Corrupt(failure);
    }

    // 4. 블록 0과 첫 mip.
    const JsonValue blockList = source.Find("blocks");
    if (!blockList.IsArray() || blockList.Size() != blocks) {
        return Corrupt(failure);
    }

    const JsonValue block = blockList.At(0);
    uint64 blockX = 0;
    uint64 blockY = 0;
    uint64 sizeX = 0;
    uint64 sizeY = 0;
    uint64 slices = 0;
    uint64 mipCount = 0;
    if (!block.IsObject() || !ReadUnsigned(block, "blockX", blockX) ||
        !ReadUnsigned(block, "blockY", blockY) || !ReadUnsigned(block, "sizeX", sizeX) ||
        !ReadUnsigned(block, "sizeY", sizeY) || !ReadUnsigned(block, "numSlices", slices) ||
        !ReadUnsigned(block, "numMips", mipCount) || blockX != 0 || blockY != 0 || sizeX == 0 ||
        sizeY == 0 || slices == 0 || mipCount == 0) {
        return Corrupt(failure);
    }

    const JsonValue mips = block.Find("mips");
    if (!mips.IsArray() || mips.Size() != mipCount) {
        return Corrupt(failure);
    }

    const JsonValue mip = mips.At(0);
    uint64 mipX = 0;
    uint64 mipY = 0;
    uint64 mipSlices = 0;
    if (!mip.IsObject() || !ReadUnsigned(mip, "sizeX", mipX) || !ReadUnsigned(mip, "sizeY", mipY) ||
        !ReadUnsigned(mip, "numSlices", mipSlices) || mipX != sizeX || mipY != sizeY ||
        mipSlices != slices) {
        return Corrupt(failure);
    }

    // 5. 클래스 slice 규칙과 크기 상한(파일 임포트와 같다).
    if (!SlicesMatchClass(asset.exchangeClass, slices)) {
        return Reject(failure, AssetDiagnosticCode::InvalidSource, ErrorCode::CorruptData);
    }

    const uint32 texelBytes = TextureSourceTexelBytes(format->format);
    if (sizeX > image::ImageDecodeLimits::DefaultMaxDimension ||
        sizeY > image::ImageDecodeLimits::DefaultMaxDimension ||
        slices > image::ImageDecodeLimits::DefaultMaxSlices ||
        sizeX * sizeY * slices > image::ImageDecodeLimits::DefaultMaxPixelBytes / texelBytes) {
        return Reject(failure, AssetDiagnosticCode::InvalidSource, ErrorCode::CapacityExceeded);
    }

    // 6. 첫 mip 층 0의 blob.
    const uint64 texels = sizeX * sizeY * slices;
    const JsonValue pixels = mip.Find("pixels");
    usize blob = 0;
    if (!pixels.IsArray() || pixels.Size() != layers ||
        !FindPixelBlob(asset, pixels.At(0), *format, texels, blob)) {
        return Corrupt(failure);
    }

    // blobBytes는 asset.blobs와 같은 순서·수여야 한다(호출 계약).
    if (blob >= blobBytes.Size()) {
        return Reject(failure, AssetDiagnosticCode::InvalidSource, ErrorCode::InvalidArgument);
    }

    const Span<const uint8> bytes = blobBytes[blob];
    const uint64 expected = texels * format->components * ExchangeBlobElementBytes(format->layout);
    if (static_cast<uint64>(bytes.Size()) != expected) {
        return Corrupt(failure);
    }

    // 7. 형식 대응.
    if (auto resized = out.pixels.TryResize(static_cast<usize>(texels * texelBytes)); !resized) {
        return resized;
    }

    switch (format->conversion) {
    case Conversion::Copy:
        platform::MemoryCopy(out.pixels.Data(), bytes.Data(), bytes.Size());
        break;
    case Conversion::SwapBlueRed:
        SwapBlueRed(bytes, out.pixels.Data());
        break;
    case Conversion::DecodeRgbe:
        DecodeRgbe(bytes, out.pixels.Data());
        break;
    }

    out.format = format->format;
    out.sizeX = static_cast<uint32>(sizeX);
    out.sizeY = static_cast<uint32>(sizeY);
    out.numSlices = static_cast<uint32>(slices);
    return Result<void>::Success();
}

} // namespace exchange_texture_source

Result<void> ConvertExchangeTextureSource(const ExchangeAsset& asset,
                                          Span<const Span<const uint8>> blobBytes,
                                          ExchangeTextureSource& out,
                                          ExchangeTextureFailure& failure) noexcept {
    using namespace exchange_texture_source;
    out.format = TextureSourceFormat::Invalid;
    out.sizeX = 0;
    out.sizeY = 0;
    out.numSlices = 0;
    out.gamma = ExchangeGammaSpace::Linear;
    out.pixels.Clear();
    failure = ExchangeTextureFailure();
    auto converted = ConvertInto(asset, blobBytes, out, failure);
    if (!converted) {
        out.format = TextureSourceFormat::Invalid;
        out.sizeX = 0;
        out.sizeY = 0;
        out.numSlices = 0;
        out.gamma = ExchangeGammaSpace::Linear;
        out.pixels.Clear();
    }

    return converted;
}

} // namespace rocks
