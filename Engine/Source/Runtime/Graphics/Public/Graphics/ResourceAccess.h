#pragma once

#include <Base/NumericLimits.h>
#include <Graphics/DeviceTypes.h>

// 명시 접근 상태와 전이다(Docs/Design/RenderBase.ko.md 4.4절 GX5, UE ERHIAccess·FRHITransitionInfo
// 대응). 렌더 그래프가 pass마다 자원 접근을 모아 전이를 계산하고, 장치는 그 전이를 자기 API로
// 낮춘다(CommandContext::Transition, 분리 전이는 BeginTransitions·EndTransitions). 1차 범위는
// graphics queue 하나이므로 pipe(그래픽·async compute) 구분은 아직 없다(RG4에서 더한다).
namespace rocks::graphics {

// 자원 하나(또는 subresource 범위)의 GPU 접근 상태다. 읽기 비트끼리는 함께 켤 수 있다(여러 읽기를
// 한 상태로 합친 것). 쓰기 비트는 아래 규칙(CanCombineAccess)을 따른다.
//
// 값은 실행 중에만 쓰며 파일에 저장하지 않는다(비트 위치를 바꿔도 된다).
enum class ResourceAccess : uint32 {
    // 상태를 모른다. 전이의 before로만 쓴다: 장치가 추적한 현재 상태에서 전이한다.
    Unknown = 0,

    // --- 읽기 ---
    // 정점·index 버퍼 입력이다.
    VertexOrIndex = 1u << 0,
    // 상수 버퍼 읽기다.
    Constant = 1u << 1,
    // 간접 그리기·dispatch 인자다.
    IndirectArgs = 1u << 2,
    // 그래픽 셰이더(정점·픽셀)의 SRV 읽기다.
    SrvGraphics = 1u << 3,
    // compute 셰이더의 SRV 읽기다.
    SrvCompute = 1u << 4,
    // 복사 원본이다.
    CopySource = 1u << 5,
    // 스왑체인 present다.
    Present = 1u << 6,
    // 깊이 읽기 전용 attachment다(깊이 쓰기와 함께 켤 수 있다).
    DepthRead = 1u << 7,

    // --- 읽기·쓰기 ---
    // 그래픽 셰이더 UAV다(셰이더 시스템 7.2절 4항에 따라 지금은 compute만 UAV를 쓴다).
    UavGraphics = 1u << 8,
    // compute 셰이더 UAV다.
    UavCompute = 1u << 9,

    // --- 쓰기 ---
    // 색 렌더 타깃이다.
    RenderTarget = 1u << 10,
    // 깊이·스텐실 쓰기 attachment다.
    DepthWrite = 1u << 11,
    // 복사 대상이다.
    CopyDestination = 1u << 12,

