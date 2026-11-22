#include "Misc/AutomationTest.h"
#include "RocksExportSelfTest.h"

// UE Automation 시험 RocksExport.*이다(12.2절). commandlet -SelfTest와 같은 함수를 부른다.
#if WITH_DEV_AUTOMATION_TESTS

namespace RocksExportAutomationPrivate
{
	static constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;

	static bool Report(FAutomationTestBase& Test, void (*Run)(TArray<FString>&))
	{
		TArray<FString> Errors;
		Run(Errors);
		for (const FString& Error : Errors)
		{
			Test.AddError(Error);
		}

		return Errors.IsEmpty();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRocksExportSha256Test, "RocksExport.Sha256", RocksExportAutomationPrivate::Flags)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRocksExportNumberFormatTest, "RocksExport.NumberFormat", RocksExportAutomationPrivate::Flags)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRocksExportJsonWriterTest, "RocksExport.JsonWriter", RocksExportAutomationPrivate::Flags)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRocksExportPropertyDumpTest, "RocksExport.PropertyDump", RocksExportAutomationPrivate::Flags)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRocksExportBundleWriterTest, "RocksExport.BundleWriter", RocksExportAutomationPrivate::Flags)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRocksExportDeterminismTest, "RocksExport.ExportDeterminism", RocksExportAutomationPrivate::Flags)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRocksExportTextureTest, "RocksExport.TextureExtraction", RocksExportAutomationPrivate::Flags)

bool FRocksExportSha256Test::RunTest(const FString&)
{
	return RocksExportAutomationPrivate::Report(*this, &RocksExportSelfTest::RunSha256);
}

bool FRocksExportNumberFormatTest::RunTest(const FString&)
{
	return RocksExportAutomationPrivate::Report(*this, &RocksExportSelfTest::RunNumberFormat);
}

bool FRocksExportJsonWriterTest::RunTest(const FString&)
{
	return RocksExportAutomationPrivate::Report(*this, &RocksExportSelfTest::RunJsonWriter);
}

bool FRocksExportPropertyDumpTest::RunTest(const FString&)
{
	return RocksExportAutomationPrivate::Report(*this, &RocksExportSelfTest::RunPropertyDump);
}

bool FRocksExportBundleWriterTest::RunTest(const FString&)
{
	return RocksExportAutomationPrivate::Report(*this, &RocksExportSelfTest::RunBundleWriter);
}

bool FRocksExportDeterminismTest::RunTest(const FString&)
{
	return RocksExportAutomationPrivate::Report(*this, &RocksExportSelfTest::RunExportDeterminism);
}

bool FRocksExportTextureTest::RunTest(const FString&)
{
	return RocksExportAutomationPrivate::Report(*this, &RocksExportSelfTest::RunTextureExtraction);
}

#endif
