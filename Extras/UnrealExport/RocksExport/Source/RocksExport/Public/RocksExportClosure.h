#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"

class IAssetRegistry;

// 에셋 JSON dependencies 항목 하나다(3.5절).
struct FRocksExportDependency
{
	FString ObjectPath;
	FString ClassName;
	bool bHard = false;
};

// 에셋 JSON diagnostics 항목 하나다(3.5절). Severity는 RocksExport::Severity* 중 하나다.
struct FRocksExportDiagnostic
{
	FString Code;
	FString Severity;
	FString Message;
};

// 묶음에 들어갈 에셋 하나다. 범위 밖 클래스(bInScope 거짓)는 사용자가 직접 고른 것만 들어간다(11.7절).
struct FRocksExportAsset
{
	FString ObjectPath;
	FName PackageName;
	FString ClassName;
	FString UeClass;
	bool bInScope = false;
	TArray<FRocksExportDependency> Dependencies;
	TArray<FRocksExportDiagnostic> Diagnostics;
};

// bundle.json redirects 항목 하나다(3.4절). To는 언제나 사슬 끝이다(사슬 없음 — 10.4절 10번).
struct FRocksExportRedirect
{
	FString From;
	FString To;
};

// 교환 클래스 대응이다(2절, 3.5절 class). Class와 상위 클래스 중 처음 만나는 범위 안 클래스 이름(UE 이름에서
// U를 뺀 것)을 돌려주고 bOutInScope를 켠다. 범위 밖(IES `TextureLightProfile`처럼 범위 안 클래스를
// 상속하지만 2절이 빼는 클래스 포함)이면 Class 자신의 이름이다.
ROCKSEXPORT_API FString RocksExchangeClassName(const UClass* Class, bool& bOutInScope);

// AssetRegistry 항목의 교환 클래스 이름이다(클래스를 찾지 못하면 클래스 경로의 이름, 범위 밖).
ROCKSEXPORT_API FString RocksExchangeClassNameOfAsset(const FAssetData& Asset, bool& bOutInScope);

// Rocks 리더의 objectPath 문법(10.4절 7번)을 만족하는가다: 앞 '/', 패키지 구성 요소 2개 이상, 빈 구성 요소
// 없음, '.'은 객체 이름 앞 하나, ':'(하위 객체)·역슬래시·제어 문자 없음, UTF-8 1024 byte 이하.
ROCKSEXPORT_API bool RocksIsValidExchangeObjectPath(const FString& ObjectPath);

// AssetRegistry 의존 닫힘과 redirector 풀기다(9.3절, 11.6절 FRocksExportClosure).
//
// 1. AddRoot로 선택을 더한다: 객체 경로(/Game/A.A), 패키지 경로(/Game/A — 그 패키지의 에셋 전부), 폴더
//    재귀(/Game/Dir/...). redirector는 끝까지 풀어 대상을 고르고 redirects에 적는다.
// 2. Collect가 범위 안 에셋에서 시작해 패키지 Hard 의존을 따라 범위 안 클래스의 닫힘을 모은다
//    (bClosure일 때). Soft 의존·범위 밖 대상은 의존으로 적기만 한다.
// 3. 에셋이 둘 이상인 패키지의 에셋은 내보내지 않고 GetSkipped에 남긴다(형식 v1이 담지 못함 — 11.7절). 그것을
//    Hard로 참조한 에셋에는 MultiAssetPackage 경고 진단을 단다.
// 4. 결과 에셋은 objectPath, redirect는 from의 UTF-8 byte 순이다. 에셋마다 의존은 objectPath 순이고
//    Hard·Soft 둘 다면 Hard 하나다(10.4절 6번).
//
// AssetRegistry 검색은 호출자가 끝내 둔다(commandlet은 SearchAllAssets(true)). 게임 스레드에서 쓴다.
class ROCKSEXPORT_API FRocksExportClosure
{
public:
	FRocksExportClosure(IAssetRegistry& InRegistry, bool bInClosure);

	// 선택 하나를 더한다. 찾지 못하거나 경로 형식이 틀리면 false와 OutError다.
	bool AddRoot(const FString& Selection, FString& OutError);

	// 선택한 FAssetData를 그대로 더한다(에디터 메뉴).
	bool AddRootAsset(const FAssetData& Asset, FString& OutError);

	// 닫힘·의존을 계산한다. 실패(경로 문법 위반, 한 패키지의 여러 에셋 등)면 false와 OutError다.
	bool Collect(FString& OutError);

	const TArray<FRocksExportAsset>& GetAssets() const
	{
		return Assets;
	}

	const TArray<FRocksExportRedirect>& GetRedirects() const
	{
		return Redirects;
	}

	// 에셋이 둘 이상인 패키지에 있어 내보내지 않은 에셋이다(11.7절, objectPath의 UTF-8 byte 순).
	const TArray<FString>& GetSkipped() const
	{
		return Skipped;
	}

private:
	enum class EResolve : uint8
	{
		Resolved,
		Missing,
		Broken,
	};

	EResolve ResolveRedirects(const FAssetData& Start, FAssetData& OutTarget, FString& OutDetail);
	bool IsMultiAssetPackage(FName PackageName);

	// 에셋을 묶음에 더하고 번호를 돌려준다. 담을 수 없는 패키지(에셋 여럿)면 INDEX_NONE이다.
	int32 AddEntry(const FAssetData& Asset);
	void CollectDependencies(int32 Index);

	IAssetRegistry& Registry;
	bool bClosure;

	// objectPath → Entries 번호다.
	TMap<FString, int32> EntryIndex;
	TArray<FRocksExportAsset> Entries;
	TArray<int32> Pending;
	TMap<FString, FString> RedirectMap;
	TMap<FName, bool> MultiAssetPackages;
	TArray<FString> Skipped;

	TArray<FRocksExportAsset> Assets;
	TArray<FRocksExportRedirect> Redirects;
};
