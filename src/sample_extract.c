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
#define mkdir_p(path) mkdir(path, 0755)
#define PATH_SEP '/'
#define PATH_SEP_STR "/"
#endif

/* ---- Instrument table ---- */

const InstrumentEntry SE_INSTRUMENTS[] = {
    {10132, 4340972,  304616,  304600,  "midi_instrument_00_acousticguitar.bnk"},
    {10132, 4645588,  242544,  242534,  "midi_instrument_01_flute.bnk"},
    {10132, 4888132,   92656,   92643,  "midi_instrument_02_recorder.bnk"},
    {10132, 4980788,   73144,   73129,  "midi_instrument_03_snaredrum.bnk"},
    {10132, 5053932,  130760,  130746,  "midi_instrument_04_handdrum.bnk"},
    {10132, 5184692,  166824,  166809,  "midi_instrument_05_piatticymbals.bnk"},
    {10132, 5351516,  108792,  108780,  "midi_instrument_06_harp.bnk"},
    {10132, 5460308,  199264,  199249,  "midi_instrument_07_piano.bnk"},
    {10132, 5659572,  188912,  188900,  "midi_instrument_08_violin.bnk"},
    {10132, 5848484,   14512,   14497,  "midi_instrument_09_pandrum.bnk"},
    {10132, 5862996, 4006304, 4006290,  "midi_instrument_10_proguitar.bnk"},
    {10133,     276, 1783752, 1783743,  "midi_instrument_11_proflute.bnk"},
    {10133, 1784028, 1695904, 1695890,  "midi_instrument_13_prodrumset.bnk"},
    {10133, 3479932, 2672776, 2672762,  "midi_instrument_14_probasselectric.bnk"},
    {10133, 6152708, 3092392, 3092378,  "midi_instrument_15_probasscontra.bnk"},
    {10134,     204, 3248320, 3248304,  "midi_instrument_16_proharp.bnk"},
    {10134, 3248524, 3897624, 3897611,  "midi_instrument_17_propiano.bnk"},
    {10134, 7146148, 2940288, 2940279,  "midi_instrument_18_proviolin.bnk"},
    {10135,     364, 1677504, 1677494,  "midi_instrument_19_propandrum.bnk"},
    {10135, 1677868, 1911312, 1911300,  "midi_instrument_24_proguitarelectricclean.bnk"},
    {10135, 3589180, 1942552, 1942541,  "midi_instrument_25_proguitarelectricdrive.bnk"},
    {10135, 5531732, 2146136, 2146127,  "midi_instrument_26_proguitarelectricdist.bnk"},
    {10135, 7677868, 3494904, 3494894,  "midi_instrument_27_proclarinet.bnk"},
    {10136,     772, 3699384, 3699369,  "midi_instrument_28_prohorn.bnk"},
    {10136, 3700156,   75896,   95517,  "midi_instrument_synth_saw_basic.bnk"},
    {10136, 3776052,  702416,  702404,  "midi_instrument_synth_saw_stereo.bnk"},
    {10136, 4478468,  829656,  829643,  "midi_instrument_synth_saw_super.bnk"},
    {10136, 5308124,  954432,  954421,  "midi_instrument_synth_saw_superoct.bnk"},
    {10136, 6262556,  117336,  141011,  "midi_instrument_synth_sine_basic.bnk"},
    {10136, 6379892,  281320,  312957,  "midi_instrument_synth_sine_stereo.bnk"},
    {10136, 6661212,  335792,  335780,  "midi_instrument_synth_sine_super.bnk"},
    {10136, 6997004,  354920,  354909,  "midi_instrument_synth_sine_superoct.bnk"},
    {10136, 7351924,   49136,   67928,  "midi_instrument_synth_square_basic.bnk"},
    {10136, 7401060,  834784,  834775,  "midi_instrument_synth_square_stereo.bnk"},
    {10136, 8235844,  689600,  689585,  "midi_instrument_synth_square_super.bnk"},
    {10137,     812,  889576,  889560,  "midi_instrument_synth_square_superoct.bnk"},
    {10137,  890388,  354472,  426766,  "midi_instrument_synth_triangle_basic.bnk"},
    {10137, 1244860,  502944,  541983,  "midi_instrument_synth_triangle_stereo.bnk"},
    {10137, 1747804,  562016,  562007,  "midi_instrument_synth_triangle_super.bnk"},
    {10137, 2309820,  621416,  621404,  "midi_instrument_synth_triangle_superoct.bnk"},
};

const int SE_NUM_INSTRUMENTS = sizeof(SE_INSTRUMENTS) / sizeof(SE_INSTRUMENTS[0]);

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

    int total_samples = 0;

    for (int i = 0; i < SE_NUM_INSTRUMENTS; i++) {
        const InstrumentEntry *inst = &SE_INSTRUMENTS[i];

        /* Derive instrument name from filename (strip .bnk) */
        char inst_name[256];
        snprintf(inst_name, sizeof(inst_name), "%s", inst->filename);
        char *dot = strrchr(inst_name, '.');
        if (dot) *dot = '\0';

        /* Report progress */
        if (progress_cb)
            progress_cb(i + 1, SE_NUM_INSTRUMENTS, inst_name, ctx);

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

    return total_samples;
}
