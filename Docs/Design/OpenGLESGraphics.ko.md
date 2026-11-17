# OpenGL ES 그래픽 backend(P11)

기준일: 2026-11-17, 개정 1. [셰이더 시스템](ShaderSystem.ko.md) 선행 작업 P11("OpenGLGraphics를 UE `OpenGLDrv` 방식으로")의 설계와 구현 상태다. `OpenGLGraphics` 모듈을 데스크톱 GL 4.5 core + DSA에서 **OpenGL ES 3.2 공통 API**로 바꾸고, Windows 개발용 미리보기(`GLES31_PC`)와 Android(`GLES31_ANDROID`)를 같은 공통 코드로 맡는다.

UE 근거는 UE 5.8.4(로컬 클론, `release` 브랜치)의 `Engine/Source/Runtime/OpenGLDrv`이며, 아래 경로는 `Runtime/OpenGLDrv/Private/` 기준이다(셰이더 컴파일러는 `Developer/ShaderFormatOpenGL/Private/`). 레거시·deprecated 경로와 데스크톱 GL SM5 경로는 보지 않았다.

상태 표기는 셰이더 시스템과 같다: **구현**(동작·시험), **계약**(API가 있고 미지원은 명시 오류), **후속**.

## 1. 결정

| 항목 | 결정 | 근거 |
|---|---|---|
| 지원 API | OpenGL ES 3.2만. 데스크톱 GL 4.x backend는 두지 않는다 | 2026-11-14 사용자 결정(UE 방식). 셰이더 시스템 1·2절 |
| 공통 경로 | 장치 동작은 모두 ES 3.2 core 함수 표(`Private/Common/GlesApi.h`)로 한다. 플랫폼 GL 헤더를 include하지 않고 상수·형식을 직접 둔다 | 같은 공통 코드가 Windows·Android에서 컴파일되게 한다. UE도 공통 기반 `OpenGLBase.h` 위에 플랫폼 클래스를 둔다 |
| 플랫폼 층 | context·current 전환·창 표면·present·fence 대기·클립 공간만 플랫폼 층(`GlesPlatform`, Win64 WGL·Android EGL)이 맡는다. 데스크톱 전용 호출(`glClipControl`, `GL_FRAMEBUFFER_SRGB` 켜기, WGL sRGB 픽셀 형식)은 Win64 층에만 있다 | 셰이더 시스템 5.2절 G2·P11 |
| Windows context | ES profile(`WGL_EXT_create_context_es2_profile`) 우선, 안 되면 데스크톱 GL 4.3 core로 내려간다(`OpenGLContextProfile::Automatic`). 둘을 골라 쓸 수도 있다 | 측정 S12·2.2절. ES profile은 드라이버가 ES 밖 호출을 거부해 공통 경로를 검증한다. 데스크톱 core는 UE 방식(`Windows/WindowsOpenGLPlatform.cpp:200–205`)이고 형식 정의 `GLES31_PC`의 `deviceRequirements`와 같다 |
| 셰이더 원문 | SH2 전에는 손으로 쓴 GLSL ES 3.20 원문을 런타임에 컴파일한다. 모양은 쿠킹된 `GLES31_PC` 출력과 같다(7절) | 7절 |
| 그리기 상수 | 루트 상수 에뮬레이션 배열 `vu_h[5]`·`pu_h[5]`(DrawConstants 80 byte = vec4 5개)에 `glUniform4fv`로 그대로 올린다 | 셰이더 시스템 7.2절 4항, UE `OpenGLShaders.cpp:1983–2055`·`:3289–3412` |
| 화면 방향 | 모든 패스(스왑체인 포함)를 저장소 첫 행 = 이미지 위쪽인 색 텍스처에 그리고, present가 Y를 뒤집어 창에 blit한다 | 셰이더 시스템 7.2절 8항, UE `Windows/WindowsOpenGLPlatform.cpp:505–517`, Android `AndroidOpenGL.cpp:366–398` |
| backend 이름 | Graphics `Backend::OpenGL`과 모듈 이름 `OpenGLGraphics`는 그대로 둔다. 이 backend가 곧 OpenGL ES다. 장치 정보의 adapter 이름 뒤에 context 종류(`OpenGL ES 3.2, WGL ES profile, GLES31_PC preview` 등)를 붙인다 | 셰이더 시스템 결정 9("OpenGLGraphics를 지우지 않고 GLES 미리보기로 전환"), 형식 정의 `"backend": "OpenGL"`, UE RHI 이름도 `OpenGL`(ES 포함) |

