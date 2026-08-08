#include "model.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* note array operations */

void note_array_init(NoteArray *a) {
    a->notes    = NULL;
    a->count    = 0;
    a->capacity = 0;
}

void note_array_free(NoteArray *a) {
    free(a->notes);
    a->notes    = NULL;
    a->count    = 0;
    a->capacity = 0;
}

void note_array_push(NoteArray *a, MuseNote n) {
    if (a->count >= a->capacity) {
        int cap = a->capacity ? a->capacity * 2 : 64;
        a->notes = realloc(a->notes, (size_t)cap * sizeof(MuseNote));
        a->capacity = cap;
    }
    a->notes[a->count++] = n;
}

void note_array_remove(NoteArray *a, int idx) {
    if (idx < 0 || idx >= a->count) return;
    memmove(&a->notes[idx], &a->notes[idx + 1],
            (size_t)(a->count - idx - 1) * sizeof(MuseNote));
    a->count--;
}

void note_array_clear(NoteArray *a) {
    a->count = 0;
}

int muse_layer_note_count(const MuseLayer *ly) {
    int total = 0;
    for (int s = 0; s < ly->num_sublayers; s++)
        total += ly->sublayers[s].count;
    return total;
}

static int cmp_note_start(const void *a, const void *b) {
    double da = ((const MuseNote *)a)->start;
    double db = ((const MuseNote *)b)->start;
    return (da > db) - (da < db);
}

double muse_layer_exceed_ms(const MuseLayer *ly) {
    int total = muse_layer_note_count(ly);
    if (total <= MAX_NOTES_PER_INSTRUMENT) return -1.0;

    /* grab all notes, sort them, find where the 10001st one starts */
    MuseNote *all = malloc((size_t)total * sizeof(MuseNote));
    if (!all) return -1.0;
    int idx = 0;
    for (int s = 0; s < ly->num_sublayers; s++)
        for (int n = 0; n < ly->sublayers[s].count; n++)
            all[idx++] = ly->sublayers[s].notes[n];
    qsort(all, (size_t)total, sizeof(MuseNote), cmp_note_start);
    double ms = all[MAX_NOTES_PER_INSTRUMENT].start;
    free(all);
    return ms;
}

/* project management */

void muse_project_init(MuseProject *p) {
    memset(p, 0, sizeof(*p));
    p->bpm      = 120;
    p->time_sig = 4;
}

static double valid_bpm(double bpm) { return bpm >= 20.0 && bpm <= 400.0 ? bpm : 120.0; }

/* Tempo conversion is on the render hot path (note X positions, velocity
   graph, playhead).  Cache a monotonic piecewise lookup for several project
   snapshots so live tempo editing and retiming can query both the old and new
   maps without repeatedly integrating and binary-solving every note. */
#define TEMPO_LUT_STEPS 128
#define TEMPO_CACHE_SLOTS 4
typedef struct {
    const MuseProject *key;
    uint64_t hash, age;
    int nseg;
    double beat0[MAX_TEMPO_POINTS], beat1[MAX_TEMPO_POINTS];
    double ms0[MAX_TEMPO_POINTS], ms1[MAX_TEMPO_POINTS];
    double lut[MAX_TEMPO_POINTS][TEMPO_LUT_STEPS + 1];
    double tail_beat, tail_ms, tail_bpm;
} TempoCache;
static TempoCache g_tempo_cache[TEMPO_CACHE_SLOTS];
static uint64_t g_tempo_cache_age;

