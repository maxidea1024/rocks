#include "RocksExporter.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "HAL/IConsoleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Engine/Texture.h"
#include "Misc/EngineVersion.h"
#include "RocksAssetBody.h"
#include "RocksBundleWriter.h"
#include "RocksExportClosure.h"
#include "RocksExportCommon.h"
#include "RocksJsonWriter.h"
#include "RocksTextureExtractor.h"

namespace RocksExporterPrivate
{
	// 플러그인 서술자를 찾지 못했을 때의 버전 문자열이다(정상 설치에서는 쓰이지 않는다).
	static const TCHAR* const UnknownValue = TEXT("unknown");

	static FString PluginVersion()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(RocksExport::PluginName);
		return Plugin.IsValid() ? Plugin->GetDescriptor().VersionName : FString(UnknownValue);
	}

	// 설치판(Launcher) 엔진에는 git commit이 없어 Perforce changelist 번호를 쓴다(11.7절). 소스 빌드처럼
	// changelist가 0이면 unknown이다.
	static FString EngineCommit()
	{
		const uint32 Changelist = FEngineVersion::Current().GetChangelist();
		return Changelist != 0 ? FString::Printf(TEXT("%u"), Changelist) : FString(UnknownValue);
	}

	static bool SubstrateEnabled()
	{
		const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Substrate"));
		return Variable != nullptr && Variable->GetInt() != 0;
	}

	// [TextureImporter] RetainJpegFormat이다(4.3절, UE 기본 True — BaseEditor.ini).
	static bool RetainJpegFormat()
	{
		bool bRetain = true;
		GConfig->GetBool(TEXT("TextureImporter"), TEXT("RetainJpegFormat"), bRetain, GEditorIni);
		return bRetain;
	}

	// 덤프가 모은 참조를 의존 목록에 합친다(11.7절 4번 — 합집합, 둘 다면 Hard). AssetRegistry가 모르는 객체(에셋이
	// 아닌 것)와 objectPath 문법을 어기는 경로는 넣지 않는다.
	static void MergeReferences(FRocksExportAsset& Asset, const TArray<FRocksPropertyDumper::FReference>& References)
	{
		IAssetRegistry& Registry = IAssetRegistry::GetChecked();
		bool bAdded = false;
		for (const FRocksPropertyDumper::FReference& Reference : References)
		{
			if (Reference.Path == Asset.ObjectPath)
			{
				continue;
			}

			FRocksExportDependency* Existing = Asset.Dependencies.FindByPredicate([&Reference](const FRocksExportDependency& Dependency)
				{
					return Dependency.ObjectPath == Reference.Path;
				});
			if (Existing != nullptr)
			{
				Existing->bHard = Existing->bHard || Reference.bHard;
				continue;
			}

			const FAssetData Target = Registry.GetAssetByObjectPath(FSoftObjectPath(Reference.Path));
			if (!Target.IsValid() || !RocksIsValidExchangeObjectPath(Reference.Path))
			{
				continue;
			}

			bool bInScope = false;
			Asset.Dependencies.Add({Reference.Path, RocksExchangeClassNameOfAsset(Target, bInScope), Reference.bHard});
			bAdded = true;
		}

		if (bAdded)
		{
			Asset.Dependencies.Sort([](const FRocksExportDependency& Left, const FRocksExportDependency& Right)
				{
					return FRocksJsonWriter::CompareUtf8(Left.ObjectPath, Right.ObjectPath) < 0;
				});
		}
	}

	// 범위 안 종류의 본문을 만든다(UX2a 텍스처). 로드 실패는 에셋 수준 error 진단이다. blob 쓰기 실패만 false다.
	static bool ExtractBody(FRocksExportAsset& Asset, FRocksBundleWriter& Bundle, FRocksAssetBody& OutBody, FString& OutError)
	{
		if (!Asset.bInScope || !FRocksTextureExtractor::IsTextureClass(Asset.ClassName))
		{
			return true;
		}

		UTexture* Texture = Cast<UTexture>(FSoftObjectPath(Asset.ObjectPath).TryLoad());
		if (Texture == nullptr)
		{
			Asset.Diagnostics.Add({TEXT("LoadFailed"), RocksExport::SeverityError, TEXT("the asset could not be loaded as a texture")});
			return true;
		}

		if (!FRocksTextureExtractor::Extract(*Texture, Bundle, OutBody, OutError))
		{
			return false;
		}

		MergeReferences(Asset, OutBody.References);
		Asset.Diagnostics.Append(OutBody.Diagnostics);
		return true;
	}

	// ISO 8601 UTC 초 단위 표기(예: 2026-11-22T09:30:00Z)다.
	static FString UtcNow()
	{
		const FDateTime Now = FDateTime::UtcNow();
		return FString::Printf(TEXT("%04d-%02d-%02dT%02d:%02d:%02dZ"), Now.GetYear(), Now.GetMonth(), Now.GetDay(), Now.GetHour(), Now.GetMinute(), Now.GetSecond());
	}
}

