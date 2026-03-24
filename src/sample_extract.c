/*
 * sample_extract.c - Shared BDO instrument sample extraction
 *
 * Pipeline: PAZ -> ICE decrypt -> BDO decompress -> BNK parse -> WEM -> OGG
 *
 * Cross-platform: Linux (gcc) and Windows (mingw).
 */

#ifndef _WIN32
#define _FILE_OFFSET_BITS 64
#endif

#include "sample_extract.h"
#include "ice.h"
#include "wem2ogg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <errno.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define mkdir_p(path) _mkdir(path)
#define PATH_SEP '\\'
#define PATH_SEP_STR "\\"
#else
#include <unistd.h>
#include <sys/types.h>
#include <dirent.h>
#define mkdir_p(path) mkdir(path, 0755)
#define PATH_SEP '/'
#define PATH_SEP_STR "/"
#endif

/* ---- Target BNK filenames (searched for in PAZ indices at runtime) ---- */

static const char *SE_TARGET_BNKS[] = {
    "midi_instrument_00_acousticguitar.bnk",
    "midi_instrument_01_flute.bnk",
    "midi_instrument_02_recorder.bnk",
    "midi_instrument_03_snaredrum.bnk",
    "midi_instrument_04_handdrum.bnk",
    "midi_instrument_05_piatticymbals.bnk",
    "midi_instrument_06_harp.bnk",
    "midi_instrument_07_piano.bnk",
    "midi_instrument_08_violin.bnk",
    "midi_instrument_09_pandrum.bnk",
    "midi_instrument_10_proguitar.bnk",
    "midi_instrument_11_proflute.bnk",
    "midi_instrument_13_prodrumset.bnk",
    "midi_instrument_14_probasselectric.bnk",
    "midi_instrument_15_probasscontra.bnk",
    "midi_instrument_16_proharp.bnk",
    "midi_instrument_17_propiano.bnk",
    "midi_instrument_18_proviolin.bnk",
    "midi_instrument_19_propandrum.bnk",
    "midi_instrument_24_proguitarelectricclean.bnk",
    "midi_instrument_25_proguitarelectricdrive.bnk",
    "midi_instrument_26_proguitarelectricdist.bnk",
    "midi_instrument_27_proclarinet.bnk",
    "midi_instrument_28_prohorn.bnk",
    "midi_instrument_synth_saw_basic.bnk",
    "midi_instrument_synth_saw_stereo.bnk",
    "midi_instrument_synth_saw_super.bnk",
    "midi_instrument_synth_saw_superoct.bnk",
    "midi_instrument_synth_sine_basic.bnk",
    "midi_instrument_synth_sine_stereo.bnk",
    "midi_instrument_synth_sine_super.bnk",
    "midi_instrument_synth_sine_superoct.bnk",
    "midi_instrument_synth_square_basic.bnk",
    "midi_instrument_synth_square_stereo.bnk",
    "midi_instrument_synth_square_super.bnk",
    "midi_instrument_synth_square_superoct.bnk",
    "midi_instrument_synth_triangle_basic.bnk",
    "midi_instrument_synth_triangle_stereo.bnk",
    "midi_instrument_synth_triangle_super.bnk",
    "midi_instrument_synth_triangle_superoct.bnk",
};
static const int SE_NUM_TARGET_BNKS = sizeof(SE_TARGET_BNKS) / sizeof(SE_TARGET_BNKS[0]);

/* ---- Platform helpers ---- */

int se_make_dirs(const char *path)
{
    char tmp[4096];
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) return -1;
    memcpy(tmp, path, len + 1);

    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            tmp[i] = '\0';
            mkdir_p(tmp);
            tmp[i] = PATH_SEP;
        }
    }
    mkdir_p(tmp);
    return 0;
}

int se_dir_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & S_IFDIR);
}

/* ---- PAZ index scanning ---- */

/* Get the Nth null-terminated string from a buffer of consecutive strings. */
static const char *paz_get_nth_string(const uint8_t *buf, size_t buf_len,
                                       uint32_t index)
{
    uint32_t current = 0;
    size_t pos = 0;
    while (pos < buf_len) {
        if (current == index)
            return (const char *)(buf + pos);
        while (pos < buf_len && buf[pos] != '\0')
            pos++;
        pos++; /* skip null terminator */
        current++;
    }
    return NULL;
}

/*
 * Scan a single PAZ file's index for target BNK filenames.
 * Appends matches to entries/count (reallocating as needed).
 * Returns number of matches found in this PAZ, or -1 on error.
 */
