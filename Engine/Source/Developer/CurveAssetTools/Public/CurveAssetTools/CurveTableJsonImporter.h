#pragma once

#include <AssetDatabase/AssetImporter.h>

// JSON 커브 표 임포터다(Docs/Design/CurveAssets 5.1.1·5.1.3절, UE
// UCurveTable::CreateTableFromJSONString — CurveTable.cpp:847-1003). 최상위 배열의 원소마다
// {"Name": "행", "<시간>": 값, …}이다. 옵션은 CSV 표와 같다. 기본 확장자는 없다(`--importer=` —
// 설계 CAD6).
namespace rocks {

inline constexpr StringView CurveTableJsonImporterName = "curve_table_json";
inline constexpr uint32 CurveTableJsonImporterVersion = 1;

class ROCKS_CURVEASSETTOOLS_API CurveTableJsonImporter final : public AssetImporter {
public:
    CurveTableJsonImporter() noexcept;

    const AssetImporterInfo& Info() const noexcept override {
        return info_;
    }

    // 오류(진단): 잘못된 UTF-8·UTF-16 CorruptData(InvalidSource), 파싱 실패·최상위가 배열이 아님·빈
    // 배열·중복 키 InvalidArgument(InvalidSource), 범위 밖 옵션
    // InvalidArgument(InvalidImportOption), 할당 실패. 원소 오류(객체 아님·Name 없음·이름 중복)는
    // 그 원소를, 항목 오류(시간 키가 float가 아님·값이 숫자가 아님·비유한)는 그 항목을, 같은 시간이
    // 둘인 행은 그 행을 건너뛰고 경고(CurveImportRowSkipped, detail = 배열 원소 번호 1부터)를
    // 남긴다.
    Result<void> Import(AssetImportContext& context) noexcept override;

private:
    AssetImporterInfo info_;
};

} // namespace rocks
