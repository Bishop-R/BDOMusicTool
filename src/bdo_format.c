#include "bdo_format.h"
#include "ice.h"
#include "instruments.h"
#include "platform.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* LE read/write helpers */
static uint16_t read_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t read_u32(const uint8_t *p) { return p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24); }
static double   read_f64(const uint8_t *p) { double v; memcpy(&v, p, 8); return v; }
static void write_u16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static void write_u32(uint8_t *p, uint32_t v) { p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF; }
static void write_f64(uint8_t *p, double v) { memcpy(p, &v, 8); }

/* parse the comma-separated instrument tag like "17,27" */
static int parse_inst_tag(const char *tag, uint8_t *ids, int max) {
    int count = 0;
    while (*tag && count < max) {
        char *end;
        long v = strtol(tag, &end, 10);
        if (end == tag) break;
        ids[count++] = (uint8_t)v;
        if (*end == ',') end++;
        tag = end;
    }
    return count;
}

int bdo_load(const char *path, const char *linked_name, MuseProject *out) {
    FILE *f = fopen_utf8(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize < 4 + 8) { fclose(f); return -1; }

    uint8_t *raw = malloc((size_t)fsize);
    fread(raw, 1, (size_t)fsize, f);
    fclose(f);

    /* v6 is the old format, v9 is current - both work */
    uint32_t ver = read_u32(raw);
    if (ver != BDO_VERSION && ver != 6) { free(raw); return -2; }

    /* decrypt the ICE-encrypted payload */
    size_t payload_len = (size_t)fsize - 4;
    uint8_t *pt = malloc(payload_len);
    int rc = ice_decrypt_full(raw + 4, payload_len, 0, pt, payload_len);
    free(raw);
    if (rc != 0) { free(pt); return -3; }

    if (payload_len < BDO_HEADER_SIZE) { free(pt); return -4; }

    /* header: owner_id(4B) pad(4B) name1(62B UTF16) name2(62B UTF16)
       bpm(2B @0x84) time_sig(2B @0x86) inst_tag(ASCII @0x88) */
    muse_project_free(out);
    muse_project_init(out);
    out->owner_id = read_u32(pt);

    /* pull the character name out - stored as UTF-16LE at offset 8 */
    int cn_i = 0;
    for (int i = 0; i < BDO_NAME_FIELD && i + 1 < (int)payload_len - 8; i += 2) {
        uint16_t ch = pt[8 + i] | (pt[8 + i + 1] << 8);
        if (ch == 0) break;
        if (ch < 128 && cn_i < (int)sizeof(out->char_name) - 1)
            out->char_name[cn_i++] = (char)ch;
    }
    out->char_name[cn_i] = '\0';

    if (linked_name && linked_name[0] != '\0' && strcmp(out->char_name, linked_name) != 0) {
        free(pt);
        muse_project_free(out);
        muse_project_init(out);
        return -5;
    }

    out->bpm      = read_u16(pt + 0x84);
    out->time_sig = read_u16(pt + 0x86);
    if (out->bpm == 0 || out->bpm > 200) out->bpm = 120;
    if (out->time_sig < 2 || out->time_sig > 12) out->time_sig = 4;

    /* inst_tag at 0x88 - null-terminated ASCII listing instrument IDs */
    char inst_tag[80] = {0};
    int tag_max = BDO_HEADER_SIZE - 0x88;
    if (tag_max > (int)sizeof(inst_tag) - 1) tag_max = (int)sizeof(inst_tag) - 1;
    for (int i = 0; i < tag_max; i++) {
        uint8_t c = pt[0x88 + i];
        if (c == 0) break;
        inst_tag[i] = (char)c;
    }
    uint8_t inst_ids[32];
    int num_inst = parse_inst_tag(inst_tag, inst_ids, 32);

    /* Map sequential synth variant IDs back to base ID + profile */
    uint8_t synth_profiles[32] = {0};
    for (int i = 0; i < num_inst; i++) {
        uint8_t id = inst_ids[i];
        if (inst_by_id(id) == NULL) {
            uint8_t bases[] = {0x14, 0x18, 0x1C, 0x20};
            for (int b = 0; b < 4; b++) {
                if (id > bases[b] && id <= bases[b] + 3) {
                    synth_profiles[i] = id - bases[b];
                    inst_ids[i] = bases[b];
                    break;
                }
            }
        }
    }

    /* now parse the actual track/note data */
    size_t off = BDO_HEADER_SIZE;
    if (off + 5 > payload_len) { free(pt); return -5; }

    /* 0x00 marker byte, then instrument count */
    off++; /* skip 0x00 marker */
    uint16_t group_count = read_u16(pt + off); off += 2;

    bool got_effector = false;

    for (int g = 0; g < (int)group_count; g++) {
        if (off + 2 > payload_len) break;
        uint16_t track_count = read_u16(pt + off); off += 2;
        uint8_t gid = (g < num_inst) ? inst_ids[g] : 0x11;
        /* find or create the layer for this instrument */
        int layer_idx = -1;
        for (int li = 0; li < out->num_layers; li++) {
            if (out->layers[li].inst_id == gid) {
                layer_idx = li;
                break;
            }
        }
        if (layer_idx < 0) {
            layer_idx = muse_project_add_layer(out, gid);
            out->layers[layer_idx].synth_profile = (g < num_inst) ? synth_profiles[g] : 0;
        }
        MuseLayer *ly = &out->layers[layer_idx];

        for (int t = 0; t < (int)track_count; t++) {
            if (off + 12 > payload_len) break;
            uint16_t data_size = read_u16(pt + off); off += 2;
            (void)data_size;
            uint16_t marker = read_u16(pt + off); off += 2;

            /* volume is stuffed into the high byte of the marker */
            uint8_t track_vol = (marker >> 8) & 0xFF;
            if (t == 0 && track_vol > 0) {
                ly->volume = track_vol;
            }

            /* 8 bytes of interleaved per-layer sends and global FX values */
            if (t == 0) {
                ly->reverb_send = pt[off];
                ly->delay_send  = pt[off + 2];
                ly->chorus_send = pt[off + 4];
            }

            /* grab global effect settings from the first track we see */
            if (!got_effector) {
                got_effector = true;
                out->effector_reverb       = pt[off + 1];
                out->effector_delay        = pt[off + 3];
                out->effector_chorus_fb    = pt[off + 5];
                out->effector_chorus_depth = pt[off + 6];
                out->effector_chorus_freq  = pt[off + 7];
            }
            off += 8;
            uint16_t note_count = read_u16(pt + off); off += 2;

            for (int n = 0; n < (int)note_count; n++) {
                if (off + BDO_NOTE_SIZE > payload_len) break;
                MuseNote note = {0};
                note.pitch = pt[off];
                note.ntype = pt[off + 1];
                note.vel   = pt[off + 2];
                /* pt[off+3] is velocity2, always same as vel1 in practice */
                note.start = read_f64(pt + off + 4);
                note.dur   = read_f64(pt + off + 12);
                off += BDO_NOTE_SIZE;

                /* everything goes into sublayer 0 on import */
                int target_sub = 0;
                while (ly->num_sublayers <= target_sub) {
                    ly->sublayers = realloc(ly->sublayers,
                        (size_t)(ly->num_sublayers + 1) * sizeof(NoteArray));
                    note_array_init(&ly->sublayers[ly->num_sublayers]);
                    ly->num_sublayers++;
                }
                note_array_push(&ly->sublayers[target_sub], note);
            }
        }
    }

    out->dirty = false;
    free(pt);
    return 0;
}

