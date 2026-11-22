#include "RocksStaticMeshExtractor.h"

#include "Engine/StaticMesh.h"
#include "MeshDescription.h"
#include "MeshDescriptionHelper.h"
#include "PhysicsEngine/BodySetup.h"
#include "RocksAssetBody.h"
#include "RocksExportCommon.h"
#include "RocksJsonWriter.h"
#include "RocksMeshDescriptionWriter.h"
#include "RocksPropertyDumper.h"
#include "StaticMeshResources.h"

namespace RocksStaticMeshExtractorPrivate
{
	// 메시 설명 하나를 MeshSource 기록으로 쓴다. 기록을 만들지 못하면(압축 실패) 진단과 null이다.
	static bool WriteMesh(const FMeshDescription& Mesh, const TCHAR* What, int32 Lod, FRocksBundleWriter& Bundle, FRocksJsonWriter& Writer, FRocksAssetBody& Body, FString& OutError)
	{
		FRocksMeshSourceRecord Record;
		FString Problem;
		if (!FRocksMeshDescriptionWriter::FromMeshDescription(Mesh, Record, Body.Diagnostics, Problem))
		{
			Body.Diagnostics.Add({TEXT("MeshSourceUnavailable"), RocksExport::SeverityError, FString::Printf(TEXT("LOD %d %s: %s"), Lod, What, *Problem)});
			Writer.Null();
			return true;
		}

		return FRocksMeshDescriptionWriter::Write(Record, Bundle, Writer, OutError);
	}

	// 원본이 있다는데 사본을 얻지 못한 LOD의 진단이다(5.4절 6번 — error, 그 기록은 null). baked로 대신하지 않는다(5.4절 10번).
	static void ReportCloneFailure(int32 Lod, const TCHAR* What, FRocksAssetBody& Body)
	{
		Body.Diagnostics.Add({TEXT("MeshSourceUnavailable"), RocksExport::SeverityError,
			FString::Printf(TEXT("LOD %d %s: the mesh description could not be read"), Lod, What)});
	}

	// LOD 하나의 data 항목을 쓴다(5.1–5.3절).
	static bool WriteLod(UStaticMesh& Mesh, int32 Lod, FRocksBundleWriter& Bundle, FRocksJsonWriter& Writer, FRocksAssetBody& Body, FString& OutError)
	{
		const bool bHasSource = Mesh.IsMeshDescriptionValid(Lod);
		Writer.BeginObject();
		Writer.Key(TEXT("hasSource"));
		Writer.Bool(bHasSource);

		Writer.Key(TEXT("source"));
		FMeshDescription Source;
		if (bHasSource && Mesh.CloneMeshDescription(Lod, Source))
		{
			FRocksMeshDescriptionWriter::Compact(Source);
			if (!WriteMesh(Source, TEXT("source"), Lod, Bundle, Writer, Body, OutError))
			{
				return false;
			}
		}
		else
		{
			if (bHasSource)
			{
				ReportCloneFailure(Lod, TEXT("source"), Body);
			}

			Writer.Null();
		}

		// UE 준비 원본: 메시 설명 사본과 빌드 설정 사본에 UE 빌더 전처리를 적용한다(5.3절 — 원본 에셋 설정을
		// 바꾸지 않는다. SetupRenderMeshDescription은 설정의 lightmap 번호를 고친다).
		FMeshBuildSettings Settings = Mesh.GetSourceModel(Lod).BuildSettings;
		FMeshDescription Prepared;
		const bool bPrepared = bHasSource && Mesh.CloneMeshDescription(Lod, Prepared);
		Writer.Key(TEXT("prepared"));
		if (bPrepared)
		{
			FMeshDescriptionHelper Helper(&Settings);
			Helper.SetupRenderMeshDescription(&Mesh, Prepared, Mesh.IsNaniteEnabled(), /*bNeedTangents*/ true);
			FRocksMeshDescriptionWriter::Compact(Prepared);
			if (!WriteMesh(Prepared, TEXT("prepared"), Lod, Bundle, Writer, Body, OutError))
			{
				return false;
			}
		}
		else
		{
			if (bHasSource)
			{
				ReportCloneFailure(Lod, TEXT("prepared"), Body);
			}

			Writer.Null();
		}

		Writer.Key(TEXT("preparedBuild"));
		if (bPrepared)
		{
			Writer.BeginObject();
			Writer.Key(TEXT("SrcLightmapIndex"));
			Writer.Int(Settings.SrcLightmapIndex);
			Writer.Key(TEXT("DstLightmapIndex"));
			Writer.Int(Settings.DstLightmapIndex);
			Writer.EndObject();
		}
		else
		{
			Writer.Null();
		}

		// UE 생성 LOD 판정(StaticMesh.cpp IsMeshDescriptionValid·IsReductionActive)과 그 렌더 데이터(5.1절 baked).
		const bool bGenerated = !bHasSource || Mesh.IsReductionActive(Lod);
		Writer.Key(TEXT("generated"));
		Writer.Bool(bGenerated);
		Writer.Key(TEXT("baked"));
		if (!bGenerated)
		{
			Writer.Null();
			Writer.EndObject();
			return true;
		}

		const FStaticMeshRenderData* RenderData = Mesh.GetRenderData();
		if (RenderData == nullptr || !RenderData->LODResources.IsValidIndex(Lod))
		{
			Body.Diagnostics.Add({TEXT("BakedUnavailable"), RocksExport::SeverityWarning, FString::Printf(TEXT("LOD %d is generated but has no render data"), Lod)});
			Writer.Null();
			Writer.EndObject();
			return true;
		}

		const FStaticMeshLODResources& Resources = RenderData->LODResources[Lod];
		TArray<FName> SlotNames;
		for (const FStaticMeshSection& Section : Resources.Sections)
		{
			const TArray<FStaticMaterial>& Materials = Mesh.GetStaticMaterials();
			SlotNames.Add(Materials.IsValidIndex(Section.MaterialIndex) ? Materials[Section.MaterialIndex].ImportedMaterialSlotName : FName(NAME_None));
		}

		FRocksMeshSourceRecord Baked;
		FString Problem;
		if (!FRocksMeshDescriptionWriter::FromStaticMeshLod(Resources, SlotNames, Baked, Body.Diagnostics, Problem))
		{
			Body.Diagnostics.Add({TEXT("BakedUnavailable"), RocksExport::SeverityWarning, FString::Printf(TEXT("LOD %d: %s"), Lod, *Problem)});
			Writer.Null();
		}
		else if (!FRocksMeshDescriptionWriter::Write(Baked, Bundle, Writer, OutError))
		{
			return false;
		}

		Writer.EndObject();
		return true;
	}
}

