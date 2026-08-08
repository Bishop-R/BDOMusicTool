#include "model.h"
#include "composition_tools.h"
#include "bdo_validate.h"
#include "midi_export.h"
#include "midi_import.h"
#include "bdo_format.h"
#include "undo.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);return 1;}}while(0)
static MuseNote note(int p,int v,double s,double d,int selected){MuseNote n={(uint8_t)p,(uint8_t)v,0,(uint8_t)selected,s,d};return n;}
int main(int argc,char**argv){(void)argc;(void)argv;MuseProject p;muse_project_init(&p);CHECK(muse_project_add_layer(&p,0x00)>=0);NoteArray*a=&p.layers[0].sublayers[0];
 note_array_push(a,note(60,100,117,220,1));note_array_push(a,note(64,100,121,220,1));note_array_push(a,note(67,100,119,220,1));
 CHECK(notes_quantize(a,500,4,100,50,false)==3);CHECK(a->notes[0].start==125);
 CHECK(notes_strum(a,20,5,0,1,true)==3);CHECK(a->notes[1].start==145);CHECK(a->notes[2].start==165);
 CHECK(notes_humanize(a,0,0,0,1)==3);CHECK(notes_arpeggiate(a,500,4,1,80)>0);
 BdoValidation v=bdo_validate_project(&p);CHECK(v.errors==0);
 a->notes[0].pitch=1;v=bdo_validate_project(&p);CHECK(v.out_of_range==1);a->notes[0].pitch=60;
 const char*mid="core_test.mid";CHECK(midi_export(mid,&p)==0);FILE*f=fopen(mid,"rb");CHECK(f);char h[4];CHECK(fread(h,1,4,f)==4);CHECK(memcmp(h,"MThd",4)==0);fclose(f);remove(mid);
 CHECK(midi_export(mid,&p)==0);MidiImportData*mi=midi_parse(mid);CHECK(mi);CHECK(mi->bpm==p.bpm);MuseProject imported;muse_project_init(&imported);midi_apply(mi,&imported);CHECK(imported.num_layers>0);CHECK(muse_layer_note_count(&imported.layers[0])>0);midi_import_data_free(mi);muse_project_free(&imported);remove(mid);
 {static const unsigned char multi[]={0x4d,0x54,0x68,0x64,0,0,0,6,0,0,0,1,1,0xe0,0x4d,0x54,0x72,0x6b,0,0,0,0x24,0,0xff,0x51,3,7,0xa1,0x20,0,0x90,0x3c,0x64,0x83,0x60,0x80,0x3c,0,0,0xff,0x51,3,0x0f,0x42,0x40,0,0x90,0x40,0x64,0x83,0x60,0x80,0x40,0,0,0xff,0x2f,0};FILE*mf=fopen(mid,"wb");CHECK(mf);CHECK(fwrite(multi,1,sizeof(multi),mf)==sizeof(multi));fclose(mf);mi=midi_parse(mid);CHECK(mi&&mi->tempo_changes==2);muse_project_init(&imported);midi_apply(mi,&imported);CHECK(imported.num_tempo_points==2);CHECK(fabs(imported.tempo_points[0].bpm-120)<.1);CHECK(fabs(imported.tempo_points[1].bpm-60)<.1);CHECK(fabs(imported.tempo_points[1].beat-1)<.001);CHECK(imported.layers[0].sublayers[0].notes[1].start>499&&imported.layers[0].sublayers[0].notes[1].start<501);CHECK(imported.layers[0].sublayers[0].notes[1].dur>999&&imported.layers[0].sublayers[0].notes[1].dur<1001);midi_import_data_free(mi);muse_project_free(&imported);remove(mid);}
 p.owner_id=1234;snprintf(p.char_name,sizeof(p.char_name),"Tester");const char*bdo="core_test.bdo";CHECK(bdo_save(bdo,&p)==0);MuseProject loaded;muse_project_init(&loaded);CHECK(bdo_load(bdo,"Tester",&loaded)==0);CHECK(loaded.owner_id==1234);CHECK(muse_layer_note_count(&loaded.layers[0])==muse_layer_note_count(&p.layers[0]));muse_project_free(&loaded);remove(bdo);
 MuseProject tp;muse_project_init(&tp);muse_project_add_layer(&tp,0);note_array_push(&tp.layers[0].sublayers[0],note(60,100,2000,500,0));CHECK(muse_project_set_tempo_at_beat(&tp,4,60));CHECK(fabs(muse_project_beat_to_ms(&tp,6)-4000)<0.001);CHECK(fabs(muse_project_ms_to_beat(&tp,4000)-6)<0.001);CHECK(fabs(tp.layers[0].sublayers[0].notes[0].start-2000)<0.001);CHECK(muse_project_set_tempo_at_beat(&tp,2,60));CHECK(fabs(tp.layers[0].sublayers[0].notes[0].start-3000)<0.001);CHECK(fabs(tp.layers[0].sublayers[0].notes[0].dur-1000)<0.001);tp.owner_id=1;snprintf(tp.char_name,sizeof(tp.char_name),"Tempo");CHECK(bdo_save(bdo,&tp)==0);muse_project_init(&loaded);CHECK(bdo_load(bdo,"Tempo",&loaded)==0);CHECK(fabs(loaded.layers[0].sublayers[0].notes[0].start-3000)<0.001);CHECK(fabs(loaded.layers[0].sublayers[0].notes[0].dur-1000)<0.001);CHECK(loaded.num_tempo_points==0);muse_project_free(&loaded);remove(bdo);CHECK(muse_project_remove_tempo_at_beat(&tp,2,0.01));CHECK(fabs(tp.layers[0].sublayers[0].notes[0].start-2000)<0.001);muse_project_free(&tp);
 MuseProject ramp;muse_project_init(&ramp);CHECK(muse_project_set_tempo_at_beat(&ramp,0,120));CHECK(muse_project_set_tempo_at_beat(&ramp,4,60));CHECK(muse_project_set_tempo_ramp(&ramp,0,true));double ramp_end=muse_project_beat_to_ms(&ramp,4);CHECK(ramp_end>2000&&ramp_end<4000);CHECK(fabs(muse_project_ms_to_beat(&ramp,ramp_end)-4)<0.001);CHECK(muse_project_set_tempo_curve(&ramp,0,.5f));CHECK(fabs(muse_project_ms_to_beat(&ramp,muse_project_beat_to_ms(&ramp,2))-2)<0.001);muse_project_free(&ramp);
 MuseProject cap;muse_project_init(&cap);cap.bpm=300;cap.owner_id=1;snprintf(cap.char_name,sizeof(cap.char_name),"Cap");muse_project_add_layer(&cap,0);CHECK(bdo_save(bdo,&cap)==0);muse_project_init(&loaded);CHECK(bdo_load(bdo,"Cap",&loaded)==0);CHECK(loaded.bpm==200);muse_project_free(&loaded);muse_project_free(&cap);remove(bdo);
 MuseProject over;muse_project_init(&over);over.owner_id=1;snprintf(over.char_name,sizeof(over.char_name),"Over");muse_project_add_layer(&over,0);for(int i=0;i<10002;i++)note_array_push(&over.layers[0].sublayers[0],note(60,100,(double)i,10,0));v=bdo_validate_project(&over);CHECK(v.errors==0&&v.truncated_notes==2);CHECK(bdo_save(bdo,&over)==0);muse_project_init(&loaded);CHECK(bdo_load(bdo,"Over",&loaded)==0);CHECK(muse_layer_note_count(&loaded.layers[0])==10000);CHECK(loaded.layers[0].sublayers[0].notes[9999].start==9999);muse_project_free(&loaded);muse_project_free(&over);remove(bdo);
 undo_init();undo_push(&p);int old=p.layers[0].sublayers[0].notes[0].pitch;p.layers[0].sublayers[0].notes[0].pitch=72;CHECK(undo_pop(&p)==0);CHECK(p.layers[0].sublayers[0].notes[0].pitch==old);CHECK(redo_pop(&p)==0);CHECK(p.layers[0].sublayers[0].notes[0].pitch==72);undo_free();
 muse_project_free(&p);puts("core tests passed");return 0;}
