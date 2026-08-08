#ifndef MUSE_UNDO_H
#define MUSE_UNDO_H

#include "model.h"

void undo_init(void);
void undo_free(void);
void undo_clear(void);   /* clear all undo/redo history */
void undo_push(const MuseProject *p);
int  undo_pop(MuseProject *p);   /* returns 0 on success, -1 if empty */
int  redo_pop(MuseProject *p);

#endif /* MUSE_UNDO_H */