## 2. UE 5.8 OpenGLDrv 조사

### 2.1 요지

| 주제 | UE 5.8 | Rocks |
|---|---|---|
| Android context | EGL context 하나를 모든 표면이 공유한다(`Android/AndroidEGL.cpp:77–78`, `:903–907`). `EGL_KHR_create_context`로 3.2를 요청하고 실패하면 3.1, 다시 client version 2로 시도한다(`:751–820`). ES 3.2 미만이면 경고 대화상자를 띄운다(`Android/AndroidOpenGL.cpp:453`, `:480–485`) | context 하나. 3.2만 요청하고 3.2 미만은 `Unsupported`(셰이더 시스템 2절 기기 요구) |
| Android offscreen | window가 없으면 pbuffer, `EGL_KHR_surfaceless_context`가 없으면 1×1 보조 pbuffer(`AndroidEGL.cpp:270–290`, `:373–398`) | 같다(surfaceless가 있으면 표면 없이) |
| Windows context | 4.3 core·forward compatible, debug이면 debug bit(`Windows/WindowsOpenGLPlatform.cpp:200–205`, `:228–243`). ES profile 경로는 `EMULATE_ES31=0`이라 컴파일되지 않는다(`:244–252`, `Windows/WindowsOpenGL.h:9`) | ES profile 우선, 데스크톱 4.3 core 대체(1절) |
| 함수 로딩 | Windows: GL 1.1은 `opengl32.dll` export, 나머지는 `wglGetProcAddress`(`WindowsOpenGLPlatform.cpp:771–800`). Android: ES 3.1 core는 직접 링크, ES 3.2·확장은 `eglGetProcAddress`(`OpenGLES.cpp:171–276`) | 모두 함수 표로 실행 시 얻는다. Windows는 같은 방식, Android는 `eglGetProcAddress`(없으면 `dlsym`) |
| clip control | Windows·Linux에서 `GL_ARB_clip_control`이 있으면 `glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE)`(`OpenGLDrv.cpp:469–475`). Android는 쓰지 않는다. 컬 방향은 clip control 유무로 뒤집는다(`OpenGLState.cpp:46–54`) | 같다. ES profile은 같은 기능의 `GL_EXT_clip_control`을 쓴다. 컬 반전은 `glFrontFace`로 한 곳에서 한다(5.3절) |
| 상수 버퍼 | OpenGL은 늘 에뮬레이션 UB다(`Runtime/RenderCore/Private/Shader.cpp:1695–1703`). 링크 때 `<단계>u_<타입>`·`<단계>c<N>_<타입>` 배열 위치를 찾고(`OpenGLShaders.cpp:1983–2055`) 그릴 때 `glUniform4fv/4iv/4uiv`로 올린다(`:3289–3412`) | 루트 상수(`vu_h`·`pu_h`)만 지금 구현. 상수 버퍼(`vc<N>_h` 등)와 복사 표는 SH2 |
| 텍스처·샘플러 | 저장소는 늘 `glTexStorage*` + `glTexSubImage*`(`OpenGLTexture.cpp:698–798`), 샘플러는 샘플러 객체 `glBindSampler`(`OpenGLCommands.cpp:607–613`, 생성 `OpenGLState.cpp:302–330`). PS 유닛 0부터, VS는 `MaxTextureImageUnits`부터(`OpenGLBase.h:128–131`) | 같다. VS 유닛은 16부터 컴파일 때 고정(셰이더 시스템 7.2절 4항) |
| 정점 입력 | ES 3.1 vertex attrib binding(`glVertexAttribFormat`·`glVertexAttribIFormat`·`glVertexAttribBinding`·`glBindVertexBuffer`·`glVertexBindingDivisor`)과 context당 VAO 하나(`OpenGLCommands.cpp:1840–1964`) | 같은 API. VAO는 파이프라인마다 하나로 형식을 한 번 둔다 |
| 버퍼·readback | 생성은 `glBufferData`, 갱신은 Windows `glBufferSubData`·Android map(`OpenGLCommands.cpp:44–48`). readback은 `GL_PIXEL_PACK_BUFFER` + `glReadPixels`(`OpenGLTexture.cpp:865–902`) | 같다. ES에 `glGetBufferSubData`가 없어 readback은 fence 뒤 `glMapBufferRange(GL_MAP_READ_BIT)` |
| fence·timestamp | `glFenceSync`·`glClientWaitSync`(`OpenGLQuery.cpp:442–486`, `:545–606`). ES timestamp는 `glQueryCounterEXT`와 `GL_GPU_DISJOINT_EXT`(`OpenGLES.h:117–120`, `:615–627`), 끊기면 대기 결과를 버린다(`OpenGLQuery.cpp:250–265`). Android는 timestamp를 끈다(`Android/AndroidOpenGL.h:58–61`) | 같은 방식. Android도 확장이 있으면 켠다(5.4절) |
| debug | Windows는 core `glPushDebugGroup`, Android는 `GL_EXT_debug_marker`(`OpenGLES.h:154–168`) | `KHR_debug`(ES 3.2 core) 하나 |
| 스레드 | context를 가진 스레드만 GL을 부른다. 소유 이전은 `PlatformRenderingContextSetup`·`PlatformNULLContextSetup`(`OpenGLDevice.cpp:1400–1410`) | Graphics GX10 소유권 이관으로 같은 일을 한다(5.6절) |
| present | Windows 백버퍼는 offscreen 텍스처이고 present가 `GL_FRAMEBUFFER_SRGB`를 끄고 Y를 뒤집어 blit한다(`WindowsOpenGLPlatform.cpp:505–517`). Android도 기본은 offscreen 백버퍼 + Y 반전 blit(`AndroidEGL.cpp:36–42`, `AndroidOpenGL.cpp:366–398`) | 같다(5.2절) |
| 프로그램 | separate shader object를 쓰지 않는다(`OpenGLBase.h:283–287`) | 같다 |

