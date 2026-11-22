#include "RocksAnimSequenceExtractor.h"

#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/AnimSequence.h"
#include "Curves/RichCurve.h"
#include "RocksAssetBody.h"
#include "RocksBundleWriter.h"
#include "RocksExportCommon.h"
#include "RocksJsonWriter.h"
#include "RocksPropertyDumper.h"

namespace RocksAnimSequenceExtractorPrivate
{
	// 본 트랙 성분의 float 수(이동·크기 Vec3, 회전 Quat x·y·z·w)다.
	static constexpr uint32 VectorComponents = 3;
	static constexpr uint32 QuatComponents = 4;

	// transform 커브의 FVectorCurve 축 수다.
	static constexpr int32 VectorCurveAxes = 3;

	// 성분 하나의 키 float 배열을 blob으로 쓴다. 모든 키가 비트 단위로 같으면 키 1개다(7절).
	static bool WriteComponent(const TArray<float>& Values, uint32 Components, FRocksBundleWriter& Bundle, FRocksJsonWriter& Writer, FString& OutError)
	{
		const int32 Keys = Components > 0 ? Values.Num() / int32(Components) : 0;
		bool bConstant = Keys > 0;
		for (int32 Key = 1; bConstant && Key < Keys; ++Key)
		{
			bConstant = FMemory::Memcmp(&Values[Key * Components], &Values[0], Components * sizeof(float)) == 0;
		}

		const int32 Written = bConstant ? 1 : Keys;
		FRocksBlobRef Ref;
		const TConstArrayView64<uint8> Bytes(reinterpret_cast<const uint8*>(Values.GetData()), int64(Written) * Components * sizeof(float));
		if (!Bundle.WriteBlob(Bytes, TEXT("f32"), Components, uint64(Written), Ref, OutError))
		{
			return false;
		}

		Ref.WriteTo(Writer);
		return true;
	}

	static void WriteCurveFlags(const FAnimCurveBase& Curve, FRocksJsonWriter& Writer)
	{
		Writer.Key(TEXT("name"));
		Writer.String(Curve.GetName().ToString());
		Writer.Key(TEXT("flags"));
		Writer.Int(Curve.GetCurveTypeFlags());
	}
}

void FRocksAnimSequenceExtractor::WriteRichCurve(const FRichCurve& Curve, FRocksJsonWriter& Writer)
{
	const UEnum* InterpModes = StaticEnum<ERichCurveInterpMode>();
	const UEnum* TangentModes = StaticEnum<ERichCurveTangentMode>();
	const UEnum* WeightModes = StaticEnum<ERichCurveTangentWeightMode>();
	const UEnum* Extrapolations = StaticEnum<ERichCurveExtrapolation>();
	Writer.BeginObject();
	Writer.Key(TEXT("keys"));
	Writer.BeginArray();
	for (const FRichCurveKey& Key : Curve.GetConstRefOfKeys())
	{
		Writer.BeginObject();
		Writer.Key(TEXT("interpMode"));
		Writer.String(InterpModes->GetNameStringByValue(Key.InterpMode));
		Writer.Key(TEXT("tangentMode"));
		Writer.String(TangentModes->GetNameStringByValue(Key.TangentMode));
		Writer.Key(TEXT("tangentWeightMode"));
		Writer.String(WeightModes->GetNameStringByValue(Key.TangentWeightMode));
		Writer.Key(TEXT("time"));
		Writer.Float(Key.Time);
		Writer.Key(TEXT("value"));
		Writer.Float(Key.Value);
		Writer.Key(TEXT("arriveTangent"));
		Writer.Float(Key.ArriveTangent);
		Writer.Key(TEXT("arriveTangentWeight"));
		Writer.Float(Key.ArriveTangentWeight);
		Writer.Key(TEXT("leaveTangent"));
		Writer.Float(Key.LeaveTangent);
		Writer.Key(TEXT("leaveTangentWeight"));
		Writer.Float(Key.LeaveTangentWeight);
		Writer.EndObject();
	}

	Writer.EndArray();
	Writer.Key(TEXT("preInfinityExtrap"));
	Writer.String(Extrapolations->GetNameStringByValue(Curve.PreInfinityExtrap));
	Writer.Key(TEXT("postInfinityExtrap"));
	Writer.String(Extrapolations->GetNameStringByValue(Curve.PostInfinityExtrap));
	Writer.Key(TEXT("defaultValue"));
	Writer.Float(Curve.GetDefaultValue());
	Writer.EndObject();
}

