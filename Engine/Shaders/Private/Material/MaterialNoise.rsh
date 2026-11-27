// 머티리얼 noise 라이브러리다(Docs/Design/MaterialSystem 6.13절 MS2f3). 생성 원문은 IR Library 연산을
// MaterialExpressionNoise·MaterialExpressionVectorNoise 호출로 낸다. 식은 UE 5.8 셰이더와 같다(결과가
// UE와 같아야 한다): Common.ush:1772–1877(진입 함수·옥타브), Random.ush:34–51·117–368·470–593(BBS·
// gradient·value·Voronoi·simplex), RandomPCG.ush:5–44(Rand3DPCG16), LWCOperations.ush:3–9(타일 정규화).
// LUT 함수(SimplexTex·GradientTex·GradientTex3D)는 번역이 막는다(MPQ-NoiseTexture — 여기에는 0을 내는
// 자리만 있다).
#pragma once

#include "/Engine/Private/Material/MaterialDouble.rsh"

// Blum-Blum-Shub 소수다(2^12 미만 최대 소수 — s*s가 24 bit 가수에 담긴다).
static const float MaterialNoiseBbsPrime = 4093.0;
// MGradient의 방향 bit mask와 크기다.
static const uint3 MaterialNoiseGradientMask = uint3(0x8000, 0x4000, 0x2000);
static const float3 MaterialNoiseGradientScale = float3(1.0 / 0x4000, 1.0 / 0x2000, 1.0 / 0x1000);
// 격자 seed 소수다.
static const float3 MaterialNoisePrimes = float3(19, 47, 101);
// Voronoi 반 칸 seed 오프셋이다(비슷한 이웃 무늬를 피하는 다른 난수 영역).
static const float MaterialNoiseVoronoiOffset = 467.0;
// simplex 크기 보정이다(-1..1).
static const float MaterialNoiseSimplexScale = 1024.0 / 375.0;

// 3D 정수 좌표의 16 bit 난수 셋이다(UE Rand3DPCG16 — LCG 뒤 Feistel MAD 여섯 단계).
uint3 MaterialNoiseRand3DPcg16(int3 p)
{
    uint3 v = uint3(p);
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v >> 16u;
}

// 정수 값 float seed의 [0, 1) 난수다(UE RandBBSfloat).
float MaterialNoiseRandBbs(float seed)
{
    float s = frac(seed / MaterialNoiseBbsPrime);
    s = frac(s * s * MaterialNoiseBbsPrime);
    s = frac(s * s * MaterialNoiseBbsPrime);
    return s;
}

// 타일링 좌표다(UE NoiseTileWrap).
float3 MaterialNoiseTileWrap(float3 v, bool tiling, float repeatSize)
{
    return tiling ? (frac(v / repeatSize) * repeatSize) : v;
}

// Perlin 보간 다항식이다(UE PerlinRamp).
float4 MaterialNoisePerlinRamp(float4 t)
{
    return t * t * t * (t * (t * 6 - 15) + 10);
}

// 격자점의 gradient 방향(xyz)과 기여(w)다(UE MGradient).
float4 MaterialNoiseGradient(int seed, float3 offset)
{
    uint rand = MaterialNoiseRand3DPcg16(int3(seed, 0, 0)).x;
    float3 direction = float3(rand.xxx & MaterialNoiseGradientMask) * MaterialNoiseGradientScale - 1;
    return float4(direction, dot(direction, offset));
}

