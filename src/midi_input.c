#include "midi_input.h"
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#include <stdatomic.h>
#define MIDI_QUEUE 256
static HMIDIIN g_in;static MidiInputEvent g_q[MIDI_QUEUE];static atomic_uint g_read,g_write;
static void CALLBACK midi_cb(HMIDIIN h,UINT msg,DWORD_PTR user,DWORD_PTR data,DWORD_PTR stamp){(void)h;(void)user;(void)stamp;if(msg!=MIM_DATA)return;unsigned s=data&255,d1=(data>>8)&127,d2=(data>>16)&127;if((s&0xf0)!=0x80&&(s&0xf0)!=0x90)return;unsigned w=atomic_load(&g_write),next=(w+1)%MIDI_QUEUE;if(next==atomic_load(&g_read))return;g_q[w]=(MidiInputEvent){s&0xf0,s&15,d1,d2};atomic_store(&g_write,next);}
bool midi_input_init(void){if(g_in)return true;UINT n=midiInGetNumDevs();if(!n)return false;if(midiInOpen(&g_in,0,(DWORD_PTR)midi_cb,0,CALLBACK_FUNCTION)!=MMSYSERR_NOERROR){g_in=NULL;return false;}midiInStart(g_in);return true;}
void midi_input_shutdown(void){if(g_in){midiInStop(g_in);midiInReset(g_in);midiInClose(g_in);g_in=NULL;}}
int midi_input_poll(MidiInputEvent*out,int cap){int n=0;while(n<cap){unsigned r=atomic_load(&g_read);if(r==atomic_load(&g_write))break;out[n++]=g_q[r];atomic_store(&g_read,(r+1)%MIDI_QUEUE);}return n;}
bool midi_input_available(void){return g_in!=NULL;}
#elif defined(HAVE_ALSA_MIDI)
#include <alsa/asoundlib.h>
static snd_seq_t*g_seq;static int g_port=-1,g_connected;
static void connect_sources(void){if(!g_seq)return;snd_seq_client_info_t*ci;snd_seq_port_info_t*pi;snd_seq_client_info_alloca(&ci);snd_seq_port_info_alloca(&pi);snd_seq_client_info_set_client(ci,-1);int self=snd_seq_client_id(g_seq);while(snd_seq_query_next_client(g_seq,ci)>=0){int c=snd_seq_client_info_get_client(ci);if(c==self||c==SND_SEQ_CLIENT_SYSTEM)continue;snd_seq_port_info_set_client(pi,c);snd_seq_port_info_set_port(pi,-1);while(snd_seq_query_next_port(g_seq,pi)>=0){unsigned cap=snd_seq_port_info_get_capability(pi);if((cap&SND_SEQ_PORT_CAP_READ)&&(cap&SND_SEQ_PORT_CAP_SUBS_READ)&&snd_seq_connect_from(g_seq,g_port,c,snd_seq_port_info_get_port(pi))==0)g_connected++;}}}
bool midi_input_init(void){if(g_seq)return true;if(snd_seq_open(&g_seq,"default",SND_SEQ_OPEN_INPUT,SND_SEQ_NONBLOCK)<0){g_seq=NULL;return false;}snd_seq_set_client_name(g_seq,"BDO Music Compositor");g_port=snd_seq_create_simple_port(g_seq,"MIDI Input",SND_SEQ_PORT_CAP_WRITE|SND_SEQ_PORT_CAP_SUBS_WRITE,SND_SEQ_PORT_TYPE_MIDI_GENERIC|SND_SEQ_PORT_TYPE_APPLICATION);if(g_port<0){snd_seq_close(g_seq);g_seq=NULL;return false;}connect_sources();return true;}
void midi_input_shutdown(void){if(g_seq){snd_seq_close(g_seq);g_seq=NULL;g_port=-1;g_connected=0;}}
int midi_input_poll(MidiInputEvent*out,int cap){if(!g_seq)return 0;static int scan;if(!g_connected&&!(scan++&255))connect_sources();int n=0;snd_seq_event_t*e;while(n<cap&&snd_seq_event_input(g_seq,&e)>=0){if(e->type==SND_SEQ_EVENT_NOTEON||e->type==SND_SEQ_EVENT_NOTEOFF){out[n++]=(MidiInputEvent){e->type==SND_SEQ_EVENT_NOTEOFF||!e->data.note.velocity?0x80:0x90,e->data.note.channel,e->data.note.note,e->data.note.velocity};}}return n;}
bool midi_input_available(void){return g_seq&&g_connected;}
#else
bool midi_input_init(void){return false;}void midi_input_shutdown(void){}int midi_input_poll(MidiInputEvent*e,int c){(void)e;(void)c;return 0;}bool midi_input_available(void){return false;}
#endif