int bdo_save(const char *path, const MuseProject *proj) {
    /* figure out how big the buffer needs to be */
    size_t max_size = BDO_HEADER_SIZE + 5; /* header + group header */
    for (int i = 0; i < proj->num_layers; i++) {
        const MuseLayer *ly = &proj->layers[i];
        int total = muse_layer_note_count(ly);
        /* notes get split into tracks of 730, each track has 14 bytes overhead
           (2 for data_size field + 2 marker + 8 sends + 2 note_count) */
        int num_tracks = (total + BDO_MAX_NOTES_PER_TRACK - 1) / BDO_MAX_NOTES_PER_TRACK;
        if (num_tracks < 1) num_tracks = 1;
        max_size += (size_t)num_tracks * 14 + (size_t)total * BDO_NOTE_SIZE;
        max_size += 14 + 2; /* empty trailing track + group track count */
    }
    /* ICE needs 8-byte alignment */
    max_size = (max_size + 7) & ~(size_t)7;

    uint8_t *pt = calloc(max_size, 1);
    if (!pt) return -1;
    size_t off = 0;

    /* write header - same layout BDO expects */
    write_u32(pt + 0x00, proj->owner_id);
    /* name in both UTF-16LE fields (BDO expects duplicate) */
    for (int i = 0; i < 31 && proj->char_name[i]; i++) {
        pt[0x08 + i * 2] = (uint8_t)proj->char_name[i];
        pt[0x08 + i * 2 + 1] = 0;
        pt[0x46 + i * 2] = (uint8_t)proj->char_name[i];
        pt[0x46 + i * 2 + 1] = 0;
    }
    write_u16(pt + 0x84, proj->bpm);
    write_u16(pt + 0x86, proj->time_sig);

    /* build the instrument tag string at 0x88 */
    char inst_tag[64] = {0};
    int tag_off = 0;
    for (int i = 0; i < proj->num_layers; i++) {
        if (i > 0) tag_off += snprintf(inst_tag + tag_off,
                                        sizeof(inst_tag) - (size_t)tag_off, ",");
        tag_off += snprintf(inst_tag + tag_off,
                            sizeof(inst_tag) - (size_t)tag_off, "%d",
                            inst_synth_variant(proj->layers[i].inst_id,
                                               proj->layers[i].synth_profile));
    }
    memcpy(pt + 0x88, inst_tag, strlen(inst_tag));

    off = BDO_HEADER_SIZE;

    /* group header */
    pt[off++] = 0x00; /* marker */
    write_u16(pt + off, (uint16_t)proj->num_layers); off += 2;

    for (int g = 0; g < proj->num_layers; g++) {
        const MuseLayer *ly = &proj->layers[g];
        double exceed = muse_layer_exceed_ms(ly);

        /* collect all exportable notes across sublayers */
        int total_notes = 0;
        for (int s = 0; s < ly->num_sublayers; s++) {
            const NoteArray *na = &ly->sublayers[s];
            for (int n = 0; n < na->count; n++) {
                if (exceed >= 0 && na->notes[n].start >= exceed) continue;
                total_notes++;
            }
        }

        /* BDO splits into tracks of max 730 notes each */
        int num_data_tracks = (total_notes + BDO_MAX_NOTES_PER_TRACK - 1) / BDO_MAX_NOTES_PER_TRACK;
        if (num_data_tracks < 1) num_data_tracks = 1;
        int total_tracks = num_data_tracks + 1; /* +1 empty trailing */
        write_u16(pt + off, (uint16_t)total_tracks); off += 2;

        uint8_t export_id = inst_synth_variant(ly->inst_id, ly->synth_profile);
        uint16_t marker = export_id | ((uint16_t)ly->volume << 8);
        uint8_t sends[8];
        sends[0] = ly->reverb_send;
        sends[1] = proj->effector_reverb;
        sends[2] = ly->delay_send;
        sends[3] = proj->effector_delay;
        sends[4] = ly->chorus_send;
        sends[5] = proj->effector_chorus_fb;
        sends[6] = proj->effector_chorus_depth;
        sends[7] = proj->effector_chorus_freq;

        /* flatten all notes into one list for splitting */
        int note_idx = 0;
        int notes_written = 0;
        int cur_sub = 0, cur_n = 0;

        for (int t = 0; t < num_data_tracks; t++) {
            int chunk = total_notes - notes_written;
            if (chunk > BDO_MAX_NOTES_PER_TRACK) chunk = BDO_MAX_NOTES_PER_TRACK;

            uint16_t data_size = 2 + 8 + 2 + (uint16_t)(chunk * BDO_NOTE_SIZE);
            write_u16(pt + off, data_size); off += 2;
            write_u16(pt + off, marker); off += 2;
            memcpy(pt + off, sends, 8); off += 8;
            write_u16(pt + off, (uint16_t)chunk); off += 2;

            int written = 0;
            while (written < chunk && cur_sub < ly->num_sublayers) {
                const NoteArray *na = &ly->sublayers[cur_sub];
                while (cur_n < na->count && written < chunk) {
                    const MuseNote *note = &na->notes[cur_n];
                    cur_n++;
                    if (exceed >= 0 && note->start >= exceed) continue;
                    pt[off]     = note->pitch & 0x7F;
                    pt[off + 1] = note->ntype;
                    pt[off + 2] = note->vel & 0x7F;
                    pt[off + 3] = note->vel & 0x7F;
                    write_f64(pt + off + 4, note->start);
                    write_f64(pt + off + 12, note->dur);
                    off += BDO_NOTE_SIZE;
                    written++;
                    notes_written++;
                }
                if (cur_n >= na->count) { cur_sub++; cur_n = 0; }
            }
        }

        /* empty trailing track */
        write_u16(pt + off, 12); off += 2;
        write_u16(pt + off, marker); off += 2;
        memcpy(pt + off, sends, 8); off += 8;
        write_u16(pt + off, 0); off += 2;
    }

    /* pad to 8-byte boundary for ICE */
    off = (off + 7) & ~(size_t)7;

    /* encrypt and write out */
    ice_encrypt(pt, off);

    /* write it out with the version prefix */
    FILE *f = fopen_utf8(path, "wb");
    if (!f) { free(pt); return -1; }

    uint8_t ver[4];
    write_u32(ver, BDO_VERSION);
    fwrite(ver, 1, 4, f);
    fwrite(pt, 1, off, f);
    fclose(f);
    free(pt);
    return 0;
}

