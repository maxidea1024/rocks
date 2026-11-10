#pragma once

#include <Core/Diagnostics/LogSink.h>

namespace rocks {

// 생성·소멸을 같은 LogBuffer 소유 스레드에서 수행하는 비복사 RAII 계측이다.
// buffer는 scope보다 오래 살아야 한다. 이름은 내부에 복사하므로 원본 문자열은 생성 후 해제해도
// 된다. 기록 실패는 실행 흐름을 바꾸지 않는다. 포화 시 begin/end 쌍이 빠질 수 있으며 Dropped로
// 손실을 본다.
class ProfileScope {
public:
    ProfileScope(LogBuffer&, const char* name, uint32 category = 0) noexcept;
    ~ProfileScope() noexcept;
    ProfileScope(const ProfileScope&) = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;

private:
    LogBuffer& buffer_;
    uint64 start_;
    uint32 category_;
    char name_[120]{};
    bool nameTruncated_ = false;
};
} // namespace rocks