// 여덟 꼭짓점 seed와 소수부다(UE NoiseSeeds).
float3 MaterialNoiseSeeds(float3 v, bool tiling, float repeatSize,
                          out float seed000, out float seed001, out float seed010, out float seed011,
                          out float seed100, out float seed101, out float seed110, out float seed111)
{
    float3 fv = frac(v);
    float3 iv = floor(v);
    if (tiling)
    {
        seed000 = dot(MaterialNoisePrimes, MaterialNoiseTileWrap(iv, true, repeatSize));
        seed100 = dot(MaterialNoisePrimes, MaterialNoiseTileWrap(iv + float3(1, 0, 0), true, repeatSize));
        seed010 = dot(MaterialNoisePrimes, MaterialNoiseTileWrap(iv + float3(0, 1, 0), true, repeatSize));
        seed110 = dot(MaterialNoisePrimes, MaterialNoiseTileWrap(iv + float3(1, 1, 0), true, repeatSize));
        seed001 = dot(MaterialNoisePrimes, MaterialNoiseTileWrap(iv + float3(0, 0, 1), true, repeatSize));
        seed101 = dot(MaterialNoisePrimes, MaterialNoiseTileWrap(iv + float3(1, 0, 1), true, repeatSize));
        seed011 = dot(MaterialNoisePrimes, MaterialNoiseTileWrap(iv + float3(0, 1, 1), true, repeatSize));
        seed111 = dot(MaterialNoisePrimes, MaterialNoiseTileWrap(iv + float3(1, 1, 1), true, repeatSize));
    }
    else
    {
        seed000 = dot(iv, MaterialNoisePrimes);
        seed100 = seed000 + MaterialNoisePrimes.x;
        seed010 = seed000 + MaterialNoisePrimes.y;
        seed110 = seed100 + MaterialNoisePrimes.y;
        seed001 = seed000 + MaterialNoisePrimes.z;
        seed101 = seed100 + MaterialNoisePrimes.z;
        seed011 = seed010 + MaterialNoisePrimes.z;
        seed111 = seed110 + MaterialNoisePrimes.z;
    }

    return fv;
}

// Perlin 식 "modified noise"다(UE GradientNoise3D_ALU — -1..1).
float MaterialNoiseGradient3D(float3 v, bool tiling, float repeatSize)
{
    float seed000, seed001, seed010, seed011, seed100, seed101, seed110, seed111;
    float3 fv = MaterialNoiseSeeds(v, tiling, repeatSize, seed000, seed001, seed010, seed011,
                                   seed100, seed101, seed110, seed111);
    float rand000 = MaterialNoiseGradient(int(seed000), fv - float3(0, 0, 0)).w;
    float rand100 = MaterialNoiseGradient(int(seed100), fv - float3(1, 0, 0)).w;
    float rand010 = MaterialNoiseGradient(int(seed010), fv - float3(0, 1, 0)).w;
    float rand110 = MaterialNoiseGradient(int(seed110), fv - float3(1, 1, 0)).w;
    float rand001 = MaterialNoiseGradient(int(seed001), fv - float3(0, 0, 1)).w;
    float rand101 = MaterialNoiseGradient(int(seed101), fv - float3(1, 0, 1)).w;
    float rand011 = MaterialNoiseGradient(int(seed011), fv - float3(0, 1, 1)).w;
    float rand111 = MaterialNoiseGradient(int(seed111), fv - float3(1, 1, 1)).w;
    float3 weights = MaterialNoisePerlinRamp(float4(fv, 0)).xyz;
    float i = lerp(lerp(rand000, rand100, weights.x), lerp(rand010, rand110, weights.x), weights.y);
    float j = lerp(lerp(rand001, rand101, weights.x), lerp(rand011, rand111, weights.x), weights.y);
    return lerp(i, j, weights.z).x;
}

// value noise다(UE ValueNoise3D_ALU — -1..1).
float MaterialNoiseValue3D(float3 v, bool tiling, float repeatSize)
{
    float seed000, seed001, seed010, seed011, seed100, seed101, seed110, seed111;
    float3 fv = MaterialNoiseSeeds(v, tiling, repeatSize, seed000, seed001, seed010, seed011,
                                   seed100, seed101, seed110, seed111);
    float rand000 = MaterialNoiseRandBbs(seed000) * 2 - 1;
    float rand100 = MaterialNoiseRandBbs(seed100) * 2 - 1;
    float rand010 = MaterialNoiseRandBbs(seed010) * 2 - 1;
    float rand110 = MaterialNoiseRandBbs(seed110) * 2 - 1;
    float rand001 = MaterialNoiseRandBbs(seed001) * 2 - 1;
    float rand101 = MaterialNoiseRandBbs(seed101) * 2 - 1;
    float rand011 = MaterialNoiseRandBbs(seed011) * 2 - 1;
    float rand111 = MaterialNoiseRandBbs(seed111) * 2 - 1;
    float3 weights = MaterialNoisePerlinRamp(float4(fv, 0)).xyz;
    float i = lerp(lerp(rand000, rand100, weights.x), lerp(rand010, rand110, weights.x), weights.y);
    float j = lerp(lerp(rand001, rand101, weights.x), lerp(rand011, rand111, weights.x), weights.y);
    return lerp(i, j, weights.z).x;
}