bool FRocksAnimSequenceExtractor::Extract(UAnimSequence& Sequence, FRocksBundleWriter& Bundle, FRocksAssetBody& OutBody, FString& OutError)
{
	using namespace RocksAnimSequenceExtractorPrivate;

	// settings: 데이터 모델은 data에 풀어 쓰므로 덤프에서 뺀다(데이터 모델 객체는 시퀀서 채널을 담아 크다).
	FRocksPropertyDumper Dumper(&Sequence);
	for (const TCHAR* Name : {TEXT("DataModelInterface"), TEXT("DataModel")})
	{
		Dumper.ExcludeProperty(FindFProperty<FProperty>(UAnimSequenceBase::StaticClass(), Name));
	}

	FRocksJsonWriter Settings(FRocksJsonWriter::EStyle::Pretty, 1);
	Dumper.DumpObject(&Sequence, Settings);
	OutBody.Settings = Settings.FinishFragment();

	FRocksJsonWriter Data(FRocksJsonWriter::EStyle::Pretty, 1);
	Data.BeginObject();
	const IAnimationDataModel* Model = Sequence.GetDataModel();
	if (Model == nullptr)
	{
		OutBody.Diagnostics.Add({TEXT("NoAnimationData"), RocksExport::SeverityError, TEXT("the sequence has no animation data model")});
		Data.EndObject();
		OutBody.Data = Data.FinishFragment();
		OutBody.References = Dumper.GetReferences();
		return true;
	}

	const FFrameRate Rate = Model->GetFrameRate();
	Data.Key(TEXT("frameRate"));
	Data.BeginObject();
	Data.Key(TEXT("numerator"));
	Data.Int(Rate.Numerator);
	Data.Key(TEXT("denominator"));
	Data.Int(Rate.Denominator);
	Data.EndObject();
	Data.Key(TEXT("frameCount"));
	Data.Int(Model->GetNumberOfFrames());
	Data.Key(TEXT("keyCount"));
	Data.Int(Model->GetNumberOfKeys());

	// 본 트랙: UE가 평가한 키별 FTransform(double)을 float 성분 blob 셋으로 쓴다(7절 — 데이터 모델 트랙 순서).
	TArray<FName> TrackNames;
	Model->GetBoneTrackNames(TrackNames);
	Data.Key(TEXT("boneTracks"));
	Data.BeginArray();
	for (const FName TrackName : TrackNames)
	{
		TArray<FTransform> Keys;
		Model->GetBoneTrackTransforms(TrackName, Keys);

		// 성분 blob의 count는 키 수 또는 1이다(7절). 키가 없거나 키 수가 데이터 모델 키 수와 다른 트랙은 그 규칙으로
		// 담을 수 없으므로 빼고 알린다(7.1절 9번).
		if (Keys.IsEmpty() || Keys.Num() != Model->GetNumberOfKeys())
		{
			OutBody.Diagnostics.Add({TEXT("BoneTrackKeyMismatch"), RocksExport::SeverityWarning,
				FString::Printf(TEXT("bone track %s has %d keys, the clip has %d; the track is left out"), *TrackName.ToString(), Keys.Num(), Model->GetNumberOfKeys())});
			continue;
		}

		TArray<float> Translation;
		TArray<float> Rotation;
		TArray<float> Scale;
		for (const FTransform& Key : Keys)
		{
			const FVector3f T(Key.GetTranslation());
			const FQuat4f R(Key.GetRotation());
			const FVector3f S(Key.GetScale3D());
			Translation.Append({T.X, T.Y, T.Z});
			Rotation.Append({R.X, R.Y, R.Z, R.W});
			Scale.Append({S.X, S.Y, S.Z});
		}

		Data.BeginObject();
		Data.Key(TEXT("bone"));
		Data.String(TrackName.ToString());
		Data.Key(TEXT("translation"));
		const bool bTranslation = WriteComponent(Translation, VectorComponents, Bundle, Data, OutError);
		Data.Key(TEXT("rotation"));
		const bool bRotation = bTranslation && WriteComponent(Rotation, QuatComponents, Bundle, Data, OutError);
		Data.Key(TEXT("scale"));
		if (!bRotation || !WriteComponent(Scale, VectorComponents, Bundle, Data, OutError))
		{
			return false;
		}

		Data.EndObject();
	}

	Data.EndArray();

	Data.Key(TEXT("curves"));
	Data.BeginArray();
	for (const FFloatCurve& Curve : Model->GetFloatCurves())
	{
		Data.BeginObject();
		WriteCurveFlags(Curve, Data);
		Data.Key(TEXT("curve"));
		WriteRichCurve(Curve.FloatCurve, Data);
		Data.EndObject();
	}

	Data.EndArray();

	// transform 커브는 커브 9개(이동 x·y·z, 회전 x·y·z, 크기 x·y·z — MeshAnimationAssets 7.5절 5)다.
	Data.Key(TEXT("transformCurves"));
	Data.BeginArray();
	for (const FTransformCurve& Curve : Model->GetTransformCurves())
	{
		Data.BeginObject();
		WriteCurveFlags(Curve, Data);
		Data.Key(TEXT("channels"));
		Data.BeginArray();
		for (const FVectorCurve* Part : {&Curve.TranslationCurve, &Curve.RotationCurve, &Curve.ScaleCurve})
		{
			for (int32 Axis = 0; Axis < VectorCurveAxes; ++Axis)
			{
				WriteRichCurve(Part->FloatCurves[Axis], Data);
			}
		}

		Data.EndArray();
		Data.EndObject();
	}

	Data.EndArray();

	// 이벤트: 계산한 절대 시간·길이(7.3절 — link 방식은 쓰지 않는다)와 종류 클래스, notify 객체 덤프.
	Data.Key(TEXT("events"));
	Data.BeginArray();
	for (const FAnimNotifyEvent& Event : Sequence.Notifies)
	{
		const UObject* Notify = Event.Notify != nullptr ? static_cast<const UObject*>(Event.Notify.Get()) : static_cast<const UObject*>(Event.NotifyStateClass.Get());
		Data.BeginObject();
		Data.Key(TEXT("time"));
		Data.Float(Event.GetTriggerTime());
		Data.Key(TEXT("duration"));
		Data.Float(Event.GetDuration());
		Data.Key(TEXT("track"));
		Data.Int(Event.TrackIndex);
		Data.Key(TEXT("name"));
		Data.String(Event.NotifyName.IsNone() ? FString() : Event.NotifyName.ToString());
		Data.Key(TEXT("type"));
		if (Notify != nullptr)
		{
			Data.String(Notify->GetClass()->GetPathName());
		}
		else
		{
			Data.Null();
		}

		Data.Key(TEXT("triggerChance"));
		Data.Float(Event.NotifyTriggerChance);
		Data.Key(TEXT("triggerOnDedicatedServer"));
		Data.Bool(Event.bTriggerOnDedicatedServer);
		Data.Key(TEXT("triggerOnFollower"));
		Data.Bool(Event.bTriggerOnFollower);
		Data.Key(TEXT("properties"));
		if (Notify != nullptr)
		{
			Dumper.DumpObject(Notify, Data);
		}
		else
		{
			Data.Null();
		}

		Data.EndObject();
	}

	Data.EndArray();

	Data.Key(TEXT("eventTracks"));
	Data.BeginArray();
	for (const FAnimNotifyTrack& Track : Sequence.AnimNotifyTracks)
	{
		Data.String(Track.TrackName.ToString());
	}

	Data.EndArray();

	Data.Key(TEXT("syncMarkers"));
	Data.BeginArray();
	for (const FAnimSyncMarker& Marker : Sequence.AuthoredSyncMarkers)
	{
		Data.BeginObject();
		Data.Key(TEXT("name"));
		Data.String(Marker.MarkerName.ToString());
		Data.Key(TEXT("time"));
		Data.Float(Marker.Time);
		Data.Key(TEXT("track"));
		Data.Int(Marker.TrackIndex);
		Data.EndObject();
	}

	Data.EndArray();
	Data.EndObject();
	OutBody.Data = Data.FinishFragment();
	OutBody.References = Dumper.GetReferences();

	if (Model->GetNumberOfAttributes() > 0)
	{
		OutBody.Diagnostics.Add({TEXT("AnimatedBoneAttributes"), RocksExport::SeverityInfo,
			FString::Printf(TEXT("%d animated bone attributes are not exported"), Model->GetNumberOfAttributes())});
	}

	return true;
}