TArray<uint8> FRocksExporter::MakeBundleJson(const TArray<FRocksExportAsset>& Assets, const TArray<FRocksExportRedirect>& Redirects, const FString& CreatedUtc)
{
	using namespace RocksExporterPrivate;

	const FEngineVersion& Engine = FEngineVersion::Current();
	FRocksJsonWriter Writer;
	Writer.BeginObject();
	Writer.Key(TEXT("formatVersion"));
	Writer.Int(RocksExport::BundleFormatVersion);

	Writer.Key(TEXT("exporter"));
	Writer.BeginObject();
	Writer.Key(TEXT("plugin"));
	Writer.String(RocksExport::PluginName);
	Writer.Key(TEXT("pluginVersion"));
	Writer.String(PluginVersion());
	Writer.Key(TEXT("engineVersion"));
	Writer.String(FString::Printf(TEXT("%u.%u.%u"), Engine.GetMajor(), Engine.GetMinor(), Engine.GetPatch()));
	Writer.Key(TEXT("engineCommit"));
	Writer.String(EngineCommit());
	Writer.EndObject();

	Writer.Key(TEXT("projectSettings"));
	Writer.BeginObject();
	Writer.Key(TEXT("substrate"));
	Writer.Bool(SubstrateEnabled());
	Writer.Key(TEXT("retainJpegFormat"));
	Writer.Bool(RetainJpegFormat());
	Writer.EndObject();

	// environment는 굽기(UX6)를 했을 때만 쓴다(3.4절).
	if (!CreatedUtc.IsEmpty())
	{
		Writer.Key(TEXT("createdUtc"));
		Writer.String(CreatedUtc);
	}

	Writer.Key(TEXT("assets"));
	Writer.BeginArray();
	for (const FRocksExportAsset& Asset : Assets)
	{
		Writer.BeginObject();
		Writer.Key(TEXT("objectPath"));
		Writer.String(Asset.ObjectPath);
		Writer.Key(TEXT("class"));
		Writer.String(Asset.ClassName);
		Writer.Key(TEXT("file"));
		Writer.String(FRocksBundleWriter::AssetFileForPackage(Asset.PackageName.ToString()));
		Writer.EndObject();
	}

	Writer.EndArray();

	Writer.Key(TEXT("redirects"));
	Writer.BeginArray();
	for (const FRocksExportRedirect& Redirect : Redirects)
	{
		Writer.BeginObject();
		Writer.Key(TEXT("from"));
		Writer.String(Redirect.From);
		Writer.Key(TEXT("to"));
		Writer.String(Redirect.To);
		Writer.EndObject();
	}

	Writer.EndArray();
	Writer.EndObject();
	return Writer.Finish();
}

