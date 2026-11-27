#pragma once

#include "CoreMinimal.h"

class FRocksBundleWriter;
class FRocksJsonWriter;
class UMaterialInterface;
struct FRocksAssetBody;

// 머티리얼 굽기 대체 기록이다(UnrealExchange 8.4·8.9절 1·2번 — UX6a). MaterialBaking 모듈로 Surface 영역 머티리얼·
// 인스턴스의 속성 9개를 기본 quad 위 UV 공간에 구워 data.baked에 쓴다.
//
// - 속성 순서: BaseColor·Normal·Metallic·Roughness·Specular·EmissiveColor·Opacity·OpacityMask·AmbientOcclusion.
// - EmissiveColor는 HDR 원값(RGBA16F, 요청 크기 그대로), 나머지는 8비트(BGRA8, 알파 255, 상수면 1×1까지 줄어든다).
// - Surface가 아닌 영역은 굽지 않는다(info BakeSkipped, baked 키 없음). 결과 배열이 크기와 맞지 않으면 warning
//   BakeFailed이고 baked는 null이다(error면 Rocks 리더가 에셋 전체를 거부한다).
//
// 렌더링이 되는 프로세스(-AllowCommandletRendering, -nullrhi 없음)의 게임 스레드에서 부른다. MaterialBaking 모듈의 전역
// 상태를 굽는 동안만 바꾼다: linear는 굽기 전 값으로 되돌리고 EmissiveHDR는 기본값 false로 둔다.
class ROCKSEXPORT_API FRocksMaterialBaker
{
public:
	// 굽기에 렌더링을 쓸 수 있는가다(FApp::CanEverRender — 8.9절 1번).
	static bool CanBake();

	// Writer의 열린 data 객체에 "baked" 키와 값을 쓰거나(Surface 영역), 쓰지 않는다(그 밖 — 진단만 더한다).
	// blob 쓰기가 실패하면 false와 OutError다(내보내기 전체 실패). BakeSize는 1–MaxBakeSize다.
	static bool WriteBaked(UMaterialInterface& Material, int32 BakeSize, FRocksBundleWriter& Bundle, FRocksJsonWriter& Writer, FRocksAssetBody& Body,
		FString& OutError);

	// bundle.json environment 값(굽기에 쓴 RHI·GPU·드라이버 이름 — 3.4절)을 쓴다.
	static void WriteEnvironment(FRocksJsonWriter& Writer);
};
