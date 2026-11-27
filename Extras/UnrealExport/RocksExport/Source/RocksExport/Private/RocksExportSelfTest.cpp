#include "RocksExportSelfTest.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionSubstrate.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MeshDescription.h"
#include "SkeletalMeshAttributes.h"
#include "StaticMeshAttributes.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "IMaterialBakingModule.h"
#include "Math/Float16Color.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "RocksBundleWriter.h"
#include "RocksExportClosure.h"
#include "RocksExportCommon.h"
#include "RocksExportSelfTestTypes.h"
#include "RocksExporter.h"
#include "RocksJsonWriter.h"
#include "RocksMaterialBaker.h"
#include "RocksMeshDescriptionWriter.h"
#include "RocksPropertyDumper.h"
#include "RocksSha256.h"
#include "RocksTextureExtractor.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include <charconv>
#include <clocale>

namespace RocksExportSelfTestPrivate
{
	// 이번 실행에서 돌지 않은 시험이다(MarkNotRun — 게임 스레드에서만 쓴다).
	static TArray<FString> NotRun;

	// 엔진 콘텐츠 시험 대상이다(12.2절 — 엔진 기본 도형).
	static const TCHAR* const CubePath = TEXT("/Engine/BasicShapes/Cube.Cube");
	static const TCHAR* const SpherePath = TEXT("/Engine/BasicShapes/Sphere.Sphere");
	static const TCHAR* const ShapeMaterialPath = TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial");

	// 왕복 시험에 쓰는 비트 패턴 수와 결정적 LCG 상수(Knuth MMIX)다.
	static constexpr int32 RoundTripSamples = 20000;
	static constexpr uint64 LcgMultiplier = 6364136223846793005ull;
	static constexpr uint64 LcgIncrement = 1442695040888963407ull;
	static constexpr uint64 LcgSeed = 0x526f636b73ull;

	// 백만 'a' NIST 벡터의 길이와 나눠 넣는 조각 크기(블록 경계와 어긋나게 고른 소수)다.
	static constexpr int32 MillionBytes = 1000000;
	static constexpr int32 OddChunkBytes = 997;

	static FString SelfTestDirectory()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("RocksExportSelfTest"));
	}

	static void Expect(TArray<FString>& Errors, bool bCondition, const FString& What)
	{
		if (!bCondition)
		{
			Errors.Add(What);
		}
	}

	static FString Utf8ToString(const TArray<uint8>& Bytes)
	{
		const FUTF8ToTCHAR Converted(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num());
		return FString::ConstructFromPtrSize(Converted.Get(), Converted.Length());
	}

	static bool LoadBytes(const FString& Path, TArray<uint8>& Out)
	{
		return FFileHelper::LoadFileToArray(Out, *Path, FILEREAD_Silent);
	}

	// 디렉터리 아래 파일의 상대 경로 목록(정렬)이다.
	static TArray<FString> ListFiles(const FString& Root)
	{
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *Root, TEXT("*"), true, false);
		for (FString& File : Files)
		{
			FPaths::MakePathRelativeTo(File, *(Root + TEXT("/")));
		}

		Files.Sort();
		return Files;
	}

	static TSharedPtr<FJsonObject> ParseJson(const TArray<uint8>& Bytes)
	{
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Utf8ToString(Bytes));
		if (!FJsonSerializer::Deserialize(Reader, Object))
		{
			return nullptr;
		}

		return Object;
	}

	// createdUtc 줄을 뺀 bundle.json이다(3.2절 결정성 예외).
	static FString WithoutCreatedUtc(const TArray<uint8>& Bytes)
	{
		TArray<FString> Lines;
		Utf8ToString(Bytes).ParseIntoArray(Lines, TEXT("\n"), false);
		Lines.RemoveAll([](const FString& Line) { return Line.Contains(TEXT("\"createdUtc\"")); });
		return FString::Join(Lines, TEXT("\n"));
	}

	// 비유한값을 비트로 만든다(상수 넘침 경고 없이).
	static constexpr uint32 FloatInfinityBits = 0x7f800000u;
	static constexpr uint32 FloatQuietNaNBits = 0x7fc00000u;
	static constexpr uint64 DoubleNegativeInfinityBits = 0xfff0000000000000ull;
	static constexpr uint64 DoubleQuietNaNBits = 0x7ff8000000000000ull;

	template <typename T, typename BitsType>
	static T FromBits(BitsType Bits)
	{
		static_assert(sizeof(T) == sizeof(BitsType), "bit pattern size");
		T Value;
		FMemory::Memcpy(&Value, &Bits, sizeof(T));
		return Value;
	}

	template <typename T>
	static bool RoundTrips(T Value, const FString& Text)
	{
		const FTCHARToUTF8 Narrow(*Text);
		T Parsed{};
		const std::from_chars_result Result = std::from_chars(Narrow.Get(), Narrow.Get() + Narrow.Length(), Parsed);
		return Result.ec == std::errc() && Result.ptr == Narrow.Get() + Narrow.Length() && FMemory::Memcmp(&Parsed, &Value, sizeof(T)) == 0;
	}
}

