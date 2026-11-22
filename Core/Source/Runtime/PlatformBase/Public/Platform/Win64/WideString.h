#pragma once

#include <Base/Compiler.h>

// Windows 전용이다. 다른 플랫폼에서 단독 include하면 아무 선언도 만들지 않는다(공개 헤더 단독
// 컴파일 검사가 모든 플랫폼에서 이 헤더를 컴파일한다).
#if ROCKS_PLATFORM_WINDOWS

#include <Base/Types.h>
#include <Platform/File.h>

// Windows "W" API가 쓰는 UTF-16과 Rocks 문자열(UTF-8) 사이의 변환 도구다(UE
// Containers/StringConv.h의 TCHAR·UTF-8 변환 대응). Private/Win64 같은 Windows 전용 소스에서
// 포함한다.
//
// 두 가지 판이 있다.
// - 할당하는 판(WideString·Utf8String): 길이 제한 없는 경로·명령줄처럼 크기를 모르는 문자열.
// - 고정 버퍼 판(ConvertUtf8ToWide·ConvertWideToUtf8): 어댑터 이름·debug label·창 제목처럼 크기가
//   정해진 칸에 담는 문자열. 할당하지 않는다.
//
// 잘못된 입력 규칙(모든 판이 같다): 잘못된 UTF-8(overlong·surrogate 부호화·잘린 sequence·U+10FFFF
// 초과)과 짝 없는 UTF-16 surrogate는 대체 문자(U+FFFD)로 바꾸지 않고 거부한다. 결과는
// FileCode::InvalidArgument이고 nativeError는 ERROR_NO_UNICODE_TRANSLATION이다.
// 근거: 변환 결과가 경로·명령줄 인자·스레드 이름처럼 다시 OS나 사용자에게 같은 값으로 돌아가야 하는
// 곳이 대부분이라, 대체하면 다른 파일을 열거나 다른 인자를 실행하는 조용한 오류가 된다. 표시용
// 이름(어댑터 이름·debug label·창 제목)은 입력이 OS 문자열이거나 엔진이 만든 UTF-8이라 실제로
// 잘못된 경우가 없고, 생기더라도 빈 이름이 되는 것으로 충분하다. 한 규칙으로 두면 호출부마다 어떤
// 판이 어떤 규칙인지 따질 필요가 없다. (UE StringConv는 '?'로 대체한다. 경로까지 같은 규칙을 쓰려고
// 일부러 다르게 둔다.)
//
// NUL 문자(U+0000)는 다른 글자처럼 바꾼다. 경로·인자의 NUL 검사는 호출자가 먼저 한다.
namespace rocks::platform::windows {

// NUL로 끝나는 UTF-16 문자열을 소유한다(UTF-8 → UTF-16 할당 판). 이동만 되고 복사는 안 된다.
// 실패한 Assign 뒤에는 빈 상태(Value가 null)다.
//
// 사용 예:
//   WideString path;
//   if (const FileStatus status = path.AssignUtf8(text, bytes); !status) { return status; }
//   const HANDLE file = CreateFileW(path.Value(), ...);
class ROCKS_PLATFORMBASE_API WideString {
public:
    WideString() noexcept = default;

    ~WideString() noexcept {
        Reset();
    }

    WideString(const WideString&) = delete;
    WideString& operator=(const WideString&) = delete;

    WideString(WideString&& other) noexcept : value_(other.value_), units_(other.units_) {
        other.value_ = nullptr;
        other.units_ = 0;
    }

    WideString& operator=(WideString&& other) noexcept {
        if (this != &other) {
            Adopt(other.value_, other.units_);
            other.value_ = nullptr;
            other.units_ = 0;
        }

        return *this;
    }

    // UTF-8 text(bytes개, NUL 종료 불필요)를 바꿔 담는다. 빈 문자열도 된다(Value는 ""). 기존 값은
    // 먼저 놓는다.
    // 실패: 잘못된 UTF-8은 InvalidArgument(ERROR_NO_UNICODE_TRANSLATION), text가 null인데 bytes가
    // 0이 아니면 InvalidArgument(0), 2^31 byte 이상은 Overflow, 메모리 부족은 OutOfMemory다.
    FileStatus AssignUtf8(const char* text, usize bytes) noexcept;

