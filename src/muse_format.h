#ifndef MUSE_FORMAT_H
#define MUSE_FORMAT_H

/* .composer project file - our own JSON-based format */

struct MuseApp;  /* forward declaration */

int muse_save(const char *path, const struct MuseApp *app);
int muse_load(const char *path, struct MuseApp *app);

#endif /* MUSE_FORMAT_H */
