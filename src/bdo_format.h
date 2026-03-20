#ifndef MUSE_BDO_FORMAT_H
#define MUSE_BDO_FORMAT_H

#include "model.h"
#include <stddef.h>
#include <stdint.h>

#define BDO_VERSION       9
#define BDO_HEADER_SIZE   0x150
#define BDO_NOTE_SIZE     20
#define BDO_NOTE_MIN      24
#define BDO_NOTE_MAX      108
#define BDO_MAX_NOTES_PER_TRACK 730
#define BDO_NAME_FIELD    62

int  bdo_load(const char *path, const char *linked_name, MuseProject *out);
int  bdo_save(const char *path, const MuseProject *proj);

/* quick peek at owner info without parsing the whole file */
int  bdo_extract_owner(const char *path, uint32_t *owner_id_out, char *name_out, int name_sz);

#endif /* MUSE_BDO_FORMAT_H */
