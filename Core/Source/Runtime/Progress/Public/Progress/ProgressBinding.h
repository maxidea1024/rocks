#pragma once

#include <Core/Result.h>
#include <Progress/ProgressTypes.h>

namespace rocks {

// 이 스레드를 작업에 붙인다(Docs/Design/ProgressReporting 6.1절). 스레드별 바인딩은 스택이고 맨
// 위가 그 스레드의 "현재 작업"이다. 그 위에서 연 ProgressScope가 그 작업의 진행을 보고한다. 자동
// 바인딩은 없다 — Begin한 스레드도 명시로 Bind한다.
//
// 사용 예:
//   auto binding = ProgressBinding::Bind(task.Id(), ProgressBindingRole::Primary);
//   if (!binding) { return Result<void>::Failure(binding.Error()); }
//   BuildAll();                         // 안의 ProgressScope가 task 진행을 보고한다
//
// 규칙: 이동은 같은 스레드 안에서만 하고, 소멸은 생성의 역순이며 같은 스레드여야 한다. 그 바인딩
// 위에서 연 구간이 남아 있으면 안 된다. 어기면 Fatal이다(스택 손상은 코드 결함).
class ROCKS_PROGRESS_API ProgressBinding {
public:
    ProgressBinding(ProgressBinding&& other) noexcept;
    ProgressBinding& operator=(ProgressBinding&&) = delete;
    ProgressBinding(const ProgressBinding&) = delete;
    ProgressBinding& operator=(const ProgressBinding&) = delete;
    ~ProgressBinding() noexcept;

    // 오류: NotReady·ShuttingDown, 발급된 적 없는 id NotFound, 끝난 작업 StaleHandle, 이미 다른
    // Primary가 있으면 AlreadyExists, 스택 깊이 MaxProgressBindingDepth 초과 CapacityExceeded.
    static Result<ProgressBinding> Bind(ProgressTaskId id, ProgressBindingRole role) noexcept;

private:
    ProgressBinding(uint32 depth, detail::ProgressTaskRecord* record) noexcept
        : record_(record), depth_(depth) {
    }

    // 이 바인딩이 붙잡은 기록이다(소멸 때 스택 칸이 이 바인딩의 것인지 확인한다).
    detail::ProgressTaskRecord* record_ = nullptr;
    // 스택에서 이 바인딩의 칸 번호 + 1이다(0은 이동된 빈 객체).
    uint32 depth_ = 0;
};

} // namespace rocks
