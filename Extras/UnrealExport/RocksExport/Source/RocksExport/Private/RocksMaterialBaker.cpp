#include "RocksMaterialBaker.h"

#include "DynamicRHI.h"
#include "IMaterialBakingModule.h"
#include "MaterialBakingStructures.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Misc/App.h"
#include "Modules/ModuleManager.h"
#include "RHIGlobals.h"
#include "RocksAssetBody.h"
#include "RocksBundleWriter.h"
#include "RocksExportCommon.h"
#include "RocksJsonWriter.h"
#include "RocksTextureExtractor.h"

namespace RocksMaterialBakerPrivate
{
	// 굽는 속성과 그 교환 이름이다(8.9절 1번 — 8.4절 목록 순서, UE MP_ 이름에서 접두를 뗀 것).
	struct FBakedProperty
	{
		EMaterialProperty Property;
		const TCHAR* Name;
	};

	static const FBakedProperty BakedProperties[] = {
		{MP_BaseColor, TEXT("BaseColor")},
		{MP_Normal, TEXT("Normal")},
		{MP_Metallic, TEXT("Metallic")},
		{MP_Roughness, TEXT("Roughness")},
		{MP_Specular, TEXT("Specular")},
		{MP_EmissiveColor, TEXT("EmissiveColor")},
		{MP_Opacity, TEXT("Opacity")},
		{MP_OpacityMask, TEXT("OpacityMask")},
		{MP_AmbientOcclusion, TEXT("AmbientOcclusion")},
	};

	// 8비트 결과의 알파다(굽기 결과 알파는 0이다 — GLTFExporter bFillAlpha와 같이 채운다).
	static constexpr uint8 OpaqueAlpha = 255;

	// 층 감마 이름이다(4.5절 1번 gammaSpace).
	static const TCHAR* const LinearGamma = TEXT("Linear");
	static const TCHAR* const SrgbGamma = TEXT("sRGB");

	// 원본 보관 형식 이름이다(굽기 결과는 푼 픽셀 그대로다).
	static const TCHAR* const NoCompression = TEXT("None");

	// 4.5절 1번 텍스처 원본(층·블록·mip 각 1개) 하나를 쓴다. blob 쓰기 실패만 false다.
	static bool WriteSource(ETextureSourceFormat Format, bool bLinear, FIntPoint Size, TConstArrayView64<uint8> Bytes, FRocksBundleWriter& Bundle,
		FRocksJsonWriter& Writer, FString& OutError)
	{
		FString FormatName;
		FString Layout;
		uint32 Components = 0;
		// 굽기는 BGRA8·RGBA16F만 만든다. 둘 다 교환 형식 표에 있다(4.2절).
		verify(FRocksTextureExtractor::DescribeSourceFormat(Format, FormatName, Layout, Components));

		Writer.BeginObject();
		Writer.Key(TEXT("format"));
		Writer.String(FormatName);
		Writer.Key(TEXT("compression"));
		Writer.String(NoCompression);
		Writer.Key(TEXT("lossyOrigin"));
		Writer.Null();
		Writer.Key(TEXT("longLat"));
		Writer.Bool(false);
		Writer.Key(TEXT("numLayers"));
		Writer.Int(1);
		Writer.Key(TEXT("numBlocks"));
		Writer.Int(1);
		Writer.Key(TEXT("layers"));
		Writer.BeginArray();
		Writer.BeginObject();
		Writer.Key(TEXT("format"));
		Writer.String(FormatName);
		Writer.Key(TEXT("gammaSpace"));
		Writer.String(bLinear ? LinearGamma : SrgbGamma);
		Writer.EndObject();
		Writer.EndArray();
		Writer.Key(TEXT("blocks"));
		Writer.BeginArray();
		Writer.BeginObject();
		Writer.Key(TEXT("blockX"));
		Writer.Int(0);
		Writer.Key(TEXT("blockY"));
		Writer.Int(0);
		Writer.Key(TEXT("sizeX"));
		Writer.Int(Size.X);
		Writer.Key(TEXT("sizeY"));
		Writer.Int(Size.Y);
		Writer.Key(TEXT("numSlices"));
		Writer.Int(1);
		Writer.Key(TEXT("numMips"));
		Writer.Int(1);
		Writer.Key(TEXT("mips"));
		Writer.BeginArray();
		Writer.BeginObject();
		Writer.Key(TEXT("sizeX"));
		Writer.Int(Size.X);
		Writer.Key(TEXT("sizeY"));
		Writer.Int(Size.Y);
		Writer.Key(TEXT("numSlices"));
		Writer.Int(1);
		Writer.Key(TEXT("pixels"));
		Writer.BeginArray();
		FRocksBlobRef Ref;
		if (!Bundle.WriteBlob(Bytes, Layout, Components, uint64(Size.X) * uint64(Size.Y), Ref, OutError))
		{
			return false;
		}

		Ref.WriteTo(Writer);
		Writer.EndArray();
		Writer.EndObject();
		Writer.EndArray();
		Writer.EndObject();
		Writer.EndArray();
		Writer.EndObject();
		return true;
	}

