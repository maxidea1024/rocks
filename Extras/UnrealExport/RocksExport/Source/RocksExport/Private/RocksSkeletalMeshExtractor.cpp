#include "RocksSkeletalMeshExtractor.h"

#include "Animation/MorphTarget.h"
#include "Engine/SkeletalMesh.h"
#include "MeshDescription.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#include "RocksAssetBody.h"
#include "RocksExportCommon.h"
#include "RocksJsonWriter.h"
#include "RocksMeshDescriptionWriter.h"
#include "RocksPropertyDumper.h"
#include "RocksSkeletonExtractor.h"

namespace RocksSkeletalMeshExtractorPrivate
{
	// 생성 LOD의 LOD 모델(6.2절 baked)을 쓴다. 없으면 진단과 null이다.
	static bool WriteBaked(USkeletalMesh& Mesh, int32 Lod, FRocksBundleWriter& Bundle, FRocksJsonWriter& Writer, FRocksAssetBody& Body, FString& OutError)
	{
		const FSkeletalMeshModel* Model = Mesh.GetImportedModel();
		if (Model == nullptr || !Model->LODModels.IsValidIndex(Lod))
		{
			Body.Diagnostics.Add({TEXT("BakedUnavailable"), RocksExport::SeverityWarning, FString::Printf(TEXT("LOD %d is generated but has no LOD model"), Lod)});
			Writer.Null();
			return true;
		}

		const FSkeletalMeshLODModel& LodModel = Model->LODModels[Lod];
		const TArray<FSkeletalMaterial>& Materials = Mesh.GetMaterials();
		TArray<FName> SlotNames;
		for (const FSkelMeshSection& Section : LodModel.Sections)
		{
			SlotNames.Add(Materials.IsValidIndex(Section.MaterialIndex) ? Materials[Section.MaterialIndex].ImportedMaterialSlotName : FName(NAME_None));
		}

		TArray<const UMorphTarget*> Morphs;
		for (const TObjectPtr<UMorphTarget>& Morph : Mesh.GetMorphTargets())
		{
			Morphs.Add(Morph.Get());
		}

		Morphs.Sort([](const UMorphTarget& Left, const UMorphTarget& Right) { return FRocksJsonWriter::CompareUtf8(Left.GetName(), Right.GetName()) < 0; });
		FRocksMeshSourceRecord Record;
		FString Problem;
		if (!FRocksMeshDescriptionWriter::FromSkeletalLodModel(LodModel, SlotNames, Morphs, Lod, Record, Body.Diagnostics, Problem))
		{
			Body.Diagnostics.Add({TEXT("BakedUnavailable"), RocksExport::SeverityWarning, FString::Printf(TEXT("LOD %d: %s"), Lod, *Problem)});
			Writer.Null();
			return true;
		}

		return FRocksMeshDescriptionWriter::Write(Record, Bundle, Writer, OutError);
	}

