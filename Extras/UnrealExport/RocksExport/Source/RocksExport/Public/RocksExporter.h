#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"

struct FRocksAssetBody;
struct FRocksExportAsset;
struct FRocksExportRedirect;

// 내보내기 입력이다. Selections(경로 문자열)와 SelectedAssets(에디터 선택)를 함께 줄 수 있다.
struct FRocksExportOptions
{
	// 객체 경로·패키지 경로·폴더 재귀(/Game/Dir/...)다(FRocksExportClosure::AddRoot).
	TArray<FString> Selections;
	TArray<FAssetData> SelectedAssets;
	FString OutputDirectory;
	bool bClosure = true;
	// 머티리얼 굽기 텍스처 한 변이다(8.9절 1번). 0이면 굽지 않는다. 굽기는 렌더링이 되는 프로세스에서만 한다.
	int32 BakeSize = 0;
};

// 내보내기 결과 요약이다.
struct FRocksExportResult
{
	int32 AssetCount = 0;
	int32 UnsupportedCount = 0;
	int32 SkippedCount = 0;
	int32 RedirectCount = 0;
	int32 DiagnosticCount = 0;
	FString OutputDirectory;
	FString Error;
};

// 묶음 내보내기다(11.6절 — 닫힘 FRocksExportClosure, 쓰기 FRocksBundleWriter). commandlet과 에디터 메뉴가 같은
// 경로를 쓴다.
//
// 에셋 JSON 공통부(objectPath·class·ueClass·dependencies·diagnostics)를 쓰고, 추출기가 있는 종류(UX2a 텍스처 —
// 4.5절)는 에셋을 로드해 settings·data를 채운다. 나머지 종류의 본문은 빈 객체다(그 단계가 채운다, 14절).
//
// AssetRegistry 검색은 호출자가 끝내 둔다. 게임 스레드에서 부른다.
class ROCKSEXPORT_API FRocksExporter
{
public:
	// 내보낸다. 실패하면 false이고 OutResult.Error에 이유가 있으며 출력 디렉터리는 바뀌지 않는다.
	static bool Export(const FRocksExportOptions& Options, FRocksExportResult& OutResult);

	// bundle.json bytes다(3.4절). CreatedUtc가 비면 createdUtc를 쓰지 않는다. bBaked면 environment(굽기 RHI·GPU·드라이버)를 쓴다.
	static TArray<uint8> MakeBundleJson(const TArray<FRocksExportAsset>& Assets, const TArray<FRocksExportRedirect>& Redirects, const FString& CreatedUtc,
		bool bBaked = false);

	// 에셋 JSON bytes다(3.5절). Body의 settings·data 조각이 비면 빈 객체다.
	static TArray<uint8> MakeAssetJson(const FRocksExportAsset& Asset, const FRocksAssetBody& Body);
};