static int paz_scan_for_instruments(const char *paz_path, int paz_num,
                                     const char **targets, int num_targets,
                                     InstrumentEntry **entries, int *count,
                                     int *capacity)
{
    FILE *f = fopen(paz_path, "rb");
    if (!f) return -1;

    /* Read 12-byte PAZ header */
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12) {
        fclose(f);
        return -1;
    }
    /* uint32_t paz_hash = LE32(hdr+0); -- unused */
    uint32_t files_count  = (uint32_t)hdr[4]  | ((uint32_t)hdr[5]  << 8)
                          | ((uint32_t)hdr[6]  << 16) | ((uint32_t)hdr[7]  << 24);
    uint32_t names_length = (uint32_t)hdr[8]  | ((uint32_t)hdr[9]  << 8)
                          | ((uint32_t)hdr[10] << 16) | ((uint32_t)hdr[11] << 24);

    if (files_count == 0 || files_count > 100000) {
        fclose(f);
        return 0;
    }

    /* Read file entry table (24 bytes per entry) */
    size_t entries_size = (size_t)files_count * 24;
    uint8_t *raw_entries = (uint8_t *)malloc(entries_size);
    if (!raw_entries) { fclose(f); return -1; }
    if (fread(raw_entries, 1, entries_size, f) != entries_size) {
        free(raw_entries);
        fclose(f);
        return -1;
    }

    /* Read and decrypt names block */
    uint8_t *names_buf = NULL;
    size_t names_buf_len = 0;
    if (names_length > 0 && names_length < 10 * 1024 * 1024) {
        names_buf = (uint8_t *)malloc(names_length);
        if (!names_buf) { free(raw_entries); fclose(f); return -1; }
        if (fread(names_buf, 1, names_length, f) != names_length) {
            free(names_buf);
            free(raw_entries);
            fclose(f);
            return -1;
        }
        ice_decrypt(names_buf, names_length);
        names_buf_len = names_length;
    }
    fclose(f);

    int found = 0;

    for (uint32_t i = 0; i < files_count; i++) {
        const uint8_t *e = raw_entries + (size_t)i * 24;
        /* uint32_t file_hash  = LE32(e+0); -- unused */
        uint32_t folder_num    = (uint32_t)e[4]  | ((uint32_t)e[5]  << 8)
                               | ((uint32_t)e[6]  << 16) | ((uint32_t)e[7]  << 24);
        uint32_t file_num      = (uint32_t)e[8]  | ((uint32_t)e[9]  << 8)
                               | ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
        uint32_t offset        = (uint32_t)e[12] | ((uint32_t)e[13] << 8)
                               | ((uint32_t)e[14] << 16) | ((uint32_t)e[15] << 24);
        uint32_t comp_size     = (uint32_t)e[16] | ((uint32_t)e[17] << 8)
                               | ((uint32_t)e[18] << 16) | ((uint32_t)e[19] << 24);
        uint32_t orig_size     = (uint32_t)e[20] | ((uint32_t)e[21] << 8)
                               | ((uint32_t)e[22] << 16) | ((uint32_t)e[23] << 24);

        (void)folder_num; /* we only match on filename, not folder path */

        const char *fname = paz_get_nth_string(names_buf, names_buf_len, file_num);
        if (!fname) continue;

        /* Check against target list */
        for (int t = 0; t < num_targets; t++) {
            if (strcmp(fname, targets[t]) == 0) {
                /* Grow array if needed */
                if (*count >= *capacity) {
                    int new_cap = *capacity ? *capacity * 2 : 64;
                    InstrumentEntry *tmp = (InstrumentEntry *)realloc(
                        *entries, (size_t)new_cap * sizeof(InstrumentEntry));
                    if (!tmp) goto cleanup;
                    *entries = tmp;
                    *capacity = new_cap;
                }
                InstrumentEntry *inst = &(*entries)[*count];
                inst->paz_num   = paz_num;
                inst->offset    = (int64_t)offset;
                inst->comp_size = (int64_t)comp_size;
                inst->orig_size = (int64_t)orig_size;
                inst->filename  = targets[t]; /* points to static string */
                (*count)++;
                found++;
                break;
            }
        }
    }

cleanup:
    free(names_buf);
    free(raw_entries);
    return found;
}

/*
 * Check if a directory contains any PAD*.PAZ files.
 * Returns 1 if found, 0 otherwise.
 */