	static bool WriteLod(USkeletalMesh& Mesh, int32 Lod, FRocksBundleWriter& Bundle, FRocksJsonWriter& Writer, FRocksAssetBody& Body, FString& OutError)
	{
		const bool bHasSource = Mesh.HasMeshDescription(Lod);
		Writer.BeginObject();
		Writer.Key(TEXT("hasSource"));
		Writer.Bool(bHasSource);
		Writer.Key(TEXT("source"));
		FMeshDescription Source;
		if (bHasSource && Mesh.CloneMeshDescription(Lod, Source))
		{
			FRocksMeshDescriptionWriter::Compact(Source);
			FRocksMeshSourceRecord Record;
			FString Problem;
			if (!FRocksMeshDescriptionWriter::FromMeshDescription(Source, Record, Body.Diagnostics, Problem))
			{
				Body.Diagnostics.Add({TEXT("MeshSourceUnavailable"), RocksExport::SeverityError, FString::Printf(TEXT("LOD %d source: %s"), Lod, *Problem)});
				Writer.Null();
			}
			else if (!FRocksMeshDescriptionWriter::Write(Record, Bundle, Writer, OutError))
			{
				return false;
			}
		}
		else
		{
			// 원본이 있다는데 사본을 얻지 못하면 error다(5.4절 6번과 같은 규칙 — 6.3절 9번).
			if (bHasSource)
			{
				Body.Diagnostics.Add({TEXT("MeshSourceUnavailable"), RocksExport::SeverityError,
					FString::Printf(TEXT("LOD %d source: the mesh description could not be read"), Lod)});
			}

			Writer.Null();
		}

		// UE가 만든 LOD다: 원본이 없거나(SkeletalMesh.h GetMeshDescription이 nullptr) 원본에 축소를 적용한다
		// (inline reduction — SkeletalMesh.cpp IsReductionActive). 그때 렌더 기준은 LOD 모델이므로 baked로 쓴다(6.3절 10번).
		const bool bGenerated = !bHasSource || Mesh.IsReductionActive(Lod);
		Writer.Key(TEXT("generated"));
		Writer.Bool(bGenerated);
		Writer.Key(TEXT("baked"));
		if (!bGenerated)
		{
			Writer.Null();
		}
		else if (!WriteBaked(Mesh, Lod, Bundle, Writer, Body, OutError))
		{
			return false;
		}

		Writer.EndObject();
		return true;
	}
}

bool FRocksSkeletalMeshExtractor::Extract(USkeletalMesh& Mesh, FRocksBundleWriter& Bundle, FRocksAssetBody& OutBody, FString& OutError)
{
	FRocksJsonWriter Data(FRocksJsonWriter::EStyle::Pretty, 1);
	Data.BeginObject();
	Data.Key(TEXT("bones"));
	FRocksSkeletonExtractor::WriteBones(Mesh.GetRefSkeleton(), nullptr, Data);
	Data.Key(TEXT("lods"));
	Data.BeginArray();
	for (int32 Lod = 0; Lod < Mesh.GetLODNum(); ++Lod)
	{
		if (!RocksSkeletalMeshExtractorPrivate::WriteLod(Mesh, Lod, Bundle, Data, OutBody, OutError))
		{
			OutError = FString::Printf(TEXT("%s: %s"), *Mesh.GetPathName(), *OutError);
			return false;
		}
	}

	Data.EndArray();

	// 머티리얼 슬롯: USkeletalMesh::Materials는 transient UPROPERTY(에셋 Serialize가 따로 저장)라 settings 덤프에 들지
	// 않으므로 FSkeletalMaterial마다 구조체 덤프로 따로 쓴다(6.3절 13번 — 정적 메시 BodySetup과 같은 방식).
	FRocksPropertyDumper MaterialsDumper(&Mesh);
	Data.Key(TEXT("materials"));
	Data.BeginArray();
	for (const FSkeletalMaterial& Material : Mesh.GetMaterials())
	{
		MaterialsDumper.DumpStruct(FSkeletalMaterial::StaticStruct(), &Material, Data);
	}

	Data.EndArray();
	Data.EndObject();
	OutBody.Data = Data.FinishFragment();

	// settings는 LOD를 읽은 뒤 덤프한다. UE는 메시 설명을 처음 읽을 때 SourceModels의 캐시(삼각형·정점 수, bounds,
	// 가중치 프로필·morph 이름 등)를 채우므로, 먼저 덤프하면 같은 에셋이 이미 읽혔는지에 따라 bytes가 달라진다(6.3절).
	FRocksPropertyDumper Dumper(&Mesh);
	FRocksJsonWriter Settings(FRocksJsonWriter::EStyle::Pretty, 1);
	Dumper.DumpObject(&Mesh, Settings);
	OutBody.Settings = Settings.FinishFragment();
	OutBody.References = Dumper.GetReferences();
	// 같은 경로는 의존을 합칠 때 하나가 된다(Hard 우선 — RocksExporter MergeReferences).
	OutBody.References.Append(MaterialsDumper.GetReferences());
	return true;
}
