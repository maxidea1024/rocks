// UE 5.8 TextureCompressorModule.cpp·TextureLODSettings.cpp 함수 구간을 Rocks 밖에서 컴파일하기
// 위한 대체 정의다(Docs/Design/AssetSystem 12.18 TP-D4). make_golden.py가 UE 클론에서 잘라 온
// 구간(TcmExtract.inc) 앞에 이 헤더를 둔다. UE 헤더는 쓰지 않는다 — 구간이 쓰는 타입·함수만 같은
// 의미로 정의한다(SSE 벡터 연산은 원소별 float 연산과 같은 순서의 스칼라 식).
//
// 초월 함수 판:
// - 기준 판(ROCKS_PROBE_UE_LIBM = 0): Rocks DeterministicMath(같이 컴파일)로 계산하고 float로
//   반올림한다. Rocks 빌드와 정의상 같은 값이다 — 이 판의 출력이 시험 golden이다.
// - UE 판(ROCKS_PROBE_UE_LIBM = 1): UE FMath처럼 호스트 float libm을 부른다. 두 판의 차이가 UE와
//   Rocks의 차이다.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

#if !ROCKS_PROBE_UE_LIBM
#include <DeterministicMath/DeterministicMath.h>
#endif

#ifndef ROCKS_PROBE_UE_LIBM
#error "Define ROCKS_PROBE_UE_LIBM to 0 (reference build) or 1 (UE libm build)."
#endif

using int32 = std::int32_t;
using uint32 = std::uint32_t;
using int64 = std::int64_t;
using uint64 = std::uint64_t;
using uint8 = std::uint8_t;

#define check(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            std::fprintf(stderr, "probe check failed: %s (%s:%d)\n", #expression, __FILE__,        \
                         __LINE__);                                                                \
            std::_Exit(3);                                                                         \
        }                                                                                          \
    } while (0)
#define checkSlow(expression) check(expression)
#define checkf(expression, ...) check(expression)
#define TEXT(text) text
#define TRACE_CPUPROFILER_EVENT_SCOPE(name)
#define UE_LOGF(...)
#define MoveTemp(value) std::move(value)
#define PLATFORM_CACHE_LINE_SIZE 64

#undef PI
constexpr float PI = 3.1415926535897932f;
constexpr float SMALL_NUMBER = 1.e-8f;
constexpr float KINDA_SMALL_NUMBER = 1.e-4f;
constexpr uint32 MAX_uint32 = 0xffffffffu;

// Rocks build_kernel과 같은 식이다(exp(x) = Exp2(x · log2 e) — 12.18 TP-D3).
constexpr double ProbeLog2E = 1.4426950408889634073599246810019;

struct FMath {
    template <class T>
    static T Square(T value) {
        return value * value;
    }

    // UE FMath는 섞인 정수 타입(int32·int64)을 받는다 — 공통 타입으로 비교한다.
    template <class T, class U, class V>
    static auto Clamp(T value, U minimum, V maximum) -> decltype(value + minimum + maximum) {
        using C = decltype(value + minimum + maximum);
        return static_cast<C>(value) < static_cast<C>(minimum)
                   ? static_cast<C>(minimum)
                   : (static_cast<C>(value) < static_cast<C>(maximum) ? static_cast<C>(value)
                                                                      : static_cast<C>(maximum));
    }

    template <class T, class U>
    static auto Min(T a, U b) -> decltype(a + b) {
        using C = decltype(a + b);
        return static_cast<C>(a) <= static_cast<C>(b) ? static_cast<C>(a) : static_cast<C>(b);
    }

    template <class T, class U>
    static auto Max(T a, U b) -> decltype(a + b) {
        using C = decltype(a + b);
        return static_cast<C>(a) >= static_cast<C>(b) ? static_cast<C>(a) : static_cast<C>(b);
    }

    template <class T>
    static bool IsPowerOfTwo(T value) {
        return value > 0 && (value & (value - 1)) == 0;
    }

    template <class T>
    static T DivideAndRoundUp(T dividend, T divisor) {
        return (dividend + divisor - 1) / divisor;
    }

    // sqrt는 IEEE 기본 연산이라 두 판이 같다(UE sqrtf = 정확 반올림).
    static float Sqrt(float value) {
        return sqrtf(value);
    }

#if ROCKS_PROBE_UE_LIBM
    static float Exp(float value) {
        return expf(value);
    }

