#pragma once

#include <Base/Types.h>
#include <Core/Error.h>

namespace rocks {

// 적재 요청의 결과 코드다(Docs/Design/ModuleLoader 3.4절, UE EModuleLoadResult 대응).
// NotFound·Incompatible·OsLoadFailed·EntryMissing은 Modular 적재(ML4)에서 쓰며 Monolithic에서는
// 표에 없는 이름만 NotFound다.
enum class ModuleLoadCode : uint8 {
    // 시작했거나 이미 시작해 있다.
    Success,
    // 모듈 표·manifest에 없는 이름, 또는 파일이 없다.
    NotFound,
    // manifest의 buildId·파일 크기, 진입 기술의 entryVersion·이름이 기대와 다르다.
    Incompatible,
    // OS 적재 실패(의존 공유 라이브러리 누락 포함). nativeError와 OS 문구를 담는다.
    OsLoadFailed,
    // 진입점 RocksModuleEntry가 없다.
    EntryMissing,
    // 수명 객체 생성·타입 등록·Startup이 실패했다. 모듈의 ErrorCode를 error에 담는다.
    StartupFailed,
    // 의존 모듈이 실패했다. 처음 실패한 모듈을 failed에 담는다.
    DependencyFailed,
    // 시작 중인 모듈(자기 자신이나 시작 중인 의존 사슬)을 다시 적재했다.
    Cycle,
    // 로더 스레드가 아니다.
    WrongThread,
    // 묶음 종료가 진행 중이거나 프로세스 종료 중이다.
    ShuttingDown
};

// 실행 중 해제 요청의 결과 코드다(3.4절). 해제 API(Unload)는 ML6에 더한다.
enum class ModuleUnloadCode : uint8 {
    // 내렸다(공유 라이브러리 해제는 미뤘을 수 있다).
    Success,
    // 시작한 적이 없거나 이미 내렸다.
    NotStarted,
    // SupportsUnload() == false이거나 부트 모듈이다.
    Unsupported,
    // 시작한 의존 모듈이 있거나 타입 해제가 불가능하다(살아 있는 인스턴스). 붙잡은 쪽을 담는다.
    Busy,
    // 로더 스레드가 아니다.
    WrongThread
};

// 진단용 열거자 이름이다(예: ModuleLoadCode::Cycle → "Cycle"). 모르는 값은 "Unknown"이다. 반환
// 문자열은 정적 수명이다.
ROCKS_MODULELOADER_API const char* ModuleLoadCodeName(ModuleLoadCode code) noexcept;
ROCKS_MODULELOADER_API const char* ModuleUnloadCodeName(ModuleUnloadCode code) noexcept;

// 적재 요청 하나의 결과다(3.4절, DeviceCreateReport와 같은 힙 없는 관례). 로더는 report를 돌려줄 뿐
// 로그를 남기지 않는다. 실패를 확정하는 계층(Launch·엔진 호스트·Rocks Studio)이 영어로 한 번
// 기록한다. 문자열 칸은 NUL 종료 UTF-8이며 넘치면 자르고 truncated를 켠다. 복사해 로더보다 오래 둘
// 수 있다.
//
// 사용 예:
//   ModuleLoadReport report = loader.Load("Physics");
//   if (!report.IsOk()) {
//       Log::Error(LogLaunch, "Module {Module} failed to start ({Code}) at {Stage}: {Message}",
//                  report.failed, ModuleLoadCodeName(report.code), report.stage, report.message);
//   }
struct ModuleLoadReport {
    // 모듈 이름 칸의 byte 수(NUL 포함)다.
    static constexpr usize ModuleNameBytes = 128;
    // 단계 이름 칸의 byte 수(NUL 포함)다.
    static constexpr usize StageBytes = 32;
    // 설명 칸의 byte 수(NUL 포함)다.
    static constexpr usize MessageBytes = 512;

    ModuleLoadCode code = ModuleLoadCode::Success;
    // 처음 실패한 모듈(failed)의 수명 객체 생성·Startup이 돌려준 오류다. StartupFailed와
    // DependencyFailed(의존 모듈의 오류)에서 채운다. manifest 적재(TryCreateFromManifest)의
    // Incompatible은 할당 실패(OutOfMemory)·표 검사 실패(InvalidArgument)를 담는다. 그 밖은
    // None이다.
    ErrorCode error = ErrorCode::None;
    // 원래 OS 오류 코드다(FileStatus.nativeError와 같은 뜻, Modular 적재). 없으면 0이다.
    uint32 nativeError = 0;
    // 요청한 모듈 이름이다.
    char requested[ModuleNameBytes]{};
    // 처음 실패한 모듈 이름이다. 성공이면 빈 문자열이다.
    char failed[ModuleNameBytes]{};
    // 실패한 단계 이름이다("Lookup"·"Create"·"Startup" 등). 성공이면 빈 문자열이다.
    char stage[StageBytes]{};
    // 사람이 읽는 설명이다(영어). 성공이면 빈 문자열이다.
    char message[MessageBytes]{};
    // 문자열 칸을 자른 적이 있는지다.
    bool truncated = false;

    bool IsOk() const noexcept {
        return code == ModuleLoadCode::Success;
    }
};

// 실행 중 해제 요청 하나의 결과다(3.4절, 해제 API는 ML6). 문자열 칸 규칙은 ModuleLoadReport와 같다.
struct ModuleUnloadReport {
    static constexpr usize ModuleNameBytes = ModuleLoadReport::ModuleNameBytes;
    static constexpr usize MessageBytes = ModuleLoadReport::MessageBytes;

    ModuleUnloadCode code = ModuleUnloadCode::Success;
    // 요청한 모듈 이름이다.
    char requested[ModuleNameBytes]{};
    // Busy일 때 붙잡은 쪽(의존 모듈 이름 또는 타입 이름)이다.
    char holder[ModuleNameBytes]{};
    // 사람이 읽는 설명이다(영어).
    char message[MessageBytes]{};
    bool truncated = false;

    bool IsOk() const noexcept {
        return code == ModuleUnloadCode::Success;
    }
};

} // namespace rocks
