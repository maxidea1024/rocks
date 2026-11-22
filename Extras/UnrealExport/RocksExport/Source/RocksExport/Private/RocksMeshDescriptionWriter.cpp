#include "RocksMeshDescriptionWriter.h"

#include "MeshAttributes.h"
#include "MeshDescription.h"
#include "RocksBundleWriter.h"
#include "RocksExportClosure.h"
#include "RocksExportCommon.h"
#include "RocksJsonWriter.h"
#include "RocksSha256.h"
#include "Animation/MorphTarget.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "SkeletalMeshAttributes.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"

namespace RocksMeshDescriptionWriterPrivate
{
	// Rocks 코너 TexCoord 채널 상한(MeshMaxTexCoords = UE MAX_MESH_TEXTURE_COORDS_MD)과 속성 채널 상한(uint8)이다.
	static constexpr int32 MaxTexCoords = 8;
	static constexpr int32 MaxChannels = 255;

	// 위상 일곱 배열(MeshAnimationAssets 9.1절)의 원소당 uint32 수다.
	static constexpr uint32 EdgeWords = 2;
	static constexpr uint32 TriangleWords = 3;

	struct FMapping
	{
		ERocksMeshElement Element;
		FName UeName;
		const TCHAR* RocksName;
		ERocksMeshValueType Type;
	};

	// MeshAnimationAssets 3.2절 표의 UE → Rocks 대응이다. UE 이름은 UE 상수를 쓴다(정점 위치 이름은 끝에 공백이 있는
	// "Position "이다 — MeshAttributes.cpp). UE 전역 FName을 쓰므로 처음 부를 때 만든다.
	static const TArray<FMapping>& Mappings()
	{
		static const TArray<FMapping> Table = {
			{ERocksMeshElement::Vertex, MeshAttribute::Vertex::Position, TEXT("Position"), ERocksMeshValueType::Vec3},
			{ERocksMeshElement::Corner, MeshAttribute::VertexInstance::Normal, TEXT("Normal"), ERocksMeshValueType::Vec3},
			{ERocksMeshElement::Corner, MeshAttribute::VertexInstance::Tangent, TEXT("Tangent"), ERocksMeshValueType::Vec3},
			{ERocksMeshElement::Corner, MeshAttribute::VertexInstance::BinormalSign, TEXT("BinormalSign"), ERocksMeshValueType::Float},
			{ERocksMeshElement::Corner, MeshAttribute::VertexInstance::Color, TEXT("Color"), ERocksMeshValueType::Vec4},
			{ERocksMeshElement::Corner, MeshAttribute::VertexInstance::TextureCoordinate, TEXT("TexCoord"), ERocksMeshValueType::Vec2},
			{ERocksMeshElement::Edge, MeshAttribute::Edge::IsHard, TEXT("IsHard"), ERocksMeshValueType::Bool},
			{ERocksMeshElement::Polygon, MeshAttribute::Polygon::ObjectName, TEXT("PartName"), ERocksMeshValueType::Name},
			{ERocksMeshElement::PolygonGroup, MeshAttribute::PolygonGroup::ImportedMaterialSlotName, TEXT("MaterialSlotName"), ERocksMeshValueType::Name},
		};
		return Table;
	}

	// Rocks 엔진 이름(3.2절)과 접두다. 보존 속성이 이 이름이면 Rocks 검증이 엔진 형태를 요구하므로 쓰지 않는다.
	static const TCHAR* const EngineNames[] = {
		TEXT("Position"), TEXT("Normal"), TEXT("Tangent"), TEXT("BinormalSign"), TEXT("Color"), TEXT("TexCoord"),
		TEXT("IsHard"), TEXT("PartName"), TEXT("MaterialSlotName"), TEXT("SkinWeights"),
	};

	static const TCHAR* const EnginePrefixes[] = {
		TEXT("SkinWeights-"), TEXT("Morph-"), TEXT("MorphNormal-"),
	};

	static const TCHAR* ElementName(ERocksMeshElement Element)
	{
		switch (Element)
		{
		case ERocksMeshElement::Vertex:
			return TEXT("Vertex");
		case ERocksMeshElement::Corner:
			return TEXT("Corner");
		case ERocksMeshElement::Edge:
			return TEXT("Edge");
		case ERocksMeshElement::Polygon:
			return TEXT("Polygon");
		case ERocksMeshElement::Triangle:
			return TEXT("Triangle");
		default:
			return TEXT("PolygonGroup");
		}
	}

	static const TCHAR* TypeName(ERocksMeshValueType Type)
	{
		switch (Type)
		{
		case ERocksMeshValueType::Float:
			return TEXT("Float");
		case ERocksMeshValueType::Vec2:
			return TEXT("Vec2");
		case ERocksMeshValueType::Vec3:
			return TEXT("Vec3");
		case ERocksMeshValueType::Vec4:
			return TEXT("Vec4");
		case ERocksMeshValueType::UInt32:
			return TEXT("UInt32");
		case ERocksMeshValueType::Bool:
			return TEXT("Bool");
		case ERocksMeshValueType::BoneInfluences:
			return TEXT("BoneInfluences");
		case ERocksMeshValueType::SparseVec3:
			return TEXT("SparseVec3");
		default:
			return TEXT("Name");
		}
	}

