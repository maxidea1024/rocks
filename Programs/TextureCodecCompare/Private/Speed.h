#pragma once

// 텍스처 압축 속도 측정 모드다(Docs/Design/AssetSystem 12.17.9 6 — TC5b). TextureCodecCompare
// --speed가 부른다.
namespace rocks::texture_codec_speed {

// 인코더 blocks/s(직렬·병렬), BC 최소 batch 후보, BuildTexture 단계 시간을 출력한다. judgeEac면
// 잡음 4096² Alpha(Android EAC) 병렬 시간도 잰다. 성공이면 0이다.
int RunSpeed(bool judgeEac) noexcept;

} // namespace rocks::texture_codec_speed
