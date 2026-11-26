#pragma once

#include <AssetCooker/CookProcessor.h>
#include <AssetDatabase/AssetDatabaseLock.h>
#include <AssetDatabase/AssetImportArguments.h>
#include <AssetDatabase/AssetService.h>
#include <AssetDatabase/ProjectLayout.h>
#include <AssetRuntime/AssetTypeHooks.h>
#include <AssetRuntime/PackageReader.h>
#include <Core/Commandlet/Commandlet.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/NamePool.h>
#include <Core/Text/StringBuilder.h>
#include <Object/ModuleTypeRegistrations.h>
#include <Object/TypeRegistry.h>
#include <ReflectionText/TextDiagnostics.h>
#include <Task/WorkerScheduler.h>

namespace rocks {

class ModuleLoader;

} // namespace rocks

// 에셋 명령(asset-*)의 공용 도구다(Docs/Design/AssetSystem 13절): 출력 쓰기, 실패 줄,
// 프로젝트 찾기·열기, 색인 읽기 옵션.
namespace rocks::commandlet_host {

// 출력 한 줄의 인라인 byte 수다(넘으면 allocator로 자란다).
constexpr usize LineBytes = 512;

inline Result<int32> Write(CommandletOutput& output, CommandletStream stream, StringView text,
                           int32 code) noexcept {
    auto written = output.write(output.context, stream, text);
    return written ? Result<int32>::Success(code) : Result<int32>::Failure(written.Error());
}

// "<command>: <서식>\n"을 표준 오류에 쓰고 code로 돌려준다.
template <class... TArgs>
Result<int32> Fail(CommandletOutput& output, Allocator& allocator, StringView command, int32 code,
                   StringView format, const TArgs&... args) noexcept {
    StringBuilder<LineBytes> text(allocator);
    if (auto formatted = text.TryAppendFormat("{}: ", command); !formatted) {
        return Result<int32>::Failure(formatted.Error());
    }

    if (auto formatted = text.TryAppendFormat(format, args...); !formatted) {
        return Result<int32>::Failure(formatted.Error());
    }

    if (auto formatted = text.TryAppend("\n"); !formatted) {
        return Result<int32>::Failure(formatted.Error());
    }

    return Write(output, CommandletStream::Error, text.View(), code);
}

// 옵션의 프로젝트(없으면 현재 디렉터리에서 위로 찾은 것)로 layout을 연다. 실패하면 오류를 출력하고
// 그 종료 코드를 failure에 둔다.
ROCKS_ASSETCOMMANDLETS_API bool OpenProject(CommandletOutput& output, Allocator& allocator,
                                            StringView command, StringView project,
                                            ProjectLayout& layout, Result<int32>& failure) noexcept;

// 에디터 패키지 색인·열기의 읽기 옵션이다(에디터 종류, 이 엔진의 버전 — 더 새 엔진의 패키지는
// NewerVersion).
ROCKS_ASSETCOMMANDLETS_API PackageReadOptions IndexReadOptions() noexcept;

// 세션의 임포터 등록이다(7.1절 18 A2f1b — 타입 등록 뒤·서비스 만들기 전에 부른다): table(세션
// TypeRegistry에 묶인 표)에 임포터를 등록하고 토큰을 tokens에 둔다(세션이 서비스 뒤에 해제한다).
// 실패하면 그 오류로 명령이 실패한다. 제품은 실제 임포터를(A5 텍스처), 시험은 시험 임포터를 준다.
struct AssetImporterSetup {
    Result<void> (*registerImporters)(void* state, AssetImporterTable& table,
                                      Array<AssetImporterRegistration>& tokens) noexcept = nullptr;
    void* state = nullptr;
};

// 세션의 에셋 타입 hook 등록이다(Docs/Design/AssetSystem 12.12절 — 설정 정규화 hook 등, 임포터
// 등록과 같은 때): table(세션 hook 표)에 hook을 등록하고 토큰을 tokens에 둔다(세션이 표보다 먼저
// 해제한다). 실패하면 그 오류로 명령이 실패한다. 제품은 TextureTools RegisterTextureTypeHooks를
// 건다.
struct AssetTypeHookSetup {
    Result<void> (*registerHooks)(void* state, AssetTypeHookTable& table,
                                  Array<AssetTypeHookRegistration>& tokens) noexcept = nullptr;
    void* state = nullptr;
};

// asset-cook의 쿠킹 처리기 등록이다(Docs/Design/AssetSystem 12.14절 — 표를 만든 뒤 부른다):
// table(기본 처리기를 fallback으로 둔 세션 표)에 처리기를 등록한다. 처리기는 state가 소유하고 명령
// 동안 산다. 실패하면 그 오류로 명령이 실패한다. 없으면 기본 처리기만 쓴다. 제품은 TextureTools
// RegisterTextureCookProcessors를 건다.
struct AssetCookProcessorSetup {
    Result<void> (*registerProcessors)(void* state, CookProcessorTable& table) noexcept = nullptr;
    void* state = nullptr;
};

// 에셋 명령의 문맥이다(commandlet_host::Run이 건다): 작업 메모리와 Launch의 프로세스 모듈 로더,
// 에셋 타입 모듈 이름(비면 AssetTypeModules 전부 — 시험은 fixture 모듈), 임포터 등록, 타입 hook
// 등록, 임포트 예약 인자 번역기(12.12절 — 없으면 예약 인자는 사용법 오류), 쿠킹 처리기 등록.
struct AssetCommandContext {
    Allocator* allocator = nullptr;
    ModuleLoader* modules = nullptr;
    StringView typeModule{};
    AssetImporterSetup importers{};
    AssetTypeHookSetup typeHooks{};
    AssetImportTranslator importTranslator{};
    AssetCookProcessorSetup cookProcessors{};
    // 엔진 디렉터리 재정의다(비면 FindEngineDirectory — 제품 호스트 CommandletHost는 두지 않는다).
    // 명령 시험이 임시 엔진 디렉터리를 넣어 실제 저장소 `Engine/Saved`를 건드리지 않게
    // 한다(10.3.1절 A4f4 4번).
    StringView engineDirectory{};
};

// 에셋 타입 등록이다(명령 하나 동안 — Docs/Design/AssetSystem 7.1절 15 A2c3b, ModuleLoader
// 3.11절): 에셋 타입 모듈(AssetTypeModules — AssetRuntime은 그 의존)을 시작하고 이 TypeRegistry의
// 타입 등록기를 붙인다. Name pool도 둔다. 소멸이 등록기를 떼고 Registry·pool을 닫는다.
class ROCKS_ASSETCOMMANDLETS_API AssetCommandTypes {
public:
    // module은 시작할 에셋 타입 모듈이다(비면 AssetTypeModules 전부).
    AssetCommandTypes(Allocator& allocator, ModuleLoader& modules, StringView module = {}) noexcept;
    ~AssetCommandTypes() noexcept;
    AssetCommandTypes(const AssetCommandTypes&) = delete;
    AssetCommandTypes& operator=(const AssetCommandTypes&) = delete;

