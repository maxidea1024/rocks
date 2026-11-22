#include "RocksBundleWriter.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"
#include "RocksExportCommon.h"
#include "RocksJsonWriter.h"
#include "RocksSha256.h"

namespace RocksBundleWriterPrivate
{
	static const TCHAR* const ManifestFile = TEXT("bundle.json");
	static const TCHAR* const AssetDirectory = TEXT("assets");
	static const TCHAR* const AssetSuffix = TEXT(".json");
	static const TCHAR* const BlobDirectory = TEXT("blobs");
	static const TCHAR* const BlobSuffix = TEXT(".bin");
	static const TCHAR* const StagingSuffix = TEXT(".staging");
	static const TCHAR* const PreviousSuffix = TEXT(".previous");

	// blob 하위 디렉터리 이름의 hex 글자 수다(3.1절 앞 2자).
	static constexpr int32 BlobFanoutChars = 2;

	struct FLayoutEntry
	{
		const TCHAR* Name;
		int32 ElementBytes;
	};

	// 3.3절 배치 이름과 원소 크기다(가변 배치는 0).
	static constexpr FLayoutEntry Layouts[] = {
		{TEXT("u8"), 1},
		{TEXT("u16"), 2},
		{TEXT("u32"), 4},
		{TEXT("i32"), 4},
		{TEXT("f16"), 2},
		{TEXT("f32"), 4},
		{TEXT("f64"), 8},
		{TEXT("names"), 0},
		{TEXT("boneInfluences"), 0},
		{TEXT("sparseVec3"), 0},
	};

	static bool IsEmptyDirectory(const FString& Directory)
	{
		bool bEmpty = true;
		IFileManager::Get().IterateDirectory(*Directory, [&bEmpty](const TCHAR*, bool)
			{
				bEmpty = false;
				return false;
			});
		return bEmpty;
	}
}

void FRocksBlobRef::WriteTo(FRocksJsonWriter& Writer) const
{
	Writer.BeginObject();
	Writer.Key(TEXT("blob"));
	Writer.String(Hex);
	Writer.Key(TEXT("bytes"));
	Writer.UInt(Bytes);
	Writer.Key(TEXT("layout"));
	Writer.String(Layout);
	Writer.Key(TEXT("components"));
	Writer.UInt(Components);
	Writer.Key(TEXT("count"));
	Writer.UInt(Count);
	Writer.EndObject();
}

FRocksBundleWriter::~FRocksBundleWriter()
{
	Abort();
}

FString FRocksBundleWriter::AssetFileForPackage(const FString& PackageName)
{
	return RocksBundleWriterPrivate::AssetDirectory + PackageName + RocksBundleWriterPrivate::AssetSuffix;
}

FString FRocksBundleWriter::BlobFileForHex(const FString& Hex)
{
	using namespace RocksBundleWriterPrivate;

	return FString::Printf(TEXT("%s/%s/%s%s"), BlobDirectory, *Hex.Left(BlobFanoutChars), *Hex, BlobSuffix);
}

int32 FRocksBundleWriter::LayoutElementBytes(const FString& Layout)
{
	for (const RocksBundleWriterPrivate::FLayoutEntry& Entry : RocksBundleWriterPrivate::Layouts)
	{
		if (Layout.Equals(Entry.Name, ESearchCase::CaseSensitive))
		{
			return Entry.ElementBytes;
		}
	}

	return -1;
}

bool FRocksBundleWriter::Begin(const FString& OutputDirectory, FString& OutError)
{
	using namespace RocksBundleWriterPrivate;

	check(!bActive);
	IFileManager& Files = IFileManager::Get();
	Output = FPaths::ConvertRelativePathToFull(OutputDirectory);
	FPaths::NormalizeDirectoryName(Output);
	if (Output.IsEmpty() || FPaths::IsDrive(Output))
	{
		OutError = FString::Printf(TEXT("output directory '%s' is not usable"), *OutputDirectory);
		return false;
	}

	if (Files.FileExists(*Output))
	{
		OutError = FString::Printf(TEXT("output path %s is a file"), *Output);
		return false;
	}

	if (Files.DirectoryExists(*Output) && !Files.FileExists(*(Output / ManifestFile)) && !IsEmptyDirectory(Output))
	{
		OutError = FString::Printf(TEXT("output directory %s exists and is not an exchange bundle; refusing to replace it"), *Output);
		return false;
	}

	Staging = Output + StagingSuffix;
	if (Files.DirectoryExists(*Staging) && !Files.DeleteDirectory(*Staging, false, true))
	{
		OutError = FString::Printf(TEXT("cannot remove stale staging directory %s"), *Staging);
		return false;
	}

	if (!Files.MakeDirectory(*Staging, true))
	{
		OutError = FString::Printf(TEXT("cannot create staging directory %s"), *Staging);
		return false;
	}

	AssetFiles.Reset();
	Blobs.Reset();
	bActive = true;
	return true;
}

