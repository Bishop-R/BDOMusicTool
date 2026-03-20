#ifndef MUSE_ICE_H
#define MUSE_ICE_H

#include <stddef.h>
#include <stdint.h>

void    ice_init(void);
void    ice_encrypt(uint8_t *data, size_t len);
void    ice_decrypt(uint8_t *data, size_t len);
int     ice_decrypt_owner_header(const uint8_t *ct, size_t len,
                                 uint8_t *out, size_t out_cap);
int     ice_decrypt_full(const uint8_t *ct, size_t len, uint32_t owner_id,
                         uint8_t *out, size_t out_cap);

#endif /* MUSE_ICE_H */