void RocksExportSelfTest::RunSha256(TArray<FString>& Errors)
{
	using namespace RocksExportSelfTestPrivate;

	struct FShaVector
	{
		const ANSICHAR* Message;
		const TCHAR* Digest;
	};

	// FIPS 180-4 예제(NIST CSRC Example Values)다.
	static const FShaVector Vectors[] = {
		{"", TEXT("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")},
		{"abc", TEXT("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")},
		{"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", TEXT("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1")},
		{"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
			TEXT("cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1")},
	};

	for (const FShaVector& Vector : Vectors)
	{
		const int32 Length = FCStringAnsi::Strlen(Vector.Message);
		const FString Digest = FRocksSha256::HashHex(Vector.Message, Length);
		Expect(Errors, Digest == Vector.Digest, FString::Printf(TEXT("SHA-256 of '%hs' is %s"), Vector.Message, *Digest));

		// 1 byte씩 넣어도 같아야 한다(조각 경계 무관).
		FRocksSha256 Bytewise;
		for (int32 Index = 0; Index < Length; ++Index)
		{
			Bytewise.Update(Vector.Message + Index, 1);
		}

		Expect(Errors, Bytewise.FinalizeHex() == Vector.Digest, FString::Printf(TEXT("byte-wise SHA-256 of '%hs' differs"), Vector.Message));
	}

	TArray<uint8> Million;
	Million.Init('a', MillionBytes);
	static const TCHAR* const MillionDigest = TEXT("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
	Expect(Errors, FRocksSha256::HashHex(Million.GetData(), Million.Num()) == MillionDigest, TEXT("SHA-256 of one million 'a'"));

	FRocksSha256 Chunked;
	for (int32 Offset = 0; Offset < Million.Num(); Offset += OddChunkBytes)
	{
		Chunked.Update(Million.GetData() + Offset, FMath::Min(OddChunkBytes, Million.Num() - Offset));
	}

	Expect(Errors, Chunked.FinalizeHex() == MillionDigest, TEXT("chunked SHA-256 of one million 'a'"));

	// 경계 길이(55·56·63·64·65 byte)는 한 번에 넣은 결과와 1 byte씩 넣은 결과가 같아야 한다.
	for (const int32 Length : {55, 56, 63, 64, 65, 119, 120})
	{
		const FString Whole = FRocksSha256::HashHex(Million.GetData(), Length);
		FRocksSha256 Split;
		Split.Update(Million.GetData(), 1);
		Split.Update(Million.GetData() + 1, Length - 1);
		Expect(Errors, Split.FinalizeHex() == Whole, FString::Printf(TEXT("SHA-256 split at length %d"), Length));
	}
}

void RocksExportSelfTest::RunNumberFormat(TArray<FString>& Errors)
{
	using namespace RocksExportSelfTestPrivate;

	struct FFloatCase
	{
		float Value;
		const TCHAR* Text;
	};

	struct FDoubleCase
	{
		double Value;
		const TCHAR* Text;
	};

	// 기대값은 C printf("%.9g"·"%.17g")와 같은 표기다.
	const FFloatCase Floats[] = {
		{0.1f, TEXT("0.100000001")},
		{1.0f, TEXT("1")},
		{-0.0f, TEXT("-0")},
		{3.40282347e+38f, TEXT("3.40282347e+38")},
		{1.40129846e-45f, TEXT("1.40129846e-45")},
		{123456789.0f, TEXT("123456792")},
		{1e20f, TEXT("1.00000002e+20")},
		{-2.5e-7f, TEXT("-2.49999999e-07")},
	};

	const FDoubleCase Doubles[] = {
		{0.1, TEXT("0.10000000000000001")},
		{1.0 / 3.0, TEXT("0.33333333333333331")},
		{1.7976931348623157e+308, TEXT("1.7976931348623157e+308")},
		{4.9406564584124654e-324, TEXT("4.9406564584124654e-324")},
		{1e21, TEXT("1e+21")},
		{123456.0, TEXT("123456")},
		{-2.5e-7, TEXT("-2.4999999999999999e-07")},
		{1e-5, TEXT("1.0000000000000001e-05")},
	};

	for (const FFloatCase& Case : Floats)
	{
		const FString Text = FRocksJsonWriter::FormatFloat(Case.Value);
		Expect(Errors, Text == Case.Text, FString::Printf(TEXT("float format %s, expected %s"), *Text, Case.Text));
	}

	for (const FDoubleCase& Case : Doubles)
	{
		const FString Text = FRocksJsonWriter::FormatDouble(Case.Value);
		Expect(Errors, Text == Case.Text, FString::Printf(TEXT("double format %s, expected %s"), *Text, Case.Text));
	}

	Expect(Errors, FRocksJsonWriter::FormatFloat(FromBits<float>(FloatInfinityBits)) == TEXT("\"inf\""), TEXT("float +inf"));
	Expect(Errors, FRocksJsonWriter::FormatDouble(FromBits<double>(DoubleNegativeInfinityBits)) == TEXT("\"-inf\""), TEXT("double -inf"));
	Expect(Errors, FRocksJsonWriter::FormatDouble(FromBits<double>(DoubleQuietNaNBits)) == TEXT("\"nan\""), TEXT("double nan"));

	// 결정적 비트 패턴 왕복: 유한값은 다시 읽으면 같은 비트다.
	uint64 State = LcgSeed;
	int32 FloatFailures = 0;
	int32 DoubleFailures = 0;
	for (int32 Sample = 0; Sample < RoundTripSamples; ++Sample)
	{
		State = State * LcgMultiplier + LcgIncrement;
		const float FloatValue = FromBits<float>(uint32(State >> 32));
		if (FMath::IsFinite(FloatValue) && !RoundTrips(FloatValue, FRocksJsonWriter::FormatFloat(FloatValue)))
		{
			++FloatFailures;
		}

		const double DoubleValue = FromBits<double>(State);
		if (FMath::IsFinite(DoubleValue) && !RoundTrips(DoubleValue, FRocksJsonWriter::FormatDouble(DoubleValue)))
		{
			++DoubleFailures;
		}
	}

	Expect(Errors, FloatFailures == 0, FString::Printf(TEXT("%d float values did not round-trip"), FloatFailures));
	Expect(Errors, DoubleFailures == 0, FString::Printf(TEXT("%d double values did not round-trip"), DoubleFailures));

	// 소수점이 쉼표인 로캘에서도 같은 표기다(std::to_chars는 로캘을 보지 않는다).
	const char* Current = std::setlocale(LC_NUMERIC, nullptr);
	const FString Saved = Current != nullptr ? FString(ANSI_TO_TCHAR(Current)) : FString(TEXT("C"));
	if (std::setlocale(LC_NUMERIC, "de-DE") != nullptr || std::setlocale(LC_NUMERIC, "de_DE.UTF-8") != nullptr)
	{
		Expect(Errors, FRocksJsonWriter::FormatDouble(2.5) == TEXT("2.5"), TEXT("number format depends on the C locale"));
		std::setlocale(LC_NUMERIC, TCHAR_TO_ANSI(*Saved));
	}
	else
	{
		UE_LOG(LogTemp, Display, TEXT("RocksExport self test: German locale unavailable, locale check skipped"));
	}
}

void RocksExportSelfTest::RunJsonWriter(TArray<FString>& Errors)
{
	using namespace RocksExportSelfTestPrivate;

	// 문자열: escape 7종, 제어 문자 \u, é(2 byte), U+1F600(surrogate 쌍 → 4 byte), 짝 없는 surrogate(→ U+FFFD).
	FString Text = TEXT("q\"b\\s\b\f\n\r\t");
	Text.AppendChar(TCHAR(0x01));
	Text.AppendChar(TCHAR(0x00e9));
	Text.AppendChar(TCHAR(0xd83d));
	Text.AppendChar(TCHAR(0xde00));
	Text.AppendChar(TCHAR(0xd800));
	FRocksJsonWriter Compact(FRocksJsonWriter::EStyle::Compact);
	Compact.String(Text);
	const TArray<uint8> Bytes = Compact.Finish();
	static const uint8 Expected[] = {
		'"', 'q', '\\', '"', 'b', '\\', '\\', 's', '\\', 'b', '\\', 'f', '\\', 'n', '\\', 'r', '\\', 't',
		'\\', 'u', '0', '0', '0', '1', 0xc3, 0xa9, 0xf0, 0x9f, 0x98, 0x80, 0xef, 0xbf, 0xbd, '"'};
	Expect(Errors, Bytes.Num() == UE_ARRAY_COUNT(Expected) && FMemory::Memcmp(Bytes.GetData(), Expected, sizeof(Expected)) == 0,
		TEXT("JSON string escaping or UTF-8 encoding"));

	// 들여쓰기 두 칸, 빈 컨테이너, 끝 LF.
	FRocksJsonWriter Pretty;
	Pretty.BeginObject();
	Pretty.Key(TEXT("a"));
	Pretty.Int(-1);
	Pretty.Key(TEXT("b"));
	Pretty.BeginArray();
	Pretty.UInt(2);
	Pretty.Bool(false);
	Pretty.Null();
	Pretty.EndArray();
	Pretty.Key(TEXT("c"));
	Pretty.BeginObject();
	Pretty.EndObject();
	Pretty.Key(TEXT("d"));
	Pretty.BeginArray();
	Pretty.EndArray();
	Pretty.EndObject();
	const FString PrettyText = Utf8ToString(Pretty.Finish());
	const FString PrettyExpected = TEXT("{\n  \"a\": -1,\n  \"b\": [\n    2,\n    false,\n    null\n  ],\n  \"c\": {},\n  \"d\": []\n}\n");
	Expect(Errors, PrettyText == PrettyExpected, FString::Printf(TEXT("pretty JSON layout:\n%s"), *PrettyText));

	// UTF-8 byte 순: 대문자 < 소문자, U+E000 < U+10000(UTF-16 단위 순과 반대), 접두가 앞.
	FString Private;
	Private.AppendChar(TCHAR(0xe000));
	FString Supplementary;
	Supplementary.AppendChar(TCHAR(0xd800));
	Supplementary.AppendChar(TCHAR(0xdc00));
	Expect(Errors, FRocksJsonWriter::CompareUtf8(TEXT("Beta"), TEXT("alpha")) < 0, TEXT("UTF-8 order of ASCII case"));
	Expect(Errors, FRocksJsonWriter::CompareUtf8(Private, Supplementary) < 0, TEXT("UTF-8 order of U+E000 and U+10000"));
	Expect(Errors, FRocksJsonWriter::CompareUtf8(TEXT("/Game/A"), TEXT("/Game/A.A")) < 0, TEXT("UTF-8 order of a prefix"));
	Expect(Errors, FRocksJsonWriter::CompareUtf8(TEXT("x"), TEXT("x")) == 0, TEXT("UTF-8 order of equal strings"));

	// objectPath 문법(Rocks 리더와 같은 규칙).
	Expect(Errors, RocksIsValidExchangeObjectPath(TEXT("/Game/My Props/_Shared._Shared")), TEXT("valid object path rejected"));
	for (const TCHAR* Invalid : {TEXT("Game/A.A"), TEXT("/Game.A"), TEXT("/Game/A"), TEXT("/Game/A.A:Sub"), TEXT("/Game//A.A"), TEXT("/Game/A.B.C"), TEXT("/Game/A.")})
	{
		Expect(Errors, !RocksIsValidExchangeObjectPath(Invalid), FString::Printf(TEXT("invalid object path %s accepted"), Invalid));
	}
}

void RocksExportSelfTest::RunPropertyDump(TArray<FString>& Errors)
{
	using namespace RocksExportSelfTestPrivate;

	UObject* Cube = LoadObject<UObject>(nullptr, CubePath);
	Expect(Errors, Cube != nullptr, TEXT("cannot load the engine cube"));
	if (Cube == nullptr)
	{
		return;
	}

	FRocksSelfTestStruct Value;
	Value.Missing = FromBits<float>(FloatQuietNaNBits);
	Value.Huge = FromBits<double>(DoubleNegativeInfinityBits);
	Value.Text = TEXT("a\"b\n");
	Value.Text.AppendChar(TCHAR(0x00e9));
	Value.Values = {3, 1, 2};
	Value.Tags = {TEXT("b"), TEXT("a"), TEXT("c")};
	Value.Weights.Add(TEXT("zeta"), 1);
	Value.Weights.Add(TEXT("alpha"), 2);
	Value.Weights.Add(TEXT("Beta"), 3);
	Value.Reference = Cube;
	Value.SoftReference = TSoftObjectPtr<UObject>(FSoftObjectPath(SpherePath));
	Value.WeakReference = LoadObject<UObject>(nullptr, SpherePath);
	Value.Maybe = 0.25f;
	Value.Utf8Text = FUtf8String(TEXT("u8"));

	FRocksPropertyDumper Dumper(nullptr);
	FRocksJsonWriter Writer(FRocksJsonWriter::EStyle::Compact);
	Dumper.DumpStruct(FRocksSelfTestStruct::StaticStruct(), &Value, Writer);
	const FString Dumped = Utf8ToString(Writer.Finish());
	FString Expected = TEXT("{\"bFlag\":true,\"Count\":-7,\"Big\":18446744073709551615,\"Ratio\":0.100000001,\"Precise\":0.10000000000000001,")
		TEXT("\"Missing\":\"nan\",\"Huge\":\"-inf\",\"Text\":\"a\\\"b\\n");
	Expected.AppendChar(TCHAR(0x00e9));
	Expected += TEXT("\",\"Label\":\"Rock\",\"Mode\":\"Second\",\"Values\":[3,1,2],\"Tags\":[\"a\",\"b\",\"c\"],")
		TEXT("\"Weights\":[{\"key\":\"Beta\",\"value\":3},{\"key\":\"alpha\",\"value\":2},{\"key\":\"zeta\",\"value\":1}],")
		TEXT("\"Offset\":{\"X\":1,\"Y\":2.5,\"Z\":-3},\"Inner\":{\"Value\":4},")
		TEXT("\"Reference\":{\"objectPath\":\"/Engine/BasicShapes/Cube.Cube\"},")
		TEXT("\"SoftReference\":{\"objectPath\":\"/Engine/BasicShapes/Sphere.Sphere\"},\"Fixed\":[5,6],")
		TEXT("\"@blob\":1,\"WeakReference\":{\"objectPath\":\"/Engine/BasicShapes/Sphere.Sphere\"},\"Maybe\":0.25,\"Nothing\":null,\"Utf8Text\":\"u8\"}");
	Expect(Errors, Dumped == Expected, FString::Printf(TEXT("property dump:\n%s\nexpected:\n%s"), *Dumped, *Expected));

	const TArray<FRocksPropertyDumper::FReference> References = Dumper.GetReferences();
	Expect(Errors, References.Num() == 2 && References[0].Path == CubePath && References[0].bHard && References[1].Path == SpherePath && !References[1].bHard,
		TEXT("property dump references"));

	// 엔진 에셋 덤프: 두 번 덤프한 bytes가 같고 UE JSON 파서가 읽는다(정적 메시·머티리얼).
	for (const TCHAR* Path : {CubePath, ShapeMaterialPath})
	{
		const UObject* Asset = LoadObject<UObject>(nullptr, Path);
		Expect(Errors, Asset != nullptr, FString::Printf(TEXT("cannot load %s"), Path));
		if (Asset == nullptr)
		{
			continue;
		}

		TArray<uint8> Dumps[2];
		for (TArray<uint8>& Dump : Dumps)
		{
			FRocksPropertyDumper AssetDumper(Asset);
			FRocksJsonWriter AssetWriter;
			AssetDumper.DumpObject(Asset, AssetWriter);
			Dump = AssetWriter.Finish();
		}

		Expect(Errors, Dumps[0] == Dumps[1], FString::Printf(TEXT("dump of %s is not deterministic"), Path));
		Expect(Errors, ParseJson(Dumps[0]).IsValid(), FString::Printf(TEXT("dump of %s is not valid JSON"), Path));
	}
}

void RocksExportSelfTest::RunBundleWriter(TArray<FString>& Errors)
{
	using namespace RocksExportSelfTestPrivate;

	IFileManager& Files = IFileManager::Get();
	const FString Root = SelfTestDirectory() / TEXT("Writer");
	Files.DeleteDirectory(*Root, false, true);
	const FString Output = Root / TEXT("Out.rxbundle");

	static const uint8 BlobBytes[] = {1, 2, 3};
	static const TCHAR* const BlobHex = TEXT("039058c6f2c0cb492c533b0a4d14ef77cc0f78abccced5287d84a1a2011cfb81");
	static const uint8 Json[] = {'{', '}', '\n'};
	{
		FRocksBundleWriter Bundle;
		FString Failure;
		Expect(Errors, Bundle.Begin(Output, Failure), FString::Printf(TEXT("writer begin: %s"), *Failure));
		FRocksBlobRef Ref;
		Expect(Errors, Bundle.WriteBlob(MakeArrayView(BlobBytes, 3), TEXT("u8"), 1, 3, Ref, Failure), TEXT("blob write"));
		Expect(Errors, Ref.Hex == BlobHex && Ref.Bytes == 3 && Ref.Layout == TEXT("u8") && Ref.Components == 1 && Ref.Count == 3, TEXT("blob reference"));
		FRocksBlobRef Again;
		Expect(Errors, Bundle.WriteBlob(MakeArrayView(BlobBytes, 3), TEXT("u8"), 3, 1, Again, Failure) && Again.Hex == BlobHex, TEXT("repeated blob"));
		Expect(Errors, !Bundle.WriteBlob(MakeArrayView(BlobBytes, 3), TEXT("u16"), 1, 1, Again, Failure), TEXT("blob size check"));
		Expect(Errors, !Bundle.WriteBlob(MakeArrayView(BlobBytes, 3), TEXT("u24"), 1, 1, Again, Failure), TEXT("blob layout check"));
		Expect(Errors, Bundle.WriteBlob(MakeArrayView(BlobBytes, 3), TEXT("names"), 1, 7, Again, Failure), TEXT("variable layout blob"));
		Expect(Errors, Bundle.WriteAssetFile(TEXT("/Game/A"), MakeArrayView(Json, 3), Failure), TEXT("asset write"));
		Expect(Errors, !Bundle.WriteAssetFile(TEXT("/Game/A"), MakeArrayView(Json, 3), Failure), TEXT("duplicate asset write"));
		Expect(Errors, !Files.DirectoryExists(*Output), TEXT("output visible before commit"));
		Expect(Errors, Bundle.Commit(MakeArrayView(Json, 3), Failure), FString::Printf(TEXT("commit: %s"), *Failure));
	}

	Expect(Errors, ListFiles(Output) == TArray<FString>({FString(TEXT("assets/Game/A.json")), TEXT("blobs/03/") + FString(BlobHex) + TEXT(".bin"), TEXT("bundle.json")}),
		TEXT("bundle file layout"));
	Expect(Errors, !Files.DirectoryExists(*(Output + TEXT(".staging"))), TEXT("staging left after commit"));

	// 기존 묶음 교체: 새 내용만 남고 옛 파일·옆 디렉터리는 없다.
	{
		FRocksBundleWriter Bundle;
		FString Failure;
		Expect(Errors, Bundle.Begin(Output, Failure) && Bundle.WriteAssetFile(TEXT("/Game/B"), MakeArrayView(Json, 3), Failure) && Bundle.Commit(MakeArrayView(Json, 3), Failure),
			FString::Printf(TEXT("replace: %s"), *Failure));
	}

	Expect(Errors, ListFiles(Output) == TArray<FString>({FString(TEXT("assets/Game/B.json")), FString(TEXT("bundle.json"))}), TEXT("replaced bundle layout"));
	Expect(Errors, !Files.DirectoryExists(*(Output + TEXT(".previous"))), TEXT("previous bundle left"));

	// Commit 없이 끝나면 출력은 그대로이고 staging은 없다.
	{
		FRocksBundleWriter Bundle;
		FString Failure;
		Expect(Errors, Bundle.Begin(Output, Failure) && Bundle.WriteAssetFile(TEXT("/Game/C"), MakeArrayView(Json, 3), Failure), TEXT("aborted export"));
	}

	Expect(Errors, ListFiles(Output) == TArray<FString>({FString(TEXT("assets/Game/B.json")), FString(TEXT("bundle.json"))}), TEXT("output changed by an aborted export"));
	Expect(Errors, !Files.DirectoryExists(*(Output + TEXT(".staging"))), TEXT("staging left after abort"));

	// 묶음이 아닌 디렉터리는 덮지 않는다.
	const FString Foreign = Root / TEXT("Foreign");
	FFileHelper::SaveStringToFile(TEXT("keep"), *(Foreign / TEXT("keep.txt")));
	{
		FRocksBundleWriter Bundle;
		FString Failure;
		Expect(Errors, !Bundle.Begin(Foreign, Failure), TEXT("non-bundle directory accepted as output"));
	}

	Expect(Errors, Files.FileExists(*(Foreign / TEXT("keep.txt"))), TEXT("non-bundle directory was modified"));

	// 묶음 모양이 아닌 옛 staging 디렉터리는 지우지 않고 거부한다(11.7절 25번).
	const FString Staging = Output + TEXT(".staging");
	FFileHelper::SaveStringToFile(TEXT("keep"), *(Staging / TEXT("notes.txt")));
	{
		FRocksBundleWriter Bundle;
		FString Failure;
		Expect(Errors, !Bundle.Begin(Output, Failure), TEXT("foreign staging directory removed"));
	}

	Expect(Errors, Files.FileExists(*(Staging / TEXT("notes.txt"))), TEXT("foreign staging directory was modified"));
	Files.DeleteDirectory(*Root, false, true);
}

void RocksExportSelfTest::RunExportDeterminism(TArray<FString>& Errors)
{
	using namespace RocksExportSelfTestPrivate;

	IAssetRegistry::GetChecked().SearchAllAssets(true);
	const FString Root = SelfTestDirectory() / TEXT("Export");
	IFileManager::Get().DeleteDirectory(*Root, false, true);

	FString Outputs[2] = {Root / TEXT("A.rxbundle"), Root / TEXT("B.rxbundle")};
	for (const FString& Output : Outputs)
	{
		FRocksExportOptions Options;
		Options.Selections = {CubePath};
		Options.OutputDirectory = Output;
		FRocksExportResult Result;
		Expect(Errors, FRocksExporter::Export(Options, Result), FString::Printf(TEXT("export: %s"), *Result.Error));
	}

	const TArray<FString> FilesA = ListFiles(Outputs[0]);
	Expect(Errors, FilesA == ListFiles(Outputs[1]), TEXT("two exports wrote different file sets"));
	for (const FString& File : FilesA)
	{
		TArray<uint8> BytesA;
		TArray<uint8> BytesB;
		Expect(Errors, LoadBytes(Outputs[0] / File, BytesA) && LoadBytes(Outputs[1] / File, BytesB), FString::Printf(TEXT("cannot read %s"), *File));
		const bool bSame = File == TEXT("bundle.json") ? WithoutCreatedUtc(BytesA) == WithoutCreatedUtc(BytesB) : BytesA == BytesB;
		Expect(Errors, bSame, FString::Printf(TEXT("%s differs between two exports"), *File));
	}

	// 형식: UE JSON 파서로 다시 읽어 3.4절 키·정렬·파일 대응과 정육면체(StaticMesh)를 확인한다.
	TArray<uint8> Manifest;
	Expect(Errors, LoadBytes(Outputs[0] / TEXT("bundle.json"), Manifest), TEXT("cannot read bundle.json"));
	const TSharedPtr<FJsonObject> Bundle = ParseJson(Manifest);
	Expect(Errors, Bundle.IsValid(), TEXT("bundle.json is not valid JSON"));
	if (Bundle.IsValid())
	{
		Expect(Errors, Bundle->GetIntegerField(TEXT("formatVersion")) == 1, TEXT("formatVersion"));
		Expect(Errors, Bundle->GetObjectField(TEXT("exporter"))->GetStringField(TEXT("plugin")) == TEXT("RocksExport"), TEXT("exporter.plugin"));
		const TArray<TSharedPtr<FJsonValue>>& Assets = Bundle->GetArrayField(TEXT("assets"));
		bool bHasCube = false;
		FString Previous;
		for (const TSharedPtr<FJsonValue>& Item : Assets)
		{
			const TSharedPtr<FJsonObject> Entry = Item->AsObject();
			const FString ObjectPath = Entry->GetStringField(TEXT("objectPath"));
			Expect(Errors, Previous.IsEmpty() || FRocksJsonWriter::CompareUtf8(Previous, ObjectPath) < 0, TEXT("assets not strictly sorted"));
			Previous = ObjectPath;
			bHasCube = bHasCube || (ObjectPath == CubePath && Entry->GetStringField(TEXT("class")) == TEXT("StaticMesh"));
			TArray<uint8> AssetBytes;
			const FString File = Entry->GetStringField(TEXT("file"));
			const TSharedPtr<FJsonObject> Asset = LoadBytes(Outputs[0] / File, AssetBytes) ? ParseJson(AssetBytes) : nullptr;
			Expect(Errors, Asset.IsValid() && Asset->GetStringField(TEXT("objectPath")) == ObjectPath, FString::Printf(TEXT("asset file %s"), *File));
		}

		Expect(Errors, bHasCube, TEXT("cube missing from the export"));
	}

	IFileManager::Get().DeleteDirectory(*Root, false, true);
}

void RocksExportSelfTest::RunTextureExtraction(TArray<FString>& Errors)
{
	using namespace RocksExportSelfTestPrivate;

	// 형식·차원이 다른 엔진 텍스처다(BGRA8 PNG, G8 JPEG, RGBA16F, BGRE8 UE Delta 큐브, BGRA8 큐브·볼륨·배열, R32F 볼륨).
	static const TCHAR* const Textures[] = {
		TEXT("/Engine/EngineMaterials/T_Default_Material_Grid_M.T_Default_Material_Grid_M"),
		TEXT("/Engine/EngineMaterials/T_Grid_Crosses.T_Grid_Crosses"),
		TEXT("/Engine/EngineMaterials/DefaultBloomKernel.DefaultBloomKernel"),
		TEXT("/Engine/EditorMaterials/AssetViewer/T_GreyAmbient.T_GreyAmbient"),
		TEXT("/Engine/EngineResources/DefaultTextureCube.DefaultTextureCube"),
		TEXT("/Engine/EngineResources/DefaultVolumeTexture.DefaultVolumeTexture"),
		TEXT("/Engine/EngineMaterials/WeightMapArrayPlaceholderTexture.WeightMapArrayPlaceholderTexture"),
		TEXT("/Engine/EngineMaterials/Substrate/Volume/SubstrateSimpleVolumeLUT3D.SubstrateSimpleVolumeLUT3D"),
	};

	IAssetRegistry::GetChecked().SearchAllAssets(true);
	const FString Root = SelfTestDirectory() / TEXT("Texture");
	IFileManager::Get().DeleteDirectory(*Root, false, true);
	const FString Output = Root / TEXT("Textures.rxbundle");
	FRocksExportOptions Options;
	Options.OutputDirectory = Output;
	Options.bClosure = false;
	for (const TCHAR* Path : Textures)
	{
		Options.Selections.Add(Path);
	}

	FRocksExportResult Result;
	Expect(Errors, FRocksExporter::Export(Options, Result), FString::Printf(TEXT("texture export: %s"), *Result.Error));
	for (const TCHAR* Path : Textures)
	{
		UTexture* Texture = LoadObject<UTexture>(nullptr, Path);
		const FString Package = FSoftObjectPath(Path).GetLongPackageName();
		TArray<uint8> Json;
		const TSharedPtr<FJsonObject> Asset = Texture != nullptr && LoadBytes(Output / FRocksBundleWriter::AssetFileForPackage(Package), Json) ? ParseJson(Json) : nullptr;
		Expect(Errors, Asset.IsValid(), FString::Printf(TEXT("%s: no texture JSON"), Path));
		if (!Asset.IsValid())
		{
			continue;
		}

		const TSharedPtr<FJsonObject> Settings = Asset->GetObjectField(TEXT("settings"));
		Expect(Errors, !Settings->HasField(TEXT("Source")) && Settings->HasField(TEXT("SRGB")) && Settings->HasField(TEXT("CompressionSettings")),
			FString::Printf(TEXT("%s: settings dump"), Path));

		FTextureSource& Source = Texture->Source;
		const TSharedPtr<FJsonObject> Data = Asset->GetObjectField(TEXT("data"))->GetObjectField(TEXT("source"));
		FString FormatName;
		FString Layout;
		uint32 Components = 0;
		Expect(Errors, FRocksTextureExtractor::DescribeSourceFormat(Source.GetFormat(0), FormatName, Layout, Components) && Data->GetStringField(TEXT("format")) == FormatName,
			FString::Printf(TEXT("%s: source format"), Path));
		const bool bLossy = Source.GetSourceCompression() == TSCF_JPEG || Source.GetSourceCompression() == TSCF_UEJPEG;
		Expect(Errors, Data->TryGetField(TEXT("lossyOrigin")).IsValid() && bLossy == !Data->TryGetField(TEXT("lossyOrigin"))->IsNull(), FString::Printf(TEXT("%s: lossyOrigin"), Path));
		Expect(Errors, Data->GetBoolField(TEXT("longLat")) == Source.IsLongLatCubemap(), FString::Printf(TEXT("%s: longLat"), Path));

		const TArray<TSharedPtr<FJsonValue>>& Blocks = Data->GetArrayField(TEXT("blocks"));
		Expect(Errors, Blocks.Num() == Source.GetNumBlocks(), FString::Printf(TEXT("%s: block count"), Path));
		int32 Compared = 0;
		for (int32 BlockIndex = 0; BlockIndex < Blocks.Num(); ++BlockIndex)
		{
			const TArray<TSharedPtr<FJsonValue>>& Mips = Blocks[BlockIndex]->AsObject()->GetArrayField(TEXT("mips"));
			for (int32 Mip = 0; Mip < Mips.Num(); ++Mip)
			{
				const TSharedPtr<FJsonObject> MipObject = Mips[Mip]->AsObject();
				const uint64 Texels = uint64(MipObject->GetNumberField(TEXT("sizeX"))) * uint64(MipObject->GetNumberField(TEXT("sizeY"))) * uint64(MipObject->GetNumberField(TEXT("numSlices")));
				const TArray<TSharedPtr<FJsonValue>>& Pixels = MipObject->GetArrayField(TEXT("pixels"));
				Expect(Errors, Pixels.Num() == Source.GetNumLayers(), FString::Printf(TEXT("%s: layer count"), Path));
				for (int32 Layer = 0; Layer < Pixels.Num(); ++Layer)
				{
					// 원본 대조: LockMip을 거치는 다른 경로로 같은 mip을 꺼내 SHA-256·크기·blob 파일 내용을 비교한다.
					const TSharedPtr<FJsonObject> Blob = Pixels[Layer]->AsObject();
					TArray64<uint8> Expected;
					TArray<uint8> Written;
					const FString Hex = Blob->GetStringField(TEXT("blob"));
					const bool bRead = Source.GetMipData(Expected, BlockIndex, Layer, Mip) && LoadBytes(Output / FRocksBundleWriter::BlobFileForHex(Hex), Written);
					const bool bSame = bRead && Hex == FRocksSha256::HashHex(Expected.GetData(), Expected.Num())
						&& uint64(Blob->GetNumberField(TEXT("bytes"))) == uint64(Expected.Num()) && int64(Written.Num()) == Expected.Num()
						&& FMemory::Memcmp(Written.GetData(), Expected.GetData(), Written.Num()) == 0
						&& uint64(Blob->GetNumberField(TEXT("count"))) == Texels && Blob->GetStringField(TEXT("layout")) == Layout;
					Expect(Errors, bSame, FString::Printf(TEXT("%s: block %d mip %d layer %d differs from the texture source"), Path, BlockIndex, Mip, Layer));
					++Compared;
				}
			}
		}

		Expect(Errors, Compared > 0, FString::Printf(TEXT("%s: no mip compared"), Path));
	}

	IFileManager::Get().DeleteDirectory(*Root, false, true);
}

void RocksExportSelfTest::RunStaticMeshExtraction(TArray<FString>& Errors)
{
	using namespace RocksExportSelfTestPrivate;

	static const TCHAR* const Meshes[] = {CubePath, SpherePath};

	// 에셋 상태의 지문: UPROPERTY 덤프 전체, LOD별 원본 메시 설명 기록의 digest, 빌드 설정의 lightmap 번호.
	const auto Snapshot = [](UStaticMesh& Mesh)
	{
		FString Text;
		FRocksPropertyDumper Dumper(&Mesh);
		FRocksJsonWriter Writer(FRocksJsonWriter::EStyle::Compact);
		Dumper.DumpObject(&Mesh, Writer);
		Text = Utf8ToString(Writer.Finish());
		for (int32 Lod = 0; Lod < Mesh.GetNumSourceModels(); ++Lod)
		{
			const FMeshBuildSettings& Settings = Mesh.GetSourceModel(Lod).BuildSettings;
			Text += FString::Printf(TEXT("|%d:%d:%d"), Lod, Settings.SrcLightmapIndex, Settings.DstLightmapIndex);
			FMeshDescription Source;
			if (Mesh.IsMeshDescriptionValid(Lod) && Mesh.CloneMeshDescription(Lod, Source))
			{
				FRocksMeshDescriptionWriter::Compact(Source);
				FRocksMeshSourceRecord Record;
				TArray<FRocksExportDiagnostic> Ignored;
				FString Problem;
				Text += FRocksMeshDescriptionWriter::FromMeshDescription(Source, Record, Ignored, Problem) ? FRocksMeshDescriptionWriter::Digest(Record) : Problem;
			}
		}

		return Text;
	};

	IAssetRegistry::GetChecked().SearchAllAssets(true);
	const FString Root = SelfTestDirectory() / TEXT("StaticMesh");
	IFileManager::Get().DeleteDirectory(*Root, false, true);
	const FString Output = Root / TEXT("Meshes.rxbundle");
	TArray<FString> Before;
	for (const TCHAR* Path : Meshes)
	{
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, Path);
		Expect(Errors, Mesh != nullptr, FString::Printf(TEXT("cannot load %s"), Path));
		Before.Add(Mesh != nullptr ? Snapshot(*Mesh) : FString());
	}

	FRocksExportOptions Options;
	Options.OutputDirectory = Output;
	Options.bClosure = false;
	for (const TCHAR* Path : Meshes)
	{
		Options.Selections.Add(Path);
	}

	FRocksExportResult Result;
	Expect(Errors, FRocksExporter::Export(Options, Result), FString::Printf(TEXT("static mesh export: %s"), *Result.Error));
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Meshes); ++Index)
	{
		const TCHAR* Path = Meshes[Index];
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, Path);
		if (Mesh == nullptr)
		{
			continue;
		}

		// 12.2절: prepared 추출(SetupRenderMeshDescription)이 원본 에셋 설정·원본 메시를 바꾸지 않는다.
		Expect(Errors, Snapshot(*Mesh) == Before[Index], FString::Printf(TEXT("%s: extraction changed the asset"), Path));

		TArray<uint8> Json;
		const TSharedPtr<FJsonObject> Asset = LoadBytes(Output / FRocksBundleWriter::AssetFileForPackage(FSoftObjectPath(Path).GetLongPackageName()), Json) ? ParseJson(Json) : nullptr;
		Expect(Errors, Asset.IsValid(), FString::Printf(TEXT("%s: no mesh JSON"), Path));
		if (!Asset.IsValid())
		{
			continue;
		}

		const TArray<TSharedPtr<FJsonValue>>& Lods = Asset->GetObjectField(TEXT("data"))->GetArrayField(TEXT("lods"));
		Expect(Errors, Lods.Num() == Mesh->GetNumSourceModels() && Asset->GetObjectField(TEXT("settings"))->HasField(TEXT("SourceModels")),
			FString::Printf(TEXT("%s: LOD list or settings"), Path));
		if (Lods.IsEmpty())
		{
			continue;
		}

		// LOD 0 원본: 수와 위치 blob이 압축한 원본 메시 설명과 같고, 엔진 속성이 다 있으며, prepared가 있다.
		FMeshDescription Source;
		FRocksMeshSourceRecord Record;
		TArray<FRocksExportDiagnostic> Ignored;
		FString Problem;
		const bool bRecord = Mesh->CloneMeshDescription(0, Source) && (FRocksMeshDescriptionWriter::Compact(Source), FRocksMeshDescriptionWriter::FromMeshDescription(Source, Record, Ignored, Problem));
		const TSharedPtr<FJsonObject> Lod0 = Lods[0]->AsObject();
		const TSharedPtr<FJsonObject> SourceJson = Lod0->GetObjectField(TEXT("source"));
		const TSharedPtr<FJsonObject> Counts = SourceJson->GetObjectField(TEXT("counts"));
		Expect(Errors, bRecord && Lod0->GetBoolField(TEXT("hasSource")) && uint32(Counts->GetNumberField(TEXT("vertices"))) == Record.VertexCount
			&& uint32(Counts->GetNumberField(TEXT("triangles"))) == Record.TriangleCount && uint32(Counts->GetNumberField(TEXT("polygons"))) == Record.PolygonCount
			&& SourceJson->GetArrayField(TEXT("topology")).Num() == 7, FString::Printf(TEXT("%s: LOD 0 counts"), Path));

		TSet<FString> Names;
		for (const TSharedPtr<FJsonValue>& Item : SourceJson->GetArrayField(TEXT("attributes")))
		{
			const TSharedPtr<FJsonObject> Attribute = Item->AsObject();
			const FString Key = Attribute->GetStringField(TEXT("element")) + TEXT(".") + Attribute->GetStringField(TEXT("name"));
			Names.Add(Key);
			if (Key == TEXT("Vertex.Position"))
			{
				TArray<uint8> Written;
				const FRocksMeshAttribute* Expected = Record.Attributes.FindByPredicate([](const FRocksMeshAttribute& A) { return A.Element == ERocksMeshElement::Vertex && A.Name == TEXT("Position"); });
				const FString Hex = Attribute->GetObjectField(TEXT("data"))->GetStringField(TEXT("blob"));
				Expect(Errors, Expected != nullptr && LoadBytes(Output / FRocksBundleWriter::BlobFileForHex(Hex), Written) && int64(Written.Num()) == Expected->Bytes.Num()
					&& FMemory::Memcmp(Written.GetData(), Expected->Bytes.GetData(), Written.Num()) == 0, FString::Printf(TEXT("%s: position blob"), Path));
			}
		}

		for (const TCHAR* Required : {TEXT("Vertex.Position"), TEXT("Corner.Normal"), TEXT("Corner.Tangent"), TEXT("Corner.BinormalSign"), TEXT("Corner.Color"), TEXT("Corner.TexCoord"), TEXT("Edge.IsHard"), TEXT("PolygonGroup.MaterialSlotName")})
		{
			Expect(Errors, Names.Contains(Required), FString::Printf(TEXT("%s: attribute %s missing"), Path, Required));
		}

		Expect(Errors, Lod0->GetObjectField(TEXT("prepared"))->GetObjectField(TEXT("counts"))->GetNumberField(TEXT("vertices")) == double(Record.VertexCount)
			&& Lod0->GetObjectField(TEXT("preparedBuild"))->HasField(TEXT("DstLightmapIndex")), FString::Printf(TEXT("%s: prepared"), Path));
	}

	IFileManager::Get().DeleteDirectory(*Root, false, true);
}