### 2.2 측정(이 기계, 2026-11-17)

GTX 1060 3GB, NVIDIA 560.94, Windows 11. probe 프로그램으로 두 context가 공개하는 것을 확인했다(셰이더 시스템 측정 S12의 이어짐).

| 항목 | WGL ES 3.2 profile | WGL 4.3 core |
|---|---|---|
| `GL_VERSION` | OpenGL ES 3.2 | 4.3.0(4.3을 요청하면 4.3을 준다) |
| clip control | `GL_EXT_clip_control` | `GL_ARB_clip_control` |
| ES 셰이더 | GLSL ES 3.20 | `GL_ARB_ES3_2_compatibility`(`#version 320 es`) |
| timestamp | `GL_EXT_disjoint_timer_query`, counter 64 bit | `GL_ARB_timer_query`(core), counter 64 bit, 끊김 표시 없음 |
| debug | `GL_KHR_debug` | `GL_KHR_debug` |
| sRGB 쓰기 스위치 | `GL_EXT_sRGB_write_control` | core |
| ES 확장 | `GL_EXT_texture_filter_anisotropic`·`GL_EXT_texture_norm16`·`GL_EXT_texture_format_BGRA8888`·`GL_OES_texture_float_linear`·`GL_EXT_color_buffer_float`·border clamp | 이방성만 같은 이름(EXT), 나머지 ES 확장 이름은 없다 |
| 기본 framebuffer 인코딩 | LINEAR로 보고(sRGB 지원 픽셀 형식이어도) | 같다 |

present blit에서 sRGB 쓰기 스위치를 끄지 않으면 ES profile에서만 창 byte가 offscreen과 달라졌다(컬링 시험 3건, 각 98302 byte). 데스크톱 core는 켜 두어도 같았다. 스위치를 끄면 두 context 모두 byte가 그대로 옮겨진다. 이 NVIDIA ES profile은 데스크톱 GL 4.4+처럼 원본 디코딩도 스위치에 묶는다는 뜻이다. ES 3.2 사양(16.2.1절)은 sRGB 원본을 늘 디코딩하고 스위치(`GL_EXT_sRGB_write_control`)는 쓰기 인코딩만 정하므로, 사양대로 동작하는 Android 드라이버에서는 스위치를 끄면 오히려 어두워진다. 그래서 "스위치를 끄고 byte를 그대로 옮긴다"는 플랫폼 성질(`PlatformInfo::rawSrgbBlit`)로 두었다: Windows 미리보기는 측정대로 참, Android는 거짓(스위치를 켠 채 sRGB 대상으로 옮겨 디코딩·인코딩이 상쇄된다).

