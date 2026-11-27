#include "RocksExportCommandlet.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "RocksExportCommon.h"
#include "RocksExportSelfTest.h"
#include "RocksExporter.h"
#include "RocksMaterialBaker.h"

URocksExportCommandlet::URocksExportCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
	UseCommandletResultAsExitCode = true;
}

int32 URocksExportCommandlet::Main(const FString& Params)
{
	TArray<FString> Tokens;
	TArray<FString> Switches;
	TMap<FString, FString> Values;
	ParseCommandLine(*Params, Tokens, Switches, Values);

	if (Switches.Contains(TEXT("SelfTest")))
	{
		TArray<FString> Errors;
		const bool bPassed = RocksExportSelfTest::RunAll(Errors);
		for (const FString& Error : Errors)
		{
			UE_LOG(LogRocksExport, Error, TEXT("Self test: %s"), *Error);
		}

		const TArray<FString> NotRun = RocksExportSelfTest::TakeNotRun();
		for (const FString& Reason : NotRun)
		{
			UE_LOG(LogRocksExport, Warning, TEXT("Self test: %s"), *Reason);
		}

		UE_LOG(LogRocksExport, Display, TEXT("Self test %s (%d failures, %d not run)"), bPassed ? TEXT("passed") : TEXT("failed"), Errors.Num(), NotRun.Num());
		return bPassed ? 0 : 1;
	}

	if (Switches.Contains(TEXT("Closure")) && Switches.Contains(TEXT("NoClosure")))
	{
		UE_LOG(LogRocksExport, Error, TEXT("-Closure and -NoClosure are mutually exclusive"));
		return 1;
	}

	FRocksExportOptions Options;
	Options.bClosure = !Switches.Contains(TEXT("NoClosure"));

	// 굽기(8.9절 1번): -BakeMaterials[ -BakeSize=<n>]. 렌더링이 되는 프로세스여야 한다(내보내기 전에 거부한다).
	const FString* BakeSize = Values.Find(TEXT("BakeSize"));
	if (Switches.Contains(TEXT("BakeMaterials")))
	{
		// int64로 읽어 범위를 본다(int32 Atoi는 플랫폼에 따라 넘침을 자른다).
		int64 Size = RocksExport::DefaultBakeSize;
		if (BakeSize != nullptr && (!BakeSize->IsNumeric() || BakeSize->Contains(TEXT(".")) || !LexTryParseString(Size, **BakeSize)
			|| Size < RocksExport::MinBakeSize || Size > RocksExport::MaxBakeSize))
		{
			UE_LOG(LogRocksExport, Error, TEXT("-BakeSize must be an integer from %d to %d"), RocksExport::MinBakeSize, RocksExport::MaxBakeSize);
			return 1;
		}

		if (!FRocksMaterialBaker::CanBake())
		{
			UE_LOG(LogRocksExport, Error, TEXT("-BakeMaterials needs rendering: pass -AllowCommandletRendering and do not pass -nullrhi"));
			return 1;
		}

		Options.BakeSize = int32(Size);
	}
	else if (BakeSize != nullptr || Values.Contains(TEXT("BakeMaterials")))
	{
		UE_LOG(LogRocksExport, Error, TEXT("-BakeSize needs -BakeMaterials (and -BakeMaterials takes no value)"));
		return 1;
	}

	if (const FString* Out = Values.Find(TEXT("Out")))
	{
		Options.OutputDirectory = *Out;
	}

	if (const FString* Assets = Values.Find(TEXT("Assets")))
	{
		FString Normalized = Assets->Replace(TEXT(";"), TEXT(","));
		Normalized.ParseIntoArray(Options.Selections, TEXT(","), true);
	}

	if (Options.Selections.IsEmpty() || Options.OutputDirectory.IsEmpty())
	{
		UE_LOG(LogRocksExport, Error, TEXT("Usage: -run=RocksExport -Assets=<path>[,<path>...] -Out=<bundle directory> [-Closure|-NoClosure] [-BakeMaterials [-BakeSize=<n>]] | -SelfTest"));
		return 1;
	}

	IAssetRegistry::GetChecked().SearchAllAssets(true);

	FRocksExportResult Result;
	if (!FRocksExporter::Export(Options, Result))
	{
		UE_LOG(LogRocksExport, Error, TEXT("Export failed: %s"), *Result.Error);
		return 1;
	}

	UE_LOG(LogRocksExport, Display, TEXT("Exported %d assets (%d outside the exchange scope, %d skipped), %d redirects, %d diagnostics to %s"),
		Result.AssetCount, Result.UnsupportedCount, Result.SkippedCount, Result.RedirectCount, Result.DiagnosticCount, *Result.OutputDirectory);
	return 0;
}