void RocksExportSelfTest::RunSkeletalExtraction(TArray<FString>& Errors)
{
	using namespace RocksExportSelfTestPrivate;

	static const TCHAR* const MeshPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP.TutorialTPP");
	static const TCHAR* const SkeletonPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton.TutorialTPP_Skeleton");

	const auto Dump = [](UObject& Object)
	{
		FRocksPropertyDumper Dumper(&Object);
		FRocksJsonWriter Writer(FRocksJsonWriter::EStyle::Compact);
		Dumper.DumpObject(&Object, Writer);
		return Utf8ToString(Writer.Finish());
	};

	IAssetRegistry::GetChecked().SearchAllAssets(true);
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, MeshPath);
	USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, SkeletonPath);
	Expect(Errors, Mesh != nullptr && Skeleton != nullptr, TEXT("cannot load the tutorial skeletal mesh"));
	if (Mesh == nullptr || Skeleton == nullptr)
	{
		return;
	}

	// UE는 메시 설명을 처음 읽을 때 SourceModels 캐시를 채운다(설정 변경이 아니다). 먼저 읽어 둔 상태에서 비교한다.
	for (int32 Lod = 0; Lod < Mesh->GetLODNum(); ++Lod)
	{
		FMeshDescription Warm;
		Mesh->CloneMeshDescription(Lod, Warm);
	}

	const FString MeshBefore = Dump(*Mesh);
	const FString SkeletonBefore = Dump(*Skeleton);
	const FString Root = SelfTestDirectory() / TEXT("Skeletal");
	IFileManager::Get().DeleteDirectory(*Root, false, true);
	const FString Output = Root / TEXT("Skeletal.rxbundle");
	FRocksExportOptions Options;
	Options.OutputDirectory = Output;
	Options.bClosure = false;
	Options.Selections = {MeshPath, SkeletonPath};
	FRocksExportResult Result;
	Expect(Errors, FRocksExporter::Export(Options, Result), FString::Printf(TEXT("skeletal export: %s"), *Result.Error));
	const auto Difference = [](const FString& Before, const FString& After)
	{
		int32 At = 0;
		while (At < Before.Len() && At < After.Len() && Before[At] == After[At])
		{
			++At;
		}

		return FString::Printf(TEXT("at %d: before '%s' after '%s'"), At, *Before.Mid(FMath::Max(0, At - 120), 240), *After.Mid(FMath::Max(0, At - 120), 240));
	};

	const FString MeshAfter = Dump(*Mesh);
	const FString SkeletonAfter = Dump(*Skeleton);
	Expect(Errors, MeshAfter == MeshBefore, FString::Printf(TEXT("skeletal mesh extraction changed the asset %s"), *Difference(MeshBefore, MeshAfter)));
	Expect(Errors, SkeletonAfter == SkeletonBefore, FString::Printf(TEXT("skeleton extraction changed the asset %s"), *Difference(SkeletonBefore, SkeletonAfter)));

	TArray<uint8> Json;
	const TSharedPtr<FJsonObject> SkeletonJson = LoadBytes(Output / FRocksBundleWriter::AssetFileForPackage(FSoftObjectPath(SkeletonPath).GetLongPackageName()), Json) ? ParseJson(Json) : nullptr;
	const TSharedPtr<FJsonObject> MeshJson = LoadBytes(Output / FRocksBundleWriter::AssetFileForPackage(FSoftObjectPath(MeshPath).GetLongPackageName()), Json) ? ParseJson(Json) : nullptr;
	Expect(Errors, SkeletonJson.IsValid() && MeshJson.IsValid(), TEXT("skeletal JSON missing"));
	if (!SkeletonJson.IsValid() || !MeshJson.IsValid())
	{
		return;
	}

	// 본 계층: 수·이름·부모가 원본 계층(raw)과 같고 스켈레톤 본에는 리타깃 방식이 있다.
	const FReferenceSkeleton& SkeletonBones = Skeleton->GetReferenceSkeleton();
	const TArray<TSharedPtr<FJsonValue>>& Bones = SkeletonJson->GetObjectField(TEXT("data"))->GetArrayField(TEXT("bones"));
	bool bBonesMatch = Bones.Num() == SkeletonBones.GetRawBoneNum();
	for (int32 Bone = 0; bBonesMatch && Bone < Bones.Num(); ++Bone)
	{
		const TSharedPtr<FJsonObject> Info = Bones[Bone]->AsObject();
		bBonesMatch = Info->GetStringField(TEXT("name")) == SkeletonBones.GetRawRefBoneInfo()[Bone].Name.ToString()
			&& int32(Info->GetNumberField(TEXT("parent"))) == SkeletonBones.GetRawRefBoneInfo()[Bone].ParentIndex && Info->HasField(TEXT("retargetMode"));
	}

	Expect(Errors, bBonesMatch, TEXT("skeleton bones differ from the reference skeleton"));
	const TSharedPtr<FJsonObject> MeshData = MeshJson->GetObjectField(TEXT("data"));
	Expect(Errors, MeshData->GetArrayField(TEXT("bones")).Num() == Mesh->GetRefSkeleton().GetRawBoneNum()
		&& MeshData->GetArrayField(TEXT("lods")).Num() == Mesh->GetLODNum(), TEXT("mesh bones or LOD count"));

	// 머티리얼 슬롯(6.3절 13번): 수와 슬롯 이름이 GetMaterials()와 같다.
	const TArray<TSharedPtr<FJsonValue>>& Materials = MeshData->GetArrayField(TEXT("materials"));
	bool bMaterialsMatch = Materials.Num() == Mesh->GetMaterials().Num();
	for (int32 Slot = 0; bMaterialsMatch && Slot < Materials.Num(); ++Slot)
	{
		const TSharedPtr<FJsonObject> Info = Materials[Slot]->AsObject();
		bMaterialsMatch = Info->GetStringField(TEXT("MaterialSlotName")) == Mesh->GetMaterials()[Slot].MaterialSlotName.ToString()
			&& Info->HasField(TEXT("ImportedMaterialSlotName")) && Info->HasField(TEXT("MaterialInterface"));
	}

	Expect(Errors, bMaterialsMatch, TEXT("skeletal mesh material slots differ from GetMaterials()"));

	// LOD 0 원본의 스킨 가중치: 영향 수 합이 메시 설명과 같다(BoneInfluences 시작 표 마지막 값).
	FMeshDescription Source;
	const bool bSource = Mesh->CloneMeshDescription(0, Source);
	int64 Expected = 0;
	if (bSource)
	{
		const FSkeletalMeshAttributesShared Attributes(Source);
		const FSkinWeightsVertexAttributesConstRef Weights = Attributes.GetVertexSkinWeights();
		for (const FVertexID Vertex : Source.Vertices().GetElementIDs())
		{
			Expected += Weights.Get(Vertex).Num();
		}
	}

	int64 Written = -1;
	const TSharedPtr<FJsonObject> Lod0 = MeshData->GetArrayField(TEXT("lods"))[0]->AsObject()->GetObjectField(TEXT("source"));
	for (const TSharedPtr<FJsonValue>& Item : Lod0->GetArrayField(TEXT("attributes")))
	{
		const TSharedPtr<FJsonObject> Attribute = Item->AsObject();
		if (Attribute->GetStringField(TEXT("name")) != TEXT("SkinWeights") || Attribute->GetStringField(TEXT("type")) != TEXT("BoneInfluences"))
		{
			continue;
		}

		TArray<uint8> Blob;
		const TSharedPtr<FJsonObject> Ref = Attribute->GetObjectField(TEXT("data"));
		const int64 Count = int64(Ref->GetNumberField(TEXT("count")));
		if (LoadBytes(Output / FRocksBundleWriter::BlobFileForHex(Ref->GetStringField(TEXT("blob"))), Blob) && Blob.Num() >= (Count + 1) * 4)
		{
			uint32 Last = 0;
			FMemory::Memcpy(&Last, Blob.GetData() + Count * 4, sizeof(Last));
			Written = Last;
		}
	}

	Expect(Errors, bSource && Written == Expected, FString::Printf(TEXT("skin weight influences %lld, expected %lld"), Written, Expected));
	IFileManager::Get().DeleteDirectory(*Root, false, true);
}