int se_has_paz_files(const char *dir)
{
#ifdef _WIN32
    char pattern[4096];
    snprintf(pattern, sizeof(pattern), "%s\\PAD*.PAZ", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    FindClose(h);
    return 1;
#else
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, "PAD", 3) == 0 &&
            strstr(ent->d_name, ".PAZ") != NULL) {
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    return 0;
#endif
}

/*
 * Build a dynamic instrument index by scanning PAZ files in paz_dir.
 * Returns 0 on success, -1 on error.
 * Caller must free(*entries_out) when done.
 */
static int se_build_instrument_index(const char *paz_dir,
                                      InstrumentEntry **entries_out,
                                      int *count_out,
                                      SampleExtractProgressFn progress_cb,
                                      void *ctx)
{
    *entries_out = NULL;
    *count_out = 0;
    int capacity = 0;

    ice_init();

    /* Scan PAZ files looking for target BNKs.
     * Sound BNKs are known to live in PAZ 10100-10200 range.
     * Try that first; only do a full directory scan as fallback. */
    for (int pass = 0; pass < 2 && *count_out < SE_NUM_TARGET_BNKS; pass++) {
        if (pass == 0) {
            /* Fast pass: scan only the sound PAZ range */
            for (int paz_num = 10100; paz_num <= 10200; paz_num++) {
                char paz_path[4096];
#ifdef _WIN32
                snprintf(paz_path, sizeof(paz_path), "%s\\PAD%05d.PAZ",
                         paz_dir, paz_num);
#else
                snprintf(paz_path, sizeof(paz_path), "%s/PAD%05d.PAZ",
                         paz_dir, paz_num);
#endif
                struct stat st;
                if (stat(paz_path, &st) != 0) continue;

                paz_scan_for_instruments(paz_path, paz_num,
                                          SE_TARGET_BNKS, SE_NUM_TARGET_BNKS,
                                          entries_out, count_out, &capacity);
                if (*count_out >= SE_NUM_TARGET_BNKS) break;
            }
        } else {
            /* Slow fallback: scan all PAZ files in the directory */
            if (progress_cb)
                progress_cb(0, 0, "Scanning all archives (fallback)...", ctx);
#ifdef _WIN32
            char pattern[4096];
            snprintf(pattern, sizeof(pattern), "%s\\PAD*.PAZ", paz_dir);
            WIN32_FIND_DATAA fd;
            HANDLE h = FindFirstFileA(pattern, &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                int paz_num = 0;
                if (sscanf(fd.cFileName, "PAD%d", &paz_num) != 1) continue;
                /* Skip range already scanned */
                if (paz_num >= 10100 && paz_num <= 10200) continue;

                if (progress_cb) {
                    char scan_msg[128];
                    snprintf(scan_msg, sizeof(scan_msg),
                             "Scanning %s", fd.cFileName);
                    progress_cb(0, 0, scan_msg, ctx);
                }

                char paz_path[4096];
                snprintf(paz_path, sizeof(paz_path),
                         "%s\\%s", paz_dir, fd.cFileName);
                paz_scan_for_instruments(paz_path, paz_num,
                                          SE_TARGET_BNKS, SE_NUM_TARGET_BNKS,
                                          entries_out, count_out, &capacity);
                if (*count_out >= SE_NUM_TARGET_BNKS) break;
            } while (FindNextFileA(h, &fd));
            FindClose(h);
#else
            DIR *d = opendir(paz_dir);
            if (!d) continue;
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                int paz_num = 0;
                if (sscanf(ent->d_name, "PAD%d", &paz_num) != 1) continue;
                if (!strstr(ent->d_name, ".PAZ")) continue;
                if (paz_num >= 10100 && paz_num <= 10200) continue;

                if (progress_cb) {
                    char scan_msg[128];
                    snprintf(scan_msg, sizeof(scan_msg),
                             "Scanning %s", ent->d_name);
                    progress_cb(0, 0, scan_msg, ctx);
                }

                char paz_path[4096];
                snprintf(paz_path, sizeof(paz_path),
                         "%s/%s", paz_dir, ent->d_name);
                paz_scan_for_instruments(paz_path, paz_num,
                                          SE_TARGET_BNKS, SE_NUM_TARGET_BNKS,
                                          entries_out, count_out, &capacity);
                if (*count_out >= SE_NUM_TARGET_BNKS) break;
            }
            closedir(d);
#endif
        }
    }

    if (*count_out == 0) {
        fprintf(stderr, "Error: no instrument BNK files found in PAZ archives\n");
        free(*entries_out);
        *entries_out = NULL;
        return -1;
    }

    if (*count_out < SE_NUM_TARGET_BNKS) {
        fprintf(stderr, "Warning: found %d/%d instruments in PAZ archives\n",
                *count_out, SE_NUM_TARGET_BNKS);
    }

    return 0;
}

