#ifndef BDO_VALIDATE_H
#define BDO_VALIDATE_H
#include "model.h"
typedef struct { int errors,warnings,out_of_range,bad_techniques,invalid_notes,truncated_notes; } BdoValidation;
BdoValidation bdo_validate_project(const MuseProject *p);
int bdo_validation_message(const MuseProject *p,char *out,int cap);
#endif