void RocksExportSelfTest::RunAnimationExtraction(TArray<FString>& Errors)
{
	using namespace RocksExportSelfTestPrivate;

	static const TCHAR* const ClipPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Walk_Fwd.Tutorial_Walk_Fwd");
	IAssetRegistry::GetChecked().SearchAllAssets(true);
	UAnimSequence* Sequence = LoadObject<UAnimSequence>(nullptr, ClipPath);
	Expect(Errors, Sequence != nullptr && Sequence->GetDataModel() != nullptr, TEXT("cannot load the tutorial walk animation"));
	if (Sequence == nullptr || Sequence->GetDataModel() == nullptr)
	{
		return;
	}

	const auto Dump = [](UObject& Object)
	{
		FRocksPropertyDumper Dumper(&Object);
		FRocksJsonWriter Writer(FRocksJsonWriter::EStyle::Compact);
		Dumper.DumpObject(&Object, Writer);
		return Utf8ToString(Writer.Finish());
	};

	const IAnimationDataModel* Model = Sequence->GetDataModel();
	TArray<FName> TrackNames;
	Model->GetBoneTrackNames(TrackNames);
	const FString Before = Dump(*Sequence);
	const FString Root = SelfTestDirectory() / TEXT("Animation");
	IFileManager::Get().DeleteDirectory(*Root, false, true);
	const FString Output = Root / TEXT("Animation.rxbundle");
	FRocksExportOptions Options;
	Options.OutputDirectory = Output;
	Options.bClosure = false;
	Options.Selections = {ClipPath};
	FRocksExportResult Result;
	Expect(Errors, FRocksExporter::Export(Options, Result), FString::Printf(TEXT("animation export: %s"), *Result.Error));
	Expect(Errors, Dump(*Sequence) == Before, TEXT("animation extraction changed the asset"));

	TArray<uint8> Json;
	const TSharedPtr<FJsonObject> Asset = LoadBytes(Output / FRocksBundleWriter::AssetFileForPackage(FSoftObjectPath(ClipPath).GetLongPackageName()), Json) ? ParseJson(Json) : nullptr;
	Expect(Errors, Asset.IsValid(), TEXT("animation JSON missing"));
	if (!Asset.IsValid())
	{
		return;
	}

	const TSharedPtr<FJsonObject> Data = Asset->GetObjectField(TEXT("data"));
	Expect(Errors, !Asset->GetObjectField(TEXT("settings"))->HasField(TEXT("DataModelInterface")), TEXT("settings dump holds the data model"));
	Expect(Errors, int32(Data->GetNumberField(TEXT("frameCount"))) == Model->GetNumberOfFrames() && int32(Data->GetNumberField(TEXT("keyCount"))) == Model->GetNumberOfKeys()
		&& Data->GetObjectField(TEXT("frameRate"))->GetNumberField(TEXT("numerator")) == double(Model->GetFrameRate().Numerator), TEXT("animation frame counts or rate"));

	// 본 트랙: 이름·순서가 데이터 모델과 같고, 회전 blob이 UE 키 FTransform 회전의 float 변환과 bytes가 같다(키 1개로
	// 접혔으면 모든 키가 같다).
	const TArray<TSharedPtr<FJsonValue>>& Tracks = Data->GetArrayField(TEXT("boneTracks"));
	bool bTracks = Tracks.Num() == TrackNames.Num();
	for (int32 Track = 0; bTracks && Track < Tracks.Num(); ++Track)
	{
		const TSharedPtr<FJsonObject> Item = Tracks[Track]->AsObject();
		bTracks = Item->GetStringField(TEXT("bone")) == TrackNames[Track].ToString();
		TArray<FTransform> Keys;
		Model->GetBoneTrackTransforms(TrackNames[Track], Keys);
		const TSharedPtr<FJsonObject> Rotation = Item->GetObjectField(TEXT("rotation"));
		TArray<uint8> Blob;
		bTracks = bTracks && LoadBytes(Output / FRocksBundleWriter::BlobFileForHex(Rotation->GetStringField(TEXT("blob"))), Blob);
		const int32 Count = int32(Rotation->GetNumberField(TEXT("count")));
		for (int32 Key = 0; bTracks && Key < Keys.Num(); ++Key)
		{
			const FQuat4f Expected(Keys[Key].GetRotation());
			const float Values[] = {Expected.X, Expected.Y, Expected.Z, Expected.W};
			const int32 Stored = Count == 1 ? 0 : Key;
			bTracks = Blob.Num() >= (Stored + 1) * int32(sizeof(Values)) && FMemory::Memcmp(Blob.GetData() + Stored * sizeof(Values), Values, sizeof(Values)) == 0;
		}
	}

	Expect(Errors, bTracks, TEXT("animation bone tracks differ from the data model"));
	IFileManager::Get().DeleteDirectory(*Root, false, true);
}

