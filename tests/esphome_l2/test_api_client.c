#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#include "esphome_api.h"
#include "private/esphome_api_codec.h"
#include "noise_test_responder.h"
typedef struct{int listen_fd;int mode;} server_arg_t;
/* Noise-mode server bookkeeping, owned by the server thread. */
static uint8_t g_psk[32];
static int g_noise_mode;      /* 0 = valid handshake, 1 = garbage, 2 = wrong psk */
static uint16_t g_cmd_type;
static uint8_t g_cmd_payload[64];
static size_t g_cmd_len;
static bool allr(int fd,uint8_t*p,size_t n){while(n){ssize_t r=recv(fd,p,n,0);if(r<=0)return false;p+=(size_t)r;n-=(size_t)r;}return true;}static bool allw(int fd,const uint8_t*p,size_t n){while(n){ssize_t r=send(fd,p,n,0);if(r<=0)return false;p+=(size_t)r;n-=(size_t)r;}return true;}
static bool readv(int fd,uint32_t*out){uint32_t v=0;unsigned sh=0;for(int i=0;i<4;i++){uint8_t b;if(!allr(fd,&b,1))return false;v|=(uint32_t)(b&0x7f)<<sh;if(!(b&0x80)){*out=v;return true;}sh+=7;}return false;}static size_t encv(uint32_t v,uint8_t*b){size_t n=0;do{uint8_t x=v&0x7f;v>>=7;if(v)x|=0x80;b[n++]=x;}while(v);return n;}
static bool recv_frame(int fd,uint16_t*t,uint8_t*p,size_t cap,size_t*n){uint8_t z;if(!allr(fd,&z,1)||z!=0)return false;uint32_t len,type;if(!readv(fd,&len)||!readv(fd,&type)||len>cap)return false;if(len&&!allr(fd,p,len))return false;*t=(uint16_t)type;*n=len;return true;}static bool send_frame(int fd,uint16_t t,const uint8_t*p,size_t n){uint8_t h[12];size_t k=0;h[k++]=0;k+=encv((uint32_t)n,h+k);k+=encv(t,h+k);return allw(fd,h,k)&&(!n||allw(fd,p,n));}
static void *server(void*vp){server_arg_t*a=vp;int fd=accept(a->listen_fd,NULL,NULL);assert(fd>=0);if(a->mode==1){uint8_t tmp[64];(void)recv(fd,tmp,sizeof(tmp),0);uint8_t one=1;assert(allw(fd,&one,1));close(fd);return NULL;}uint8_t p[512];size_t n;uint16_t t;assert(recv_frame(fd,&t,p,sizeof(p),&n)&&t==1);
esphome_pb_writer_t w;esphome_pb_writer_init(&w,p,sizeof(p));esphome_pb_put_varint(&w,1,1);esphome_pb_put_varint(&w,2,15);esphome_pb_put_string(&w,3,"ESPHome 2026.8.0",32);esphome_pb_put_string(&w,4,"test-node",31);assert(send_frame(fd,2,p,w.len));
assert(recv_frame(fd,&t,p,sizeof(p),&n)&&t==9);esphome_pb_writer_init(&w,p,sizeof(p));esphome_pb_put_string(&w,2,"test-node",31);esphome_pb_put_string(&w,3,"AA:BB:CC:DD:EE:FF",17);esphome_pb_put_string(&w,4,"2026.8.0",32);esphome_pb_put_string(&w,6,"esp32-c6",127);esphome_pb_put_string(&w,12,"Espressif",20);esphome_pb_put_string(&w,13,"Kitchen Node",120);assert(send_frame(fd,10,p,w.len));
assert(recv_frame(fd,&t,p,sizeof(p),&n)&&t==11);assert(send_frame(fd,36,NULL,0));esphome_pb_writer_init(&w,p,sizeof(p));esphome_pb_put_string(&w,1,"relay",120);esphome_pb_put_fixed32(&w,2,0x11111111);esphome_pb_put_string(&w,3,"Relay",120);esphome_pb_put_string(&w,9,"outlet",47);assert(send_frame(fd,17,p,w.len));esphome_pb_writer_init(&w,p,sizeof(p));esphome_pb_put_string(&w,1,"temp",120);esphome_pb_put_fixed32(&w,2,0x22222222);esphome_pb_put_string(&w,3,"Temperature",120);esphome_pb_put_string(&w,6,"C",63);assert(send_frame(fd,16,p,w.len));assert(send_frame(fd,13,NULL,0));assert(send_frame(fd,19,NULL,0));
assert(recv_frame(fd,&t,p,sizeof(p),&n)&&t==20);esphome_pb_writer_init(&w,p,sizeof(p));esphome_pb_put_fixed32(&w,1,0x11111111);esphome_pb_put_bool(&w,2,true);assert(send_frame(fd,26,p,w.len));
assert(recv_frame(fd,&t,p,sizeof(p),&n)&&t==5);assert(send_frame(fd,6,NULL,0));close(fd);return NULL;}
static uint16_t start_server(pthread_t*th,server_arg_t*a,int mode){a->listen_fd=socket(AF_INET,SOCK_STREAM,0);assert(a->listen_fd>=0);int yes=1;setsockopt(a->listen_fd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));struct sockaddr_in sa={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK),.sin_port=0};assert(bind(a->listen_fd,(void*)&sa,sizeof(sa))==0&&listen(a->listen_fd,1)==0);socklen_t sl=sizeof(sa);assert(getsockname(a->listen_fd,(void*)&sa,&sl)==0);a->mode=mode;assert(pthread_create(th,NULL,server,a)==0);return ntohs(sa.sin_port);}
/* Encrypted peer: Noise handshake, then the same read-only interrogation the
 * plaintext peer answers, and finally an assertion that the switch command
 * arrives as an authenticated encrypted frame. */
