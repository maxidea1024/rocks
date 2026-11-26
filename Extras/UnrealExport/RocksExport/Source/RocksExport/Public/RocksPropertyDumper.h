#pragma once

#include "CoreMinimal.h"

class FRocksJsonWriter;

// UPROPERTY 덤프 writer다(UnrealExchange 11.5절, 11.7절). FJsonObjectConverter를 쓰지 않는다(키 첫 글자
// 변환과 숫자 서식 때문).
//
// 규칙:
// - 객체 하나 = JSON 객체 하나, 키는 UE 프로퍼티 이름 그대로, 순서는 TFieldIterator 순(그 클래스 선언부터 상위
//   클래스 쪽으로)이다. 고정 배열(ArrayDim > 1)은 배열이다. 예약 키(blob·objectPath·subobject — 3.3절)와 같은
//   프로퍼티 이름은 앞에 '@'를 붙인다(UE 이름에 '@'는 쓸 수 없다 — 리더가 blob 참조·객체 참조로 오인하지 않게).
// - Transient·Deprecated 프로퍼티는 건너뛴다. 제외 구조체(기본 FExpressionInput 계열 — 8.1절 inputs로 옮긴다)를
//   타입으로 갖는 프로퍼티도 건너뛴다.
// - 숫자는 11.4절(float %.9g, double %.17g, 정수 그대로), bool, 문자열(FString·FName·FText 원문), enum은 이름
//   문자열(값이 enum에 없으면 정수)이다.
// - 객체 참조: null은 null, 덤프 루트를 Outer 사슬에 둔 하위 객체는 처음 한 번만
//   { "subobject": 루트 기준 경로, "class": 클래스 경로, "properties": {…} }로 펼치고 다시 만나면
//   { "subobject": 경로 }다. 그 밖은 { "objectPath": 전체 경로 }다. soft 참조도 { "objectPath" }다(저장된 경로
//   그대로 — 옛 경로면 bundle.json redirects가 잇는다). weak·lazy 참조는 Soft로 모은다(UE가 대상을 함께 로드하지 않는다).
// - TArray는 배열, TSet은 원소, TMap은 { "key", "value" } 객체의 배열이다. TSet·TMap은 원소(키)의 compact
//   JSON 표기의 UTF-8 byte 순으로 정렬한다(3.2절 맵 정렬).
// - optional은 값이 있으면 그 값(같은 규칙), 없으면 null이다. UTF-8·ANSI 문자열은 문자열이다.
// - 그 밖의 프로퍼티(delegate·field path 등)는 UE ExportText 문자열이다(11.7절 24번).
//
// 참조한 에셋(하위 객체가 아닌 객체·soft 참조의 최상위 에셋 경로)은 GetReferences로 모은다(Hard 또는 Soft).
// /Script 네이티브 객체와 transient 패키지 객체는 모으지 않는다. 호출자는 이것을 에셋 JSON dependencies에
// 더한다(3.5절).
//
// 사용 예:
//   FRocksPropertyDumper Dumper(Asset);
//   Writer.Key(TEXT("settings"));
//   Dumper.DumpObject(Asset, Writer);
//
// 게임 스레드에서 쓴다(UObject 접근).
class ROCKSEXPORT_API FRocksPropertyDumper
{
public:
	// 모은 참조 하나다. Path는 최상위 에셋 객체 경로(/Game/A.A)다.
	struct FReference
	{
		FString Path;
		bool bHard = false;
	};

	explicit FRocksPropertyDumper(const UObject* InDumpRoot);

	// 덤프에서 뺄 구조체를 더한다(파생 구조체 포함).
	void ExcludeStruct(const UScriptStruct* Struct);

	// 덤프에서 뺄 프로퍼티 하나를 더한다(다른 키로 따로 쓰는 값 — 예: 텍스처 Source는 data에 쓴다, 4.5절).
	void ExcludeProperty(const FProperty* Property);

	// Object 클래스의 프로퍼티 전부를 JSON 객체 하나로 쓴다.
	void DumpObject(const UObject* Object, FRocksJsonWriter& Writer);

	// Struct 인스턴스 Data의 프로퍼티를 JSON 객체 하나로 쓴다.
	void DumpStruct(const UStruct* Struct, const void* Data, FRocksJsonWriter& Writer);

	// 모은 참조다(경로 UTF-8 byte 순, 경로마다 하나 — Hard·Soft 둘 다면 Hard).
	TArray<FReference> GetReferences() const;

	// 프로퍼티 값 하나(Value는 그 값의 주소)를 위 규칙으로 쓴다.
	void DumpValue(const FProperty* Property, const void* Value, FRocksJsonWriter& Writer);

	// 객체 참조 하나를 위 규칙으로 쓴다(하위 객체 펼침·참조 수집 포함). bHard가 거짓이면 Soft로 모은다(weak·lazy
	// 참조, 덤프가 아닌 계산 값의 참조 — 머티리얼 인스턴스 resolved, 8.6절).
	void DumpObjectReference(const UObject* Object, FRocksJsonWriter& Writer, bool bHard = true);

	// Object를 이미 펼친 하위 객체로 표시한다. 다른 곳에서 따로 쓰는 하위 객체(머티리얼 그래프 노드 — 8.6절)를 참조
	// 자리에서 다시 펼치지 않고 { "subobject" }로만 쓰게 한다.
	void MarkExpanded(const UObject* Object);

private:
	bool ShouldSkip(const FProperty* Property) const;
	void DumpProperty(const FProperty* Property, const void* ContainerData, FRocksJsonWriter& Writer);
	void DumpSoftReference(const FSoftObjectPath& Path, FRocksJsonWriter& Writer);
	void AddReference(const FSoftObjectPath& Path, bool bHard);
	TArray<uint8> SortKey(const FProperty* Property, const void* Value);

	const UObject* DumpRoot;
	TArray<const UScriptStruct*> ExcludedStructs;
	TSet<const FProperty*> ExcludedProperties;
	TSet<const UObject*> Expanded;
	TMap<FString, bool> References;

	// 정렬 키 계산 중이면 참이다. 이때는 하위 객체를 펼치지 않고 참조를 모으지 않는다.
	bool bKeyPass = false;
};
