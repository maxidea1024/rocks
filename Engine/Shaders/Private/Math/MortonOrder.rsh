// Morton(Z 순서) 부호화다(Docs/Design/EngineShaders 4.2절 A02·7.3절). Morton, "A Computer Oriented
// Geodetic Data Base and a New Technique in File Sequencing", IBM 1966. 비트 벌리기는 마스크·자리
// 이동 단계(분할 정복)로 한다. 2D는 성분당 16 bit(x가 짝수 bit), 3D는 성분당 10 bit(x가 3k bit)다.
// 넘는 bit는 버린다.
#pragma once

// 하위 16 bit를 짝수 bit로 벌린다.
uint MortonSpread2(uint x) {
    x &= 0x0000ffffu;
    x = (x | (x << 8)) & 0x00ff00ffu;
    x = (x | (x << 4)) & 0x0f0f0f0fu;
    x = (x | (x << 2)) & 0x33333333u;
    x = (x | (x << 1)) & 0x55555555u;
    return x;
}

// MortonSpread2의 역(짝수 bit → 하위 16 bit)이다.
uint MortonCompact2(uint x) {
    x &= 0x55555555u;
    x = (x | (x >> 1)) & 0x33333333u;
    x = (x | (x >> 2)) & 0x0f0f0f0fu;
    x = (x | (x >> 4)) & 0x00ff00ffu;
    x = (x | (x >> 8)) & 0x0000ffffu;
    return x;
}

// 하위 10 bit를 3 bit 간격으로 벌린다.
uint MortonSpread3(uint x) {
    x &= 0x000003ffu;
    x = (x | (x << 16)) & 0x030000ffu;
    x = (x | (x << 8)) & 0x0300f00fu;
    x = (x | (x << 4)) & 0x030c30c3u;
    x = (x | (x << 2)) & 0x09249249u;
    return x;
}

uint MortonCompact3(uint x) {
    x &= 0x09249249u;
    x = (x | (x >> 2)) & 0x030c30c3u;
    x = (x | (x >> 4)) & 0x0300f00fu;
    x = (x | (x >> 8)) & 0x030000ffu;
    x = (x | (x >> 16)) & 0x000003ffu;
    return x;
}

uint MortonEncode2D(uint2 position) {
    return MortonSpread2(position.x) | (MortonSpread2(position.y) << 1);
}

uint2 MortonDecode2D(uint code) {
    return uint2(MortonCompact2(code), MortonCompact2(code >> 1));
}

uint MortonEncode3D(uint3 position) {
    return MortonSpread3(position.x) | (MortonSpread3(position.y) << 1) | (MortonSpread3(position.z) << 2);
}

uint3 MortonDecode3D(uint code) {
    return uint3(MortonCompact3(code), MortonCompact3(code >> 1), MortonCompact3(code >> 2));
}
