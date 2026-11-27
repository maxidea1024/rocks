#pragma once

#include <AssetDatabase/AssetImporter.h>

// CSV 커브 표 임포터다(Docs/Design/CurveAssets 5.1.1·5.1.2절, UE
// UCurveTable::CreateTableFromCSVString — CurveTable.cpp:656-760). 첫 행은 시간 축(0열은 무시),
// 다음 행부터 0열은 행 이름, 1열부터 시간 순서의 값이다. 옵션 interp(CurveTableImportOptions)로
// 모드를 정한다. 기본 확장자는 없다(`--importer=` — 설계 CAD6).
namespace rocks {

// 임포터 이름(`--importer=` 값·기록 importerName)과 버전이다. 결과가 바뀌면 버전을 올린다.
inline constexpr StringView CurveTableCsvImporterName = "curve_table_csv";
inline constexpr uint32 CurveTableCsvImporterVersion = 1;

class ROCKS_CURVEASSETTOOLS_API CurveTableCsvImporter final : public AssetImporter {
public:
    CurveTableCsvImporter() noexcept;

    const AssetImporterInfo& Info() const noexcept override {
        return info_;
    }

    // 오류(진단): 잘못된 UTF-8·UTF-16 CorruptData(InvalidSource), 형식 오류 — 행 부족·시간 칸 숫자
    // 아님·비유한·시간 중복·값 칸 숫자 아님 InvalidArgument(InvalidSource, detail = 물리 줄 번호),
    // 범위 밖 옵션 InvalidArgument(InvalidImportOption), 할당 실패. 이름 없음·이름 중복·열 수
    // 불일치 행은 건너뛰고 경고(CurveImportRowSkipped, detail = 물리 줄 번호)를 남긴다.
    Result<void> Import(AssetImportContext& context) noexcept override;

private:
    AssetImporterInfo info_;
};

} // namespace rocks
