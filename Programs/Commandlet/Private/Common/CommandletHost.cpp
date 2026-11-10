#include "CommandletHost.h"
#include <Core/Memory/SystemAllocator.h>

namespace rocks::commandlet_host {
namespace {

Result<int32> CoreCheck(void*, Span<const StringView> args, CommandletOutput& output) noexcept {
    if (args.Size() != 0) {
        auto written = output.write(output.context, CommandletStream::Error,
                                    "core-check accepts no arguments.\n");
        return written ? Result<int32>::Success(2) : Result<int32>::Failure(written.Error());
    }
    SystemAllocator allocator;
    auto allocation = allocator.Allocate(64, 16, 0x434d444cu);
    if (!allocation) {
        return Result<int32>::Failure(allocation.Error());
    }
    auto released = allocator.Deallocate(allocation.Value());
    if (!released || allocator.Stats().liveAllocations != 0) {
        return Result<int32>::Failure(released ? ErrorCode::DependencyFailed : released.Error());
    }
    auto written = output.write(output.context, CommandletStream::Output,
                                "Core allocation and release: PASS\n");
    return written ? Result<int32>::Success(0) : Result<int32>::Failure(written.Error());
}

} // namespace

int Run(Span<const StringView> args, CommandletOutput output) noexcept {
    CommandletRegistry registry;
    auto registered = registry.Register(
        {"core-check", "Check the Core allocator lifecycle.", &CoreCheck, nullptr});
    if (!registered) {
        return 1;
    }
    auto result = registry.Run(args, output);
    if (!result) {
        if (output.write) {
            // 진단 출력도 실패할 수 있다. 복구할 수 없으면 실패 코드를 그대로 반환한다.
            (void)output.write(output.context, CommandletStream::Error,
                               "Commandlet execution or output failed.\n");
        }
        return 1;
    }
    return static_cast<int>(result.Value());
}

} // namespace rocks::commandlet_host