	// 결과 배열이 크기와 맞는가다(8.9절 1번 실패 판정 — 한 변이 0이거나 길이가 가로 × 세로와 다르면 실패).
	static bool HasSize(int64 Length, FIntPoint Size)
	{
		return Size.X > 0 && Size.Y > 0 && Length == int64(Size.X) * int64(Size.Y);
	}

	// 굽기 동안 모듈 전역 상태를 바꾸고 끝에 되돌린다(GLTFExporter와 같은 값 — GLTFMaterialUtilities.cpp:157-160).
	struct FBakeModuleState
	{
		explicit FBakeModuleState(IMaterialBakingModule& InModule)
			: Module(InModule)
		{
			Module.SetLinearBake(true);
			Module.SetEmissiveHDR(true);
		}

		~FBakeModuleState()
		{
			Module.SetLinearBake(false);
			Module.SetEmissiveHDR(false);
		}

		IMaterialBakingModule& Module;
	};
}

bool FRocksMaterialBaker::CanBake()
{
	return FApp::CanEverRender();
}

bool FRocksMaterialBaker::WriteBaked(UMaterialInterface& Material, int32 BakeSize, FRocksBundleWriter& Bundle, FRocksJsonWriter& Writer,
	FRocksAssetBody& Body, FString& OutError)
{
	using namespace RocksMaterialBakerPrivate;

	const UMaterial* Base = Material.GetMaterial();
	const EMaterialDomain Domain = Base != nullptr ? EMaterialDomain(Base->MaterialDomain) : MD_Surface;
	if (Domain != MD_Surface)
	{
		Body.Diagnostics.Add({TEXT("BakeSkipped"), RocksExport::SeverityInfo,
			FString::Printf(TEXT("material domain %s is not baked (UV space baking only fits the surface domain)"), *UEnum::GetValueAsString(Domain))});
		return true;
	}

	FMaterialDataEx MaterialData;
	MaterialData.Material = &Material;
	MaterialData.bTangentSpaceNormal = true;
	for (const FBakedProperty& Entry : BakedProperties)
	{
		MaterialData.PropertySizes.Add(FMaterialPropertyEx(Entry.Property), FIntPoint(BakeSize, BakeSize));
	}

	// 메시 데이터 없이 기본 quad로 굽는다(MaterialBakingStructures.h:118-121).
	FMeshData MeshData;
	TArray<FMaterialDataEx*> MaterialSettings = {&MaterialData};
	TArray<FMeshData*> MeshSettings = {&MeshData};
	TArray<FBakeOutputEx> Outputs;
	IMaterialBakingModule& Module = FModuleManager::Get().LoadModuleChecked<IMaterialBakingModule>("MaterialBaking");
	{
		const FBakeModuleState State(Module);
		Module.BakeMaterials(MaterialSettings, MeshSettings, Outputs);
	}

	const auto Fail = [&Body, &Writer](const FString& Message)
	{
		Body.Diagnostics.Add({TEXT("BakeFailed"), RocksExport::SeverityError, Message});
		Writer.Key(TEXT("baked"));
		Writer.Null();
		return true;
	};

	if (Outputs.Num() != 1)
	{
		return Fail(FString::Printf(TEXT("the baking module returned %d outputs for one material"), Outputs.Num()));
	}

	// 결과를 모두 확인한 뒤에 쓴다(실패면 baked는 null 하나다).
	const FBakeOutputEx& Output = Outputs[0];
	for (const FBakedProperty& Entry : BakedProperties)
	{
		const FMaterialPropertyEx Key(Entry.Property);
		if (Entry.Property == MP_EmissiveColor)
		{
			// HDR 배열은 shrinking을 받지 않아 요청 크기 그대로다(MaterialBakingModule.cpp:980-1012).
			const TArray<FFloat16Color>* Pixels = Output.HDRPropertyData.Find(Key);
			if (Pixels == nullptr || !HasSize(Pixels->Num(), FIntPoint(BakeSize, BakeSize)))
			{
				return Fail(FString::Printf(TEXT("%s HDR result is missing or does not match the requested size"), Entry.Name));
			}

			continue;
		}

		const TArray<FColor>* Pixels = Output.PropertyData.Find(Key);
		const FIntPoint* Size = Output.PropertySizes.Find(Key);
		if (Pixels == nullptr || Size == nullptr || !HasSize(Pixels->Num(), *Size))
		{
			return Fail(FString::Printf(TEXT("%s result is missing or does not match its size"), Entry.Name));
		}
	}

	Writer.Key(TEXT("baked"));
	Writer.BeginObject();
	Writer.Key(TEXT("bakeSize"));
	Writer.Int(BakeSize);
	Writer.Key(TEXT("properties"));
	Writer.BeginArray();
	for (const FBakedProperty& Entry : BakedProperties)
	{
		const FMaterialPropertyEx Key(Entry.Property);
		const bool* Linear = Output.PropertyIsLinearColor.Find(Key);
		// EmissiveColor는 HDR이라 언제나 선형이다(SetLinearBake(true) — MaterialBakingModule.cpp:1136-1146).
		const bool bLinear = Entry.Property == MP_EmissiveColor || (Linear != nullptr && *Linear);
		Writer.BeginObject();
		Writer.Key(TEXT("property"));
		Writer.String(Entry.Name);
		Writer.Key(TEXT("linear"));
		Writer.Bool(bLinear);
		Writer.Key(TEXT("source"));
		bool bWritten = false;
		if (Entry.Property == MP_EmissiveColor)
		{
			const TArray<FFloat16Color>& Pixels = Output.HDRPropertyData.FindChecked(Key);
			const TConstArrayView64<uint8> Bytes(reinterpret_cast<const uint8*>(Pixels.GetData()), int64(Pixels.Num()) * int64(sizeof(FFloat16Color)));
			bWritten = WriteSource(TSF_RGBA16F, bLinear, FIntPoint(BakeSize, BakeSize), Bytes, Bundle, Writer, OutError);
		}
		else
		{
			TArray<FColor> Pixels = Output.PropertyData.FindChecked(Key);
			for (FColor& Pixel : Pixels)
			{
				Pixel.A = OpaqueAlpha;
			}

			// FColor 메모리 순서는 B·G·R·A라 교환 BGRA8 배치와 같다(4.5절 2번).
			const TConstArrayView64<uint8> Bytes(reinterpret_cast<const uint8*>(Pixels.GetData()), int64(Pixels.Num()) * int64(sizeof(FColor)));
			bWritten = WriteSource(TSF_BGRA8, bLinear, Output.PropertySizes.FindChecked(Key), Bytes, Bundle, Writer, OutError);
		}

		if (!bWritten)
		{
			OutError = FString::Printf(TEXT("%s: %s"), *Material.GetPathName(), *OutError);
			return false;
		}

		Writer.EndObject();
	}

	Writer.EndArray();
	Writer.EndObject();
	return true;
}

void FRocksMaterialBaker::WriteEnvironment(FRocksJsonWriter& Writer)
{
	Writer.BeginObject();
	Writer.Key(TEXT("rhi"));
	Writer.String(GDynamicRHI != nullptr ? FString(GDynamicRHI->GetName()) : FString());
	Writer.Key(TEXT("gpu"));
	Writer.String(GRHIAdapterName);
	Writer.Key(TEXT("driver"));
	Writer.String(GRHIAdapterUserDriverVersion);
	Writer.EndObject();
}
