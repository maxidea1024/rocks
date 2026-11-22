#include "RocksExportSelfTest.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Texture.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RocksBundleWriter.h"
#include "RocksExportClosure.h"
#include "RocksExportSelfTestTypes.h"
#include "RocksExporter.h"
#include "RocksJsonWriter.h"
#include "RocksPropertyDumper.h"
#include "RocksSha256.h"
#include "RocksTextureExtractor.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include <charconv>
#include <clocale>

namespace RocksExportSelfTestPrivate
{
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
		TEXT("\"SoftReference\":{\"objectPath\":\"/Engine/BasicShapes/Sphere.Sphere\"},\"Fixed\":[5,6]}");
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

bool RocksExportSelfTest::RunAll(TArray<FString>& Errors)
{
	RunSha256(Errors);
	RunNumberFormat(Errors);
	RunJsonWriter(Errors);
	RunPropertyDump(Errors);
	RunBundleWriter(Errors);
	RunExportDeterminism(Errors);
	RunTextureExtraction(Errors);
	return Errors.IsEmpty();
}
