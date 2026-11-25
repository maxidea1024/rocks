// Rocks glue that replaces FreeType's system layer src/base/ftsystem.c (an upstream configuration
// point: FreeType documents ftsystem.c as the file to replace; Docs/Design/Font/08-ThirdParty.ko.md
// section 8.2). The engine opens faces only from memory or with FT_Open_Face and a caller stream,
// so there is no CRT file IO: FT_Stream_Open fails with Cannot_Open_Resource. The font backend
// creates every library with FT_New_Library and an Allocator-backed FT_Memory, so FT_Init_FreeType
// (the only FT_New_Memory caller) gets no memory object and fails instead of using the CRT heap.
#include <freetype/freetype.h>
#include <freetype/ftsystem.h>
#include <freetype/internal/ftdebug.h>
#include <freetype/internal/ftstream.h>

FT_BASE_DEF(FT_Error)
FT_Stream_Open(FT_Stream stream, const char* filepathname) {
    FT_UNUSED(filepathname);
    if (stream != NULL) {
        stream->descriptor.pointer = NULL;
        stream->pathname.pointer = NULL;
        stream->base = NULL;
        stream->size = 0;
        stream->pos = 0;
        stream->read = NULL;
        stream->close = NULL;
    }

    return FT_THROW(Cannot_Open_Resource);
}

FT_BASE_DEF(FT_Memory)
FT_New_Memory(void) {
    return NULL;
}

FT_BASE_DEF(void)
FT_Done_Memory(FT_Memory memory) {
    FT_UNUSED(memory);
}
