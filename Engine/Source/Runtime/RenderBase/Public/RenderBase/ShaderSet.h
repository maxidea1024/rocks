#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Graphics/Device.h>
#include <Platform/Sync.h>
#include <ShaderCore/ShaderBindingLayout.h>
#include <ShaderCore/ShaderLibrary.h>
#include <ShaderCore/ShaderLibraryContents.h>
#include <ShaderCore/ShaderTypeRegistry.h>

// 셰이더 집합(Docs/Design/RenderBase 9.2절 RB3, UE FShaderMap·FShaderCodeLibrary 런타임 쪽
// 대응)이다. 셰이더 라이브러리 하나(ShaderCore ShaderLibrary.h)를 열어 완결성·서명·타입 집합을
// 검사하고, 항목마다 장치 셰이더를 처음 쓸 때 만든다(UE FShaderMapResource::GetShader와 같은 지연
// 생성 — 경쟁하면 생성은 두 번일 수 있지만 게시는 한 번이고 진 쪽은 Retire한다).
//
// 공급 경로 두 가지(9.2절):
// - 쿠킹 라이브러리: OpenCooked(byte). 여는 즉시 검사하고 Ready가 된다.
// - 필요 시 컴파일 형식(GLES31_PC·쿠킹 라이브러리가 없는 개발 트리): OpenFromProvider(공급자).
//   집합은 WaitingForProvider로 열리고, 공급자가 CompleteProvision으로 byte를 넘기면 같은 검사를 한
//   뒤 Ready가 된다. 공급자가 없는 실행 파일(Release)이면 UnsupportedCapability다(SX14 — Runtime은
//   Developer 컴파일러에 의존하지 않고, Development 실행 파일이 ShaderProvider 구현을 넘긴다).
//
// 사용 예:
//   render::ShaderSetDesc desc;
//   desc.allocator = &allocator;
//   desc.device = &device;
//   desc.registry = &registry;                    // Freeze됨
//   desc.formatId = "D3D12_SM6";
//   render::ShaderSet set(desc);
//   if (auto opened = set.OpenFromProvider(provider); !opened) { ... set.ProblemDetail() ... }
//   (void)set.WaitUntilSettled();
//   auto vertex = render::TypedShader<FillVS>::Find(set);
//   auto handle = vertex.Value().DeviceShader();   // 첫 사용 생성
//
// 수명: 파이프라인 캐시 항목처럼 집합의 항목·layout을 붙잡는 쪽은 TryAcquireLease로 lease를
// 잡는다. Retire 뒤에는 새 lease·새 장치 셰이더 생성을 거부하고 게시한 장치 셰이더를 Retire한다.
// 집합은 lease가 모두 풀린 뒤 파괴한다(파괴 때 남은 lease는 Fatal 대신 assert로 잡는다).
namespace rocks::render {

class ShaderSet;

// 집합의 상태다.
enum class ShaderSetState : uint8 {
    // 아직 열지 않았다.
    Closed,
    // 공급자가 라이브러리를 만드는 중이다.
    WaitingForProvider,
    // 검사를 통과했다(조회·장치 셰이더 생성 가능).
    Ready,
    // 열기·검사가 실패했다(Problem·ProblemDetail).
    Failed,
    // Retire했다.
    Retired
};

ROCKS_RENDERBASE_API const char* ShaderSetStateName(ShaderSetState state) noexcept;

// 열기 실패의 원인이다. CorruptLibrary·UnsupportedLibraryVersion 밖은 UE IncompatibleCook에
// 해당한다 (실행 파일과 라이브러리가 서로 맞지 않는다).
enum class ShaderSetProblem : uint8 {
    None,
    // 라이브러리 byte가 형식 규칙을 어긴다(ShaderReadStatus::CorruptPackage).
    CorruptLibrary,
    // 라이브러리 형식 버전이 다르다.
    UnsupportedLibraryVersion,
    // 라이브러리 형식 id·backend·종류가 요청과 다르다.
    WrongFormat,
    // 라이브러리의 셰이더 타입 집합 SHA가 실행 파일 등록부와 다르다.
    TypeSetMismatch,
    // 등록된 타입 × 정점 입력 타입 × 퍼뮤테이션 가운데 라이브러리에 없는 항목이 있다(완결성).
    MissingEntry,
    // 항목의 파라미터 레이아웃 서명·단계가 실행 파일의 셰이더 타입과 다르다.
    SignatureMismatch,
    // 항목 값(바인딩 layout·GLES 머리)이 손상되었다.
    CorruptEntry,
    // 필요 시 컴파일 형식인데 공급자가 없다.
    NoProvider,
    // 공급자가 라이브러리를 만들지 못했다.
    ProviderFailed
};

ROCKS_RENDERBASE_API const char* ShaderSetProblemName(ShaderSetProblem problem) noexcept;

struct ShaderSetDesc {
    Allocator* allocator = nullptr;
    // 장치 셰이더를 만들 장치다. 집합보다 오래 산다.
    graphics::Device* device = nullptr;
    // Freeze된 등록부다(셰이더 타입과 정점 입력 타입 — 10.6절 1항). 집합보다 오래 산다.
    const shader::ShaderTypeRegistry* registry = nullptr;
    shader::ShaderLibraryKind library = shader::ShaderLibraryKind::Engine;
    // 기대하는 셰이더 형식 id(예: "D3D12_SM6")다. 라이브러리 형식 id와 같아야 하고 그 형식의
    // backend 코드 종류가 장치 코드 종류와 같아야 한다.
    StringView formatId;
};

// 공급자에게 넘기는 요청이다. 포인터는 CompleteProvision까지 유효하다.
struct ShaderProvisionRequest {
    StringView formatId;
    shader::ShaderLibraryKind library = shader::ShaderLibraryKind::Engine;
    // 셰이더 타입과 정점 입력 타입의 등록부다(Freeze됨).
    const shader::ShaderTypeRegistry* registry = nullptr;
};

// 필요 시 컴파일 형식의 라이브러리 공급자 interface다(SX14). Development 실행 파일이 Developer
// ShaderCompiler(CompileShaderLibrary)로 구현해 넘긴다.
class ShaderProvider {
public:
    // 요청을 받는다. 공급자는 라이브러리를 다 만들면(이 호출 안이나 다른 스레드에서) set의
    // CompleteProvision을 정확히 한 번 부른다. 요청을 받지 못하면 실패를 돌려주고
    // CompleteProvision을 부르지 않는다.
    virtual Result<void> RequestLibrary(const ShaderProvisionRequest& request,
                                        ShaderSet& set) noexcept = 0;

protected:
    ~ShaderProvider() = default;
};

// 항목 번호의 "없음" 표지다.
inline constexpr uint32 NoShaderSetEntry = 0xFFFF'FFFFu;

class ROCKS_RENDERBASE_API ShaderSet final {
public:
    explicit ShaderSet(const ShaderSetDesc& desc) noexcept;
    ~ShaderSet() noexcept;
    ShaderSet(const ShaderSet&) = delete;
    ShaderSet& operator=(const ShaderSet&) = delete;

