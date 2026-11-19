#pragma once

#include <Base/NumericLimits.h>
#include <Base/Types.h>

// OpenGL ES 3.2 공통 API의 형식·상수·함수 표다(설계: Docs/Design/OpenGLESGraphics.ko.md 3절).
//
// 공통 코드는 플랫폼 GL 헤더(GL/gl.h, GLES3/gl32.h)를 include하지 않는다. 형식과 상수는 Khronos
// 헤더 값을 이 namespace에 직접 두고, 함수는 플랫폼 층(WGL·EGL)이 준 주소 조회 함수로 실행 시
// 얻는다. 그래서 같은 공통 코드가 Windows 미리보기와 Android에서 그대로 컴파일된다. 이 파일을
// include하는 번역 단위는 플랫폼 GL 헤더를 함께 include하면 안 된다(GL_ 매크로와 이름이 겹친다).
//
// 함수 표에는 OpenGL ES 3.2 core 함수만 둔다. 데스크톱 GL 전용 함수(glClipControl, DSA)는 Windows
// 플랫폼 층만 따로 얻고, 확장 함수(timestamp query)는 확장 표(GlesExtensionFunctions)에 둔다.
namespace rocks::graphics::gles {

// GL 진입점의 호출 규약은 Windows에서 APIENTRY(__stdcall)이고 Android에서 기본 규약이다. Rocks는
// 64-bit 대상만 빌드하며 64-bit에서는 __stdcall이 기본 규약과 같으므로 함수 표는 규약을 적지 않는다
// (공통 코드에 플랫폼 분기를 두지 않는다).
static_assert(sizeof(void*) == 8, "The OpenGL ES function table assumes a 64-bit target where "
                                  "APIENTRY is the default calling convention");

using GLenum = unsigned int;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;
using GLint = int;
using GLuint = unsigned int;
using GLsizei = int;
using GLfloat = float;
using GLchar = char;
using GLubyte = unsigned char;
using GLint64 = int64;
using GLuint64 = uint64;
using GLsizeiptr = isize;
using GLintptr = isize;
using GLsync = struct __GLsync*;
using GLDEBUGPROC = void (*)(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                             const GLchar* message, const void* user);

inline constexpr GLboolean GL_FALSE = 0;
inline constexpr GLboolean GL_TRUE = 1;

// --- 상태·조회 ---
inline constexpr GLenum GL_NO_ERROR = 0;
inline constexpr GLenum GL_INVALID_ENUM = 0x0500;
inline constexpr GLenum GL_INVALID_VALUE = 0x0501;
inline constexpr GLenum GL_INVALID_OPERATION = 0x0502;
inline constexpr GLenum GL_OUT_OF_MEMORY = 0x0505;
inline constexpr GLenum GL_INVALID_FRAMEBUFFER_OPERATION = 0x0506;
inline constexpr GLenum GL_CONTEXT_LOST = 0x0507;
inline constexpr GLenum GL_RENDERER = 0x1F01;
inline constexpr GLenum GL_VERSION = 0x1F02;
inline constexpr GLenum GL_EXTENSIONS = 0x1F03;
inline constexpr GLenum GL_SHADING_LANGUAGE_VERSION = 0x8B8C;
inline constexpr GLenum GL_MAJOR_VERSION = 0x821B;
inline constexpr GLenum GL_MINOR_VERSION = 0x821C;
inline constexpr GLenum GL_NUM_EXTENSIONS = 0x821D;
inline constexpr GLenum GL_MAX_TEXTURE_SIZE = 0x0D33;
inline constexpr GLenum GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS = 0x8B4D;
inline constexpr GLenum GL_MAX_VERTEX_UNIFORM_VECTORS = 0x8DFB;
inline constexpr GLenum GL_MAX_FRAGMENT_UNIFORM_VECTORS = 0x8DFD;
inline constexpr GLenum GL_MAX_VERTEX_ATTRIBS = 0x8869;

// --- 기능 켜기 ---
inline constexpr GLenum GL_CULL_FACE = 0x0B44;
inline constexpr GLenum GL_DEPTH_TEST = 0x0B71;
inline constexpr GLenum GL_DITHER = 0x0BD0;
inline constexpr GLenum GL_BLEND = 0x0BE2;
inline constexpr GLenum GL_SCISSOR_TEST = 0x0C11;
// 데스크톱 GL core와 GL_EXT_sRGB_write_control의 sRGB 쓰기 변환 스위치다(같은 값).
inline constexpr GLenum GL_FRAMEBUFFER_SRGB = 0x8DB9;
inline constexpr GLenum GL_DEBUG_OUTPUT = 0x92E0;
inline constexpr GLenum GL_DEBUG_OUTPUT_SYNCHRONOUS = 0x8242;

// --- 래스터·블렌드 ---
inline constexpr GLenum GL_FRONT = 0x0404;
inline constexpr GLenum GL_BACK = 0x0405;
inline constexpr GLenum GL_CW = 0x0900;
inline constexpr GLenum GL_CCW = 0x0901;
inline constexpr GLenum GL_ZERO = 0;
inline constexpr GLenum GL_ONE = 1;
inline constexpr GLenum GL_SRC_ALPHA = 0x0302;
inline constexpr GLenum GL_ONE_MINUS_SRC_ALPHA = 0x0303;
inline constexpr GLenum GL_TRIANGLES = 0x0004;
inline constexpr GLbitfield GL_COLOR_BUFFER_BIT = 0x00004000;

// --- 데이터 형식 ---
inline constexpr GLenum GL_BYTE = 0x1400;
inline constexpr GLenum GL_UNSIGNED_BYTE = 0x1401;
inline constexpr GLenum GL_SHORT = 0x1402;
inline constexpr GLenum GL_UNSIGNED_SHORT = 0x1403;
inline constexpr GLenum GL_INT = 0x1404;
inline constexpr GLenum GL_UNSIGNED_INT = 0x1405;
inline constexpr GLenum GL_FLOAT = 0x1406;
inline constexpr GLenum GL_HALF_FLOAT = 0x140B;
inline constexpr GLenum GL_UNSIGNED_INT_2_10_10_10_REV = 0x8368;
inline constexpr GLenum GL_UNSIGNED_SHORT_5_6_5 = 0x8363;
inline constexpr GLenum GL_UNSIGNED_INT_10F_11F_11F_REV = 0x8C3B;
inline constexpr GLenum GL_UNSIGNED_INT_5_9_9_9_REV = 0x8C3E;
inline constexpr GLenum GL_UNSIGNED_INT_24_8 = 0x84FA;
inline constexpr GLenum GL_FLOAT_32_UNSIGNED_INT_24_8_REV = 0x8DAD;

// 외부(업로드·readback) 형식이다.
inline constexpr GLenum GL_STENCIL_INDEX = 0x1901;
inline constexpr GLenum GL_DEPTH_COMPONENT = 0x1902;
inline constexpr GLenum GL_RED = 0x1903;
inline constexpr GLenum GL_GREEN = 0x1904;
inline constexpr GLenum GL_BLUE = 0x1905;
inline constexpr GLenum GL_ALPHA = 0x1906;
inline constexpr GLenum GL_RGB = 0x1907;
inline constexpr GLenum GL_RGBA = 0x1908;
inline constexpr GLenum GL_RG = 0x8227;
inline constexpr GLenum GL_RG_INTEGER = 0x8228;
inline constexpr GLenum GL_RED_INTEGER = 0x8D94;
inline constexpr GLenum GL_RGB_INTEGER = 0x8D98;
inline constexpr GLenum GL_RGBA_INTEGER = 0x8D99;
inline constexpr GLenum GL_DEPTH_STENCIL = 0x84F9;
// GL_EXT_texture_format_BGRA8888이다.
inline constexpr GLenum GL_BGRA_EXT = 0x80E1;

// 내부 형식(ES 3.2 Table 8.10·8.11·8.17과 확장)이다.
inline constexpr GLenum GL_R8 = 0x8229;
inline constexpr GLenum GL_R8I = 0x8231;
inline constexpr GLenum GL_R8UI = 0x8232;
inline constexpr GLenum GL_RG8 = 0x822B;
inline constexpr GLenum GL_RG8_SNORM = 0x8F95;
inline constexpr GLenum GL_RG8UI = 0x8238;
inline constexpr GLenum GL_RGB8 = 0x8051;
inline constexpr GLenum GL_SRGB8 = 0x8C41;
inline constexpr GLenum GL_RGBA8 = 0x8058;
inline constexpr GLenum GL_SRGB8_ALPHA8 = 0x8C43;
inline constexpr GLenum GL_RGBA8_SNORM = 0x8F97;
inline constexpr GLenum GL_RGBA8UI = 0x8D7C;
inline constexpr GLenum GL_R16I = 0x8233;
inline constexpr GLenum GL_R16UI = 0x8234;
inline constexpr GLenum GL_R16F = 0x822D;
inline constexpr GLenum GL_RG16I = 0x8239;
inline constexpr GLenum GL_RG16UI = 0x823A;
inline constexpr GLenum GL_RG16F = 0x822F;
inline constexpr GLenum GL_RGBA16I = 0x8D88;
inline constexpr GLenum GL_RGBA16UI = 0x8D76;
inline constexpr GLenum GL_RGBA16F = 0x881A;
inline constexpr GLenum GL_R32I = 0x8235;
inline constexpr GLenum GL_R32UI = 0x8236;
inline constexpr GLenum GL_R32F = 0x822E;
inline constexpr GLenum GL_RG32UI = 0x823C;
inline constexpr GLenum GL_RG32F = 0x8230;
inline constexpr GLenum GL_RGB32I = 0x8D83;
inline constexpr GLenum GL_RGB32UI = 0x8D71;
inline constexpr GLenum GL_RGB32F = 0x8815;
inline constexpr GLenum GL_RGBA32UI = 0x8D70;
inline constexpr GLenum GL_RGBA32F = 0x8814;
inline constexpr GLenum GL_RGB565 = 0x8D62;
inline constexpr GLenum GL_RGB10_A2 = 0x8059;
inline constexpr GLenum GL_R11F_G11F_B10F = 0x8C3A;
inline constexpr GLenum GL_RGB9_E5 = 0x8C3D;
inline constexpr GLenum GL_DEPTH_COMPONENT16 = 0x81A5;
inline constexpr GLenum GL_DEPTH_COMPONENT24 = 0x81A6;
inline constexpr GLenum GL_DEPTH_COMPONENT32F = 0x8CAC;
inline constexpr GLenum GL_DEPTH24_STENCIL8 = 0x88F0;
inline constexpr GLenum GL_DEPTH32F_STENCIL8 = 0x8CAD;
// GL_EXT_texture_sRGB_R8·GL_EXT_texture_sRGB_RG8이다.
inline constexpr GLenum GL_SR8_EXT = 0x8FBD;
inline constexpr GLenum GL_SRG8_EXT = 0x8FBE;
// GL_EXT_texture_format_BGRA8888(개정 1.4)이다.
inline constexpr GLenum GL_BGRA8_EXT = 0x93A1;
// GL_EXT_texture_norm16이다.
inline constexpr GLenum GL_R16_EXT = 0x822A;
inline constexpr GLenum GL_RG16_EXT = 0x822C;
inline constexpr GLenum GL_RGBA16_EXT = 0x805B;
inline constexpr GLenum GL_RG16_SNORM_EXT = 0x8F99;
inline constexpr GLenum GL_RGBA16_SNORM_EXT = 0x8F9B;
// 블록 압축 내부 형식이다(S3TC·RGTC·BPTC는 확장, ETC2·EAC·ASTC LDR은 ES 3.2 core).
inline constexpr GLenum GL_COMPRESSED_RGBA_S3TC_DXT1_EXT = 0x83F1;
inline constexpr GLenum GL_COMPRESSED_RGBA_S3TC_DXT3_EXT = 0x83F2;
inline constexpr GLenum GL_COMPRESSED_RGBA_S3TC_DXT5_EXT = 0x83F3;
inline constexpr GLenum GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT = 0x8C4D;
inline constexpr GLenum GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT = 0x8C4E;
inline constexpr GLenum GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT = 0x8C4F;
inline constexpr GLenum GL_COMPRESSED_RED_RGTC1_EXT = 0x8DBB;
inline constexpr GLenum GL_COMPRESSED_RED_GREEN_RGTC2_EXT = 0x8DBD;
inline constexpr GLenum GL_COMPRESSED_RGBA_BPTC_UNORM_EXT = 0x8E8C;
inline constexpr GLenum GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM_EXT = 0x8E8D;
inline constexpr GLenum GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT_EXT = 0x8E8F;
inline constexpr GLenum GL_COMPRESSED_R11_EAC = 0x9270;
inline constexpr GLenum GL_COMPRESSED_RG11_EAC = 0x9272;
inline constexpr GLenum GL_COMPRESSED_RGB8_ETC2 = 0x9274;
inline constexpr GLenum GL_COMPRESSED_SRGB8_ETC2 = 0x9275;
inline constexpr GLenum GL_COMPRESSED_RGBA8_ETC2_EAC = 0x9278;
inline constexpr GLenum GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC = 0x9279;
inline constexpr GLenum GL_COMPRESSED_RGBA_ASTC_4x4 = 0x93B0;
inline constexpr GLenum GL_COMPRESSED_RGBA_ASTC_6x6 = 0x93B4;
inline constexpr GLenum GL_COMPRESSED_RGBA_ASTC_8x8 = 0x93B7;
inline constexpr GLenum GL_COMPRESSED_RGBA_ASTC_10x10 = 0x93BB;
inline constexpr GLenum GL_COMPRESSED_RGBA_ASTC_12x12 = 0x93BD;
inline constexpr GLenum GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4 = 0x93D0;
inline constexpr GLenum GL_COMPRESSED_SRGB8_ALPHA8_ASTC_6x6 = 0x93D4;
inline constexpr GLenum GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x8 = 0x93D7;
inline constexpr GLenum GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x10 = 0x93DB;
inline constexpr GLenum GL_COMPRESSED_SRGB8_ALPHA8_ASTC_12x12 = 0x93DD;

// --- 버퍼 ---
inline constexpr GLenum GL_ARRAY_BUFFER = 0x8892;
inline constexpr GLenum GL_ELEMENT_ARRAY_BUFFER = 0x8893;
inline constexpr GLenum GL_PIXEL_PACK_BUFFER = 0x88EB;
inline constexpr GLenum GL_COPY_READ_BUFFER = 0x8F36;
inline constexpr GLenum GL_COPY_WRITE_BUFFER = 0x8F37;
inline constexpr GLenum GL_STATIC_DRAW = 0x88E4;
inline constexpr GLenum GL_STREAM_READ = 0x88E1;
inline constexpr GLbitfield GL_MAP_READ_BIT = 0x0001;

// --- 텍스처·샘플러 ---
inline constexpr GLenum GL_TEXTURE_2D = 0x0DE1;
// 차원별 대상이다(ES 3.0 core: 3D·2D 배열·cube, ES 3.2 core: cube 배열).
inline constexpr GLenum GL_TEXTURE_3D = 0x806F;
inline constexpr GLenum GL_TEXTURE_2D_ARRAY = 0x8C1A;
inline constexpr GLenum GL_TEXTURE_CUBE_MAP = 0x8513;
inline constexpr GLenum GL_TEXTURE_CUBE_MAP_POSITIVE_X = 0x8515;
inline constexpr GLenum GL_TEXTURE_CUBE_MAP_ARRAY = 0x9009;
inline constexpr GLenum GL_TEXTURE_WRAP_R = 0x8072;
// 깊이 비교 샘플러다(ES 3.0 core).
inline constexpr GLenum GL_TEXTURE_COMPARE_MODE = 0x884C;
inline constexpr GLenum GL_TEXTURE_COMPARE_FUNC = 0x884D;
inline constexpr GLenum GL_COMPARE_REF_TO_TEXTURE = 0x884E;
inline constexpr GLenum GL_TEXTURE0 = 0x84C0;
inline constexpr GLenum GL_NEAREST = 0x2600;
inline constexpr GLenum GL_LINEAR = 0x2601;
inline constexpr GLenum GL_NEAREST_MIPMAP_NEAREST = 0x2700;
inline constexpr GLenum GL_LINEAR_MIPMAP_NEAREST = 0x2701;
inline constexpr GLenum GL_LINEAR_MIPMAP_LINEAR = 0x2703;
inline constexpr GLenum GL_TEXTURE_MAG_FILTER = 0x2800;
inline constexpr GLenum GL_TEXTURE_MIN_FILTER = 0x2801;
inline constexpr GLenum GL_TEXTURE_WRAP_S = 0x2802;
inline constexpr GLenum GL_TEXTURE_WRAP_T = 0x2803;
inline constexpr GLenum GL_REPEAT = 0x2901;
inline constexpr GLenum GL_CLAMP_TO_EDGE = 0x812F;
inline constexpr GLenum GL_MIRRORED_REPEAT = 0x8370;
// ES 3.2 core(GL_OES_texture_border_clamp를 흡수)다.
inline constexpr GLenum GL_CLAMP_TO_BORDER = 0x812D;
inline constexpr GLenum GL_TEXTURE_BORDER_COLOR = 0x1004;
// GL_EXT_texture_filter_anisotropic이다.
inline constexpr GLenum GL_TEXTURE_MAX_ANISOTROPY = 0x84FE;
inline constexpr GLenum GL_MAX_TEXTURE_MAX_ANISOTROPY = 0x84FF;
// 텍스처 성분 대응(ES 3.0 core)과 깊이·스텐실 텍스처의 읽기 대상(ES 3.1 core)이다.
inline constexpr GLenum GL_TEXTURE_SWIZZLE_R = 0x8E42;
inline constexpr GLenum GL_TEXTURE_SWIZZLE_G = 0x8E43;
inline constexpr GLenum GL_TEXTURE_SWIZZLE_B = 0x8E44;
inline constexpr GLenum GL_TEXTURE_SWIZZLE_A = 0x8E45;
inline constexpr GLenum GL_DEPTH_STENCIL_TEXTURE_MODE = 0x90EA;
inline constexpr GLenum GL_UNPACK_ALIGNMENT = 0x0CF5;
inline constexpr GLenum GL_PACK_ALIGNMENT = 0x0D05;

// --- framebuffer ---
inline constexpr GLenum GL_FRAMEBUFFER = 0x8D40;
inline constexpr GLenum GL_READ_FRAMEBUFFER = 0x8CA8;
inline constexpr GLenum GL_DRAW_FRAMEBUFFER = 0x8CA9;
inline constexpr GLenum GL_COLOR_ATTACHMENT0 = 0x8CE0;
inline constexpr GLenum GL_FRAMEBUFFER_COMPLETE = 0x8CD5;
// 읽기 framebuffer의 구현 선택 ReadPixels 형식·타입이다(ES 3.2 16.1.2절).
inline constexpr GLenum GL_IMPLEMENTATION_COLOR_READ_TYPE = 0x8B9A;
inline constexpr GLenum GL_IMPLEMENTATION_COLOR_READ_FORMAT = 0x8B9B;

// --- 셰이더·프로그램 ---
inline constexpr GLenum GL_FRAGMENT_SHADER = 0x8B30;
inline constexpr GLenum GL_VERTEX_SHADER = 0x8B31;
inline constexpr GLenum GL_COMPILE_STATUS = 0x8B81;
inline constexpr GLenum GL_LINK_STATUS = 0x8B82;
inline constexpr GLenum GL_INFO_LOG_LENGTH = 0x8B84;
inline constexpr GLenum GL_ACTIVE_ATTRIBUTES = 0x8B89;
inline constexpr GLenum GL_FLOAT_VEC2 = 0x8B50;
inline constexpr GLenum GL_FLOAT_VEC3 = 0x8B51;
inline constexpr GLenum GL_FLOAT_VEC4 = 0x8B52;
inline constexpr GLenum GL_INT_VEC2 = 0x8B53;
inline constexpr GLenum GL_INT_VEC3 = 0x8B54;
inline constexpr GLenum GL_INT_VEC4 = 0x8B55;
inline constexpr GLenum GL_UNSIGNED_INT_VEC2 = 0x8DC6;
inline constexpr GLenum GL_UNSIGNED_INT_VEC3 = 0x8DC7;
inline constexpr GLenum GL_UNSIGNED_INT_VEC4 = 0x8DC8;

// --- 동기화·query ---
inline constexpr GLenum GL_SYNC_GPU_COMMANDS_COMPLETE = 0x9117;
inline constexpr GLenum GL_ALREADY_SIGNALED = 0x911A;
inline constexpr GLenum GL_TIMEOUT_EXPIRED = 0x911B;
inline constexpr GLenum GL_CONDITION_SATISFIED = 0x911C;
inline constexpr GLenum GL_WAIT_FAILED = 0x911D;
inline constexpr GLenum GL_QUERY_RESULT = 0x8866;
inline constexpr GLenum GL_QUERY_RESULT_AVAILABLE = 0x8867;
// GL_EXT_disjoint_timer_query(ES)와 데스크톱 GL 3.3 timer query가 같은 값을 쓴다.
inline constexpr GLenum GL_TIMESTAMP = 0x8E28;
inline constexpr GLenum GL_QUERY_COUNTER_BITS = 0x8864;
inline constexpr GLenum GL_GPU_DISJOINT = 0x8FBB;

// --- 장치 한도(GX1, ES 3.2 core 6.1절 표 20.40–20.48) ---
inline constexpr GLenum GL_MAX_COLOR_ATTACHMENTS = 0x8CDF;
inline constexpr GLenum GL_MAX_DRAW_BUFFERS = 0x8824;
inline constexpr GLenum GL_MAX_VERTEX_UNIFORM_BLOCKS = 0x8A2B;
inline constexpr GLenum GL_MAX_FRAGMENT_UNIFORM_BLOCKS = 0x8A2D;
inline constexpr GLenum GL_MAX_COMPUTE_UNIFORM_BLOCKS = 0x91BB;
inline constexpr GLenum GL_MAX_TEXTURE_IMAGE_UNITS = 0x8872;
inline constexpr GLenum GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS = 0x8B4C;
inline constexpr GLenum GL_MAX_COMPUTE_TEXTURE_IMAGE_UNITS = 0x91BC;
inline constexpr GLenum GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS = 0x90D6;
inline constexpr GLenum GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS = 0x90DA;
inline constexpr GLenum GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS = 0x90DB;
inline constexpr GLenum GL_MAX_COMBINED_SHADER_STORAGE_BLOCKS = 0x90DC;
inline constexpr GLenum GL_MAX_VERTEX_IMAGE_UNIFORMS = 0x90CA;
inline constexpr GLenum GL_MAX_FRAGMENT_IMAGE_UNIFORMS = 0x90CE;
inline constexpr GLenum GL_MAX_COMPUTE_IMAGE_UNIFORMS = 0x91BD;
inline constexpr GLenum GL_MAX_COMBINED_IMAGE_UNIFORMS = 0x90CF;
inline constexpr GLenum GL_MAX_COMPUTE_UNIFORM_COMPONENTS = 0x8263;
inline constexpr GLenum GL_MAX_UNIFORM_BLOCK_SIZE = 0x8A30;
inline constexpr GLenum GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT = 0x8A34;
inline constexpr GLenum GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT = 0x90DF;
inline constexpr GLenum GL_TEXTURE_BUFFER_OFFSET_ALIGNMENT = 0x919F;
inline constexpr GLenum GL_MAX_SHADER_STORAGE_BLOCK_SIZE = 0x90DE;
inline constexpr GLenum GL_MAX_TEXTURE_BUFFER_SIZE = 0x8C2B;
inline constexpr GLenum GL_MAX_VERTEX_ATTRIB_BINDINGS = 0x82DA;
inline constexpr GLenum GL_MAX_VARYING_VECTORS = 0x8DFC;
inline constexpr GLenum GL_MAX_COMPUTE_WORK_GROUP_COUNT = 0x91BE;
inline constexpr GLenum GL_MAX_COMPUTE_WORK_GROUP_SIZE = 0x91BF;
inline constexpr GLenum GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS = 0x90EB;
inline constexpr GLenum GL_MAX_COMPUTE_SHARED_MEMORY_SIZE = 0x8262;
inline constexpr GLenum GL_MAX_3D_TEXTURE_SIZE = 0x8073;
inline constexpr GLenum GL_MAX_CUBE_MAP_TEXTURE_SIZE = 0x851C;
inline constexpr GLenum GL_MAX_ARRAY_TEXTURE_LAYERS = 0x88FF;
// --- MSAA 질의(glGetInternalformativ, ES 3.1 core) ---
inline constexpr GLenum GL_TEXTURE_2D_MULTISAMPLE = 0x9100;
inline constexpr GLenum GL_SAMPLES = 0x80A9;
inline constexpr GLenum GL_NUM_SAMPLE_COUNTS = 0x9380;
// --- 버퍼 view(GX2): 버퍼 텍스처(ES 3.2 core)·storage block(ES 3.1 core) ---
inline constexpr GLenum GL_TEXTURE_BUFFER = 0x8C2A;
inline constexpr GLenum GL_SHADER_STORAGE_BUFFER = 0x90D2;
inline constexpr GLenum GL_ACTIVE_UNIFORMS = 0x8B86;
inline constexpr GLenum GL_SAMPLER_BUFFER = 0x8DC2;
inline constexpr GLenum GL_INT_SAMPLER_BUFFER = 0x8DD0;
inline constexpr GLenum GL_UNSIGNED_INT_SAMPLER_BUFFER = 0x8DD8;
// 그 밖의 sampler 타입(배치 대조용, ES 3.2 Table 7.3)이다.
inline constexpr GLenum GL_SAMPLER_2D = 0x8B5E;
inline constexpr GLenum GL_SAMPLER_3D = 0x8B5F;
inline constexpr GLenum GL_SAMPLER_CUBE = 0x8B60;
inline constexpr GLenum GL_SAMPLER_2D_SHADOW = 0x8B62;
inline constexpr GLenum GL_SAMPLER_2D_ARRAY = 0x8DC1;
inline constexpr GLenum GL_SAMPLER_2D_ARRAY_SHADOW = 0x8DC4;
inline constexpr GLenum GL_SAMPLER_CUBE_SHADOW = 0x8DC5;
inline constexpr GLenum GL_INT_SAMPLER_2D = 0x8DCA;
inline constexpr GLenum GL_INT_SAMPLER_3D = 0x8DCB;
inline constexpr GLenum GL_INT_SAMPLER_CUBE = 0x8DCC;
inline constexpr GLenum GL_INT_SAMPLER_2D_ARRAY = 0x8DCF;
inline constexpr GLenum GL_UNSIGNED_INT_SAMPLER_2D = 0x8DD2;
inline constexpr GLenum GL_UNSIGNED_INT_SAMPLER_3D = 0x8DD3;
inline constexpr GLenum GL_UNSIGNED_INT_SAMPLER_CUBE = 0x8DD4;
inline constexpr GLenum GL_UNSIGNED_INT_SAMPLER_2D_ARRAY = 0x8DD7;
inline constexpr GLenum GL_SAMPLER_CUBE_MAP_ARRAY = 0x900C;
inline constexpr GLenum GL_SAMPLER_CUBE_MAP_ARRAY_SHADOW = 0x900D;
inline constexpr GLenum GL_INT_SAMPLER_CUBE_MAP_ARRAY = 0x900E;
inline constexpr GLenum GL_UNSIGNED_INT_SAMPLER_CUBE_MAP_ARRAY = 0x900F;
inline constexpr GLenum GL_SAMPLER_2D_MULTISAMPLE = 0x9108;
inline constexpr GLenum GL_INT_SAMPLER_2D_MULTISAMPLE = 0x9109;
inline constexpr GLenum GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE = 0x910A;
inline constexpr GLenum GL_SAMPLER_2D_MULTISAMPLE_ARRAY = 0x910B;
inline constexpr GLenum GL_INT_SAMPLER_2D_MULTISAMPLE_ARRAY = 0x910C;
inline constexpr GLenum GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE_ARRAY = 0x910D;
// image uniform 타입 범위다(GL_IMAGE_1D 0x904C부터 GL_UNSIGNED_INT_IMAGE_2D_MULTISAMPLE_ARRAY
// 0x906C까지, ES 타입은 이 안에 있다).
inline constexpr GLenum GL_FIRST_IMAGE_TYPE = 0x904C;
inline constexpr GLenum GL_LAST_IMAGE_TYPE = 0x906C;
inline constexpr GLenum GL_SHADER_STORAGE_BLOCK = 0x92E6;
inline constexpr GLenum GL_ACTIVE_RESOURCES = 0x92F5;
inline constexpr GLenum GL_BUFFER_BINDING = 0x9302;
inline constexpr GLenum GL_BUFFER_VARIABLE = 0x92E5;
inline constexpr GLenum GL_NUM_ACTIVE_VARIABLES = 0x9304;
inline constexpr GLenum GL_ACTIVE_VARIABLES = 0x9305;
inline constexpr GLenum GL_TOP_LEVEL_ARRAY_STRIDE = 0x930D;
// --- 파이프라인 고정 기능 상태(GX3, ES 3.2 core) ---
inline constexpr GLenum GL_STENCIL_TEST = 0x0B90;
inline constexpr GLenum GL_POLYGON_OFFSET_FILL = 0x8037;
inline constexpr GLenum GL_SAMPLE_ALPHA_TO_COVERAGE = 0x809E;
// GL_EXT_depth_clamp(ES)·데스크톱 GL core의 깊이 clamp 스위치다(같은 값).
inline constexpr GLenum GL_DEPTH_CLAMP = 0x864F;
inline constexpr GLenum GL_FRONT_AND_BACK = 0x0408;
inline constexpr GLenum GL_POINTS = 0x0000;
inline constexpr GLenum GL_LINES = 0x0001;
inline constexpr GLenum GL_LINE_STRIP = 0x0003;
inline constexpr GLenum GL_TRIANGLE_STRIP = 0x0005;
inline constexpr GLenum GL_NEVER = 0x0200;
inline constexpr GLenum GL_LESS = 0x0201;
inline constexpr GLenum GL_EQUAL = 0x0202;
inline constexpr GLenum GL_LEQUAL = 0x0203;
inline constexpr GLenum GL_GREATER = 0x0204;
inline constexpr GLenum GL_NOTEQUAL = 0x0205;
inline constexpr GLenum GL_GEQUAL = 0x0206;
inline constexpr GLenum GL_ALWAYS = 0x0207;
inline constexpr GLenum GL_KEEP = 0x1E00;
inline constexpr GLenum GL_REPLACE = 0x1E01;
inline constexpr GLenum GL_INCR = 0x1E02;
inline constexpr GLenum GL_DECR = 0x1E03;
inline constexpr GLenum GL_INVERT = 0x150A;
inline constexpr GLenum GL_INCR_WRAP = 0x8507;
inline constexpr GLenum GL_DECR_WRAP = 0x8508;
inline constexpr GLenum GL_SRC_COLOR = 0x0300;
inline constexpr GLenum GL_ONE_MINUS_SRC_COLOR = 0x0301;
inline constexpr GLenum GL_DST_ALPHA = 0x0304;
inline constexpr GLenum GL_ONE_MINUS_DST_ALPHA = 0x0305;
inline constexpr GLenum GL_DST_COLOR = 0x0306;
inline constexpr GLenum GL_ONE_MINUS_DST_COLOR = 0x0307;
inline constexpr GLenum GL_SRC_ALPHA_SATURATE = 0x0308;
inline constexpr GLenum GL_FUNC_ADD = 0x8006;
inline constexpr GLenum GL_MIN = 0x8007;
inline constexpr GLenum GL_MAX = 0x8008;
inline constexpr GLenum GL_FUNC_SUBTRACT = 0x800A;
inline constexpr GLenum GL_FUNC_REVERSE_SUBTRACT = 0x800B;
// --- 패스 attachment(GX4) ---
inline constexpr GLenum GL_COLOR = 0x1800;
inline constexpr GLenum GL_DEPTH = 0x1801;
inline constexpr GLenum GL_STENCIL = 0x1802;
inline constexpr GLenum GL_NONE = 0;
inline constexpr GLenum GL_DEPTH_ATTACHMENT = 0x8D00;
inline constexpr GLenum GL_DEPTH_STENCIL_ATTACHMENT = 0x821A;
// --- compute·간접 명령·복사(GX3·GX4, ES 3.1·3.2 core) ---
inline constexpr GLenum GL_COMPUTE_SHADER = 0x91B9;
inline constexpr GLenum GL_DRAW_INDIRECT_BUFFER = 0x8F3F;
inline constexpr GLenum GL_DISPATCH_INDIRECT_BUFFER = 0x90EE;
inline constexpr GLenum GL_READ_WRITE = 0x88BA;
// 모든 memory barrier bit다(GL 헤더의 0xFFFFFFFF).
inline constexpr GLbitfield GL_ALL_BARRIER_BITS = NumericLimits<GLbitfield>::Max();
inline constexpr GLenum GL_IMAGE_BUFFER = 0x9051;
inline constexpr GLenum GL_IMAGE_2D = 0x904D;
inline constexpr GLenum GL_IMAGE_3D = 0x904E;
inline constexpr GLenum GL_IMAGE_2D_ARRAY = 0x9053;
inline constexpr GLenum GL_IMAGE_CUBE = 0x9050;
inline constexpr GLenum GL_IMAGE_CUBE_MAP_ARRAY = 0x9054;
inline constexpr GLenum GL_INT_IMAGE_CUBE = 0x905B;
inline constexpr GLenum GL_INT_IMAGE_CUBE_MAP_ARRAY = 0x905F;
inline constexpr GLenum GL_UNSIGNED_INT_IMAGE_CUBE = 0x9066;
inline constexpr GLenum GL_UNSIGNED_INT_IMAGE_CUBE_MAP_ARRAY = 0x906A;
inline constexpr GLenum GL_INT_IMAGE_2D = 0x9058;
inline constexpr GLenum GL_INT_IMAGE_3D = 0x9059;
inline constexpr GLenum GL_INT_IMAGE_2D_ARRAY = 0x905E;
inline constexpr GLenum GL_UNSIGNED_INT_IMAGE_2D = 0x9063;
inline constexpr GLenum GL_UNSIGNED_INT_IMAGE_3D = 0x9064;
inline constexpr GLenum GL_UNSIGNED_INT_IMAGE_2D_ARRAY = 0x9069;
inline constexpr GLenum GL_INT_IMAGE_BUFFER = 0x905C;
inline constexpr GLenum GL_UNSIGNED_INT_IMAGE_BUFFER = 0x9067;
// --- debug(KHR_debug, ES 3.2 core) ---
inline constexpr GLenum GL_DEBUG_SOURCE_APPLICATION = 0x824A;
inline constexpr GLenum GL_DEBUG_TYPE_ERROR = 0x824C;
inline constexpr GLenum GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR = 0x824E;

// ES 3.2 core 함수 목록이다. X(반환, 이름(gl 접두사 없이), 인자)로 펼친다. 모두 필수이며 하나라도
// 없으면 장치 생성이 실패한다.
#define ROCKS_GLES_CORE_FUNCTIONS(X)                                                               \
    X(GLenum, GetError, ())                                                                        \
    X(const GLubyte*, GetString, (GLenum))                                                         \
    X(const GLubyte*, GetStringi, (GLenum, GLuint))                                                \
    X(void, GetIntegerv, (GLenum, GLint*))                                                         \
    X(void, GetInteger64v, (GLenum, GLint64*))                                                     \
    X(void, GetIntegeri_v, (GLenum, GLuint, GLint*))                                               \
    X(void, GetInternalformativ, (GLenum, GLenum, GLenum, GLsizei, GLint*))                        \
    X(void, TexBufferRange, (GLenum, GLenum, GLuint, GLintptr, GLsizeiptr))                        \
    X(void, BindBufferRange, (GLenum, GLuint, GLuint, GLintptr, GLsizeiptr))                       \
    X(void, GetActiveUniform, (GLuint, GLuint, GLsizei, GLsizei*, GLint*, GLenum*, GLchar*))       \
    X(void, GetUniformiv, (GLuint, GLint, GLint*))                                                 \
    X(void, GetProgramInterfaceiv, (GLuint, GLenum, GLenum, GLint*))                               \
    X(void, GetProgramResourceiv,                                                                  \
      (GLuint, GLenum, GLuint, GLsizei, const GLenum*, GLsizei, GLsizei*, GLint*))                 \
    X(void, GetProgramResourceName, (GLuint, GLenum, GLuint, GLsizei, GLsizei*, GLchar*))          \
    X(void, GetFloatv, (GLenum, GLfloat*))                                                         \
    X(void, Enable, (GLenum))                                                                      \
    X(void, Disable, (GLenum))                                                                     \
    X(void, Viewport, (GLint, GLint, GLsizei, GLsizei))                                            \
    X(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))                                      \
    X(void, Clear, (GLbitfield))                                                                   \
    X(void, Finish, ())                                                                            \
    X(void, Flush, ())                                                                             \
    X(void, PixelStorei, (GLenum, GLint))                                                          \
    X(void, ReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*))                   \
    X(void, FrontFace, (GLenum))                                                                   \
    X(void, CullFace, (GLenum))                                                                    \
    X(void, BlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum))                                   \
    X(void, GenBuffers, (GLsizei, GLuint*))                                                        \
    X(void, DeleteBuffers, (GLsizei, const GLuint*))                                               \
    X(void, BindBuffer, (GLenum, GLuint))                                                          \
    X(void, BufferData, (GLenum, GLsizeiptr, const void*, GLenum))                                 \
    X(void, BufferSubData, (GLenum, GLintptr, GLsizeiptr, const void*))                            \
    X(void*, MapBufferRange, (GLenum, GLintptr, GLsizeiptr, GLbitfield))                           \
    X(GLboolean, UnmapBuffer, (GLenum))                                                            \
    X(void, GenVertexArrays, (GLsizei, GLuint*))                                                   \
    X(void, DeleteVertexArrays, (GLsizei, const GLuint*))                                          \
    X(void, BindVertexArray, (GLuint))                                                             \
    X(void, EnableVertexAttribArray, (GLuint))                                                     \
    X(void, VertexAttribFormat, (GLuint, GLint, GLenum, GLboolean, GLuint))                        \
    X(void, VertexAttribIFormat, (GLuint, GLint, GLenum, GLuint))                                  \
    X(void, VertexAttribBinding, (GLuint, GLuint))                                                 \
    X(void, BindVertexBuffer, (GLuint, GLuint, GLintptr, GLsizei))                                 \
    X(void, VertexBindingDivisor, (GLuint, GLuint))                                                \
    X(void, DrawArraysInstanced, (GLenum, GLint, GLsizei, GLsizei))                                \
    X(void, DrawElementsInstanced, (GLenum, GLsizei, GLenum, const void*, GLsizei))                \
    X(void, GetActiveAttrib, (GLuint, GLuint, GLsizei, GLsizei*, GLint*, GLenum*, GLchar*))        \
    X(GLint, GetAttribLocation, (GLuint, const GLchar*))                                           \
    X(GLuint, CreateShader, (GLenum))                                                              \
    X(void, ShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*))                   \
    X(void, CompileShader, (GLuint))                                                               \
    X(void, GetShaderiv, (GLuint, GLenum, GLint*))                                                 \
    X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                                \
    X(void, DeleteShader, (GLuint))                                                                \
    X(GLuint, CreateProgram, ())                                                                   \
    X(void, AttachShader, (GLuint, GLuint))                                                        \
    X(void, LinkProgram, (GLuint))                                                                 \
    X(void, GetProgramiv, (GLuint, GLenum, GLint*))                                                \
    X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                               \
    X(void, DeleteProgram, (GLuint))                                                               \
    X(void, UseProgram, (GLuint))                                                                  \
    X(GLint, GetUniformLocation, (GLuint, const GLchar*))                                          \
    X(void, Uniform4fv, (GLint, GLsizei, const GLfloat*))                                          \
    X(void, Uniform1i, (GLint, GLint))                                                             \
    X(void, GenTextures, (GLsizei, GLuint*))                                                       \
    X(void, DeleteTextures, (GLsizei, const GLuint*))                                              \
    X(void, ActiveTexture, (GLenum))                                                               \
    X(void, BindTexture, (GLenum, GLuint))                                                         \
    X(void, TexStorage2D, (GLenum, GLsizei, GLenum, GLsizei, GLsizei))                             \
    X(void, TexStorage2DMultisample, (GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLboolean))       \
    X(void, TexStorage3D, (GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLsizei))                    \
    X(void, TexSubImage3D,                                                                         \
      (GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum,              \
       const void*))                                                                               \
    X(void, CompressedTexSubImage3D,                                                               \
      (GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLsizei,             \
       const void*))                                                                               \
    X(void, TexSubImage2D,                                                                         \
      (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*))                \
    X(void, CompressedTexSubImage2D,                                                               \
      (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, const void*))               \
    X(void, TexParameteri, (GLenum, GLenum, GLint))                                                \
    X(void, GenSamplers, (GLsizei, GLuint*))                                                       \
    X(void, DeleteSamplers, (GLsizei, const GLuint*))                                              \
    X(void, BindSampler, (GLuint, GLuint))                                                         \
    X(void, SamplerParameteri, (GLuint, GLenum, GLint))                                            \
    X(void, SamplerParameterf, (GLuint, GLenum, GLfloat))                                          \
    X(void, SamplerParameterfv, (GLuint, GLenum, const GLfloat*))                                  \
    X(void, GenFramebuffers, (GLsizei, GLuint*))                                                   \
    X(void, DeleteFramebuffers, (GLsizei, const GLuint*))                                          \
    X(void, BindFramebuffer, (GLenum, GLuint))                                                     \
    X(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))                         \
    X(void, FramebufferTextureLayer, (GLenum, GLenum, GLuint, GLint, GLint))                       \
    X(void, Enablei, (GLenum, GLuint))                                                             \
    X(void, Disablei, (GLenum, GLuint))                                                            \
    X(void, BlendFuncSeparatei, (GLuint, GLenum, GLenum, GLenum, GLenum))                          \
    X(void, BlendEquationSeparatei, (GLuint, GLenum, GLenum))                                      \
    X(void, ColorMaski, (GLuint, GLboolean, GLboolean, GLboolean, GLboolean))                      \
    X(void, DepthFunc, (GLenum))                                                                   \
    X(void, DepthMask, (GLboolean))                                                                \
    X(void, DepthRangef, (GLfloat, GLfloat))                                                       \
    X(void, StencilFuncSeparate, (GLenum, GLenum, GLint, GLuint))                                  \
    X(void, StencilOpSeparate, (GLenum, GLenum, GLenum, GLenum))                                   \
    X(void, StencilMaskSeparate, (GLenum, GLuint))                                                 \
    X(void, PolygonOffset, (GLfloat, GLfloat))                                                     \
    X(void, Scissor, (GLint, GLint, GLsizei, GLsizei))                                             \
    X(void, ClearBufferfv, (GLenum, GLint, const GLfloat*))                                        \
    X(void, ClearBufferiv, (GLenum, GLint, const GLint*))                                          \
    X(void, ClearBufferuiv, (GLenum, GLint, const GLuint*))                                        \
    X(void, ClearBufferfi, (GLenum, GLint, GLfloat, GLint))                                        \
    X(void, DrawBuffers, (GLsizei, const GLenum*))                                                 \
    X(void, DispatchCompute, (GLuint, GLuint, GLuint))                                             \
    X(void, DispatchComputeIndirect, (GLintptr))                                                   \
    X(void, MemoryBarrier, (GLbitfield))                                                           \
    X(void, BindImageTexture, (GLuint, GLuint, GLint, GLboolean, GLint, GLenum, GLenum))           \
    X(void, DrawArraysIndirect, (GLenum, const void*))                                             \
    X(void, DrawElementsIndirect, (GLenum, GLenum, const void*))                                   \
    X(void, CopyBufferSubData, (GLenum, GLenum, GLintptr, GLintptr, GLsizeiptr))                   \
    X(void, CopyImageSubData,                                                                      \
      (GLuint, GLenum, GLint, GLint, GLint, GLint, GLuint, GLenum, GLint, GLint, GLint, GLint,     \
       GLsizei, GLsizei, GLsizei))                                                                 \
    X(GLenum, CheckFramebufferStatus, (GLenum))                                                    \
    X(void, BlitFramebuffer,                                                                       \
      (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum))                \
    X(void, ReadBuffer, (GLenum))                                                                  \
    X(GLsync, FenceSync, (GLenum, GLbitfield))                                                     \
    X(GLenum, ClientWaitSync, (GLsync, GLbitfield, GLuint64))                                      \
    X(void, DeleteSync, (GLsync))                                                                  \
    X(void, GenQueries, (GLsizei, GLuint*))                                                        \
    X(void, DeleteQueries, (GLsizei, const GLuint*))                                               \
    X(void, GetQueryiv, (GLenum, GLenum, GLint*))                                                  \
    X(void, GetQueryObjectuiv, (GLuint, GLenum, GLuint*))                                          \
    X(void, DebugMessageCallback, (GLDEBUGPROC, const void*))                                      \
    X(void, PushDebugGroup, (GLenum, GLuint, GLsizei, const GLchar*))                              \
    X(void, PopDebugGroup, ())

