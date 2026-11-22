#include "RocksTextureExtractor.h"

#include "Engine/Texture.h"
#include "ImageCore.h"
#include "RocksAssetBody.h"
#include "RocksBundleWriter.h"
#include "RocksExportCommon.h"
#include "RocksJsonWriter.h"
#include "RocksPropertyDumper.h"

namespace RocksTextureExtractorPrivate
{
	// 2절 텍스처 교환 클래스다.
	static const TCHAR* const TextureClasses[] = {
		TEXT("Texture2D"),
		TEXT("TextureCube"),
		TEXT("Texture2DArray"),
		TEXT("TextureCubeArray"),
		TEXT("VolumeTexture"),
	};

	struct FFormatEntry
	{
		ETextureSourceFormat Format;
		const TCHAR* Name;
		const TCHAR* Layout;
		uint32 Components;
	};

	// 4.2절 원본 형식 9종과 blob 배치다(성분 순서는 UE 저장 순서 그대로 — BGRA8은 B·G·R·A).
	static const FFormatEntry Formats[] = {
		{TSF_G8, TEXT("G8"), TEXT("u8"), 1},
		{TSF_BGRA8, TEXT("BGRA8"), TEXT("u8"), 4},
		{TSF_BGRE8, TEXT("BGRE8"), TEXT("u8"), 4},
		{TSF_RGBA16, TEXT("RGBA16"), TEXT("u16"), 4},
		{TSF_RGBA16F, TEXT("RGBA16F"), TEXT("f16"), 4},
		{TSF_G16, TEXT("G16"), TEXT("u16"), 1},
		{TSF_RGBA32F, TEXT("RGBA32F"), TEXT("f32"), 4},
		{TSF_R16F, TEXT("R16F"), TEXT("f16"), 1},
		{TSF_R32F, TEXT("R32F"), TEXT("f32"), 1},
	};

	static const TCHAR* CompressionName(ETextureSourceCompressionFormat Compression)
	{
		switch (Compression)
		{
		case TSCF_PNG:
			return TEXT("PNG");
		case TSCF_JPEG:
			return TEXT("JPEG");
		case TSCF_UEJPEG:
			return TEXT("UEJPEG");
		case TSCF_UEDELTA:
			return TEXT("UEDELTA");
		default:
			return TEXT("None");
		}
	}

	static bool IsLossy(ETextureSourceCompressionFormat Compression)
	{
		return Compression == TSCF_JPEG || Compression == TSCF_UEJPEG;
	}

	static const TCHAR* GammaName(EGammaSpace Gamma)
	{
		switch (Gamma)
		{
		case EGammaSpace::Linear:
			return TEXT("Linear");
		case EGammaSpace::Pow22:
			return TEXT("Pow22");
		case EGammaSpace::sRGB:
			return TEXT("sRGB");
		default:
			return TEXT("Invalid");
		}
	}