    // 쿠킹 라이브러리를 연다(byte의 소유를 넘겨받는다). 검사를 통과하면 Ready다. 실패: 이미 열었음
    // Busy, desc 입력 오류 InvalidArgument, 라이브러리·항목 byte 손상 CorruptData(Problem
    // CorruptLibrary·CorruptEntry — Docs/Design/RenderBase 6.6절), 그 밖의 검사 실패
    // DependencyFailed(Problem·ProblemDetail), 할당 실패 OutOfMemory. 실패하면 상태는 Failed다.
    Result<void> OpenCooked(Array<uint8>&& library) noexcept;

    // 공급자로 연다. provider가 null이면 UnsupportedCapability(Problem NoProvider)다. 요청을 넘기면
    // WaitingForProvider이고 CompleteProvision 뒤 Ready·Failed가 된다(공급자가 이 호출 안에서
    // 끝내면 돌아올 때 이미 정해져 있다). 요청 실패는 그 오류(ProviderFailed)다.
    Result<void> OpenFromProvider(ShaderProvider* provider) noexcept;

    // 공급자가 부른다(어느 스레드에서나). outcome이 실패면 diagnostics를 ProblemDetail에 남기고
    // Failed다. 성공이면 library를 OpenCooked와 같이 검사한다. WaitingForProvider가 아니면
    // 무시한다.
    void CompleteProvision(Result<void> outcome, Array<uint8>&& library,
                           StringView diagnostics) noexcept;

    // WaitingForProvider가 끝날 때까지 기다린다(스레드 안전). 결과 상태를 돌려준다.
    ShaderSetState WaitUntilSettled() noexcept;

    // 스레드 안전 조회다.
    ShaderSetState State() const noexcept;
    ShaderSetProblem Problem() const noexcept;
    // 실패의 영어 설명(타입 이름·퍼뮤테이션 등)이다. 다음 열기까지 유효하다.
    StringView ProblemDetail() const noexcept;

    // 집합 epoch다(프로세스 안 고유, 셰이더 교체마다 새 집합 — 9.8절). 파이프라인 캐시 키에 든다.
    uint64 Epoch() const noexcept {
        return epoch_;
    }

    // --- Ready 상태에서만 의미가 있다(아니면 비었거나 NotReady) ---

    const shader::ShaderLibraryView& Library() const noexcept {
        return view_;
    }

    // (타입 이름 해시, 정점 입력 타입, 퍼뮤테이션)의 항목 번호다. 없으면 NotFound, Ready가 아니면
    // NotReady다.
    Result<uint32> FindEntry(uint64 typeNameHash,
                             shader::VertexInputTypeId vertexInputType = shader::NoVertexInputType,
                             uint32 permutationId = 0) const noexcept;