static uint64_t tempo_hash(const MuseProject *p) {
    uint64_t h = 1469598103934665603ULL;
    const unsigned char *b = (const unsigned char *)&p->bpm;
    for (size_t i=0;i<sizeof(p->bpm);i++) { h ^= b[i]; h *= 1099511628211ULL; }
    h ^= (uint64_t)p->num_tempo_points; h *= 1099511628211ULL;
    b = (const unsigned char *)p->tempo_points;
    size_t n=(size_t)p->num_tempo_points*sizeof(p->tempo_points[0]);
    for(size_t i=0;i<n;i++){h^=b[i];h*=1099511628211ULL;}
    return h;
}
static double tempo_shape(double u,float curve){
    if(curve>0)return 1.0-pow(1.0-u,1.0+4.0*curve);
    if(curve<0)return pow(u,1.0-4.0*curve);
    return u;
}
static void tempo_cache_build(TempoCache*c,const MuseProject*p,uint64_t hash){
    memset(c,0,sizeof(*c));c->key=p;c->hash=hash;c->age=++g_tempo_cache_age;
    double cursor=0,ms=0,bpm=valid_bpm(p->bpm);bool ramp=false;float curve=0;
    for(int i=0;i<p->num_tempo_points;i++){
        double boundary=p->tempo_points[i].beat,next=valid_bpm(p->tempo_points[i].bpm),span=boundary-cursor;
        if(span>1e-9&&c->nseg<MAX_TEMPO_POINTS){int s=c->nseg++;c->beat0[s]=cursor;c->beat1[s]=boundary;c->ms0[s]=ms;c->lut[s][0]=0;
            double acc=0;
            for(int k=0;k<TEMPO_LUT_STEPS;k++){double u=(k+.5)/TEMPO_LUT_STEPS,sh=tempo_shape(u,curve);double tb=ramp?bpm+(next-bpm)*sh:bpm;acc+=(span/TEMPO_LUT_STEPS)*60000.0/tb;c->lut[s][k+1]=acc;}
            ms+=acc;c->ms1[s]=ms;cursor=boundary;
        }
        bpm=next;ramp=p->tempo_points[i].ramp_to_next!=0;curve=p->tempo_points[i].curve_to_next;
    }
    c->tail_beat=cursor;c->tail_ms=ms;c->tail_bpm=bpm;
}
static TempoCache*tempo_cache_get(const MuseProject*p){
    uint64_t h=tempo_hash(p);int victim=0;
    for(int i=0;i<TEMPO_CACHE_SLOTS;i++){if(g_tempo_cache[i].key==p&&g_tempo_cache[i].hash==h){g_tempo_cache[i].age=++g_tempo_cache_age;return &g_tempo_cache[i];}if(g_tempo_cache[i].age<g_tempo_cache[victim].age)victim=i;}
    tempo_cache_build(&g_tempo_cache[victim],p,h);return &g_tempo_cache[victim];
}

float muse_project_tempo_at_beat(const MuseProject *p, double beat) {
    double bpm = valid_bpm(p ? p->bpm : 120);
    if (!p) return (float)bpm;
    for (int i = 0; i < p->num_tempo_points; i++) {
        if (p->tempo_points[i].beat > beat + 1e-9) break;
        bpm = valid_bpm(p->tempo_points[i].bpm);
    }
    return (float)bpm;
}

double muse_project_beat_to_ms(const MuseProject *p, double beat) {
    if (!p || beat <= 0.0) return 0.0;
    TempoCache*c=tempo_cache_get(p);
    for(int s=0;s<c->nseg;s++)if(beat<=c->beat1[s]){double f=(beat-c->beat0[s])/(c->beat1[s]-c->beat0[s])*TEMPO_LUT_STEPS;if(f<0)f=0;if(f>TEMPO_LUT_STEPS)f=TEMPO_LUT_STEPS;int k=(int)f;if(k>=TEMPO_LUT_STEPS)return c->ms1[s];double q=f-k;return c->ms0[s]+c->lut[s][k]+(c->lut[s][k+1]-c->lut[s][k])*q;}
    return c->tail_ms+(beat-c->tail_beat)*60000.0/c->tail_bpm;
}

double muse_project_ms_to_beat(const MuseProject *p, double target) {
    if (!p || target <= 0.0) return 0.0;
    TempoCache*c=tempo_cache_get(p);
    for(int s=0;s<c->nseg;s++)if(target<=c->ms1[s]){double local=target-c->ms0[s];int lo=0,hi=TEMPO_LUT_STEPS;while(lo+1<hi){int mid=(lo+hi)/2;if(c->lut[s][mid]<local)lo=mid;else hi=mid;}double den=c->lut[s][lo+1]-c->lut[s][lo],q=den>1e-12?(local-c->lut[s][lo])/den:0;return c->beat0[s]+(c->beat1[s]-c->beat0[s])*(lo+q)/TEMPO_LUT_STEPS;}
    return c->tail_beat+(target-c->tail_ms)*c->tail_bpm/60000.0;
}