void RocksExportSelfTest::RunMaterialExtraction(TArray<FString>& Errors)
{
	using namespace RocksExportSelfTestPrivate;

	static const TCHAR* const GridPath = TEXT("/Engine/EngineMaterials/M_Grid.M_Grid");
	static const TCHAR* const InstancePath = TEXT("/Engine/EngineMaterials/Widget3DPassThrough_Translucent_OneSided.Widget3DPassThrough_Translucent_OneSided");
	static const TCHAR* const DepthPath = TEXT("/Engine/ArtTools/RenderToTexture/Materials/M_RenderNormalizedDepth_01.M_RenderNormalizedDepth_01");
	static const TCHAR* const CollectionPath = TEXT("/Engine/ArtTools/RenderToTexture/Materials/MPC_R2T.MPC_R2T");
	static const TCHAR* const FunctionCallClass = TEXT("MaterialExpressionMaterialFunctionCall");
	IAssetRegistry::GetChecked().SearchAllAssets(true);
	UMaterial* Grid = LoadObject<UMaterial>(nullptr, GridPath);
	UMaterialInstanceConstant* Instance = LoadObject<UMaterialInstanceConstant>(nullptr, InstancePath);
	Expect(Errors, Grid != nullptr && Grid->GetEditorOnlyData() != nullptr && Instance != nullptr && Instance->Parent != nullptr,
		TEXT("cannot load the engine grid material or the widget material instance"));
	if (Grid == nullptr || Grid->GetEditorOnlyData() == nullptr || Instance == nullptr || Instance->Parent == nullptr)
	{
		return;
	}

	const auto Dump = [](UObject& Object)
	{
		FRocksPropertyDumper Dumper(&Object);
		FRocksJsonWriter Writer(FRocksJsonWriter::EStyle::Compact);
		Dumper.DumpObject(&Object, Writer);
		return Utf8ToString(Writer.Finish());
	};

	const FString Before = Dump(*Grid);
	const FString Root = SelfTestDirectory() / TEXT("Material");
	IFileManager::Get().DeleteDirectory(*Root, false, true);
	const FString Output = Root / TEXT("Material.rxbundle");
	FRocksExportOptions Options;
	Options.OutputDirectory = Output;
	Options.bClosure = false;
	Options.Selections = {GridPath, InstancePath, DepthPath};
	FRocksExportResult Result;
	Expect(Errors, FRocksExporter::Export(Options, Result), FString::Printf(TEXT("material export: %s"), *Result.Error));
	Expect(Errors, Dump(*Grid) == Before, TEXT("material extraction changed the asset"));

	const auto Load = [&Output](const TCHAR* Path, TArray<uint8>& Json)
	{
		return LoadBytes(Output / FRocksBundleWriter::AssetFileForPackage(FSoftObjectPath(Path).GetLongPackageName()), Json) ? ParseJson(Json) : nullptr;
	};

	const auto HasDependency = [](const TSharedPtr<FJsonObject>& Asset, const FString& Path)
	{
		for (const TSharedPtr<FJsonValue>& Dependency : Asset->GetArrayField(TEXT("dependencies")))
		{
			if (Dependency->AsObject()->GetStringField(TEXT("objectPath")) == Path)
			{
				return true;
			}
		}

		return false;
	};

	TArray<uint8> GridJson;
	TArray<uint8> InstanceJson;
	TArray<uint8> DepthJson;
	const TSharedPtr<FJsonObject> GridAsset = Load(GridPath, GridJson);
	const TSharedPtr<FJsonObject> InstanceAsset = Load(InstancePath, InstanceJson);
	const TSharedPtr<FJsonObject> DepthAsset = Load(DepthPath, DepthJson);
	Expect(Errors, GridAsset.IsValid() && InstanceAsset.IsValid() && DepthAsset.IsValid(), TEXT("material JSON missing"));
	if (!GridAsset.IsValid() || !InstanceAsset.IsValid() || !DepthAsset.IsValid())
	{
		return;
	}

	// 노드: expression 다음 comment, UE 배열 순서. 이름·클래스와 입력 연결(대상 노드 이름·출력 번호)이 UE 그래프와 같다.
	TArray<const UMaterialExpression*> Expected;
	for (const TObjectPtr<UMaterialExpression>& Expression : Grid->GetExpressions())
	{
		Expected.Add(Expression.Get());
	}

	for (const TObjectPtr<UMaterialExpressionComment>& Comment : Grid->GetEditorComments())
	{
		Expected.Add(Comment.Get());
	}

	const TSharedPtr<FJsonObject> GridData = GridAsset->GetObjectField(TEXT("data"));
	const TArray<TSharedPtr<FJsonValue>>& Nodes = GridData->GetArrayField(TEXT("nodes"));
	bool bNodes = Nodes.Num() == Expected.Num();
	bool bFunctions = true;
	TSet<FString> Names;
	for (int32 Index = 0; bNodes && Index < Nodes.Num(); ++Index)
	{
		const TSharedPtr<FJsonObject> Node = Nodes[Index]->AsObject();
		UMaterialExpression* Expression = const_cast<UMaterialExpression*>(Expected[Index]);
		bool bDuplicate = false;
		Names.Add(Node->GetStringField(TEXT("name")), &bDuplicate);
		bNodes = !bDuplicate && Node->GetStringField(TEXT("name")) == Expression->GetName() && Node->GetStringField(TEXT("class")) == Expression->GetClass()->GetName();
		const TArray<TSharedPtr<FJsonValue>>& Inputs = Node->GetArrayField(TEXT("inputs"));
		bNodes = bNodes && Inputs.Num() == Expression->CountInputs();
		for (int32 Slot = 0; bNodes && Slot < Inputs.Num(); ++Slot)
		{
			const FExpressionInput* Input = Expression->GetInput(Slot);
			const TSharedPtr<FJsonObject>* From = nullptr;
			const bool bLinked = Inputs[Slot]->AsObject()->TryGetObjectField(TEXT("from"), From);
			const bool bConnected = Input != nullptr && Input->Expression != nullptr;
			bNodes = bLinked == bConnected
				&& (!bConnected || ((*From)->GetStringField(TEXT("node")) == Input->Expression->GetName() && int32((*From)->GetNumberField(TEXT("output"))) == Input->OutputIndex));
		}

		if (Node->GetStringField(TEXT("class")) == FunctionCallClass)
		{
			const TSharedPtr<FJsonObject>* Function = nullptr;
			bFunctions = bFunctions && Node->GetObjectField(TEXT("properties"))->TryGetObjectField(TEXT("MaterialFunction"), Function)
				&& HasDependency(GridAsset, (*Function)->GetStringField(TEXT("objectPath")));
		}
	}

	Expect(Errors, bNodes, TEXT("material nodes or links differ from the UE graph"));
	Expect(Errors, bFunctions, TEXT("material function call without a dependency"));

	// 출력 핀: 개별 핀(bUseMaterialAttributes 꺼짐)이고 BaseColor 연결이 UE 핀과 같다.
	const FExpressionInput& BaseColor = Grid->GetEditorOnlyData()->BaseColor;
	bool bPins = !Grid->bUseMaterialAttributes && BaseColor.Expression != nullptr;
	bool bBaseColor = false;
	for (const TSharedPtr<FJsonValue>& Pin : GridData->GetArrayField(TEXT("outputs")))
	{
		const TSharedPtr<FJsonObject> Item = Pin->AsObject();
		const TSharedPtr<FJsonObject>* From = nullptr;
		bPins = bPins && Item->GetStringField(TEXT("name")) != TEXT("MaterialAttributes");
		if (bPins && Item->GetStringField(TEXT("name")) == TEXT("BaseColor"))
		{
			bBaseColor = Item->TryGetObjectField(TEXT("from"), From) && (*From)->GetStringField(TEXT("node")) == BaseColor.Expression->GetName();
		}
	}

	Expect(Errors, bPins && bBaseColor, TEXT("material output pins differ from the UE material"));

	// settings에는 노드·핀이 펼쳐지지 않는다(그래프는 data에만 있다).
	const FString GridText = Utf8ToString(GridJson);
	const int32 DataAt = GridText.Find(TEXT("\n  \"data\": "));
	const FString SettingsText = DataAt != INDEX_NONE ? GridText.Left(DataAt) : GridText;
	Expect(Errors, DataAt != INDEX_NONE && !SettingsText.Contains(TEXT("\"class\": \"/Script/Engine.MaterialExpression")) && !SettingsText.Contains(TEXT("\"BaseColor\"")),
		TEXT("material settings expand graph nodes or output pins"));

	// 컬렉션 참조 노드의 컬렉션이 dependencies에 있다.
	Expect(Errors, HasDependency(DepthAsset, CollectionPath), TEXT("material parameter collection missing from dependencies"));

	// 인스턴스: 부모 참조와 resolved(GetAllParametersOfType 전 종류의 합, scalar 값 일치).
	const TSharedPtr<FJsonObject>* Parent = nullptr;
	Expect(Errors, InstanceAsset->GetObjectField(TEXT("settings"))->TryGetObjectField(TEXT("Parent"), Parent) && (*Parent)->GetStringField(TEXT("objectPath")) == Instance->Parent->GetPathName()
		&& HasDependency(InstanceAsset, Instance->Parent->GetPathName()), TEXT("material instance parent reference"));
	int32 ParameterCount = 0;
	for (int32 Type = 0; Type < int32(EMaterialParameterType::Num); ++Type)
	{
		TMap<FMaterialParameterInfo, FMaterialParameterMetadata> Parameters;
		Instance->GetAllParametersOfType(EMaterialParameterType(Type), Parameters);
		ParameterCount += Parameters.Num();
	}

	const TArray<TSharedPtr<FJsonValue>>& Resolved = InstanceAsset->GetObjectField(TEXT("data"))->GetArrayField(TEXT("resolved"));
	bool bResolved = Resolved.Num() == ParameterCount && ParameterCount > 0;
	for (const TSharedPtr<FJsonValue>& Entry : Resolved)
	{
		const TSharedPtr<FJsonObject> Item = Entry->AsObject();
		if (Item->GetStringField(TEXT("type")) != TEXT("Scalar"))
		{
			continue;
		}

		float Value = 0.0f;
		const FName Name(*Item->GetObjectField(TEXT("parameter"))->GetStringField(TEXT("Name")));
		bResolved = bResolved && Instance->GetScalarParameterValue(FHashedMaterialParameterInfo(Name), Value) && float(Item->GetNumberField(TEXT("value"))) == Value;
	}

	Expect(Errors, bResolved, TEXT("material instance resolved parameters differ from GetAllParametersOfType"));
	IFileManager::Get().DeleteDirectory(*Root, false, true);
}

