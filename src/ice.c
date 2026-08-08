#include "ice.h"
#include <string.h>

static const uint8_t KA[8] = { 0xA3, 0x71, 0x8F, 0x92, 0x46, 0xB2, 0xC8, 0x55 };
static const uint8_t KB[8] = { 0xF2, 0x82, 0x80, 0x83, 0x42, 0x96, 0xA2, 0x55 };

static const int SMOD[4][4] = {
    { 333, 313, 505, 369 },
    { 379, 375, 319, 391 },
    { 361, 445, 451, 397 },
    { 397, 425, 395, 505 },
};
static const int SXOR[4][4] = {
    { 0x83, 0x85, 0x9B, 0xCD },
    { 0xCC, 0xA7, 0xAD, 0x41 },
    { 0x4B, 0x2E, 0xD4, 0x33 },
    { 0xEA, 0xCB, 0x2E, 0x04 },
};
static const uint32_t PBOX[32] = {
    0x00000001, 0x00000080, 0x00000400, 0x00002000,
    0x00080000, 0x00200000, 0x01000000, 0x40000000,
    0x00000008, 0x00000020, 0x00000100, 0x00004000,
    0x00010000, 0x00800000, 0x04000000, 0x20000000,
    0x00000004, 0x00000010, 0x00000200, 0x00008000,
    0x00020000, 0x00400000, 0x08000000, 0x10000000,
    0x00000002, 0x00000040, 0x00000800, 0x00001000,
    0x00040000, 0x00100000, 0x02000000, 0x80000000,
};
static const int KEYROT[16] = { 0,1,2,3, 2,1,3,0, 1,3,2,0, 3,1,0,2 };

#define MAX_OWNER_PAYLOAD 512

static uint32_t sbox[4][1024];
static uint32_t ks[8][3];
static int      ice_ready;

static uint32_t perm32(uint32_t x) {
    uint32_t result = 0;
    int i = 0;
    while (x) {
        if (x & 1) result |= PBOX[i];
        i++;
        x >>= 1;
    }
    return result;
}

static uint32_t gf_mult(uint32_t a, uint32_t b, uint32_t m) {
    uint32_t result = 0;
    while (b) {
        if (b & 1) result ^= a;
        a <<= 1;
        b >>= 1;
        if (a >= 256) a ^= m;
    }
    return result;
}

static uint32_t gf_exp7(uint32_t b, uint32_t m) {
    if (b == 0) return 0;
    uint32_t x = gf_mult(b, b, m);
    x = gf_mult(b, x, m);
    x = gf_mult(x, x, m);
    return gf_mult(b, x, m);
}

static void init_sbox(void) {
    for (int i = 0; i < 1024; i++) {
        int col = (i >> 1) & 0xFF;
        int row = (i & 1) | ((i & 0x200) >> 8);
        sbox[0][i] = perm32(gf_exp7(col ^ SXOR[0][row], SMOD[0][row]) << 24);
        sbox[1][i] = perm32(gf_exp7(col ^ SXOR[1][row], SMOD[1][row]) << 16);
        sbox[2][i] = perm32(gf_exp7(col ^ SXOR[2][row], SMOD[2][row]) << 8);
        sbox[3][i] = perm32(gf_exp7(col ^ SXOR[3][row], SMOD[3][row]));
    }
}

static void build_key_schedule(void) {
    uint8_t key[8];
    for (int i = 0; i < 8; i++) key[i] = KA[i] ^ KB[i];

    uint16_t kb[4] = {0};
    for (int i = 0; i < 4; i++)
        kb[3 - i] = ((uint16_t)key[i * 2] << 8) | key[i * 2 + 1];

    memset(ks, 0, sizeof(ks));
    for (int i = 0; i < 8; i++) {
        int kr = KEYROT[i];
        for (int j = 0; j < 15; j++) {
            for (int k = 0; k < 4; k++) {
                int t = (kr + k) & 3;
                uint16_t kbb = kb[t];
                int bit = kbb & 1;
                ks[i][j % 3] = (ks[i][j % 3] << 1) | bit;
                kb[t] = (kbb >> 1) | (uint16_t)((bit ^ 1) << 15);
            }
        }
    }
}

static uint32_t ice_f(uint32_t p, const uint32_t sk[3]) {
    uint32_t tl = ((p >> 16) & 0x3FF) | (((p >> 14) | (p << 18)) & 0xFFC00);
    uint32_t tr = (p & 0x3FF) | ((p << 2) & 0xFFC00);
    uint32_t al = sk[2] & (tl ^ tr);
    uint32_t ar = al ^ tr ^ sk[1];
    al ^= tl ^ sk[0];
    return sbox[0][al >> 10] | sbox[1][al & 0x3FF] |
           sbox[2][ar >> 10] | sbox[3][ar & 0x3FF];
}

static void encrypt_block(uint8_t *data) {
    uint32_t l = 0, r = 0;
    for (int i = 0; i < 4; i++) {
        int t = 24 - i * 8;
        l |= (uint32_t)data[i]     << t;
        r |= (uint32_t)data[i + 4] << t;
    }
    for (int i = 0; i < 8; i += 2) {
        l ^= ice_f(r, ks[i]);
        r ^= ice_f(l, ks[i + 1]);
    }
    for (int i = 3; i >= 0; i--) {
        data[3 - i] = (uint8_t)(r >> (i * 8));
        data[7 - i] = (uint8_t)(l >> (i * 8));
    }
}

static void decrypt_block(uint8_t *data) {
    uint32_t l = 0, r = 0;
    for (int i = 0; i < 4; i++) {
        int t = 24 - i * 8;
        l |= (uint32_t)data[i]     << t;
        r |= (uint32_t)data[i + 4] << t;
    }
    for (int i = 7; i > 0; i -= 2) {
        l ^= ice_f(r, ks[i]);
        r ^= ice_f(l, ks[i - 1]);
    }
    for (int i = 3; i >= 0; i--) {
        data[3 - i] = (uint8_t)(r >> (i * 8));
        data[7 - i] = (uint8_t)(l >> (i * 8));
    }
}

void ice_init(void) {
    if (ice_ready) return;
    init_sbox();
    build_key_schedule();
    ice_ready = 1;
}

void ice_encrypt(uint8_t *data, size_t len) {
    ice_init();
    size_t off = 0;
    while (off + 8 <= len) {
        encrypt_block(data + off);
        off += 8;
    }
}

void ice_decrypt(uint8_t *data, size_t len) {
    ice_init();
    size_t off = 0;
    while (off + 8 <= len) {
        decrypt_block(data + off);
        off += 8;
    }
}

int ice_decrypt_owner_header(const uint8_t *ct, size_t len,
                             uint8_t *out, size_t out_cap) {
    if (len < 8) return -1;
    if (len > MAX_OWNER_PAYLOAD) return -2;
    if (out_cap < len) return -1;
    ice_init();
    memcpy(out, ct, len);
    ice_decrypt(out, len);
    return 0;
}

int ice_decrypt_full(const uint8_t *ct, size_t len, uint32_t owner_id,
                     uint8_t *out, size_t out_cap) {
    if (len < 8 || out_cap < len) return -1;
    ice_init();

    uint8_t first[8];
    memcpy(first, ct, 8);
    decrypt_block(first);

    (void)owner_id;

    memcpy(out, first, 8);
    if (len > 8) {
        memcpy(out + 8, ct + 8, len - 8);
        ice_decrypt(out + 8, len - 8);
    }
    return 0;
}