## 3. 구조

```text
Engine/Source/Runtime/OpenGLGraphics/
  Public/OpenGLGraphics/OpenGLDevice.h   CreateOpenGLDevice(+ OpenGLDeviceOptions, DeviceCreateReport)
  Private/Common/                        ES 3.2 공통(모든 지원 플랫폼)
    GlesApi.h/.cpp                       형식·상수·ES 3.2 core 함수 표·확장 조회
    GlesFormats.h/.cpp                   확장 기능 조사, 픽셀·정점 형식·샘플러 대응표, 진단 문자열
    GlesPlatform.h                       플랫폼 층 인터페이스, 클립 공간 규약
    GlesDevice.h/.cpp                    장치: 생성·자원·회수
    GlesDeviceFrame.cpp                  프레임·CommandContext·present
    GlesDeviceTransfer.cpp               업로드·readback·계측·fence·소유권
  Private/Win64/WglPlatform.cpp          Windows 미리보기 플랫폼 층(WGL)
  Private/Android/EglPlatform.cpp        Android 플랫폼 층(EGL)
  Private/Unsupported/                   그 밖의 플랫폼(Unsupported)
```

빌드 규칙은 Windows = Win64 + Common, Android = Android + Common(시스템 라이브러리 `EGL`·`GLESv3`), 그 밖은 Unsupported다.

## 4. 플랫폼 층

### 4.1 Windows 미리보기(`GLES31_PC`)

- 임시 창의 legacy context로 WGL 확장 함수를 얻고, 장치 숨김 창에 sRGB 지원 더블 버퍼 RGBA8 픽셀 형식을 정한다. 스왑체인 창도 같은 형식이며 창 클래스는 `CS_OWNDC`여야 한다.
- context:
  - `EsProfile`: ES 3.2 profile. `GL_EXT_clip_control`이 없으면 `Unsupported`.
  - `DesktopCore`: GL 4.3 core. `GL_ARB_ES3_2_compatibility`·`GL_ARB_clip_control`이 없으면 `Unsupported`.
  - `Automatic`(기본): `EsProfile`을 먼저 시도하고 실패하면 `DesktopCore`. 둘 다 실패하면 두 원인을 `DeviceCreateReport::detail`에 함께 적는다.
  - 두 경우 모두 ES 3.2 core 함수 표를 다 얻어야 하고 버전을 확인한다.
- context를 만든 뒤 `glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE)`을 한 번 부른다. 데스크톱 core는 sRGB 렌더 타깃 인코딩을 위해 `GL_FRAMEBUFFER_SRGB`를 켠다(ES는 늘 인코딩한다).
- fence 관찰 스레드는 같은 속성의 공유 context를 숨김 창에 붙여 `glClientWaitSync`로 기다린다.
- present는 창 DC에 context를 붙이고 Y 반전 blit 뒤 `SwapBuffers`다. swap interval(`WGL_EXT_swap_control`)은 창마다 값이 바뀔 때만 바꾼다.

### 4.2 Android(`GLES31_ANDROID`) — 컴파일 확인, 실행은 P13

- `eglInitialize` → `EGL_KHR_fence_sync` 필수 → RGBA8 ES3 window·pbuffer config → `EGL_KHR_create_context`로 3.2 또는 client version 3 뒤 버전 확인. `enableValidation`이면 EGL 1.5 이상에서 `EGL_CONTEXT_OPENGL_DEBUG`를 요청하고 거절되면 debug 없이 다시 만든다(`EGL_CONTEXT_FLAGS_KHR`의 debug bit는 OpenGL context 전용이다).
- 장치 표면: `EGL_KHR_surfaceless_context`면 표면 없이, 아니면 1×1 pbuffer.
- fence: `eglCreateSyncKHR(EGL_SYNC_FENCE_KHR)` + `glFlush`, 관찰 스레드는 context 없이 `eglClientWaitSyncKHR`로 기다린다. 그래서 공유 context가 없다(UE처럼 context 하나).
- 창 표면: `NativeSurface::window`를 `ANativeWindow*`로 받아 `eglCreateWindowSurface`(`EGL_GL_COLORSPACE_SRGB_KHR`). `EGL_KHR_gl_colorspace`가 없으면 `Unsupported`다(5.2절). 표면의 생성·소실·재생성 수명과 앱 진입은 P13이다.
- 클립 공간은 `ShaderFlip`이다(5.3절).

