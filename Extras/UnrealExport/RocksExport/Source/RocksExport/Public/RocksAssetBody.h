#pragma once

#include "CoreMinimal.h"
#include "RocksExportClosure.h"
#include "RocksPropertyDumper.h"

// 종류별 추출기가 만든 에셋 JSON 본문이다(3.5절 settings·data와 그 과정의 참조·진단, 4.5절).
//
// Settings·Data는 FRocksJsonWriter(Pretty, 1)·FinishFragment로 만든 값 조각이다(에셋 JSON 최상위 객체의
// 키 자리 깊이). 비어 있으면 빈 객체 {}를 쓴다. References는 덤프가 모은 참조로, 내보내기가 의존 목록에
// 합친다(11.7절 4번). Diagnostics는 에셋 JSON diagnostics 뒤에 붙는다.
struct FRocksAssetBody
{
	TArray<uint8> Settings;
	TArray<uint8> Data;
	TArray<FRocksPropertyDumper::FReference> References;
	TArray<FRocksExportDiagnostic> Diagnostics;
};
