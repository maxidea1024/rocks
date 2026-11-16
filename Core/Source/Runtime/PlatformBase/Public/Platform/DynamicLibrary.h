#pragma once

#include <Platform/File.h>

namespace rocks::platform {

// 심볼 표의 한 항목이다. name은 찾을 export 이름(NUL 종료, 비어 있으면 안 된다)이고 address는
// DynamicLibrary::Find가 채운다. name이 가리키는 문자열은 호출 동안만 읽는다.
struct DynamicSymbol {
    const char* name = nullptr;
    void* address = nullptr;
};

class DynamicLibraryLease;

// OS 공유 라이브러리(Windows DLL, macOS dylib, Android so) 하나의 소유권이다(UE
// FPlatformProcess::GetDllHandle·GetDllExport·FreeDllHandle 대응). 셰이더 워커가 도구(DXC·
// SPIRV-Cross·SPIRV-Tools·FXC)를 절대 경로에서 열고, 렌더러가 OS 공급 라이브러리(Vulkan 로더,
// XInput)를 여는 데 쓴다. 엔진 모듈을 동적으로 싣는 로더가 아니다.
//
// 사용 예(도구 함수 표):
//   platform::DynamicLibrary compiler;
//   if (auto opened = compiler.Open(dxcPath, dxcPathBytes); !opened) { ... }   // 절대 경로만
//   platform::DynamicSymbol symbols[] = {{"DxcCreateInstance", nullptr}};
//   platform::DynamicLibraryLease lease;
//   if (auto found = compiler.Find(symbols, 1, lease); !found) { ... }
//   auto create = reinterpret_cast<DxcCreateInstanceProc>(symbols[0].address);
//   ...                                  // lease가 살아 있는 동안 create를 부를 수 있다
//   lease.Reset();                       // 함수 표를 더 쓰지 않는다
//   (void)compiler.Close();              // lease가 남아 있으면 Busy로 거부한다
//
// 검색 규칙(현재 디렉터리·PATH에 둔 같은 이름 라이브러리를 열지 않는다):
// - Open(absolutePath): 절대 경로의 파일만 연다.
//   - Windows: LoadLibraryExW(LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32).
//     의존 DLL은 연 DLL과 같은 디렉터리와 System32에서만 찾는다. 단, 같은 이름(기본 이름)의 DLL이
//     이 프로세스에 이미 올라와 있으면 OS가 디렉터리와 관계없이 그것을 쓴다.
//   - macOS·Android: dlopen(RTLD_NOW | RTLD_LOCAL). 모든 심볼을 여는 때 결합하고 다른 라이브러리의
//     심볼 검색에 섞지 않는다. macOS의 같은 디렉터리 의존은 라이브러리의 install name
//     (@loader_path)이, Android는 라이브러리의 DT_RUNPATH($ORIGIN)가 정한다. macOS에서 hardened
//     runtime이 아닌 프로세스는 DYLD_LIBRARY_PATH 환경 변수가 같은 이름 파일을 먼저 고를 수 있다.
// - OpenSystem(name): OS가 공급하는 라이브러리만 파일 이름으로 연다. Windows는 System32만 찾는다
//   (vulkan-1.dll·xinput1_4.dll). macOS는 /usr/lib/<name>을 연다. Android는 링커 namespace의
//   시스템 경로에서 찾는다(libvulkan.so. 앱 프로세스는 앱의 native 라이브러리 디렉터리도 먼저
//   찾는다).
//
// 수명 계약:
// - Find로 얻은 주소(함수 표)는 함께 받은 DynamicLibraryLease가 살아 있는 동안 유효하다. lease가
//   하나라도 남아 있으면 Close는 Busy로 거부하고 라이브러리를 그대로 둔다.
// - lease가 남은 채 DynamicLibrary를 소멸시키면 내리지 않고 소유권만 놓는다. 라이브러리는 마지막
//   lease가 풀릴 때 내린다(그 lease를 푸는 스레드에서). 그래서 lease가 있는 한 주소는 계속
//   유효하다.
// - 라이브러리 안의 코드가 만든 스레드·콜백·객체가 라이브러리를 내린 뒤에 실행되지 않게 하는 것은
//   호출자의 책임이다(그 수명 동안 lease를 쥔다).
// - Windows에서 연 DLL의 DllMain은 OS 로더 잠금 안에서 실행된다. 다른 스레드가 같은 로더 잠금을
//   기다리는 동안 DllMain이 그 스레드를 기다리면 교착한다(OS 규칙).
//
// 오류(FileStatus.code):
// - InvalidArgument: 경로·이름·심볼이 null·빈 값이거나, NUL·잘못된 UTF-8을 담거나, Open에 상대
//   경로를 주었거나, OpenSystem 이름에 경로 구분자·':'가 있거나 "."·".."이다.
// - Unsupported: Windows 장치 namespace 경로("\\?\"·"\\.\")·DOS 장치 이름.
// - NotFound: Open·OpenSystem의 파일이 없다. Find의 심볼이 없다. Windows는 의존 DLL(또는 의존 DLL의
//   import 심볼)이 없을 때도 NotFound다(nativeError가 ERROR_MOD_NOT_FOUND·ERROR_PROC_NOT_FOUND).
//   Android OpenSystem은 확인할 파일 경로가 없으므로 링커가 열지 못한 모든 경우가 NotFound다.
// - WrongType: 디렉터리이거나, 이 프로세스가 실을 수 있는 라이브러리가 아니다(다른 형식·다른
//   아키텍처).
// - AccessDenied: 읽거나 실행할 권한이 없다.
// - AlreadyOpen: 이미 열린 객체에 Open·OpenSystem을 불렀다. NotOpen: 열리지 않은 객체에 Find를
//   불렀다. Busy: lease가 남은 채 Close를 불렀다. Overflow: lease 수가 상한에 닿았다.
// - OutOfMemory: 상태·경로 사본을 할당하지 못했다.
// - SystemError: 그 밖의 OS 실패. macOS·Android는 파일이 있고 형식이 맞는데 dlopen이 실패한
//   경우(의존 라이브러리 없음 포함)이며 nativeError는 0이다(dlerror 문구는 아직 넘기지 않는다).
// Windows에서 열기 실패는 OS 오류 대화상자를 띄우지 않는다(호출 스레드의 오류 모드를 잠시
// SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX로 바꾼다).
//
// 스레드: Find와 LeaseCount는 여러 스레드에서 동시에 불러도 된다. Open·OpenSystem·Close·이동·소멸은
// 같은 객체의 다른 메서드와 동시에 부르지 않는다. lease 해제는 어느 스레드에서 언제 해도 되며
// Close와 겹치면 Close가 Busy이거나 해제가 먼저 끝난 것으로 본다.
//
// 후속: dlerror 문구 전달, 심볼 버전(dlvsym), 다른 라이브러리를 거치지 않는 POSIX 심볼 검색(dlsym은
// 의존 라이브러리의 심볼도 찾는다. GetProcAddress는 그 DLL의 export만 찾는다).
class DynamicLibrary final {
public:
    // OpenSystem 이름의 최대 바이트 수다(대부분 파일 시스템의 파일 이름 한도).
    static constexpr usize MaxSystemNameBytes = 255;