void RocksExportSelfTest::MarkNotRun(const FString& Reason)
{
	UE_LOG(LogRocksExport, Display, TEXT("%s"), *Reason);
	RocksExportSelfTestPrivate::NotRun.Add(Reason);
}

TArray<FString> RocksExportSelfTest::TakeNotRun()
{
	return MoveTemp(RocksExportSelfTestPrivate::NotRun);
}

void RocksExportSelfTest::RunSubstrateExtraction(TArray<FString>& Errors)
{
	using namespace RocksExportSelfTestPrivate;

	// Substrate가 꺼진 프로젝트에서는 돌지 않음을 따로 기록한다(통과로 세지 않는다 — 8.8절 6번).
	const IConsoleVariable* Substrate = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Substrate"));
	if (Substrate == nullptr || Substrate->GetInt() == 0)
	{
		MarkNotRun(TEXT("SubstrateExtraction: not run: Substrate disabled"));
		return;
	}

	static const TCHAR* const SubstratePath = TEXT("/Game/RxGoldenSubstrate/M_GoldenSubstrate.M_GoldenSubstrate");
	static const TCHAR* const AttributesPath = TEXT("/Game/RxGoldenSubstrate/M_GoldenSubstrateAttributes.M_GoldenSubstrateAttributes");
	static const TCHAR* const LegacyPath = TEXT("/Game/RxGoldenSubstrate/M_Golden.M_Golden");
	static const TCHAR* const FrontMaterialPin = TEXT("FrontMaterial");
	static const TCHAR* const BaseColorPin = TEXT("BaseColor");
	// UE 5.8 Slab BSDF의 입력 수다(MaterialExpressionSubstrate.cpp:1098-1173 GetInputName 0–17).
	static constexpr int32 SlabInputCount = 18;
	// golden 개별 핀 머티리얼의 Slab 수다(make_golden_content.py substrate — 둘을 VerticalLayering으로 쌓는다).
	static constexpr int32 GoldenSlabs = 2;
	IAssetRegistry::GetChecked().SearchAllAssets(true);
	UMaterial* Layered = LoadObject<UMaterial>(nullptr, SubstratePath);
	UMaterial* Attributes = LoadObject<UMaterial>(nullptr, AttributesPath);
	UMaterial* Legacy = LoadObject<UMaterial>(nullptr, LegacyPath);
	const bool bLoaded = Layered != nullptr && Attributes != nullptr && Legacy != nullptr && Layered->GetEditorOnlyData() != nullptr
		&& Attributes->GetEditorOnlyData() != nullptr && Legacy->GetEditorOnlyData() != nullptr;
	Expect(Errors, bLoaded, TEXT("Substrate golden content missing (run make_golden_content.py substrate)"));
	if (!bLoaded)
	{
		return;
	}

	// 5.8에는 숨은 그래프 재작성이 없다: FrontMaterial 없는 머티리얼은 로드 뒤에도 비어 있다(8.3절의 근거).
	Expect(Errors, !Legacy->GetEditorOnlyData()->FrontMaterial.IsConnected(), TEXT("loading rewrote the legacy material into a Substrate graph"));
	const UMaterialExpression* Front = Layered->GetEditorOnlyData()->FrontMaterial.Expression;
	const UMaterialExpression* LayeredBaseColor = Layered->GetEditorOnlyData()->BaseColor.Expression;
	const UMaterialExpression* AttributesFront = Attributes->GetEditorOnlyData()->FrontMaterial.Expression;
	Expect(Errors, Front != nullptr && Front->IsA<UMaterialExpressionSubstrateVerticalLayering>() && LayeredBaseColor != nullptr && AttributesFront != nullptr
		&& Attributes->bUseMaterialAttributes, TEXT("Substrate golden content has an unexpected FrontMaterial graph"));
	if (Front == nullptr || LayeredBaseColor == nullptr || AttributesFront == nullptr)
	{
		return;
	}

	const FString Root = SelfTestDirectory() / TEXT("Substrate");
	IFileManager::Get().DeleteDirectory(*Root, false, true);
	const FString Output = Root / TEXT("Substrate.rxbundle");
	FRocksExportOptions Options;
	Options.OutputDirectory = Output;
	Options.bClosure = false;
	Options.Selections = {SubstratePath, AttributesPath, LegacyPath};
	FRocksExportResult Result;
	Expect(Errors, FRocksExporter::Export(Options, Result), FString::Printf(TEXT("Substrate export: %s"), *Result.Error));

	TArray<uint8> BundleJson;
	const TSharedPtr<FJsonObject> BundleObject = LoadBytes(Output / TEXT("bundle.json"), BundleJson) ? ParseJson(BundleJson) : nullptr;
	const TSharedPtr<FJsonObject>* Settings = nullptr;
	bool bSubstrate = false;
	Expect(Errors, BundleObject.IsValid() && BundleObject->TryGetObjectField(TEXT("projectSettings"), Settings) && (*Settings)->TryGetBoolField(TEXT("substrate"), bSubstrate)
		&& bSubstrate, TEXT("bundle.json does not record projectSettings.substrate = true"));

	const auto Load = [&Output](const TCHAR* Path, TArray<uint8>& Json)
	{
		return LoadBytes(Output / FRocksBundleWriter::AssetFileForPackage(FSoftObjectPath(Path).GetLongPackageName()), Json) ? ParseJson(Json) : nullptr;
	};

	// data의 배열 필드다(없으면 null — 모양이 틀린 JSON에서 멈추지 않는다).
	const auto DataArray = [](const TSharedPtr<FJsonObject>& Asset, const TCHAR* Name)
	{
		const TSharedPtr<FJsonObject>* Data = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		return Asset.IsValid() && Asset->TryGetObjectField(TEXT("data"), Data) && (*Data)->TryGetArrayField(Name, Items) ? Items : nullptr;
	};

	// 출력 핀 이름 목록과 핀마다 연결 대상 노드 이름(연결이 없으면 빈 문자열)이다.
	const auto Pins = [&DataArray](const TSharedPtr<FJsonObject>& Asset, TArray<FString>& Names, TMap<FString, FString>& Links)
	{
		const TArray<TSharedPtr<FJsonValue>>* Outputs = DataArray(Asset, TEXT("outputs"));
		for (int32 Index = 0; Outputs != nullptr && Index < Outputs->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Item = (*Outputs)[Index]->AsObject();
			FString Name;
			const TSharedPtr<FJsonObject>* From = nullptr;
			FString Node;
			if (Item.IsValid() && Item->TryGetStringField(TEXT("name"), Name))
			{
				if (Item->TryGetObjectField(TEXT("from"), From))
				{
					(*From)->TryGetStringField(TEXT("node"), Node);
				}

				Names.Add(Name);
				Links.Add(Name, Node);
			}
		}
	};

	TArray<uint8> LayeredJson;
	TArray<uint8> AttributesJson;
	TArray<uint8> LegacyJson;
	const TSharedPtr<FJsonObject> LayeredAsset = Load(SubstratePath, LayeredJson);
	const TSharedPtr<FJsonObject> AttributesAsset = Load(AttributesPath, AttributesJson);
	const TSharedPtr<FJsonObject> LegacyAsset = Load(LegacyPath, LegacyJson);
	Expect(Errors, LayeredAsset.IsValid() && AttributesAsset.IsValid() && LegacyAsset.IsValid(), TEXT("Substrate material JSON missing"));
	if (!LayeredAsset.IsValid() || !AttributesAsset.IsValid() || !LegacyAsset.IsValid())
	{
		return;
	}

	// 개별 핀 머티리얼: FrontMaterial이 VerticalLayering 노드에, 읽지 않는 BaseColor가 UE 연결 노드에 이어진다.
	TArray<FString> LayeredPins;
	TMap<FString, FString> LayeredLinks;
	Pins(LayeredAsset, LayeredPins, LayeredLinks);
	Expect(Errors, LayeredLinks.FindRef(FrontMaterialPin) == Front->GetName() && LayeredLinks.FindRef(BaseColorPin) == LayeredBaseColor->GetName()
		&& !LayeredPins.Contains(TEXT("MaterialAttributes")), TEXT("Substrate material output pins differ from the UE material"));

	// 속성 집합 머티리얼: MaterialAttributes 다음 FrontMaterial 두 핀이다(8.8절 2번).
	TArray<FString> AttributesPins;
	TMap<FString, FString> AttributesLinks;
	Pins(AttributesAsset, AttributesPins, AttributesLinks);
	const TArray<FString> ExpectedAttributesPins = {TEXT("MaterialAttributes"), FrontMaterialPin};
	Expect(Errors, AttributesPins == ExpectedAttributesPins && AttributesLinks.FindRef(FrontMaterialPin) == AttributesFront->GetName(),
		FString::Printf(TEXT("attributes material pins are [%s], expected [MaterialAttributes, FrontMaterial] with the slab"), *FString::Join(AttributesPins, TEXT(", "))));

	// 옛 핀 머티리얼: FrontMaterial 핀은 쓰지만 연결이 없다.
	TArray<FString> LegacyPins;
	TMap<FString, FString> LegacyLinks;
	Pins(LegacyAsset, LegacyPins, LegacyLinks);
	Expect(Errors, LegacyPins.Contains(FrontMaterialPin) && LegacyLinks.FindRef(FrontMaterialPin).IsEmpty(), TEXT("legacy material FrontMaterial pin"));

	// Slab 노드: 입력이 18개이고 이름·순서가 UE GetInputName과 같다(Rocks 핀 별칭의 근거 — 8.8절 4번).
	int32 Slabs = 0;
	bool bSlabs = true;
	const TArray<TSharedPtr<FJsonValue>>* Nodes = DataArray(LayeredAsset, TEXT("nodes"));
	for (int32 NodeIndex = 0; Nodes != nullptr && NodeIndex < Nodes->Num(); ++NodeIndex)
	{
		const TSharedPtr<FJsonObject> Node = (*Nodes)[NodeIndex]->AsObject();
		FString NodeName;
		const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
		if (!Node.IsValid() || !Node->TryGetStringField(TEXT("name"), NodeName) || !Node->TryGetArrayField(TEXT("inputs"), Inputs))
		{
			bSlabs = false;
			continue;
		}

		UMaterialExpression* Expression = FindObject<UMaterialExpression>(Layered, *NodeName);
		if (Expression == nullptr || !Expression->IsA<UMaterialExpressionSubstrateSlabBSDF>())
		{
			continue;
		}

		++Slabs;
		bSlabs = bSlabs && Inputs->Num() == SlabInputCount && Expression->CountInputs() == SlabInputCount;
		for (int32 Slot = 0; bSlabs && Slot < Inputs->Num(); ++Slot)
		{
			FString InputName;
			bSlabs = (*Inputs)[Slot]->AsObject()->TryGetStringField(TEXT("name"), InputName) && InputName == Expression->GetInputName(Slot).ToString();
		}
	}

	Expect(Errors, bSlabs && Slabs == GoldenSlabs, FString::Printf(TEXT("Substrate slab inputs differ from GetInputName (%d slabs)"), Slabs));
	IFileManager::Get().DeleteDirectory(*Root, false, true);
}

