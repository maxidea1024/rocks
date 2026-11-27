// nanosvg 할당 hook이다(Engine/Source/ThirdParty/NanoSvg/NanoSvgGlue.c가 malloc·realloc·free를 이
// 이름으로 바꾼다 — Docs/Design/UiToolkit 17.2절). NanoSvg를 링크하는 모듈이 정의하며, 모두 Core
// LibraryHeap(프로세스 수명 Rocks 할당기)을 지난다.
#include <Core/Memory/LibraryHeap.h>
#include <stddef.h>

extern "C" void* RocksNanoSvgMalloc(size_t size) {
    return rocks::LibraryHeapAllocate(size);
}

extern "C" void* RocksNanoSvgRealloc(void* memory, size_t size) {
    return rocks::LibraryHeapReallocate(memory, size);
}

extern "C" void RocksNanoSvgFree(void* memory) {
    rocks::LibraryHeapFree(memory);
}