	// 값 하나의 성분 수(blob components 계산용)와 배치 이름이다.
	static void DescribeType(ERocksMeshValueType Type, uint32& OutComponents, const TCHAR*& OutLayout)
	{
		switch (Type)
		{
		case ERocksMeshValueType::Vec2:
			OutComponents = 2;
			OutLayout = TEXT("f32");
			return;
		case ERocksMeshValueType::Vec3:
			OutComponents = 3;
			OutLayout = TEXT("f32");
			return;
		case ERocksMeshValueType::Vec4:
			OutComponents = 4;
			OutLayout = TEXT("f32");
			return;
		case ERocksMeshValueType::UInt32:
			OutComponents = 1;
			OutLayout = TEXT("u32");
			return;
		case ERocksMeshValueType::Bool:
			OutComponents = 1;
			OutLayout = TEXT("u8");
			return;
		case ERocksMeshValueType::Name:
			OutComponents = 1;
			OutLayout = TEXT("names");
			return;
		case ERocksMeshValueType::BoneInfluences:
			OutComponents = 1;
			OutLayout = TEXT("boneInfluences");
			return;
		case ERocksMeshValueType::SparseVec3:
			OutComponents = 1;
			OutLayout = TEXT("sparseVec3");
			return;
		default:
			OutComponents = 1;
			OutLayout = TEXT("f32");
			return;
		}
	}

