// 텍스처 표본 보조다(Docs/Design/EngineShaders 4.18절 A18·14.4절 1항, UE TextureSampling.ush의
// SnapToClosestPixelCenterUV·GetBicubic2DCatmullRomSamples 역할). 텍스처·샘플러는 인자로 받는다(라이브러리
// 자원 선언 없음 — 3.5절). textureSize는 표본할 mip의 texel 크기다.
//
// 근거: Catmull, Rom, "A Class of Local Interpolating Splines", 1974(장력 ½ 커널); Jimenez, "Filmic SMAA",
// SIGGRAPH 2016 Advances(모서리 네 texel을 뺀 bilinear 다섯 번 Catmull–Rom).
//
// 사용 예(TAA 이력):
//   const float4 history = SampleCatmullRom5Tap(History, HistoryLinearClamp, uv, historySize);
#pragma once

// uv를 가장 가까운 texel 중심으로 옮긴다((floor(uv · size) + 0.5)/size).
float2 SnapUvToTexelCenter(float2 uv, float2 textureSize) {
    return (floor(uv * textureSize) + 0.5) / textureSize;
}

// 장력 ½ Catmull–Rom 가중(이웃 texel −1·0·+1·+2, f = texel 중심에서 잰 소수 위치)이다.
float4 CatmullRomWeights(float f) {
    const float f2 = f * f;
    const float f3 = f2 * f;
    return float4(-0.5 * f + f2 - 0.5 * f3, 1.0 - 2.5 * f2 + 1.5 * f3, 0.5 * f + 2.0 * f2 - 1.5 * f3,
                  -0.5 * f2 + 0.5 * f3);
}

// Catmull–Rom 4×4 커널에서 네 모서리 texel을 빼고 가중을 다시 정규화한 필터를 bilinear 표본 다섯 번으로
// 낸다(가운데 2×2 한 번, 네 변의 2 texel 쌍 한 번씩). linearSampler는 bilinear·clamp여야 한다. 결과는 음수가
// 될 수 있다(Catmull–Rom 음의 엽 — 호출자가 자른다).
float4 SampleCatmullRom5Tap(Texture2D source, SamplerState linearSampler, float2 uv, float2 textureSize) {
    const float2 position = uv * textureSize - 0.5;
    const float2 base = floor(position);
    const float2 f = position - base;
    const float4 wx = CatmullRomWeights(f.x);
    const float4 wy = CatmullRomWeights(f.y);
    // 축마다 가운데 두 가중을 합쳐 bilinear 위치를 정한다(합은 1 + f/2 − f²/2 ≥ 1이라 0이 아니다).
    const float2 middleWeight = float2(wx.y + wx.z, wy.y + wy.z);
    const float2 middle = base + float2(wx.z, wy.z) / middleWeight;
    const float2 inverseSize = 1.0 / textureSize;
    const float2 uvFirst = (base - 1.0 + 0.5) * inverseSize;
    const float2 uvMiddle = (middle + 0.5) * inverseSize;
    const float2 uvLast = (base + 2.0 + 0.5) * inverseSize;
    const float weightTop = middleWeight.x * wy.x;
    const float weightLeft = wx.x * middleWeight.y;
    const float weightCenter = middleWeight.x * middleWeight.y;
    const float weightRight = wx.w * middleWeight.y;
    const float weightBottom = middleWeight.x * wy.w;
    const float4 sum = source.SampleLevel(linearSampler, float2(uvMiddle.x, uvFirst.y), 0.0) * weightTop +
                       source.SampleLevel(linearSampler, float2(uvFirst.x, uvMiddle.y), 0.0) * weightLeft +
                       source.SampleLevel(linearSampler, uvMiddle, 0.0) * weightCenter +
                       source.SampleLevel(linearSampler, float2(uvLast.x, uvMiddle.y), 0.0) * weightRight +
                       source.SampleLevel(linearSampler, float2(uvMiddle.x, uvLast.y), 0.0) * weightBottom;
    return sum / (weightTop + weightLeft + weightCenter + weightRight + weightBottom);
}

// 수동 bilinear의 이웃 texel 좌표(가장자리 clamp)와 가중이다(내부).
void BilinearFootprint(float2 uv, uint2 textureSize, out int2 first, out int2 last, out float2 blend) {
    const float2 position = uv * float2(textureSize) - 0.5;
    const float2 base = floor(position);
    const int2 limit = int2(textureSize) - 1;
    blend = position - base;
    first = clamp(int2(base), int2(0, 0), limit);
    last = clamp(int2(base) + 1, int2(0, 0), limit);
}

// 정수 형식(필터 불가) uint 텍스처의 수동 bilinear다(mip 0, 이웃 네 texel Load — 가장자리 clamp).
float4 LoadBilinearUint(Texture2D<uint4> source, float2 uv, uint2 textureSize) {
    int2 first = int2(0, 0);
    int2 last = int2(0, 0);
    float2 blend = float2(0.0, 0.0);
    BilinearFootprint(uv, textureSize, first, last, blend);
    const float4 a = float4(source.Load(int3(first.x, first.y, 0)));
    const float4 b = float4(source.Load(int3(last.x, first.y, 0)));
    const float4 c = float4(source.Load(int3(first.x, last.y, 0)));
    const float4 d = float4(source.Load(int3(last.x, last.y, 0)));
    return lerp(lerp(a, b, blend.x), lerp(c, d, blend.x), blend.y);
}

// 정수 형식 int 텍스처의 수동 bilinear다.
float4 LoadBilinearInt(Texture2D<int4> source, float2 uv, uint2 textureSize) {
    int2 first = int2(0, 0);
    int2 last = int2(0, 0);
    float2 blend = float2(0.0, 0.0);
    BilinearFootprint(uv, textureSize, first, last, blend);
    const float4 a = float4(source.Load(int3(first.x, first.y, 0)));
    const float4 b = float4(source.Load(int3(last.x, first.y, 0)));
    const float4 c = float4(source.Load(int3(first.x, last.y, 0)));
    const float4 d = float4(source.Load(int3(last.x, last.y, 0)));
    return lerp(lerp(a, b, blend.x), lerp(c, d, blend.x), blend.y);
}