    static float Pow(float base, float exponent) {
        return powf(base, exponent);
    }
#else
    static float Exp(float value) {
        return static_cast<float>(
            rocks::deterministic::Exp2(static_cast<double>(value) * ProbeLog2E));
    }

    static float Pow(float base, float exponent) {
        return static_cast<float>(
            rocks::deterministic::Pow(static_cast<double>(base), static_cast<double>(exponent)));
    }
#endif
};

struct FLinearColor {
    float R = 0.0f;
    float G = 0.0f;
    float B = 0.0f;
    float A = 0.0f;

    FLinearColor() = default;

    FLinearColor(float r, float g, float b, float a = 1.0f) : R(r), G(g), B(b), A(a) {
    }

    float& Component(int32 index) {
        return (&R)[index];
    }

    const float& Component(int32 index) const {
        return (&R)[index];
    }

    FLinearColor operator+(const FLinearColor& other) const {
        return FLinearColor(R + other.R, G + other.G, B + other.B, A + other.A);
    }

    FLinearColor& operator+=(const FLinearColor& other) {
        R += other.R;
        G += other.G;
        B += other.B;
        A += other.A;
        return *this;
    }

    FLinearColor operator*(float scalar) const {
        return FLinearColor(R * scalar, G * scalar, B * scalar, A * scalar);
    }

    // UE Color.h 308–315: 역수를 곱한다(나눗셈이 아니다).
    FLinearColor& operator/=(float scalar) {
        const float inverse = 1.0f / scalar;
        R *= inverse;
        G *= inverse;
        B *= inverse;
        A *= inverse;
        return *this;
    }
};

struct FVector4f {
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    float W = 0.0f;

    FVector4f() = default;

    FVector4f(float x, float y, float z, float w) : X(x), Y(y), Z(z), W(w) {
    }

    float& operator[](int32 index) {
        return (&X)[index];
    }

    const float& operator[](int32 index) const {
        return (&X)[index];
    }

    bool operator!=(const FVector4f& other) const {
        return X != other.X || Y != other.Y || Z != other.Z || W != other.W;
    }

    // UE Vector4.h 820: 허용 오차 이하(<=)면 같다.
    bool Equals(const FVector4f& other, float tolerance = KINDA_SMALL_NUMBER) const {
        return fabsf(X - other.X) <= tolerance && fabsf(Y - other.Y) <= tolerance &&
               fabsf(Z - other.Z) <= tolerance && fabsf(W - other.W) <= tolerance;
    }
};

template <class T>
struct TArray {
    std::vector<T> Items;

    int64 Num() const {
        return static_cast<int64>(Items.size());
    }

    T& operator[](int64 index) {
        return Items[static_cast<size_t>(index)];
    }

    const T& operator[](int64 index) const {
        return Items[static_cast<size_t>(index)];
    }

    T& Last() {
        return Items.back();
    }

    T* GetData() {
        return Items.data();
    }

    const T* GetData() const {
        return Items.data();
    }

    int64 GetSlack() const {
        return static_cast<int64>(Items.capacity() - Items.size());
    }

    void Empty(int64 slack = 0) {
        Items.clear();
        Items.shrink_to_fit();
        Items.reserve(static_cast<size_t>(slack));
    }

    void AddUninitialized(int64 count) {
        Items.resize(Items.size() + static_cast<size_t>(count));
    }

    template <class... Args>
    T& Emplace_GetRef(Args&&... args) {
        check(Items.size() < Items.capacity());
        Items.emplace_back(std::forward<Args>(args)...);
        return Items.back();
    }
};

template <class T>
using TArray64 = TArray<T>;

namespace ERawImageFormat {
enum Type {
    RGBA32F,
};
} // namespace ERawImageFormat

enum class EGammaSpace {
    Linear,
};

// RGBA32F 그림 하나다(구간이 쓰는 멤버만). SizeX·SizeY·NumSlices만 줄여 다시 쓰는 사용(평균 사슬
// 임시 그림)을 위해 RawData는 처음 크기를 그대로 둔다.
struct FImage {
    int64 SizeX = 0;
    int64 SizeY = 0;
    int64 NumSlices = 1;
    ERawImageFormat::Type Format = ERawImageFormat::RGBA32F;
    EGammaSpace GammaSpace = EGammaSpace::Linear;
    TArray64<uint8> RawData;

    FImage() = default;

    FImage(int64 x, int64 y, int64 slices, ERawImageFormat::Type format,
           EGammaSpace gamma = EGammaSpace::Linear) {
        Init(x, y, slices, format, gamma);
    }

