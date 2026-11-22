#pragma once

#include "CoreMinimal.h"
#include "Runtime/Launch/Resources/Version.h"

// RocksExport 플러그인 공통 선언이다(Rocks 저장소 Docs/Design/UnrealExchange.ko.md 11절). 지원 UE 버전은
// 5.8 하나다(11.1절). 다른 버전에서는 컴파일을 멈춘다.
#if ENGINE_MAJOR_VERSION != 5 || ENGINE_MINOR_VERSION != 8
#error "RocksExport supports Unreal Engine 5.8 only (UnrealExchange.ko.md section 11.1)."
#endif

ROCKSEXPORT_API DECLARE_LOG_CATEGORY_EXTERN(LogRocksExport, Log, All);

namespace RocksExport
{
	// 묶음 형식 버전이다(3.4절 formatVersion). Rocks 리더는 이 값만 받는다.
	inline constexpr int32 BundleFormatVersion = 1;

	// 플러그인 이름이다(bundle.json exporter.plugin, 플러그인 관리자 조회 이름).
	inline constexpr const TCHAR* PluginName = TEXT("RocksExport");

	// UE 객체 경로 byte 상한이다(UE NAME_SIZE, Rocks 리더 UnrealObjectPathMaxBytes와 같다 — 10.4절 7번).
	inline constexpr int32 ObjectPathMaxBytes = 1024;

	// redirector 사슬을 따라가는 최대 단계다. 넘으면 끊어진 사슬로 본다(순환 방어, 11.7절).
	inline constexpr int32 RedirectMaxChain = 64;

	// 에셋 진단 수준 문자열이다(3.5절 diagnostics.severity — Rocks 리더가 받는 세 값).
	inline constexpr const TCHAR* SeverityError = TEXT("error");
	inline constexpr const TCHAR* SeverityWarning = TEXT("warning");
	inline constexpr const TCHAR* SeverityInfo = TEXT("info");
}