bool FRocksStaticMeshExtractor::Extract(UStaticMesh& Mesh, FRocksBundleWriter& Bundle, FRocksAssetBody& OutBody, FString& OutError)
{
	FRocksPropertyDumper Dumper(&Mesh);
	FRocksJsonWriter Settings(FRocksJsonWriter::EStyle::Pretty, 1);
	Dumper.DumpObject(&Mesh, Settings);
	OutBody.Settings = Settings.FinishFragment();
	OutBody.References = Dumper.GetReferences();

	if (Mesh.IsHiResMeshDescriptionValid())
	{
		OutBody.Diagnostics.Add({TEXT("HiResSourceSkipped"), RocksExport::SeverityInfo, TEXT("the Nanite high-resolution source is not exported in format v1")});
	}

	if (const UBodySetup* BodySetup = Mesh.GetBodySetup())
	{
		const FKAggregateGeom& Geometry = BodySetup->AggGeom;
		const int32 LevelSets = Geometry.LevelSetElems.Num() + Geometry.SkinnedLevelSetElems.Num() + Geometry.MLLevelSetElems.Num() + Geometry.SkinnedTriangleMeshElems.Num();
		if (LevelSets > 0)
		{
			OutBody.Diagnostics.Add({TEXT("LevelSetCollision"), RocksExport::SeverityInfo,
				FString::Printf(TEXT("%d level set or skinned collision shapes; Rocks import does not take them"), LevelSets)});
		}
	}

	FRocksJsonWriter Data(FRocksJsonWriter::EStyle::Pretty, 1);
	Data.BeginObject();
	Data.Key(TEXT("lods"));
	Data.BeginArray();
	for (int32 Lod = 0; Lod < Mesh.GetNumSourceModels(); ++Lod)
	{
		if (!RocksStaticMeshExtractorPrivate::WriteLod(Mesh, Lod, Bundle, Data, OutBody, OutError))
		{
			OutError = FString::Printf(TEXT("%s: %s"), *Mesh.GetPathName(), *OutError);
			return false;
		}
	}

	Data.EndArray();

	// 충돌: UStaticMesh::BodySetup은 transient UPROPERTY(따로 직렬화)라 settings 덤프에 들지 않으므로 BodySetup 객체를
	// 따로 덤프한다(AggGeom 형상, CollisionTraceFlag, bDoubleSidedGeometry, 물리 머티리얼 등 — 5.1절).
	Data.Key(TEXT("collision"));
	if (UBodySetup* BodySetup = Mesh.GetBodySetup())
	{
		FRocksPropertyDumper CollisionDumper(BodySetup);
		CollisionDumper.DumpObject(BodySetup, Data);
		OutBody.References.Append(CollisionDumper.GetReferences());
	}
	else
	{
		Data.Null();
	}

	Data.EndObject();
	OutBody.Data = Data.FinishFragment();
	return true;
}