	// 원본 표를 쓴다. 실패하면 false와 OutError(blob 쓰기) 또는 OutDiagnostic(에셋 수준)이다.
	static bool WriteSource(UTexture& Texture, FRocksBundleWriter& Bundle, FRocksJsonWriter& Writer, FRocksAssetBody& Body, FString& OutError)
	{
		FTextureSource& Source = Texture.Source;
		const int32 NumLayers = Source.GetNumLayers();
		TArray<FString> LayerFormats;
		TArray<FString> LayerLayouts;
		TArray<uint32> LayerComponents;
		for (int32 Layer = 0; Layer < NumLayers; ++Layer)
		{
			FString Name;
			FString Layout;
			uint32 Components = 0;
			if (!FRocksTextureExtractor::DescribeSourceFormat(Source.GetFormat(Layer), Name, Layout, Components))
			{
				Body.Diagnostics.Add({TEXT("UnsupportedSourceFormat"), RocksExport::SeverityError,
					FString::Printf(TEXT("source layer %d has format value %d, which the exchange format does not carry"), Layer, int32(Source.GetFormat(Layer)))});
				Writer.Null();
				return true;
			}

			LayerFormats.Add(Name);
			LayerLayouts.Add(Layout);
			LayerComponents.Add(Components);
		}

		// mip 사슬 전체를 한 번 푼다(4.1절). 각 mip은 이 버퍼의 view다.
		const FTextureSource::FMipData Mips = Source.GetMipData(nullptr);
		if (!Mips.IsValid())
		{
			Body.Diagnostics.Add({TEXT("SourceReadFailed"), RocksExport::SeverityError, TEXT("the source mip chain could not be read or decompressed")});
			Writer.Null();
			return true;
		}

		// 모든 mip이 있고 크기가 그 mip 정보와 맞는지 쓰기 전에 본다. 어긋나면 이 에셋만 실패다(4.5절 9번).
		for (int32 BlockIndex = 0; BlockIndex < Source.GetNumBlocks(); ++BlockIndex)
		{
			FTextureSourceBlock Block;
			Source.GetBlock(BlockIndex, Block);
			for (int32 Mip = 0; Mip < Block.NumMips; ++Mip)
			{
				for (int32 Layer = 0; Layer < NumLayers; ++Layer)
				{
					FImageInfo Info;
					const FSharedBuffer Pixels = Mips.GetMipDataWithInfo(BlockIndex, Layer, Mip, Info);
					if (Pixels.IsNull() || int64(Pixels.GetSize()) != Info.GetImageSizeBytes())
					{
						Body.Diagnostics.Add({TEXT("SourceReadFailed"), RocksExport::SeverityError,
							FString::Printf(TEXT("mip %d of block %d layer %d is missing from the decompressed source or has the wrong size"), Mip, BlockIndex, Layer)});
						Writer.Null();
						return true;
					}
				}
			}
		}

		const ETextureSourceCompressionFormat Compression = Source.GetSourceCompression();
		Writer.BeginObject();
		Writer.Key(TEXT("format"));
		Writer.String(LayerFormats[0]);
		Writer.Key(TEXT("compression"));
		Writer.String(CompressionName(Compression));
		Writer.Key(TEXT("lossyOrigin"));
		if (IsLossy(Compression))
		{
			Writer.String(TEXT("jpeg"));
			Body.Diagnostics.Add({TEXT("LossyOrigin"), RocksExport::SeverityInfo,
				TEXT("the source is stored as JPEG; decoded pixels are exported and Rocks import accepts them")});
		}
		else
		{
			Writer.Null();
		}

		Writer.Key(TEXT("longLat"));
		Writer.Bool(Source.IsLongLatCubemap());
		Writer.Key(TEXT("numLayers"));
		Writer.Int(NumLayers);
		Writer.Key(TEXT("numBlocks"));
		Writer.Int(Source.GetNumBlocks());

		Writer.Key(TEXT("layers"));
		Writer.BeginArray();
		for (int32 Layer = 0; Layer < NumLayers; ++Layer)
		{
			Writer.BeginObject();
			Writer.Key(TEXT("format"));
			Writer.String(LayerFormats[Layer]);
			Writer.Key(TEXT("gammaSpace"));
			Writer.String(GammaName(Source.GetGammaSpace(Layer)));
			Writer.EndObject();
		}

		Writer.EndArray();

		Writer.Key(TEXT("blocks"));
		Writer.BeginArray();
		for (int32 BlockIndex = 0; BlockIndex < Source.GetNumBlocks(); ++BlockIndex)
		{
			FTextureSourceBlock Block;
			Source.GetBlock(BlockIndex, Block);
			Writer.BeginObject();
			Writer.Key(TEXT("blockX"));
			Writer.Int(Block.BlockX);
			Writer.Key(TEXT("blockY"));
			Writer.Int(Block.BlockY);
			Writer.Key(TEXT("sizeX"));
			Writer.Int(Block.SizeX);
			Writer.Key(TEXT("sizeY"));
			Writer.Int(Block.SizeY);
			Writer.Key(TEXT("numSlices"));
			Writer.Int(Block.NumSlices);
			Writer.Key(TEXT("numMips"));
			Writer.Int(Block.NumMips);
			Writer.Key(TEXT("mips"));
			Writer.BeginArray();
			for (int32 Mip = 0; Mip < Block.NumMips; ++Mip)
			{
				Writer.BeginObject();
				for (int32 Layer = 0; Layer < NumLayers; ++Layer)
				{
					FImageInfo Info;
					// 위에서 확인했다.
					const FSharedBuffer Pixels = Mips.GetMipDataWithInfo(BlockIndex, Layer, Mip, Info);
					if (Layer == 0)
					{
						Writer.Key(TEXT("sizeX"));
						Writer.Int(Info.SizeX);
						Writer.Key(TEXT("sizeY"));
						Writer.Int(Info.SizeY);
						Writer.Key(TEXT("numSlices"));
						Writer.Int(Info.NumSlices);
						Writer.Key(TEXT("pixels"));
						Writer.BeginArray();
					}

					FRocksBlobRef Ref;
					const uint64 Texels = uint64(Info.SizeX) * uint64(Info.SizeY) * uint64(Info.NumSlices);
					const TConstArrayView64<uint8> Bytes(static_cast<const uint8*>(Pixels.GetData()), int64(Pixels.GetSize()));
					if (!Bundle.WriteBlob(Bytes, LayerLayouts[Layer], LayerComponents[Layer], Texels, Ref, OutError))
					{
						OutError = FString::Printf(TEXT("%s: %s"), *Texture.GetPathName(), *OutError);
						return false;
					}

					Ref.WriteTo(Writer);
				}

				Writer.EndArray();
				Writer.EndObject();
			}

			Writer.EndArray();
			Writer.EndObject();
		}

		Writer.EndArray();
		Writer.EndObject();
		return true;
	}
}