void RocksExportSelfTest::RunMaterialBaking(TArray<FString>& Errors)
{
	using namespace RocksExportSelfTestPrivate;

	// 굽기는 렌더링이 되는 프로세스에서만 한다(8.9절 8번 — 통과로 세지 않는다).
	if (!FRocksMaterialBaker::CanBake())
	{
		MarkNotRun(TEXT("MaterialBaking: not run: rendering unavailable (pass -AllowCommandletRendering without -nullrhi)"));
		return;
	}

	static const TCHAR* const GoldenMaterialPath = TEXT("/Game/RxGolden/M_Golden.M_Golden");
	static const TCHAR* const GoldenInstancePath = TEXT("/Game/RxGolden/MI_Golden.MI_Golden");
	// 굽지 않는 영역의 엔진 머티리얼이다(LightFunction — BaseEngine.ini DefaultLightFunctionMaterialName).
	static const TCHAR* const LightFunctionPath = TEXT("/Engine/EngineMaterials/DefaultLightFunctionMaterial.DefaultLightFunctionMaterial");
	// 시험 굽기 크기다(작게 — HDR emissive는 줄지 않는다).
	static constexpr int32 TestBakeSize = 16;
	// 기대 8비트 값과의 허용 차다(GPU 반올림 — 8.9절 8번 ±1).
	static constexpr int32 ChannelTolerance = 1;
	static const TCHAR* const PropertyNames[] = {TEXT("BaseColor"), TEXT("Normal"), TEXT("Metallic"), TEXT("Roughness"), TEXT("Specular"),
		TEXT("EmissiveColor"), TEXT("Opacity"), TEXT("OpacityMask"), TEXT("AmbientOcclusion")};
	IAssetRegistry::GetChecked().SearchAllAssets(true);
	UMaterial* Material = LoadObject<UMaterial>(nullptr, GoldenMaterialPath);
	UMaterialInstanceConstant* Instance = LoadObject<UMaterialInstanceConstant>(nullptr, GoldenInstancePath);
	Expect(Errors, Material != nullptr && Instance != nullptr, TEXT("golden material content missing (run make_golden_content.py content)"));
	if (Material == nullptr || Instance == nullptr)
	{
		return;
	}

	const FString Root = SelfTestDirectory() / TEXT("Baking");
	IFileManager::Get().DeleteDirectory(*Root, false, true);
	const FString Output = Root / TEXT("Baking.rxbundle");
	FRocksExportOptions Options;
	Options.OutputDirectory = Output;
	Options.bClosure = false;
	Options.BakeSize = TestBakeSize;
	Options.Selections = {GoldenMaterialPath, GoldenInstancePath, LightFunctionPath};
	FRocksExportResult Result;
	Expect(Errors, FRocksExporter::Export(Options, Result), FString::Printf(TEXT("baking export: %s"), *Result.Error));

	// 굽기 뒤 모듈 전역 상태가 되돌려졌다(SetLinearBake(false)면 Metallic은 sRGB 굽기다).
	IMaterialBakingModule& Module = FModuleManager::Get().LoadModuleChecked<IMaterialBakingModule>("MaterialBaking");
	Expect(Errors, !Module.IsLinearBake(FMaterialPropertyEx(MP_Metallic)), TEXT("baking left the module in linear bake mode"));

	TArray<uint8> BundleJson;
	const TSharedPtr<FJsonObject> BundleObject = LoadBytes(Output / TEXT("bundle.json"), BundleJson) ? ParseJson(BundleJson) : nullptr;
	const TSharedPtr<FJsonObject>* Environment = nullptr;
	FString Rhi;
	Expect(Errors, BundleObject.IsValid() && BundleObject->TryGetObjectField(TEXT("environment"), Environment) && (*Environment)->TryGetStringField(TEXT("rhi"), Rhi)
		&& !Rhi.IsEmpty() && (*Environment)->HasTypedField<EJson::String>(TEXT("gpu")) && (*Environment)->HasTypedField<EJson::String>(TEXT("driver")),
		TEXT("bundle.json environment of a baked export"));

	const auto Load = [&Output](const TCHAR* Path, TArray<uint8>& Json)
	{
		return LoadBytes(Output / FRocksBundleWriter::AssetFileForPackage(FSoftObjectPath(Path).GetLongPackageName()), Json) ? ParseJson(Json) : nullptr;
	};

	// baked.properties 항목을 속성 이름으로 찾는다(순서도 함께 본다 — Order).
	struct FBaked
	{
		bool bLinear = false;
		int32 SizeX = 0;
		int32 SizeY = 0;
		FString Format;
		FString Gamma;
		TArray<uint8> Pixels;
	};

	const auto ReadBaked = [&Output](const TSharedPtr<FJsonObject>& Asset, TArray<FBaked>& Out, FString& Problem)
	{
		const TSharedPtr<FJsonObject>* Data = nullptr;
		const TSharedPtr<FJsonObject>* Baked = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		int32 BakeSize = 0;
		if (!Asset.IsValid() || !Asset->TryGetObjectField(TEXT("data"), Data) || !(*Data)->TryGetObjectField(TEXT("baked"), Baked)
			|| !(*Baked)->TryGetNumberField(TEXT("bakeSize"), BakeSize) || BakeSize != TestBakeSize || !(*Baked)->TryGetArrayField(TEXT("properties"), Properties)
			|| Properties->Num() != UE_ARRAY_COUNT(PropertyNames))
		{
			Problem = TEXT("baked shape");
			return false;
		}

		for (int32 Index = 0; Index < Properties->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Item = (*Properties)[Index]->AsObject();
			FBaked Entry;
			FString Name;
			const TSharedPtr<FJsonObject>* Source = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Layers = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Blocks = nullptr;
			if (!Item.IsValid() || !Item->TryGetStringField(TEXT("property"), Name) || Name != PropertyNames[Index] || !Item->TryGetBoolField(TEXT("linear"), Entry.bLinear)
				|| !Item->TryGetObjectField(TEXT("source"), Source) || !(*Source)->TryGetStringField(TEXT("format"), Entry.Format)
				|| !(*Source)->TryGetArrayField(TEXT("layers"), Layers) || Layers->Num() != 1 || !(*Source)->TryGetArrayField(TEXT("blocks"), Blocks) || Blocks->Num() != 1)
			{
				Problem = FString::Printf(TEXT("baked property %d"), Index);
				return false;
			}

			(*Layers)[0]->AsObject()->TryGetStringField(TEXT("gammaSpace"), Entry.Gamma);
			const TSharedPtr<FJsonObject> Block = (*Blocks)[0]->AsObject();
			const TArray<TSharedPtr<FJsonValue>>& Mips = Block->GetArrayField(TEXT("mips"));
			Block->TryGetNumberField(TEXT("sizeX"), Entry.SizeX);
			Block->TryGetNumberField(TEXT("sizeY"), Entry.SizeY);
			FString Hex;
			if (Mips.Num() != 1 || !Mips[0]->AsObject()->GetArrayField(TEXT("pixels"))[0]->AsObject()->TryGetStringField(TEXT("blob"), Hex)
				|| !FFileHelper::LoadFileToArray(Entry.Pixels, *(Output / FRocksBundleWriter::BlobFileForHex(Hex))))
			{
				Problem = FString::Printf(TEXT("baked property %s blob"), *Name);
				return false;
			}

			Out.Add(MoveTemp(Entry));
		}

		return true;
	};

	// 첫 texel의 R·G·B다(BGRA8 — 4.5절 2번).
	const auto FirstTexel = [](const FBaked& Entry)
	{
		return Entry.Pixels.Num() >= 4 ? FColor(Entry.Pixels[2], Entry.Pixels[1], Entry.Pixels[0], Entry.Pixels[3]) : FColor::Transparent;
	};

	const auto Near = [](uint8 Actual, uint8 Wanted)
	{
		return FMath::Abs(int32(Actual) - int32(Wanted)) <= ChannelTolerance;
	};

	// 기대 값: BaseColor는 sRGB 부호화(SetLinearBake(true)에서 BaseColor만 sRGB), 스칼라는 선형 8비트다.
	const FColor BaseColor = FLinearColor(0.5f, 0.4f, 0.3f).ToFColor(true);
	const uint8 MaterialRoughness = FLinearColor(0.6f, 0.0f, 0.0f).ToFColor(false).R;
	const uint8 InstanceRoughness = FLinearColor(0.25f, 0.0f, 0.0f).ToFColor(false).R;
	const auto CheckAsset = [&](const TCHAR* Path, uint8 Roughness)
	{
		TArray<uint8> Json;
		TArray<FBaked> Baked;
		FString Problem;
		if (!ReadBaked(Load(Path, Json), Baked, Problem))
		{
			Expect(Errors, false, FString::Printf(TEXT("%s: %s"), Path, *Problem));
			return;
		}

		const FBaked& Color = Baked[0];
		const FBaked& Rough = Baked[3];
		const FBaked& Emissive = Baked[5];
		const FColor Texel = FirstTexel(Color);
		Expect(Errors, Color.SizeX == 1 && Color.SizeY == 1 && !Color.bLinear && Color.Gamma == TEXT("sRGB") && Color.Format == TEXT("BGRA8")
			&& Near(Texel.R, BaseColor.R) && Near(Texel.G, BaseColor.G) && Near(Texel.B, BaseColor.B) && Texel.A == 255,
			FString::Printf(TEXT("%s: baked BaseColor %dx%d %s (%d, %d, %d), expected 1x1 sRGB (%d, %d, %d)"), Path, Color.SizeX, Color.SizeY, *Color.Gamma,
				Texel.R, Texel.G, Texel.B, BaseColor.R, BaseColor.G, BaseColor.B));
		Expect(Errors, Rough.SizeX == 1 && Rough.SizeY == 1 && Rough.bLinear && Near(FirstTexel(Rough).R, Roughness),
			FString::Printf(TEXT("%s: baked Roughness %dx%d value %d, expected 1x1 linear %d"), Path, Rough.SizeX, Rough.SizeY, FirstTexel(Rough).R, Roughness));
		Expect(Errors, Emissive.Format == TEXT("RGBA16F") && Emissive.bLinear && Emissive.SizeX == TestBakeSize && Emissive.SizeY == TestBakeSize
			&& Emissive.Pixels.Num() == TestBakeSize * TestBakeSize * int32(sizeof(FFloat16Color)),
			FString::Printf(TEXT("%s: baked EmissiveColor %s %dx%d"), Path, *Emissive.Format, Emissive.SizeX, Emissive.SizeY));
	};

	CheckAsset(GoldenMaterialPath, MaterialRoughness);
	CheckAsset(GoldenInstancePath, InstanceRoughness);

	// 굽지 않는 영역: baked 키가 없고 BakeSkipped info가 있다.
	TArray<uint8> LightJson;
	const TSharedPtr<FJsonObject> LightAsset = Load(LightFunctionPath, LightJson);
	const TSharedPtr<FJsonObject>* LightData = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Diagnostics = nullptr;
	bool bSkipped = false;
	if (LightAsset.IsValid() && LightAsset->TryGetArrayField(TEXT("diagnostics"), Diagnostics))
	{
		for (const TSharedPtr<FJsonValue>& Diagnostic : *Diagnostics)
		{
			FString Code;
			bSkipped = bSkipped || (Diagnostic->AsObject()->TryGetStringField(TEXT("code"), Code) && Code == TEXT("BakeSkipped"));
		}
	}

	Expect(Errors, LightAsset.IsValid() && LightAsset->TryGetObjectField(TEXT("data"), LightData) && !(*LightData)->HasField(TEXT("baked")) && bSkipped,
		TEXT("light function material should not be baked and should carry BakeSkipped"));
	IFileManager::Get().DeleteDirectory(*Root, false, true);
}