static void retime_content(MuseProject *p, const MuseProject *old) {
    for (int li=0; li<p->num_layers; li++) for (int si=0; si<p->layers[li].num_sublayers; si++) {
        NoteArray *a=&p->layers[li].sublayers[si];
        for (int ni=0; ni<a->count; ni++) {
            double b0=muse_project_ms_to_beat(old,a->notes[ni].start);
            double b1=muse_project_ms_to_beat(old,a->notes[ni].start+a->notes[ni].dur);
            a->notes[ni].start=muse_project_beat_to_ms(p,b0);
            a->notes[ni].dur=muse_project_beat_to_ms(p,b1)-a->notes[ni].start;
        }
    }
    for(int i=0;i<p->num_markers;i++) p->markers[i].time_ms=muse_project_beat_to_ms(p,muse_project_ms_to_beat(old,p->markers[i].time_ms));
}

bool muse_project_set_tempo_at_beat(MuseProject *p, double beat, float bpm) {
    if(!p || beat<0 || bpm<20 || bpm>400) return false;
    MuseProject old=*p;
    int at=0; while(at<p->num_tempo_points && p->tempo_points[at].beat<beat-1e-6) at++;
    if(at<p->num_tempo_points && fabs(p->tempo_points[at].beat-beat)<1e-6) p->tempo_points[at].bpm=bpm;
    else { if(p->num_tempo_points>=MAX_TEMPO_POINTS)return false; memmove(&p->tempo_points[at+1],&p->tempo_points[at],(size_t)(p->num_tempo_points-at)*sizeof(p->tempo_points[0])); p->tempo_points[at]=(MuseTempoPoint){.beat=beat,.bpm=bpm}; p->num_tempo_points++; }
    retime_content(p,&old); p->dirty=true; return true;
}

bool muse_project_remove_tempo_at_beat(MuseProject *p, double beat, double tolerance) {
    if(!p)return false; int at=-1; double best=tolerance;
    for(int i=0;i<p->num_tempo_points;i++){double d=fabs(p->tempo_points[i].beat-beat);if(d<=best){best=d;at=i;}}
    if(at<0)return false; MuseProject old=*p; memmove(&p->tempo_points[at],&p->tempo_points[at+1],(size_t)(p->num_tempo_points-at-1)*sizeof(p->tempo_points[0]));p->num_tempo_points--;retime_content(p,&old);p->dirty=true;return true;
}

bool muse_project_set_base_bpm(MuseProject *p,uint16_t bpm){if(!p||bpm<20||bpm>200)return false;if(p->bpm==bpm)return true;MuseProject old=*p;p->bpm=bpm;retime_content(p,&old);p->dirty=true;return true;}
int muse_project_move_tempo_point(MuseProject*p,int index,double beat,float bpm){if(!p||index<0||index>=p->num_tempo_points||beat<0||bpm<20||bpm>400)return -1;MuseProject old=*p;MuseTempoPoint t=p->tempo_points[index];t.beat=beat;t.bpm=bpm;memmove(&p->tempo_points[index],&p->tempo_points[index+1],(size_t)(p->num_tempo_points-index-1)*sizeof(t));int at=0;while(at<p->num_tempo_points-1&&p->tempo_points[at].beat<beat)at++;memmove(&p->tempo_points[at+1],&p->tempo_points[at],(size_t)(p->num_tempo_points-1-at)*sizeof(t));p->tempo_points[at]=t;retime_content(p,&old);p->dirty=true;return at;}
int muse_project_move_tempo_point_raw(MuseProject*p,int index,double beat,float bpm){if(!p||index<0||index>=p->num_tempo_points||beat<0||bpm<20||bpm>400)return -1;MuseTempoPoint t=p->tempo_points[index];t.beat=beat;t.bpm=bpm;memmove(&p->tempo_points[index],&p->tempo_points[index+1],(size_t)(p->num_tempo_points-index-1)*sizeof(t));int at=0;while(at<p->num_tempo_points-1&&p->tempo_points[at].beat<beat)at++;memmove(&p->tempo_points[at+1],&p->tempo_points[at],(size_t)(p->num_tempo_points-1-at)*sizeof(t));p->tempo_points[at]=t;p->dirty=true;return at;}
bool muse_project_set_tempo_ramp(MuseProject*p,int i,bool e){if(!p||i<0||i>=p->num_tempo_points)return false;MuseProject old=*p;p->tempo_points[i].ramp_to_next=e;retime_content(p,&old);p->dirty=true;return true;}
bool muse_project_set_tempo_curve(MuseProject*p,int i,float c){if(!p||i<0||i>=p->num_tempo_points)return false;if(c<-1)c=-1;if(c>1)c=1;MuseProject old=*p;p->tempo_points[i].curve_to_next=c;retime_content(p,&old);p->dirty=true;return true;}
void muse_project_retime_from(MuseProject*p,const MuseProject*old_tempo){if(p&&old_tempo)retime_content(p,old_tempo);}

