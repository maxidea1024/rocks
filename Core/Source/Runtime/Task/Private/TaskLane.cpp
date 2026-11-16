#include <Task/TaskLane.h>

namespace rocks {
namespace {

thread_local TaskLane currentLane = TaskLane::None;

} // namespace

TaskLane CurrentTaskLane() noexcept {
    return currentLane;
}

TaskLaneScope::TaskLaneScope(TaskLane lane) noexcept : previous_(currentLane) {
    currentLane = lane;
}

TaskLaneScope::~TaskLaneScope() noexcept {
    currentLane = previous_;
}

} // namespace rocks