static void *nserver(void*vp){server_arg_t*a=vp;int fd=accept(a->listen_fd,NULL,NULL);assert(fd>=0);
uint8_t m1[48],m2[48],fr[1280],pl[512];size_t n;uint16_t t;
ntr_state_t st;uint8_t priv[32];uint8_t peer_psk[32];
for(unsigned i=0;i<32;i++)priv[i]=(uint8_t)(0x40u+i);
/* Mode 1 is the wrong-PSK peer: it knows a different secret on purpose. */
memcpy(peer_psk,g_psk,32);
if(g_noise_mode==1)peer_psk[0]^=0xffu;
ntr_reset(&st,peer_psk,(const uint8_t*)ESPHOME_NOISE_PROLOGUE,priv,NULL);
assert(allr(fd,m1,4)&&m1[0]==0x00&&m1[1]==0x01&&m1[2]==0x00&&m1[3]==0x00);
assert(allr(fd,m1,3));{unsigned len=((unsigned)m1[1]<<8)|m1[2];if(m1[0]!=0x01||len!=48u){fprintf(stderr,"noise hello header: %02x %02x %02x (want 01 00 30), closing\n",m1[0],m1[1],m1[2]);fflush(stderr);close(fd);return NULL;}}
assert(allr(fd,m1,48));
if(!ntr_handshake(&st,m1,m2)){fprintf(stderr,"PEER rejected m1=%02x%02x%02x%02x..%02x%02x\n",m1[0],m1[1],m1[2],m1[3],m1[46],m1[47]);fflush(stderr);/* Wrong PSK: report it the way an ESPHome peer does. */uint8_t err[4]={0x01,0x00,0x01,0x01};(void)send(fd,err,sizeof(err),0);close(fd);return NULL;}
{uint8_t ssend[32],srecv[32];assert(ntr_split(&st,ssend,srecv));uint8_t hdr[3];hdr[0]=0x01;hdr[1]=0;hdr[2]=48;assert(allw(fd,hdr,3)&&allw(fd,m2,48));
if(g_noise_mode==2){/* Handshake accepted, then frames that are not authentic
 * ciphertext: the client must drop them, never dispatch them, and eventually
 * disconnect rather than loop forever. */
uint8_t junk[8]={0x01,0x00,0x10,1,2,3,4,5};
for(int i=0;i<32;i++){(void)send(fd,junk,sizeof(junk),0);}
close(fd);return NULL;}
/* HelloRequest is the first encrypted frame. */
assert(allr(fd,fr,3)&&fr[0]==0x01);n=((size_t)fr[1]<<8)|fr[2];assert(n<=sizeof(fr)&&allr(fd,fr,n));
assert(ntr_open(&st,fr,n,&t,pl,sizeof(pl),&n)&&t==1&&n==0);
esphome_pb_writer_t w;esphome_pb_writer_init(&w,pl,sizeof(pl));esphome_pb_put_varint(&w,1,1);esphome_pb_put_varint(&w,2,15);esphome_pb_put_string(&w,3,"ESPHome 2026.8.0",32);esphome_pb_put_string(&w,4,"noise-node",31);size_t fl=ntr_seal(&st,2,pl,w.len,fr,sizeof(fr));assert(fl&&allw(fd,fr,fl));
/* DeviceInfoRequest / response. */
assert(allr(fd,fr,3)&&fr[0]==0x01);n=((size_t)fr[1]<<8)|fr[2];assert(n<=sizeof(fr)&&allr(fd,fr,n));assert(ntr_open(&st,fr,n,&t,pl,sizeof(pl),&n)&&t==9);
esphome_pb_writer_init(&w,pl,sizeof(pl));esphome_pb_put_string(&w,2,"noise-node",31);esphome_pb_put_string(&w,3,"AA:BB:CC:DD:EE:FF",17);esphome_pb_put_string(&w,4,"2026.8.0",32);esphome_pb_put_string(&w,6,"esp32-c6",127);esphome_pb_put_string(&w,12,"Espressif",20);esphome_pb_put_string(&w,13,"Kitchen Node",120);fl=ntr_seal(&st,10,pl,w.len,fr,sizeof(fr));assert(fl&&allw(fd,fr,fl));
/* ListEntitiesRequest: one switch plus the finished marker. */
assert(allr(fd,fr,3)&&fr[0]==0x01);n=((size_t)fr[1]<<8)|fr[2];assert(n<=sizeof(fr)&&allr(fd,fr,n));assert(ntr_open(&st,fr,n,&t,pl,sizeof(pl),&n)&&t==11);
esphome_pb_writer_init(&w,pl,sizeof(pl));esphome_pb_put_string(&w,1,"relay",120);esphome_pb_put_fixed32(&w,2,0x11111111);esphome_pb_put_string(&w,3,"Relay",120);esphome_pb_put_string(&w,9,"outlet",47);fl=ntr_seal(&st,17,pl,w.len,fr,sizeof(fr));assert(fl&&allw(fd,fr,fl));
fl=ntr_seal(&st,19,NULL,0,fr,sizeof(fr));assert(fl&&allw(fd,fr,fl));
/* SubscribeStatesRequest then one encrypted state report. */
assert(allr(fd,fr,3)&&fr[0]==0x01);n=((size_t)fr[1]<<8)|fr[2];assert(n<=sizeof(fr)&&allr(fd,fr,n));assert(ntr_open(&st,fr,n,&t,pl,sizeof(pl),&n)&&t==20);
esphome_pb_writer_init(&w,pl,sizeof(pl));esphome_pb_put_fixed32(&w,1,0x11111111);esphome_pb_put_bool(&w,2,true);fl=ntr_seal(&st,26,pl,w.len,fr,sizeof(fr));assert(fl&&allw(fd,fr,fl));
/* The switch command must arrive authenticated and encrypted. */
assert(allr(fd,fr,3)&&fr[0]==0x01);n=((size_t)fr[1]<<8)|fr[2];assert(n<=sizeof(fr)&&allr(fd,fr,n));
assert(ntr_open(&st,fr,n,&t,pl,sizeof(pl),&n));g_cmd_type=t;g_cmd_len=n;memcpy(g_cmd_payload,pl,n);
/* Answered by a state report, never by assuming the command took effect. */
esphome_pb_writer_init(&w,pl,sizeof(pl));esphome_pb_put_fixed32(&w,1,0x11111111);esphome_pb_put_bool(&w,2,false);fl=ntr_seal(&st,26,pl,w.len,fr,sizeof(fr));assert(fl&&allw(fd,fr,fl));
/* DisconnectRequest. */
assert(allr(fd,fr,3)&&fr[0]==0x01);n=((size_t)fr[1]<<8)|fr[2];assert(n<=sizeof(fr)&&allr(fd,fr,n));assert(ntr_open(&st,fr,n,&t,pl,sizeof(pl),&n)&&t==5);
fl=ntr_seal(&st,6,NULL,0,fr,sizeof(fr));assert(fl&&allw(fd,fr,fl));}
close(fd);return NULL;}
static uint16_t start_nserver(pthread_t*th,server_arg_t*a,int mode){a->listen_fd=socket(AF_INET,SOCK_STREAM,0);assert(a->listen_fd>=0);int yes=1;setsockopt(a->listen_fd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));struct sockaddr_in sa={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK),.sin_port=0};assert(bind(a->listen_fd,(void*)&sa,sizeof(sa))==0&&listen(a->listen_fd,1)==0);socklen_t sl=sizeof(sa);assert(getsockname(a->listen_fd,(void*)&sa,&sl)==0);a->mode=mode;g_noise_mode=mode;assert(pthread_create(th,NULL,nserver,a)==0);return ntohs(sa.sin_port);}
static int got_state;static void scb(const esphome_api_state_t*s,void*u){(void)u;assert(s->kind==ESPHOME_API_ENTITY_SWITCH&&s->key==0x11111111&&s->value.boolean);got_state++;}
static int got_off;static void scb_off(const esphome_api_state_t*s,void*u){(void)u;assert(s->kind==ESPHOME_API_ENTITY_SWITCH&&s->key==0x11111111&&!s->value.boolean);got_off++;}
int main(void){signal(SIGPIPE,SIG_IGN);pthread_t th;server_arg_t a;uint16_t port=start_server(&th,&a,0);esphome_api_session_t s;esphome_api_config_t c={.host="127.0.0.1",.port=port,.timeout_ms=1000,.max_frame_bytes=512};assert(esphome_api_init(&s,&c)==ESP_OK);esphome_api_probe_result_t pr;assert(esphome_api_probe(&s,&pr)==ESP_OK&&pr.api_version_minor==15&&!strcmp(pr.name,"test-node")&&!strcmp(pr.model,"esp32-c6"));esphome_api_entity_t item[1];esphome_api_entity_list_t list={.items=item,.capacity=1};assert(esphome_api_entities(&s,&list)==ESP_OK&&list.count==1&&list.total_seen==2&&list.unsupported_seen==1&&list.truncated&&item[0].kind==ESPHOME_API_ENTITY_SWITCH);assert(esphome_api_subscribe(&s,scb,NULL)==ESP_OK);assert(esphome_api_poll(&s,1000)==ESP_OK&&got_state==1);esphome_api_command_t cmd={.kind=ESPHOME_API_COMMAND_SWITCH,.key=0x11111111,.value.switch_.state=false};assert(esphome_api_command(&s,&cmd)==ESP_ERR_NOT_SUPPORTED);assert(esphome_api_last_protocol_error(&s)==ESPHOME_API_PROTOCOL_ERROR_AUTH_REQUIRED);assert(esphome_api_close(&s)==ESP_OK&&!esphome_api_is_connected(&s));esphome_api_deinit(&s);pthread_join(th,NULL);close(a.listen_fd);
port=start_server(&th,&a,1);c.port=port;assert(esphome_api_init(&s,&c)==ESP_OK);assert(esphome_api_probe(&s,&pr)==ESP_ERR_NOT_SUPPORTED);assert(esphome_api_last_protocol_error(&s)==ESPHOME_API_PROTOCOL_ERROR_ENCRYPTION_REQUIRED);esphome_api_deinit(&s);pthread_join(th,NULL);close(a.listen_fd);
/* Encrypted Native API session end to end: Noise handshake, authenticated
 * read-only interrogation, an authenticated command whose send does not move
 * observed state by itself, and a state report that does. */
{uint8_t psk[32];for(unsigned i=0;i<32;i++)psk[i]=(uint8_t)(0xa0u+i);memcpy(g_psk,psk,32);g_cmd_type=0;g_cmd_len=0;memset(g_cmd_payload,0,sizeof(g_cmd_payload));
port=start_nserver(&th,&a,0);c.port=port;c.noise_psk=psk;c.noise_psk_len=sizeof(psk);
assert(esphome_api_init(&s,&c)==ESP_OK);
{esp_err_t pe=esphome_api_probe(&s,&pr);if(pe!=ESP_OK||strcmp(pr.name,"noise-node")||strcmp(pr.model,"esp32-c6")){fprintf(stderr,"noise probe failed: err=%d proto=%d name='%s' model='%s' ver=%u.%u\n",(int)pe,(int)esphome_api_last_protocol_error(&s),pr.name,pr.model,(unsigned)pr.api_version_major,(unsigned)pr.api_version_minor);fflush(stderr);}assert(pe==ESP_OK&&!strcmp(pr.name,"noise-node")&&!strcmp(pr.model,"esp32-c6"));}
esphome_api_entity_t nitem[2];esphome_api_entity_list_t nlist={.items=nitem,.capacity=2};
assert(esphome_api_entities(&s,&nlist)==ESP_OK&&nlist.count==1&&nlist.total_seen==1&&!nlist.truncated&&nitem[0].kind==ESPHOME_API_ENTITY_SWITCH);
assert(esphome_api_subscribe(&s,scb_off,NULL)==ESP_OK);
esphome_api_command_t ncmd={.kind=ESPHOME_API_COMMAND_SWITCH,.key=0x11111111,.value.switch_.state=false};
assert(esphome_api_command(&s,&ncmd)==ESP_OK);
/* Sending the command changed nothing on its own: the observed state moves only
 * when the peer reports it, which is what this poll receives. */
assert(got_off==0);
assert(esphome_api_poll(&s,1000)==ESP_OK&&got_off==1);
assert(esphome_api_last_protocol_error(&s)==ESPHOME_API_PROTOCOL_ERROR_NONE);
assert(esphome_api_close(&s)==ESP_OK&&!esphome_api_is_connected(&s));
esphome_api_deinit(&s);pthread_join(th,NULL);close(a.listen_fd);
/* The command really crossed the wire, encrypted, with the ESPHome switch
 * command message type and the requested key. */
assert(g_cmd_type==33&&g_cmd_len>=4);
assert(g_cmd_payload[0]==0x11&&g_cmd_payload[1]==0x11&&g_cmd_payload[2]==0x11&&g_cmd_payload[3]==0x11);
assert(got_state==1&&got_off==1);}
/* Wrong PSK: the peer rejects the handshake and the client reports an
 * authentication failure instead of proceeding on an unauthenticated link. */
{uint8_t psk[32];for(unsigned i=0;i<32;i++)psk[i]=(uint8_t)(0xa0u+i);memcpy(g_psk,psk,32);
port=start_nserver(&th,&a,1);c.port=port;c.noise_psk=psk;c.noise_psk_len=sizeof(psk);
assert(esphome_api_init(&s,&c)==ESP_OK);
assert(esphome_api_probe(&s,&pr)==ESP_ERR_INVALID_RESPONSE);
assert(esphome_api_last_protocol_error(&s)==ESPHOME_API_PROTOCOL_ERROR_AUTH_REQUIRED);
assert(!esphome_api_is_connected(&s));
esphome_api_deinit(&s);pthread_join(th,NULL);close(a.listen_fd);}
/* Frames that are not authenticated ciphertext are dropped, and a peer that
 * never sends a valid frame is disconnected rather than retried forever. */
{uint8_t psk[32];for(unsigned i=0;i<32;i++)psk[i]=(uint8_t)(0xa0u+i);memcpy(g_psk,psk,32);
port=start_nserver(&th,&a,2);c.port=port;c.noise_psk=psk;c.noise_psk_len=sizeof(psk);
assert(esphome_api_init(&s,&c)==ESP_OK);
assert(esphome_api_probe(&s,&pr)==ESP_ERR_INVALID_RESPONSE);
assert(esphome_api_last_protocol_error(&s)==ESPHOME_API_PROTOCOL_ERROR_MALFORMED);
assert(!esphome_api_is_connected(&s));
esphome_api_deinit(&s);pthread_join(th,NULL);close(a.listen_fd);}
puts("api client tests: ok");return 0;}
