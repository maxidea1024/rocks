#pragma once

#include <AssetDatabase/AssetImporter.h>

// CSV 단일 커브 임포터다(Docs/Design/CurveAssets 5.1.1·5.1.4절, UE
// UCurveBase::CreateCurveFromCSVString — CurveBase.cpp:131-191). 머리 행 없이 행마다 `시간, 채널0,
// 채널1, …`이다. 출력 종류(스칼라·벡터·색)마다 인스턴스
// 하나다(float_curve_csv·vector_curve_csv·color_curve_csv). 키는 늘 Linear다. 기본 확장자는 없다.
namespace rocks {

inline constexpr StringView FloatCurveCsvImporterName = "float_curve_csv";
inline constexpr StringView VectorCurveCsvImporterName = "vector_curve_csv";
inline constexpr StringView ColorCurveCsvImporterName = "color_curve_csv";
inline constexpr uint32 CurveCsvImporterVersion = 1;

// 출력 커브 에셋 종류다.
enum class CurveCsvOutput : uint8 {
    Float,
    Vector,
    Color
};

class ROCKS_CURVEASSETTOOLS_API CurveCsvImporter final : public AssetImporter {
public:
    explicit CurveCsvImporter(CurveCsvOutput output) noexcept;

    const AssetImporterInfo& Info() const noexcept override {
        return info_;
    }

    // 모든 채널의 키를 비우고 다시 넣는다(기본값·외삽·색 조정·eventCurve는 그대로 — 설계 5.1.1절).
    // 오류(진단): 잘못된 UTF-8·UTF-16 CorruptData(InvalidSource), 행이 하나도 없음·숫자 아닌
    // 칸·비유한 InvalidArgument(InvalidSource, detail = 물리 줄 번호), 할당 실패. 칸이 2개 미만인
    // 행은 건너뛰고, 칸이 채널 수 + 1과 다른 행은 있는 채널만 쓰고, 둘 다
    // 경고(CurveImportRowSkipped)를 남긴다.
    Result<void> Import(AssetImportContext& context) noexcept override;

private:
    CurveCsvOutput output_;
    AssetImporterInfo info_;
};

} // namespace rocks
