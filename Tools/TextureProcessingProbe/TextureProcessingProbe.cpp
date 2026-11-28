// 텍스처 보정 golden probe다(Docs/Design/AssetSystem 12.18 TP-D4).
//
// make_golden.py가 UE 5.8 클론의 TextureCompressorModule.cpp에서 함수 구간을 잘라 TcmExtract.inc로
// 묶고, ProbeShim.h와 함께 이 파일을 컴파일한다. UE 코드는 저장소에 복사하지 않는다.
//
// 출력(little endian):
//   KernelGolden.bin — "RTPK", 판 1, 항목 수, 항목마다 { 크기 u32, sharpen f32, 1D 가중치 f32 ×
//   크기, 2D 가중치 f32 × 크기² } (UE FImageKernel2D::BuildSeparatableGaussWithSharpen 결과).
//   MipChainGolden.bin — "RTPM", 판 1, 경우 수, 경우마다(ProcessingGoldenImages.h MipCases 순서)
//   { 이름 길이 u32·이름, mip 수 u32, mip마다 { X·Y·slice 수 u32, FNV-1a 64 u64 }, 전체 float 수가
//   FullDataFloats 이하면 mip마다 float 전부 }.
//
// 사용: TextureProcessingProbe <출력 디렉터리>

#include "ProbeShim.h"

#include "TcmExtract.inc"

#include "../../Engine/Source/Developer/TextureTools/Tests/ProcessingGoldenImages.h"

#include <cstring>
#include <string>

namespace {

constexpr uint32 KernelGoldenVersion = 1;

struct KernelCase {
    uint32 size;
    float sharpen;
};

void PutU32(std::vector<uint8>& out, uint32 value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<uint8>(value >> shift));
    }
}

void PutF32(std::vector<uint8>& out, float value) {
    uint32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    PutU32(out, bits);
}

bool WriteFile(const std::string& path, const std::vector<uint8>& bytes) {
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }

    const bool written = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    return std::fclose(file) == 0 && written;
}

// UE가 만드는 커널 전부다: SimpleAverage 2, Blur n(UE GetMipGenSettings — 크기 2 + 2n, sharpen
// −2n), Sharpen n(크기 8, sharpen n × 0.2f — float 곱), 합성 법선 사슬(6·7탭 Gauss −3.5 — TCM
// 3951·1263).
std::vector<KernelCase> KernelCases() {
    std::vector<KernelCase> cases;
    cases.push_back({2, 0.0f});
    for (int32 blur = 1; blur <= 5; ++blur) {
        cases.push_back({static_cast<uint32>(2 + 2 * blur), static_cast<float>(-blur * 2)});
    }

    for (int32 sharpen = 0; sharpen <= 10; ++sharpen) {
        cases.push_back({8, sharpen * 0.2f});
    }

    cases.push_back({6, -3.5f});
    cases.push_back({7, -3.5f});
    return cases;
}

std::vector<uint8> KernelGolden() {
    const std::vector<KernelCase> cases = KernelCases();
    std::vector<uint8> out = {'R', 'T', 'P', 'K'};
    PutU32(out, KernelGoldenVersion);
    PutU32(out, static_cast<uint32>(cases.size()));
    for (const KernelCase& one : cases) {
        FImageKernel2D kernel;
        kernel.BuildSeparatableGaussWithSharpen(one.size, one.sharpen);
        check(kernel.GetFilterTableSize() == one.size);
        PutU32(out, one.size);
        PutF32(out, one.sharpen);
        for (uint32 x = 0; x < one.size; ++x) {
            PutF32(out, kernel.Get1D(x));
        }

        for (uint32 y = 0; y < one.size; ++y) {
            for (uint32 x = 0; x < one.size; ++x) {
                PutF32(out, kernel.GetAt(x, y));
            }
        }
    }

    return out;
}

namespace pg = rocks::test::processing_golden;

constexpr uint32 MipChainGoldenVersion = 1;

void PutU64(std::vector<uint8>& out, uint64 value) {
    PutU32(out, static_cast<uint32>(value));
    PutU32(out, static_cast<uint32>(value >> 32));
}

ETextureClass ClassOf(pg::Dimension dimension) {
    switch (dimension) {
    case pg::Volume:
        return ETextureClass::Volume;
    case pg::Cube:
        return ETextureClass::Cube;
    case pg::CubeArray:
        return ETextureClass::CubeArray;
    case pg::Array:
        return ETextureClass::Array;
    default:
        return ETextureClass::TwoD;
    }
}

