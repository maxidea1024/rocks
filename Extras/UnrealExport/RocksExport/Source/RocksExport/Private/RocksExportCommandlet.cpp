#include "RocksExportCommandlet.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "RocksExportCommon.h"
#include "RocksExportSelfTest.h"
#include "RocksExporter.h"

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

		UE_LOG(LogRocksExport, Display, TEXT("Self test %s (%d failures)"), bPassed ? TEXT("passed") : TEXT("failed"), Errors.Num());
		return bPassed ? 0 : 1;
	}

	if (Values.Contains(TEXT("BakeMaterials")) || Switches.Contains(TEXT("BakeMaterials")) || Values.Contains(TEXT("BakeSize")))
	{
		UE_LOG(LogRocksExport, Error, TEXT("-BakeMaterials and -BakeSize are not available before stage UX6"));
		return 1;
	}

	if (Switches.Contains(TEXT("Closure")) && Switches.Contains(TEXT("NoClosure")))
	{
		UE_LOG(LogRocksExport, Error, TEXT("-Closure and -NoClosure are mutually exclusive"));
		return 1;
	}

	FRocksExportOptions Options;
	Options.bClosure = !Switches.Contains(TEXT("NoClosure"));
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
		UE_LOG(LogRocksExport, Error, TEXT("Usage: -run=RocksExport -Assets=<path>[,<path>...] -Out=<bundle directory> [-Closure|-NoClosure] | -SelfTest"));
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
