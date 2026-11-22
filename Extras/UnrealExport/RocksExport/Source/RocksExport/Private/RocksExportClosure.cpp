#include "RocksExportClosure.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "RocksExportCommon.h"
#include "RocksJsonWriter.h"
#include "UObject/ObjectRedirector.h"

namespace RocksExportClosurePrivate
{
	// 2절 범위 안 UE 클래스 이름(U 접두 없음)이다. 이 이름이 곧 교환 클래스 이름이다.
	static const TCHAR* const InScopeClasses[] = {
		TEXT("Texture2D"),
		TEXT("TextureCube"),
		TEXT("Texture2DArray"),
		TEXT("TextureCubeArray"),
		TEXT("VolumeTexture"),
		TEXT("StaticMesh"),
		TEXT("Skeleton"),
		TEXT("SkeletalMesh"),
		TEXT("AnimSequence"),
		TEXT("Material"),
		TEXT("MaterialInstanceConstant"),
		TEXT("MaterialFunction"),
		TEXT("MaterialFunctionInstance"),
		TEXT("MaterialParameterCollection"),
	};

	// 범위 안 클래스를 상속하지만 2절이 범위 밖으로 둔 클래스다(IES 프로파일은 UTexture2D 파생).
	static const TCHAR* const ExcludedClasses[] = {
		TEXT("TextureLightProfile"),
	};

	// AssetRegistry 의존 중 에셋이 아닌 패키지(네이티브 스크립트·메모리 패키지) 접두다.
	static const TCHAR* const NonAssetPackagePrefixes[] = {
		TEXT("/Script/"),
		TEXT("/Memory/"),
	};

	// redirector FAssetData의 대상 태그 이름이다(AssetRegistry.cpp DestinationObjectFName과 같다).
	static const FName DestinationObjectTag(TEXT("DestinationObject"));