    DynamicLibrary() noexcept = default;
    ~DynamicLibrary() noexcept;
    DynamicLibrary(DynamicLibrary&& other) noexcept;
    // 기존 라이브러리의 소유권을 놓고(소멸과 같다) other의 것을 받는다.
    DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;
    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;

    // absolutePath(UTF-8, 길이 지정, NUL 종료 불필요)의 라이브러리를 연다. Windows 절대 경로는
    // "X:\"·"X:/"로 시작하는 드라이브 경로나 "\\server\share\" UNC 경로이고, POSIX는 '/'로
    // 시작한다. 실패하면 닫힌 상태로 남는다. 오류는 클래스 주석을 따른다.
    FileStatus Open(const char* absolutePath, usize pathBytes) noexcept;

    // NUL 종료 absolutePath를 쓰는 편의 함수다.
    FileStatus Open(const char* absolutePath) noexcept;

    // OS가 공급하는 라이브러리를 파일 이름(확장자 포함, UTF-8, 최대 MaxSystemNameBytes)으로 연다
    // (예: "vulkan-1.dll", "xinput1_4.dll", "libvulkan.so"). 검색 위치는 클래스 주석을 따른다.
    // 실패하면 닫힌 상태로 남는다.
    FileStatus OpenSystem(const char* name, usize nameBytes) noexcept;

