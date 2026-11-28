#pragma once

#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Lifetime/ModuleLease.h>
#include <Core/Memory/Allocator.h>
#include <Core/Module/HostServiceKey.h>
#include <Core/Result.h>
#include <ShaderCore/ShaderType.h>

// 셰이더 타입 등록부다(Docs/Design/ShaderSystem 12.2.1절 SSD3).
//
// 셰이더 타입을 정의하는 모듈의 수명 객체가 Startup에서 자기 desc 목록을 등록하고 Shutdown에서
// 거둔다(ShaderTypeModuleRegistration). 등록부는 호스트 서비스다: 호스트(엔진 부트·쿠커·시험)가
// 등록부를 만들어 모듈 단계 실행 전에 HostServices에 올리고, 모든 모듈을 시작한 뒤
// Freeze한다(엔진은 렌더 lane 시작 전). Freeze 뒤에는 읽기 전용이고 이름 해시순으로 정렬되어
// 있다(등록 순서는 결과에 영향이 없다). UE는 정적 생성자가 전역 연결 목록에
// 넣지만(IMPLEMENT_GLOBAL_SHADER, GlobalShader.h:433) Rocks는 정적 초기화 순서에 기대지 않는다.
//
// 등록 검사(실패하면 InvalidArgument이고 그 목록 전체가 등록되지 않는다 — 진단은 LastFailure):
// - desc가 null이 아니고 이름이 ASCII [A-Za-z0-9_](첫 글자는 숫자가 아님) MaxShaderTypeNameBytes
//   이하이며 nameHash = ShaderTypeNameHash(name)이다.
// - 같은 이름이 이미 있거나(대소문자 무시) 다른 이름이 같은 해시다(충돌).
// - 원문 경로가 가상 경로 규칙(ShaderVirtualPath.h)을 지키고 엔트리(.rsf)이며 루트가 소속과 맞는다.
// - 엔트리 함수 이름이 식별자이고, 단계가 ShaderStageBits 한 비트이며, 요구 기능·표지가 어휘
// 안이다.
// - 퍼뮤테이션 도메인이 ValidateShaderPermutationDomain을 통과한다.
// - 파라미터 레이아웃 표 목록이 ValidateShaderParameterLayouts를 통과한다(7.2절 5항, SSD27).
// - 거르기 칸이 표지와 맞는다: ShaderTypeMaterialShader 타입은 materialShouldCompile만, 그 밖은
//   shouldCompile만 쓴다(16.5절 SSD86 — 타입마다 거르기 하나).
// Freeze 뒤 등록은 ScopeClosed다(등록 창이 닫혔다).
//
// 정점 입력 타입(ShaderVertexInputType — RenderBase 10.6절 1항)도 이 등록부가 받고 같은 Freeze로
// 닫는다(UE도 vertex factory 타입과 셰이더 타입을 같은 초기화 단계에서 다룬다). 검사(실패하면 목록
// 전체 거부, InvalidArgument): 이름이 셰이더 타입 이름과 같은 문법, id = ShaderTypeNameHash(name),
// 정점 입력 타입끼리 이름·해시 중복 없음, 원문이 가상 경로 규칙을 지키는 .rsh이고 <루트>/Generated
// 아래가 아니며 루트가 소속과 맞음. Freeze 뒤에는 id 순이다. 쿠커·공급자·셰이더 집합은
// VertexInputTypes()를 쓴다.
//
// 사용 예(호스트):
//   ShaderTypeRegistry registry(allocator);
//   (void)loader.Services().Register(registry);
//   ... 모듈 시작(각 모듈이 등록) ...
//   if (auto frozen = registry.Freeze(); !frozen) { ... }
//   const ShaderTypeDesc* type = registry.Find(FullscreenTriangleVS.hash);
//   ...
//   loader.Services().Unregister(registry);            // 모듈을 내린 뒤
//
// 스레드: 등록·해제·Freeze는 로더 스레드에서 한다. Freeze 뒤 Find·Types는 여러 스레드가 동시에
// 불러도 된다(그동안 해제하지 않는다 — 해제는 모듈을 내리는 종료 단계에서만).
namespace rocks {

class ModuleStartContext;
class HostServices;

} // namespace rocks

namespace rocks::shader {

// 등록 실패의 진단이다(마지막 실패 하나 — 영어 문구는 정적 수명).
struct ShaderTypeRegistrationFailure {
    ErrorCode code = ErrorCode::None;
    // 실패한 타입 이름(잘릴 수 있다)이다. 타입과 무관하면 빈 문자열이다.
    char typeName[MaxShaderTypeNameBytes + 1] = {};
    const char* reason = "";
};

class ROCKS_SHADERCORE_API ShaderTypeRegistry final {
public:
    // allocator는 표 저장소용이며 이 객체보다 오래 살아야 한다.
    explicit ShaderTypeRegistry(Allocator& allocator) noexcept;
    ShaderTypeRegistry(const ShaderTypeRegistry&) = delete;
    ShaderTypeRegistry& operator=(const ShaderTypeRegistry&) = delete;

    // types를 등록한다(위 검사). desc는 등록한 동안 살아 있어야 한다(보통 정적 수명). 실패하면
    // 아무것도 등록하지 않고 LastFailure에 이유를 둔다. 오류: 검사 위반 InvalidArgument, Freeze 뒤
    // ScopeClosed, 표 할당 실패.
    Result<void> Register(Span<const ShaderTypeDesc* const> types) noexcept;

    // types를 거둔다(등록한 desc 포인터가 같은 항목만). 없는 항목은 건너뛴다. Freeze 뒤에도 부를 수
    // 있다(모듈 종료 단계 — 그동안 Find를 부르지 않는다).
    void Unregister(Span<const ShaderTypeDesc* const> types) noexcept;