    // 모듈을 시작하고 등록기를 붙인 뒤 pool을 연다. 실패하면 오류(모듈 시작 실패는 그 report의
    // 오류 — 없으면 Unsupported)다.
    Result<void> Start() noexcept;

    TypeRegistry& Types() noexcept {
        return types_;
    }

    NamePool& Names() noexcept {
        return names_;
    }

    // 시작하는 에셋 타입 모듈 이름이다(실패 줄이 쓴다 — 목록이면 Start가 실패한 모듈, 시작 전이나
    // 성공 뒤에는 첫 모듈).
    StringView Module() const noexcept {
        return module_;
    }

private:
    ModuleLoader* modules_;
    StringView module_;
    // module이 비어 AssetTypeModules 전부를 시작하는지다.
    bool defaultModules_ = false;
    TypeRegistry types_;
    ModuleTypeRegistrations registrations_;
    NamePool names_;
    bool attached_ = false;
    bool namesReady_ = false;
};

// 에셋 명령이 시작하는 에셋 타입 모듈들이다(차례로 시작 — 각 의존 폐포의 타입이 등록된다,
// Docs/Design/AssetSystem 12.8절). 시험은 AssetCommandContext::typeModule로 모듈 하나를 준다.
inline constexpr StringView AssetTypeModules[] = {"Animation", "Mesh", "Texture"};

// 대입 진단 줄의 진단 이름이다(AS 13절 `<command>: <진단>: <경로>: <설명>`). 숨김 열거자 입력
// (HiddenEnumerator — 저장 값으로만 유효한 값)은 에셋 설정 진단 InvalidSetting으로 낸다
// (Docs/Design/AssetSystem 12.8절, ReflectionExtensions "A5가 InvalidSetting으로 바꾼다"). 그 밖은
// ReflectionText 이름 그대로다.
ROCKS_ASSETCOMMANDLETS_API StringView
AssetTextDiagnosticName(reflection_text::TextDiagnosticCode code) noexcept;

// 공통 옵션 --wait=<초>의 상한이다(하루 — 밖이면 사용법 오류).
inline constexpr uint32 MaxWaitSeconds = 86400;
inline constexpr uint32 MillisecondsPerSecond = 1000;

// --wait 값(10진 숫자만, 0–MaxWaitSeconds)을 밀리초로 푼다. 비어 있으면 0이다. 형식·범위 위반은
// false다.
ROCKS_ASSETCOMMANDLETS_API bool ParseWaitMilliseconds(StringView text, uint32& out) noexcept;

// `--wait` 마감 시각 하나에서 남은 밀리초다(두 잠금이 대기를 나눠 쓴다 — Docs/Design/AssetSystem
// 10.3.1절 A4f4 1번): startNanoseconds부터 nowNanoseconds까지 지난 시간을 waitMilliseconds에서
// 빼고, 다 썼으면 0이다(now가 start보다 앞이면 지난 시간 0). 값은 단조
// 시계(platform::MonotonicNanoseconds)다.
ROCKS_ASSETCOMMANDLETS_API uint32 RemainingWaitMilliseconds(uint32 waitMilliseconds,
                                                            uint64 startNanoseconds,
                                                            uint64 nowNanoseconds) noexcept;

// 에셋 Registry의 문맥 id다(명령 하나의 에디터 집합).
inline constexpr ContextId CommandletAssetContext{0x524f434b41535345ull, 1};

// 명령 하나의 실행 상태다: 프로젝트·타입·서비스.
// 색인 검색의 파일 읽기를 나눌 worker 수다(Docs/Design/AssetSystem 7.1절 16 A2d2 — 패키지
// 5만 개 측정에서 3개가 가장 빨랐고 더 늘리면 느려졌다).
inline constexpr uint32 CommandletScanWorkers = 3;

// 명령 하나 동안의 검색 scheduler다. 시작하지 못하거나(스레드 생성 실패) 논리 프로세서가 하나면
// Get이 null이고 검색은 직렬이다(결과가 같다). allocator는 여러 스레드의 동시 호출을 받는다
// (프로세스 기본 allocator).
class ROCKS_ASSETCOMMANDLETS_API ScanScheduler {
public:
    explicit ScanScheduler(Allocator& allocator) noexcept;
    ~ScanScheduler() noexcept;
    ScanScheduler(const ScanScheduler&) = delete;
    ScanScheduler& operator=(const ScanScheduler&) = delete;