// Voronoi 칸 안의 흔들림 오프셋이다(UE VoronoiCornerSample).
float3 MaterialNoiseVoronoiCorner(float3 pos, int quality)
{
    float3 noise = float3(MaterialNoiseRand3DPcg16(int3(pos))) / 0xffff - 0.5;
    if (quality <= 2)
    {
        return normalize(noise) * 0.2588;
    }

    if (quality == 3)
    {
        return normalize(noise) * 0.3090;
    }

    return noise;
}

// 지금까지 가장 가까운 점과 새 후보를 견준다(UE VoronoiCompare).
float4 MaterialNoiseVoronoiCompare(float4 minval, float3 candidate, float3 offset, bool distanceOnly)
{
    if (distanceOnly)
    {
        return float4(0, 0, 0, min(minval.w, dot(offset, offset)));
    }

    float newdist = dot(offset, offset);
    return newdist > minval.w ? minval : float4(candidate, newdist);
}

// Worley noise다(UE VoronoiNoise3D_ALU — xyz 가장 가까운 점, w 거리).
float4 MaterialNoiseVoronoi3D(float3 v, int quality, bool tiling, float repeatSize, bool distanceOnly)
{
    float3 fv = frac(v), fv2 = frac(v + 0.5);
    float3 iv = floor(v), iv2 = floor(v + 0.5);
    float4 mindist = float4(0, 0, 0, 100);
    float3 p, offset;
    if (quality == 3)
    {
        [unroll] for (offset.x = -1; offset.x <= 1; ++offset.x)
        {
            [unroll] for (offset.y = -1; offset.y <= 1; ++offset.y)
            {
                [unroll] for (offset.z = -1; offset.z <= 1; ++offset.z)
                {
                    p = offset + MaterialNoiseVoronoiCorner(MaterialNoiseTileWrap(iv2 + offset, tiling, repeatSize), quality);
                    mindist = MaterialNoiseVoronoiCompare(mindist, iv2 + p, fv2 - p, distanceOnly);
                }
            }
        }
    }
    else
    {
        [unroll] for (offset.x = 0; offset.x <= 1; ++offset.x)
        {
            [unroll] for (offset.y = 0; offset.y <= 1; ++offset.y)
            {
                [unroll] for (offset.z = 0; offset.z <= 1; ++offset.z)
                {
                    p = offset + MaterialNoiseVoronoiCorner(MaterialNoiseTileWrap(iv + offset, tiling, repeatSize), quality);
                    mindist = MaterialNoiseVoronoiCompare(mindist, iv + p, fv - p, distanceOnly);
                    if (quality == 2)
                    {
                        p = offset + MaterialNoiseVoronoiCorner(MaterialNoiseTileWrap(iv2 + offset, tiling, repeatSize) + MaterialNoiseVoronoiOffset, quality);
                        mindist = MaterialNoiseVoronoiCompare(mindist, iv2 + p, fv2 - p, distanceOnly);
                    }
                }
            }
        }
    }

    if (quality >= 4)
    {
        [unroll] for (offset.x = -1; offset.x <= 2; offset.x += 3)
        {
            [unroll] for (offset.y = 0; offset.y <= 1; ++offset.y)
            {
                [unroll] for (offset.z = 0; offset.z <= 1; ++offset.z)
                {
                    p = offset.xyz + MaterialNoiseVoronoiCorner(MaterialNoiseTileWrap(iv + offset.xyz, tiling, repeatSize), quality);
                    mindist = MaterialNoiseVoronoiCompare(mindist, iv + p, fv - p, distanceOnly);
                    p = offset.yzx + MaterialNoiseVoronoiCorner(MaterialNoiseTileWrap(iv + offset.yzx, tiling, repeatSize), quality);
                    mindist = MaterialNoiseVoronoiCompare(mindist, iv + p, fv - p, distanceOnly);
                    p = offset.zxy + MaterialNoiseVoronoiCorner(MaterialNoiseTileWrap(iv + offset.zxy, tiling, repeatSize), quality);
                    mindist = MaterialNoiseVoronoiCompare(mindist, iv + p, fv - p, distanceOnly);
                }
            }
        }
    }

    return float4(mindist.xyz, sqrt(mindist.w));
}

