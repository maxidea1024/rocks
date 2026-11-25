#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Graphics/Device.h>
#include <Platform/Sync.h>
#include <RenderBase/ShaderSet.h>

// 파이프라인 캐시(Docs/Design/RenderBase 9.7절 RB4의 SH2분, UE PipelineStateCache 대응)다.
// 그래픽 키는 셰이더 집합 epoch·VS·PS 항목과 고정 기능 상태 전체(정점 배치·출력 형식·블렌드·깊이·
// 래스터·그리기별 상수 크기·slot 이름 표)다. compute 키는 epoch·CS 항목·그리기별 상수 크기·slot
// 이름 표다(Docs/Design/RenderBase 4.4절 "GX3·GX4 compute 연결 설계" (1)). 두 키는 종류 표지로
// 나뉜다. 키가 같으면 같은 장치 파이프라인을 돌려준다. 항목은 셰이더 집합의 lease를 잡고, 집합을
// Retire할 때 RetireSet으로 그 epoch 항목을 함께 Retire한다(9.7절). 그래픽은 만들기 전에 VS·PS 단계
// 사이 연결을 검사한다(ShaderCore CheckShaderStageLink, ShaderSystem SSD37). 비동기 미리 준비·기록
// 파일은 후속이다.
//
// 사용 예:
//   render::PipelineCache cache(allocator, device);
//   render::GraphicsPipelineRequest request;
//   request.set = &set;
//   request.vertexEntry = vertex.Entry();
//   request.pixelEntry = pixel.Entry();
//   request.state.colorFormats[0] = PixelFormat::RGBA8Unorm;
//   auto pipeline = cache.FindOrCreate(request);   // 두 번째 호출은 적중
//
// 사용 예(compute):
//   render::ComputePipelineRequest compute;
//   compute.set = &set;
//   compute.computeEntry = reduce.Entry();          // TypedShader<ReduceCS>::Find(set)
//   compute.state.drawConstantBytes = 16;
//   compute.state.bindings.writableBuffers[0] = "Output";
//   auto pipeline = cache.FindOrCreate(compute);
namespace rocks::render {

// 그래픽 파이프라인 요청이다. state의 셰이더 handle 칸은 무시한다(캐시가 집합에서 채운다).
// state의 정점 배치 Span은 호출 동안만 유효하면 된다(키에 복사한다).
struct GraphicsPipelineRequest {
    ShaderSet* set = nullptr;
    uint32 vertexEntry = NoShaderSetEntry;
    uint32 pixelEntry = NoShaderSetEntry;
    graphics::PipelineDesc state;
};

// compute 파이프라인 요청이다. state.computeShader는 무시한다(캐시가 집합에서 채운다).
struct ComputePipelineRequest {
    ShaderSet* set = nullptr;
    uint32 computeEntry = NoShaderSetEntry;
    graphics::ComputePipelineDesc state;
};

class ROCKS_RENDERBASE_API PipelineCache {
public:
    PipelineCache(Allocator& allocator, graphics::Device& device) noexcept;
    ~PipelineCache() noexcept;
    PipelineCache(const PipelineCache&) = delete;
    PipelineCache& operator=(const PipelineCache&) = delete;

    // 키가 같은 항목이 있으면 그 파이프라인(적중), 없으면 장치 셰이더(첫 사용 생성)와 장치
    // 파이프라인을 만들어 담는다(미적중). 스레드 안전(장치 생성 규칙은 호출자가 지킨다). 실패:
    // 집합 없음·항목 번호 밖·단계가 VS·PS가 아님 InvalidArgument, VS·PS 연결 위반 InvalidArgument
    // (LastLinkFailure), 집합 Ready 아님 NotReady·Retire됨 Destroyed, 장치 셰이더·파이프라인 생성
    // 실패는 그 오류. 실패는 담지 않는다(다음 호출이 다시 시도한다).
    Result<graphics::PipelineHandle> FindOrCreate(const GraphicsPipelineRequest& request) noexcept;

    // compute 판이다. 규칙은 위와 같고 단계 검사는 CS 하나다(단계 사이 연결 검사는 없다). 실패:
    // 집합 없음·항목 번호 밖·단계가 CS가 아님 InvalidArgument, 집합 Ready 아님 NotReady·Retire됨
    // Destroyed, 장치 셰이더·파이프라인 생성 실패는 그 오류(Device::CreateComputePipeline).
    Result<graphics::PipelineHandle> FindOrCreate(const ComputePipelineRequest& request) noexcept;

    // 집합 epoch의 항목을 모두 장치에 Retire하고 lease를 놓는다. 소유 스레드에서 부른다.
    void RetireSet(const ShaderSet& set) noexcept;

    // 모든 항목을 Retire한다(파괴자도 부른다).
    void Clear() noexcept;

    uint64 Hits() const noexcept;
    uint64 Misses() const noexcept;
    usize Size() const noexcept;

    // 마지막 연결 검사 실패의 영어 이유다(정적 문구, 없으면 빈 문자열).
    const char* LastLinkFailure() const noexcept;

private:
    struct Entry {
        explicit Entry(Allocator& allocator) noexcept : key(allocator) {
        }

        Array<uint8> key;
        uint64 hash = 0;
        uint64 epoch = 0;
        ShaderSet* set = nullptr;
        graphics::PipelineHandle pipeline;
    };

    // 잠금 안에서 키가 같은 항목을 찾는다(없으면 null).
    const Entry* FindLocked(const Entry& candidate) const noexcept;
    // 잠금 안에서 미적중 항목을 담는다: lease를 잡은 뒤 create로 장치 파이프라인을 만든다.
    template <class Create>
    Result<graphics::PipelineHandle> InsertLocked(Entry&& candidate, ShaderSet& set,
                                                  Create&& create) noexcept;

    Allocator& allocator_;
    graphics::Device& device_;
    mutable platform::Mutex gate_;
    Array<Entry> entries_;
    uint64 hits_ = 0;
    uint64 misses_ = 0;
    const char* linkFailure_ = "";
};

} // namespace rocks::render