    // 이전 내용이 필요 없다(내용 버림). 전이의 after로 쓰면 장치는 내용을 보존하지 않아도 된다.
    // transient aliasing(RG5)의 해제 상태다. 지금 backend는 아무것도 하지 않는다.
    Discard = 1u << 13
};

constexpr ResourceAccess operator|(ResourceAccess left, ResourceAccess right) noexcept {
    return static_cast<ResourceAccess>(static_cast<uint32>(left) | static_cast<uint32>(right));
}

constexpr ResourceAccess operator&(ResourceAccess left, ResourceAccess right) noexcept {
    return static_cast<ResourceAccess>(static_cast<uint32>(left) & static_cast<uint32>(right));
}

constexpr ResourceAccess operator~(ResourceAccess value) noexcept {
    return static_cast<ResourceAccess>(~static_cast<uint32>(value));
}

constexpr ResourceAccess& operator|=(ResourceAccess& left, ResourceAccess right) noexcept {
    left = left | right;
    return left;
}

// 접근 비트 묶음이다(UE ERHIAccess의 같은 이름 mask 대응).
inline constexpr ResourceAccess SrvAccessMask =
    ResourceAccess::SrvGraphics | ResourceAccess::SrvCompute;
inline constexpr ResourceAccess UavAccessMask =
    ResourceAccess::UavGraphics | ResourceAccess::UavCompute;
inline constexpr ResourceAccess DepthAccessMask =
    ResourceAccess::DepthRead | ResourceAccess::DepthWrite;
// 쓰기 상태와 함께 켤 수 없는 읽기 전용 비트다.
inline constexpr ResourceAccess ReadOnlyExclusiveAccessMask =
    ResourceAccess::VertexOrIndex | ResourceAccess::Constant | ResourceAccess::IndirectArgs |
    SrvAccessMask | ResourceAccess::CopySource | ResourceAccess::Present;
// 모든 읽기 전용 비트다(깊이 읽기는 깊이 쓰기와 함께 켤 수 있다).
inline constexpr ResourceAccess ReadOnlyAccessMask =
    ReadOnlyExclusiveAccessMask | ResourceAccess::DepthRead;
// 읽을 수 있는 모든 비트다(UAV 포함).
inline constexpr ResourceAccess ReadableAccessMask = ReadOnlyAccessMask | UavAccessMask;
// 읽기 상태와 함께 켤 수 없는 쓰기 전용 비트다.
inline constexpr ResourceAccess WriteOnlyExclusiveAccessMask =
    ResourceAccess::RenderTarget | ResourceAccess::CopyDestination;
// 모든 쓰기 전용 비트다.
inline constexpr ResourceAccess WriteOnlyAccessMask =
    WriteOnlyExclusiveAccessMask | ResourceAccess::DepthWrite;
// 쓸 수 있는 모든 비트다(UAV 포함).
inline constexpr ResourceAccess WritableAccessMask = WriteOnlyAccessMask | UavAccessMask;
// 정의된 모든 비트다.
inline constexpr ResourceAccess AllAccessMask =
    ReadableAccessMask | WritableAccessMask | ResourceAccess::Discard;

// mask의 비트가 하나라도 켜져 있는지다.
constexpr bool HasAnyAccess(ResourceAccess value, ResourceAccess mask) noexcept {
    return (value & mask) != ResourceAccess::Unknown;
}

// 쓰기 비트가 하나라도 있는지다(UAV 포함).
constexpr bool IsWritableAccess(ResourceAccess value) noexcept {
    return HasAnyAccess(value, WritableAccessMask);
}

// 읽기 전용 비트만 있는지다(Unknown·Discard는 false).
constexpr bool IsReadOnlyAccess(ResourceAccess value) noexcept {
    return value != ResourceAccess::Unknown &&
           (value & ~ReadOnlyAccessMask) == ResourceAccess::Unknown;
}

// 비트 조합이 한 상태로 성립하는지다(UE FRDGSubresourceState::IsMergeAllowed의 접근 규칙).
// - Unknown·정의 밖 비트는 성립하지 않는다. Discard는 혼자일 때만 성립한다.
// - 쓰기와 함께 켤 수 없는 읽기(ReadOnlyExclusive)와 쓰기 비트, 읽기와 함께 켤 수 없는
//   쓰기(WriteOnlyExclusive)와 읽기 비트는 함께 켜지 않는다.
// - UAV는 UAV끼리만, 깊이 읽기·쓰기는 깊이끼리만 함께 켠다.
constexpr bool CanCombineAccess(ResourceAccess value) noexcept {
    if (value == ResourceAccess::Unknown || HasAnyAccess(value, ~AllAccessMask)) {
        return false;
    }

    if (HasAnyAccess(value, ResourceAccess::Discard)) {
        return value == ResourceAccess::Discard;
    }

    if (HasAnyAccess(value, ReadOnlyExclusiveAccessMask) && IsWritableAccess(value)) {
        return false;
    }

    if (HasAnyAccess(value, WriteOnlyExclusiveAccessMask) &&
        HasAnyAccess(value, ReadableAccessMask)) {
        return false;
    }

    if (HasAnyAccess(value, UavAccessMask) && HasAnyAccess(value, ~UavAccessMask)) {
        return false;
    }

    // 쓰기 전용 비트는 서로 다른 둘을 함께 켜지 않는다(렌더 타깃이면서 복사 대상인 상태는 없다).
    const ResourceAccess writes = value & WriteOnlyAccessMask;
    const uint32 bits = static_cast<uint32>(writes);
    if ((bits & (bits - 1)) != 0) {
        return false;
    }

    if (HasAnyAccess(value, ResourceAccess::DepthWrite) && HasAnyAccess(value, ~DepthAccessMask)) {
        return false;
    }

    return true;
}

// subresource 개수 "끝까지"를 뜻하는 값이다(SubresourceRange::mipCount·sliceCount).
inline constexpr uint32 RemainingSubresources = NumericLimits<uint32>::Max();

// 텍스처 subresource 범위다(mip × slice). slice는 배열 층·cube 면(cube array는 cube c의 면 f가
// c × 6 + f)이고 3D·2D 텍스처는 slice 0 하나다(TextureSliceCount, DeviceTypes.h). 버퍼는 범위를
// 쓰지 않는다(전체 하나). 기본값은 전체다. 개수 RemainingSubresources는 끝까지다.
struct SubresourceRange {
    uint32 firstMip = 0;
    uint32 mipCount = RemainingSubresources;
    uint32 firstSlice = 0;
    uint32 sliceCount = RemainingSubresources;

    bool operator==(const SubresourceRange&) const noexcept = default;
};

// 텍스처(mip mipLevels개 × slice slices개)에 맞춰 자른 범위다. 시작이 밖이거나 개수가 0이면
// IsEmpty다.
struct ResolvedSubresourceRange {
    uint32 firstMip = 0;
    uint32 mipCount = 0;
    uint32 firstSlice = 0;
    uint32 sliceCount = 0;

