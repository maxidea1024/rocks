#pragma once

#include "CoreMinimal.h"
#include "Engine/TextureDefines.h"

class FRocksBundleWriter;
class UTexture;
struct FRocksAssetBody;

// 텍스처 추출이다(UnrealExchange 4절, 4.5절 — 11.6절 FRocksTextureExtractor).
//
// - settings: 텍스처 객체(UTexture와 차원별 클래스)의 UPROPERTY 덤프(11.5절). `Source`는 data에 쓰므로 뺀다.
// - data.source: FTextureSource의 형식·크기·압축·손실 원본·위도경도 표시·층별 형식과 감마, 블록별 mip 사슬.
//   mip 사슬은 GetMipData(nullptr)로 한 번 풀고 (block, mip, layer)마다 그 view를 blob 하나로 쓴다(4.1절).
// - 원본이 없거나(동적·쿠킹 전용) 모르는 형식이면 data.source는 null이고 error 진단(NoSourceData·
//   UnsupportedSourceFormat)을 단다. JPEG 원본이면 lossyOrigin "jpeg"와 info 진단 LossyOrigin이다(4.3절).
//
// 게임 스레드에서 부른다(UObject·FTextureSource 접근).
class ROCKSEXPORT_API FRocksTextureExtractor
{
public:
	// 교환 클래스 이름이 텍스처 5종(2절)인가다.
	static bool IsTextureClass(const FString& ExchangeClass);

	// 본문을 만든다. blob 쓰기 실패(디스크·크기 불일치)만 false와 OutError다. 에셋 수준 문제는 진단으로 남긴다.
	static bool Extract(UTexture& Texture, FRocksBundleWriter& Bundle, FRocksAssetBody& OutBody, FString& OutError);

	// 원본 형식의 교환 이름(TSF_ 접두를 뗀 것)과 blob 배치(3.3절)다. 교환에서 받지 않는 값이면 false다.
	static bool DescribeSourceFormat(ETextureSourceFormat Format, FString& OutName, FString& OutLayout, uint32& OutComponents);
};
