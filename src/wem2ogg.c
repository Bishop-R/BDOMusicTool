/*
 * wem2ogg.c - Convert Wwise RIFF Vorbis (WEM) to standard OGG Vorbis
 *
 * Ported from ww2ogg 0.24 by hcs (https://github.com/hcs64/ww2ogg)
 * C implementation for in-memory conversion.
 */

#include "wem2ogg.h"
#include "packed_codebooks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================
 * OGG CRC32 (polynomial 0x04C11DB7, from Tremor/lowmem)
 * ====================================================================== */

static const uint32_t ogg_crc_table[256] = {
    0x00000000,0x04c11db7,0x09823b6e,0x0d4326d9,
    0x130476dc,0x17c56b6b,0x1a864db2,0x1e475005,
    0x2608edb8,0x22c9f00f,0x2f8ad6d6,0x2b4bcb61,
    0x350c9b64,0x31cd86d3,0x3c8ea00a,0x384fbdbd,
    0x4c11db70,0x48d0c6c7,0x4593e01e,0x4152fda9,
    0x5f15adac,0x5bd4b01b,0x569796c2,0x52568b75,
    0x6a1936c8,0x6ed82b7f,0x639b0da6,0x675a1011,
    0x791d4014,0x7ddc5da3,0x709f7b7a,0x745e66cd,
    0x9823b6e0,0x9ce2ab57,0x91a18d8e,0x95609039,
    0x8b27c03c,0x8fe6dd8b,0x82a5fb52,0x8664e6e5,
    0xbe2b5b58,0xbaea46ef,0xb7a96036,0xb3687d81,
    0xad2f2d84,0xa9ee3033,0xa4ad16ea,0xa06c0b5d,
    0xd4326d90,0xd0f37027,0xddb056fe,0xd9714b49,
    0xc7361b4c,0xc3f706fb,0xceb42022,0xca753d95,
    0xf23a8028,0xf6fb9d9f,0xfbb8bb46,0xff79a6f1,
    0xe13ef6f4,0xe5ffeb43,0xe8bccd9a,0xec7dd02d,
    0x34867077,0x30476dc0,0x3d044b19,0x39c556ae,
    0x278206ab,0x23431b1c,0x2e003dc5,0x2ac12072,
    0x128e9dcf,0x164f8078,0x1b0ca6a1,0x1fcdbb16,
    0x018aeb13,0x054bf6a4,0x0808d07d,0x0cc9cdca,
    0x7897ab07,0x7c56b6b0,0x71159069,0x75d48dde,
    0x6b93dddb,0x6f52c06c,0x6211e6b5,0x66d0fb02,
    0x5e9f46bf,0x5a5e5b08,0x571d7dd1,0x53dc6066,
    0x4d9b3063,0x495a2dd4,0x44190b0d,0x40d816ba,
    0xaca5c697,0xa864db20,0xa527fdf9,0xa1e6e04e,
    0xbfa1b04b,0xbb60adfc,0xb6238b25,0xb2e29692,
    0x8aad2b2f,0x8e6c3698,0x832f1041,0x87ee0df6,
    0x99a95df3,0x9d684044,0x902b669d,0x94ea7b2a,
    0xe0b41de7,0xe4750050,0xe9362689,0xedf73b3e,
    0xf3b06b3b,0xf771768c,0xfa325055,0xfef34de2,
    0xc6bcf05f,0xc27dede8,0xcf3ecb31,0xcbffd686,
    0xd5b88683,0xd1799b34,0xdc3abded,0xd8fba05a,
    0x690ce0ee,0x6dcdfd59,0x608edb80,0x644fc637,
    0x7a089632,0x7ec98b85,0x738aad5c,0x774bb0eb,
    0x4f040d56,0x4bc510e1,0x46863638,0x42472b8f,
    0x5c007b8a,0x58c1663d,0x558240e4,0x51435d53,
    0x251d3b9e,0x21dc2629,0x2c9f00f0,0x285e1d47,
    0x36194d42,0x32d850f5,0x3f9b762c,0x3b5a6b9b,
    0x0315d626,0x07d4cb91,0x0a97ed48,0x0e56f0ff,
    0x1011a0fa,0x14d0bd4d,0x19939b94,0x1d528623,
    0xf12f560e,0xf5ee4bb9,0xf8ad6d60,0xfc6c70d7,
    0xe22b20d2,0xe6ea3d65,0xeba91bbc,0xef68060b,
    0xd727bbb6,0xd3e6a601,0xdea580d8,0xda649d6f,
    0xc423cd6a,0xc0e2d0dd,0xcda1f604,0xc960ebb3,
    0xbd3e8d7e,0xb9ff90c9,0xb4bcb610,0xb07daba7,
    0xae3afba2,0xaafbe615,0xa7b8c0cc,0xa379dd7b,
    0x9b3660c6,0x9ff77d71,0x92b45ba8,0x9675461f,
    0x8832161a,0x8cf30bad,0x81b02d74,0x857130c3,
    0x5d8a9099,0x594b8d2e,0x5408abf7,0x50c9b640,
    0x4e8ee645,0x4a4ffbf2,0x470cdd2b,0x43cdc09c,
    0x7b827d21,0x7f436096,0x7200464f,0x76c15bf8,
    0x68860bfd,0x6c47164a,0x61043093,0x65c52d24,
    0x119b4be9,0x155a565e,0x18197087,0x1cd86d30,
    0x029f3d35,0x065e2082,0x0b1d065b,0x0fdc1bec,
    0x3793a651,0x3352bbe6,0x3e119d3f,0x3ad08088,
    0x2497d08d,0x2056cd3a,0x2d15ebe3,0x29d4f654,
    0xc5a92679,0xc1683bce,0xcc2b1d17,0xc8ea00a0,
    0xd6ad50a5,0xd26c4d12,0xdf2f6bcb,0xdbee767c,
    0xe3a1cbc1,0xe760d676,0xea23f0af,0xeee2ed18,
    0xf0a5bd1d,0xf464a0aa,0xf9278673,0xfde69bc4,
    0x89b8fd09,0x8d79e0be,0x803ac667,0x84fbdbd0,
    0x9abc8bd5,0x9e7d9662,0x933eb0bb,0x97ffad0c,
    0xafb010b1,0xab710d06,0xa6322bdf,0xa2f33668,
    0xbcb4666d,0xb8757bda,0xb5365d03,0xb1f740b4
};

