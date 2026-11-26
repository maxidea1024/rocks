#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Graphics/Device.h>
#include <ShaderCore/ShaderBindingLayout.h>
#include <ShaderCore/ShaderParameterLayout.h>

// 파라미터 바인딩 지도와 상수 블록(Docs/Design/RenderBase 9.3·9.4절 RB4)이다. 입력은 레이아웃 표다
// (ShaderCore ShaderParameterLayout.h — 생성 표 ShaderParameterLayoutOf<T>() 또는 손으로 쓴 표,
// ShaderSystem 16.2절). 장치로
// 가는 경로는 둘이다: 루트 상수 표는 그리기별 상수(ApplyDrawConstants), 그 밖의 표는 상수 버퍼
// (ConstantBlock::Commit·ApplyConstantBlock, 9.11절 GX6).
namespace rocks::render {

// 레이아웃 표 하나가 셰이더 단계마다 놓이는 곳이다(9.3절 바인딩 지도의 상수 부분).
struct ShaderConstantPlacement {
    // 단계 비트(ShaderCore ShaderStageBits)다.
    uint32 stage = 0;
    // 루트 상수(그리기별 상수)면 참이다. 아니면 상수 버퍼 slot(backend 위치, ShaderSystem
    // SSD30)이다.
    bool rootConstants = false;
    uint16 slot = 0;
    // 셰이더가 선언한 블록 크기(byte)다.
    uint32 bytes = 0;
};

inline constexpr usize MaxShaderConstantPlacements = 3;

// 레이아웃 표 하나의 바인딩 지도다. 표는 지도보다 오래 산다(정적 표).
//
// 사용 예:
//   const shader::ShaderBindingLayout* stages[] = {set.BindingLayout(vs), set.BindingLayout(ps)};
//   auto map = ShaderBindingMap::Build(TintLayout, stages);
//   ConstantBlock block(allocator, TintLayout, ConstantBlockLifetime::PerDraw);
//   block.Set("tint", ...);
//   (void)map.Value().ApplyDrawConstants(commands, block, pipelineDrawConstantBytes);
class ROCKS_RENDERBASE_API ShaderBindingMap {
public:
    // 표와 단계 layout들(null은 건너뛴다)로 만든다. 표가 루트 상수면 루트 상수를 읽는 단계마다,
    // 아니면 표의 상수 버퍼를 선언한 단계마다 자리를 만든다(생성 표는 구조체 identity로, 손으로 쓴
    // 표는 이름으로 찾는다 — SX8, 9.14절). 규칙 위반은 InvalidArgument
    // 이며 reason에 영어 이유를 둔다: 표 자체 위반(ValidateShaderParameterLayout), 단계 블록이
    // 표보다 큼, 상수 버퍼 멤버가 표에 없거나 오프셋·크기가 다름, 어느 단계도 표를 쓰지 않음.
    static Result<ShaderBindingMap> Build(const shader::ShaderParameterLayout& layout,
                                          Span<const shader::ShaderBindingLayout* const> stages,
                                          const char** reason = nullptr) noexcept;

    const shader::ShaderParameterLayout& Layout() const noexcept {
        return *layout_;
    }

    Span<const ShaderConstantPlacement> Placements() const noexcept {
        return Span<const ShaderConstantPlacement>(placements_, count_);
    }

    // 표 멤버 이름의 (오프셋, byte 수)다. 없으면 NotFound다.
    Result<void> FindMember(StringView name, uint32& offset, uint32& bytes) const noexcept;

private:
    const shader::ShaderParameterLayout* layout_ = nullptr;
    ShaderConstantPlacement placements_[MaxShaderConstantPlacements] = {};
    usize count_ = 0;
};

// 바인딩 지도 자리의 단계 비트(ShaderCore ShaderStageBits)를 Graphics 단계로 바꾼다. 한 비트가
// 아니면 Compute다(지도는 한 비트만 만든다).
ROCKS_RENDERBASE_API graphics::ShaderStage ShaderStageOfBit(uint32 stage) noexcept;

// 상수 블록의 수명이다(9.4절). 장치 표현은 9.11절 1항: PerDraw는 장치 버퍼 없이 그리기마다
// SetConstantData, PerFrame은 SingleFrame 상수 버퍼(주인이 프레임 끝에 Release — 회수는 마지막
// 사용 fence), Persistent는 MultiFrame 상수 버퍼(바뀐 version만 Update)다.
enum class ConstantBlockLifetime : uint8 {
    PerDraw,
    PerFrame,
    Persistent
};

// 레이아웃 표 하나의 상수 블록이다(CPU 사본과, PerFrame·Persistent면 장치 상수 버퍼). identity는
// 프로세스 안 고유하고 version은 값을 바꿀 때마다 오른다(GLES 업로드 판단 — 9.4절).
//
// 사용 예(뷰 상수 — 프레임마다):
//   ConstantBlock view(allocator, ViewLayout, ConstantBlockLifetime::PerFrame);
//   view.Initialize();
//   view.SetValue("viewToClip", matrix);
//   view.Commit(device);                           // SingleFrame 상수 버퍼를 만든다
//   ApplyConstantBlock(commands, viewMap, view);   // 지도의 단계마다 SetConstantBuffer
//   ... 프레임 끝 ...
//   view.Release(device);                          // 마지막 사용 fence 뒤 해제된다
//
// 수명: 장치 버퍼는 소멸자가 놓지 않는다(장치를 모른다). 주인이 Release한다.
class ROCKS_RENDERBASE_API ConstantBlock {
public:
    ConstantBlock(Allocator& allocator, const shader::ShaderParameterLayout& layout,
                  ConstantBlockLifetime lifetime) noexcept;
    ConstantBlock(const ConstantBlock&) = delete;
    ConstantBlock& operator=(const ConstantBlock&) = delete;