// simplex 사면체 꼭짓점 넷이다(UE SimplexCorners).
float4x3 MaterialNoiseSimplexCorners(float3 v)
{
    float3 tet = floor(v + v.x / 3 + v.y / 3 + v.z / 3);
    float3 base = tet - tet.x / 6 - tet.y / 6 - tet.z / 6;
    float3 f = v - base;
    float3 g = step(f.yzx, f.xyz), h = 1 - g.zxy;
    float3 a1 = min(g, h) - 1.0 / 6.0, a2 = max(g, h) - 1.0 / 3.0;
    return float4x3(base, base + a1, base + a2, base + 0.5);
}

// simplex 가중치다(UE SimplexSmooth).
float4 MaterialNoiseSimplexSmooth(float4x3 f)
{
    float4 d = float4(dot(f[0], f[0]), dot(f[1], f[1]), dot(f[2], f[2]), dot(f[3], f[3]));
    float4 s = saturate(2 * d);
    return (1 * MaterialNoiseSimplexScale + s * (-3 * MaterialNoiseSimplexScale + s * (3 * MaterialNoiseSimplexScale - s * MaterialNoiseSimplexScale)));
}

// simplex 가중치의 x·y·z 미분이다(UE SimplexDSmooth).
float3x4 MaterialNoiseSimplexDSmooth(float4x3 f)
{
    float4 d = float4(dot(f[0], f[0]), dot(f[1], f[1]), dot(f[2], f[2]), dot(f[3], f[3]));
    float4 s = saturate(2 * d);
    s = -12 * MaterialNoiseSimplexScale + s * (24 * MaterialNoiseSimplexScale - s * 12 * MaterialNoiseSimplexScale);
    return float3x4(
        s * float4(f[0][0], f[1][0], f[2][0], f[3][0]),
        s * float4(f[0][1], f[1][1], f[2][1], f[3][1]),
        s * float4(f[0][2], f[1][2], f[2][2], f[3][2]));
}

// simplex noise와 그 Jacobian이다(UE JacobianSimplex_ALU — J[i].w가 벡터 noise, J[i].xyz가 미분).
float3x4 MaterialNoiseJacobianSimplex(float3 v, bool tiling, float repeatSize)
{
    float4x3 t = MaterialNoiseSimplexCorners(v);
    uint3 rand;
    float4x3 gvec[3], fv;
    float3x4 grad;
    [unroll] for (int corner = 0; corner < 4; ++corner)
    {
        fv[corner] = v - t[corner];
        rand = MaterialNoiseRand3DPcg16(int3(floor(MaterialNoiseTileWrap(6 * t[corner] + 0.5, tiling, repeatSize))));
        gvec[0][corner] = float3(rand.xxx & MaterialNoiseGradientMask) * MaterialNoiseGradientScale - 1;
        gvec[1][corner] = float3(rand.yyy & MaterialNoiseGradientMask) * MaterialNoiseGradientScale - 1;
        gvec[2][corner] = float3(rand.zzz & MaterialNoiseGradientMask) * MaterialNoiseGradientScale - 1;
        grad[0][corner] = dot(gvec[0][corner], fv[corner]);
        grad[1][corner] = dot(gvec[1][corner], fv[corner]);
        grad[2][corner] = dot(gvec[2][corner], fv[corner]);
    }

    float4 sv = MaterialNoiseSimplexSmooth(fv);
    float3x4 ds = MaterialNoiseSimplexDSmooth(fv);
    float3x4 jacobian;
    jacobian[0] = float4(mul(sv, gvec[0]) + mul(ds, grad[0]), dot(sv, grad[0]));
    jacobian[1] = float4(mul(sv, gvec[1]) + mul(ds, grad[1]), dot(sv, grad[1]));
    jacobian[2] = float4(mul(sv, gvec[2]) + mul(ds, grad[2]), dot(sv, grad[2]));
    return jacobian;
}

