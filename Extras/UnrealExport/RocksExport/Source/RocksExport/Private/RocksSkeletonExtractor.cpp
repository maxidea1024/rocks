#include "RocksSkeletonExtractor.h"

#include "Animation/AnimCurveMetadata.h"
#include "Animation/Skeleton.h"
#include "ReferenceSkeleton.h"
#include "RocksAssetBody.h"
#include "RocksJsonWriter.h"
#include "RocksPropertyDumper.h"

namespace RocksSkeletonExtractorPrivate
{
	static void WriteVector(FRocksJsonWriter& Writer, const FVector& Value)
	{
		Writer.BeginArray();
		Writer.Double(Value.X);
		Writer.Double(Value.Y);
		Writer.Double(Value.Z);
		Writer.EndArray();
	}

	struct FCurve
	{
		FName Name;
		FCurveMetaData Data;
	};
}

void FRocksSkeletonExtractor::WriteBones(const FReferenceSkeleton& Bones, const USkeleton* RetargetSource, FRocksJsonWriter& Writer)
{
	using namespace RocksSkeletonExtractorPrivate;

	const TArray<FMeshBoneInfo>& Infos = Bones.GetRawRefBoneInfo();
	const TArray<FTransform>& Poses = Bones.GetRawRefBonePose();
	const UEnum* RetargetModes = StaticEnum<EBoneTranslationRetargetingMode::Type>();
	Writer.BeginArray();
	for (int32 Bone = 0; Bone < Infos.Num(); ++Bone)
	{
		const FTransform& Pose = Poses[Bone];
		Writer.BeginObject();
		Writer.Key(TEXT("name"));
		Writer.String(Infos[Bone].Name.ToString());
		Writer.Key(TEXT("exportName"));
		Writer.String(Infos[Bone].ExportName);
		Writer.Key(TEXT("parent"));
		Writer.Int(Infos[Bone].ParentIndex);
		Writer.Key(TEXT("pose"));
		Writer.BeginObject();
		Writer.Key(TEXT("translation"));
		WriteVector(Writer, Pose.GetTranslation());
		Writer.Key(TEXT("rotation"));
		Writer.BeginArray();
		const FQuat Rotation = Pose.GetRotation();
		Writer.Double(Rotation.X);
		Writer.Double(Rotation.Y);
		Writer.Double(Rotation.Z);
		Writer.Double(Rotation.W);
		Writer.EndArray();
		Writer.Key(TEXT("scale"));
		WriteVector(Writer, Pose.GetScale3D());
		Writer.EndObject();
		if (RetargetSource != nullptr)
		{
			Writer.Key(TEXT("retargetMode"));
			Writer.String(RetargetModes->GetNameStringByValue(int64(RetargetSource->GetBoneTranslationRetargetingMode(Bone))));
		}

		Writer.EndObject();
	}

	Writer.EndArray();
}

void FRocksSkeletonExtractor::Extract(USkeleton& Skeleton, FRocksAssetBody& OutBody)
{
	using namespace RocksSkeletonExtractorPrivate;

	FRocksPropertyDumper Dumper(&Skeleton);
	FRocksJsonWriter Settings(FRocksJsonWriter::EStyle::Pretty, 1);
	Dumper.DumpObject(&Skeleton, Settings);
	OutBody.Settings = Settings.FinishFragment();
	OutBody.References = Dumper.GetReferences();

	TArray<FCurve> Curves;
	Skeleton.ForEachCurveMetaData([&Curves](FName Name, const FCurveMetaData& Data) { Curves.Add({Name, Data}); });
	Curves.Sort([](const FCurve& Left, const FCurve& Right) { return FRocksJsonWriter::CompareUtf8(Left.Name.ToString(), Right.Name.ToString()) < 0; });

	FRocksJsonWriter Data(FRocksJsonWriter::EStyle::Pretty, 1);
	Data.BeginObject();
	Data.Key(TEXT("bones"));
	WriteBones(Skeleton.GetReferenceSkeleton(), &Skeleton, Data);
	Data.Key(TEXT("curves"));
	Data.BeginArray();
	for (const FCurve& Curve : Curves)
	{
		Data.BeginObject();
		Data.Key(TEXT("name"));
		Data.String(Curve.Name.ToString());
		Data.Key(TEXT("isMaterial"));
		Data.Bool(Curve.Data.Type.bMaterial);
		Data.Key(TEXT("isMorphTarget"));
		Data.Bool(Curve.Data.Type.bMorphtarget);
		Data.Key(TEXT("linkedBones"));
		Data.BeginArray();
		for (const FBoneReference& Bone : Curve.Data.LinkedBones)
		{
			Data.String(Bone.BoneName.ToString());
		}

		Data.EndArray();
		Data.Key(TEXT("maxLod"));
		Data.UInt(Curve.Data.MaxLOD);
		Data.EndObject();
	}

	Data.EndArray();
	Data.EndObject();
	OutBody.Data = Data.FinishFragment();
}
