#include "../Common/DynamicLibraryInternal.h"
#include "WindowsError.h"
#include "WindowsFileCommon.h"

#include <Windows.h>

// Windows DLL 로더 backend다. 의존 DLL 검색을 연 DLL의 디렉터리와 System32로 제한해 현재
// 디렉터리·PATH·실행 파일 디렉터리의 같은 이름 DLL을 쓰지 않는다(DLL 바꿔치기 방지).
namespace rocks::platform::detail {

namespace {

// 로더가 실패할 때 오류 대화상자(파일 열기 실패, 잘못된 이미지 hard error)를 띄우지 않게 호출
// 스레드의 오류 모드를 잠시 바꾼다(UE GetDllHandle과 같다). 다른 스레드에는 영향이 없다.
class QuietErrorMode {
public:
    QuietErrorMode() noexcept {
        restore_ =
            SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previous_) != 0;
    }

    ~QuietErrorMode() noexcept {
        if (restore_) {
            (void)SetThreadErrorMode(previous_, nullptr);
        }
    }

    QuietErrorMode(const QuietErrorMode&) = delete;
    QuietErrorMode& operator=(const QuietErrorMode&) = delete;

private:
    DWORD previous_ = 0;
    bool restore_ = false;
};

bool IsSeparator(char value) noexcept {
    return value == '\\' || value == '/';
}

// 현재 디렉터리와 무관한 경로인지다: "X:\"·"X:/" 드라이브 절대 경로 또는 "\\"·"//"로 시작하는
// UNC·장치 경로("\\?\"·"\\.\"는 뒤에서 WidePath가 Unsupported로 거부한다). "\a"(현재 드라이브의
// 루트)와 "C:a"(그 드라이브의 현재 디렉터리)는 현재 상태에 따라 달라지므로 상대 경로다.
bool IsAbsolute(const char* path, usize bytes) noexcept {
    if (bytes >= 2 && IsSeparator(path[0]) && IsSeparator(path[1])) {
        return true;
    }

    const char letter = path[0];
    const bool alphabetic = (letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z');
    return bytes >= 3 && alphabetic && path[1] == ':' && IsSeparator(path[2]);
}

// 로더 오류를 공통 코드로 바꾼다. 의존 DLL·그 import 심볼이 없으면 NotFound, 실을 수 없는 이미지
// (형식·아키텍처)는 WrongType이다. 그 밖은 파일 오류 규칙(NativeError)을 따른다.
FileStatus LoaderError(DWORD error) noexcept {
    switch (error) {
    case ERROR_MOD_NOT_FOUND:
    case ERROR_PROC_NOT_FOUND:
        return {FileCode::NotFound, static_cast<uint32>(error)};
    case ERROR_BAD_EXE_FORMAT:
    case ERROR_EXE_MACHINE_TYPE_MISMATCH:
    case ERROR_INVALID_IMAGE_HASH:
        return {FileCode::WrongType, static_cast<uint32>(error)};
    default:
        return NativeError(error);
    }
}

// LoadLibraryExW 실패를 분류한다. 로더는 파일이 없을 때와 의존 DLL이 없을 때 모두
// ERROR_MOD_NOT_FOUND를 주므로 파일 속성으로 디렉터리·없음을 먼저 가린다.
FileStatus ClassifyLoadFailure(const wchar_t* path, DWORD error) noexcept {
    const DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD attributeError = GetLastError();
        if (attributeError == ERROR_FILE_NOT_FOUND || attributeError == ERROR_PATH_NOT_FOUND) {
            return {FileCode::NotFound, static_cast<uint32>(error)};
        }
    } else if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return {FileCode::WrongType, static_cast<uint32>(error)};
    }

    return LoaderError(error);
}

} // namespace

// Windows는 nativeError에 원래 OS 코드를 담으므로 문구는 비워 둔다(지금 동작 유지).
FileStatus DynamicLibraryOpenPath(const char* path, usize pathBytes, uptr& handle,
                                  DynamicLibraryError* loaderText) noexcept {
    (void)loaderText;
    if (!IsAbsolute(path, pathBytes)) {
        return {FileCode::InvalidArgument, 0};
    }

    WidePath native;
    FileStatus status = native.Initialize(path, pathBytes);
    if (!status) {
        return status;
    }

    HMODULE module = nullptr;
    DWORD error = ERROR_SUCCESS;
    {
        const QuietErrorMode quiet;
        module = LoadLibraryExW(native.Value(), nullptr,
                                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module == nullptr) {
            error = GetLastError();
        }
    }

    if (module == nullptr) {
        return ClassifyLoadFailure(native.Value(), error);
    }

    handle = reinterpret_cast<uptr>(module);
    return {};
}

FileStatus DynamicLibraryOpenSystem(const char* name, usize nameBytes, uptr& handle,
                                    DynamicLibraryError* loaderText) noexcept {
    (void)loaderText;
    windows::WideString wide;
    FileStatus status = wide.AssignUtf8(name, nameBytes);
    if (!status) {
        return status;
    }

    HMODULE module = nullptr;
    DWORD error = ERROR_SUCCESS;
    {
        const QuietErrorMode quiet;
        module = LoadLibraryExW(wide.Value(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module == nullptr) {
            error = GetLastError();
        }
    }

    if (module == nullptr) {
        // System32에 그 이름이 없으면 ERROR_MOD_NOT_FOUND다(NotFound).
        return LoaderError(error);
    }

    handle = reinterpret_cast<uptr>(module);
    return {};
}

void* DynamicLibraryFindSymbol(uptr handle, const char* symbol) noexcept {
    // GetProcAddress는 FARPROC(함수 포인터)를 준다. 데이터 export도 있으므로 객체 포인터로 넘긴다.
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(handle), symbol));
}

FileStatus DynamicLibraryClose(uptr handle) noexcept {
    if (FreeLibrary(reinterpret_cast<HMODULE>(handle)) == 0) {
        return NativeError(GetLastError());
    }

    return {};
}

} // namespace rocks::platform::detail
