// LZ4 1.11.1의 lz4.c와 lz4hc.c를 한 번역 단위로 빌드하는 Rocks 글루다.
// upstream이 지원하는 방식이다. lz4.c가 LZ4_SRC_INCLUDED를 정의하면 lz4hc.c는 lz4.c를 다시 포함하지
// 않으므로, lz4hc.c를 단독으로 빌드할 때만 쓰는 -Wunused-function 억제 분기가 비활성이다. 빌드
// 정의(LZ4_STATIC_LINKING_ONLY_DISABLE_MEMORY_ALLOCATION 등)는 ThirdParty.json을 따른다.
// lz4frame.c는 이 합본에 넣지 않는다(lz4.c의 할당 매크로를 이어 쓰는데 위 정의가 그 매크로를
// 막는다. manifest notes).
#include "lz4.c"
#include "lz4hc.c"

// lz4frame.c의 기본 할당 경로(사용자 할당기가 없을 때)가 부르는 함수다(LZ4_USER_MEMORY_FUNCTIONS,
// upstream 설정 지점). Rocks는 모든 LZ4F 문맥을 Allocator 경유 할당 함수로 만들므로 이 경로를 쓰지
// 않는다. 실수로 기본 할당 경로를 타는 호출(LZ4F_compressFrame, _advanced가 아닌 생성 함수)은 CRT
// 대신 할당 실패(LZ4F_ERROR_allocation_failed)가 된다. lz4.c는 DISABLE_MEMORY_ALLOCATION 분기가
// 먼저라 이 함수를 쓰지 않는다.
void* LZ4_malloc(size_t size);
void* LZ4_calloc(size_t count, size_t size);
void LZ4_free(void* address);

void* LZ4_malloc(size_t size) {
    (void)size;
    return NULL;
}

void* LZ4_calloc(size_t count, size_t size) {
    (void)count;
    (void)size;
    return NULL;
}

void LZ4_free(void* address) {
    // LZ4F_free는 NULL이면 바로 돌아가고 위 두 함수는 주소를 돌려주지 않으므로 여기 오는 주소는
    // 없다.
    (void)address;
}