## 5. Graphics 계약의 ES 3.2 대응

### 5.1 자원(GX3a·GX8)

- 버퍼: `glGenBuffers` + `GL_COPY_WRITE_BUFFER`에 묶어 `glBufferData`·`glBufferSubData`. 정점 배열 상태(`GL_ELEMENT_ARRAY_BUFFER` binding)를 건드리지 않으려고 복사 대상 binding을 쓴다.
- 텍스처: `glTexStorage2D` + `glTexSubImage2D`(첫 행 = 이미지 위쪽, UV (0,0) = 좌상단). 업로드는 그리기 유닛과 겹치지 않는 유닛 32에서 묶는다.
- 샘플러: 텍스처마다 샘플러 객체 하나(`glSamplerParameter*`), 그릴 때 `glBindSampler`. border 색은 ES 3.2 core다.
- 형식 표(`GlesFormats.cpp`): 행마다 내부·외부 형식, 렌더 타깃 가능 여부, 샘플링·렌더 타깃에 필요한 확장(`GlesFeature`)을 둔다. 지금은 `RGBA8Unorm`·`RGBA8UnormSrgb`(ES 3.2 core) 두 행이다. 픽셀 형식 확장(`Docs/Design/PixelFormat.ko.md`, 병합 예정) 때 행을 더하고, 확장이 없으면 다른 형식으로 대신하지 않고 `Unsupported`다.

  | 확장 | `GlesFeature` | 쓰는 곳 |
  |---|---|---|
  | `GL_EXT_texture_norm16` | `TextureNorm16` | R16·RG16·RGBA16 UNORM |
  | `GL_EXT_texture_format_BGRA8888` | `TextureBgra8` | BGRA8 |
  | `GL_OES_texture_float_linear` | `FloatLinearFilter` | 32-bit float 선형 필터 |
  | `GL_EXT_color_buffer_float` | `ColorBufferFloat` | float·half 렌더 타깃 |
  | `GL_EXT_texture_filter_anisotropic` | `Anisotropic` | 이방성 필터(없으면 Trilinear, 기존 규약) |

  Windows 데스크톱 core context도 ES 확장 이름으로 판정한다. 그래서 그 context에서는 이방성 밖의 확장 형식이 미지원으로 나온다(형식이 생기면 데스크톱 core에서 그 형식 시험은 `Unsupported` 음성 대조다).
- 정점: 파이프라인마다 VAO 하나에 `glVertexAttribFormat`/`glVertexAttribIFormat`(정규화 없는 정수)·`glVertexAttribBinding`·`glVertexBindingDivisor`를 한 번 두고, 버퍼는 그릴 때 `glBindVertexBuffer`다. `Bgra8Unorm`은 ES에 BGRA 정점 배열이 없어 `Unsupported`(RenderBase 10.4절).
- RenderTarget: 색 attachment 하나의 framebuffer. 기록 중 만들어도 그리기 binding을 바꾸지 않도록 읽기 binding에서 만든다.

### 5.2 패스·present·창 경로

- `BeginOffscreen`·`BeginFrame` 모두 색 텍스처 framebuffer에 그린다. 스왑체인은 `RGBA8UnormSrgb` offscreen 텍스처를 가지며 `ResizeSwapChain`이 새로 만든다(옛 텍스처는 fence 뒤 회수). 창 크기와 다르면 present가 늘려 그린다(D3D12 표시 확대와 같다).
- `EndFrame`의 present: 창 표면에 context를 붙이고 텍스처 → 기본 framebuffer로 대상 y를 뒤집어 blit한 뒤 버퍼를 바꾼다. Windows 미리보기는 sRGB 쓰기 스위치를 끄고 옮겨 byte를 그대로 둔다(2.2절 측정, UE와 같다).
- Android(ES 사양 의미)는 blit이 sRGB 원본을 선형으로 읽으므로 스위치를 켠 채 sRGB 창 표면(`EGL_KHR_gl_colorspace`)으로 옮겨 디코딩·인코딩이 상쇄되게 한다. sRGB 창 표면을 만들 수 없는 기기는 셰이더로 다시 인코딩하는 present가 필요하며 P13 후속이다. 그때까지 창 표면 생성이 `Unsupported`다(거짓 색을 내지 않는다).
- 창 표면에 붙이지 못하면(`wglMakeCurrent`·`eglMakeCurrent` 실패) 장치 표면으로 되돌린다. 그래도 context가 없으면 fence를 지어내지 않고 장치 손실로 게시하며 `EndFrame`이 `Destroyed`다.
- `CopyPassTarget`: RenderTarget 패스는 패스 텍스처를 그대로 복사한다. 스왑체인 패스는 present와 같은 blit으로 창의 기본 framebuffer에 옮긴 뒤 그것을 다시 뒤집어 읽는다. 그래서 창에 실제로 놓일 byte(방향 포함)를 offscreen 결과와 대조할 수 있다(C7 창 경로 시험 유지).