    void Init(int64 x, int64 y, int64 slices, ERawImageFormat::Type format,
              EGammaSpace gamma = EGammaSpace::Linear) {
        SizeX = x;
        SizeY = y;
        NumSlices = slices;
        Format = format;
        GammaSpace = gamma;
        RawData.Items.assign(static_cast<size_t>(x * y * slices) * sizeof(FLinearColor), 0);
    }

    FLinearColor* AsRGBA32F() {
        return reinterpret_cast<FLinearColor*>(RawData.Items.data());
    }

    const FLinearColor* AsRGBA32F() const {
        return reinterpret_cast<const FLinearColor*>(RawData.Items.data());
    }

    int64 GetNumPixels() const {
        return SizeX * SizeY * NumSlices;
    }

    int64 GetSliceNumPixels() const {
        return SizeX * SizeY;
    }
};

// ---- 병렬 helper(직렬 — 결과는 행 분할과 무관) ----

enum class EParallelForFlags {
    None,
    Unbalanced,
};

inline int32 ImageParallelForComputeNumJobsForRows(int32& rowsPerJob, int64 sizeX, int64 sizeY) {
    (void)sizeX;
    rowsPerJob = static_cast<int32>(sizeY);
    return 1;
}

template <class Body>
void ParallelFor(const char* name, int32 count, int32 minBatch, Body body,
                 EParallelForFlags flags = EParallelForFlags::None) {
    (void)name;
    (void)minBatch;
    (void)flags;
    for (int32 index = 0; index < count; ++index) {
        body(index);
    }
}

struct FPlatformAtomics {
    static int64 InterlockedAdd(int64* value, int64 amount) {
        const int64 old = *value;
        *value += amount;
        return old;
    }
};

namespace LowLevelTasks {
enum class ETaskPriority {
    BackgroundNormal,
};
} // namespace LowLevelTasks

namespace UE::Tasks {

template <class T>
struct TTask {
    void GetResult() {
    }
};

template <class Body>
TTask<void> Launch(const char* name, Body body) {
    (void)name;
    body();
    return {};
}

template <class Body>
TTask<void> Launch(const char* name, Body body, LowLevelTasks::ETaskPriority priority) {
    (void)priority;
    return Launch(name, body);
}

} // namespace UE::Tasks

// ---- SSE 벡터 연산(원소별 float — _mm_add_ps·_mm_mul_ps·_mm_div_ps와 같은 IEEE 결과) ----

struct VectorRegister4Float {
    float V[4];
};

