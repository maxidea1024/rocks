// 텍스처 보정 golden probe다(Docs/Design/AssetSystem 12.18 TP-D4).
//
// make_golden.py가 UE 5.8 클론의 TextureCompressorModule.cpp에서 함수 구간을 잘라 TcmExtract.inc로
// 묶고, ProbeShim.h와 함께 이 파일을 컴파일한다. UE 코드는 저장소에 복사하지 않는다.
//
// 출력(little endian):
//   KernelGolden.bin — "RTPK", 판 1, 항목 수, 항목마다 { 크기 u32, sharpen f32, 1D 가중치 f32 ×
//   크기, 2D 가중치 f32 × 크기² } (UE FImageKernel2D::BuildSeparatableGaussWithSharpen 결과).
//
// 사용: TextureProcessingProbe <출력 디렉터리>

#include "ProbeShim.h"

#include "TcmExtract.inc"

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

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: TextureProcessingProbe <output directory>\n");
        return 2;
    }

    const std::string directory = argv[1];
    if (!WriteFile(directory + "/KernelGolden.bin", KernelGolden())) {
        std::fprintf(stderr, "cannot write %s/KernelGolden.bin\n", directory.c_str());
        return 1;
    }

    return 0;
}
