#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Graphics/Device.h>
#include <ShaderCore/ShaderBindingLayout.h>
#include <ShaderCore/ShaderParameterLayout.h>

// 파라미터 바인딩 지도와 상수 블록(Docs/Design/RenderBase 9.3·9.4절 RB4의 SH2분)이다. SH2는
// 손으로 쓴 레이아웃 표(ShaderSystem 7.2절 5항, ShaderCore ShaderParameterLayout.h)를 입력으로
// 쓴다(생성 표는 SH3). 상수 버퍼(cbuffer)를 장치에 묶는 일은 파라미터 묶음(GX6)이라, SH2에서
// 장치까지 가는 경로는 루트 상수(그리기별 상수, SetDrawConstants)뿐이다.
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
    // 아니면 표 이름과 같은 상수 버퍼를 선언한 단계마다 자리를 만든다. 규칙 위반은 InvalidArgument
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

// 상수 블록의 수명이다(9.4절). PerFrame 회수(프레임 slot GPU 완료)와 GPU 버퍼 표현은 GX6·GX8과
// 함께 한다 — SH2의 블록은 CPU 사본이다.
enum class ConstantBlockLifetime : uint8 {
    PerDraw,
    PerFrame,
    Persistent
};

// 레이아웃 표 하나의 CPU 상수 블록이다. identity는 프로세스 안 고유하고 version은 값을 바꿀 때마다
// 오른다(GLES 업로드 판단 — 9.4절).
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

private:
    const shader::ShaderParameterLayout* layout_;
    ConstantBlockLifetime lifetime_;
    Array<uint8> bytes_;
    uint64 identity_ = 0;
    uint64 version_ = 0;
};

// 루트 상수 블록을 그리기별 상수로 올린다(9.4절 그리기별 상수). map이 루트 상수 표가 아니거나
// block의 표가 map의 표와 다르면 InvalidArgument, 장치 거부는 그 오류다.
ROCKS_RENDERBASE_API Result<void> ApplyDrawConstants(graphics::CommandContext& commands,
                                                     const ShaderBindingMap& map,
                                                     const ConstantBlock& block) noexcept;

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