    // 현재 실행 파일의 전체 경로를 읽는다(GetModuleFileNameW). 긴 경로를 위해 버퍼를 키워 가며 읽고
    // 마지막 시도는 Windows 경로 최대 길이(32767 + NUL)다. 확장 경로 접두사(\\?\, \\?\UNC\)는 떼서
    // 일반 Win32 경로로 둔다(긴 경로는 파일 API가 다시 확장한다). 실패는 OS 원인 코드다.
    FileStatus AssignModuleFileName() noexcept;

    // AlignedAllocate로 할당하고 value[units]가 NUL인 buffer의 소유권을 넘겨받는다. 기존 값은
    // 놓는다.
    void Adopt(wchar_t* value, usize units) noexcept;

    // 값을 놓고 빈 상태로 만든다.
    void Reset() noexcept;

    // 담긴 경로의 확장 경로 접두사를 제자리에서 뗀다. "\\?\C:\..."는 "C:\...", "\\?\UNC\server"는
    // "\\server"가 된다. 그 밖의 확장 경로("\\?\Volume{GUID}\..." 등)는 접두사를 떼면 상대 경로가
    // 되므로 그대로 둔다. 비어 있으면 아무것도 하지 않는다. AssignModuleFileName이 부르며 Adopt한
    // OS 경로에도 쓸 수 있다.
    void RemoveExtendedPrefix() noexcept;

    // NUL로 끝나는 UTF-16이다. 비어 있으면 null이다. Win32 API가 쓰기 가능한 버퍼를 요구하는
    // 경우(CreateProcessW 명령줄)가 있어 non-const를 준다. 다음 Assign·Reset·소멸까지 유효하다.
    wchar_t* Value() const noexcept {
        return value_;
    }

    // NUL을 뺀 UTF-16 단위 수다.
    usize Units() const noexcept {
        return units_;
    }

private:
    wchar_t* value_ = nullptr;
    usize units_ = 0;
};

// NUL로 끝나는 UTF-8 문자열을 소유한다(UTF-16 → UTF-8 할당 판). 이동만 되고 복사는 안 된다.
// 실패한 Assign 뒤에는 빈 상태(Value가 null)다.
//
// 사용 예:
//   Utf8String name;
//   if (name.AssignWide(description, WideLength(description))) { Use(name.Value(), name.Bytes()); }
class ROCKS_PLATFORMBASE_API Utf8String {
public:
    Utf8String() noexcept = default;

    ~Utf8String() noexcept {
        Reset();
    }

    Utf8String(const Utf8String&) = delete;
    Utf8String& operator=(const Utf8String&) = delete;

    Utf8String(Utf8String&& other) noexcept : value_(other.value_), bytes_(other.bytes_) {
        other.value_ = nullptr;
        other.bytes_ = 0;
    }

    Utf8String& operator=(Utf8String&& other) noexcept {
        if (this != &other) {
            Reset();
            value_ = other.value_;
            bytes_ = other.bytes_;
            other.value_ = nullptr;
            other.bytes_ = 0;
        }

        return *this;
    }

    // UTF-16 text(units개, NUL 종료 불필요)를 바꿔 담는다. 빈 문자열도 된다(Value는 ""). 기존 값은
    // 먼저 놓는다.
    // 실패: 짝 없는 surrogate는 InvalidArgument(ERROR_NO_UNICODE_TRANSLATION), text가 null인데
    // units가 0이 아니면 InvalidArgument(0), UTF-8 결과가 2^31 byte 이상이면 Overflow, 메모리
    // 부족은 OutOfMemory다.
    FileStatus AssignWide(const wchar_t* text, usize units) noexcept;

    // 값을 놓고 빈 상태로 만든다.
    void Reset() noexcept;

    // NUL로 끝나는 UTF-8이다. 비어 있으면 null이다. 다음 Assign·Reset·소멸까지 유효하다.
    const char* Value() const noexcept {
        return value_;
    }

