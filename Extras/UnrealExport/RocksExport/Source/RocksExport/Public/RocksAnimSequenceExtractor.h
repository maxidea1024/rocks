#pragma once

#include "CoreMinimal.h"

class FRocksBundleWriter;
class FRocksJsonWriter;
class UAnimSequence;
struct FRichCurve;
struct FRocksAssetBody;

// 애니메이션 클립 추출이다(UnrealExchange 7절, 7.1절 — 11.6절 FRocksAnimSequenceExtractor).
//
// - settings: UAnimSequence UPROPERTY 덤프(additive·root motion·interpolation·RateScale·bLoop·PlatformTargetFrameRate·
//   RetargetSource·RefPoseSeq·notify 등). 데이터 모델(DataModelInterface·DataModel)은 data에 풀어 쓰므로 뺀다.
// - data: 비율·프레임 수·키 수, 본 트랙(GetBoneTrackTransforms를 float로 — 이동·회전·크기 blob, 모든 키가 비트 단위로
//   같은 성분은 키 1개), float 커브·transform 커브(FRichCurve 키·외삽·기본값·플래그), 이벤트(notify — 계산한 절대
//   시간·길이·트랙·이름·종류 클래스·확률·트리거 플래그·notify 객체 덤프), 이벤트 트랙 이름, 동기 마커.
// - 진단: animated bone attribute는 AnimatedBoneAttributes(info, 개수), 데이터 모델이 없으면 NoAnimationData(error).
//
// 게임 스레드에서 부른다.
class ROCKSEXPORT_API FRocksAnimSequenceExtractor
{
public:
	// 본문을 만든다. blob 쓰기 실패만 false와 OutError다.
	static bool Extract(UAnimSequence& Sequence, FRocksBundleWriter& Bundle, FRocksAssetBody& OutBody, FString& OutError);

	// FRichCurve 하나를 7.1절 커브 객체로 쓴다({ "keys", "preInfinityExtrap", "postInfinityExtrap", "defaultValue" }).
	static void WriteRichCurve(const FRichCurve& Curve, FRocksJsonWriter& Writer);
};