void muse_project_free(MuseProject *p) {
    for (int i = 0; i < p->num_layers; i++) {
        MuseLayer *ly = &p->layers[i];
        for (int s = 0; s < ly->num_sublayers; s++)
            note_array_free(&ly->sublayers[s]);
        free(ly->sublayers);
    }
    free(p->layers);
    memset(p, 0, sizeof(*p));
}

int muse_project_add_layer(MuseProject *p, uint8_t inst_id) {
    int idx = p->num_layers;
    p->layers = realloc(p->layers, (size_t)(idx + 1) * sizeof(MuseLayer));
    MuseLayer *ly = &p->layers[idx];
    memset(ly, 0, sizeof(*ly));
    ly->inst_id       = inst_id;
    ly->volume         = 70;   /* 0x46, what BDO defaults to */
    /* marnian synths get different default profiles */
    if (inst_id == 0x14 || inst_id == 0x18)
        ly->synth_profile = 2;  /* Super */
    else if (inst_id == 0x1C || inst_id == 0x20)
        ly->synth_profile = 1;  /* Stereo */
    ly->num_sublayers  = 1;
    ly->sublayers      = calloc(1, sizeof(NoteArray));
    note_array_init(&ly->sublayers[0]);
    p->num_layers++;
    return idx;
}

void muse_project_remove_layer(MuseProject *p, int idx) {
    if (idx < 0 || idx >= p->num_layers) return;
    MuseLayer *ly = &p->layers[idx];
    for (int s = 0; s < ly->num_sublayers; s++)
        note_array_free(&ly->sublayers[s]);
    free(ly->sublayers);
    memmove(&p->layers[idx], &p->layers[idx + 1],
            (size_t)(p->num_layers - idx - 1) * sizeof(MuseLayer));
    p->num_layers--;
    if (p->active_layer >= p->num_layers && p->num_layers > 0)
        p->active_layer = p->num_layers - 1;
}

int muse_layer_add_sublayer(MuseLayer *ly) {
    if (ly->num_sublayers >= 16) return -1;
    int idx = ly->num_sublayers;
    ly->sublayers = realloc(ly->sublayers, (size_t)(idx + 1) * sizeof(NoteArray));
    note_array_init(&ly->sublayers[idx]);
    ly->num_sublayers++;
    ly->active_sub = idx;
    return idx;
}

void muse_layer_remove_sublayer(MuseLayer *ly, int idx) {
    if (ly->num_sublayers <= 1 || idx < 0 || idx >= ly->num_sublayers) return;
    note_array_free(&ly->sublayers[idx]);
    memmove(&ly->sublayers[idx], &ly->sublayers[idx + 1],
            (size_t)(ly->num_sublayers - idx - 1) * sizeof(NoteArray));
    ly->num_sublayers--;
    if (ly->active_sub >= ly->num_sublayers)
        ly->active_sub = ly->num_sublayers - 1;
}
