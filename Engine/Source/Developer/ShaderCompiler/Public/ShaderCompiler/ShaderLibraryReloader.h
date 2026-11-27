#pragma once

#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Result.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>
#include <Platform/File.h>
#include <Platform/Sync.h>
#include <ShaderCompiler/ShaderDiagnostics.h>
#include <ShaderCompiler/ShaderLibraryCook.h>

// 개발 실행의 형식 라이브러리와 그 다시 만들기다(Docs/Design/ShaderSystem 16.3절 SSD69–SSD71, UE
// `recompileshaders changed`·저장 감시 재컴파일 대응). Development
// 공급자(DevelopmentShaderProvider)가 형식마다 한 번 Build로 메모리 라이브러리를 만들어 들고 있고,
// 원문이 바뀌면 Reload가 그 원문에 의존하는 셰이더가 있을 때 만든 형식을 모두 다시 만든다.
//
// - 의존 지도: 라이브러리를 만들 때 보고서의 sources(job마다의 전처리 include 폐포)를 합집합으로
// 쌓는다.
//   줄이지 않는다(조건부 include가 다음 컴파일에서 살아날 수 있다).
// - 다시 만들기: 처음과 같은 desc로 CompileShaderLibrary를 다시 부른다. 너겟 캐시가 내용 주소라
// 영향받지
//   않은 job은 적중이다. 새 byte가 이전과 같으면 revision을 올리지 않는다.
// - 실패: job이 하나라도 실패하면 그 형식의 라이브러리·revision을 바꾸지 않는다(이전 유지). 진단은
//   위치를 해석해 합친 목록으로 보고서에 둔다(SSD72).
// - 직전 다시 만들기가 실패했으면 의존 지도에 없는 원문 변경도 다시 시도한다(없던 include 파일을
// 만든
//   경우 — 실패한 전처리는 폐포를 남기지 않는다).
//
// 사용 예:
//   ShaderLibraryReloaderDesc desc;
//   desc.allocator = &allocator;
//   desc.registry = &registry;                  // Freeze됨
//   desc.manager = &manager;                    // Start됨
//   desc.formatDirectory = "<engine>/Config/ShaderFormats";
//   ShaderLibraryReloader libraries(desc);
//   ShaderLibraryCookReport report(allocator);
//   if (auto built = libraries.Build("D3D12_SM6", report); !built) { ... report.failures ... }
//   Array<uint8> bytes(allocator);
//   auto revision = libraries.CopyLibrary("D3D12_SM6", bytes);
//   ...
//   const StringView changed[] = {"/Engine/Private/Tonemap.rsf"};
//   ShaderReloadReport reload(allocator);
//   (void)libraries.Reload(changed, false, reload);
//
// 스레드: Build·Reload는 내부 잠금으로 차례로 돈다(관리자 Compile을 겹쳐 부르지 않는다).
// CopyLibrary·Revision·FormatCount는 어느 스레드에서나 부를 수 있고 다시 만드는 동안에도 짧은
// 잠금만 잡는다(렌더 lane을 막지 않는다 — SSD76).
namespace rocks::shader {

struct ShaderLibraryReloaderDesc {
    // 여러 스레드의 동시 할당을 받는 할당자다.
    Allocator* allocator = nullptr;
    // Freeze된 등록부다(이 객체보다 오래 산다).
    const ShaderTypeRegistry* registry = nullptr;
    ShaderLibraryKind library = ShaderLibraryKind::Engine;
    // 개발 전용 타입·퍼뮤테이션을 담는가(공급자는 true — SSD16).
    bool includeDevelopment = true;
    // Start된 관리자다(이 객체보다 오래 산다).
    ShaderCompileManager* manager = nullptr;
    // 형식 정의 디렉터리(<engine>/Config/ShaderFormats)다. 복사한다.
    StringView formatDirectory;
    // 모든 job을 debugSymbols=true로 만들고 symbolDirectory에 심볼 파일을 쓴다(SSD74·SSD75).
    bool debugSymbols = false;
    StringView symbolDirectory;
    // 파일 시스템(null이면 실제 파일 시스템)이다.
    platform::FileSystem* files = nullptr;
};

enum class ShaderReloadOutcome : uint8 {
    // 바뀐 원문에 의존하는 셰이더가 없어 다시 만들지 않았다.
    NoShaderAffected,
    // 다시 만들었지만 모든 형식의 byte가 같다(revision 그대로).
    Unchanged,
    // 한 형식 이상의 라이브러리가 바뀌었다(그 형식의 revision이 올랐다).
    Rebuilt,
    // job이 실패해 한 형식 이상을 바꾸지 못했다(그 형식은 이전 라이브러리 유지).
    Failed
};

ROCKS_SHADERCOMPILER_API const char* ShaderReloadOutcomeName(ShaderReloadOutcome outcome) noexcept;

// 다시 만들기 한 번의 결과다.
struct ShaderReloadReport {
    explicit ShaderReloadReport(Allocator& allocator) noexcept
        : diagnostics(allocator), failedType(allocator), failedFormat(allocator) {
    }

