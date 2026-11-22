#pragma once

#include "CoreMinimal.h"

// 플러그인 자기 시험이다(UnrealExchange.ko.md 12.2절). commandlet -SelfTest와 UE Automation 시험
// (RocksExport.*)이 같은 함수를 부른다. 각 함수는 실패 설명을 Errors에 더한다(비어 있으면 통과).
namespace RocksExportSelfTest
{
	// SHA-256 NIST 벡터(FIPS 180-4 예제·백만 'a')와 조각 경계 무관성.
	void RunSha256(TArray<FString>& Errors);

	// 11.4절 숫자 서식: 표준 표기, 왕복(같은 비트), 비유한값, 로캘 무관.
	void RunNumberFormat(TArray<FString>& Errors);

	// JSON writer: escape·UTF-8·surrogate, 들여쓰기, 정렬 비교.
	void RunJsonWriter(TArray<FString>& Errors);

	// UPROPERTY 덤프: 시험 구조체의 기대 JSON과 참조 수집, 엔진 에셋 덤프의 결정성.
	void RunPropertyDump(TArray<FString>& Errors);

	// 묶음 writer: blob 경로·중복 건너뜀·크기 검사, 원자적 교체, 묶음 아닌 디렉터리 보호.
	void RunBundleWriter(TArray<FString>& Errors);

	// 엔진 콘텐츠 내보내기: 형식 확인(UE JSON 파서로 다시 읽음)과 두 번 내보낸 bytes 일치(createdUtc 제외).
	void RunExportDeterminism(TArray<FString>& Errors);

	// 텍스처 추출(4.5절): 엔진 텍스처 여러 형식·차원을 내보내 blob마다 FTextureSource 원본 mip(LockMip 경로)과
	// SHA-256·bytes·blob 파일 내용을 대조하고, data.source 표와 settings(Source 제외), 손실 원본 표시를 확인한다.
	void RunTextureExtraction(TArray<FString>& Errors);

	// 정적 메시 추출(5.4절): 엔진 메시를 내보내 LOD 수·원본 수·위치 blob·엔진 속성·prepared를 확인하고, 추출 전후의
	// 에셋 덤프·원본 메시 digest·빌드 설정 lightmap 번호가 같은지(12.2절 — prepared가 원본 설정을 바꾸지 않음) 본다.
	void RunStaticMeshExtraction(TArray<FString>& Errors);

	// 스켈레톤·스킨 메시 추출(6.3절): 튜토리얼 스킨 메시와 스켈레톤을 내보내 본 계층(수·이름·부모·리타깃 방식), LOD 수,
	// LOD 0 스킨 가중치 영향 수를 원본과 대조하고 추출 전후 덤프가 같은지 본다.
	void RunSkeletalExtraction(TArray<FString>& Errors);

	// 애니메이션 추출(7.1절): 튜토리얼 걷기 클립을 내보내 비율·프레임·키 수, 본 트랙 이름·순서, 회전 blob을 데이터
	// 모델 값과 대조하고, settings에 데이터 모델이 없고 추출 전후 덤프가 같은지 본다.
	void RunAnimationExtraction(TArray<FString>& Errors);

	// 위 전부를 돌린다. 모두 통과하면 true다.
	bool RunAll(TArray<FString>& Errors);
}
