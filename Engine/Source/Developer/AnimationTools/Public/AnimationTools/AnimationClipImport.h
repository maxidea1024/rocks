#pragma once

#include <Animation/AnimationDiagnostic.h>
#include <AnimationTools/AnimationClipImportSpec.h>
#include <AssetDatabase/AssetImporter.h>

// 애니메이션 클립 임포트 서비스다(Docs/Design/MeshAnimationAssets 7.6.2절 — MA6b2). 파일 형식을
// 읽는 임포터(교환 형식 UX7b, 뒤의 FBX·glTF)가 AssetImporter::Import 안에서
// AnimationClipImportSpec을 만들어 부르면, 서비스가 Output()으로 받은 AnimationClip을 채운다(원본
// bulk는 AddBulk, 참조는 OpenAsset). 정적·스킨 메시 임포트와 같은 역할이다.
namespace rocks {

// spec으로 output(context.Output이 준 AnimationClip)을 채운다(7.6.2절 5 순서): 모두 검증한 뒤 원본
// bulk를 넘긴다. 재임포트(context.IsReimport())는 이벤트·동기 마커·이벤트 트랙·재생 설정·기준
// 클립을 보존하고(replaceSettingsOnReimport면 명세 값 — 보존하면 명세의 그 값은 만들지도 검증하지도
// 않는다), 커브·변환 커브는 명세 것에 옛 커브 가운데 같은 이름이 없는 것을 더한다(UE Interchange가
// 기존 커브를 지우지 않는 것과 같다). playback은 비운다. 경고: 스켈레톤에 없는 트랙
// UnknownBoneTrack(detail = 명세 트랙 번호), 버린 이벤트 속성 EventPropertiesDropped(detail = 합),
// animated attribute UnsupportedSetting(detail = 수).
//
// 오류(진단은 context.Fail): Registry Name pool 없음 InvalidArgument(진단 None), output이
// AnimationClip이 아님 TypeMismatch, 스켈레톤 id 없음·이름 intern 실패·플랫폼 키(None으로 접히거나
// 겹침)·커브 null InvalidSetting(detail = 그 목록 안 위치 — 목록마다 따로 센다), 원본 위반은
// ValidateClipSource의 진단(InvalidClipData, detail = 트랙 — failure가 null이 아니면 세부도
// 채운다), 스켈레톤 TypeMismatch(detail 0)·기준 클립 TypeMismatch(detail 1), 참조 열기 실패는
// 문맥의 오류, 클립 설정 위반은 ValidateAnimationClip의 진단(InvalidClipData, detail = 정렬 뒤
// index), 할당 실패. 실패하면 output은 부분적으로 바뀔 수 있다(서비스가 출력을 버린다 —
// AssetImporter 계약).
ROCKS_ANIMATIONTOOLS_API Result<void>
ImportAnimationClip(AssetImportContext& context, ObjectHandle output,
                    const AnimationClipImportSpec& spec,
                    ClipDiagnostic* failure = nullptr) noexcept;

} // namespace rocks
