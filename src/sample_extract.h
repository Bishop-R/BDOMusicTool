/*
 * sample_extract.h - Shared BDO instrument sample extraction
 *
 * Used by both the standalone get_samples CLI tool and the Muse app
 * for integrated extraction on first launch.
 *
 * Pipeline: PAZ -> ICE decrypt -> BDO decompress -> BNK parse -> WEM -> OGG
 */
#ifndef SAMPLE_EXTRACT_H
#define SAMPLE_EXTRACT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Instrument table entry ---- */

typedef struct {
    int     paz_num;
    int64_t offset;
    int64_t comp_size;
    int64_t orig_size;
    const char *filename;
} InstrumentEntry;

/* The full instrument table and its count */
extern const InstrumentEntry SE_INSTRUMENTS[];
extern const int SE_NUM_INSTRUMENTS;

/* ---- Extracted WEM file ---- */

typedef struct {
    uint32_t wem_id;
    uint8_t *data;
    size_t   size;
} WemFile;

/* ---- Core extraction functions ---- */

/* Create directory tree (like mkdir -p). Returns 0 on success. */
int se_make_dirs(const char *path);

/* Check if a directory exists. Returns non-zero if it does. */
int se_dir_exists(const char *path);

/* Extract raw data from a PAZ archive, ICE-decrypt and BDO-decompress.
 * Caller must free() the returned buffer. */
uint8_t *se_extract_from_paz(const char *paz_dir, int paz_num,
                              int64_t offset, int64_t comp_size,
                              int64_t orig_size, size_t *out_len);

/* BDO unwrap: strip BDO header and decompress if needed.
 * Caller must free() the returned buffer. */
uint8_t *se_bdo_unwrap(const uint8_t *data, size_t data_len,
                        int64_t original_size, size_t *out_len);

/* Parse a BNK file and extract all WEM entries.
 * Returns array of WemFile (caller must free each .data and the array).
 * Sets *num_wems to the count. */
WemFile *se_extract_wems_from_bnk(const uint8_t *bnk, size_t bnk_len,
                                   int *num_wems);

/* ---- Progress callback ---- */

typedef void (*SampleExtractProgressFn)(int current, int total,
                                         const char *instrument_name,
                                         void *ctx);

/* ---- High-level extraction ---- */

/* Extract all instrument samples from PAZ archives into out_dir.
 * Calls ice_init() internally.
 * progress_cb may be NULL.
 * Returns number of samples extracted, or -1 on fatal error. */
int extract_all_samples(const char *paz_dir, const char *out_dir,
                        SampleExtractProgressFn progress_cb, void *ctx);

/* ---- BDO installation finder ---- */

/* Try common BDO install paths. Returns a static pointer to the Paz dir
 * if found, or NULL. */
const char *se_find_paz_dir(void);

#ifdef __cplusplus
}
#endif

#endif /* SAMPLE_EXTRACT_H */