### 5.3 좌표·앞면(C7)

- `ClipControlUpperLeft`(Windows): `glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE)`, 셰이더 보정 없음, 앞면 `GL_CCW`. 클립 공간이 D3D와 같고 저장소 첫 행이 이미지 위쪽이다.
- `ShaderFlip`(Android): 셰이더가 SPIRV-Cross `fixup_clipspace`·`flip_vert_y`로 y를 뒤집고 z를 옮긴다. 저장소 첫 행이 이미지 위쪽이 되고 window 감김이 반대가 되므로 앞면 `GL_CW`다(UE는 컬 모드를 뒤집는다, `OpenGLState.cpp:46–54`).
- 앞면 감김은 장치 생성 때 한 번 정한다(모든 패스가 같은 규약). 컬 모드는 `glCullFace(GL_BACK/GL_FRONT)`다. 음수 행렬식 반전은 파이프라인 선택이다(셰이더 시스템 7.2절 8항).
- 행렬: 행 우선 byte를 전치 없이 vec4 배열로 올리고 셰이더가 `mat4(vu_h[0..3]) * v`로 쓴다(7절).

### 5.4 GPU 완료·계측(GX7·GX9)

- fence: 제출마다 플랫폼 fence(Windows `glFenceSync` + `glFlush`, Android EGL fence). 관찰 스레드가 1초 단위로 다시 기다린다(시간으로 완료를 판정하지 않는다). 만들지 못한 제출은 `glFinish`로 끝낸다.
- 회수: `Retire`는 기존 계약 그대로 마지막 사용 fence 뒤에 지운다. 스왑체인은 텍스처·framebuffer와 창 표면을 함께 fence 뒤에 놓는다.
- timestamp: ES는 `GL_EXT_disjoint_timer_query`(`glQueryCounterEXT`, `glGetQueryObjectui64vEXT`, counter bit 조회 `glGetQueryivEXT`. ES 3.2 core `glGetQueryiv`는 `GL_TIMESTAMP` 대상이 없다)가 있어야 있다. 조사는 debug output을 켜기 전에 한다. 결과를 읽은 뒤 `GL_GPU_DISJOINT_EXT`가 서 있으면 그 묶음 전체가 disjoint다. 데스크톱 core는 GL 3.3 core 이름(끊김 표시 없음). 확장이 없거나 counter bit가 0이면 `DeviceInfo::timestamps`가 false이고 `WriteTimestamp`·`CalibrateClocks`가 `Unsupported`다(GPU 시간 "없음", 추정값 없음).
- calibration: `glGetInteger64v(GL_TIMESTAMP)`를 CPU tick 두 번으로 감싼다(기존과 같다).
- debug marker: `KHR_debug`(ES 3.2 core) debug group. 검증은 `enableValidation`이면 debug context와 `GL_DEBUG_OUTPUT`(동기), ERROR·UNDEFINED_BEHAVIOR를 `ValidationErrorCount`로 센다.

### 5.5 셰이더·파이프라인

- `glCreateProgram`·`glAttachShader`·`glLinkProgram`(separate shader object 없음). 컴파일·링크 실패는 `DependencyFailed`이고 info log가 `LastError`다.
- 링크 뒤 active attribute로 정점 입력을 대조한다(RenderBase 10.4절).
- 루트 상수 배열 위치(`vu_h`·`pu_h`)를 링크 때 한 번 찾는다. 없으면 그 단계는 상수를 읽지 않는다.

### 5.6 스레드(GX10·GX11)