    WorkerScheduler* Get() noexcept {
        return started_ ? &scheduler_ : nullptr;
    }

private:
    WorkerScheduler scheduler_;
    bool started_ = false;
};

struct EditSession {
    explicit EditSession(const AssetCommandContext& command) noexcept
        : context(&command), layout(*command.allocator), scan(*command.allocator),
          types(*command.allocator, *command.modules, command.typeModule),
          hooks(*command.allocator, types.Types()), hookTokens(*command.allocator),
          importers(*command.allocator, types.Types()), importerTokens(*command.allocator),
          warnings(*command.allocator), diagnostics(*command.allocator) {
    }

    const AssetCommandContext* context;
    ProjectLayout layout;
    // 서비스(색인)보다 먼저 만들고 나중에 없앤다(서비스가 빌린다).
    ScanScheduler scan;
    AssetCommandTypes types;
    AssetTypeHookTable hooks;
    // hook 등록 토큰이다(표보다 먼저 사라진다 — 선언 순서).
    Array<AssetTypeHookRegistration> hookTokens;
    // 임포터 표와 등록 토큰이다(토큰이 표보다, 서비스가 둘보다 먼저 사라진다).
    AssetImporterTable importers;
    Array<AssetImporterRegistration> importerTokens;
    UniquePtr<AssetService> service;
    AssetDatabaseLock lock;
    Array<EditorWarning> warnings;
    reflection_text::TextDiagnostics diagnostics;
};

// 프로젝트를 열고(write면 바로 잠그고 — 색인은 잠금 아래에서 만든다) 타입을 등록하고 서비스를
// 만든다. 실패하면 `<명령>: <코드>: <대상>: <설명>` 줄을 쓰고 failure에 종료 코드를 둔다.
ROCKS_ASSETCOMMANDLETS_API bool OpenSession(CommandletOutput& output, Allocator& allocator,
                                            StringView command, StringView project, StringView wait,
                                            bool write, EditSession& session,
                                            Result<int32>& failure) noexcept;

} // namespace rocks::commandlet_host
