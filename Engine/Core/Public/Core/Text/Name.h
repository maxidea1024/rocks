#pragma once

#include <Base/Types.h>
#include <Core/Text/StringView.h>

namespace rocks {

class NamePool;

// pool의 한 실행 세션에서만 유효한 비소유 식별자다. 복사해도 pool/문자열 수명을 늘리지 않는다.
// domain/id는 내부 조회 값이며 주소·hash·파일/네트워크의 영속 식별자가 아니다.
// 기본값은 이름 없음이다. 빈 문자열을 intern한 유효한 Name과는 다르다.
struct Name {
    uint64 domain = 0;
    uint64 id = 0;

    constexpr bool IsValid() const noexcept {
        return domain != 0 && id != 0;
    }

    friend constexpr bool operator==(Name, Name) noexcept = default;
};

// immutable 문자열의 조회 수명을 붙드는 이동 전용 lease다. pool 객체 자체를 소유하지 않으므로
// pool/allocator는 모든 lease 반환과 진행 중 호출 종료까지 살아 있어야 한다.
// 서로 다른 lease의 반환은 병행 가능하지만 같은 lease의 이동/반환/조회 경합은 caller가 막는다.
class NameLease {
public:
    NameLease() noexcept = default;
    ~NameLease() noexcept;
    NameLease(const NameLease&) = delete;
    NameLease& operator=(const NameLease&) = delete;
    NameLease(NameLease&& other) noexcept;
    NameLease& operator=(NameLease&& other) noexcept;

    explicit operator bool() const noexcept;
    Name Identifier() const noexcept;

    // 반환 view는 이 lease 또는 같은 entry의 다른 살아 있는 lease가 있는 동안만 보관한다.
    // embedded NUL을 허용한다. 길이는 byte이며 C 문자열 소비자의 첫 NUL 규칙과 다르다.
    StringView View() const noexcept;

    // 새 lease admission이다. Shutdown 이후 ShuttingDown, 계수 소진은 Overflow다.
    // 실패해도 원본 lease와 그 view는 유효하다. 빈 lease는 StaleHandle이다.
    Result<NameLease> TryClone() const noexcept;
    void Reset() noexcept;

private:
    friend class NamePool;
    NameLease(NamePool& pool, Name name, StringView view) noexcept;

    NamePool* pool_ = nullptr;
    Name name_{};
    StringView view_{};
};

} // namespace rocks