// ES 3.2 core 함수 표다. 실행 시 플랫폼 층의 주소 조회 함수로 채운다(LoadCoreFunctions).
struct GlesFunctions {
#define ROCKS_GLES_DECLARE_FUNCTION(ReturnType, Name, Parameters)                                  \
    ReturnType(*Name) Parameters = nullptr;
    ROCKS_GLES_CORE_FUNCTIONS(ROCKS_GLES_DECLARE_FUNCTION)
#undef ROCKS_GLES_DECLARE_FUNCTION
};

// timestamp query 함수다. ES는 GL_EXT_disjoint_timer_query(EXT 접미사), 데스크톱 GL core context는
// GL 3.3 core 이름으로 얻는다(GlesTimerQueryNames). 둘 다 없으면 null이고 timestamp는 "없음"이다.
struct GlesTimerQueryFunctions {
    void (*QueryCounter)(GLuint, GLenum) = nullptr;
    void (*GetQueryObjectui64v)(GLuint, GLenum, GLuint64*) = nullptr;
    // GL_TIMESTAMP의 counter bit 조회다. ES 3.2 core glGetQueryiv는 GL_TIMESTAMP 대상을 정의하지
    // 않으므로 ES는 확장의 glGetQueryivEXT를 쓴다.
    void (*GetQueryiv)(GLenum, GLenum, GLint*) = nullptr;
};