/* ---- BDO decompression ---- */

static uint8_t *bdo_decompress_core(const uint8_t *inp, size_t inp_len,
                                     int64_t out_len, size_t *actual_out)
{
    if (out_len <= 0) {
        *actual_out = 0;
        return calloc(1, 1);
    }

    uint8_t *out = (uint8_t *)calloc((size_t)out_len, 1);
    if (!out) return NULL;

    size_t ip = 0;
    size_t op = 0;
    size_t olen = (size_t)out_len;

    while (op < olen && ip < inp_len) {
        if (ip + 4 > inp_len) break;
        uint32_t group = (uint32_t)inp[ip]
                       | ((uint32_t)inp[ip+1] << 8)
                       | ((uint32_t)inp[ip+2] << 16)
                       | ((uint32_t)inp[ip+3] << 24);
        ip += 4;

        for (int bit = 0; bit < 31; bit++) {
            if (op >= olen || ip >= inp_len) break;

            if (group & (1u << bit)) {
                if (ip + 2 > inp_len) goto done;
                uint16_t ref = (uint16_t)inp[ip] | ((uint16_t)inp[ip+1] << 8);
                ip += 2;
                int length = (ref >> 10) + 3;
                int offset = ref & 0x3FF;
                if (offset == 0) goto done;

                size_t src = op - (size_t)offset;
                for (int j = 0; j < length; j++) {
                    if (op >= olen) break;
                    size_t si = src + (size_t)j;
                    out[op] = (si < olen) ? out[si] : 0;
                    op++;
                }
            } else {
                out[op] = inp[ip];
                op++;
                ip++;
            }
        }
    }

done:
    *actual_out = op;
    return out;
}

uint8_t *se_bdo_unwrap(const uint8_t *data, size_t data_len,
                        int64_t original_size, size_t *out_len)
{
    if (data_len < 3) {
        uint8_t *copy = (uint8_t *)malloc(data_len);
        if (copy) memcpy(copy, data, data_len);
        *out_len = data_len;
        return copy;
    }

    uint8_t b0 = data[0];
    int hdr_size;
    int64_t decomp_len;

    if (b0 & 0x02) {
        hdr_size = 9;
        if (data_len < 9) {
            *out_len = 0;
            return calloc(1, 1);
        }
        decomp_len = (int64_t)((uint32_t)data[5]
                              | ((uint32_t)data[6] << 8)
                              | ((uint32_t)data[7] << 16)
                              | ((uint32_t)data[8] << 24));
    } else {
        hdr_size = 3;
        decomp_len = (int64_t)data[2];
    }

    const uint8_t *payload = data + hdr_size;
    size_t payload_len = data_len - (size_t)hdr_size;
    int is_compressed = b0 & 0x01;

    if (is_compressed) {
        size_t actual;
        uint8_t *result = bdo_decompress_core(payload, payload_len, decomp_len, &actual);
        if (!result) {
            size_t sz = (payload_len < (size_t)original_size) ? payload_len : (size_t)original_size;
            result = (uint8_t *)malloc(sz);
            if (result) memcpy(result, payload, sz);
            *out_len = sz;
            return result;
        }
        *out_len = actual;
        return result;
    } else {
        size_t sz = (payload_len < (size_t)original_size) ? payload_len : (size_t)original_size;
        uint8_t *result = (uint8_t *)malloc(sz);
        if (result) memcpy(result, payload, sz);
        *out_len = sz;
        return result;
    }
}

/* ---- PAZ extraction ---- */