TArray<uint8> FRocksExporter::MakeAssetJson(const FRocksExportAsset& Asset, const FRocksAssetBody& Body)
{
	FRocksJsonWriter Writer;
	Writer.BeginObject();
	Writer.Key(TEXT("objectPath"));
	Writer.String(Asset.ObjectPath);
	Writer.Key(TEXT("class"));
	Writer.String(Asset.ClassName);
	Writer.Key(TEXT("ueClass"));
	Writer.String(Asset.UeClass);

	Writer.Key(TEXT("dependencies"));
	Writer.BeginArray();
	for (const FRocksExportDependency& Dependency : Asset.Dependencies)
	{
		Writer.BeginObject();
		Writer.Key(TEXT("objectPath"));
		Writer.String(Dependency.ObjectPath);
		Writer.Key(TEXT("class"));
		Writer.String(Dependency.ClassName);
		Writer.Key(TEXT("kind"));
		Writer.String(Dependency.bHard ? TEXT("hard") : TEXT("soft"));
		Writer.EndObject();
	}

	Writer.EndArray();

	Writer.Key(TEXT("diagnostics"));
	Writer.BeginArray();
	for (const FRocksExportDiagnostic& Diagnostic : Asset.Diagnostics)
	{
		Writer.BeginObject();
		Writer.Key(TEXT("code"));
		Writer.String(Diagnostic.Code);
		Writer.Key(TEXT("severity"));
		Writer.String(Diagnostic.Severity);
		Writer.Key(TEXT("message"));
		Writer.String(Diagnostic.Message);
		Writer.EndObject();
	}

	Writer.EndArray();

	// 종류별 본문(4–8절)이다. 추출기가 없는 종류는 빈 객체다.
	const auto WritePart = [&Writer](const TCHAR* Key, const TArray<uint8>& Fragment)
	{
		Writer.Key(Key);
		if (Fragment.IsEmpty())
		{
			Writer.BeginObject();
			Writer.EndObject();
		}
		else
		{
			Writer.RawValue(Fragment);
		}
	};

	WritePart(TEXT("settings"), Body.Settings);
	WritePart(TEXT("data"), Body.Data);
	Writer.EndObject();
	return Writer.Finish();
}

bool FRocksExporter::Export(const FRocksExportOptions& Options, FRocksExportResult& OutResult)
{
	OutResult = FRocksExportResult();
	if (Options.OutputDirectory.IsEmpty())
	{
		OutResult.Error = TEXT("no output directory");
		return false;
	}

	if (Options.Selections.IsEmpty() && Options.SelectedAssets.IsEmpty())
	{
		OutResult.Error = TEXT("no assets selected");
		return false;
	}

	FRocksExportClosure Closure(IAssetRegistry::GetChecked(), Options.bClosure);
	for (const FString& Selection : Options.Selections)
	{
		if (!Closure.AddRoot(Selection, OutResult.Error))
		{
			return false;
		}
	}

	for (const FAssetData& Asset : Options.SelectedAssets)
	{
		if (!Closure.AddRootAsset(Asset, OutResult.Error))
		{
			return false;
		}
	}

	if (!Closure.Collect(OutResult.Error))
	{
		return false;
	}

	for (const FString& Skipped : Closure.GetSkipped())
	{
		UE_LOG(LogRocksExport, Warning, TEXT("%s: MultiAssetPackage its package holds several assets; exchange format v1 stores one asset per package, so it is not exported"), *Skipped);
	}

	FRocksBundleWriter Bundle;
	if (!Bundle.Begin(Options.OutputDirectory, OutResult.Error))
	{
		return false;
	}

	TArray<FRocksExportAsset> Assets = Closure.GetAssets();
	for (FRocksExportAsset& Asset : Assets)
	{
		FRocksAssetBody Body;
		if (!RocksExporterPrivate::ExtractBody(Asset, Bundle, Body, OutResult.Error))
		{
			return false;
		}

		if (!Bundle.WriteAssetFile(Asset.PackageName.ToString(), MakeAssetJson(Asset, Body), OutResult.Error))
		{
			return false;
		}

		OutResult.UnsupportedCount += Asset.bInScope ? 0 : 1;
		OutResult.DiagnosticCount += Asset.Diagnostics.Num();
		for (const FRocksExportDiagnostic& Diagnostic : Asset.Diagnostics)
		{
			if (Diagnostic.Severity == RocksExport::SeverityInfo)
			{
				UE_LOG(LogRocksExport, Display, TEXT("%s: %s %s"), *Asset.ObjectPath, *Diagnostic.Code, *Diagnostic.Message);
			}
			else
			{
				UE_LOG(LogRocksExport, Warning, TEXT("%s: %s %s"), *Asset.ObjectPath, *Diagnostic.Code, *Diagnostic.Message);
			}
		}
	}

	const TArray<uint8> BundleJson = MakeBundleJson(Assets, Closure.GetRedirects(), RocksExporterPrivate::UtcNow());
	if (!Bundle.Commit(BundleJson, OutResult.Error))
	{
		return false;
	}

	OutResult.AssetCount = Closure.GetAssets().Num();
	OutResult.RedirectCount = Closure.GetRedirects().Num();
	OutResult.SkippedCount = Closure.GetSkipped().Num();
	OutResult.OutputDirectory = FPaths::ConvertRelativePathToFull(Options.OutputDirectory);
	return true;
}