    // NUL을 뺀 byte 수다.
    usize Bytes() const noexcept {
        return bytes_;
    }

private:
    char* value_ = nullptr;
    usize bytes_ = 0;
};

// 고정 버퍼 변환에서 결과가 칸보다 길 때의 처리다.
enum class TextFit : uint8 {
    // 전부 들어가야 한다. 모자라면 Overflow이고 required에 필요한 길이를 둔다. 경로·인자처럼 잘리면
    // 다른 값이 되는 곳에 쓴다.
    Exact,
    // 들어가는 만큼 글자 경계에서 자른다(UTF-8 sequence와 UTF-16 surrogate 쌍을 나누지 않는다).
    // 잘렸으면 truncated가 참이다. 표시용 이름에 쓴다.
    Truncate
};

// 고정 버퍼 변환 결과다.
// - status: 성공, 잘못된 입력(InvalidArgument, ERROR_NO_UNICODE_TRANSLATION), text가 null인데
//   길이가 0이 아님(InvalidArgument, 0), 칸 부족(Overflow). Overflow는 Exact에서 모자랄 때,
//   capacity가 0일 때, 결과가 2^31 단위·byte 이상일 때(Exact, 또는 UTF-8 입력이 2^31 byte 이상)다.
//   Truncate는 UTF-16→UTF-8 결과가 2^31 byte를 넘어도 그 안에서 자른다.
// - length: buffer에 쓴 길이(NUL 제외, UTF-16 단위 또는 UTF-8 byte). 실패하면 0이다.
// - required: Overflow에서 필요한 길이(NUL 제외)다. 입력이 너무 커 세지 못했으면 0이다. 그 밖에는
//   0이다.
// - truncated: Truncate에서 잘렸는지다.
// 실패해도 capacity가 1 이상이면 buffer는 빈 문자열(NUL 하나)이다.
struct [[nodiscard]] TextConversion {
    FileStatus status;
    usize length = 0;
    usize required = 0;
    bool truncated = false;

    explicit operator bool() const noexcept {
        return static_cast<bool>(status);
    }
};

// UTF-8 text(bytes개, NUL 종료 불필요)를 UTF-16으로 buffer(capacity 단위, NUL 포함)에 쓴다.
// 할당하지 않는다. 입력 전체를 먼저 검사하므로 Truncate에서도 잘린 뒤쪽의 잘못된 UTF-8은 실패다.
//
// 사용 예:
//   wchar_t title[MaxTitleCharacters];
//   (void)ConvertUtf8ToWide(text.Data(), text.ByteCount(), title, MaxTitleCharacters,
//                           TextFit::Truncate); // 실패하면 title은 ""
ROCKS_PLATFORMBASE_API TextConversion ConvertUtf8ToWide(const char* text, usize bytes,
                                                        wchar_t* buffer, usize capacity,
                                                        TextFit fit) noexcept;

// UTF-16 text(units개, NUL 종료 불필요)를 UTF-8로 buffer(capacity byte, NUL 포함)에 쓴다. 할당하지
// 않는다. 입력 전체를 먼저 검사하므로 Truncate에서도 잘린 뒤쪽의 짝 없는 surrogate는 실패다.
//
// 사용 예:
//   char name[NameBytes];
//   const TextConversion converted = ConvertWideToUtf8(wide, WideLength(wide, MaxUnits), name,
//                                                      NameBytes, TextFit::Exact);
//   if (!converted) { return converted.status; }
ROCKS_PLATFORMBASE_API TextConversion ConvertWideToUtf8(const wchar_t* text, usize units,
                                                        char* buffer, usize capacity,
                                                        TextFit fit) noexcept;

// text의 NUL 앞 UTF-16 단위 수다. maxUnits 안에 NUL이 없으면 maxUnits다. DXGI_ADAPTER_DESC1의
// Description처럼 NUL 종료가 보장되지 않는 고정 배열 필드를 넘기기 전에 쓴다. text는 maxUnits개를
// 읽을 수 있어야 한다.
constexpr usize WideLength(const wchar_t* text, usize maxUnits) noexcept {
    usize units = 0;
    while (units < maxUnits && text[units] != L'\0') {
        ++units;
    }

    return units;
}

// 고정 배열 판이다. 배열 크기를 상한으로 쓴다.
template <usize N>
constexpr usize WideLength(const wchar_t (&text)[N]) noexcept {
    return WideLength(text, N);
}

} // namespace rocks::platform::windows

#endif