    ShaderReloadOutcome outcome = ShaderReloadOutcome::NoShaderAffected;
    // 영향받은 셰이더 타입 수(모두 다시 만들기면 등록부 전체)다.
    uint32 affectedTypes = 0;
    // 다시 만든 형식 수와 그 가운데 byte가 바뀐 형식 수다.
    uint32 formats = 0;
    uint32 changedFormats = 0;
    // 실패한 job 수(형식 합)다.
    uint32 failedJobs = 0;
    // 위치를 해석해 합친 실패 진단이다(SSD72).
    Array<ShaderDiagnostic> diagnostics;
    // 첫 실패 job의 타입 이름과 형식 id다(없으면 빈 값).
    String failedType;
    String failedFormat;
};

class ROCKS_SHADERCOMPILER_API ShaderLibraryReloader final {
public:
    explicit ShaderLibraryReloader(const ShaderLibraryReloaderDesc& desc) noexcept;
    ~ShaderLibraryReloader() noexcept;
    ShaderLibraryReloader(const ShaderLibraryReloader&) = delete;
    ShaderLibraryReloader& operator=(const ShaderLibraryReloader&) = delete;

    // formatId의 라이브러리를 처음 만든다(이미 있으면 아무 일도 하지 않고 성공). report는 그
    // 만들기의 보고서다. 오류: desc 누락 InvalidArgument, 형식 정의를 읽지 못함 NotFound, job 실패
    // DependencyFailed(report.failures), 그 밖은 CompileShaderLibrary의 오류.
    Result<void> Build(StringView formatId, ShaderLibraryCookReport& report) noexcept;

    // 만든 라이브러리 byte를 out에 복사하고 revision(1부터)을 돌려준다. 없으면 NotFound, 할당 실패
    // OutOfMemory다.
    Result<uint64> CopyLibrary(StringView formatId, Array<uint8>& out) const noexcept;

    // 형식의 revision이다(만들지 않은 형식은 0).
    uint64 Revision(StringView formatId) const noexcept;

    // 만든 형식 수다.
    usize FormatCount() const noexcept;

    // changed(가상 경로)가 바뀌었다. everything이면(감시 이벤트 손실) 의존과 관계없이 모두 다시
    // 만든다. report는 먼저 비운다. 실행 실패(할당·관리자 오류)만 Result 실패이고 job 실패는
    // report.outcome Failed다.
    Result<void> Reload(Span<const StringView> changed, bool everything,
                        ShaderReloadReport& report) noexcept;

    // virtualPath에 의존하는 셰이더 타입 이름 해시를 out에 둔다(out은 먼저 비운다 — 시험·진단).
    Result<void> DependentTypes(StringView virtualPath, Array<uint64>& out) const noexcept;

    // 직전 Reload가 실패했는가.
    bool LastReloadFailed() const noexcept;

    // 내부 상태다. 정의는 cpp에 있다.
    struct State;

private:
    UniquePtr<State> state_;
};

// report.failures의 진단을 해석해 out에 합친다(SSD72 — 같은 진단은 occurrences로 센다). 할당 실패만
// OutOfMemory다.
ROCKS_SHADERCOMPILER_API Result<void>
CollectShaderLibraryDiagnostics(const ShaderLibraryCookReport& report,
                                Array<ShaderDiagnostic>& out) noexcept;

} // namespace rocks::shader