	template <int32 Count>
	static bool Contains(const TCHAR* const (&Names)[Count], const FString& Name)
	{
		for (const TCHAR* Candidate : Names)
		{
			if (Name.Equals(Candidate, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}

		return false;
	}

	static bool IsNonAssetPackage(const FString& PackageName)
	{
		for (const TCHAR* Prefix : NonAssetPackagePrefixes)
		{
			if (PackageName.StartsWith(Prefix, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}

		return false;
	}

	// Rocks 리더의 objectPath 문법(10.4절 7번)과 같은 검사다: 앞 '/', 패키지 구성 요소 2개 이상, 빈 구성
	// 요소 없음, '.'은 객체 이름 앞 하나, ':'·'\'·제어 문자 없음, UTF-8 1024 byte 이하.
	static bool IsValidObjectPath(const FString& Path)
	{
		const TArray<uint8> Bytes = FRocksJsonWriter::ToUtf8(Path);
		if (Bytes.IsEmpty() || Bytes.Num() > RocksExport::ObjectPathMaxBytes || Bytes[0] != '/')
		{
			return false;
		}

		static constexpr uint8 AsciiControlLast = 0x1f;
		static constexpr uint8 AsciiDelete = 0x7f;
		int32 Dot = INDEX_NONE;
		int32 Components = 0;
		int32 ComponentBytes = 0;
		for (int32 Index = 0; Index < Bytes.Num(); ++Index)
		{
			const uint8 Byte = Bytes[Index];
			if (Byte <= AsciiControlLast || Byte == AsciiDelete || Byte == ':' || Byte == '\\')
			{
				return false;
			}

			if (Byte == '.')
			{
				if (Dot != INDEX_NONE || ComponentBytes == 0)
				{
					return false;
				}

				Dot = Index;
				continue;
			}

			if (Byte == '/')
			{
				if (Dot != INDEX_NONE || (Index != 0 && ComponentBytes == 0))
				{
					return false;
				}

				++Components;
				ComponentBytes = 0;
				continue;
			}

			if (Dot == INDEX_NONE)
			{
				++ComponentBytes;
			}
		}

		return Dot != INDEX_NONE && Dot + 1 < Bytes.Num() && Components >= 2;
	}

	static bool IsUtf8Less(const FString& Left, const FString& Right)
	{
		return FRocksJsonWriter::CompareUtf8(Left, Right) < 0;
	}

	static FString ClassNameOf(const FAssetData& Asset, bool& bOutInScope)
	{
		if (const UClass* Class = Asset.GetClass())
		{
			return RocksExchangeClassName(Class, bOutInScope);
		}

		// 클래스를 찾지 못하면(로드되지 않은 Blueprint 클래스 등) 경로의 이름을 쓴다. 범위 안 클래스는 모두
		// 네이티브라 늘 찾으므로 여기는 범위 밖이다.
		bOutInScope = false;
		return Asset.AssetClassPath.GetAssetName().ToString();
	}
}

FString RocksExchangeClassNameOfAsset(const FAssetData& Asset, bool& bOutInScope)
{
	return RocksExportClosurePrivate::ClassNameOf(Asset, bOutInScope);
}

bool RocksIsValidExchangeObjectPath(const FString& ObjectPath)
{
	return RocksExportClosurePrivate::IsValidObjectPath(ObjectPath);
}

FString RocksExchangeClassName(const UClass* Class, bool& bOutInScope)
{
	using namespace RocksExportClosurePrivate;

	bOutInScope = false;
	for (const UClass* Current = Class; Current != nullptr; Current = Current->GetSuperClass())
	{
		const FString Name = Current->GetName();
		if (Contains(ExcludedClasses, Name))
		{
			break;
		}

		if (Contains(InScopeClasses, Name))
		{
			bOutInScope = true;
			return Name;
		}
	}

	return Class != nullptr ? Class->GetName() : FString();
}

FRocksExportClosure::FRocksExportClosure(IAssetRegistry& InRegistry, bool bInClosure)
	: Registry(InRegistry)
	, bClosure(bInClosure)
{
}

FRocksExportClosure::EResolve FRocksExportClosure::ResolveRedirects(const FAssetData& Start, FAssetData& OutTarget, FString& OutDetail)
{
	TArray<FString> Chain;
	FAssetData Current = Start;
	while (Current.IsRedirector())
	{
		const FString CurrentPath = Current.GetObjectPathString();
		if (Chain.Contains(CurrentPath) || Chain.Num() >= RocksExport::RedirectMaxChain)
		{
			OutDetail = FString::Printf(TEXT("redirector chain from %s loops or exceeds %d steps"), *Start.GetObjectPathString(), RocksExport::RedirectMaxChain);
			return EResolve::Broken;
		}

		Chain.Add(CurrentPath);

		// 대상은 AssetRegistry 태그로 읽는다. 태그가 없으면 redirector를 로드해 대상 객체를 본다.
		FString Destination;
		if (!Current.GetTagValue(RocksExportClosurePrivate::DestinationObjectTag, Destination) || Destination.IsEmpty())
		{
			const UObjectRedirector* Redirector = LoadObject<UObjectRedirector>(nullptr, *CurrentPath);
			if (Redirector == nullptr || Redirector->DestinationObject == nullptr)
			{
				OutDetail = FString::Printf(TEXT("redirector %s has no destination"), *CurrentPath);
				return EResolve::Broken;
			}

			Destination = Redirector->DestinationObject->GetPathName();
		}

		const FSoftObjectPath Next(Destination);
		Current = Registry.GetAssetByObjectPath(Next);
		if (!Current.IsValid())
		{
			OutDetail = FString::Printf(TEXT("redirector %s points to missing asset %s"), *CurrentPath, *Next.ToString());
			return EResolve::Missing;
		}
	}

	const FString Target = Current.GetObjectPathString();
	for (const FString& From : Chain)
	{
		RedirectMap.Add(From, Target);
	}

	OutTarget = Current;
	return EResolve::Resolved;
}

bool FRocksExportClosure::IsMultiAssetPackage(FName PackageName)
{
	if (const bool* Known = MultiAssetPackages.Find(PackageName))
	{
		return *Known;
	}

	TArray<FAssetData> PackageAssets;
	Registry.GetAssetsByPackageName(PackageName, PackageAssets);
	const int32 Count = PackageAssets.FilterByPredicate([](const FAssetData& Asset) { return !Asset.IsRedirector(); }).Num();
	return MultiAssetPackages.Add(PackageName, Count > 1);
}

int32 FRocksExportClosure::AddEntry(const FAssetData& Asset)
{
	const FString ObjectPath = Asset.GetObjectPathString();
	if (const int32* Found = EntryIndex.Find(ObjectPath))
	{
		return *Found;
	}

	// 형식 v1은 파일 이름과 Rocks 경로를 패키지 경로에서 만든다(9.1절, 10.4절 8번). 그래서 에셋이 둘 이상인
	// 패키지는 담을 수 없어 내보내지 않는다(11.7절).
	if (IsMultiAssetPackage(Asset.PackageName))
	{
		Skipped.AddUnique(ObjectPath);
		return INDEX_NONE;
	}

	FRocksExportAsset& Entry = Entries.AddDefaulted_GetRef();
	Entry.ObjectPath = ObjectPath;
	Entry.PackageName = Asset.PackageName;
	Entry.ClassName = RocksExportClosurePrivate::ClassNameOf(Asset, Entry.bInScope);
	Entry.UeClass = Asset.AssetClassPath.ToString();
	if (!Entry.bInScope)
	{
		Entry.Diagnostics.Add({TEXT("UnsupportedAsset"), RocksExport::SeverityWarning,
			FString::Printf(TEXT("class %s is outside the exchange scope; only the common part is written"), *Entry.ClassName)});
	}

	const int32 Index = Entries.Num() - 1;
	EntryIndex.Add(ObjectPath, Index);
	Pending.Add(Index);
	return Index;
}

bool FRocksExportClosure::AddRootAsset(const FAssetData& Asset, FString& OutError)
{
	FAssetData Target = Asset;
	if (Asset.IsRedirector())
	{
		FString Detail;
		if (ResolveRedirects(Asset, Target, Detail) != EResolve::Resolved)
		{
			OutError = FString::Printf(TEXT("cannot resolve selected redirector: %s"), *Detail);
			return false;
		}
	}

	AddEntry(Target);
	return true;
}

bool FRocksExportClosure::AddRoot(const FString& Selection, FString& OutError)
{
	static const FString RecursiveSuffix(TEXT("/..."));
	const FString Trimmed = Selection.TrimStartAndEnd();
	if (!Trimmed.StartsWith(TEXT("/")))
	{
		OutError = FString::Printf(TEXT("asset selection '%s' must start with '/'"), *Selection);
		return false;
	}

	TArray<FAssetData> Found;
	if (Trimmed.EndsWith(RecursiveSuffix))
	{
		const FString Folder = Trimmed.LeftChop(RecursiveSuffix.Len());
		if (Folder.IsEmpty())
		{
			OutError = FString::Printf(TEXT("asset selection '%s' has no folder"), *Selection);
			return false;
		}

		Registry.GetAssetsByPath(FName(*Folder), Found, true);
	}
	else if (Trimmed.Contains(TEXT(".")))
	{
		const FAssetData Asset = Registry.GetAssetByObjectPath(FSoftObjectPath(Trimmed));
		if (Asset.IsValid())
		{
			Found.Add(Asset);
		}
	}
	else
	{
		Registry.GetAssetsByPackageName(FName(*Trimmed), Found);
	}

	if (Found.IsEmpty())
	{
		OutError = FString::Printf(TEXT("asset selection '%s' matched no asset (folders need the '/...' suffix)"), *Selection);
		return false;
	}

	for (const FAssetData& Asset : Found)
	{
		if (!AddRootAsset(Asset, OutError))
		{
			return false;
		}
	}

	return true;
}

void FRocksExportClosure::CollectDependencies(int32 Index)
{
	using namespace RocksExportClosurePrivate;

	const FName PackageName = Entries[Index].PackageName;
	const bool bTraverse = bClosure && Entries[Index].bInScope;

	TArray<FName> HardPackages;
	TArray<FName> SoftPackages;
	Registry.GetDependencies(PackageName, HardPackages, UE::AssetRegistry::EDependencyCategory::Package, UE::AssetRegistry::EDependencyQuery::Hard);
	Registry.GetDependencies(PackageName, SoftPackages, UE::AssetRegistry::EDependencyCategory::Package, UE::AssetRegistry::EDependencyQuery::Soft);

	// 패키지 이름 → Hard 여부다. 둘 다면 Hard다. 처리 순서를 정해 진단 순서를 결정적으로 한다.
	TMap<FString, bool> Packages;
	for (const FName Package : SoftPackages)
	{
		Packages.Add(Package.ToString(), false);
	}

	for (const FName Package : HardPackages)
	{
		Packages.Add(Package.ToString(), true);
	}

	Packages.KeySort([](const FString& Left, const FString& Right) { return IsUtf8Less(Left, Right); });

	TMap<FString, FRocksExportDependency> Dependencies;
	TArray<FRocksExportDiagnostic> Diagnostics;
	const FString OwnPath = Entries[Index].ObjectPath;
	for (const TPair<FString, bool>& Package : Packages)
	{
		if (IsNonAssetPackage(Package.Key) || Package.Key == PackageName.ToString())
		{
			continue;
		}

		TArray<FAssetData> PackageAssets;
		Registry.GetAssetsByPackageName(FName(*Package.Key), PackageAssets);
		if (PackageAssets.IsEmpty())
		{
			Diagnostics.Add({TEXT("MissingUnrealPackage"), RocksExport::SeverityWarning,
				FString::Printf(TEXT("%s dependency package %s is not in the asset registry"), Package.Value ? TEXT("hard") : TEXT("soft"), *Package.Key)});
			continue;
		}

		PackageAssets.Sort([](const FAssetData& Left, const FAssetData& Right)
			{
				return IsUtf8Less(Left.GetObjectPathString(), Right.GetObjectPathString());
			});

		for (const FAssetData& PackageAsset : PackageAssets)
		{
			FAssetData Target;
			FString Detail;
			if (ResolveRedirects(PackageAsset, Target, Detail) != EResolve::Resolved)
			{
				Diagnostics.Add({TEXT("BrokenRedirector"), RocksExport::SeverityWarning, Detail});
				continue;
			}

			const FString TargetPath = Target.GetObjectPathString();
			if (TargetPath == OwnPath)
			{
				continue;
			}

			if (!IsValidObjectPath(TargetPath))
			{
				Diagnostics.Add({TEXT("InvalidObjectPath"), RocksExport::SeverityWarning,
					FString::Printf(TEXT("dependency %s does not follow the exchange object path syntax and is not recorded"), *TargetPath)});
				continue;
			}

			bool bTargetInScope = false;
			FRocksExportDependency& Dependency = Dependencies.FindOrAdd(TargetPath);
			Dependency.ObjectPath = TargetPath;
			Dependency.ClassName = ClassNameOf(Target, bTargetInScope);
			Dependency.bHard = Dependency.bHard || Package.Value;
			if (bTraverse && Package.Value && bTargetInScope && AddEntry(Target) == INDEX_NONE)
			{
				Diagnostics.Add({TEXT("MultiAssetPackage"), RocksExport::SeverityWarning,
					FString::Printf(TEXT("hard dependency %s lives in a package with several assets and is not exported"), *TargetPath)});
			}
		}
	}

	// AddEntry가 Entries를 늘렸을 수 있으므로 참조를 여기서 다시 잡는다.
	FRocksExportAsset& Entry = Entries[Index];
	Dependencies.GenerateValueArray(Entry.Dependencies);
	Entry.Dependencies.Sort([](const FRocksExportDependency& Left, const FRocksExportDependency& Right)
		{
			return IsUtf8Less(Left.ObjectPath, Right.ObjectPath);
		});
	Entry.Diagnostics.Append(MoveTemp(Diagnostics));
}

bool FRocksExportClosure::Collect(FString& OutError)
{
	using namespace RocksExportClosurePrivate;

	while (!Pending.IsEmpty())
	{
		CollectDependencies(Pending.Pop(EAllowShrinking::No));
	}

	Skipped.Sort([](const FString& Left, const FString& Right) { return IsUtf8Less(Left, Right); });
	if (Entries.IsEmpty())
	{
		OutError = TEXT("nothing to export: every selected asset was skipped");
		return false;
	}

	// 파일 이름은 패키지 경로에서 나오므로(10.4절 8번) 한 패키지의 에셋은 하나만 담을 수 있다.
	TMap<FName, FString> PackageOwners;
	for (const FRocksExportAsset& Entry : Entries)
	{
		if (!IsValidObjectPath(Entry.ObjectPath))
		{
			OutError = FString::Printf(TEXT("object path %s does not follow the exchange object path syntax"), *Entry.ObjectPath);
			return false;
		}

		if (const FString* Owner = PackageOwners.Find(Entry.PackageName))
		{
			OutError = FString::Printf(TEXT("package %s holds two exported assets (%s, %s); one asset per package is supported"),
				*Entry.PackageName.ToString(), **Owner, *Entry.ObjectPath);
			return false;
		}

		PackageOwners.Add(Entry.PackageName, Entry.ObjectPath);
	}

	for (const TPair<FString, FString>& Redirect : RedirectMap)
	{
		if (!IsValidObjectPath(Redirect.Key) || !IsValidObjectPath(Redirect.Value) || EntryIndex.Contains(Redirect.Key))
		{
			OutError = FString::Printf(TEXT("redirector %s cannot be recorded (invalid path or same as an exported asset)"), *Redirect.Key);
			return false;
		}

		Redirects.Add({Redirect.Key, Redirect.Value});
	}

	Assets = Entries;
	Assets.Sort([](const FRocksExportAsset& Left, const FRocksExportAsset& Right) { return IsUtf8Less(Left.ObjectPath, Right.ObjectPath); });
	Redirects.Sort([](const FRocksExportRedirect& Left, const FRocksExportRedirect& Right) { return IsUtf8Less(Left.From, Right.From); });
	return true;
}
