// LZ4 1.11.1의 lz4.c와 lz4hc.c를 한 번역 단위로 빌드하는 Rocks 글루다.
// upstream이 지원하는 방식이다. lz4.c가 LZ4_SRC_INCLUDED를 정의하면 lz4hc.c는 lz4.c를 다시 포함하지
// 않으므로, lz4hc.c를 단독으로 빌드할 때만 쓰는 -Wunused-function 억제 분기가 비활성이다. 빌드
// 정의(LZ4_STATIC_LINKING_ONLY_DISABLE_MEMORY_ALLOCATION 등)는 ThirdParty.json을 따른다.
#include "lz4.c"
#include "lz4hc.c"
