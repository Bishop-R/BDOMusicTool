/*
 * wem2ogg.h - Convert Wwise RIFF Vorbis (WEM) to standard OGG Vorbis
 *
 * Ported from ww2ogg by hcs (https://github.com/hcs64/ww2ogg)
 * Single-file C implementation for in-memory conversion.
 */
#ifndef WEM2OGG_H
#define WEM2OGG_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Convert a WEM file (in memory) to OGG Vorbis (in memory).
 * Returns 0 on success, non-zero on error.
 * Caller must free(*ogg_out) on success.
 */
int wem_to_ogg(const uint8_t *wem_data, size_t wem_size,
               uint8_t **ogg_out, size_t *ogg_size);

#ifdef __cplusplus
}
#endif

#endif /* WEM2OGG_H */
