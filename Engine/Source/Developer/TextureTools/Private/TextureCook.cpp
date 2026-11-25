#include <TextureTools/TextureCook.h>

#include <AssetNugget/NuggetReflected.h>
#include <AssetNugget/NuggetScc.h>
#include <AssetRuntime/BulkStore.h>
#include <Base/NumericLimits.h>
#include <Core/Encoding/ByteReader.h>
#include <Core/Encoding/ByteWriter.h>
#include <Core/Encoding/Sha256.h>
#include <Meta/GeneratedType.h>
#include <Object/ObjectRegistry.h>
#include <Platform/Memory.h>
#include <Texture/Texture.generated.h>
#include <Texture/TextureBuildSettings.generated.h>
#include <Texture/TextureSettingsCheck.h>

// 텍스처 쿠킹 처리기다(Docs/Design/AssetSystem 12.14절 — A5j1).
namespace rocks {
namespace {

using reflection_text::AssignmentTarget;

// 너겟 머리 고정부(버전·PixelFormat·srgb·swizzle 넷·numSlices·mip 수)와 mip 항목 하나의 byte다.
constexpr usize CookHeaderFixedBytes = 4 + 4 + 1 + 4 + 4 + 4;
constexpr usize CookHeaderMipBytes = 4 * 3 + 8;
constexpr uint32 CookSwizzleCount = 4;

// 쿠킹이 새로 만든 bulk(mip·꼬리)의 id는 uint32 최댓값부터 내려간다(12.14절 9번 — 에디터 저장소의
// 저장하지 않은 bulk는 최상위 bit id를 1부터 올려 쓰므로 겹치지 않는다).
constexpr uint32 CookBulkIdFirst = NumericLimits<uint32>::Max();

bool CookIsPowerOfTwo(uint32 value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

uint32 CookHalf(uint32 value) noexcept {
    return value > 1 ? value >> 1 : 1u;
}

// 새 bulk를 먼저, 그 밖 ref는 에디터 bulk 저장소로 보내는 연쇄 원본이다(12.14절 9번). byte는
// 빌림이다 (쓰기가 끝날 때까지 산다).
class CookChainedBulkSource final : public PackageBulkSource {
public:
    CookChainedBulkSource(Allocator& allocator, PackageBulkSource* next) noexcept
        : entries_(allocator), next_(next) {
    }

    Result<BulkDataRef> Add(Span<const uint8> bytes) noexcept {
        // 내려가는 id가 저장하지 않은 bulk의 최상위 bit 범위로 내려오지 않게 한다(실제로는 mip 수 +
        // 1개뿐이다).
        if (entries_.Size() >= CookBulkIdFirst - UnsavedBulkIdBit) {
            return Result<BulkDataRef>::Failure(ErrorCode::CapacityExceeded);
        }

        if (auto pushed = entries_.TryPushBack(bytes); !pushed) {
            return Result<BulkDataRef>::Failure(pushed.Error());
        }

        BulkDataRef ref;
        ref.bulkId = CookBulkIdFirst - static_cast<uint32>(entries_.Size() - 1);
        ref.rawBytes = bytes.Size();
        ref.sha256 = Sha256::Hash(bytes);
        return Result<BulkDataRef>::Success(ref);
    }

    Result<Span<const uint8>> ReadBulk(const BulkDataRef& ref) noexcept override {
        if (const Span<const uint8>* own = Find(ref.bulkId)) {
            return Result<Span<const uint8>>::Success(*own);
        }

        if (next_ == nullptr) {
            return Result<Span<const uint8>>::Failure(ErrorCode::NotFound);
        }

        return next_->ReadBulk(ref);
    }

    // 새 bulk는 저장 byte가 없다(writer가 raw를 부호화한다).
    Result<PackageStoredBulk> ReadStoredBulk(const BulkDataRef& ref) noexcept override {
        if (Find(ref.bulkId) != nullptr || next_ == nullptr) {
            return Result<PackageStoredBulk>::Failure(ErrorCode::NotFound);
        }

        return next_->ReadStoredBulk(ref);
    }

private:
    const Span<const uint8>* Find(uint32 bulkId) const noexcept {
        if (bulkId == 0) {
            return nullptr;
        }

        const uint32 offset = CookBulkIdFirst - bulkId;
        return offset < entries_.Size() ? &entries_[offset] : nullptr;
    }

    Array<Span<const uint8>> entries_;
    PackageBulkSource* next_;
};

// 실패 진단을 채우고 error로 실패한다.
Result<void> CookFail(const CookContext& context, AssetDiagnosticCode code, ErrorCode error,
                      uint64 detail = 0) noexcept {
    if (context.failure != nullptr) {
        *context.failure = AssetDiagnostic{};
        context.failure->code = code;
        context.failure->error = error;
        context.failure->asset = context.entry != nullptr ? context.entry->id : AssetId{};
        context.failure->detail = detail;
    }

    return Result<void>::Failure(error);
}

// 처리기 경고를 문맥에 둔다(없으면 버린다). 할당 실패는 OutOfMemory다.
Result<void> CookWarn(const CookContext& context, AssetDiagnosticCode code) noexcept {
    if (context.warnings == nullptr) {
        return Result<void>::Success();
    }

    const AssetId asset = context.entry != nullptr ? context.entry->id : AssetId{};
    return context.warnings->TryPushBack(CookDiagnostic{code, true, asset, {}, 0, ErrorCode::None});
}

// 객체 타입의 차원이다(Texture 파생 다섯 — 아니면 false).
bool CookDimensionOf(TypeId type, TextureDimension& out) noexcept {
    if (type == TypeIdOf<Texture2D>()) {
        out = TextureDimension::Texture2D;
    } else if (type == TypeIdOf<TextureCube>()) {
        out = TextureDimension::TextureCube;
    } else if (type == TypeIdOf<Texture2DArray>()) {
        out = TextureDimension::Texture2DArray;
    } else if (type == TypeIdOf<TextureCubeArray>()) {
        out = TextureDimension::TextureCubeArray;
    } else if (type == TypeIdOf<VolumeTexture>()) {
        out = TextureDimension::Volume;
    } else {
        return false;
    }

    return true;
}

bool CookIsCube(TextureDimension dimension) noexcept {
    return dimension == TextureDimension::TextureCube ||
           dimension == TextureDimension::TextureCubeArray;
}

// 빌드 밖에서 쓰는 텍스처 값 사본이다(Borrow를 오래 쥐지 않는다).
struct CookTextureFacts {
    TextureSource source;
    TextureGroup lodGroup = TextureGroup::World;
    TextureFilter filter = TextureFilter::Default;
    TextureMipLoadOptions mipLoadOptions = TextureMipLoadOptions::Default;
    TextureAvailability availability = TextureAvailability::GPU;
    bool neverStream = false;
    bool compressionNoAlpha = false;
    bool compressionForceAlpha = false;
    bool compressionNone = false;
    Guid compressionCacheId{};
    TextureSamplerSettings sampler;
};

// 샘플러 주소·Border 색이다(12.14절 8번 — UE Texture.h 2033·TextureCubeArray.cpp 171–176·
// Texture2DArrayResource.cpp 21–23·VolumeTexture.h 92–94).
void CookSamplerAddress(const Texture& texture, TextureDimension dimension,
                        TextureSamplerSettings& out) noexcept {
    switch (dimension) {
    case TextureDimension::Texture2D: {
        const auto& typed = static_cast<const Texture2D&>(texture);
        out.addressU = typed.addressX;
        out.addressV = typed.addressY;
        out.addressW = TextureAddress::Wrap;
        out.borderColor = typed.borderColor;
        return;
    }
    case TextureDimension::Texture2DArray: {
        const auto& typed = static_cast<const Texture2DArray&>(texture);
        out.addressU = typed.addressX;
        out.addressV = typed.addressY;
        out.addressW = typed.addressZ;
        out.borderColor = typed.borderColor;
        return;
    }
    case TextureDimension::Volume: {
        const auto& typed = static_cast<const VolumeTexture&>(texture);
        out.addressU = typed.addressMode;
        out.addressV = typed.addressMode;
        out.addressW = typed.addressMode;
        out.borderColor = typed.borderColor;
        return;
    }
    case TextureDimension::TextureCube:
    case TextureDimension::TextureCubeArray:
        break;
    }

    out.addressU = TextureAddress::Clamp;
    out.addressV = TextureAddress::Clamp;
    out.addressW = TextureAddress::Clamp;
    out.borderColor = TextureBorderColor::TransparentBlack;
}

// 너겟 지역 키다(12.14절 5번 — 머리, TypeId, 빌드 버전, 원본 정체성, 유효 설정, 압축 기록 칸).
Result<Sha256Digest> CookLocalKey(const CookContext& context, TypeId type,
                                  const CookTextureFacts& facts,
                                  const TextureBuildSettings& settings) noexcept {
    auto lease = context.registry->Types().AcquireValueType(
        GeneratedType<TextureBuildSettings>::Id(), TypeKind::Struct);
    if (!lease) {
        return Result<Sha256Digest>::Failure(lease.Error());
    }

    NuggetKeyWriter writer;
    WriteNuggetKeyHeader(writer, TextureNuggetProcessorId, context.nuggets->ProjectGeneration());
    writer.WriteGuid(type);
    writer.WriteU32(TextureBuildVersion);
    const TextureSource& source = facts.source;
    writer.WriteDigest(source.bulk.sha256);
    writer.WriteU64(source.bulk.rawBytes);
    writer.WriteI64(static_cast<int64>(source.format));
    writer.WriteU32(source.sizeX);
    writer.WriteU32(source.sizeY);
    writer.WriteU32(source.numSlices);
    writer.WriteU32(source.numMips);
    writer.WriteU32(source.numLayers);
    // 결과를 바꾸지 않는 칸은 사본에서 고정한다(12.14절 5번 — 주소는 mip 결과에 쓰이지 않고 경고는
    // 너겟 밖에서 계산한다, norm16은 출력이 바뀔 때만).
    TextureBuildSettings keyed = settings;
    keyed.addressX = TextureAddress::Wrap;
    keyed.addressY = TextureAddress::Wrap;
    keyed.addressZ = TextureAddress::Wrap;
    TextureBuildSettings other = settings;
    other.norm16Supported = !settings.norm16Supported;
    if (TextureBuildOutput(settings).pixelFormat == TextureBuildOutput(other).pixelFormat) {
        keyed.norm16Supported = true;
    }

    const AssignmentTarget target = AssignmentTarget::Value(
        *context.registry, context.access, lease.Value(), &keyed, *context.allocator);
    if (auto written = WriteReflected(writer, target); !written) {
        return Result<Sha256Digest>::Failure(written.Error());
    }

    writer.WriteBool(facts.compressionNoAlpha);
    writer.WriteBool(facts.compressionForceAlpha);
    writer.WriteBool(facts.compressionNone);
    writer.WriteGuid(facts.compressionCacheId);
    return writer.Finish();
}

// 적중 값을 받아들일 수 있으면 mips에 값 byte를 잇는다(12.14절 6번 — 아니면 false, 미적중으로
// 다룬다). 오류는 할당 실패뿐이다.
Result<bool> CookAcceptHit(const NuggetCacheGetResult& got, const TextureBuildSettings& settings,
                           Allocator& allocator, Array<TextureCookMip>& mips) noexcept {
    mips.Clear();
    if (got.values.IsEmpty() || got.values[0].id != NuggetSccValueId(0, 0)) {
        return Result<bool>::Success(false);
    }

    TextureNuggetHeader header(allocator);
    if (auto read = ReadTextureNuggetHeader(got.values[0].bytes.View(), header); !read) {
        return read.Error() == ErrorCode::OutOfMemory ? Result<bool>::Failure(read.Error())
                                                      : Result<bool>::Success(false);
    }

    const TextureBuildOutputInfo expected = TextureBuildOutput(settings);
    const bool volume = settings.dimension == TextureDimension::Volume;
    const uint32 slices = volume ? 1u : settings.numSlices;
    bool same = got.values.Size() == header.mips.Size() + 1 &&
                header.output.pixelFormat == expected.pixelFormat &&
                header.output.srgb == expected.srgb && header.numSlices == slices;
    for (uint32 channel = 0; same && channel < CookSwizzleCount; ++channel) {
        same = header.output.swizzle[channel] == expected.swizzle[channel];
    }

    if (!same) {
        return Result<bool>::Success(false);
    }

    if (auto reserved = mips.TryReserve(header.mips.Size()); !reserved) {
        return Result<bool>::Failure(reserved.Error());
    }

    for (usize index = 0; index < header.mips.Size(); ++index) {
        const TextureNuggetMip& mip = header.mips[index];
        if (index == 0) {
            same = mip.sizeX != 0 && mip.sizeY != 0 && mip.sizeZ != 0 && (volume || mip.sizeZ == 1);
        } else {
            const TextureNuggetMip& upper = header.mips[index - 1];
            same = mip.sizeX == CookHalf(upper.sizeX) && mip.sizeY == CookHalf(upper.sizeY) &&
                   mip.sizeZ == (volume ? CookHalf(upper.sizeZ) : 1u);
        }

        // byte 수는 형식 크기 × 깊이 × slice다(넘침이면 손상으로 본다).
        const uint64 image = PixelFormatImageBytes(header.output.pixelFormat, mip.sizeX, mip.sizeY);
        const uint64 layers = uint64{mip.sizeZ} * slices;
        const NuggetValue& value = got.values[index + 1];
        same = same && image != 0 && layers <= NumericLimits<uint64>::Max() / image &&
               mip.byteSize == image * layers && value.bytes.Size() == mip.byteSize &&
               value.id == NuggetSccValueId(0, static_cast<uint32>(index + 1));
        if (!same) {
            mips.Clear();
            return Result<bool>::Success(false);
        }

        (void)mips.TryPushBack(TextureCookMip{mip.sizeX, mip.sizeY, mip.sizeZ, value.bytes.View()});
    }

    // 사슬 끝: NoMipmaps면 mip 하나, 아니면 1×1(×1)까지다(빌드는 위 mip만 버린다 — 잘린 사슬을
    // 거른다).
    const TextureCookMip& last = mips[mips.Size() - 1];
    const bool complete = settings.mipGenSettings == TextureMipGenSettings::NoMipmaps
                              ? mips.Size() == 1
                              : last.sizeX == 1 && last.sizeY == 1 && last.sizeZ == 1;
    if (!complete) {
        mips.Clear();
        return Result<bool>::Success(false);
    }

    return Result<bool>::Success(true);
}

// 두 결과가 byte까지 같은지다(verify — 머리와 mip마다).
bool CookSameResult(Span<const uint8> leftHeader, Span<const TextureCookMip> left,
                    Span<const uint8> rightHeader, Span<const TextureCookMip> right) noexcept {
    if (leftHeader.Size() != rightHeader.Size() || left.Size() != right.Size() ||
        !platform::MemoryEquals(leftHeader.Data(), rightHeader.Data(), leftHeader.Size())) {
        return false;
    }

    for (usize index = 0; index < left.Size(); ++index) {
        const Span<const uint8> a = left[index].bytes;
        const Span<const uint8> b = right[index].bytes;
        if (a.Size() != b.Size() || !platform::MemoryEquals(a.Data(), b.Data(), a.Size())) {
            return false;
        }
    }

    return true;
}

// 쿠킹 결과 하나다(빌드 결과 또는 적중 값 — mips의 byte는 그쪽 소유).
struct CookOutput {
    TextureBuildOutputInfo info;
    uint32 numSlices = 1;
};

// platformData를 채운다(12.14절 7번). 스트리밍 mip은 mip마다 bulk, 상주 꼬리는 큰 mip부터 이은
// tail 하나다. tail은 bulk가 빌리므로 쓰기가 끝날 때까지 산다.
Result<void> CookFillPlatformData(TexturePlatformData& data, TextureDimension dimension,
                                  const CookOutput& output, Span<const TextureCookMip> mips,
                                  uint32 resident, const TextureSamplerSettings& sampler,
                                  CookChainedBulkSource& bulk, Array<uint8>& tail) noexcept {
    const uint32 count = static_cast<uint32>(mips.Size());
    const uint32 streamed = count - resident;
    // 상주 꼬리: mip마다 16 byte 정렬 시작, 사이는 0(RenderBase 6.5절 — 마지막 뒤는 채우지 않는다).
    for (uint32 index = streamed; index < count; ++index) {
        const usize aligned = static_cast<usize>((tail.Size() + TextureTailMipAlignment - 1) &
                                                 ~(TextureTailMipAlignment - 1));
        if (auto padded = tail.TryResize(aligned); !padded) {
            return padded;
        }

        if (auto appended = tail.TryAppend(mips[index].bytes); !appended) {
            return appended;
        }
    }

    data.dimension = dimension;
    data.pixelFormat = static_cast<uint8>(output.info.pixelFormat);
    data.srgb = output.info.srgb;
    data.swizzleR = output.info.swizzle[0];
    data.swizzleG = output.info.swizzle[1];
    data.swizzleB = output.info.swizzle[2];
    data.swizzleA = output.info.swizzle[3];
    data.sizeX = mips[0].sizeX;
    data.sizeY = mips[0].sizeY;
    data.sizeZ = mips[0].sizeZ;
    data.numMips = count;
    data.numSlices = output.numSlices;
    data.residentTailMips = resident;
    data.sampler = sampler;
    // 앞 값이 남아 있어도 이번 mip만 둔다(되돌리기가 늘 기본값을 보장하지만 채우기가 그것에 기대지
    // 않는다).
    data.mips.Clear();
    if (auto reserved = data.mips.TryReserve(count); !reserved) {
        return reserved;
    }

    uint64 offset = 0;
    for (uint32 index = 0; index < count; ++index) {
        TextureMipData mip;
        mip.sizeX = mips[index].sizeX;
        mip.sizeY = mips[index].sizeY;
        mip.sizeZ = mips[index].sizeZ;
        mip.byteSize = mips[index].bytes.Size();
        if (index < streamed) {
            auto added = bulk.Add(mips[index].bytes);
            if (!added) {
                return Result<void>::Failure(added.Error());
            }

            mip.bulk = added.Value();
        } else {
            offset = (offset + TextureTailMipAlignment - 1) & ~(TextureTailMipAlignment - 1);
            mip.tailOffset = offset;
            offset += mip.byteSize;
        }

        (void)data.mips.TryPushBack(mip);
    }

    auto added = bulk.Add(tail.View());
    if (!added) {
        return Result<void>::Failure(added.Error());
    }

    data.tailBulk = added.Value();
    return Result<void>::Success();
}

// 에디터 객체의 platformData를 기본값으로 되돌린다(12.14절 9번 — 할당하지 않는다).
Result<void> CookResetPlatformData(ObjectRegistry& registry, const ObjectAccessContext& access,
                                   ObjectHandle handle) noexcept {
    auto borrowed = registry.ResolveForWrite<Texture>(access, handle);
    if (!borrowed) {
        return Result<void>::Failure(borrowed.Error());
    }

    auto value = borrowed.Value().Get();
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    TexturePlatformData& data = value.Value()->platformData;
    TexturePlatformData empty(data.mips.GetAllocator());
    data = Move(empty);
    return Result<void>::Success();
}

} // namespace

Result<void> WriteTextureNuggetHeader(const TextureBuildOutputInfo& output, uint32 numSlices,
                                      Span<const TextureCookMip> mips, Array<uint8>& out) noexcept {
    out.Clear();
    if (mips.IsEmpty() || mips.Size() > TextureNuggetMaxMips) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    ByteWriter writer(out);
    writer.WriteU32LE(TextureNuggetHeaderVersion);
    writer.WriteU32LE(static_cast<uint32>(output.pixelFormat));
    writer.WriteU8(output.srgb ? 1 : 0);
    for (uint32 channel = 0; channel < CookSwizzleCount; ++channel) {
        writer.WriteU8(static_cast<uint8>(output.swizzle[channel]));
    }

    writer.WriteU32LE(numSlices);
    writer.WriteU32LE(static_cast<uint32>(mips.Size()));
    for (const TextureCookMip& mip : mips) {
        writer.WriteU32LE(mip.sizeX);
        writer.WriteU32LE(mip.sizeY);
        writer.WriteU32LE(mip.sizeZ);
        writer.WriteU64LE(mip.bytes.Size());
    }

    return writer.Status();
}

Result<void> ReadTextureNuggetHeader(Span<const uint8> bytes, TextureNuggetHeader& out) noexcept {
    out.mips.Clear();
    ByteReader reader(bytes);
    if (bytes.Size() < CookHeaderFixedBytes || reader.ReadU32LE() != TextureNuggetHeaderVersion) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto format = PixelFormatFromNumber(reader.ReadU32LE());
    const uint8 srgb = reader.ReadU8();
    if (!format || !IsValidPixelFormat(format.Value()) || srgb > 1) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    out.output.pixelFormat = format.Value();
    out.output.srgb = srgb != 0;
    for (uint32 channel = 0; channel < CookSwizzleCount; ++channel) {
        const uint8 swizzle = reader.ReadU8();
        if (swizzle > static_cast<uint8>(TextureSwizzle::One)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        out.output.swizzle[channel] = static_cast<TextureSwizzle>(swizzle);
    }

    out.numSlices = reader.ReadU32LE();
    const uint32 count = reader.ReadU32LE();
    if (count == 0 || count > TextureNuggetMaxMips || out.numSlices == 0 ||
        bytes.Size() != CookHeaderFixedBytes + usize{count} * CookHeaderMipBytes) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto reserved = out.mips.TryReserve(count); !reserved) {
        return reserved;
    }

    for (uint32 index = 0; index < count; ++index) {
        TextureNuggetMip mip;
        mip.sizeX = reader.ReadU32LE();
        mip.sizeY = reader.ReadU32LE();
        mip.sizeZ = reader.ReadU32LE();
        mip.byteSize = reader.ReadU64LE();
        (void)out.mips.TryPushBack(mip);
    }

    return reader.Failed() ? Result<void>::Failure(ErrorCode::InvalidArgument)
                           : Result<void>::Success();
}

uint32 TextureResidentTailMips(uint32 numMips, bool streaming) noexcept {
    if (!streaming || numMips <= 1) {
        return numMips;
    }

    return numMips < TextureMinResidentMipCount ? numMips : TextureMinResidentMipCount;
}

TextureSamplerFilter TextureCookSamplerFilter(TextureFilter filter,
                                              const TextureLodGroup& group) noexcept {
    switch (filter) {
    case TextureFilter::Nearest:
        return TextureSamplerFilter::Point;
    case TextureFilter::Bilinear:
        return TextureSamplerFilter::Bilinear;
    case TextureFilter::Trilinear:
        return TextureSamplerFilter::Trilinear;
    case TextureFilter::Default:
        break;
    }

    // UE SetupGroup(TextureLODSettings.cpp 110–137)의 조합이다.
    const bool linearMip = group.mipFilter == TextureLodMipFilter::Linear;
    switch (group.minMagFilter) {
    case TextureLodMinMagFilter::Point:
        return TextureSamplerFilter::Point;
    case TextureLodMinMagFilter::Linear:
        return linearMip ? TextureSamplerFilter::Trilinear : TextureSamplerFilter::Bilinear;
    case TextureLodMinMagFilter::Aniso:
        break;
    }

    return linearMip ? TextureSamplerFilter::AnisotropicLinear
                     : TextureSamplerFilter::AnisotropicPoint;
}

Result<void> TextureCookProcessor::Cook(CookContext& context) const noexcept {
    Allocator& allocator = *context.allocator;
    ObjectRegistry& registry = *context.registry;
    const ObjectAccessContext& access = context.access;
    const AssetId assetId = context.entry->id;

    // 1. 대표 에셋과 차원.
    auto asset = registry.FindChild(access, context.package, "");
    if (!asset) {
        return CookFail(context, AssetDiagnosticCode::None, asset.Error());
    }

    auto type = registry.AcquireType(access, asset.Value());
    if (!type) {
        return CookFail(context, AssetDiagnosticCode::None, type.Error());
    }

    const TypeId typeId = type.Value().Id();
    TextureDimension dimension = TextureDimension::Texture2D;
    if (!CookDimensionOf(typeId, dimension)) {
        return CookFail(context, AssetDiagnosticCode::TypeMismatch, ErrorCode::TypeMismatch);
    }

    // 2. 설정 판정(12.8절).
    {
        Array<TextureSettingIssue> issues(allocator);
        if (auto checked = CheckTextureSettings(registry, access, asset.Value(), allocator, issues);
            !checked) {
            return CookFail(context, AssetDiagnosticCode::None, checked.Error());
        }

        if (!issues.IsEmpty()) {
            const AssetDiagnosticCode code = issues[0].code;
            return CookFail(context, code,
                            code == AssetDiagnosticCode::UnsupportedSetting
                                ? ErrorCode::Unsupported
                                : ErrorCode::InvalidArgument,
                            issues.Size());
        }
    }

    // 3. 유효 설정(텍스처 + LOD 그룹)과 쿠킹 밖 값.
    CookTextureFacts facts;
    TextureLodGroup group;
    TextureBuildSettings settings;
    TextureBuildResult built(allocator);
    {
        auto borrowed = registry.Resolve<Texture>(access, asset.Value());
        if (!borrowed) {
            return CookFail(context, AssetDiagnosticCode::None, borrowed.Error());
        }

        auto value = borrowed.Value().Get();
        if (!value) {
            return CookFail(context, AssetDiagnosticCode::None, value.Error());
        }

        const Texture& texture = *value.Value();
        facts.source = texture.source;
        facts.lodGroup = texture.lodGroup;
        facts.filter = texture.filter;
        facts.mipLoadOptions = texture.mipLoadOptions;
        facts.availability = texture.availability;
        facts.neverStream = texture.neverStream != 0;
        facts.compressionNoAlpha = texture.compressionNoAlpha != 0;
        facts.compressionForceAlpha = texture.compressionForceAlpha != 0;
        facts.compressionNone = texture.compressionNone != 0;
        facts.compressionCacheId = texture.compressionCacheId;
        CookSamplerAddress(texture, dimension, facts.sampler);
        group = FindTextureLodGroup(*context.profile, facts.lodGroup);
        TextureLodGroupValues values;
        values.minLodSize = group.minLodSize;
        values.maxLodSize = group.maxLodSize;
        values.lodBias = group.lodBias;
        values.mipGenSettings = group.mipGenSettings;
        if (auto resolved =
                ResolveTextureBuildSettings(texture, dimension, values, settings, built);
            !resolved) {
            return CookFail(context, built.failure.code, resolved.Error(), built.failure.detail);
        }
    }

    settings.norm16Supported = context.profile->textureNorm16Supported;
    facts.sampler.filter = TextureCookSamplerFilter(facts.filter, group);
    // 음수 maxAniso는 프로파일 검증이 거른다(0 = 전역 값).
    facts.sampler.maxAniso = group.maxAniso > 0 ? static_cast<uint32>(group.maxAniso) : 0u;
    if (facts.source.bulk.IsEmpty() || facts.source.bulk.rawBytes == 0) {
        return CookFail(context, AssetDiagnosticCode::InvalidSource, ErrorCode::InvalidArgument);
    }

    // 10. 경고(설정만으로 정한다 — 적중·미적중이 같다).
    AssetDiagnosticCode warnings[TextureBuildMaxWarnings];
    const usize warningCount = TextureBuildWarnings(settings, warnings);
    for (usize index = 0; index < warningCount; ++index) {
        if (auto warned = CookWarn(context, warnings[index]); !warned) {
            return warned;
        }
    }

    // 5–6. 너겟 조회.
    NuggetCache* nuggets = context.nuggets;
    Sha256Digest sccKey{};
    NuggetCacheGetResult got(allocator);
    Array<TextureCookMip> hitMips(allocator);
    bool hit = false;
    if (nuggets != nullptr) {
        auto localKey = CookLocalKey(context, typeId, facts, settings);
        if (!localKey) {
            return CookFail(context, AssetDiagnosticCode::None, localKey.Error());
        }

        const NuggetSccMember member{assetId, localKey.Value()};
        auto key = NuggetSccKey(allocator, Span<const NuggetSccMember>(&member, 1), {});
        if (!key) {
            return CookFail(context, AssetDiagnosticCode::None, key.Error());
        }

        sccKey = key.Value();
        lastKey_ = sccKey;
        got = nuggets->GetSccMember(NuggetBucketTexture, sccKey, assetId);
        if (got.failed && nuggets->IsRequired()) {
            return CookFail(context, AssetDiagnosticCode::None,
                            got.error != ErrorCode::None ? got.error : ErrorCode::IoError);
        }

        if (got.status == NuggetStatus::Hit) {
            auto accepted = CookAcceptHit(got, settings, allocator, hitMips);
            if (!accepted) {
                return CookFail(context, AssetDiagnosticCode::None, accepted.Error());
            }

            hit = accepted.Value();
            // 받아들이지 못한 Record는 치운다(12.14절 6번 — 남기면 다시 만든 값의 Put이 늘
            // Conflict다).
            if (!hit) {
                auto rejected = nuggets->Reject(NuggetBucketTexture, sccKey, got.digest);
                if (!rejected && nuggets->IsRequired()) {
                    return CookFail(context, AssetDiagnosticCode::None, rejected.Error());
                }
            }
        }
    }

    // 미적중이거나 verify면 빌드한다.
    Array<TextureCookMip> builtMips(allocator);
    Array<uint8> builtHeader(allocator);
    if (!hit || context.verifyNuggets) {
        if (context.bulkSource == nullptr) {
            return CookFail(context, AssetDiagnosticCode::None, ErrorCode::InvalidArgument);
        }

        auto read = context.bulkSource->ReadBulk(facts.source.bulk);
        if (!read) {
            return CookFail(context, AssetDiagnosticCode::InvalidSource, read.Error());
        }

        ++builds_;
        if (auto done = BuildTexture(allocator, settings, read.Value(), built); !done) {
            return CookFail(context, built.failure.code, done.Error(), built.failure.detail);
        }

        if (auto reserved = builtMips.TryReserve(built.mips.Size()); !reserved) {
            return CookFail(context, AssetDiagnosticCode::None, reserved.Error());
        }

        for (const TextureBuiltMip& mip : built.mips) {
            (void)builtMips.TryPushBack(
                TextureCookMip{mip.sizeX, mip.sizeY, mip.sizeZ, mip.bytes.View()});
        }

        TextureBuildOutputInfo info;
        info.pixelFormat = built.pixelFormat;
        info.srgb = built.srgb;
        for (uint32 channel = 0; channel < CookSwizzleCount; ++channel) {
            info.swizzle[channel] = built.swizzle[channel];
        }

        if (auto written =
                WriteTextureNuggetHeader(info, built.numSlices, builtMips.View(), builtHeader);
            !written) {
            return CookFail(context, AssetDiagnosticCode::None, written.Error());
        }

        if (hit && !CookSameResult(got.values[0].bytes.View(), hitMips.View(), builtHeader.View(),
                                   builtMips.View())) {
            return CookFail(context, AssetDiagnosticCode::NuggetMismatch,
                            ErrorCode::InvalidArgument);
        }
    }

    // 7–8. 플랫폼 데이터(적중이면 적중 값, 아니면 빌드 결과 — verify 일치면 같은 byte다).
    const bool useBuilt = !builtMips.IsEmpty();
    const Span<const TextureCookMip> mips = useBuilt ? builtMips.View() : hitMips.View();
    CookOutput output;
    if (useBuilt) {
        output.info.pixelFormat = built.pixelFormat;
        output.info.srgb = built.srgb;
        for (uint32 channel = 0; channel < CookSwizzleCount; ++channel) {
            output.info.swizzle[channel] = built.swizzle[channel];
        }

        output.numSlices = built.numSlices;
    } else {
        output.info = TextureBuildOutput(settings);
        output.numSlices = dimension == TextureDimension::Volume ? 1u : settings.numSlices;
    }

    const uint32 count = static_cast<uint32>(mips.Size());
    const TextureMipLoadOptions mipLoad = facts.mipLoadOptions == TextureMipLoadOptions::Default
                                              ? group.mipLoadOptions
                                              : facts.mipLoadOptions;
    // 12.5.1절 3번 비스트리밍 조건이다.
    const bool streaming =
        !facts.neverStream && facts.lodGroup != TextureGroup::UI && count > 1 &&
        settings.mipGenSettings != TextureMipGenSettings::NoMipmaps && !CookIsCube(dimension) &&
        CookIsPowerOfTwo(mips[0].sizeX) && CookIsPowerOfTwo(mips[0].sizeY) &&
        (dimension != TextureDimension::Volume || CookIsPowerOfTwo(mips[0].sizeZ)) &&
        mipLoad != TextureMipLoadOptions::OnlyFirstMip &&
        facts.availability != TextureAvailability::CPU;
    const uint32 resident = TextureResidentTailMips(count, streaming);

    // 9. 쿠킹 패키지 — platformData를 채우고 쓰고, 성공·실패 모두 되돌린다.
    CookChainedBulkSource bulk(allocator, context.bulkSource);
    Array<uint8> tail(allocator);
    Result<void> filled = Result<void>::Success();
    {
        auto borrowed = registry.ResolveForWrite<Texture>(access, asset.Value());
        if (!borrowed) {
            return CookFail(context, AssetDiagnosticCode::None, borrowed.Error());
        }

        auto value = borrowed.Value().Get();
        if (!value) {
            return CookFail(context, AssetDiagnosticCode::None, value.Error());
        }

        filled = CookFillPlatformData(value.Value()->platformData, dimension, output, mips,
                                      resident, facts.sampler, bulk, tail);
    }

    Result<void> cooked = filled;
    if (filled) {
        CookContext write = context;
        write.bulkSource = &bulk;
        cooked = CookProcessor::Cook(write);
    } else {
        cooked = CookFail(context, AssetDiagnosticCode::None, filled.Error());
    }

    Result<void> restored = CookResetPlatformData(registry, access, asset.Value());
    if (!cooked) {
        return cooked;
    }

    if (!restored) {
        return CookFail(context, AssetDiagnosticCode::None, restored.Error());
    }

    // 미적중이면 Put한다(값 byte를 옮긴다 — 쓰기가 끝난 뒤). Put 실패는 required일 때만 오류다.
    if (nuggets == nullptr || hit) {
        return Result<void>::Success();
    }

    Array<NuggetValue> values(allocator);
    if (auto reserved = values.TryReserve(built.mips.Size() + 1); !reserved) {
        return CookFail(context, AssetDiagnosticCode::None, reserved.Error());
    }

    NuggetValue head(allocator);
    head.id = NuggetSccValueId(0, 0);
    head.bytes = Move(builtHeader);
    (void)values.TryPushBack(Move(head));
    for (usize index = 0; index < built.mips.Size(); ++index) {
        NuggetValue mip(allocator);
        mip.id = NuggetSccValueId(0, static_cast<uint32>(index + 1));
        mip.bytes = Move(built.mips[index].bytes);
        (void)values.TryPushBack(Move(mip));
    }

    NuggetCachePut put;
    put.bucket = NuggetBucketTexture;
    put.key = sccKey;
    put.values = values.View();
    const AssetId members[1] = {assetId};
    const NuggetCachePutResult stored = nuggets->PutScc(Span<const AssetId>(members), put);
    if (stored.status == NuggetStatus::Conflict) {
        if (auto warned = CookWarn(context, AssetDiagnosticCode::NuggetConflict); !warned) {
            return warned;
        }
    }

    if (stored.failed && nuggets->IsRequired()) {
        return CookFail(context, AssetDiagnosticCode::None,
                        stored.error != ErrorCode::None ? stored.error : ErrorCode::IoError);
    }

    return Result<void>::Success();
}

Result<void> RegisterTextureCookProcessors(void* state, CookProcessorTable& table) noexcept {
    if (state == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return table.Register(TypeIdOf<Texture>(), *static_cast<const TextureCookProcessor*>(state));
}

} // namespace rocks