	static bool IsEngineName(const FString& Name)
	{
		for (const TCHAR* Engine : EngineNames)
		{
			if (Name.Equals(Engine, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}

		for (const TCHAR* Prefix : EnginePrefixes)
		{
			if (Name.StartsWith(Prefix, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}

		return false;
	}

	static void AppendU32(TArray64<uint8>& Out, uint32 Value)
	{
		const uint8 Bytes[] = {uint8(Value), uint8(Value >> 8), uint8(Value >> 16), uint8(Value >> 24)};
		Out.Append(Bytes, UE_ARRAY_COUNT(Bytes));
	}

	static void AppendF32(TArray64<uint8>& Out, float Value)
	{
		uint32 Bits;
		FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
		AppendU32(Out, Bits);
	}

	static void AppendValue(TArray64<uint8>& Out, float Value)
	{
		AppendF32(Out, Value);
	}

	static void AppendValue(TArray64<uint8>& Out, const FVector2f& Value)
	{
		AppendF32(Out, Value.X);
		AppendF32(Out, Value.Y);
	}

	static void AppendValue(TArray64<uint8>& Out, const FVector3f& Value)
	{
		AppendF32(Out, Value.X);
		AppendF32(Out, Value.Y);
		AppendF32(Out, Value.Z);
	}

	static void AppendValue(TArray64<uint8>& Out, const FVector4f& Value)
	{
		AppendF32(Out, Value.X);
		AppendF32(Out, Value.Y);
		AppendF32(Out, Value.Z);
		AppendF32(Out, Value.W);
	}

	static void AppendValue(TArray64<uint8>& Out, int32 Value)
	{
		AppendU32(Out, uint32(Value));
	}

	static void AppendValue(TArray64<uint8>& Out, bool bValue)
	{
		Out.Add(bValue ? 1 : 0);
	}

	// Name 값: uint32 byte 길이 + UTF-8(None은 빈 문자열 — MA1 3.7절 8).
	static void AppendValue(TArray64<uint8>& Out, FName Value)
	{
		TArray<uint8> Text;
		if (!Value.IsNone())
		{
			Text = FRocksJsonWriter::ToUtf8(Value.ToString());
		}

		AppendU32(Out, uint32(Text.Num()));
		Out.Append(Text.GetData(), Text.Num());
	}

	template <typename IDType, typename ValueType>
	static void AppendChannels(const TAttributesSet<IDType>& Set, FName Name, int32 Count, int32 Channels, TArray64<uint8>& Out)
	{
		const auto Values = Set.template GetAttributesRef<ValueType>(Name);
		for (int32 Channel = 0; Channel < Channels; ++Channel)
		{
			for (int32 Index = 0; Index < Count; ++Index)
			{
				AppendValue(Out, Values.Get(IDType(Index), Channel));
			}
		}
	}

	static const FMapping* FindMapping(ERocksMeshElement Element, FName UeName)
	{
		for (const FMapping& Mapping : Mappings())
		{
			if (Mapping.Element == Element && Mapping.UeName == UeName)
			{
				return &Mapping;
			}
		}

		return nullptr;
	}

	// 요소 하나의 속성 집합을 기록에 옮긴다(3.2절 대응·보존 규칙).
	template <typename IDType>
	static void CollectAttributes(const TAttributesSet<IDType>& Set, ERocksMeshElement Element, int32 Count, FRocksMeshSourceRecord& Record, TArray<FRocksExportDiagnostic>& Diagnostics)
	{
		TArray<FName> Names;
		Set.GetAttributeNames(Names);
		Names.Sort([](const FName Left, const FName Right) { return FRocksJsonWriter::CompareUtf8(Left.ToString(), Right.ToString()) < 0; });
		for (const FName UeName : Names)
		{
			// 스킨 가중치·morph는 CollectSkeletalAttributes가 Rocks 배치로 옮긴다.
			if (FSkeletalMeshAttributesShared::IsSkinWeightAttribute(UeName) || FSkeletalMeshAttributesShared::IsMorphTargetAttribute(UeName))
			{
				continue;
			}

			if (Set.DoesAttributeHaveAnyFlags(UeName, EMeshAttributeFlags::Transient | EMeshAttributeFlags::IndexReference))
			{
				continue;
			}

			ERocksMeshValueType Type;
			if (Set.template HasAttributeOfType<float>(UeName))
			{
				Type = ERocksMeshValueType::Float;
			}
			else if (Set.template HasAttributeOfType<FVector2f>(UeName))
			{
				Type = ERocksMeshValueType::Vec2;
			}
			else if (Set.template HasAttributeOfType<FVector3f>(UeName))
			{
				Type = ERocksMeshValueType::Vec3;
			}
			else if (Set.template HasAttributeOfType<FVector4f>(UeName))
			{
				Type = ERocksMeshValueType::Vec4;
			}
			else if (Set.template HasAttributeOfType<int32>(UeName))
			{
				Type = ERocksMeshValueType::UInt32;
			}
			else if (Set.template HasAttributeOfType<bool>(UeName))
			{
				Type = ERocksMeshValueType::Bool;
			}
			else if (Set.template HasAttributeOfType<FName>(UeName))
			{
				Type = ERocksMeshValueType::Name;
			}
			else
			{
				Diagnostics.Add({TEXT("SkippedMeshAttribute"), RocksExport::SeverityInfo,
					FString::Printf(TEXT("%s attribute %s has a value type the exchange format does not carry"), ElementName(Element), *UeName.ToString())});
				continue;
			}

			const int32 Channels = Set.GetAttributeChannelCount(UeName);
			const FMapping* Mapping = FindMapping(Element, UeName);
			FString RocksName = UeName.ToString();
			if (Mapping != nullptr)
			{
				RocksName = Mapping->RocksName;
				if (Mapping->Type != Type)
				{
					Diagnostics.Add({TEXT("SkippedMeshAttribute"), RocksExport::SeverityWarning,
						FString::Printf(TEXT("%s attribute %s has an unexpected value type"), ElementName(Element), *UeName.ToString())});
					continue;
				}
			}
			else if (IsEngineName(RocksName))
			{
				Diagnostics.Add({TEXT("SkippedMeshAttribute"), RocksExport::SeverityWarning,
					FString::Printf(TEXT("%s attribute %s collides with a Rocks engine attribute name"), ElementName(Element), *RocksName)});
				continue;
			}

			// 채널이 없는 속성(UV 0채널 등)은 값이 없으므로 쓰지 않는다. Rocks 상한을 넘는 채널은 빼고 알린다.
			const int32 ChannelLimit = RocksName == TEXT("TexCoord") ? MaxTexCoords : MaxChannels;
			if (Channels <= 0)
			{
				continue;
			}

			if (Channels > ChannelLimit)
			{
				Diagnostics.Add({TEXT("SkippedMeshAttribute"), RocksExport::SeverityWarning,
					FString::Printf(TEXT("%s attribute %s has %d channels, more than %d"), ElementName(Element), *UeName.ToString(), Channels, ChannelLimit)});
				continue;
			}

			FRocksMeshAttribute& Attribute = Record.Attributes.AddDefaulted_GetRef();
			Attribute.Element = Element;
			Attribute.Name = RocksName;
			Attribute.Type = Type;
			Attribute.Channels = Channels;
			switch (Type)
			{
			case ERocksMeshValueType::Float:
				AppendChannels<IDType, float>(Set, UeName, Count, Channels, Attribute.Bytes);
				break;
			case ERocksMeshValueType::Vec2:
				AppendChannels<IDType, FVector2f>(Set, UeName, Count, Channels, Attribute.Bytes);
				break;
			case ERocksMeshValueType::Vec3:
				AppendChannels<IDType, FVector3f>(Set, UeName, Count, Channels, Attribute.Bytes);
				break;
			case ERocksMeshValueType::Vec4:
				AppendChannels<IDType, FVector4f>(Set, UeName, Count, Channels, Attribute.Bytes);
				break;
			case ERocksMeshValueType::UInt32:
				AppendChannels<IDType, int32>(Set, UeName, Count, Channels, Attribute.Bytes);
				break;
			case ERocksMeshValueType::Bool:
				AppendChannels<IDType, bool>(Set, UeName, Count, Channels, Attribute.Bytes);
				break;
			default:
				AppendChannels<IDType, FName>(Set, UeName, Count, Channels, Attribute.Bytes);
				break;
			}
		}
	}

	// 세 성분이 모두 비트 단위 +0.0인가다(희소 저장은 그 밖의 값만 담는다 — MeshAnimationAssets 3.2절).
	static bool IsZeroBits(const FVector3f& Value)
	{
		static const FVector3f Zero(0.0f, 0.0f, 0.0f);
		return FMemory::Memcmp(&Value, &Zero, sizeof(FVector3f)) == 0;
	}

	// BoneInfluences 배치: 시작 위치 uint32 × (N + 1) + (uint16 본, uint16 가중치) × 마지막 값(3.4.1절).
	static void AppendInfluences(TArray64<uint8>& Out, const TArray<TArray<TPair<uint16, uint16>>>& PerVertex)
	{
		uint32 Start = 0;
		AppendU32(Out, Start);
		for (const TArray<TPair<uint16, uint16>>& Influences : PerVertex)
		{
			Start += uint32(Influences.Num());
			AppendU32(Out, Start);
		}

		for (const TArray<TPair<uint16, uint16>>& Influences : PerVertex)
		{
			for (const TPair<uint16, uint16>& Influence : Influences)
			{
				const uint8 Bytes[] = {uint8(Influence.Key), uint8(Influence.Key >> 8), uint8(Influence.Value), uint8(Influence.Value >> 8)};
				Out.Append(Bytes, UE_ARRAY_COUNT(Bytes));
			}
		}
	}

	// SparseVec3 배치: 개수 K + 번호 uint32 × K(엄격 증가) + Vec3 × K(3.4.1절). 0 비트 값은 넣지 않는다.
	static void AppendSparse(TArray64<uint8>& Out, const TArray<TPair<uint32, FVector3f>>& Entries)
	{
		TArray<TPair<uint32, FVector3f>> NonZero;
		for (const TPair<uint32, FVector3f>& Entry : Entries)
		{
			if (!IsZeroBits(Entry.Value))
			{
				NonZero.Add(Entry);
			}
		}

		NonZero.Sort([](const TPair<uint32, FVector3f>& Left, const TPair<uint32, FVector3f>& Right) { return Left.Key < Right.Key; });
		AppendU32(Out, uint32(NonZero.Num()));
		for (const TPair<uint32, FVector3f>& Entry : NonZero)
		{
			AppendU32(Out, Entry.Key);
		}

		for (const TPair<uint32, FVector3f>& Entry : NonZero)
		{
			AppendValue(Out, Entry.Value);
		}
	}

	// 스킨 메시 설명의 가중치·morph 속성을 Rocks 배치로 옮긴다(정적 메시 설명에는 없다).
	static void CollectSkeletalAttributes(const FMeshDescription& Mesh, FRocksMeshSourceRecord& Record)
	{
		const FSkeletalMeshAttributesShared Attributes(Mesh);
		TArray<FName> Names;
		Mesh.VertexAttributes().GetAttributeNames(Names);
		Names.Sort([](const FName Left, const FName Right) { return FRocksJsonWriter::CompareUtf8(Left.ToString(), Right.ToString()) < 0; });
		for (const FName Name : Names)
		{
			if (!FSkeletalMeshAttributesShared::IsSkinWeightAttribute(Name))
			{
				continue;
			}

			// UE 이름(SkinWeights, SkinWeights-<profile>)이 Rocks 이름과 같다. 본 번호는 메시 원본 계층 번호다.
			const FSkinWeightsVertexAttributesConstRef Weights = Attributes.GetVertexSkinWeightsFromAttributeName(Name);
			TArray<TArray<TPair<uint16, uint16>>> PerVertex;
			PerVertex.SetNum(int32(Record.VertexCount));
			for (int32 Vertex = 0; Vertex < int32(Record.VertexCount); ++Vertex)
			{
				for (const UE::AnimationCore::FBoneWeight Weight : Weights.Get(FVertexID(Vertex)))
				{
					PerVertex[Vertex].Add(TPair<uint16, uint16>(uint16(Weight.GetBoneIndex()), Weight.GetRawWeight()));
				}
			}

			FRocksMeshAttribute& Attribute = Record.Attributes.AddDefaulted_GetRef();
			Attribute.Element = ERocksMeshElement::Vertex;
			Attribute.Name = Name.ToString();
			Attribute.Type = ERocksMeshValueType::BoneInfluences;
			AppendInfluences(Attribute.Bytes, PerVertex);
		}

		TArray<FName> Morphs = Attributes.GetMorphTargetNames();
		Morphs.Sort([](const FName Left, const FName Right) { return FRocksJsonWriter::CompareUtf8(Left.ToString(), Right.ToString()) < 0; });
		for (const FName Morph : Morphs)
		{
			if (Attributes.HasMorphTargetPositionsAttribute(Morph))
			{
				const TVertexAttributesConstRef<FVector3f> Deltas = Attributes.GetVertexMorphPositionDelta(Morph);
				TArray<TPair<uint32, FVector3f>> Entries;
				for (int32 Vertex = 0; Vertex < int32(Record.VertexCount); ++Vertex)
				{
					Entries.Add(TPair<uint32, FVector3f>(uint32(Vertex), Deltas.Get(FVertexID(Vertex))));
				}

				FRocksMeshAttribute& Attribute = Record.Attributes.AddDefaulted_GetRef();
				Attribute.Element = ERocksMeshElement::Vertex;
				Attribute.Name = TEXT("Morph-") + Morph.ToString();
				Attribute.Type = ERocksMeshValueType::SparseVec3;
				AppendSparse(Attribute.Bytes, Entries);
			}

			// UE는 코너 쪽 법선 변화량도 Morph-<이름>으로 등록한다. Rocks는 MorphNormal-<이름>이다(6.2절).
			if (Attributes.HasMorphTargetNormalsAttribute(Morph))
			{
				const TVertexInstanceAttributesConstRef<FVector3f> Deltas = Attributes.GetVertexInstanceMorphNormalDelta(Morph);
				TArray<TPair<uint32, FVector3f>> Entries;
				for (int32 Corner = 0; Corner < int32(Record.CornerCount); ++Corner)
				{
					Entries.Add(TPair<uint32, FVector3f>(uint32(Corner), Deltas.Get(FVertexInstanceID(Corner))));
				}

				FRocksMeshAttribute& Attribute = Record.Attributes.AddDefaulted_GetRef();
				Attribute.Element = ERocksMeshElement::Corner;
				Attribute.Name = TEXT("MorphNormal-") + Morph.ToString();
				Attribute.Type = ERocksMeshValueType::SparseVec3;
				AppendSparse(Attribute.Bytes, Entries);
			}
		}
	}

	static void SortAttributes(FRocksMeshSourceRecord& Record)
	{
		Record.Attributes.Sort([](const FRocksMeshAttribute& Left, const FRocksMeshAttribute& Right)
			{
				if (Left.Element != Right.Element)
				{
					return Left.Element < Right.Element;
				}

				return FRocksJsonWriter::CompareUtf8(Left.Name, Right.Name) < 0;
			});
	}

	static FRocksMeshAttribute& AddAttribute(FRocksMeshSourceRecord& Record, ERocksMeshElement Element, const TCHAR* Name, ERocksMeshValueType Type, int32 Channels)
	{
		FRocksMeshAttribute& Attribute = Record.Attributes.AddDefaulted_GetRef();
		Attribute.Element = Element;
		Attribute.Name = Name;
		Attribute.Type = Type;
		Attribute.Channels = Channels;
		return Attribute;
	}

	static bool WriteWords(const TArray<uint32>& Words, uint32 Components, uint64 Count, FRocksBundleWriter& Bundle, FRocksJsonWriter& Writer, FString& OutError)
	{
		FRocksBlobRef Ref;
		const TConstArrayView64<uint8> Bytes(reinterpret_cast<const uint8*>(Words.GetData()), int64(Words.Num()) * int64(sizeof(uint32)));
		if (!Bundle.WriteBlob(Bytes, TEXT("u32"), Components, Count, Ref, OutError))
		{
			return false;
		}

		Ref.WriteTo(Writer);
		return true;
	}
}

void FRocksMeshDescriptionWriter::Compact(FMeshDescription& Mesh)
{
	if (Mesh.NeedsCompact())
	{
		FElementIDRemappings Remappings;
		Mesh.Compact(Remappings);
	}
}

bool FRocksMeshDescriptionWriter::FromMeshDescription(const FMeshDescription& Mesh, FRocksMeshSourceRecord& OutRecord, TArray<FRocksExportDiagnostic>& OutDiagnostics, FString& OutError)
{
	using namespace RocksMeshDescriptionWriterPrivate;

	const bool bCompact = Mesh.Vertices().Num() == Mesh.Vertices().GetArraySize()
		&& Mesh.VertexInstances().Num() == Mesh.VertexInstances().GetArraySize()
		&& Mesh.Edges().Num() == Mesh.Edges().GetArraySize()
		&& Mesh.Polygons().Num() == Mesh.Polygons().GetArraySize()
		&& Mesh.Triangles().Num() == Mesh.Triangles().GetArraySize()
		&& Mesh.PolygonGroups().Num() == Mesh.PolygonGroups().GetArraySize();
	if (!bCompact)
	{
		OutError = TEXT("mesh description is not compact");
		return false;
	}

	FRocksMeshSourceRecord Record;
	Record.VertexCount = uint32(Mesh.Vertices().Num());
	Record.CornerCount = uint32(Mesh.VertexInstances().Num());
	Record.EdgeCount = uint32(Mesh.Edges().Num());
	Record.PolygonCount = uint32(Mesh.Polygons().Num());
	Record.TriangleCount = uint32(Mesh.Triangles().Num());
	Record.PolygonGroupCount = uint32(Mesh.PolygonGroups().Num());

	for (uint32 Corner = 0; Corner < Record.CornerCount; ++Corner)
	{
		Record.CornerVertices.Add(uint32(Mesh.GetVertexInstanceVertex(FVertexInstanceID(int32(Corner))).GetValue()));
	}

	// 변은 작은 정점 번호를 먼저 쓴다(MeshAnimationAssets 3.1절).
	for (uint32 Edge = 0; Edge < Record.EdgeCount; ++Edge)
	{
		const uint32 First = uint32(Mesh.GetEdgeVertex(FEdgeID(int32(Edge)), 0).GetValue());
		const uint32 Second = uint32(Mesh.GetEdgeVertex(FEdgeID(int32(Edge)), 1).GetValue());
		Record.EdgeVertices.Add(FMath::Min(First, Second));
		Record.EdgeVertices.Add(FMath::Max(First, Second));
	}

	Record.PolygonStarts.Add(0);
	for (uint32 Polygon = 0; Polygon < Record.PolygonCount; ++Polygon)
	{
		const FPolygonID PolygonID(static_cast<int32>(Polygon));
		for (const FVertexInstanceID Corner : Mesh.GetPolygonVertexInstances(PolygonID))
		{
			Record.PolygonCorners.Add(uint32(Corner.GetValue()));
		}

		Record.PolygonStarts.Add(uint32(Record.PolygonCorners.Num()));
		Record.PolygonGroups.Add(uint32(Mesh.GetPolygonPolygonGroup(PolygonID).GetValue()));
	}

	for (uint32 Triangle = 0; Triangle < Record.TriangleCount; ++Triangle)
	{
		const FTriangleID TriangleID(static_cast<int32>(Triangle));
		for (const FVertexInstanceID Corner : Mesh.GetTriangleVertexInstances(TriangleID))
		{
			Record.TriangleCorners.Add(uint32(Corner.GetValue()));
		}

		Record.TrianglePolygons.Add(uint32(Mesh.GetTrianglePolygon(TriangleID).GetValue()));
	}

	CollectAttributes(Mesh.VertexAttributes(), ERocksMeshElement::Vertex, int32(Record.VertexCount), Record, OutDiagnostics);
	CollectAttributes(Mesh.VertexInstanceAttributes(), ERocksMeshElement::Corner, int32(Record.CornerCount), Record, OutDiagnostics);
	CollectAttributes(Mesh.EdgeAttributes(), ERocksMeshElement::Edge, int32(Record.EdgeCount), Record, OutDiagnostics);
	CollectAttributes(Mesh.PolygonAttributes(), ERocksMeshElement::Polygon, int32(Record.PolygonCount), Record, OutDiagnostics);
	CollectAttributes(Mesh.TriangleAttributes(), ERocksMeshElement::Triangle, int32(Record.TriangleCount), Record, OutDiagnostics);
	CollectAttributes(Mesh.PolygonGroupAttributes(), ERocksMeshElement::PolygonGroup, int32(Record.PolygonGroupCount), Record, OutDiagnostics);
	CollectSkeletalAttributes(Mesh, Record);
	SortAttributes(Record);
	OutRecord = MoveTemp(Record);
	return true;
}

bool FRocksMeshDescriptionWriter::FromStaticMeshLod(const FStaticMeshLODResources& Lod, TConstArrayView<FName> SectionSlotNames, FRocksMeshSourceRecord& OutRecord, TArray<FRocksExportDiagnostic>& OutDiagnostics, FString& OutError)
{
	const FPositionVertexBuffer& Positions = Lod.VertexBuffers.PositionVertexBuffer;
	const FStaticMeshVertexBuffer& Vertices = Lod.VertexBuffers.StaticMeshVertexBuffer;
	const FColorVertexBuffer& Colors = Lod.VertexBuffers.ColorVertexBuffer;
	const uint32 VertexCount = Positions.GetNumVertices();
	if (VertexCount > 0 && (Positions.GetVertexData() == nullptr || Vertices.GetTangentData() == nullptr || Vertices.GetNumVertices() != VertexCount))
	{
		OutError = TEXT("render data has no CPU copy");
		return false;
	}

	FRocksBakedMesh Baked;
	Baked.TexCoordCount = int32(Vertices.GetNumTexCoords());
	for (uint32 Vertex = 0; Vertex < VertexCount; ++Vertex)
	{
		Baked.Positions.Add(Positions.VertexPosition(Vertex));
		Baked.TangentZ.Add(Vertices.VertexTangentZ(Vertex));
		const FVector4f TangentX = Vertices.VertexTangentX(Vertex);
		Baked.TangentX.Add(FVector3f(TangentX.X, TangentX.Y, TangentX.Z));
	}

	for (int32 Channel = 0; Channel < Baked.TexCoordCount; ++Channel)
	{
		for (uint32 Vertex = 0; Vertex < VertexCount; ++Vertex)
		{
			Baked.TexCoords.Add(Vertices.GetVertexUV(Vertex, Channel));
		}
	}

	if (VertexCount > 0 && Colors.GetNumVertices() == VertexCount && Colors.GetVertexData() != nullptr)
	{
		for (uint32 Vertex = 0; Vertex < VertexCount; ++Vertex)
		{
			Baked.Colors.Add(Colors.VertexColor(Vertex));
		}
	}

	Lod.IndexBuffer.GetCopy(Baked.Indices);
	for (int32 Section = 0; Section < Lod.Sections.Num(); ++Section)
	{
		const FStaticMeshSection& Info = Lod.Sections[Section];
		Baked.Sections.Add({Info.FirstIndex, Info.NumTriangles, SectionSlotNames.IsValidIndex(Section) ? SectionSlotNames[Section] : FName(NAME_None)});
	}

	return FromBakedMesh(Baked, OutRecord, OutDiagnostics, OutError);
}

bool FRocksMeshDescriptionWriter::FromSkeletalLodModel(const FSkeletalMeshLODModel& Model, TConstArrayView<FName> SectionSlotNames, TConstArrayView<const UMorphTarget*> MorphTargets, int32 Lod, FRocksMeshSourceRecord& OutRecord, TArray<FRocksExportDiagnostic>& OutDiagnostics, FString& OutError)
{
	using namespace RocksMeshDescriptionWriterPrivate;

	FRocksBakedMesh Baked;
	Baked.TexCoordCount = FMath::Min(int32(Model.NumTexCoords), MaxTexCoords);
	TArray<const FSoftSkinVertex*> Vertices;
	for (const FSkelMeshSection& Section : Model.Sections)
	{
		if (Section.BaseVertexIndex != uint32(Vertices.Num()))
		{
			OutError = TEXT("skeletal LOD model sections are not contiguous");
			return false;
		}

		for (const FSoftSkinVertex& Vertex : Section.SoftVertices)
		{
			Vertices.Add(&Vertex);
			Baked.Positions.Add(Vertex.Position);
			Baked.TangentZ.Add(Vertex.TangentZ);
			Baked.TangentX.Add(Vertex.TangentX);
			Baked.Colors.Add(Vertex.Color);

			// 섹션 지역 본 번호를 BoneMap으로 메시 본 번호로 바꾼다(SkeletalMeshLODModel.h). 가중치 0 칸은 쓰지 않는다.
			TArray<TPair<uint16, uint16>>& Influences = Baked.Influences.AddDefaulted_GetRef();
			for (int32 Slot = 0; Slot < MAX_TOTAL_INFLUENCES; ++Slot)
			{
				if (Vertex.InfluenceWeights[Slot] != 0 && Section.BoneMap.IsValidIndex(Vertex.InfluenceBones[Slot]))
				{
					Influences.Add(TPair<uint16, uint16>(uint16(Section.BoneMap[Vertex.InfluenceBones[Slot]]), Vertex.InfluenceWeights[Slot]));
				}
			}
		}

		const int32 SectionIndex = Baked.Sections.Num();
		Baked.Sections.Add({Section.BaseIndex, Section.NumTriangles, SectionSlotNames.IsValidIndex(SectionIndex) ? SectionSlotNames[SectionIndex] : FName(NAME_None)});
	}

	for (int32 Channel = 0; Channel < Baked.TexCoordCount; ++Channel)
	{
		for (const FSoftSkinVertex* Vertex : Vertices)
		{
			Baked.TexCoords.Add(Vertex->UVs[Channel]);
		}
	}

	Baked.Indices = Model.IndexBuffer;
	for (const UMorphTarget* Target : MorphTargets)
	{
		if (Target == nullptr || !Target->GetMorphLODModels().IsValidIndex(Lod))
		{
			continue;
		}

		FRocksBakedMesh::FMorph& Morph = Baked.Morphs.AddDefaulted_GetRef();
		Morph.Name = Target->GetFName();
		for (const FMorphTargetDelta& Delta : Target->GetMorphLODModels()[Lod].Vertices)
		{
			if (Delta.SourceIdx < uint32(Vertices.Num()))
			{
				Morph.Deltas.Add(Delta.SourceIdx, TPair<FVector3f, FVector3f>(Delta.PositionDelta, Delta.TangentZDelta));
			}
		}
	}

	return FromBakedMesh(Baked, OutRecord, OutDiagnostics, OutError);
}

bool FRocksMeshDescriptionWriter::FromBakedMesh(const FRocksBakedMesh& Mesh, FRocksMeshSourceRecord& OutRecord, TArray<FRocksExportDiagnostic>& OutDiagnostics, FString& OutError)
{
	using namespace RocksMeshDescriptionWriterPrivate;

	const uint32 VertexCount = uint32(Mesh.Positions.Num());
	FRocksMeshSourceRecord Record;
	Record.VertexCount = VertexCount;
	Record.CornerCount = VertexCount;
	Record.PolygonGroupCount = uint32(Mesh.Sections.Num());
	for (uint32 Vertex = 0; Vertex < VertexCount; ++Vertex)
	{
		Record.CornerVertices.Add(Vertex);
	}

	// 섹션 순서로 삼각형을 모은다. 다각형 = 삼각형, 변은 정점 쌍마다 하나(처음 나온 순서).
	TMap<uint64, uint32> EdgeIndex;
	uint32 Degenerate = 0;
	Record.PolygonStarts.Add(0);
	for (int32 Section = 0; Section < Mesh.Sections.Num(); ++Section)
	{
		const FRocksBakedMesh::FSection& Info = Mesh.Sections[Section];
		for (uint32 Triangle = 0; Triangle < Info.NumTriangles; ++Triangle)
		{
			const uint32 First = Info.FirstIndex + Triangle * TriangleWords;
			if (First + TriangleWords > uint32(Mesh.Indices.Num()))
			{
				OutError = TEXT("baked section points past the index buffer");
				return false;
			}

			const uint32 Corners[] = {Mesh.Indices[First], Mesh.Indices[First + 1], Mesh.Indices[First + 2]};
			if (Corners[0] >= VertexCount || Corners[1] >= VertexCount || Corners[2] >= VertexCount)
			{
				OutError = TEXT("baked index out of range");
				return false;
			}

			if (Corners[0] == Corners[1] || Corners[1] == Corners[2] || Corners[0] == Corners[2])
			{
				++Degenerate;
				continue;
			}

			const uint32 Polygon = Record.PolygonCount++;
			for (int32 Side = 0; Side < 3; ++Side)
			{
				const uint32 A = Corners[Side];
				const uint32 B = Corners[(Side + 1) % 3];
				const uint64 Key = (uint64(FMath::Min(A, B)) << 32) | uint64(FMath::Max(A, B));
				if (!EdgeIndex.Contains(Key))
				{
					EdgeIndex.Add(Key, Record.EdgeCount++);
					Record.EdgeVertices.Add(FMath::Min(A, B));
					Record.EdgeVertices.Add(FMath::Max(A, B));
				}

				Record.PolygonCorners.Add(Corners[Side]);
				Record.TriangleCorners.Add(Corners[Side]);
			}

			Record.PolygonStarts.Add(uint32(Record.PolygonCorners.Num()));
			Record.PolygonGroups.Add(uint32(Section));
			Record.TrianglePolygons.Add(Polygon);
			++Record.TriangleCount;
		}
	}

	if (Degenerate > 0)
	{
		OutDiagnostics.Add({TEXT("BakedDegenerateTriangles"), RocksExport::SeverityInfo,
			FString::Printf(TEXT("%u baked triangles use one vertex twice and were left out"), Degenerate)});
	}

	FRocksMeshAttribute& Position = AddAttribute(Record, ERocksMeshElement::Vertex, TEXT("Position"), ERocksMeshValueType::Vec3, 1);
	FRocksMeshAttribute& Normal = AddAttribute(Record, ERocksMeshElement::Corner, TEXT("Normal"), ERocksMeshValueType::Vec3, 1);
	FRocksMeshAttribute& Tangent = AddAttribute(Record, ERocksMeshElement::Corner, TEXT("Tangent"), ERocksMeshValueType::Vec3, 1);
	FRocksMeshAttribute& Sign = AddAttribute(Record, ERocksMeshElement::Corner, TEXT("BinormalSign"), ERocksMeshValueType::Float, 1);
	for (uint32 Vertex = 0; Vertex < VertexCount; ++Vertex)
	{
		AppendValue(Position.Bytes, Mesh.Positions[Vertex]);
		const FVector4f& TangentZ = Mesh.TangentZ[Vertex];
		AppendValue(Normal.Bytes, FVector3f(TangentZ.X, TangentZ.Y, TangentZ.Z));
		AppendValue(Tangent.Bytes, Mesh.TangentX[Vertex]);
		AppendValue(Sign.Bytes, TangentZ.W);
	}

	const int32 TexCoords = FMath::Min(Mesh.TexCoordCount, MaxTexCoords);
	if (TexCoords > 0)
	{
		FRocksMeshAttribute& Uv = AddAttribute(Record, ERocksMeshElement::Corner, TEXT("TexCoord"), ERocksMeshValueType::Vec2, TexCoords);
		for (int32 Index = 0; Index < TexCoords * int32(VertexCount); ++Index)
		{
			AppendValue(Uv.Bytes, Mesh.TexCoords[Index]);
		}
	}

	// UE 빌드 결과의 색은 8비트 sRGB다. 선형으로 풀어 쓰고 양자화 손실을 알린다.
	if (VertexCount > 0 && uint32(Mesh.Colors.Num()) == VertexCount)
	{
		FRocksMeshAttribute& Color = AddAttribute(Record, ERocksMeshElement::Corner, TEXT("Color"), ERocksMeshValueType::Vec4, 1);
		for (uint32 Vertex = 0; Vertex < VertexCount; ++Vertex)
		{
			const FLinearColor Linear = FLinearColor::FromSRGBColor(Mesh.Colors[Vertex]);
			AppendValue(Color.Bytes, FVector4f(Linear.R, Linear.G, Linear.B, Linear.A));
		}

		OutDiagnostics.Add({TEXT("BakedColorQuantized"), RocksExport::SeverityInfo, TEXT("baked vertex colors come from 8-bit sRGB render data")});
	}

	if (uint32(Mesh.Influences.Num()) == VertexCount && VertexCount > 0)
	{
		FRocksMeshAttribute& Weights = AddAttribute(Record, ERocksMeshElement::Vertex, TEXT("SkinWeights"), ERocksMeshValueType::BoneInfluences, 1);
		AppendInfluences(Weights.Bytes, Mesh.Influences);
	}

	for (const FRocksBakedMesh::FMorph& Morph : Mesh.Morphs)
	{
		TArray<TPair<uint32, FVector3f>> Positions;
		TArray<TPair<uint32, FVector3f>> Normals;
		for (const TPair<uint32, TPair<FVector3f, FVector3f>>& Delta : Morph.Deltas)
		{
			Positions.Add(TPair<uint32, FVector3f>(Delta.Key, Delta.Value.Key));
			Normals.Add(TPair<uint32, FVector3f>(Delta.Key, Delta.Value.Value));
		}

		FRocksMeshAttribute& PositionDelta = Record.Attributes.AddDefaulted_GetRef();
		PositionDelta.Element = ERocksMeshElement::Vertex;
		PositionDelta.Name = TEXT("Morph-") + Morph.Name.ToString();
		PositionDelta.Type = ERocksMeshValueType::SparseVec3;
		AppendSparse(PositionDelta.Bytes, Positions);
		FRocksMeshAttribute& NormalDelta = Record.Attributes.AddDefaulted_GetRef();
		NormalDelta.Element = ERocksMeshElement::Corner;
		NormalDelta.Name = TEXT("MorphNormal-") + Morph.Name.ToString();
		NormalDelta.Type = ERocksMeshValueType::SparseVec3;
		AppendSparse(NormalDelta.Bytes, Normals);
	}

	FRocksMeshAttribute& Slots = AddAttribute(Record, ERocksMeshElement::PolygonGroup, TEXT("MaterialSlotName"), ERocksMeshValueType::Name, 1);
	for (const FRocksBakedMesh::FSection& Section : Mesh.Sections)
	{
		AppendValue(Slots.Bytes, Section.SlotName);
	}

	SortAttributes(Record);
	OutRecord = MoveTemp(Record);
	return true;
}

bool FRocksMeshDescriptionWriter::Write(const FRocksMeshSourceRecord& Record, FRocksBundleWriter& Bundle, FRocksJsonWriter& Writer, FString& OutError)
{
	using namespace RocksMeshDescriptionWriterPrivate;

	Writer.BeginObject();
	Writer.Key(TEXT("counts"));
	Writer.BeginObject();
	Writer.Key(TEXT("vertices"));
	Writer.UInt(Record.VertexCount);
	Writer.Key(TEXT("corners"));
	Writer.UInt(Record.CornerCount);
	Writer.Key(TEXT("edges"));
	Writer.UInt(Record.EdgeCount);
	Writer.Key(TEXT("polygons"));
	Writer.UInt(Record.PolygonCount);
	Writer.Key(TEXT("triangles"));
	Writer.UInt(Record.TriangleCount);
	Writer.Key(TEXT("polygonGroups"));
	Writer.UInt(Record.PolygonGroupCount);
	Writer.EndObject();

	// 9.1절 순서의 위상 배열 일곱이다.
	Writer.Key(TEXT("topology"));
	Writer.BeginArray();
	const bool bTopology = WriteWords(Record.CornerVertices, 1, Record.CornerCount, Bundle, Writer, OutError)
		&& WriteWords(Record.EdgeVertices, EdgeWords, Record.EdgeCount, Bundle, Writer, OutError)
		&& WriteWords(Record.PolygonStarts, 1, uint64(Record.PolygonCount) + 1, Bundle, Writer, OutError)
		&& WriteWords(Record.PolygonCorners, 1, uint64(Record.PolygonCorners.Num()), Bundle, Writer, OutError)
		&& WriteWords(Record.PolygonGroups, 1, Record.PolygonCount, Bundle, Writer, OutError)
		&& WriteWords(Record.TriangleCorners, TriangleWords, Record.TriangleCount, Bundle, Writer, OutError)
		&& WriteWords(Record.TrianglePolygons, 1, Record.TriangleCount, Bundle, Writer, OutError);
	if (!bTopology)
	{
		return false;
	}

	Writer.EndArray();

	const uint32 Counts[] = {0, Record.VertexCount, Record.CornerCount, Record.EdgeCount, Record.PolygonCount, Record.TriangleCount, Record.PolygonGroupCount};
	Writer.Key(TEXT("attributes"));
	Writer.BeginArray();
	for (const FRocksMeshAttribute& Attribute : Record.Attributes)
	{
		uint32 Components = 0;
		const TCHAR* Layout = nullptr;
		DescribeType(Attribute.Type, Components, Layout);
		FRocksBlobRef Ref;
		const uint32 BlobComponents = Attribute.Type == ERocksMeshValueType::Name ? uint32(Attribute.Channels) : Components * uint32(Attribute.Channels);
		if (!Bundle.WriteBlob(Attribute.Bytes, Layout, BlobComponents, Counts[uint8(Attribute.Element)], Ref, OutError))
		{
			OutError = FString::Printf(TEXT("attribute %s: %s"), *Attribute.Name, *OutError);
			return false;
		}

		Writer.BeginObject();
		Writer.Key(TEXT("element"));
		Writer.String(ElementName(Attribute.Element));
		Writer.Key(TEXT("name"));
		Writer.String(Attribute.Name);
		Writer.Key(TEXT("type"));
		Writer.String(TypeName(Attribute.Type));
		Writer.Key(TEXT("channels"));
		Writer.Int(Attribute.Channels);
		Writer.Key(TEXT("data"));
		Ref.WriteTo(Writer);
		Writer.EndObject();
	}

	Writer.EndArray();
	Writer.EndObject();
	return true;
}

FString FRocksMeshDescriptionWriter::Digest(const FRocksMeshSourceRecord& Record)
{
	FRocksSha256 Hash;
	const uint32 Counts[] = {Record.VertexCount, Record.CornerCount, Record.EdgeCount, Record.PolygonCount, Record.TriangleCount, Record.PolygonGroupCount};
	Hash.Update(Counts, sizeof(Counts));
	for (const TArray<uint32>* Words : {&Record.CornerVertices, &Record.EdgeVertices, &Record.PolygonStarts, &Record.PolygonCorners, &Record.PolygonGroups, &Record.TriangleCorners, &Record.TrianglePolygons})
	{
		Hash.Update(Words->GetData(), uint64(Words->Num()) * sizeof(uint32));
	}

	for (const FRocksMeshAttribute& Attribute : Record.Attributes)
	{
		const TArray<uint8> Name = FRocksJsonWriter::ToUtf8(Attribute.Name);
		const uint8 Header[] = {uint8(Attribute.Element), uint8(Attribute.Type), uint8(Attribute.Channels)};
		Hash.Update(Header, sizeof(Header));
		Hash.Update(Name.GetData(), Name.Num());
		Hash.Update(Attribute.Bytes.GetData(), uint64(Attribute.Bytes.Num()));
	}

	return Hash.FinalizeHex();
}