uint8_t *se_extract_from_paz(const char *paz_dir, int paz_num,
                              int64_t offset, int64_t comp_size,
                              int64_t orig_size, size_t *out_len)
{
    char paz_path[4096];
    snprintf(paz_path, sizeof(paz_path), "%s" PATH_SEP_STR "PAD%05d.PAZ",
             paz_dir, paz_num);

    FILE *f = fopen(paz_path, "rb");
    if (!f) {
        fprintf(stderr, "Error: cannot open %s: %s\n", paz_path, strerror(errno));
        *out_len = 0;
        return NULL;
    }

#ifdef _WIN32
    _fseeki64(f, offset, SEEK_SET);
#else
    fseeko(f, (off_t)offset, SEEK_SET);
#endif

    size_t raw_size = (size_t)comp_size;
    uint8_t *raw = (uint8_t *)malloc(raw_size);
    if (!raw) {
        fclose(f);
        fprintf(stderr, "Error: out of memory allocating %zu bytes\n", raw_size);
        *out_len = 0;
        return NULL;
    }

    size_t nread = fread(raw, 1, raw_size, f);
    fclose(f);

    if (nread != raw_size) {
        fprintf(stderr, "Error: read %zu/%zu bytes from %s\n", nread, raw_size, paz_path);
        free(raw);
        *out_len = 0;
        return NULL;
    }

    /* ICE decrypt (only if size is multiple of 8) */
    if (raw_size % 8 == 0) {
        ice_decrypt(raw, raw_size);
    }

    /* BDO unwrap */
    uint8_t *result = se_bdo_unwrap(raw, raw_size, orig_size, out_len);
    free(raw);
    return result;
}

/* ---- BNK parsing ---- */

typedef struct {
    uint32_t wem_id;
    uint32_t wem_offset;
    uint32_t wem_size;
} DidxEntry;

WemFile *se_extract_wems_from_bnk(const uint8_t *bnk, size_t bnk_len,
                                   int *num_wems)
{
    *num_wems = 0;

    DidxEntry *didx = NULL;
    int didx_count = 0;
    size_t data_offset = 0;
    int found_data = 0;

    size_t off = 0;
    while (off + 8 <= bnk_len) {
        uint32_t size = (uint32_t)bnk[off+4]
                      | ((uint32_t)bnk[off+5] << 8)
                      | ((uint32_t)bnk[off+6] << 16)
                      | ((uint32_t)bnk[off+7] << 24);

        if (memcmp(bnk + off, "DIDX", 4) == 0) {
            didx_count = (int)(size / 12);
            didx = (DidxEntry *)calloc((size_t)didx_count, sizeof(DidxEntry));
            if (!didx) return NULL;

            for (int i = 0; i < didx_count; i++) {
                size_t base = off + 8 + (size_t)i * 12;
                if (base + 12 > bnk_len) break;
                didx[i].wem_id     = (uint32_t)bnk[base]
                                   | ((uint32_t)bnk[base+1] << 8)
                                   | ((uint32_t)bnk[base+2] << 16)
                                   | ((uint32_t)bnk[base+3] << 24);
                didx[i].wem_offset = (uint32_t)bnk[base+4]
                                   | ((uint32_t)bnk[base+5] << 8)
                                   | ((uint32_t)bnk[base+6] << 16)
                                   | ((uint32_t)bnk[base+7] << 24);
                didx[i].wem_size   = (uint32_t)bnk[base+8]
                                   | ((uint32_t)bnk[base+9] << 8)
                                   | ((uint32_t)bnk[base+10] << 16)
                                   | ((uint32_t)bnk[base+11] << 24);
            }
        } else if (memcmp(bnk + off, "DATA", 4) == 0) {
            data_offset = off + 8;
            found_data = 1;
        }

        off += 8 + (size_t)size;
    }

    if (!found_data || !didx || didx_count == 0) {
        free(didx);
        return NULL;
    }

    WemFile *wems = (WemFile *)calloc((size_t)didx_count, sizeof(WemFile));
    if (!wems) {
        free(didx);
        return NULL;
    }

    int count = 0;
    for (int i = 0; i < didx_count; i++) {
        size_t start = data_offset + (size_t)didx[i].wem_offset;
        size_t wsize = (size_t)didx[i].wem_size;
        if (start + wsize > bnk_len) continue;

        wems[count].wem_id = didx[i].wem_id;
        wems[count].data = (uint8_t *)malloc(wsize);
        if (!wems[count].data) continue;
        memcpy(wems[count].data, bnk + start, wsize);
        wems[count].size = wsize;
        count++;
    }

    free(didx);
    *num_wems = count;
    return wems;
}

/* ---- Find BDO installation ---- */