// UE GetTextureBuildSettings(TDD 1140–1340)의 mip 관련 칸이다. GetMipGenSettings는 잘라 온
// 원본이고, alpha coverage 조건(TDD 1159–1170)만 손으로 옮긴다.
FTextureBuildSettings SettingsFor(const pg::MipCase& one) {
    UTexture texture;
    texture.MipGenSettings = static_cast<TextureMipGenSettings>(one.mipGen);
    texture.CompressionSettings = static_cast<TextureCompressionSettings>(one.compression);
    texture.Class = ClassOf(one.dimension);
    UTextureLODSettings lod;
    FTextureBuildSettings settings;
    TextureMipGenSettings mipGen = TMGS_SimpleAverage;
    lod.GetMipGenSettings(texture, mipGen, settings.MipSharpening, settings.SharpenMipKernelSize,
                          settings.bDownsampleWithAverage, settings.bSharpenWithoutColorShift,
                          settings.bBorderColorBlack);
    settings.MipGenSettings = static_cast<uint8>(mipGen);
    settings.bPreserveBorder = one.preserveBorder;
    settings.bCubemap = one.dimension == pg::Cube || one.dimension == pg::CubeArray;
    settings.bVolume = one.dimension == pg::Volume;
    settings.bTextureArray = one.dimension == pg::Array || one.dimension == pg::CubeArray;
    settings.bUseNewMipFilter = one.useNewMipFilter;
    settings.TextureAddressModeX = static_cast<int32>(one.addressX);
    settings.TextureAddressModeY = static_cast<int32>(one.addressY);
    // UE 텍스처 값은 double(FVector4)이고 빌드 설정은 FVector4f다.
    const double thresholds[4] = {one.thresholdMilli[0] / 1000.0, one.thresholdMilli[1] / 1000.0,
                                  one.thresholdMilli[2] / 1000.0, one.thresholdMilli[3] / 1000.0};
    if (thresholds[0] != 0.0 || thresholds[1] != 0.0 || thresholds[2] != 0.0 ||
        thresholds[3] != 0.0) {
        settings.bDoScaleMipsForAlphaCoverage = true;
        settings.AlphaCoverageThresholds =
            FVector4f(static_cast<float>(thresholds[0]), static_cast<float>(thresholds[1]),
                      static_cast<float>(thresholds[2]), static_cast<float>(thresholds[3]));
    }

    return settings;
}

uint32 MipCount(const FImage& top, bool volume) {
    int64 longest = top.SizeX > top.SizeY ? top.SizeX : top.SizeY;
    if (volume && top.NumSlices > longest) {
        longest = top.NumSlices;
    }

    uint32 count = 1;
    while (longest > 1) {
        longest >>= 1;
        ++count;
    }

    return count;
}

// UE BuildTextureMips(TCM 4126–4722)의 mip 부분이다(손으로 옮긴 driver): 최대 크기 단계 경로(4330–
// 4433 — GenerateMipChain(…, 1) 반복), mip 0 복사, 나머지 사슬(4642).
std::vector<FImage> BuildChain(const pg::MipCase& one, const FTextureBuildSettings& settings) {
    FImage base(one.sizeX, one.sizeY, one.slices, ERawImageFormat::RGBA32F);
    pg::Generate(one, &base.AsRGBA32F()->R);
    FImage top = base;
    if (one.maxSize != 0) {
        TArray<FImage> steps;
        steps.Empty(20);
        ITextureCompressorModule::GenerateMipChain(settings, base, steps, 1);
        while (steps.Last().SizeX > one.maxSize || steps.Last().SizeY > one.maxSize) {
            FImage& last = steps.Last();
            ITextureCompressorModule::GenerateMipChain(settings, last, steps, 1);
        }

        top = steps.Last();
    }

    const uint32 count = one.mipGen == pg::NoMipmaps ? 1 : MipCount(top, settings.bVolume);
    TArray<FImage> chain;
    chain.Empty(count);
    chain.Emplace_GetRef(top);
    if (count > 1) {
        ITextureCompressorModule::GenerateMipChain(settings, chain[0], chain, MAX_uint32);
    }

    check(chain.Num() == count);
    return chain.Items;
}

std::vector<uint8> MipChainGolden() {
    std::vector<uint8> out = {'R', 'T', 'P', 'M'};
    PutU32(out, MipChainGoldenVersion);
    PutU32(out, pg::MipCaseCount);
    for (const pg::MipCase& one : pg::MipCases) {
        const std::vector<FImage> chain = BuildChain(one, SettingsFor(one));
        const uint32 nameBytes = static_cast<uint32>(std::strlen(one.name));
        PutU32(out, nameBytes);
        out.insert(out.end(), one.name, one.name + nameBytes);
        PutU32(out, static_cast<uint32>(chain.size()));
        uint64 floats = 0;
        for (const FImage& mip : chain) {
            PutU32(out, static_cast<uint32>(mip.SizeX));
            PutU32(out, static_cast<uint32>(mip.SizeY));
            PutU32(out, static_cast<uint32>(mip.NumSlices));
            const uint64 bytes = static_cast<uint64>(mip.GetNumPixels()) * sizeof(FLinearColor);
            PutU64(out, pg::Fnv1a64(mip.RawData.Items.data(), bytes));
            floats += static_cast<uint64>(mip.GetNumPixels()) * 4;
        }

        if (floats <= pg::FullDataFloats) {
            for (const FImage& mip : chain) {
                const uint64 bytes = static_cast<uint64>(mip.GetNumPixels()) * sizeof(FLinearColor);
                out.insert(out.end(), mip.RawData.Items.data(), mip.RawData.Items.data() + bytes);
            }
        }
    }

    return out;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: TextureProcessingProbe <output directory>\n");
        return 2;
    }

    const std::string directory = argv[1];
    if (!WriteFile(directory + "/KernelGolden.bin", KernelGolden()) ||
        !WriteFile(directory + "/MipChainGolden.bin", MipChainGolden())) {
        std::fprintf(stderr, "cannot write golden files under %s\n", directory.c_str());
        return 1;
    }

    return 0;
}
