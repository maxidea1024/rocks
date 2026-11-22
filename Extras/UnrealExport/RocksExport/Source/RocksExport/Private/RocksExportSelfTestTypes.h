#pragma once

#include "CoreMinimal.h"

#include "RocksExportSelfTestTypes.generated.h"

// UPROPERTY 덤프 자기 시험용 타입이다(12.2절). 덤프 규칙(11.5절)의 각 갈래를 한 구조체에 모은다.

UENUM()
enum class ERocksSelfTestMode : uint8
{
	First,
	Second,
};

USTRUCT()
struct FRocksSelfTestInner
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Value = 4;
};

USTRUCT()
struct FRocksSelfTestStruct
{
	GENERATED_BODY()

	UPROPERTY()
	bool bFlag = true;

	UPROPERTY()
	int32 Count = -7;

	UPROPERTY()
	uint64 Big = MAX_uint64;

	UPROPERTY()
	float Ratio = 0.1f;

	UPROPERTY()
	double Precise = 0.1;

	UPROPERTY()
	float Missing = 0.0f;

	UPROPERTY()
	double Huge = 0.0;

	UPROPERTY()
	FString Text;

	UPROPERTY()
	FName Label = TEXT("Rock");

	UPROPERTY()
	ERocksSelfTestMode Mode = ERocksSelfTestMode::Second;

	UPROPERTY()
	TArray<int32> Values;

	UPROPERTY()
	TSet<FName> Tags;

	UPROPERTY()
	TMap<FString, int32> Weights;

	UPROPERTY()
	FVector Offset = FVector(1.0, 2.5, -3.0);

	UPROPERTY()
	FRocksSelfTestInner Inner;

	UPROPERTY()
	TObjectPtr<UObject> Reference = nullptr;

	UPROPERTY()
	TSoftObjectPtr<UObject> SoftReference;

	UPROPERTY()
	int32 Fixed[2] = {5, 6};

	UPROPERTY(Transient)
	int32 Scratch = 1;

	UPROPERTY()
	int32 Old_DEPRECATED = 2;
};
