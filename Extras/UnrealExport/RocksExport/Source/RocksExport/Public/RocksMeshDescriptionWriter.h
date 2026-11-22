#pragma once

#include "CoreMinimal.h"

class FRocksBundleWriter;
class FRocksJsonWriter;
struct FMeshDescription;
struct FRocksExportDiagnostic;
struct FStaticMeshLODResources;

// Rocks MeshSource 요소(MeshAnimationAssets 3.4절 MeshElement 번호와 같은 순서)다.
enum class ERocksMeshElement : uint8
{
	Vertex = 1,
	Corner = 2,
	Edge = 3,
	Polygon = 4,
	Triangle = 5,
	PolygonGroup = 6,
};

// Rocks MeshSource 값 타입(MeshValueType)이다. 정적 메시 교환에 쓰는 것만 둔다.
enum class ERocksMeshValueType : uint8
{
	Float = 1,
	Vec2 = 2,
	Vec3 = 3,
	Vec4 = 4,
	UInt32 = 5,
	Bool = 6,
	Name = 7,
};

// 속성 하나다. Bytes는 MeshAnimationAssets 3.4.1절 배치(채널 우선, little-endian, Name은 uint32 길이 + UTF-8)다.
struct FRocksMeshAttribute
{
	ERocksMeshElement Element = ERocksMeshElement::Vertex;
	FString Name;
	ERocksMeshValueType Type = ERocksMeshValueType::Float;
	int32 Channels = 1;
	TArray64<uint8> Bytes;
};

// 교환 형식 MeshSource 기록 하나(UnrealExchange.ko.md 5.2절)의 메모리 형태다. 위상 배열은 MeshAnimationAssets 9.1절
// 일곱 배열이고 번호는 0부터 이어진다. 정점 위치는 속성 Vertex·Position(Vec3)이다(MA1 3.7절 1).
struct FRocksMeshSourceRecord
{
	uint32 VertexCount = 0;
	uint32 CornerCount = 0;
	uint32 EdgeCount = 0;
	uint32 PolygonCount = 0;
	uint32 TriangleCount = 0;
	uint32 PolygonGroupCount = 0;

	TArray<uint32> CornerVertices;
	TArray<uint32> EdgeVertices;
	TArray<uint32> PolygonStarts;
	TArray<uint32> PolygonCorners;
	TArray<uint32> PolygonGroups;
	TArray<uint32> TriangleCorners;
	TArray<uint32> TrianglePolygons;

	// (요소, 이름 UTF-8 byte) 순이다(MeshAnimationAssets 9.2절).
	TArray<FRocksMeshAttribute> Attributes;
};

// FMeshDescription·정적 메시 렌더 LOD → 교환 MeshSource 기록이다(11.6절 FRocksMeshDescriptionWriter, 5.2절, 5.4절).
//
// 속성 대응(MeshAnimationAssets 3.2절 표): 정점 Position, 코너 Normal·Tangent·BinormalSign·Color·TexCoord(UE
// TextureCoordinate), 변 IsHard, 다각형 PartName(UE ObjectName), 다각형 묶음 MaterialSlotName(UE ImportedMaterialSlotName).
// 그 밖의 UE 속성은 같은 요소·이름으로 보존한다(float·FVector2f·FVector3f·FVector4f·int32(UInt32 비트 그대로)·bool·
// FName). transient·번호 참조 속성은 쓰지 않고, 다른 타입이거나 Rocks 엔진 이름과 겹치는 보존 속성은 빼고
// SkippedMeshAttribute 진단을 단다.
//
// 게임 스레드에서 쓴다.
class ROCKSEXPORT_API FRocksMeshDescriptionWriter
{
public:
	// 압축(Compact)한 메시 설명에서 기록을 만든다. 압축되지 않았으면(번호가 비면) false와 OutError다.
	static bool FromMeshDescription(const FMeshDescription& Mesh, FRocksMeshSourceRecord& OutRecord, TArray<FRocksExportDiagnostic>& OutDiagnostics, FString& OutError);

	// 정적 메시 렌더 LOD(UE 빌드 결과)를 다각형 묶음 = 섹션인 삼각형 메시로 바꾼다(5.1절 baked). 렌더 정점 하나가
	// 정점 하나·코너 하나다. 같은 정점을 두 번 쓰는 삼각형은 뺀다(진단 BakedDegenerateTriangles). SectionSlotNames는
	// 섹션마다의 머티리얼 슬롯 이름(ImportedMaterialSlotName)이다. CPU 데이터가 없으면 false와 OutError다.
	static bool FromStaticMeshLod(const FStaticMeshLODResources& Lod, TConstArrayView<FName> SectionSlotNames, FRocksMeshSourceRecord& OutRecord, TArray<FRocksExportDiagnostic>& OutDiagnostics, FString& OutError);

	// 기록을 5.2절 JSON 객체로 쓰고 위상·속성을 blob으로 저장한다. blob 쓰기 실패만 false다.
	static bool Write(const FRocksMeshSourceRecord& Record, FRocksBundleWriter& Bundle, FRocksJsonWriter& Writer, FString& OutError);

	// 기록 전체(수·위상·속성)의 SHA-256 hex다(자기 시험의 원본 불변 확인용).
	static FString Digest(const FRocksMeshSourceRecord& Record);

	// 메시 설명을 제자리에서 압축한다(번호를 0부터 잇는다).
	static void Compact(FMeshDescription& Mesh);
};