    uint32 EntryCount() const noexcept {
        return static_cast<uint32>(entries_.Size());
    }

    // 항목의 라이브러리 기록과 정규화 바인딩 layout(값 1)이다. 번호가 범위 밖이면 null이다.
    const shader::ShaderLibraryEntry* Entry(uint32 entry) const noexcept;
    const shader::ShaderBindingLayout* BindingLayout(uint32 entry) const noexcept;

    // 항목의 장치 셰이더다. 처음 부를 때 만든다(스레드 안전 — 장치의 생성 스레드 규칙은 호출자가
    // 지킨다, RenderBase 5.4절 GX11). 실패: Ready 아님 NotReady, Retire됨 Destroyed, 번호 밖
    // InvalidArgument, 항목의 런타임 확인 기능이 장치에 없음 Unsupported(만들지 않는다 — 9.2절),
    // 장치 생성 실패는 그 오류.
    Result<graphics::ShaderHandle> DeviceShader(uint32 entry) noexcept;

    // 게시한 장치 셰이더 수(시험·통계)다.
    uint32 PublishedShaders() const noexcept;

    // lease다(스레드 안전). Retire 뒤나 Ready가 아니면 false다.
    bool TryAcquireLease() noexcept;
    void ReleaseLease() noexcept;
    uint32 LeaseCount() const noexcept;

    // 새 lease·새 장치 셰이더 생성을 막고 게시한 장치 셰이더를 장치에 Retire한다(장치가 마지막 사용
    // fence 뒤에 해제한다). 만든 파이프라인은 장치 셰이더를 붙잡지 않으므로 그대로 쓸 수 있다.
    // 소유 스레드에서 부른다(Device::Retire 규칙).
    void Retire() noexcept;

private:
    struct EntryState {
        shader::ShaderLibraryEntry record;
        shader::ShaderBindingLayout layout;
        // 게시한 장치 셰이더(SlotHandle index << 32 | generation, 0 = 없음)다.
        uint64 published = 0;

        explicit EntryState(Allocator& allocator) noexcept : layout(allocator) {
        }
    };

    Result<void> Validate() noexcept;
    Result<void> FailWith(ShaderSetProblem problem, const char* detail) noexcept;
    void Settle() noexcept;
    bool RuntimeFeaturesSupported(uint32 features) const noexcept;

    Allocator& allocator_;
    ShaderSetDesc desc_;
    uint64 epoch_ = 0;
    mutable platform::Mutex gate_;
    platform::ManualResetEvent settled_{true};
    ShaderSetState state_ = ShaderSetState::Closed;
    ShaderSetProblem problem_ = ShaderSetProblem::None;
    char detail_[256] = {};
    Array<uint8> bytes_;
    shader::ShaderLibraryView view_;
    Array<EntryState> entries_;
    uint32 leases_ = 0;
    uint32 publishedCount_ = 0;
    bool retired_ = false;
};

// 타입 있는 셰이더 참조다(9.2절): (셰이더 타입, 집합)의 한 항목. T는 셰이더 타입 기술을
// `static constexpr shader::ShaderTypeDesc Type`로 가진다.
//
// 사용 예:
//   struct FillVS {
//       static constexpr shader::ShaderTypeDesc Type = shader::MakeShaderTypeDesc(
//           FillVSName, "/Plugin/Sandbox/Fill.rsf", "VSMain", shader::ShaderStageVertex,
//           shader::ShaderTypeOwner::EnginePlugin);
//   };
//   auto shader = render::TypedShader<FillVS>::Find(set);
template <class T>
class TypedShader {
public:
    TypedShader() noexcept = default;

    // 집합에서 찾는다. 실패는 ShaderSet::FindEntry와 같다.
    static Result<TypedShader>
    Find(ShaderSet& set, uint32 permutationId = 0,
         shader::VertexInputTypeId vertexInputType = shader::NoVertexInputType) noexcept {
        auto entry = set.FindEntry(T::Type.nameHash, vertexInputType, permutationId);
        if (!entry) {
            return Result<TypedShader>::Failure(entry.Error());
        }

        TypedShader shader;
        shader.set_ = &set;
        shader.entry_ = entry.Value();
        return Result<TypedShader>::Success(shader);
    }

    ShaderSet* Set() const noexcept {
        return set_;
    }

    uint32 Entry() const noexcept {
        return entry_;
    }

    bool IsNull() const noexcept {
        return set_ == nullptr;
    }

    // 장치 셰이더(첫 사용 생성)다. 비었으면 InvalidArgument다.
    Result<graphics::ShaderHandle> DeviceShader() const noexcept {
        if (set_ == nullptr) {
            return Result<graphics::ShaderHandle>::Failure(ErrorCode::InvalidArgument);
        }

        return set_->DeviceShader(entry_);
    }

private:
    ShaderSet* set_ = nullptr;
    uint32 entry_ = NoShaderSetEntry;
};

} // namespace rocks::render
