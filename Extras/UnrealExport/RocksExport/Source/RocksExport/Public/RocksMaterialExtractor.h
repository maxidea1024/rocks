#pragma once

#include "CoreMinimal.h"

class UObject;
struct FRocksAssetBody;

// 머티리얼 계열 추출이다(UnrealExchange 8.1·8.2절, 8.6절 — 11.6절 FRocksMaterialExtractor). 번역(8.5절)·
// Substrate 번역(8.3절)·굽기(8.4절)는 하지 않는다. Substrate 그래프도 에디터 메모리 그대로 덤프한다.
//
// - Material·MaterialFunction: settings = 객체 UPROPERTY 덤프(그래프 묶음 FMaterialExpressionCollection과 핀은 뺀다),
//   data.nodes = 그래프 노드(expression과 comment)마다 guid·이름·클래스·위치·입력(연결 대상 노드 이름·출력 번호·
//   mask)·출력·properties 덤프. 노드 식별자는 객체 이름이다(guid는 겹칠 수 있다). Material은 data.outputs =
//   머티리얼 출력 핀(bUseMaterialAttributes에 따라 MaterialAttributes 하나 또는 개별 핀, 연결 또는 미연결 상수)이다.
// - MaterialInstanceConstant: settings = 덤프(부모·override 9종·BasePropertyOverrides·static 파라미터 등),
//   data.resolved = GetAllParametersOfType로 해석한 최종 파라미터 값(파라미터 정보 순).
// - MaterialFunctionInstance·MaterialParameterCollection: settings 덤프만, data는 빈 객체다.
// - 진단: Custom 노드의 include 파일은 추출하지 않는다(CustomIncludeSkipped, info).
//
// 게임 스레드에서 부른다.
class ROCKSEXPORT_API FRocksMaterialExtractor
{
public:
	// 교환 클래스 이름이 머티리얼 계열 5종(2절)인가다.
	static bool IsMaterialClass(const FString& ExchangeClass);

	static void Extract(UObject& Asset, FRocksAssetBody& OutBody);
};
