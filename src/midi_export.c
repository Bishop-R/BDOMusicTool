#include "midi_export.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct{unsigned char*d;size_t n,cap;}Buf;
static int put(Buf*b,unsigned v){if(b->n==b->cap){size_t c=b->cap?b->cap*2:1024;void*p=realloc(b->d,c);if(!p)return 0;b->d=p;b->cap=c;}b->d[b->n++]=(unsigned char)v;return 1;}
static int vlq(Buf*b,unsigned v){unsigned char x[5];int n=0;x[n++]=v&127;while((v>>=7))x[n++]=(v&127)|128;while(n--)if(!put(b,x[n]))return 0;return 1;}
static void be16(FILE*f,unsigned v){fputc(v>>8,f);fputc(v,f);}static void be32(FILE*f,unsigned v){fputc(v>>24,f);fputc(v>>16,f);fputc(v>>8,f);fputc(v,f);}
typedef struct{unsigned tick;unsigned char st,a,b;}Ev;static int evcmp(const void*a,const void*b){const Ev*x=a,*y=b;if(x->tick!=y->tick)return x->tick<y->tick?-1:1;return (x->st&0xf0)==0x80?-1:1;}
int midi_export(const char*path,const MuseProject*p){if(!path||!p)return-1;FILE*f=fopen(path,"wb");if(!f)return-2;fwrite("MThd",1,4,f);be32(f,6);be16(f,1);be16(f,(unsigned)(p->num_layers+1));be16(f,480);
 Buf t={0};put(&t,0);put(&t,0xff);put(&t,0x51);put(&t,3);unsigned us=60000000u/(p->bpm?p->bpm:120);put(&t,us>>16);put(&t,us>>8);put(&t,us);put(&t,0);put(&t,0xff);put(&t,0x2f);put(&t,0);fwrite("MTrk",1,4,f);be32(f,(unsigned)t.n);fwrite(t.d,1,t.n,f);free(t.d);
 double ticks_ms=480.0*(p->bpm?p->bpm:120)/60000.0;
 for(int l=0;l<p->num_layers;l++){const MuseLayer*ly=&p->layers[l];int nn=muse_layer_note_count(ly);Ev*e=malloc((size_t)nn*2*sizeof(Ev));if(nn&& !e){fclose(f);return-3;}int k=0;unsigned char ch=(unsigned char)(l%16);
  for(int s=0;s<ly->num_sublayers;s++)for(int n=0;n<ly->sublayers[s].count;n++){const MuseNote*x=&ly->sublayers[s].notes[n];unsigned st=(unsigned)(x->start*ticks_ms+.5),en=(unsigned)((x->start+x->dur)*ticks_ms+.5);e[k++]=(Ev){st,(unsigned char)(0x90|ch),x->pitch,x->vel};e[k++]=(Ev){en,(unsigned char)(0x80|ch),x->pitch,0};}qsort(e,(size_t)k,sizeof(Ev),evcmp);Buf b={0};unsigned prev=0;for(int i=0;i<k;i++){vlq(&b,e[i].tick-prev);put(&b,e[i].st);put(&b,e[i].a);put(&b,e[i].b);prev=e[i].tick;}vlq(&b,0);put(&b,0xff);put(&b,0x2f);put(&b,0);fwrite("MTrk",1,4,f);be32(f,(unsigned)b.n);fwrite(b.d,1,b.n,f);free(b.d);free(e);}
 int bad=ferror(f);if(fclose(f)!=0)bad=1;return bad?-4:0;}
