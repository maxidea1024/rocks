#pragma once

#include <AssetCooker/CookArtifacts.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/StringView.h>
#include <ShaderCompiler/ShaderCookRegistrySource.h>
#include <ShaderCompiler/ShaderLibraryCook.h>

// 셰이더 라이브러리 비에셋 산출물 단계다(Docs/Design/ShaderSystem 12.3.1절 SSD11·SSD41, 16.1절
// SSD44–SSD46, AssetSystem 10.3절 3항). `asset-cook`이 에셋 쿠킹 뒤 이 단계를 불러 프로필
// `shaderFormats`의 형식마다 `Engine` 셰이더 라이브러리를 만들게 한다(UE 쿠커가 Global 셰이더
// 라이브러리를 저장하는 자리).
//
// Run 흐름(SSD45): 형식 목록(graphicsBackends 순서) → 형식 정의 대조(위반 InvalidSetting) → 등록부
// 원천에서 Freeze된 등록부(프로젝트 소속 타입이 있으면 Unsupported — ModuleLoader ML5b 전) → 원문
// /Engine·관리자·너겟(request.nuggets, null이면 engineOnly일 때 엔진 너겟 root) → 형식마다 메모리
// 라이브러리와 cookKey(SSD46) → 같은 cookKey의 재사용 후보면 Reuse, 아니면 staging 파일 + Produce.
// job 실패는 형식마다 진단 하나(UnsupportedCapability 또는 DependencyFailed — 상세는 Log)이고
// 게시하지 않는다. verify 쿠킹은 캐시 없이 다시 만든 byte와 대조한다(다르면 NuggetMismatch).
//
// 보통은 ShaderCompiler 모듈 수명 객체가 만들어 CookArtifactStages()에 등록한다(직접 만들 일은
// 시험뿐).
//
// 사용 예(시험):
//   ShaderLibraryCookStageConfig config;
//   config.allocator = &allocator;               // 여러 스레드의 동시 할당을 받는다
//   config.registry = &fixedRegistrySource;
//   config.sourceDirectories = mounts;           // 시험 원문 루트(/Plugin/<이름>)
//   ShaderLibraryCookStage stage(config);
//   CookArtifactStageTable table;
//   (void)table.Register(stage);
//   ... CookProjectRequest::stages = &table ...
namespace rocks::shader {

// 단계 정체성이다(processor = cookKey 머리 Guid, version은 단계 흐름·cookKey 배치가 바뀌면 바꾼다).
inline constexpr NuggetProcessorId ShaderLibraryCookStageId{
    ShaderLibraryCookerGuid, Guid{0x52534c434f4f4b31ull, 0x0000000000000001ull}};

// 단계가 원문 트리에 더 mount하는 디렉터리 하나다(시험 원문 루트 — 제품은 /Engine만).
struct ShaderCookSourceDirectory {
    // 가상 디렉터리(/Plugin/<이름> 또는 /Project).
    StringView virtualRoot;
    StringView directory;
};

struct ShaderLibraryCookStageConfig {
    // 관리자·결과·라이브러리 byte 할당자다(여러 스레드의 동시 할당을 받아야 한다).
    Allocator* allocator = nullptr;
    // 등록부 원천이다(필수).
    ShaderCookRegistrySource* registry = nullptr;
    // 엔진 폴더(Shaders·Config/ShaderFormats)다. 비면 Run마다 FindEngineDirectory다.
    StringView engineDirectory{};
    // 워커 실행 파일이다. 비면 이 실행 파일 옆의 ShaderCompileWorker다.
    StringView workerExecutable{};
    // /Engine 밖에 더 mount할 원문 디렉터리다.
    Span<const ShaderCookSourceDirectory> sourceDirectories{};
    // 워커 수(0이면 관리자 기본값)다.
    uint32 workerCount = 0;
};

class ROCKS_SHADERCOMPILER_API ShaderLibraryCookStage final : public CookArtifactStage {
public:
    // config의 문자열·원천·배열은 이 객체보다 오래 산다.
    explicit ShaderLibraryCookStage(const ShaderLibraryCookStageConfig& config) noexcept;

    CookArtifactKind Kind() const noexcept override {
        return CookArtifactKind::ShaderLibrary;
    }

    NuggetProcessorId Identity() const noexcept override {
        return ShaderLibraryCookStageId;
    }

    // 위 흐름이다. 쿠킹 규칙 위반·job 실패·환경 실패(엔진 폴더·워커·너겟 캐시·등록부 원천)는
    // sink.Report로 남기고 성공으로 끝낸다. 오류: sink 오류(그대로 — 계약 위반 InvalidArgument),
    // staging 파일 쓰기 오류, 관리자 Compile 자체의 오류, 할당 실패. request.profile·settings·
    // allocator·files가 null이거나 config가 비면 InvalidArgument다.
    Result<void> Run(const CookArtifactRequest& request, CookArtifactSink& sink) noexcept override;

private:
    ShaderLibraryCookStageConfig config_;
};

} // namespace rocks::shader