// 함수 선택이다(UE Noise3D_Multiplexer — 0–2 LUT 함수는 번역이 막는다).
float MaterialNoise3D(int function, float3 position, int quality, bool tiling, float repeatSize)
{
    switch (function)
    {
    case 3:
        return MaterialNoiseGradient3D(position, tiling, repeatSize);
    case 4:
        return MaterialNoiseValue3D(position, tiling, repeatSize);
    case 5:
        return MaterialNoiseVoronoi3D(position, quality, tiling, repeatSize, true).w * 2.0 - 1.0;
    default:
        return 0;
    }
}

// Noise 노드다(UE MaterialExpressionNoise — 결과는 OutputMin..OutputMax).
float MaterialExpressionNoise(float3 position, float scale, int quality, int function, bool turbulence,
                              uint levels, float outputMin, float outputMax, float levelScale,
                              float filterWidth, bool tiling, float repeatSize)
{
    position *= scale;
    filterWidth *= scale;
    float result = 0.0f;
    float outScale = 1.0f;
    float inverseLevelScale = 1.0f / levelScale;
    [loop] for (uint level = 0; level < levels; ++level)
    {
        outScale *= saturate(1.0 - filterWidth);
        if (turbulence)
        {
            result += abs(MaterialNoise3D(function, position, quality, tiling, repeatSize)) * outScale;
        }
        else
        {
            result += MaterialNoise3D(function, position, quality, tiling, repeatSize) * outScale;
        }

        position *= levelScale;
        repeatSize *= levelScale;
        outScale *= inverseLevelScale;
        filterWidth *= levelScale;
    }

    if (!turbulence)
    {
        result = result * 0.5f + 0.5f;
    }

    return lerp(outputMin, outputMax, result);
}

// LWC 위치 판이다(UE FLWCVector3 오버로드 — 타일 안 오프셋, LWCNormalizeTile).
float MaterialExpressionNoise(MaterialDouble3 position, float scale, int quality, int function,
                              bool turbulence, uint levels, float outputMin, float outputMax,
                              float levelScale, float filterWidth, bool tiling, float repeatSize)
{
    float3 tile = floor(position.tile + (position.offset * MaterialDoubleTileSizeRcp + 0.5f));
    float3 local = (position.tile - tile) * MaterialDoubleTileSize + position.offset;
    return MaterialExpressionNoise(local, scale, quality, function, turbulence, levels, outputMin,
                                   outputMax, levelScale, filterWidth, tiling, repeatSize);
}

// VectorNoise 노드다(UE MaterialExpressionVectorNoise).
float4 MaterialExpressionVectorNoise(float3 position, int quality, int function, bool tiling, float tileSize)
{
    float4 result = float4(0, 0, 0, 1);
    float3x4 jacobian = MaterialNoiseJacobianSimplex(position, tiling, tileSize);
    switch (function)
    {
    case 0:
        result.xyz = float3(MaterialNoiseRand3DPcg16(int3(floor(MaterialNoiseTileWrap(position, tiling, tileSize))))) / 0xffff;
        break;
    case 1:
        result.xyz = float3(jacobian[0].w, jacobian[1].w, jacobian[2].w);
        break;
    case 2:
        result = jacobian[0];
        break;
    case 3:
        result.xyz = float3(jacobian[2][1] - jacobian[1][2], jacobian[0][2] - jacobian[2][0], jacobian[1][0] - jacobian[0][1]);
        break;
    default:
        result = MaterialNoiseVoronoi3D(position, quality, tiling, tileSize, false);
        break;
    }

    return result;
}