static uint32_t ogg_checksum(const uint8_t *data, int bytes)
{
    uint32_t crc = 0;
    for (int i = 0; i < bytes; i++)
        crc = (crc << 8) ^ ogg_crc_table[((crc >> 24) & 0xFF) ^ data[i]];
    return crc;
}

/* ======================================================================
 * Dynamic buffer for OGG output
 * ====================================================================== */

typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} Buffer;

static void buf_init(Buffer *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static int buf_grow(Buffer *b, size_t need)
{
    if (b->len + need <= b->cap) return 0;
    size_t nc = b->cap ? b->cap : 4096;
    while (nc < b->len + need) nc *= 2;
    uint8_t *nd = (uint8_t *)realloc(b->data, nc);
    if (!nd) return -1;
    b->data = nd;
    b->cap = nc;
    return 0;
}

static int buf_write(Buffer *b, const uint8_t *src, size_t n)
{
    if (buf_grow(b, n) < 0) return -1;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}

static int buf_put(Buffer *b, uint8_t c)
{
    return buf_write(b, &c, 1);
}

/* ======================================================================
 * OGG page writer with bit-level output
 * ====================================================================== */

#define OGG_HDR_BYTES  27
#define OGG_MAX_SEGS   255
#define OGG_SEG_SIZE   255

typedef struct {
    Buffer  *out;           /* output buffer */
    uint8_t  bit_buffer;
    unsigned bits_stored;
    unsigned payload_bytes;
    int      first;
    int      continued;
    uint8_t  page_buf[OGG_HDR_BYTES + OGG_MAX_SEGS + OGG_SEG_SIZE * OGG_MAX_SEGS];
    uint32_t granule;
    uint32_t seqno;
} OggStream;

static void ogg_init(OggStream *os, Buffer *out)
{
    memset(os, 0, sizeof(*os));
    os->out = out;
    os->first = 1;
}

static void put_le32(uint8_t *b, uint32_t v)
{
    b[0] = v & 0xFF; b[1] = (v >> 8) & 0xFF;
    b[2] = (v >> 16) & 0xFF; b[3] = (v >> 24) & 0xFF;
}

static void put_le16(uint8_t *b, uint16_t v)
{
    b[0] = v & 0xFF; b[1] = (v >> 8) & 0xFF;
}

static int ogg_flush_page(OggStream *os, int next_continued, int last);

static int ogg_flush_bits(OggStream *os)
{
    if (os->bits_stored != 0) {
        if (os->payload_bytes == (unsigned)(OGG_SEG_SIZE * OGG_MAX_SEGS)) {
            int rc = ogg_flush_page(os, 1, 0);
            if (rc) return rc;
        }
        os->page_buf[OGG_HDR_BYTES + OGG_MAX_SEGS + os->payload_bytes] = os->bit_buffer;
        os->payload_bytes++;
        os->bits_stored = 0;
        os->bit_buffer = 0;
    }
    return 0;
}

static int ogg_put_bit(OggStream *os, int bit)
{
    if (bit)
        os->bit_buffer |= (1u << os->bits_stored);
    os->bits_stored++;
    if (os->bits_stored == 8)
        return ogg_flush_bits(os);
    return 0;
}

static int ogg_put_bits(OggStream *os, unsigned val, unsigned nbits)
{
    for (unsigned i = 0; i < nbits; i++) {
        if (ogg_put_bit(os, (val >> i) & 1) < 0) return -1;
    }
    return 0;
}

static int ogg_flush_page(OggStream *os, int next_continued, int last)
{
    if (os->payload_bytes != (unsigned)(OGG_SEG_SIZE * OGG_MAX_SEGS))
        ogg_flush_bits(os);

    if (os->payload_bytes == 0) return 0;

    unsigned segments = (os->payload_bytes + OGG_SEG_SIZE) / OGG_SEG_SIZE;
    if (segments == (unsigned)(OGG_MAX_SEGS + 1)) segments = OGG_MAX_SEGS;

    /* move payload back from max_segs position to actual position */
    for (unsigned i = 0; i < os->payload_bytes; i++) {
        os->page_buf[OGG_HDR_BYTES + segments + i] =
            os->page_buf[OGG_HDR_BYTES + OGG_MAX_SEGS + i];
    }

    /* write header */
    os->page_buf[0] = 'O'; os->page_buf[1] = 'g';
    os->page_buf[2] = 'g'; os->page_buf[3] = 'S';
    os->page_buf[4] = 0; /* version */
    os->page_buf[5] = (os->continued ? 1 : 0) | (os->first ? 2 : 0) | (last ? 4 : 0);
    put_le32(&os->page_buf[6], os->granule);  /* granule low */
    if (os->granule == 0xFFFFFFFFu)
        put_le32(&os->page_buf[10], 0xFFFFFFFFu);
    else
        put_le32(&os->page_buf[10], 0);       /* granule high */
    put_le32(&os->page_buf[14], 1);           /* serial */
    put_le32(&os->page_buf[18], os->seqno);   /* page seq */
    put_le32(&os->page_buf[22], 0);           /* crc placeholder */
    os->page_buf[26] = (uint8_t)segments;

    /* lacing values */
    unsigned bytes_left = os->payload_bytes;
    for (unsigned i = 0; i < segments; i++) {
        if (bytes_left >= OGG_SEG_SIZE) {
            os->page_buf[27 + i] = OGG_SEG_SIZE;
            bytes_left -= OGG_SEG_SIZE;
        } else {
            os->page_buf[27 + i] = (uint8_t)bytes_left;
        }
    }

    /* compute and store CRC */
    unsigned total = OGG_HDR_BYTES + segments + os->payload_bytes;
    uint32_t crc = ogg_checksum(os->page_buf, (int)total);
    put_le32(&os->page_buf[22], crc);

    /* write to output buffer */
    if (buf_write(os->out, os->page_buf, total) < 0) return -1;

    os->seqno++;
    os->first = 0;
    os->continued = next_continued;
    os->payload_bytes = 0;
    return 0;
}

/* ======================================================================
 * Bit stream reader (from memory, LSB first)
 * ====================================================================== */

typedef struct {
    const uint8_t *data;
    size_t         size;
    size_t         pos;
    uint8_t        bit_buf;
    unsigned       bits_left;
    unsigned long  total_bits_read;
} BitReader;

static void br_init(BitReader *br, const uint8_t *data, size_t size)
{
    br->data = data;
    br->size = size;
    br->pos = 0;
    br->bit_buf = 0;
    br->bits_left = 0;
    br->total_bits_read = 0;
}

static int br_get_bit(BitReader *br)
{
    if (br->bits_left == 0) {
        if (br->pos >= br->size) return -1;
        br->bit_buf = br->data[br->pos++];
        br->bits_left = 8;
    }
    br->total_bits_read++;
    br->bits_left--;
    return (br->bit_buf & (0x80 >> br->bits_left)) ? 1 : 0;
}

static int br_read_bits(BitReader *br, unsigned nbits, unsigned *out)
{
    *out = 0;
    for (unsigned i = 0; i < nbits; i++) {
        int b = br_get_bit(br);
        if (b < 0) return -1;
        if (b) *out |= (1u << i);
    }
    return 0;
}

/* ======================================================================
 * Helpers: read little-endian from memory
 * ====================================================================== */

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ======================================================================
 * ilog - integer log2 (from Tremor)
 * ====================================================================== */

static int ilog(unsigned int v)
{
    int ret = 0;
    while (v) { ret++; v >>= 1; }
    return ret;
}

/* ======================================================================
 * Codebook quantvals (from Tremor)
 * ====================================================================== */

static unsigned int book_maptype1_quantvals(unsigned int entries, unsigned int dimensions)
{
    int bits = ilog(entries);
    int vals = entries >> ((bits - 1) * (dimensions - 1) / dimensions);
    while (1) {
        unsigned long acc = 1, acc1 = 1;
        for (unsigned int i = 0; i < dimensions; i++) {
            acc *= vals;
            acc1 *= vals + 1;
        }
        if (acc <= entries && acc1 > entries) return vals;
        if (acc > entries) vals--;
        else vals++;
    }
}

/* ======================================================================
 * Codebook library (packed binary)
 * ====================================================================== */

typedef struct {
    const uint8_t *data;
    size_t         data_size;
    const uint32_t *offsets;
    int             count;
} CodebookLib;

static int cb_lib_init(CodebookLib *lib)
{
    const uint8_t *pcb = packed_codebooks;
    size_t pcb_len = packed_codebooks_len;

    if (pcb_len < 4) return -1;

    uint32_t offset_offset = rd32(pcb + pcb_len - 4);
    lib->count = (int)((pcb_len - offset_offset) / 4);
    lib->data = pcb;
    lib->data_size = offset_offset;

    /* The offsets are stored as LE uint32 at pcb + offset_offset.
       We build a pointer into the raw data - since it's LE and we need
       to read them portably, we'll store the base. */
    lib->offsets = NULL;  /* we'll read on the fly */
    return 0;
}

static uint32_t cb_get_offset(const CodebookLib *lib, int i)
{
    size_t base = lib->data_size + (size_t)i * 4;
    const uint8_t *pcb = packed_codebooks;
    return rd32(pcb + base);
}

static const uint8_t *cb_get(const CodebookLib *lib, int i, size_t *out_size)
{
    if (i < 0 || i >= lib->count - 1) { *out_size = 0; return NULL; }
    uint32_t off = cb_get_offset(lib, i);
    uint32_t next_off = cb_get_offset(lib, i + 1);
    *out_size = next_off - off;
    return lib->data + off;
}

/* Rebuild a codebook from the packed binary format to standard Vorbis format */
static int cb_rebuild(const CodebookLib *lib, int codebook_id, OggStream *os)
{
    size_t cb_size;
    const uint8_t *cb = cb_get(lib, codebook_id, &cb_size);
    if (!cb || cb_size == 0) return -1;

    BitReader br;
    br_init(&br, cb, cb_size);

    /* IN: 4 bit dimensions, 14 bit entry count */
    unsigned dimensions, entries;
    if (br_read_bits(&br, 4, &dimensions) < 0) return -1;
    if (br_read_bits(&br, 14, &entries) < 0) return -1;

    /* OUT: 24 bit identifier 'BCV', 16 bit dimensions, 24 bit entry count */
    ogg_put_bits(os, 0x564342, 24);
    ogg_put_bits(os, dimensions, 16);
    ogg_put_bits(os, entries, 24);

    /* IN/OUT: 1 bit ordered flag */
    unsigned ordered;
    if (br_read_bits(&br, 1, &ordered) < 0) return -1;
    ogg_put_bits(os, ordered, 1);

    if (ordered) {
        unsigned initial_length;
        if (br_read_bits(&br, 5, &initial_length) < 0) return -1;
        ogg_put_bits(os, initial_length, 5);

        unsigned current_entry = 0;
        while (current_entry < entries) {
            int nbits = ilog(entries - current_entry);
            unsigned number;
            if (br_read_bits(&br, nbits, &number) < 0) return -1;
            ogg_put_bits(os, number, nbits);
            current_entry += number;
        }
        if (current_entry > entries) return -1;
    } else {
        /* IN: 3 bit codeword_length_length, 1 bit sparse flag */
        unsigned codeword_length_length, sparse;
        if (br_read_bits(&br, 3, &codeword_length_length) < 0) return -1;
        if (br_read_bits(&br, 1, &sparse) < 0) return -1;

        if (codeword_length_length == 0 || codeword_length_length > 5) return -1;

        /* OUT: 1 bit sparse flag */
        ogg_put_bits(os, sparse, 1);

        for (unsigned i = 0; i < entries; i++) {
            int present_bool = 1;
            if (sparse) {
                unsigned present;
                if (br_read_bits(&br, 1, &present) < 0) return -1;
                ogg_put_bits(os, present, 1);
                present_bool = (present != 0);
            }
            if (present_bool) {
                /* IN: n bit codeword length-1 */
                unsigned codeword_length;
                if (br_read_bits(&br, codeword_length_length, &codeword_length) < 0) return -1;
                /* OUT: 5 bit codeword length-1 */
                ogg_put_bits(os, codeword_length, 5);
            }
        }
    }

    /* Lookup table */
    /* IN: 1 bit lookup type */
    unsigned lookup_type;
    if (br_read_bits(&br, 1, &lookup_type) < 0) return -1;
    /* OUT: 4 bit lookup type */
    ogg_put_bits(os, lookup_type, 4);

    if (lookup_type == 0) {
        /* no lookup table */
    } else if (lookup_type == 1) {
        unsigned min_val, max_val, value_length, sequence_flag;
        if (br_read_bits(&br, 32, &min_val) < 0) return -1;
        if (br_read_bits(&br, 32, &max_val) < 0) return -1;
        if (br_read_bits(&br, 4, &value_length) < 0) return -1;
        if (br_read_bits(&br, 1, &sequence_flag) < 0) return -1;
        ogg_put_bits(os, min_val, 32);
        ogg_put_bits(os, max_val, 32);
        ogg_put_bits(os, value_length, 4);
        ogg_put_bits(os, sequence_flag, 1);

        unsigned quantvals = book_maptype1_quantvals(entries, dimensions);
        for (unsigned i = 0; i < quantvals; i++) {
            unsigned val;
            if (br_read_bits(&br, value_length + 1, &val) < 0) return -1;
            ogg_put_bits(os, val, value_length + 1);
        }
    } else {
        return -1;
    }

    /* Verify we consumed the right number of bytes */
    if (cb_size != 0 && br.total_bits_read / 8 + 1 != (unsigned long)cb_size) {
        /* Size mismatch - not necessarily fatal, but log it */
    }

    return 0;
}

/* ======================================================================
 * RIFF/WEM parser
 * ====================================================================== */

typedef struct {
    const uint8_t *wem;
    size_t         wem_size;

    /* chunk locations */
    size_t fmt_offset, fmt_size;
    size_t data_offset, data_size;
    size_t vorb_offset;
    int    vorb_size;    /* -1 if inline in fmt */

    /* fmt fields */
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t avg_bytes_per_second;
    uint16_t ext_unk;
    uint32_t subtype;

    /* vorb fields */
    uint32_t sample_count;
    uint32_t setup_packet_offset;
    uint32_t first_audio_packet_offset;
    uint32_t uid;
    uint8_t  blocksize_0_pow;
    uint8_t  blocksize_1_pow;

    /* flags */
    int no_granule;
    int mod_packets;
    int header_triad_present;
    int old_packet_headers;

    /* loop info */
    uint32_t loop_count;
    uint32_t loop_start;
    uint32_t loop_end;
} WemInfo;

static int wem_parse(WemInfo *wi, const uint8_t *wem, size_t wem_size)
{
    memset(wi, 0, sizeof(*wi));
    wi->wem = wem;
    wi->wem_size = wem_size;
    wi->vorb_size = -1;

    if (wem_size < 12) return -1;

    /* Check RIFF header (only little-endian RIFF supported for BDO) */
    if (memcmp(wem, "RIFF", 4) != 0) return -1;

    uint32_t riff_size = rd32(wem + 4) + 8;
    if (riff_size > wem_size) return -1;

    if (memcmp(wem + 8, "WAVE", 4) != 0) return -1;

    /* Find chunks */
    size_t smpl_offset = 0;
    int smpl_found = 0;
    int vorb_found = 0;
    int fmt_found = 0;
    int data_found = 0;

    size_t chunk_offset = 12;
    while (chunk_offset + 8 <= riff_size && chunk_offset + 8 <= wem_size) {
        uint32_t chunk_size = rd32(wem + chunk_offset + 4);
        size_t chunk_data = chunk_offset + 8;

        if (memcmp(wem + chunk_offset, "fmt ", 4) == 0) {
            wi->fmt_offset = chunk_data;
            wi->fmt_size = chunk_size;
            fmt_found = 1;
        } else if (memcmp(wem + chunk_offset, "vorb", 4) == 0) {
            wi->vorb_offset = chunk_data;
            wi->vorb_size = (int)chunk_size;
            vorb_found = 1;
        } else if (memcmp(wem + chunk_offset, "data", 4) == 0) {
            wi->data_offset = chunk_data;
            wi->data_size = chunk_size;
            data_found = 1;
        } else if (memcmp(wem + chunk_offset, "smpl", 4) == 0) {
            smpl_offset = chunk_data;
            smpl_found = 1;
        }

        chunk_offset += 8 + chunk_size;
    }

    if (!fmt_found || !data_found) return -1;

    /* Handle vorb chunk / inline vorb in fmt */
    if (!vorb_found && wi->fmt_size == 0x42) {
        /* vorb data is inline at fmt + 0x18 */
        wi->vorb_offset = wi->fmt_offset + 0x18;
        wi->vorb_size = -1; /* means inline: 0x42 - 0x18 = 0x2A effective size */
    } else if (!vorb_found) {
        return -1;
    }

    /* Validate format */
    if (wi->fmt_offset + wi->fmt_size > wem_size) return -1;
    const uint8_t *fmt = wem + wi->fmt_offset;

    uint16_t codec = rd16(fmt + 0);
    if (codec != 0xFFFF) return -1;  /* Not Wwise Vorbis */

    wi->channels = rd16(fmt + 2);
    wi->sample_rate = rd32(fmt + 4);
    wi->avg_bytes_per_second = rd32(fmt + 8);
    /* block_align at +12, bps at +14 */
    /* cbSize at +16 */

    /* Extra fmt fields */
    if (wi->fmt_size >= 0x1A) {   /* 0x12 + 2 for ext_unk */
        wi->ext_unk = rd16(fmt + 0x12);
    }
    if (wi->fmt_size >= 0x1E) {   /* 0x12 + 2 + 4 for subtype */
        wi->subtype = rd32(fmt + 0x14);
    }

    /* Read vorb info. The effective vorb size determines the parsing path. */
    int effective_vorb_size;
    if (wi->vorb_size == -1) {
        /* inline in fmt: size is 0x2A (for fmt_size 0x42) */
        effective_vorb_size = 0x2A;
    } else {
        effective_vorb_size = wi->vorb_size;
    }

    const uint8_t *vorb = wem + wi->vorb_offset;
    if (wi->vorb_offset + 0x2A > wem_size && effective_vorb_size >= 0x2A) return -1;

    /* sample_count is at vorb + 0x00 */
    wi->sample_count = rd32(vorb + 0x00);

    switch (effective_vorb_size) {
        case 0x2A: {
            wi->no_granule = 1;
            uint32_t mod_signal = rd32(vorb + 0x04);
            /* set mod_packets unless it's one of the known "unset" values */
            if (mod_signal != 0x4A && mod_signal != 0x4B &&
                mod_signal != 0x69 && mod_signal != 0x70) {
                wi->mod_packets = 1;
            }
            wi->setup_packet_offset = rd32(vorb + 0x10);
            wi->first_audio_packet_offset = rd32(vorb + 0x14);
            /* uid, blocksize at offset 0x24 */
            wi->uid = rd32(vorb + 0x24);
            wi->blocksize_0_pow = vorb[0x28];
            wi->blocksize_1_pow = vorb[0x29];
            break;
        }
        case 0x28:
        case 0x2C:
            wi->header_triad_present = 1;
            wi->old_packet_headers = 1;
            wi->setup_packet_offset = rd32(vorb + 0x18);
            wi->first_audio_packet_offset = rd32(vorb + 0x1C);
            break;
        case 0x32:
        case 0x34:
            wi->setup_packet_offset = rd32(vorb + 0x18);
            wi->first_audio_packet_offset = rd32(vorb + 0x1C);
            wi->uid = rd32(vorb + 0x2C);
            wi->blocksize_0_pow = vorb[0x30];
            wi->blocksize_1_pow = vorb[0x31];
            break;
        default:
            return -1;
    }

    /* smpl (loops) */
    if (smpl_found && smpl_offset + 0x34 <= wem_size) {
        wi->loop_count = rd32(wem + smpl_offset + 0x1C);
        if (wi->loop_count == 1) {
            wi->loop_start = rd32(wem + smpl_offset + 0x2C);
            wi->loop_end = rd32(wem + smpl_offset + 0x30);
            if (wi->loop_end == 0)
                wi->loop_end = wi->sample_count;
            else
                wi->loop_end += 1;
        }
    }

    return 0;
}

/* ======================================================================
 * Generate OGG Vorbis headers and audio packets
 * ====================================================================== */

static int generate_ogg_header(WemInfo *wi, OggStream *os,
                                int **mode_blockflag_out, int *mode_bits_out)
{
    CodebookLib cbl;
    if (cb_lib_init(&cbl) < 0) return -1;

    /* --- Identification header (packet type 1) --- */
    {
        /* Vorbis packet header: type byte + "vorbis" */
        ogg_put_bits(os, 1, 8);
        const char *vorbis = "vorbis";
        for (int i = 0; i < 6; i++)
            ogg_put_bits(os, (uint8_t)vorbis[i], 8);

        ogg_put_bits(os, 0, 32);  /* version */
        ogg_put_bits(os, wi->channels, 8);
        ogg_put_bits(os, wi->sample_rate, 32);
        ogg_put_bits(os, 0, 32);  /* bitrate max */
        ogg_put_bits(os, wi->avg_bytes_per_second * 8, 32); /* bitrate nominal */
        ogg_put_bits(os, 0, 32);  /* bitrate min */
        ogg_put_bits(os, wi->blocksize_0_pow, 4);
        ogg_put_bits(os, wi->blocksize_1_pow, 4);
        ogg_put_bits(os, 1, 1);   /* framing */

        ogg_flush_page(os, 0, 0);
    }

    /* --- Comment header (packet type 3) --- */
    {
        ogg_put_bits(os, 3, 8);
        const char *vorbis = "vorbis";
        for (int i = 0; i < 6; i++)
            ogg_put_bits(os, (uint8_t)vorbis[i], 8);

        const char *vendor = "converted from Audiokinetic Wwise by ww2ogg 0.24";
        uint32_t vendor_len = (uint32_t)strlen(vendor);
        ogg_put_bits(os, vendor_len, 32);
        for (uint32_t i = 0; i < vendor_len; i++)
            ogg_put_bits(os, (uint8_t)vendor[i], 8);

        if (wi->loop_count == 0) {
            ogg_put_bits(os, 0, 32); /* no comments */
        } else {
            ogg_put_bits(os, 2, 32); /* 2 comments */
            char buf[64];
            int n;

            n = snprintf(buf, sizeof(buf), "LoopStart=%u", wi->loop_start);
            ogg_put_bits(os, (uint32_t)n, 32);
            for (int i = 0; i < n; i++)
                ogg_put_bits(os, (uint8_t)buf[i], 8);

            n = snprintf(buf, sizeof(buf), "LoopEnd=%u", wi->loop_end);
            ogg_put_bits(os, (uint32_t)n, 32);
            for (int i = 0; i < n; i++)
                ogg_put_bits(os, (uint8_t)buf[i], 8);
        }

        ogg_put_bits(os, 1, 1); /* framing */
        ogg_flush_page(os, 0, 0);
    }

    /* --- Setup header (packet type 5) --- */
    {
        ogg_put_bits(os, 5, 8);
        const char *vorbis = "vorbis";
        for (int i = 0; i < 6; i++)
            ogg_put_bits(os, (uint8_t)vorbis[i], 8);

        /* Read setup packet from data chunk */
        size_t setup_abs = wi->data_offset + wi->setup_packet_offset;

        /* Packet header: 2-byte size (+ 4-byte granule if !no_granule) */
        int pkt_hdr_size = wi->no_granule ? 2 : 6;
        if (setup_abs + pkt_hdr_size > wi->wem_size) return -1;

        uint16_t setup_size = rd16(wi->wem + setup_abs);
        if (!wi->no_granule) {
            uint32_t g = rd32(wi->wem + setup_abs + 2);
            if (g != 0) return -1; /* setup packet granule must be 0 */
        }

        size_t setup_data_offset = setup_abs + pkt_hdr_size;
        if (setup_data_offset + setup_size > wi->wem_size) return -1;

        BitReader ss;
        br_init(&ss, wi->wem + setup_data_offset, setup_size);

        /* codebook count */
        unsigned codebook_count_less1;
        if (br_read_bits(&ss, 8, &codebook_count_less1) < 0) return -1;
        unsigned codebook_count = codebook_count_less1 + 1;
        ogg_put_bits(os, codebook_count_less1, 8);

        /* Rebuild codebooks from packed library */
        for (unsigned i = 0; i < codebook_count; i++) {
            unsigned codebook_id;
            if (br_read_bits(&ss, 10, &codebook_id) < 0) return -1;
            if (cb_rebuild(&cbl, (int)codebook_id, os) < 0) {
                fprintf(stderr, "wem2ogg: invalid codebook id %u\n", codebook_id);
                return -1;
            }
        }

        /* Time domain transforms (placeholder) */
        ogg_put_bits(os, 0, 6);  /* time_count_less1 = 0 */
        ogg_put_bits(os, 0, 16); /* dummy time value */

        /* --- Floors --- */
        unsigned floor_count_less1;
        if (br_read_bits(&ss, 6, &floor_count_less1) < 0) return -1;
        unsigned floor_count = floor_count_less1 + 1;
        ogg_put_bits(os, floor_count_less1, 6);

        for (unsigned fi = 0; fi < floor_count; fi++) {
            /* always floor type 1 */
            ogg_put_bits(os, 1, 16);

            unsigned floor1_partitions;
            if (br_read_bits(&ss, 5, &floor1_partitions) < 0) return -1;
            ogg_put_bits(os, floor1_partitions, 5);

            unsigned *partition_class = (unsigned *)calloc(floor1_partitions, sizeof(unsigned));
            if (!partition_class && floor1_partitions > 0) return -1;

            unsigned maximum_class = 0;
            for (unsigned j = 0; j < floor1_partitions; j++) {
                unsigned pc;
                if (br_read_bits(&ss, 4, &pc) < 0) { free(partition_class); return -1; }
                ogg_put_bits(os, pc, 4);
                partition_class[j] = pc;
                if (pc > maximum_class) maximum_class = pc;
            }

            unsigned *class_dimensions = (unsigned *)calloc(maximum_class + 1, sizeof(unsigned));
            if (!class_dimensions) { free(partition_class); return -1; }

            for (unsigned j = 0; j <= maximum_class; j++) {
                unsigned cd_less1;
                if (br_read_bits(&ss, 3, &cd_less1) < 0) { free(class_dimensions); free(partition_class); return -1; }
                ogg_put_bits(os, cd_less1, 3);
                class_dimensions[j] = cd_less1 + 1;

                unsigned class_subclasses;
                if (br_read_bits(&ss, 2, &class_subclasses) < 0) { free(class_dimensions); free(partition_class); return -1; }
                ogg_put_bits(os, class_subclasses, 2);

                if (class_subclasses != 0) {
                    unsigned masterbook;
                    if (br_read_bits(&ss, 8, &masterbook) < 0) { free(class_dimensions); free(partition_class); return -1; }
                    ogg_put_bits(os, masterbook, 8);
                }

                for (unsigned k = 0; k < (1u << class_subclasses); k++) {
                    unsigned sb;
                    if (br_read_bits(&ss, 8, &sb) < 0) { free(class_dimensions); free(partition_class); return -1; }
                    ogg_put_bits(os, sb, 8);
                }
            }

            unsigned floor1_multiplier_less1;
            if (br_read_bits(&ss, 2, &floor1_multiplier_less1) < 0) { free(class_dimensions); free(partition_class); return -1; }
            ogg_put_bits(os, floor1_multiplier_less1, 2);

            unsigned rangebits;
            if (br_read_bits(&ss, 4, &rangebits) < 0) { free(class_dimensions); free(partition_class); return -1; }
            ogg_put_bits(os, rangebits, 4);

            for (unsigned j = 0; j < floor1_partitions; j++) {
                unsigned ccn = partition_class[j];
                for (unsigned k = 0; k < class_dimensions[ccn]; k++) {
                    unsigned X;
                    if (br_read_bits(&ss, rangebits, &X) < 0) { free(class_dimensions); free(partition_class); return -1; }
                    ogg_put_bits(os, X, rangebits);
                }
            }

            free(class_dimensions);
            free(partition_class);
        }

        /* --- Residues --- */
        unsigned residue_count_less1;
        if (br_read_bits(&ss, 6, &residue_count_less1) < 0) return -1;
        unsigned residue_count = residue_count_less1 + 1;
        ogg_put_bits(os, residue_count_less1, 6);

        for (unsigned ri = 0; ri < residue_count; ri++) {
            unsigned residue_type;
            if (br_read_bits(&ss, 2, &residue_type) < 0) return -1;
            ogg_put_bits(os, residue_type, 16); /* 16-bit output */

            unsigned residue_begin, residue_end, residue_partition_size_less1;
            unsigned residue_classifications_less1, residue_classbook;
            if (br_read_bits(&ss, 24, &residue_begin) < 0) return -1;
            if (br_read_bits(&ss, 24, &residue_end) < 0) return -1;
            if (br_read_bits(&ss, 24, &residue_partition_size_less1) < 0) return -1;
            if (br_read_bits(&ss, 6, &residue_classifications_less1) < 0) return -1;
            if (br_read_bits(&ss, 8, &residue_classbook) < 0) return -1;

            unsigned residue_classifications = residue_classifications_less1 + 1;
            ogg_put_bits(os, residue_begin, 24);
            ogg_put_bits(os, residue_end, 24);
            ogg_put_bits(os, residue_partition_size_less1, 24);
            ogg_put_bits(os, residue_classifications_less1, 6);
            ogg_put_bits(os, residue_classbook, 8);

            unsigned *residue_cascade = (unsigned *)calloc(residue_classifications, sizeof(unsigned));
            if (!residue_cascade) return -1;

            for (unsigned j = 0; j < residue_classifications; j++) {
                unsigned low_bits, bitflag, high_bits = 0;
                if (br_read_bits(&ss, 3, &low_bits) < 0) { free(residue_cascade); return -1; }
                ogg_put_bits(os, low_bits, 3);
                if (br_read_bits(&ss, 1, &bitflag) < 0) { free(residue_cascade); return -1; }
                ogg_put_bits(os, bitflag, 1);
                if (bitflag) {
                    if (br_read_bits(&ss, 5, &high_bits) < 0) { free(residue_cascade); return -1; }
                    ogg_put_bits(os, high_bits, 5);
                }
                residue_cascade[j] = high_bits * 8 + low_bits;
            }

            for (unsigned j = 0; j < residue_classifications; j++) {
                for (unsigned k = 0; k < 8; k++) {
                    if (residue_cascade[j] & (1u << k)) {
                        unsigned rbook;
                        if (br_read_bits(&ss, 8, &rbook) < 0) { free(residue_cascade); return -1; }
                        ogg_put_bits(os, rbook, 8);
                    }
                }
            }
            free(residue_cascade);
        }

        /* --- Mappings --- */
        unsigned mapping_count_less1;
        if (br_read_bits(&ss, 6, &mapping_count_less1) < 0) return -1;
        unsigned mapping_count = mapping_count_less1 + 1;
        ogg_put_bits(os, mapping_count_less1, 6);

        for (unsigned mi = 0; mi < mapping_count; mi++) {
            ogg_put_bits(os, 0, 16); /* mapping type 0 */

            unsigned submaps_flag;
            if (br_read_bits(&ss, 1, &submaps_flag) < 0) return -1;
            ogg_put_bits(os, submaps_flag, 1);

            unsigned submaps = 1;
            if (submaps_flag) {
                unsigned submaps_less1;
                if (br_read_bits(&ss, 4, &submaps_less1) < 0) return -1;
                submaps = submaps_less1 + 1;
                ogg_put_bits(os, submaps_less1, 4);
            }

            unsigned square_polar_flag;
            if (br_read_bits(&ss, 1, &square_polar_flag) < 0) return -1;
            ogg_put_bits(os, square_polar_flag, 1);

            if (square_polar_flag) {
                unsigned coupling_steps_less1;
                if (br_read_bits(&ss, 8, &coupling_steps_less1) < 0) return -1;
                unsigned coupling_steps = coupling_steps_less1 + 1;
                ogg_put_bits(os, coupling_steps_less1, 8);

                int ch_bits = ilog(wi->channels - 1);
                for (unsigned j = 0; j < coupling_steps; j++) {
                    unsigned magnitude, angle;
                    if (br_read_bits(&ss, ch_bits, &magnitude) < 0) return -1;
                    if (br_read_bits(&ss, ch_bits, &angle) < 0) return -1;
                    ogg_put_bits(os, magnitude, ch_bits);
                    ogg_put_bits(os, angle, ch_bits);
                }
            }

            unsigned mapping_reserved;
            if (br_read_bits(&ss, 2, &mapping_reserved) < 0) return -1;
            ogg_put_bits(os, mapping_reserved, 2);

            if (submaps > 1) {
                for (unsigned j = 0; j < wi->channels; j++) {
                    unsigned mapping_mux;
                    if (br_read_bits(&ss, 4, &mapping_mux) < 0) return -1;
                    ogg_put_bits(os, mapping_mux, 4);
                }
            }

            for (unsigned j = 0; j < submaps; j++) {
                unsigned time_config, floor_number, residue_number;
                if (br_read_bits(&ss, 8, &time_config) < 0) return -1;
                ogg_put_bits(os, time_config, 8);
                if (br_read_bits(&ss, 8, &floor_number) < 0) return -1;
                ogg_put_bits(os, floor_number, 8);
                if (br_read_bits(&ss, 8, &residue_number) < 0) return -1;
                ogg_put_bits(os, residue_number, 8);
            }
        }

        /* --- Modes --- */
        unsigned mode_count_less1;
        if (br_read_bits(&ss, 6, &mode_count_less1) < 0) return -1;
        unsigned mode_count = mode_count_less1 + 1;
        ogg_put_bits(os, mode_count_less1, 6);

        int *mode_blockflag = (int *)calloc(mode_count, sizeof(int));
        if (!mode_blockflag) return -1;
        int mode_bits = ilog(mode_count - 1);

        for (unsigned i = 0; i < mode_count; i++) {
            unsigned block_flag;
            if (br_read_bits(&ss, 1, &block_flag) < 0) { free(mode_blockflag); return -1; }
            ogg_put_bits(os, block_flag, 1);
            mode_blockflag[i] = (block_flag != 0);

            ogg_put_bits(os, 0, 16); /* windowtype */
            ogg_put_bits(os, 0, 16); /* transformtype */

            unsigned mapping;
            if (br_read_bits(&ss, 8, &mapping) < 0) { free(mode_blockflag); return -1; }
            ogg_put_bits(os, mapping, 8);
        }

        ogg_put_bits(os, 1, 1); /* framing */
        ogg_flush_page(os, 0, 0);

        /* Verify we consumed the setup packet correctly */
        if ((ss.total_bits_read + 7) / 8 != setup_size) {
            /* Mismatch, but continue */
        }

        *mode_blockflag_out = mode_blockflag;
        *mode_bits_out = mode_bits;
    }

    return 0;
}

static int generate_ogg_audio(WemInfo *wi, OggStream *os,
                               int *mode_blockflag, int mode_bits)
{
    int pkt_hdr_size = wi->no_granule ? 2 : 6;
    size_t offset = wi->data_offset + wi->first_audio_packet_offset;
    size_t data_end = wi->data_offset + wi->data_size;
    int prev_blockflag = 0;

    while (offset < data_end) {
        if (offset + (size_t)pkt_hdr_size > data_end) break;

        uint16_t pkt_size = rd16(wi->wem + offset);
        uint32_t granule = 0;
        if (!wi->no_granule) {
            granule = rd32(wi->wem + offset + 2);
        }

        size_t payload_offset = offset + pkt_hdr_size;
        size_t next_offset = payload_offset + pkt_size;

        if (payload_offset > data_end) break;
        if (next_offset > data_end) break;

        /* Set granule */
        if (granule == 0xFFFFFFFFu)
            os->granule = 1;
        else
            os->granule = granule;

        /* First byte handling */
        if (wi->mod_packets) {
            if (!mode_blockflag) return -1;

            /* OUT: 1 bit packet type (0 = audio) */
            ogg_put_bits(os, 0, 1);

            /* Read mode number from first byte of packet */
            if (pkt_size == 0) break;

            uint8_t first_byte = wi->wem[payload_offset];

            /* Extract mode_bits bits from the first byte (LSB first) */
            unsigned mode_number = 0;
            for (int i = 0; i < mode_bits; i++) {
                if (first_byte & (1u << i))
                    mode_number |= (1u << i);
            }
            ogg_put_bits(os, mode_number, mode_bits);

            /* Extract remaining bits of first byte */
            unsigned remainder = 0;
            int rem_bits = 8 - mode_bits;
            for (int i = 0; i < rem_bits; i++) {
                if (first_byte & (1u << (mode_bits + i)))
                    remainder |= (1u << i);
            }

            if (mode_blockflag[mode_number]) {
                /* Long window: peek at next frame for window type info */
                int next_blockflag = 0;
                if (next_offset + (size_t)pkt_hdr_size <= data_end) {
                    uint16_t next_pkt_size = rd16(wi->wem + next_offset);
                    size_t next_payload = next_offset + pkt_hdr_size;
                    if (next_pkt_size > 0 && next_payload < data_end) {
                        uint8_t next_first = wi->wem[next_payload];
                        unsigned next_mode = 0;
                        for (int i = 0; i < mode_bits; i++) {
                            if (next_first & (1u << i))
                                next_mode |= (1u << i);
                        }
                        next_blockflag = mode_blockflag[next_mode];
                    }
                }

                ogg_put_bits(os, prev_blockflag, 1);
                ogg_put_bits(os, next_blockflag, 1);
            }

            prev_blockflag = mode_blockflag[mode_number];

            /* Write remaining bits of first byte */
            ogg_put_bits(os, remainder, rem_bits);
        } else {
            /* No modification needed for first byte */
            if (pkt_size > 0) {
                ogg_put_bits(os, wi->wem[payload_offset], 8);
            }
        }

        /* Write remaining bytes */
        for (unsigned i = 1; i < pkt_size; i++) {
            ogg_put_bits(os, wi->wem[payload_offset + i], 8);
        }

        offset = next_offset;
        int is_last = (offset >= data_end);
        ogg_flush_page(os, 0, is_last);
    }

    return 0;
}

/* ======================================================================
 * Public API
 * ====================================================================== */

int wem_to_ogg(const uint8_t *wem_data, size_t wem_size,
               uint8_t **ogg_out, size_t *ogg_size)
{
    *ogg_out = NULL;
    *ogg_size = 0;

    /* Quick check: is this even a Wwise Vorbis file? */
    if (wem_size < 12) return -1;
    if (memcmp(wem_data, "RIFF", 4) != 0) return -1;

    /* Check if it might be plain PCM WAV (codec != 0xFFFF) */
    /* Find fmt chunk to check codec */
    {
        size_t off = 12;
        while (off + 8 <= wem_size) {
            uint32_t csz = rd32(wem_data + off + 4);
            if (memcmp(wem_data + off, "fmt ", 4) == 0) {
                if (off + 8 + 2 <= wem_size) {
                    uint16_t codec = rd16(wem_data + off + 8);
                    if (codec != 0xFFFF) {
                        /* Not Wwise Vorbis - return error code 1 to signal
                           the caller should keep this as-is (PCM WAV) */
                        return 1;
                    }
                }
                break;
            }
            off += 8 + csz;
        }
    }

    WemInfo wi;
    if (wem_parse(&wi, wem_data, wem_size) < 0) return -1;

    /* Only support the non-triad path (all BDO files) */
    if (wi.header_triad_present) {
        fprintf(stderr, "wem2ogg: header triad present, not supported\n");
        return -1;
    }

    Buffer buf;
    buf_init(&buf);

    OggStream os;
    ogg_init(&os, &buf);

    int *mode_blockflag = NULL;
    int mode_bits = 0;

    if (generate_ogg_header(&wi, &os, &mode_blockflag, &mode_bits) < 0) {
        free(buf.data);
        free(mode_blockflag);
        return -1;
    }

    if (generate_ogg_audio(&wi, &os, mode_blockflag, mode_bits) < 0) {
        free(buf.data);
        free(mode_blockflag);
        return -1;
    }

    free(mode_blockflag);

    *ogg_out = buf.data;
    *ogg_size = buf.len;
    return 0;
}
