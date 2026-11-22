#pragma once

#include "CoreMinimal.h"

class FRocksJsonWriter;

// blob 참조 하나다(3.3절). WriteTo가 { "blob", "bytes", "layout", "components", "count" }를 그 순서로 쓴다.
struct ROCKSEXPORT_API FRocksBlobRef
{
	FString Hex;
	uint64 Bytes = 0;
	FString Layout;
	uint32 Components = 0;
	uint64 Count = 0;

	void WriteTo(FRocksJsonWriter& Writer) const;
};

// 묶음 디렉터리 writer다(3.1절, 11.6절 FRocksBundleWriter).
//
// 원자적 쓰기(11.7절): 모든 파일을 출력 디렉터리 옆 staging 디렉터리(<출력>.staging)에 쓰고, Commit이
// bundle.json을 마지막으로 쓴 뒤 staging을 출력 이름으로 바꾼다. 기존 출력은 묶음(bundle.json 있음)이거나
// 빈 디렉터리일 때만 바꾸고(<출력>.previous로 옮긴 뒤 지움), 그 밖의 디렉터리는 지우지 않고 실패한다. 실패나
// Commit 없이 소멸하면 staging을 지운다. 그래서 출력 이름에는 완성된 묶음만 나타난다.
//
// blob은 내용 주소다: blobs/<hex 앞 2자>/<hex 64자>.bin. 이번 내보내기에서 같은 내용을 이미 썼으면 다시 쓰지
// 않는다. 고정 크기 배치(u8·u16·u32·i32·f16·f32·f64)는 bytes = 원소 크기 × components × count를 확인한다.
//
// 사용 예:
//   FRocksBundleWriter Bundle;
//   if (!Bundle.Begin(OutputDirectory, Error)) { ... }
//   Bundle.WriteAssetFile(TEXT("/Game/Props/SM_Rock"), AssetJson, Error);
//   Bundle.Commit(BundleJson, Error);
//
// 게임 스레드 하나가 쓴다. 경로는 절대 경로로 바꿔 쓴다.
class ROCKSEXPORT_API FRocksBundleWriter
{
public:
	FRocksBundleWriter() = default;
	~FRocksBundleWriter();

	FRocksBundleWriter(const FRocksBundleWriter&) = delete;
	FRocksBundleWriter& operator=(const FRocksBundleWriter&) = delete;

	// staging 디렉터리를 새로 만든다(남은 staging은 지운다). 출력 자리가 덮을 수 없는 디렉터리·파일이면 실패다.
	bool Begin(const FString& OutputDirectory, FString& OutError);

	// 에셋 JSON을 assets + 패키지 경로 + .json에 쓴다(10.4절 8번). 같은 패키지를 두 번 쓰면 실패다.
	bool WriteAssetFile(const FString& PackageName, TConstArrayView<uint8> Json, FString& OutError);

	// blob을 쓰고 참조를 채운다. Layout은 3.3절 이름이다.
	bool WriteBlob(TConstArrayView64<uint8> Bytes, const FString& Layout, uint32 Components, uint64 Count, FRocksBlobRef& OutRef, FString& OutError);

	// bundle.json을 쓰고 staging을 출력 자리로 옮긴다. 성공하면 writer는 끝난 상태다.
	bool Commit(TConstArrayView<uint8> BundleJson, FString& OutError);

	// staging을 지우고 끝낸다(Commit 전 실패 경로).
	void Abort();

	// 묶음 안 상대 경로다(시험·검증용).
	static FString AssetFileForPackage(const FString& PackageName);
	static FString BlobFileForHex(const FString& Hex);

	// 고정 크기 배치의 원소 byte 크기다. 가변 배치(names·boneInfluences·sparseVec3)는 0, 모르는 이름은 -1이다.
	static int32 LayoutElementBytes(const FString& Layout);

private:
	bool WriteFile(const FString& Relative, const void* Data, int64 Size, FString& OutError);

	FString Output;
	FString Staging;
	TSet<FString> AssetFiles;
	TSet<FString> Blobs;
	bool bActive = false;
};
