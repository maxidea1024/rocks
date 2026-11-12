#include "GraphicsSelfTest.h"

#include <Core/Memory/SystemAllocator.h>
#include <string.h>

// Sandbox 진입점이다.
//   Sandbox --self-test     창 없이 그래픽 backend 검사(종료 코드 0 = 통과)
int main(int argc, char** argv) {
    rocks::SystemAllocator allocator;
    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--self-test") == 0) {
            return sandbox::RunGraphicsSelfTest(allocator);
        }
    }

    return sandbox::RunGraphicsSelfTest(allocator);
}
