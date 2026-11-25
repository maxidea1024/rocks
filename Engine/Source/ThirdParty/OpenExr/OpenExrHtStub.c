/* Rocks glue for OpenEXRCore 3.4.3 (Docs/Design/AssetSystem section 12.10). HTJ2K compression is
   deferred: upstream implements it in internal_ht.cpp on top of OpenJPH, and both use C++
   exceptions, which the Rocks build disables. Those two files are not compiled. These definitions
   keep the compression table linkable and report the feature as not implemented. ImageCodec rejects
   HTJ2K parts from the header before decoding, so the decoder never reaches them; the encoder path
   fails the same way. */
#include "internal_compress.h"
#include "internal_decompress.h"

exr_result_t internal_exr_apply_ht(exr_encode_pipeline_t* encode) {
    (void)encode;
    return EXR_ERR_FEATURE_NOT_IMPLEMENTED;
}

exr_result_t internal_exr_undo_ht(exr_decode_pipeline_t* decode, const void* compressed_data,
                                  uint64_t comp_buf_size, void* uncompressed_data,
                                  uint64_t uncompressed_size) {
    (void)decode;
    (void)compressed_data;
    (void)comp_buf_size;
    (void)uncompressed_data;
    (void)uncompressed_size;
    return EXR_ERR_FEATURE_NOT_IMPLEMENTED;
}
