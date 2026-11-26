#pragma once

#include "CoreMinimal.h"

class FRocksJsonWriter;
class USkeleton;
struct FReferenceSkeleton;
struct FRocksAssetBody;

// 스켈레톤 추출이다(UnrealExchange 6.1절, 6.3절 — 11.6절 FRocksSkeletonExtractor).
//
// - settings: USkeleton UPROPERTY 덤프(11.5절 — BoneTree·가상 본·소켓·슬롯 묶음·리타깃 원본·호환 스켈레톤·blend
//   profile·커브 메타데이터 AssetUserData 등이 이름 그대로).
// - data.bones: 원본 계층(raw — 가상 본 제외)의 본마다 이름·ExportName·부모 번호·기준 pose(double)·이동 리타깃 방식.
// - data.curves: 커브 메타데이터(ForEachCurveMetaData — 이름 UTF-8 byte 순).
//
// 게임 스레드에서 부른다.
class ROCKSEXPORT_API FRocksSkeletonExtractor
{
public:
	static void Extract(USkeleton& Skeleton, FRocksAssetBody& OutBody);

	// 원본 본 계층을 JSON 배열로 쓴다(6.3절 1번). RetargetSource가 있으면 본마다 retargetMode를 더한다.
	static void WriteBones(const FReferenceSkeleton& Bones, const USkeleton* RetargetSource, FRocksJsonWriter& Writer);
};