int bdo_extract_owner(const char *path, uint32_t *owner_id_out,
                      char *name_out, int name_sz) {
    FILE *f = fopen_utf8(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize < 12) { fclose(f); return -1; }

    uint8_t *raw = malloc((size_t)fsize);
    fread(raw, 1, (size_t)fsize, f);
    fclose(f);

    uint32_t ver = read_u32(raw);
    if (ver != BDO_VERSION) { free(raw); return -2; }

    size_t payload_len = (size_t)fsize - 4;
    uint8_t *pt = malloc(payload_len);
    int rc = ice_decrypt_owner_header(raw + 4, payload_len, pt, payload_len);
    free(raw);
    if (rc != 0) { free(pt); return -3; }

    *owner_id_out = read_u32(pt);

    /* character name is UTF-16LE at offset 8 */
    if (name_out && name_sz > 0) {
        int out_i = 0;
        for (int i = 0; i < BDO_NAME_FIELD && i + 1 < (int)payload_len - 8; i += 2) {
            uint16_t ch = pt[8 + i] | (pt[8 + i + 1] << 8);
            if (ch == 0) break;
            if (ch < 128 && out_i < name_sz - 1)
                name_out[out_i++] = (char)ch;
        }
        name_out[out_i] = '\0';
    }

    free(pt);
    return 0;
}