// 이름으로 GL 함수 주소를 얻는 플랫폼 함수다(wglGetProcAddress·eglGetProcAddress를 감싼다). 없으면
// nullptr이다.
using GlesProcLoader = void* (*)(const char* name, void* context) noexcept;

// core 함수 표를 채운다. 모두 얻으면 true다. 하나라도 없으면 false이고 *missing에 그 함수 이름
// ("glTexStorage2D" 등, 정적 문자열)을 둔다.
bool LoadCoreFunctions(GlesFunctions& gl, GlesProcLoader loader, void* context,
                       const char** missing) noexcept;

// timestamp query 함수 이름 쌍이다.
struct GlesTimerQueryNames {
    const char* queryCounter = nullptr;
    const char* getQueryObjectui64v = nullptr;
    const char* getQueryiv = nullptr;
};

// GL_EXT_disjoint_timer_query(ES)의 이름이다.
inline constexpr GlesTimerQueryNames EsTimerQueryNames = {
    "glQueryCounterEXT", "glGetQueryObjectui64vEXT", "glGetQueryivEXT"};

// 데스크톱 GL 3.3 core(ARB_timer_query)의 이름이다. Windows 미리보기의 데스크톱 core context만
// 쓴다.
inline constexpr GlesTimerQueryNames DesktopTimerQueryNames = {
    "glQueryCounter", "glGetQueryObjectui64v", "glGetQueryiv"};

// timestamp query 함수를 names로 얻는다. 모두 얻으면 true다.
bool LoadTimerQueryFunctions(GlesTimerQueryFunctions& out, const GlesTimerQueryNames& names,
                             GlesProcLoader loader, void* context) noexcept;

// 지금 context가 name 확장(GL_EXT_... 온전한 이름)을 공개하는지다(glGetStringi로 찾는다).
bool HasGlExtension(const GlesFunctions& gl, const char* name) noexcept;

// 공백으로 나뉜 확장 목록 문자열(WGL·EGL 확장 문자열)에 name이 온전한 토큰으로 있는지다. list가
// nullptr이면 false다.
bool HasExtensionToken(const char* list, const char* name) noexcept;

// GL 오류 코드의 이름이다("GL_INVALID_ENUM" 등). 모르는 값은 "GL error"다.
const char* GlErrorName(GLenum error) noexcept;

} // namespace rocks::graphics::gles