inline uint32 ProbeBits(float value) {
    uint32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

inline float ProbeFloat(uint32 bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

inline VectorRegister4Float VectorLoad(const float* data) {
    return {{data[0], data[1], data[2], data[3]}};
}

inline void VectorStore(const VectorRegister4Float& value, float* data) {
    for (int i = 0; i < 4; ++i) {
        data[i] = value.V[i];
    }
}

inline VectorRegister4Float VectorSetFloat1(float value) {
    return {{value, value, value, value}};
}

inline VectorRegister4Float VectorZeroFloat() {
    return VectorSetFloat1(0.0f);
}

inline VectorRegister4Float MakeVectorRegisterFloat(float x, float y, float z, float w) {
    return {{x, y, z, w}};
}

inline VectorRegister4Float MakeVectorRegisterFloatMask(uint32 x, uint32 y, uint32 z, uint32 w) {
    return {{ProbeFloat(x), ProbeFloat(y), ProbeFloat(z), ProbeFloat(w)}};
}

inline VectorRegister4Float VectorAdd(const VectorRegister4Float& a,
                                      const VectorRegister4Float& b) {
    return {{a.V[0] + b.V[0], a.V[1] + b.V[1], a.V[2] + b.V[2], a.V[3] + b.V[3]}};
}

inline VectorRegister4Float VectorMultiply(const VectorRegister4Float& a,
                                           const VectorRegister4Float& b) {
    return {{a.V[0] * b.V[0], a.V[1] * b.V[1], a.V[2] * b.V[2], a.V[3] * b.V[3]}};
}

inline VectorRegister4Float VectorDivide(const VectorRegister4Float& a,
                                         const VectorRegister4Float& b) {
    return {{a.V[0] / b.V[0], a.V[1] / b.V[1], a.V[2] / b.V[2], a.V[3] / b.V[3]}};
}

// UnrealMathSSE.h 1493–1506: (x·x' + y·y') + z·z'를 네 칸에 복제한다.
inline VectorRegister4Float VectorDot3(const VectorRegister4Float& a,
                                       const VectorRegister4Float& b) {
    const VectorRegister4Float product = VectorMultiply(a, b);
    const float sum = (product.V[0] + product.V[1]) + product.V[2];
    return VectorSetFloat1(sum);
}

inline VectorRegister4Float VectorCompareGT(const VectorRegister4Float& a,
                                            const VectorRegister4Float& b) {
    VectorRegister4Float out;
    for (int i = 0; i < 4; ++i) {
        out.V[i] = ProbeFloat(a.V[i] > b.V[i] ? 0xffffffffu : 0u);
    }

    return out;
}

inline VectorRegister4Float VectorSelect(const VectorRegister4Float& mask,
                                         const VectorRegister4Float& a,
                                         const VectorRegister4Float& b) {
    VectorRegister4Float out;
    for (int i = 0; i < 4; ++i) {
        const uint32 m = ProbeBits(mask.V[i]);
        out.V[i] = ProbeFloat((m & ProbeBits(a.V[i])) | (~m & ProbeBits(b.V[i])));
    }

    return out;
}

// ---- 빌드 설정과 텍스처(구간이 읽는 칸만, 값은 UE enum 값) ----

enum TextureMipGenSettings : int {
    TMGS_FromTextureGroup,
    TMGS_SimpleAverage,
    TMGS_Sharpen0,
    TMGS_Sharpen1,
    TMGS_Sharpen2,
    TMGS_Sharpen3,
    TMGS_Sharpen4,
    TMGS_Sharpen5,
    TMGS_Sharpen6,
    TMGS_Sharpen7,
    TMGS_Sharpen8,
    TMGS_Sharpen9,
    TMGS_Sharpen10,
    TMGS_NoMipmaps,
    TMGS_LeaveExistingMips,
    TMGS_Blur1,
    TMGS_Blur2,
    TMGS_Blur3,
    TMGS_Blur4,
    TMGS_Blur5,
    TMGS_Unfiltered,
    TMGS_Angular,
};

enum TextureCompressionSettings : int {
    TC_Default,
    TC_Normalmap,
    TC_Masks,
};

enum TextureAddress : int {
    TA_Wrap,
    TA_Clamp,
    TA_Mirror,
};

enum class ETextureClass {
    TwoD,
    Cube,
    Array,
    CubeArray,
    Volume,
};

struct FTextureBuildSettings {
    uint8 MipGenSettings = TMGS_SimpleAverage;
    float MipSharpening = 0.0f;
    uint32 SharpenMipKernelSize = 2;
    bool bDownsampleWithAverage = true;
    bool bSharpenWithoutColorShift = true;
    bool bBorderColorBlack = false;
    bool bPreserveBorder = false;
    bool bCubemap = false;
    bool bVolume = false;
    bool bTextureArray = false;
    bool bUseNewMipFilter = true;
    bool bDoScaleMipsForAlphaCoverage = false;
    FVector4f AlphaCoverageThresholds;
    int32 TextureAddressModeX = TA_Wrap;
    int32 TextureAddressModeY = TA_Wrap;
};

struct ITextureCompressorModule {
    static void GenerateMipChain(const FTextureBuildSettings& Settings, const FImage& BaseImage,
                                 TArray<FImage>& OutMipChain, uint32 MipChainDepth = MAX_uint32);
};

struct FTextureLODGroup {
    TextureMipGenSettings MipGenSettings = TMGS_SimpleAverage;
};

struct UTexture {
    TextureMipGenSettings MipGenSettings = TMGS_SimpleAverage;
    TextureCompressionSettings CompressionSettings = TC_Default;
    int32 LODGroup = 0;
    ETextureClass Class = ETextureClass::TwoD;

    bool IsNormalMap() const {
        return CompressionSettings == TC_Normalmap;
    }

    ETextureClass GetTextureClass() const {
        return Class;
    }
};

struct UTextureLODSettings {
    std::vector<FTextureLODGroup> TextureLODGroups = std::vector<FTextureLODGroup>(1);

    void GetMipGenSettings(const UTexture& Texture, TextureMipGenSettings& OutMipGenSettings,
                           float& OutSharpen, uint32& OutKernelSize,
                           bool& bOutDownsampleWithAverage, bool& bOutSharpenWithoutColorShift,
                           bool& bOutBorderColorBlack) const;
};