    // NUL 종료 name을 쓰는 편의 함수다.
    FileStatus OpenSystem(const char* name) noexcept;

    bool IsOpen() const noexcept {
        return state_ != nullptr;
    }

    // symbols[0..count)의 주소를 모두 찾아 address에 채우고 lease를 하나 준다(lease에 이전 것이
    // 있으면 새 lease를 얻은 뒤 놓는다). 하나라도 없으면 NotFound이며 모든 address를 null로
    // 되돌리고 lease는 바꾸지 않는다. missingIndex가 null이 아니면 처음 찾지 못한 항목의 색인을
    // 두고, 성공이나 다른 실패에는 IndexNone을 둔다. symbols가 null이거나 count가 0이거나 이름이
    // null·빈 값이면 InvalidArgument, 열리지 않았으면 NotOpen이다. 주소의 유효 기간은 클래스 주석의
    // 수명 계약이다.
    FileStatus Find(DynamicSymbol* symbols, usize count, DynamicLibraryLease& lease,
                    usize* missingIndex = nullptr) const noexcept;

    // 심볼 하나를 찾는 편의 함수다. 실패하면 address는 null이다.
    FileStatus Find(const char* symbol, void*& address, DynamicLibraryLease& lease) const noexcept;

    // 이 라이브러리의 살아 있는 lease 수다. 다른 스레드가 lease를 풀고 있으면 읽는 순간의 값이다.
    // 열리지 않았으면 0이다.
    usize LeaseCount() const noexcept;

    // 라이브러리를 내린다. lease가 남아 있으면 Busy이고 열린 상태 그대로다. 열려 있지 않으면 아무
    // 일도 하지 않고 Success다. OS 해제가 실패하면 그 오류를 돌려주지만 객체는 닫힌다(handle을 다시
    // 쓰지 않는다).
    FileStatus Close() noexcept;

    // 플랫폼 공통 구현(Private/Common/DynamicLibrary.cpp)만 정의하는 상태다. 공개 API가 아니며 이
    // 헤더에서는 불완전 타입이다.
    struct State;

private:
    State* state_ = nullptr;
};

// DynamicLibrary::Find로 얻은 주소가 유효하다는 보증이다. 살아 있는 동안 라이브러리를 내리지
// 못하게 한다. 이동만 된다. 빈 lease(기본 생성, 이동한 뒤, Reset 뒤)는 아무것도 보증하지 않는다.
// 한 lease 객체는 한 번에 한 스레드에서만 쓴다. 서로 다른 lease는 어느 스레드에서든 동시에 풀어도
// 된다.
class DynamicLibraryLease final {
public:
    DynamicLibraryLease() noexcept = default;

    ~DynamicLibraryLease() noexcept {
        Reset();
    }

    DynamicLibraryLease(DynamicLibraryLease&& other) noexcept : state_(other.state_) {
        other.state_ = nullptr;
    }

    DynamicLibraryLease& operator=(DynamicLibraryLease&& other) noexcept;
    DynamicLibraryLease(const DynamicLibraryLease&) = delete;
    DynamicLibraryLease& operator=(const DynamicLibraryLease&) = delete;

    bool IsHeld() const noexcept {
        return state_ != nullptr;
    }

    // 보증을 놓는다. 소유자가 이미 소멸했고 이것이 마지막 lease면 여기서 라이브러리를 내린다. 빈
    // lease에는 아무 일도 하지 않는다.
    void Reset() noexcept;

private:
    friend class DynamicLibrary;

    DynamicLibrary::State* state_ = nullptr;
};

} // namespace rocks::platform
