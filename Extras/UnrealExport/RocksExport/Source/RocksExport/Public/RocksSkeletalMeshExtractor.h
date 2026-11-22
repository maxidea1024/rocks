#pragma once

#include "CoreMinimal.h"

class FRocksBundleWriter;
class USkeletalMesh;
struct FRocksAssetBody;

// 스킨 메시 추출이다(UnrealExchange.ko.md 6.2절, 6.3절 — 11.6절 FRocksSkeletalMeshExtractor).
//
// - settings: USkeletalMesh UPROPERTY 덤프(LOD 정보·빌드 설정·머티리얼·스켈레톤 참조·소켓·가중치 프로필·morph target
//   객체 등).
// - data.bones: 메시 자체 원본 계층(FRocksSkeletonExtractor::WriteBones와 같은 형식, retargetMode 없음).
// - data.lods[i]: hasSource(HasMeshDescription)·source(메시 설명 사본 — 스킨 가중치·morph 포함)·generated(원본 없음)·
//   baked(생성 LOD의 GetImportedModel LOD 모델, 6.2절). 스킨 메시에는 UE 준비 원본이 없다(6.2절).
//
// 게임 스레드에서 부른다.
class ROCKSEXPORT_API FRocksSkeletalMeshExtractor
{
public:
	// 본문을 만든다. blob 쓰기 실패만 false와 OutError다.
	static bool Extract(USkeletalMesh& Mesh, FRocksBundleWriter& Bundle, FRocksAssetBody& OutBody, FString& OutError);
};
