#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "RocksExportCommandlet.generated.h"

// commandlet 진입점 -run=RocksExport다(UnrealExchange 11.2절).
//
//   UnrealEditor-Cmd.exe <프로젝트>.uproject -run=RocksExport -Assets=<선택>[,<선택>...] -Out=<묶음 디렉터리>
//                        [-Closure | -NoClosure] [-BakeMaterials [-BakeSize=<n>] -AllowCommandletRendering]
//                        [-unattended -nullrhi -nopause]
//   UnrealEditor-Cmd.exe <프로젝트>.uproject -run=RocksExport -SelfTest
//
// - 선택: 객체 경로(/Game/A.A), 패키지 경로(/Game/A), 폴더 재귀(/Game/Dir/...). 쉼표·세미콜론으로 나눈다.
// - -Closure(기본)는 범위 안 Hard 의존 닫힘을 함께 내보낸다. -NoClosure는 선택한 것만 내보낸다.
// - -BakeMaterials(값 없는 switch)는 Surface 영역 머티리얼·인스턴스를 구워 data.baked에 더한다(8.9절). -BakeSize는
//   한 변(기본 1024, 1–8192)이다. 렌더링이 필요하다: -AllowCommandletRendering을 주고 -nullrhi를 빼야 한다.
// - -SelfTest는 SHA-256·숫자 서식·JSON·덤프·writer·결정성 자기 시험(12.2절)을 돌린다.
// 시작 때 AssetRegistry 검색을 끝낸다(SearchAllAssets(true)). 성공하면 0, 실패하면 1이다.
UCLASS()
class URocksExportCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	URocksExportCommandlet();

	virtual int32 Main(const FString& Params) override;
};
