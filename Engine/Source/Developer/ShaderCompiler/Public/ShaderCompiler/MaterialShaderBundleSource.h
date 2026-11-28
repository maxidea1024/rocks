#pragma once

#include <AssetCooker/CookArtifacts.h>
#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Hash/Sha256.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <ShaderCore/ShaderType.h>

// 머티리얼 셰이더 묶음과 그 원천이다(Docs/Design/ShaderSystem 16.5절 SSD87·SSD88, MaterialSystem
// 12.8절 MS3f).
//
// 셰이더 라이브러리 단계(ShaderLibraryCookStage)는 프로젝트 쿠킹에서 등록된 원천에게 이번 게시가
// 담을 쿠킹 패키지(CookArtifactRequest::packages)의 머티리얼 셰이더 묶음을 받아 형식마다 Project
// 라이브러리를 만든다. 재사용한(Cook이 불리지 않은) 패키지도 같은 길로 온다. 셰이더 시스템은
// 머티리얼을 모르므로 원천은 머티리얼 쪽(MaterialCook)이 구현하고, 모듈 수명 객체가 Startup에서
// MaterialShaderBundleSources()에 명시 등록한다(정적 초기화 순서에 기대지 않는다 — 12.3.1절 SSD41).
//
// 사용 예(MaterialCook 모듈):
//   class MaterialBundles final : public shader::MaterialShaderBundleSource {
//   public:
//       Result<void> Collect(const CookArtifactRequest& request,
//                            shader::MaterialShaderBundleSink& sink) noexcept override {
//           for (const CookedManifestEntry* package : request.packages) {
//               // 머티리얼 패키지면 형식별 셰이더 집합 id·사용 대상을 읽고 원문·traits를 조회한다.
//               shader::MaterialShaderBundle bundle;
//               bundle.formatId = "D3D12_SM6"; bundle.shaderSetId = setId; ...
//               if (auto added = sink.Add(bundle); !added) { return added; }
//           }
//           return Result<void>::Success();
//       }
//   };
//   Result<void> Startup() { return shader::MaterialShaderBundleSources().Register(bundles); }
//   void Shutdown() { (void)shader::MaterialShaderBundleSources().Unregister(bundles); }
namespace rocks::shader {

// 형식 하나의 머티리얼 셰이더 묶음 하나다(값은 빌림 — sink.Add가 복사한다).
struct MaterialShaderBundle {
    // 형식 id(프로필 shaderFormats의 것)다.
    StringView formatId{};
    // 셰이더 집합 id(MaterialSystem 8.6절 — 생성 원문·layout 서명·traits의 SHA-256)다. 묶음 id는
    // MaterialShaderBundleId(shaderSetId)다.
    Sha256Digest shaderSetId{};
    // 생성 원문(/Engine/Generated/Material.rsh 텍스트)이다.
    StringView materialSource{};
    // MaterialShaderTraits 정규 바이트(머티리얼 거르기 입력 — SSD86)다. 원천이 읽기로 검사한
    // 것이다.
    Span<const uint8> traits{};
    // 사용 대상 정점 입력 타입 id다(정점 입력 축 타입이 곱한다 — MaterialSystem 8.4절). 등록부에
    // 있어야 한다. 순서·중복은 상관없다.
    Span<const VertexInputTypeId> vertexInputTypes{};
    // 진단에 붙일 머티리얼 에셋이다.
    AssetId asset{};
};

// 원천의 출력 sink다(단계가 구현한다). 오류: 할당 실패, 계약 위반(형식 id 문법·빈 원문)
// InvalidArgument — 원천은 그 오류를 Collect 결과로 그대로 돌려준다.
class MaterialShaderBundleSink {
public:
    virtual ~MaterialShaderBundleSink() = default;

    // 묶음 하나를 복사해 둔다.
    virtual Result<void> Add(const MaterialShaderBundle& bundle) noexcept = 0;

    // 패키지별 실패(읽기·조회·재번역 실패, traits를 읽을 수 없음)를 그 머티리얼 asset의 오류
    // 진단으로 남긴다(쿠킹 보고서 — 오류가 있으면 게시하지 않는다). 상세 문구는 원천이 영어 Log로
    // 남긴다.
    virtual Result<void> Report(const CookDiagnostic& diagnostic) noexcept = 0;
};

// 머티리얼 셰이더 묶음 원천이다.
class MaterialShaderBundleSource {
public:
    virtual ~MaterialShaderBundleSource() = default;

    // request.packages의 머티리얼 셰이더 묶음을 프로필 shaderFormats의 형식마다 sink.Add로 넘긴다.
    // 패키지별 실패는 sink.Report로 남기고 다음 패키지로 간다(조용히 빼지 않는다). 쿠킹 단계 Run
    // 하나에서 한 번 부른다(단계 스레드). 오류: sink 오류(그대로), 할당 실패 — 그 밖의 오류도
    // 단계는 쿠킹 오류(DependencyFailed)로 보고한다.
    virtual Result<void> Collect(const CookArtifactRequest& request,
                                 MaterialShaderBundleSink& sink) noexcept = 0;
};

// 원천 칸 하나의 표다(CookArtifactStageTable과 같은 규칙: 등록·해제는 모듈 시작·종료 hook에서만,
// 단계가 도는 동안은 Busy).
class ROCKS_SHADERCOMPILER_API MaterialShaderBundleSourceTable final {
public:
    MaterialShaderBundleSourceTable() noexcept = default;
    MaterialShaderBundleSourceTable(const MaterialShaderBundleSourceTable&) = delete;
    MaterialShaderBundleSourceTable& operator=(const MaterialShaderBundleSourceTable&) = delete;

    // 오류: 이미 원천이 있음 AlreadyExists, 사용 중 Busy.
    Result<void> Register(MaterialShaderBundleSource& source) noexcept;

    // 오류: 이 원천이 아님 NotFound, 사용 중 Busy.
    Result<void> Unregister(MaterialShaderBundleSource& source) noexcept;

    // 등록된 원천이다(없으면 null).
    MaterialShaderBundleSource* Current() const noexcept {
        return source_;
    }

    void BeginUse() noexcept {
        ++uses_;
    }

    void EndUse() noexcept {
        --uses_;
    }

    bool InUse() const noexcept {
        return uses_ != 0;
    }

private:
    MaterialShaderBundleSource* source_ = nullptr;
    uint32 uses_ = 0;
};

// 프로세스 원천 표다(ShaderCompiler 모듈 소유).
ROCKS_SHADERCOMPILER_API MaterialShaderBundleSourceTable& MaterialShaderBundleSources() noexcept;

} // namespace rocks::shader