void RocksExportSelfTest::RunMeshWriterEdgeCases(TArray<FString>& Errors)
{
	using namespace RocksExportSelfTestPrivate;

	const auto HasCode = [](const TArray<FRocksExportDiagnostic>& Diagnostics, const TCHAR* Code)
	{
		return Diagnostics.ContainsByPredicate([Code](const FRocksExportDiagnostic& Item) { return Item.Code == Code; });
	};

	const auto FindAttribute = [](const FRocksMeshSourceRecord& Record, ERocksMeshElement Element, const TCHAR* Name)
	{
		return Record.Attributes.FindByPredicate([Element, Name](const FRocksMeshAttribute& Item) { return Item.Element == Element && Item.Name == Name; });
	};

	const auto ReadU32 = [](const TArray64<uint8>& Bytes, int64 Offset)
	{
		uint32 Value = 0;
		FMemory::Memcpy(&Value, Bytes.GetData() + Offset, sizeof(Value));
		return Value;
	};

	// 1. 퇴화 다각형: 둘레 0-1-1-2(정점 1이 이웃)와 삼각형 0-2-3. UE는 자기 변 (1, 1)을 만든다.
	{
		FMeshDescription Mesh;
		FStaticMeshAttributes Attributes(Mesh);
		Attributes.Register();
		TArray<FVertexID> Vertices;
		for (const FVector3f& Position : {FVector3f(0, 0, 0), FVector3f(1, 0, 0), FVector3f(1, 1, 0), FVector3f(0, 1, 0)})
		{
			const FVertexID Vertex = Mesh.CreateVertex();
			Attributes.GetVertexPositions()[Vertex] = Position;
			Vertices.Add(Vertex);
		}

		const FPolygonGroupID Group = Mesh.CreatePolygonGroup();
		TArray<FVertexInstanceID> Quad;
		for (const int32 Index : {0, 1, 1, 2})
		{
			Quad.Add(Mesh.CreateVertexInstance(Vertices[Index]));
		}

		TArray<FVertexInstanceID> Triangle;
		for (const int32 Index : {0, 2, 3})
		{
			Triangle.Add(Mesh.CreateVertexInstance(Vertices[Index]));
		}

		Mesh.CreatePolygon(Group, Quad);
		Mesh.CreatePolygon(Group, Triangle);
		const int32 UeEdges = Mesh.Edges().Num();
		FRocksMeshSourceRecord Record;
		TArray<FRocksExportDiagnostic> Diagnostics;
		FString Problem;
		const bool bWritten = FRocksMeshDescriptionWriter::FromMeshDescription(Mesh, Record, Diagnostics, Problem);
		bool bNoSelfEdge = true;
		for (uint32 Edge = 0; bWritten && Edge < Record.EdgeCount; ++Edge)
		{
			bNoSelfEdge = bNoSelfEdge && Record.EdgeVertices[Edge * 2] != Record.EdgeVertices[Edge * 2 + 1];
		}

		const FRocksMeshAttribute* Hard = bWritten ? FindAttribute(Record, ERocksMeshElement::Edge, TEXT("IsHard")) : nullptr;
		TArray<uint32> Perimeter;
		for (uint32 At = bWritten ? Record.PolygonStarts[0] : 0; bWritten && At < Record.PolygonStarts[1]; ++At)
		{
			Perimeter.Add(Record.PolygonCorners[At]);
		}

		TArray<uint32> Expected;
		for (const FVertexInstanceID Corner : Quad)
		{
			Expected.Add(uint32(Corner.GetValue()));
		}

		Perimeter.Sort();
		Expected.Sort();
		Expect(Errors, bWritten && Record.EdgeCount == uint32(UeEdges - 1) && bNoSelfEdge && Hard != nullptr && Hard->Bytes.Num() == int64(Record.EdgeCount)
			&& HasCode(Diagnostics, TEXT("DegenerateEdgesDropped")) && Perimeter == Expected,
			FString::Printf(TEXT("degenerate polygon record (%s)"), *Problem));
	}

	// 2. 정점 영향 12개 초과: 본 0..13에 가중치 14..1(UE float 가중치는 0–1이라 1/100로 준다). 상위 12개(본 0..11)만
	// 남고 합이 65535다.
	{
		static constexpr int32 SourceInfluences = 12;
		static constexpr int32 GivenInfluences = 14;
		static constexpr uint32 WeightTotal = 65535;
		static constexpr float WeightScale = 0.01f;
		FMeshDescription Mesh;
		FSkeletalMeshAttributes Attributes(Mesh);
		Attributes.Register();
		const FVertexID Vertex = Mesh.CreateVertex();
		TArray<UE::AnimationCore::FBoneWeight> Weights;
		for (int32 Bone = 0; Bone < GivenInfluences; ++Bone)
		{
			Weights.Add(UE::AnimationCore::FBoneWeight(FBoneIndexType(Bone), float(GivenInfluences - Bone) * WeightScale));
		}

		UE::AnimationCore::FBoneWeightsSettings Settings;
		Settings.SetMaxWeightCount(GivenInfluences);
		Attributes.GetVertexSkinWeights().Set(Vertex, Weights, Settings);
		FRocksMeshSourceRecord Record;
		TArray<FRocksExportDiagnostic> Diagnostics;
		FString Problem;
		const bool bWritten = FRocksMeshDescriptionWriter::FromMeshDescription(Mesh, Record, Diagnostics, Problem);
		const FRocksMeshAttribute* Skin = bWritten ? FindAttribute(Record, ERocksMeshElement::Vertex, TEXT("SkinWeights")) : nullptr;
		const int64 Table = 2 * int64(sizeof(uint32));
		bool bTrimmed = Skin != nullptr && Skin->Bytes.Num() == Table + SourceInfluences * 4 && ReadU32(Skin->Bytes, 0) == 0 && ReadU32(Skin->Bytes, 4) == SourceInfluences;
		uint32 Sum = 0;
		for (int32 Slot = 0; bTrimmed && Slot < SourceInfluences; ++Slot)
		{
			const uint8* Pair = Skin->Bytes.GetData() + Table + Slot * 4;
			const uint16 Bone = uint16(Pair[0] | (Pair[1] << 8));
			Sum += uint32(Pair[2] | (Pair[3] << 8));
			bTrimmed = Bone == uint16(Slot);
		}

		Expect(Errors, bTrimmed && Sum == WeightTotal && HasCode(Diagnostics, TEXT("TooManyInfluences")), FString::Printf(TEXT("influence trimming (%s)"), *Problem));
	}

	// 3. 정점 0개 메시: 수는 모두 0이고 위치 속성은 빈 값이다.
	{
		FMeshDescription Mesh;
		FStaticMeshAttributes(Mesh).Register();
		FRocksMeshSourceRecord Record;
		TArray<FRocksExportDiagnostic> Diagnostics;
		FString Problem;
		const bool bWritten = FRocksMeshDescriptionWriter::FromMeshDescription(Mesh, Record, Diagnostics, Problem);
		const FRocksMeshAttribute* Position = bWritten ? FindAttribute(Record, ERocksMeshElement::Vertex, TEXT("Position")) : nullptr;
		Expect(Errors, bWritten && Record.VertexCount == 0 && Record.CornerCount == 0 && Record.EdgeCount == 0 && Record.PolygonCount == 0 && Position != nullptr
			&& Position->Bytes.IsEmpty(), FString::Printf(TEXT("empty mesh record (%s)"), *Problem));
	}

	// 4. 다채널 Name 속성: 다각형 묶음 2개에 채널 2개. 값은 채널 우선(채널 0의 묶음 0·1, 채널 1의 묶음 0·1)이다.
	{
		static constexpr int32 NameChannels = 2;
		FMeshDescription Mesh;
		FStaticMeshAttributes(Mesh).Register();
		const FPolygonGroupID First = Mesh.CreatePolygonGroup();
		const FPolygonGroupID Second = Mesh.CreatePolygonGroup();
		Mesh.PolygonGroupAttributes().RegisterAttribute<FName>(TEXT("Tags"), NameChannels, NAME_None);
		TPolygonGroupAttributesRef<FName> Tags = Mesh.PolygonGroupAttributes().GetAttributesRef<FName>(TEXT("Tags"));
		Tags.Set(First, 0, TEXT("A"));
		Tags.Set(Second, 0, TEXT("Bc"));
		Tags.Set(First, 1, TEXT("D"));
		FRocksMeshSourceRecord Record;
		TArray<FRocksExportDiagnostic> Diagnostics;
		FString Problem;
		const bool bWritten = FRocksMeshDescriptionWriter::FromMeshDescription(Mesh, Record, Diagnostics, Problem);
		const FRocksMeshAttribute* Attribute = bWritten ? FindAttribute(Record, ERocksMeshElement::PolygonGroup, TEXT("Tags")) : nullptr;
		static const uint8 ExpectedBytes[] = {1, 0, 0, 0, 'A', 2, 0, 0, 0, 'B', 'c', 1, 0, 0, 0, 'D', 0, 0, 0, 0};
		Expect(Errors, Attribute != nullptr && Attribute->Type == ERocksMeshValueType::Name && Attribute->Channels == NameChannels
			&& Attribute->Bytes == TArray64<uint8>(ExpectedBytes, UE_ARRAY_COUNT(ExpectedBytes)), FString::Printf(TEXT("multi-channel name attribute (%s)"), *Problem));
	}
}

bool RocksExportSelfTest::RunAll(TArray<FString>& Errors)
{
	RunSha256(Errors);
	RunNumberFormat(Errors);
	RunJsonWriter(Errors);
	RunPropertyDump(Errors);
	RunBundleWriter(Errors);
	RunExportDeterminism(Errors);
	RunTextureExtraction(Errors);
	RunStaticMeshExtraction(Errors);
	RunSkeletalExtraction(Errors);
	RunAnimationExtraction(Errors);
	RunMaterialExtraction(Errors);
	RunSubstrateExtraction(Errors);
	RunMaterialBaking(Errors);
	RunMeshWriterEdgeCases(Errors);
	return Errors.IsEmpty();
}