bool FRocksTextureExtractor::IsTextureClass(const FString& ExchangeClass)
{
	for (const TCHAR* Name : RocksTextureExtractorPrivate::TextureClasses)
	{
		if (ExchangeClass.Equals(Name, ESearchCase::CaseSensitive))
		{
			return true;
		}
	}

	return false;
}

bool FRocksTextureExtractor::DescribeSourceFormat(ETextureSourceFormat Format, FString& OutName, FString& OutLayout, uint32& OutComponents)
{
	for (const RocksTextureExtractorPrivate::FFormatEntry& Entry : RocksTextureExtractorPrivate::Formats)
	{
		if (Entry.Format == Format)
		{
			OutName = Entry.Name;
			OutLayout = Entry.Layout;
			OutComponents = Entry.Components;
			return true;
		}
	}

	return false;
}

bool FRocksTextureExtractor::Extract(UTexture& Texture, FRocksBundleWriter& Bundle, FRocksAssetBody& OutBody, FString& OutError)
{
	// settings: 덤프(11.5절). 원본은 data에 쓰므로 Source 프로퍼티를 뺀다.
	FRocksPropertyDumper Dumper(&Texture);
	Dumper.ExcludeProperty(FindFProperty<FProperty>(UTexture::StaticClass(), GET_MEMBER_NAME_CHECKED(UTexture, Source)));
	FRocksJsonWriter Settings(FRocksJsonWriter::EStyle::Pretty, 1);
	Dumper.DumpObject(&Texture, Settings);
	OutBody.Settings = Settings.FinishFragment();
	OutBody.References = Dumper.GetReferences();

	FRocksJsonWriter Data(FRocksJsonWriter::EStyle::Pretty, 1);
	Data.BeginObject();
	Data.Key(TEXT("source"));
	if (!Texture.Source.IsValid())
	{
		OutBody.Diagnostics.Add({TEXT("NoSourceData"), RocksExport::SeverityError, TEXT("the texture has no source data (dynamic or cooked-only texture)")});
		Data.Null();
	}
	else if (!RocksTextureExtractorPrivate::WriteSource(Texture, Bundle, Data, OutBody, OutError))
	{
		return false;
	}

	Data.EndObject();
	OutBody.Data = Data.FinishFragment();
	return true;
}
