#include <TestSupport/StepScheduler.h>

namespace rocks::test::detail {

namespace {

thread_local StepScheduler* currentScheduler = nullptr;
thread_local uint32 currentActor = 0;

} // namespace

StepScheduler*& CurrentStepScheduler() noexcept {
    return currentScheduler;
}

uint32& CurrentStepActor() noexcept {
    return currentActor;
}

} // namespace rocks::test::detail
