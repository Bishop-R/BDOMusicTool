#include "bdo_validate.h"
#include "instruments.h"
#include <stdio.h>
BdoValidation bdo_validate_project(const MuseProject*p){BdoValidation r={0};if(!p){r.errors=1;return r;}
 if(p->bpm<1||p->bpm>200)r.errors++;if(p->time_sig<2||p->time_sig>12)r.errors++;if(p->owner_id==0)r.warnings++;
 for(int l=0;l<p->num_layers;l++){const MuseLayer*ly=&p->layers[l];const MuseInstrument*in=inst_by_id(ly->inst_id);if(!in){r.errors++;continue;}int count=0;
  for(int s=0;s<ly->num_sublayers;s++)for(int n=0;n<ly->sublayers[s].count;n++){const MuseNote*x=&ly->sublayers[s].notes[n];count++;
   if(x->pitch<in->pitch_lo||x->pitch>in->pitch_hi){r.out_of_range++;r.errors++;}if(!inst_has_technique(ly->inst_id,x->ntype)){r.bad_techniques++;r.errors++;}
   if(x->dur<=0||x->start<0||x->vel==0||x->vel>127){r.invalid_notes++;r.errors++;}}
  if(count>MAX_NOTES_PER_INSTRUMENT){r.truncated_notes+=count-MAX_NOTES_PER_INSTRUMENT;r.warnings++;}}
 return r;}
int bdo_validation_message(const MuseProject*p,char*out,int cap){BdoValidation r=bdo_validate_project(p);return snprintf(out,(size_t)cap,
 "BDO validation: %d error(s), %d warning(s)\nOut of range: %d\nUnsupported techniques: %d\nInvalid notes: %d\nNotes omitted after the 10,000-per-instrument export limit: %d",
 r.errors,r.warnings,r.out_of_range,r.bad_techniques,r.invalid_notes,r.truncated_notes);}
