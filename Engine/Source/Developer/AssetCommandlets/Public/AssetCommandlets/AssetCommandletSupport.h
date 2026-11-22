#pragma once

#include <AssetDatabase/AssetDatabaseLock.h>
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
#include <Task/WorkerScheduler.h>

namespace rocks {

class ModuleLoader;

} // namespace rocks

// 에셋 명령(asset-*)의 공용 도구다(Docs/Design/AssetSystem.ko.md 13절): 출력 쓰기, 실패 줄,
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
bool OpenProject(CommandletOutput& output, Allocator& allocator, StringView command,
                 StringView project, ProjectLayout& layout, Result<int32>& failure) noexcept;

// 에디터 패키지 색인·열기의 읽기 옵션이다(에디터 종류, 이 엔진의 버전 — 더 새 엔진의 패키지는
// NewerVersion).
PackageReadOptions IndexReadOptions() noexcept;

// 에셋 명령의 문맥이다(commandlet_host::Run이 건다): 작업 메모리와 Launch의 프로세스 모듈 로더.
struct AssetCommandContext {
    Allocator* allocator = nullptr;
    ModuleLoader* modules = nullptr;
};

// 에셋 타입 등록이다(명령 하나 동안 — Docs/Design/AssetSystem.ko.md 7.1절 15 A2c3b, ModuleLoader
// 3.11절): 에셋 타입 모듈(AssetTypeModule — AssetRuntime은 그 의존)을 시작하고 이 TypeRegistry의
// 타입 등록기를 붙인다. Name pool도 둔다. 소멸이 등록기를 떼고 Registry·pool을 닫는다.
class AssetCommandTypes {
public:
    AssetCommandTypes(Allocator& allocator, ModuleLoader& modules) noexcept;
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

private:
    ModuleLoader* modules_;
    TypeRegistry types_;
    ModuleTypeRegistrations registrations_;
    NamePool names_;
    bool attached_ = false;
    bool namesReady_ = false;
};

// 에셋 명령이 시작하는 에셋 타입 모듈이다(그 의존 폐포의 타입이 등록된다 — A5 뒤 텍스처 모듈을
// 더한다).
inline constexpr StringView AssetTypeModule = "Animation";

// 공통 옵션 --wait=<초>의 상한이다(하루 — 밖이면 사용법 오류).
inline constexpr uint32 MaxWaitSeconds = 86400;
inline constexpr uint32 MillisecondsPerSecond = 1000;

// --wait 값(10진 숫자만, 0–MaxWaitSeconds)을 밀리초로 푼다. 비어 있으면 0이다. 형식·범위 위반은
// false다.
bool ParseWaitMilliseconds(StringView text, uint32& out) noexcept;

// 에셋 Registry의 문맥 id다(명령 하나의 에디터 집합).
inline constexpr ContextId CommandletAssetContext{0x524f434b41535345ull, 1};

// 명령 하나의 실행 상태다: 프로젝트·타입·서비스.
// 색인 검색의 파일 읽기를 나눌 worker 수다(Docs/Design/AssetSystem.ko.md 7.1절 16 A2d2 — 패키지
// 5만 개 측정에서 3개가 가장 빨랐고 더 늘리면 느려졌다).
inline constexpr uint32 CommandletScanWorkers = 3;

// 명령 하나 동안의 검색 scheduler다. 시작하지 못하거나(스레드 생성 실패) 논리 프로세서가 하나면
// Get이 null이고 검색은 직렬이다(결과가 같다). allocator는 여러 스레드의 동시 호출을 받는다
// (프로세스 기본 allocator).
class ScanScheduler {
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
    EditSession(Allocator& allocator, ModuleLoader& modules) noexcept
        : layout(allocator), scan(allocator), types(allocator, modules),
          hooks(allocator, types.Types()), warnings(allocator), diagnostics(allocator) {
    }

    ProjectLayout layout;
    // 서비스(색인)보다 먼저 만들고 나중에 없앤다(서비스가 빌린다).
    ScanScheduler scan;
    AssetCommandTypes types;
    AssetTypeHookTable hooks;
    UniquePtr<AssetService> service;
    AssetDatabaseLock lock;
    Array<EditorWarning> warnings;
    reflection_text::TextDiagnostics diagnostics;
};

// 프로젝트를 열고(write면 바로 잠그고 — 색인은 잠금 아래에서 만든다) 타입을 등록하고 서비스를
// 만든다. 실패하면 `<명령>: <코드>: <대상>: <설명>` 줄을 쓰고 failure에 종료 코드를 둔다.
bool OpenSession(CommandletOutput& output, Allocator& allocator, StringView command,
                 StringView project, StringView wait, bool write, EditSession& session,
                 Result<int32>& failure) noexcept;

} // namespace rocks::commandlet_host