- `concurrentResourceCreation`은 false다(context가 소유 스레드에 묶인다).
- `ReleaseOwnership`이 이 스레드에서 context를 떼고 `AcquireOwnership`이 새 스레드에 붙인다(UE `PlatformNULLContextSetup`·`PlatformRenderingContextSetup` 대응).

### 5.7 생성 실패 보고

`CreateOpenGLDevice(allocator, desc, [options,] report)`의 `DeviceCreateReport`([로깅](LoggingSystem.ko.md) 13절 모양)에 실패 단계(호출 이름), 원본 오류(GL error, EGL error, Win32 `GetLastError`, 버전 비교면 major×10+minor), 설명을 적는다. 장치 안 실패는 `LastError`에 직전 GL 오류 이름·값을 덧붙인다. 로그는 LG7 전이라 `TODO(log)`로 남겼다.

## 6. 공개 제약

| 제약 | 공개 방법 |
|---|---|
| ES 3.2 미만, clip control 없는 Windows 드라이버, `GL_ARB_ES3_2_compatibility` 없는 데스크톱 core | 장치 생성 `Unsupported` + `DeviceCreateReport` |
| timestamp 확장 없음 | `DeviceInfo::timestamps` false, `WriteTimestamp`·`CalibrateClocks` `Unsupported` |
| 끊김 표시 없음(데스크톱 core) | `DeviceInfo::timestampDisjointDetection` false |
| 확장 형식이 없는 장치 | `CreateTexture` `Unsupported`(대신하지 않는다) |
| `Bgra8Unorm` 정점 형식 | `CreatePipeline` `Unsupported` |
| 이방성 확장 없음 | Trilinear로 대신(기존 `SamplerDesc` 규약) |
| 기록 중 갱신 | GL은 명령 순서로 실행하므로 갱신 뒤의 그리기부터 새 내용을 본다(D3D12는 그 프레임 전체). 지금 시험은 갱신을 프레임 밖에서 한다 |
| sRGB 쓰기 스위치도 sRGB 창 표면도 없는 Android 기기 | 창 표면 생성 `Unsupported`(P13에서 셰이더 present) |
| 자원 생성 스레드 | `concurrentResourceCreation` false |

## 7. 셰이더 원문 경로

선택: **(나) 손으로 쓴 GLSL ES 3.20 원문**이며 모양을 쿠킹된 `GLES31_PC` 출력에 맞춘다.

- 원문: `#version 320 es`, `precision highp float/int`, 정점 입력 `layout(location = n) in ... in_var_ATTRIBUTEn`, 단계 사이 `layout(location = n) ... out_var_TEXCOORDn`/`in_var_TEXCOORDn`, PS 출력 `layout(location = 0) out vec4 out_var_SV_Target0`(SPIRV-Cross 이름), 그리기 상수 `uniform highp vec4 vu_h[5]`와 `#define transform mat4(vu_h[0], vu_h[1], vu_h[2], vu_h[3])`, 텍스처 `layout(binding = 0) uniform highp sampler2D ps0`. 클립 공간 보정은 없다(`fixupClipSpace`·`flipVertexY` false).
- 근거:
  1. 형식 정의와 맞는다: ESSL 320, 행 우선 byte(DXC `-Zpr`)를 열 우선 `mat4`로 읽어 `M * v`(셰이더 시스템 7.2절 6항), 클립 공간 보정 끔, 루트 상수 에뮬레이션 이름·텍스처 유닛(7.2절 4항).
  2. 장치 쪽 경로가 SH2와 같다: 루트 상수 배열 위치 조회와 `glUniform4fv` 업로드, `binding`으로 고정한 유닛. SH2는 원문의 출처(쿠킹된 ESSL)와 상수 버퍼 복사 표만 더한다.
  3. (가) DXC → SPIR-V → SPIRV-Cross를 런타임에 쓰면 Sandbox 실행 파일이 개발 도구 DLL을 열어야 하고(Release 배제 정책), 상수 버퍼를 vec4 배열로 바꾸는 텍스트 재작성은 SH2 워커(ShaderFormatOpenGL) 범위다. 지금 Sandbox HLSL도 SH 규약(register 없음, 샘플러 짝)을 따르지 않는다.
- 한계: `GLES31_ANDROID` 원문(셰이더 쪽 y 반전)은 손으로 두지 않았다. Android 규약의 픽셀 대조는 SH6(쿠킹된 원문 + 기기)에서 한다.

## 8. 시험(2026-11-17)