    bool IsEmpty() const noexcept {
        return mipCount == 0 || sliceCount == 0;
    }

    // mip·slice가 범위 안인지다.
    bool Contains(uint32 mip, uint32 slice) const noexcept {
        return mip >= firstMip && mip - firstMip < mipCount && slice >= firstSlice &&
               slice - firstSlice < sliceCount;
    }
};

// range를 텍스처 크기로 자른다(backend 전이·상태 추적 공용). 끝을 넘는 개수는 끝까지로 줄인다.
//
// 사용 예:
//   const ResolvedSubresourceRange resolved =
//       ResolveSubresourceRange(transition.range, record.mipLevels, record.slices);
//   if (resolved.IsEmpty()) { ++ignoredDraws_; continue; }
constexpr ResolvedSubresourceRange
ResolveSubresourceRange(const SubresourceRange& range, uint32 mipLevels, uint32 slices) noexcept {
    ResolvedSubresourceRange resolved;
    if (range.firstMip >= mipLevels || range.firstSlice >= slices) {
        return resolved;
    }

    const uint32 mipsLeft = mipLevels - range.firstMip;
    const uint32 slicesLeft = slices - range.firstSlice;
    resolved.firstMip = range.firstMip;
    resolved.mipCount = range.mipCount < mipsLeft ? range.mipCount : mipsLeft;
    resolved.firstSlice = range.firstSlice;
    resolved.sliceCount = range.sliceCount < slicesLeft ? range.sliceCount : slicesLeft;
    return resolved;
}

// view의 mip·slice 범위를 원 텍스처(dimension·usage·shape)에 맞춰 정한다(개수 0은 끝까지). 장치와
// 무관한 규칙이며 backend의 CreateTextureView가 부른다. InvalidArgument: 범위가 텍스처 밖이거나 빔,
// 3D의 slice 범위가 전체가 아님, cube의 slice 범위가 6면 전체가 아님, cube 배열의 시작·개수가 6의
// 배수가 아님, 모르는 access, ReadWrite인데 Storage 텍스처가 아니거나 mip이 하나가 아니거나 plane
// 0이 아니거나 swizzle이 항등이 아님. 형식 능력(StorageWrite)은 backend가 Unsupported로 본다.
Result<ResolvedSubresourceRange> ResolveTextureViewRange(TextureDimension dimension,
                                                         TextureUsage usage,
                                                         const TextureSubresources& shape,
                                                         const TextureViewDesc& desc) noexcept;

// 두 범위가 mip과 slice 모두에서 겹치는지다(텍스처 크기를 모를 때 RemainingSubresources는 끝없음).
// 분리 전이 begin·end 사이 접근 검사가 쓴다.
constexpr bool SubresourceRangesOverlap(const SubresourceRange& left,
                                        const SubresourceRange& right) noexcept {
    auto overlaps = [](uint32 leftFirst, uint32 leftCount, uint32 rightFirst, uint32 rightCount) {
        const uint64 leftEnd = uint64{leftFirst} + leftCount;
        const uint64 rightEnd = uint64{rightFirst} + rightCount;
        return leftCount != 0 && rightCount != 0 && leftFirst < rightEnd && rightFirst < leftEnd;
    };
    return overlaps(left.firstMip, left.mipCount, right.firstMip, right.mipCount) &&
           overlaps(left.firstSlice, left.sliceCount, right.firstSlice, right.sliceCount);
}

// 전이 하나다(CommandContext::Transition). texture와 buffer 가운데 하나만 null이 아니다.
//
// - before: 전이 전 상태다. Unknown이면 장치가 추적한 현재 상태에서 전이한다. Unknown이 아니면
// 장치가
//   추적한 상태와 같아야 하며, 검증 계층을 켠 장치는 다르면 ValidationErrorCount를 올린다(GPU에는
//   추적한 상태를 쓴다).
// - after: 전이 뒤 상태다. Unknown이면 그 전이는 무시한다(IgnoredDraws). CanCombineAccess를 지켜야
//   한다.
// - UAV → 같은 UAV 전이는 앞 쓰기가 끝나기를 기다리는 UAV barrier다(렌더 그래프는 생략 표시가 없을
//   때만 낸다).
struct ResourceTransition {
    TextureHandle texture;
    BufferHandle buffer;
    SubresourceRange range;
    ResourceAccess before = ResourceAccess::Unknown;
    ResourceAccess after = ResourceAccess::Unknown;

    bool operator==(const ResourceTransition&) const noexcept = default;
};

// 분리 전이(begin·end) 짝이다(CommandContext::BeginTransitions가 주고 EndTransitions가 받는다, UE
// FRHITransition 대응). 그 기록 안에서만 의미가 있다. id 0은 null이다.
struct SplitTransition {
    uint32 id = 0;

    bool IsNull() const noexcept {
        return id == 0;
    }

    bool operator==(const SplitTransition&) const noexcept = default;
};

} // namespace rocks::graphics