    // 값 byte를 표 크기만큼 0으로 만든다. 실패: 할당 실패 OutOfMemory.
    Result<void> Initialize() noexcept;

    // 멤버 이름의 값을 바꾼다(bytes 크기 = 멤버 byte 수). 실패: 멤버 없음 NotFound, 크기 다름
    // InvalidArgument.
    Result<void> Set(StringView member, Span<const uint8> bytes) noexcept;

    template <class T>
    Result<void> SetValue(StringView member, const T& value) noexcept {
        return Set(member, Span<const uint8>(reinterpret_cast<const uint8*>(&value), sizeof(T)));
    }

    Span<const uint8> Bytes() const noexcept {
        return bytes_.View();
    }

    const shader::ShaderParameterLayout& Layout() const noexcept {
        return *layout_;
    }

    ConstantBlockLifetime Lifetime() const noexcept {
        return lifetime_;
    }

    uint64 Identity() const noexcept {
        return identity_;
    }

    uint64 Version() const noexcept {
        return version_;
    }

    // 지금 값을 장치 상수 버퍼에 올린다(9.11절 7항). PerDraw는 아무것도 하지 않는다. 이미 지금
    // version이면 아무것도 하지 않는다. PerFrame은 SingleFrame 버퍼를 만든다(값이 바뀐 뒤면 새로
    // 만들고 옛것을 Retire한다 — SingleFrame은 바꿀 수 없다). Persistent는 MultiFrame 버퍼를
    // 만들거나 UpdateConstantBuffer한다. 실패: Initialize 전 InvalidArgument, 장치 오류는 그 오류다
    // (그때 commit된 version은 그대로다).
    Result<void> Commit(graphics::Device& device) noexcept;

    // 장치 버퍼를 Retire한다(없으면 아무것도 하지 않는다). 다시 Commit하면 새로 만든다.
    void Release(graphics::Device& device) noexcept;

    // commit된 장치 버퍼다(PerDraw·Commit 전·Release 뒤는 null).
    graphics::ConstantBufferHandle Buffer() const noexcept {
        return buffer_;
    }

    // 장치 버퍼가 지금 값을 담는가(PerDraw는 늘 참 — 장치 버퍼가 없다).
    bool IsCommitted() const noexcept {
        return lifetime_ == ConstantBlockLifetime::PerDraw ||
               (!buffer_.IsNull() && committedVersion_ == version_);
    }

private:
    const shader::ShaderParameterLayout* layout_;
    ConstantBlockLifetime lifetime_;
    Array<uint8> bytes_;
    uint64 identity_ = 0;
    uint64 version_ = 0;
    graphics::ConstantBufferHandle buffer_;
    uint64 committedVersion_ = 0;
};

// 루트 상수 블록을 그리기별 상수로 올린다(9.4절 그리기별 상수). map이 루트 상수 표가 아니거나
// block의 표가 map의 표와 다르면 InvalidArgument, 장치 거부는 그 오류다.
ROCKS_RENDERBASE_API Result<void> ApplyDrawConstants(graphics::CommandContext& commands,
                                                     const ShaderBindingMap& map,
                                                     const ConstantBlock& block) noexcept;

// 상수 버퍼 표의 블록을 지도의 단계 자리마다 묶는다(9.11절 7항): PerDraw는 SetConstantData(값
// 복사), 그 밖은 commit된 버퍼를 SetConstantBuffer. 실패(InvalidArgument): map이 루트 상수 표,
// block의 표가 map의 표와 다름, PerFrame·Persistent인데 지금 값이 commit되지 않음.
// SetConstantData의 실패는 그 오류다(앞 자리는 이미 묶였을 수 있다).
ROCKS_RENDERBASE_API Result<void> ApplyConstantBlock(graphics::CommandContext& commands,
                                                     const ShaderBindingMap& map,
                                                     const ConstantBlock& block) noexcept;

// 상수 버퍼 handle(그래프 상수 등)을 지도의 상수 버퍼 자리마다 묶는다. map이 루트 상수 표면
// InvalidArgument다.
ROCKS_RENDERBASE_API Result<void>
ApplyConstantBuffer(graphics::CommandContext& commands, const ShaderBindingMap& map,
                    graphics::ConstantBufferHandle buffer) noexcept;

// GLES 업로드 판단(9.4절)이다: (program, 상수 버퍼 순번)마다 마지막으로 올린 (블록 identity,
// version)을 기억하고, 그리기 때 다를 때만 올리라고 답한다. program을 바꾸면 같은 블록도 그
// program에는 다시 올린다.
class ROCKS_RENDERBASE_API ConstantUploadTracker {
public:
    explicit ConstantUploadTracker(Allocator& allocator) noexcept : uploads_(allocator) {
    }

    // 올려야 하면 참을 돌려주고 올린 것으로 기록한다. 할당 실패면 참(늘 올림 — 정확성 우선)이다.
    bool NeedsUpload(uint64 program, uint32 slot, const ConstantBlock& block) noexcept;

    // program이 사라졌다(그 기록을 지운다).
    void ForgetProgram(uint64 program) noexcept;

    uint64 Uploads() const noexcept {
        return uploadCount_;
    }

    uint64 Skips() const noexcept {
        return skipCount_;
    }

private:
    struct Upload {
        uint64 program = 0;
        uint32 slot = 0;
        uint64 identity = 0;
        uint64 version = 0;
    };

    Array<Upload> uploads_;
    uint64 uploadCount_ = 0;
    uint64 skipCount_ = 0;
};

} // namespace rocks::render