- Sandbox `--self-test`가 `OpenGL ES`(ES profile)와 `OpenGL ES on GL core`(데스크톱 core)를 따로 만들어 D3D12와 같은 검사를 돌린다: 픽셀, 제출·완료·회수, 소유권 이관, 스왑체인, 컬링·음수 행렬식·행 우선 행렬(창·offscreen 경로 byte 대조), 정점 형식 20종(`Bgra8Unorm`은 `Unsupported` 음성 대조), 업로드·readback, 계측, 렌더 lane. 두 context 모두 통과, D3D12와 픽셀 차이 0.
- 독립 리뷰 1회 반영: ES 사양의 sRGB blit 의미(`rawSrgbBlit`), 표면 전환 실패 뒤 context 복구·손실 게시, EGL debug 속성, timestamp 조사 함수, 생성 보고의 첫 시도 원인·잘림 표시, 기록 중 회수 실패 때 창 표면 해제, 창 표면 전환 뒤 viewport 복원.
- 음성 대조(임시 스위치, 확인 뒤 제거): present의 Y 반전을 빼면 두 context에서 창 경로 7건 불일치, 앞면을 `GL_CW`로 두면 컬 검사 6건 실패, present blit에서 sRGB 쓰기 스위치를 켜 두면 ES profile에서 창 경로 3건 불일치.
- Android(`-p:RocksPlatform=android`) 교차 빌드에서 Common·Android 층이 컴파일·링크된다. 실행은 P13.

## 9. 셰이더 시스템 스펙과 다른 점

- P11 행은 Windows 미리보기를 "GL 4.3 core 이상"으로 적었다. 구현은 ES profile을 우선하고 4.3 core를 대체로 둔다(측정 2.2절). 형식 정의 `GLES31_PC.json`의 `deviceRequirements`는 데스크톱 core 대체 경로의 요구로 남는다.
- 7.2절 8항은 이 전환을 SH2에 두었다. 클립 공간·앞면·present Y 반전은 P11에서 구현했다.
- 7.2절 4항 에뮬레이션은 루트 상수 배열만 P11에서 구현했다. 상수 버퍼 배열·복사 표·바인딩 머리는 SH2다.
- P11 의존으로 A0-G(G2)를 적었지만 A0-G 형식 확장 전에 진행했다. 형식 표는 지금 형식만 두고 확장 조건 열을 미리 두었다(5.1절).

## 10. 후속

- P13 Android 실행 기반: 앱 진입·`ANativeWindow` 수명·APK, 에뮬레이터·기기 실행(측정 S11), sRGB 스위치·sRGB 표면이 없는 기기의 셰이더 present.
- SH2: 쿠킹된 ESSL과 바인딩 머리 받기, 상수 버퍼 에뮬레이션(`<단계>c<N>_<타입>`)·복사 표, `ShaderSource::glsl` 제거.
- SH6: `GLES31_ANDROID` 픽셀 대조(앞면 `GL_CW`·y 반전), 기기 호환성(S12 기기 부분).
- 픽셀 형식 병합 뒤 형식 표 확장과 `Format` → `PixelFormat` 이름 맞춤.
- LG7: `TODO(log)` 자리에 로그.

## 11. UE 대응표

| Rocks | UE 5.8 OpenGLDrv |
|---|---|
| `OpenGLGraphics` 모듈, `CreateOpenGLDevice` | `OpenGLDrv`, `FOpenGLDynamicRHIModule` |
| `GlesFunctions`(ES 3.2 core 함수 표) | `FOpenGLBase`·`FOpenGLES`의 함수 포인터 |
| `GlesPlatform`·`WglPlatform`·`EglPlatform` | `FPlatformOpenGLDevice`·`FPlatformOpenGLContext`(`WindowsOpenGLPlatform.cpp`, `AndroidOpenGL.cpp`·`AndroidEGL.cpp`) |
| `ClipConvention` | `FOpenGL::SupportsClipControl` |
| 스왑체인 offscreen 텍스처 + present blit | viewport 백버퍼 + `PlatformBlitToViewport` |
| 루트 상수 배열 `vu_h`·`pu_h` | 전역 packed uniform 배열(`$Globals`) |
| `GlesFeature`·`GlesFeatures` | `FOpenGLES::bSupports*` |
| fence 관찰 스레드 | `FOpenGLGPUFence`·RHI 스레드 polling |
