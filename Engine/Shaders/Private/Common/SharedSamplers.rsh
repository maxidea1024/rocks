// 공유 샘플러 둘이다(Docs/Design/ShaderSystem 7.2절 9항 — UE Wrap_WorldGroupSettings·
// Clamp_WorldGroupSettings). 텍스처 X를 자기 샘플러 XSampler 대신 이 둘 가운데 하나로 샘플링한다(텍스처마다
// 출처 하나). 예약 이름이라 짝 규칙의 XSampler로 쓸 수 없다. 다른 샘플러처럼 register 없이 선언하고
// backend 위치는 자기 샘플러 다음 순번이다. 머티리얼 생성 원문의 공유 출처 텍스처가 쓴다
// (Docs/Design/MaterialSystem 7.4절 9항 — MaterialCommon.rsh가 include한다). 셰이더 시스템 7.2절 9항은
// /Engine/Shared/SharedSamplers.rsh라 적었지만 Shared 아래는 C++·HLSL 공용 .h만 받으므로(전처리 규칙)
// 셰이더 전용 공용 원문 자리인 Private/Common에 둔다.
#pragma once

SamplerState SharedWrapSampler;
SamplerState SharedClampSampler;
