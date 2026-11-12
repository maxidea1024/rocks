#pragma once

#include <Core/Result.h>
#include <Meta/TypeSchema.h>

namespace rocks {

// World/PIE 인스턴스 등의 문맥 일치 확인용 값이다. 권한 인증 토큰은 아니다.
// 문맥이 일치해도 Registry 소유 스레드 검사를 별도로 통과해야 한다.
struct ObjectAccessContext {
    ContextId context;
};

// World·PIE 등의 실행 진입점에서 한 번 설정하는 스레드별 문맥이다. task/worker로 자동 전파하지
// 않는다. 비복사·비이동이며 만든 스레드에서 정확히 역순으로 소멸한다(위반은 Fatal).
// 서로 다른 문맥을 중첩할 수 있고 안쪽 범위가 끝나면 이전 문맥을 복원한다.
class ObjectAccessScope {
public:
    explicit ObjectAccessScope(ObjectAccessContext) noexcept;
    ~ObjectAccessScope() noexcept;
    ObjectAccessScope(const ObjectAccessScope&) = delete;
    ObjectAccessScope& operator=(const ObjectAccessScope&) = delete;
    ObjectAccessScope(ObjectAccessScope&&) = delete;
    ObjectAccessScope& operator=(ObjectAccessScope&&) = delete;

    // 현재 스레드에 범위가 없으면 WrongContext다. 반환값은 인증 권한이 아니다.
    static Result<ObjectAccessContext> Current() noexcept;

private:
    ObjectAccessContext access_;
    ObjectAccessScope* previous_ = nullptr;
    uint64 ownerThread_ = 0;
};

} // namespace rocks
