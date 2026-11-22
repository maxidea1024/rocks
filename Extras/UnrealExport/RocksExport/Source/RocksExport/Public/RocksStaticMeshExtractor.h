#pragma once

#include "CoreMinimal.h"

class FRocksBundleWriter;
class UStaticMesh;
struct FRocksAssetBody;

// 정적 메시 추출이다(UnrealExchange.ko.md 5절, 5.4절 — 11.6절 FRocksStaticMeshExtractor).
//
// - settings: UStaticMesh UPROPERTY 덤프(11.5절). LOD별 빌드·축소·화면 크기(SourceModels), 슬롯, 섹션 정보, 소켓,
//   Nanite 설정 등이 이름 그대로 들어간다.
// - data.collision: BodySetup 객체의 UPROPERTY 덤프(AggGeom 형상·CollisionTraceFlag·bDoubleSidedGeometry 등). UE에서
//   BodySetup은 transient UPROPERTY라 settings 덤프에 들지 않는다. 없으면 null이다.
// - data.lods[i]: hasSource·source(순수 원본)·prepared(UE 빌드 전처리를 적용한 사본, 5.3절)·preparedBuild(사본
//   설정이 고친 lightmap 번호)·generated(UE 생성 LOD 판정)·baked(생성 LOD의 UE 렌더 데이터, 5.1절).
// - 원본 설정은 바꾸지 않는다: prepared는 메시 설명 사본과 빌드 설정 사본에 적용한다(5.3절).
// - 진단: Nanite 고해상도 원본은 HiResSourceSkipped(info), level set 계열 충돌은 LevelSetCollision(info, 개수).
//
// 게임 스레드에서 부른다.
class ROCKSEXPORT_API FRocksStaticMeshExtractor
{
public:
	// 본문을 만든다. blob 쓰기 실패만 false와 OutError다. 에셋 수준 문제는 진단으로 남긴다.
	static bool Extract(UStaticMesh& Mesh, FRocksBundleWriter& Bundle, FRocksAssetBody& OutBody, FString& OutError);
};