const char *se_find_paz_dir(void)
{
    static char paz_path[4096];

#ifdef _WIN32
    const char *candidates[] = {
        "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Black Desert Online\\Paz",
        "C:\\Program Files\\Steam\\steamapps\\common\\Black Desert Online\\Paz",
        "C:\\PearlAbyss\\BlackDesert\\Paz",
        "C:\\Pearl Abyss\\BlackDesert\\Paz",
        "C:\\PearlAbyss\\Paz",
        "C:\\Pearl Abyss\\Paz",
        "C:\\Program Files (x86)\\PearlAbyss\\BlackDesert\\Paz",
        "C:\\Program Files\\PearlAbyss\\BlackDesert\\Paz",
        "C:\\Program Files (x86)\\BlackDesert\\Paz",
        "C:\\Program Files\\BlackDesert\\Paz",
        NULL
    };
#else
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";

    static char path1[4096], path2[4096];
    snprintf(path1, sizeof(path1), "%s/.local/share/Steam/steamapps/common/Black Desert Online/Paz", home);
    snprintf(path2, sizeof(path2), "%s/.steam/steam/steamapps/common/Black Desert Online/Paz", home);

    const char *candidates[] = {
        path1,
        path2,
        NULL
    };
#endif

    for (int i = 0; candidates[i]; i++) {
        if (se_dir_exists(candidates[i])) {
            snprintf(paz_path, sizeof(paz_path), "%s", candidates[i]);
            return paz_path;
        }
    }
    return NULL;
}

/* ---- High-level extraction ---- */

int extract_all_samples(const char *paz_dir, const char *out_dir,
                        SampleExtractProgressFn progress_cb, void *ctx)
{
    ice_init();
    se_make_dirs(out_dir);

    /* Build instrument index dynamically from PAZ file indices */
    InstrumentEntry *instruments = NULL;
    int num_instruments = 0;
    if (se_build_instrument_index(paz_dir, &instruments, &num_instruments,
                                   progress_cb, ctx) != 0) {
        return -1;
    }

    int total_samples = 0;

    for (int i = 0; i < num_instruments; i++) {
        const InstrumentEntry *inst = &instruments[i];

        /* Derive instrument name from filename (strip .bnk) */
        char inst_name[256];
        snprintf(inst_name, sizeof(inst_name), "%s", inst->filename);
        char *dot = strrchr(inst_name, '.');
        if (dot) *dot = '\0';

        /* Report progress */
        if (progress_cb)
            progress_cb(i + 1, num_instruments, inst_name, ctx);

        /* Create output directory */
        char inst_dir[4096];
        snprintf(inst_dir, sizeof(inst_dir), "%s" PATH_SEP_STR "%s",
                 out_dir, inst_name);
        se_make_dirs(inst_dir);

        /* Extract BNK from PAZ */
        size_t bnk_len = 0;
        uint8_t *bnk_data = se_extract_from_paz(paz_dir, inst->paz_num,
                                                  inst->offset, inst->comp_size,
                                                  inst->orig_size, &bnk_len);
        if (!bnk_data || bnk_len == 0) {
            free(bnk_data);
            continue;
        }

        /* Parse BNK and extract WEM files */
        int num_wems = 0;
        WemFile *wems = se_extract_wems_from_bnk(bnk_data, bnk_len, &num_wems);
        free(bnk_data);

        if (!wems || num_wems == 0) {
            free(wems);
            continue;
        }

        /* Convert each WEM to OGG (or save as WAV if PCM) */
        for (int w = 0; w < num_wems; w++) {
            uint8_t *ogg_data = NULL;
            size_t ogg_len = 0;
            int rc = wem_to_ogg(wems[w].data, wems[w].size, &ogg_data, &ogg_len);

            if (rc == 0 && ogg_data) {
                char ogg_path[4096];
                snprintf(ogg_path, sizeof(ogg_path), "%s" PATH_SEP_STR "%u.ogg",
                         inst_dir, wems[w].wem_id);
                FILE *fout = fopen(ogg_path, "wb");
                if (fout) {
                    fwrite(ogg_data, 1, ogg_len, fout);
                    fclose(fout);
                    total_samples++;
                }
                free(ogg_data);
            } else if (rc == 1) {
                char wav_path[4096];
                snprintf(wav_path, sizeof(wav_path), "%s" PATH_SEP_STR "%u.wav",
                         inst_dir, wems[w].wem_id);
                FILE *fout = fopen(wav_path, "wb");
                if (fout) {
                    fwrite(wems[w].data, 1, wems[w].size, fout);
                    fclose(fout);
                    total_samples++;
                }
            }

            free(wems[w].data);
        }
        free(wems);
    }

    free(instruments);
    return total_samples;
}
