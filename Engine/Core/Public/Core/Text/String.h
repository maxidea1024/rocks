#pragma once

#include <Core/Memory/Allocator.h>
#include <Core/Text/StringView.h>

namespace rocks {

// Allocator를 빌리고 유효한 UTF-8 payload와 마지막 NUL을 소유한다.
// 소유권은 이동으로만 전달한다. allocator는 문자열과 진행 중 호출보다 오래 살아야 한다.
// allocator 콜백에서 같은 String을 변경하는 재진입은 금지한다.
// 같은 String의 동시 읽기/쓰기는 caller가 동기화하며 allocator의 thread 계약도 지킨다.
class String {
public:
    explicit String(Allocator& allocator) noexcept;
    ~String() noexcept;

    String(const String&) = delete;
    String& operator=(const String&) = delete;
    String(String&& other) noexcept;
    String& operator=(String&& other) noexcept;

    // 다른 allocator에 명시적으로 복제한다. 실패해도 이 문자열은 변하지 않는다.
    Result<String> TryClone(Allocator& allocator) const noexcept;

    // 유효하지 않은 UTF-8, overflow, OOM이면 내용·capacity·기존 주소를 보존한다.
    // 자기 전체/부분 view도 허용하며 성장 시 source를 읽은 뒤 이전 storage를 해제한다.
    Result<void> TryAssign(StringView source) noexcept;
    Result<void> TryAppend(StringView source) noexcept;

    // capacity는 마지막 종단 NUL을 제외한 payload byte 수다. 실제 재할당 성공 시
    // 이전 pointer/view는 무효다. 공간이 이미 충분하거나 실패하면 주소도 보존한다.
    Result<void> TryReserve(usize byteCapacity) noexcept;

    // payload를 비우고 종단 NUL을 복원한다. buffer는 재사용하며 기존 내용 view는 무효다.
    void Clear() noexcept;

    const char* Data() const noexcept;
    usize ByteCount() const noexcept;
    usize ByteCapacity() const noexcept;
    bool IsEmpty() const noexcept;
    StringView View() const noexcept;

    // 항상 non-null이고 ByteCount() 위치에 NUL이 있다. embedded NUL도 payload에 허용하므로
    // C API는 첫 NUL까지만 볼 수 있다. 전체 payload를 전달하려면 View/ByteCount를 사용한다.
    // 반환 pointer는 빌린 값이며 재할당·Clear·대입·소멸에 따른 무효화를 지켜야 한다.
    const char* CStr() const noexcept;

private:
    Result<char*> AllocateBuffer(usize byteCapacity) noexcept;
    usize GrowthCapacity(usize required) const noexcept;
    void FreeBuffer(char* buffer) noexcept;
    void Release() noexcept;

    Allocator* allocator_;
    char* data_ = nullptr;
    usize bytes_ = 0;
    usize capacity_ = 0;
};

} // namespace rocks