bool FRocksBundleWriter::WriteFile(const FString& Relative, const void* Data, int64 Size, FString& OutError)
{
	const FString Path = Staging / Relative;
	IFileManager& Files = IFileManager::Get();
	if (!Files.MakeDirectory(*FPaths::GetPath(Path), true))
	{
		OutError = FString::Printf(TEXT("cannot create directory for %s"), *Path);
		return false;
	}

	TUniquePtr<FArchive> Writer(Files.CreateFileWriter(*Path, FILEWRITE_NoReplaceExisting));
	if (!Writer)
	{
		OutError = FString::Printf(TEXT("cannot create %s"), *Path);
		return false;
	}

	Writer->Serialize(const_cast<void*>(Data), Size);
	const bool bWritten = !Writer->IsError() && Writer->Close();
	if (!bWritten)
	{
		OutError = FString::Printf(TEXT("cannot write %s"), *Path);
		return false;
	}

	return true;
}

bool FRocksBundleWriter::WriteAssetFile(const FString& PackageName, TConstArrayView<uint8> Json, FString& OutError)
{
	check(bActive);
	const FString Relative = AssetFileForPackage(PackageName);
	if (AssetFiles.Contains(Relative))
	{
		OutError = FString::Printf(TEXT("asset file %s written twice"), *Relative);
		return false;
	}

	AssetFiles.Add(Relative);
	return WriteFile(Relative, Json.GetData(), Json.Num(), OutError);
}

bool FRocksBundleWriter::WriteBlob(TConstArrayView64<uint8> Bytes, const FString& Layout, uint32 Components, uint64 Count, FRocksBlobRef& OutRef, FString& OutError)
{
	check(bActive);
	const int32 ElementBytes = LayoutElementBytes(Layout);
	if (ElementBytes < 0 || Components == 0)
	{
		OutError = FString::Printf(TEXT("blob layout %s with %u components is not valid"), *Layout, Components);
		return false;
	}

	const uint64 Size = uint64(Bytes.Num());
	if (ElementBytes > 0)
	{
		const uint64 Unit = uint64(ElementBytes) * Components;
		if (Size % Unit != 0 || Size / Unit != Count)
		{
			OutError = FString::Printf(TEXT("blob of %llu bytes does not match %s x %u x %llu"), Size, *Layout, Components, Count);
			return false;
		}
	}

	OutRef.Hex = FRocksSha256::HashHex(Bytes.GetData(), Size);
	OutRef.Bytes = Size;
	OutRef.Layout = Layout;
	OutRef.Components = Components;
	OutRef.Count = Count;
	if (Blobs.Contains(OutRef.Hex))
	{
		return true;
	}

	Blobs.Add(OutRef.Hex);
	return WriteFile(BlobFileForHex(OutRef.Hex), Bytes.GetData(), int64(Size), OutError);
}

bool FRocksBundleWriter::Commit(TConstArrayView<uint8> BundleJson, FString& OutError)
{
	using namespace RocksBundleWriterPrivate;

	check(bActive);
	if (!WriteFile(ManifestFile, BundleJson.GetData(), BundleJson.Num(), OutError))
	{
		Abort();
		return false;
	}

	// 기존 묶음은 옆 이름으로 옮긴 뒤 staging을 출력 이름으로 바꾸고 옛것을 지운다. 그래서 출력 이름이 없는
	// 순간은 두 rename 사이뿐이고, 출력 이름에는 언제나 완성된 묶음만 있다.
	IPlatformFile& Platform = FPlatformFileManager::Get().GetPlatformFile();
	IFileManager& Files = IFileManager::Get();
	const FString Previous = Output + PreviousSuffix;
	if (Files.DirectoryExists(*Previous) && !Files.DeleteDirectory(*Previous, false, true))
	{
		OutError = FString::Printf(TEXT("cannot remove stale directory %s"), *Previous);
		Abort();
		return false;
	}

	const bool bHadOutput = Files.DirectoryExists(*Output);
	if (bHadOutput && !Platform.MoveFile(*Previous, *Output))
	{
		OutError = FString::Printf(TEXT("cannot move existing bundle %s aside"), *Output);
		Abort();
		return false;
	}

	if (!Platform.MoveFile(*Output, *Staging))
	{
		OutError = FString::Printf(TEXT("cannot move %s to %s"), *Staging, *Output);
		if (bHadOutput)
		{
			Platform.MoveFile(*Output, *Previous);
		}

		Abort();
		return false;
	}

	bActive = false;
	if (bHadOutput && !Files.DeleteDirectory(*Previous, false, true))
	{
		UE_LOG(LogRocksExport, Warning, TEXT("Previous bundle %s could not be removed"), *Previous);
	}

	return true;
}

void FRocksBundleWriter::Abort()
{
	if (!bActive)
	{
		return;
	}

	bActive = false;
	IFileManager::Get().DeleteDirectory(*Staging, false, true);
}
