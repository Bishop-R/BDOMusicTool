#include "composition_tools.h"
#include <math.h>
#include <stdlib.h>

static uint32_t rng_next(uint32_t *s) {
    *s = *s * 1664525u + 1013904223u;
    return *s;
}
static int rng_sym(uint32_t *s, int amount) {
    if (amount <= 0) return 0;
    return (int)(rng_next(s) % (uint32_t)(amount * 2 + 1)) - amount;
}
static int selected_count(const NoteArray *a) {
    int n = 0; for (int i=0;i<a->count;i++) if (a->notes[i].selected) n++; return n;
}

int notes_quantize(NoteArray *a,double beat_ms,int div,int strength,int swing,bool ends){
    if(!a||beat_ms<=0||div<1)return 0;if(strength<0)strength=0;if(strength>100)strength=100;
    if(swing<50)swing=50;if(swing>75)swing=75;double step=beat_ms/div,k=strength/100.0;int changed=0;
    for(int i=0;i<a->count;i++){MuseNote*n=&a->notes[i];if(!n->selected)continue;
        double idx=round(n->start/step),target=idx*step;if(((long long)idx&1)!=0)target+=(swing-50)/25.0*step*.5;
        double old_end=n->start+n->dur;n->start+=(target-n->start)*k;if(n->start<0)n->start=0;
        if(ends){double ei=round(old_end/step),et=ei*step;if(((long long)ei&1)!=0)et+=(swing-50)/25.0*step*.5;
            double ne=old_end+(et-old_end)*k;n->dur=ne-n->start;if(n->dur<10)n->dur=10;}changed++;}return changed;
}

int notes_humanize(NoteArray*a,int timing,int velocity,int length,uint32_t seed){
    if(!a)return 0;int changed=0;if(seed==0)seed=1;
    for(int i=0;i<a->count;i++){MuseNote*n=&a->notes[i];if(!n->selected)continue;
        n->start+=rng_sym(&seed,timing);if(n->start<0)n->start=0;int v=n->vel+rng_sym(&seed,velocity);if(v<1)v=1;if(v>127)v=127;n->vel=(uint8_t)v;
        n->dur*=1.0+rng_sym(&seed,length)/100.0;if(n->dur<10)n->dur=10;changed++;}return changed;
}
static int cmp_int(const void*a,const void*b){return *(const int*)a-*(const int*)b;}
int notes_arpeggiate(NoteArray*a,double beat_ms,int div,int octaves,int gate){
    if(!a||div<1||octaves<1)return 0;int sel=selected_count(a);if(sel<2)return 0;int*p=malloc((size_t)sel*sizeof(int));if(!p)return 0;
    double start=1e100,end=0;int pc=0,vel=100,ntype=0;
    for(int i=0;i<a->count;i++)if(a->notes[i].selected){MuseNote*n=&a->notes[i];p[pc++]=n->pitch;if(n->start<start)start=n->start;if(n->start+n->dur>end)end=n->start+n->dur;vel=n->vel;ntype=n->ntype;}
    qsort(p,(size_t)pc,sizeof(int),cmp_int);int unique=0;for(int i=0;i<pc;i++)if(i==0||p[i]!=p[i-1])p[unique++]=p[i];
    for(int i=a->count-1;i>=0;i--)if(a->notes[i].selected)note_array_remove(a,i);
    double step=beat_ms/div,dur=step*(gate<10?10:gate>100?100:gate)/100.0;int made=0;
    for(double t=start;t<end-.5;t+=step){int pos=made%(unique*octaves),pitch=p[pos%unique]+12*(pos/unique);if(pitch<=127){MuseNote n={(uint8_t)pitch,(uint8_t)vel,(uint8_t)ntype,1,t,dur};note_array_push(a,n);made++;}}
    free(p);return made;
}
int notes_strum(NoteArray*a,int spread,int taper,int jitter,uint32_t seed,bool upward){
    if(!a)return 0;if(seed==0)seed=1;int changed=0;
    for(int i=0;i<a->count;i++){MuseNote*base=&a->notes[i];if(!base->selected)continue;int rank=0;
        for(int j=0;j<a->count;j++)if(a->notes[j].selected&&fabs(a->notes[j].start-base->start)<=30&&
            (upward?a->notes[j].pitch<base->pitch:a->notes[j].pitch>base->pitch))rank++;
        double delta=rank*spread+rng_sym(&seed,jitter);base->start+=delta;if(base->start<0)base->start=0;int v=base->vel-rank*taper*base->vel/100;if(v<1)v=1;base->vel=(uint8_t)v;changed++;}return changed;
}