    // 등록을 닫고 이름 해시순으로 정렬한다. 두 번째 호출은 아무것도 하지 않는다.
    Result<void> Freeze() noexcept;

    bool IsFrozen() const noexcept {
        return frozen_;
    }

    // nameHash의 타입이다. 없으면 null이다. Freeze 전에는 선형 탐색, 뒤에는 이진 탐색이다.
    const ShaderTypeDesc* Find(uint64 nameHash) const noexcept;

    // 등록된 타입이다(Freeze 뒤에는 이름 해시순).
    Span<const ShaderTypeDesc* const> Types() const noexcept {
        return types_.View();
    }

    // 등록된 타입 해시 목록(정렬)의 SHA-256이다(SSD3 — 쿠킹한 모듈 집합과 런타임 모듈 집합의 대조).
    // Freeze 뒤에만 의미가 있다. 정점 입력 타입은 넣지 않는다(완결성 검사가 잡는다 — RenderBase
    // 10.6절 1항).
    Sha256Digest TypeSetDigest() const noexcept;

    // 정점 입력 타입을 등록한다(위 검사). 문자열은 등록한 동안 살아 있어야 한다(보통 정적 수명).
    // 오류: 검사 위반 InvalidArgument, Freeze 뒤 ScopeClosed, 표 할당 실패.
    Result<void> RegisterVertexInputTypes(Span<const ShaderVertexInputType> inputs) noexcept;

    // 정점 입력 타입을 거둔다(id와 이름이 같은 항목). 없는 항목은 건너뛴다.
    void UnregisterVertexInputTypes(Span<const ShaderVertexInputType> inputs) noexcept;

    // 등록된 정점 입력 타입이다(Freeze 뒤에는 id 순 — EnumerateShaderLibraryCombinations의 입력).
    Span<const ShaderVertexInputType> VertexInputTypes() const noexcept {
        return vertexInputs_.View();
    }

    // id의 정점 입력 타입이다. 없으면 null이다.
    const ShaderVertexInputType* FindVertexInputType(VertexInputTypeId id) const noexcept;

    const ShaderTypeRegistrationFailure& LastFailure() const noexcept {
        return failure_;
    }

private:
    bool Check(const ShaderTypeDesc* desc, Span<const ShaderTypeDesc* const> batch,
               usize index) noexcept;
    bool CheckVertexInput(const ShaderVertexInputType& input,
                          Span<const ShaderVertexInputType> batch, usize index) noexcept;
    void Fail(ErrorCode code, StringView typeName, const char* reason) noexcept;

    Array<const ShaderTypeDesc*> types_;
    Array<ShaderVertexInputType> vertexInputs_;
    ShaderTypeRegistrationFailure failure_;
    bool frozen_ = false;
};

// 셰이더 타입을 정의하는 모듈의 수명 객체가 쓰는 등록
// 도우미다(Docs/Design/ModuleLoader 3.12절). Startup에서 호스트 서비스의 ShaderTypeRegistry를
// 찾아 types를 등록하고, 등록한 동안 모듈 lease를 잡는다(desc가 이 모듈의 데이터다). 호스트가
// 등록부를 올리지 않았으면(셰이더를 쓰지 않는 도구) 아무 일도 하지 않는다.
//
// 사용 예(모듈 cpp):
//   (정점 입력 타입도 등록하면 ShaderTypeModuleRegistration shaders_{Types, VertexInputs})
//   class RendererModule final : public ModuleImplementation {
//   public:
//       Result<void> Startup(ModuleStartContext& context) noexcept override {
//           return shaders_.Startup(context);
//       }
//       void Shutdown() noexcept override { shaders_.Shutdown(); }
//   private:
//       shader::ShaderTypeModuleRegistration shaders_{ModuleShaderTypes};
//   };
class ROCKS_SHADERCORE_API ShaderTypeModuleRegistration final {
public:
    explicit ShaderTypeModuleRegistration(
        Span<const ShaderTypeDesc* const> types,
        Span<const ShaderVertexInputType> vertexInputs = {}) noexcept
        : types_(types), vertexInputs_(vertexInputs) {
    }

    ~ShaderTypeModuleRegistration() noexcept {
        Shutdown();
    }

    ShaderTypeModuleRegistration(const ShaderTypeModuleRegistration&) = delete;
    ShaderTypeModuleRegistration& operator=(const ShaderTypeModuleRegistration&) = delete;

    // context의 호스트 서비스로 Attach한다.
    Result<void> Startup(ModuleStartContext& context) noexcept;

    // services에 등록부가 있으면 types와 정점 입력 타입을 등록하고 lease를 잡는다. 등록
    // 실패(등록부의 오류)는 그대로 돌려준다(모듈 시작 실패 — 앞서 등록한 셰이더 타입은 거둔다).
    // 이미 붙어 있으면 InvalidArgument다.
    Result<void> Attach(HostServices& services, ModuleLease lease) noexcept;

    // 등록을 거두고 lease를 놓는다. 붙어 있지 않으면 아무 일도 하지 않는다.
    void Shutdown() noexcept;

    // 등록한 등록부다(없으면 null).
    ShaderTypeRegistry* Registry() const noexcept {
        return registry_;
    }

private:
    Span<const ShaderTypeDesc* const> types_;
    Span<const ShaderVertexInputType> vertexInputs_;
    ShaderTypeRegistry* registry_ = nullptr;
    ModuleLease lease_;
};

} // namespace rocks::shader

ROCKS_DECLARE_HOST_SERVICE(rocks::shader::ShaderTypeRegistry, ROCKS_SHADERCORE_API)
