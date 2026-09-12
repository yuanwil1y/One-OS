#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "esphome_ble_gatt.h"
#include "private/esphome_ble_gatt_internal.h"
typedef struct{void*owner;esphome_ble_gatt_backend_notify_fn notify;esphome_ble_gatt_session_t *session;bool connected;bool fail_connect;bool lose_next;bool timeout_next;bool cancel_during_read;bool cancel_during_discover;bool deinit_during_read;bool deinit_during_discover;uint16_t last_cccd;bool notify_enabled;int disconnects,cancels,reads,writes,deinits;} mock_t;
/* Proof that the in-flight call really ran and really tore the session down. It has
 * to live outside the mock: deinit wipes the backend's own storage, so the mock's
 * counters are gone by the time the call returns. */
static int g_deinit_seen_reads, g_deinit_seen_deinits;
static esp_err_t mi(void*ctx,void*owner,esphome_ble_gatt_backend_notify_fn n,int*ne){mock_t*c=ctx;memset(c,0,sizeof(*c));c->owner=owner;c->notify=n;*ne=0;return ESP_OK;} static void md(void*ctx){mock_t*c=ctx;c->deinits++;}
static esp_err_t mc(void*ctx,const esphome_ble_peer_t*p,uint32_t t,int*n){(void)p;(void)t;mock_t*c=ctx;if(c->fail_connect){*n=7;return ESP_FAIL;}c->connected=true;return ESP_OK;}
static esp_err_t mdisc(void*ctx,esphome_ble_gatt_db_t*db,uint32_t t,int*n){(void)t;(void)n;mock_t*c=ctx;if(c->cancel_during_discover){assert(esphome_ble_gatt_cancel(c->session)==ESP_OK);return ESP_OK;}if(db->service_capacity){db->services[0]=(esphome_ble_gatt_service_t){.start_handle=1,.end_handle=9,.first_characteristic=0,.characteristic_count=1};db->service_count=1;}else db->truncated=true;if(db->characteristic_capacity){db->characteristics[0]=(esphome_ble_gatt_characteristic_t){.definition_handle=2,.value_handle=3,.end_handle=9};db->characteristic_count=1;}else db->truncated=true;return ESP_OK;}
static esp_err_t mr(void*ctx,uint16_t h,uint8_t*out,size_t cap,size_t*len,uint32_t t,int*n){(void)h;(void)t;mock_t*c=ctx;c->reads++;if(c->deinit_during_read){/* Another task tears the session down while this read is outstanding. This models the NimBLE backend, which deletes its completion semaphore in deinit: whatever the transport hands back must not be a success, and nothing may touch the wiped session afterwards. The counters are copied out first, because the teardown below wipes this struct. */g_deinit_seen_reads=c->reads;esphome_ble_gatt_deinit(c->session);g_deinit_seen_deinits=c->deinits;*len=0;return ESP_OK;}if(c->cancel_during_read){/* Another task cancels while this read is outstanding. */assert(esphome_ble_gatt_cancel(c->session)==ESP_OK);*len=0;return ESP_OK;}if(c->timeout_next){c->timeout_next=false;*n=99;return ESP_ERR_TIMEOUT;}if(c->lose_next){c->lose_next=false;c->connected=false;*n=8;return ESP_FAIL;}static const uint8_t v[]={1,2,3};*len=sizeof(v);if(cap<sizeof(v))return ESP_ERR_INVALID_SIZE;memcpy(out,v,sizeof(v));return ESP_OK;}
static esp_err_t mw(void*ctx,uint16_t h,const uint8_t*d,size_t l,bool resp,uint32_t t,int*n){(void)h;(void)d;(void)l;(void)resp;(void)t;(void)n;mock_t*c=ctx;c->writes++;return ESP_OK;}
static esp_err_t mn(void*ctx,uint16_t cccd,bool en,bool ind,uint32_t t,int*n){(void)ind;(void)t;(void)n;mock_t*c=ctx;c->last_cccd=cccd;c->notify_enabled=en;return ESP_OK;}
static esp_err_t mcan(void*ctx,uint32_t t,int*n){(void)t;(void)n;mock_t*c=ctx;c->connected=false;c->cancels++;return ESP_OK;} static esp_err_t mdc(void*ctx,uint32_t t,int*n){mock_t*c=ctx;c->disconnects++;return mcan(ctx,t,n);} static bool mis(const void*ctx){return ((const mock_t*)ctx)->connected;}
static const esphome_ble_gatt_backend_ops_t ops={.init=mi,.deinit=md,.connect=mc,.discover=mdisc,.read=mr,.write=mw,.set_notify=mn,.cancel=mcan,.disconnect=mdc,.connected=mis};
const esphome_ble_gatt_backend_ops_t esphome_ble_gatt_nimble_backend={0};
typedef struct{int suspends,resumes;uintptr_t token_seen;} radio_t; static esp_err_t rs(void*u,uintptr_t*t){radio_t*r=u;r->suspends++;*t=0x1234;return ESP_OK;} static void rr(void*u,uintptr_t t){radio_t*r=u;r->resumes++;r->token_seen=t;}
static int notify_count;static void cb(uint16_t h,const uint8_t*d,size_t n,bool trunc,void*u){(void)u;assert(h==3&&n==2&&d[0]==9&&!trunc);notify_count++;}
static mock_t*ctxof(esphome_ble_gatt_session_t*s){return (mock_t*)((esphome_ble_gatt_impl_t*)(void*)s)->backend_ctx;}
int main(void){esphome_ble_gatt_session_t s;radio_t r={0};esphome_ble_gatt_config_t cfg={.connect_timeout_ms=10,.operation_timeout_ms=10,.disconnect_timeout_ms=10,.radio_suspend=rs,.radio_resume=rr,.radio_user=&r};assert(esphome_ble_gatt_init_with_backend(&s,&cfg,&ops)==ESP_OK);esphome_ble_peer_t p={{1,2,3,4,5,6},0};assert(esphome_ble_gatt_connect(&s,&p)==ESP_OK);assert(r.suspends==1&&!r.resumes&&esphome_ble_gatt_is_connected(&s));
esphome_ble_gatt_service_t sv[1];esphome_ble_gatt_characteristic_t ch[1];esphome_ble_gatt_descriptor_t ds[1];esphome_ble_gatt_db_t db={.services=sv,.service_capacity=1,.characteristics=ch,.characteristic_capacity=1,.descriptors=ds,.descriptor_capacity=1};assert(esphome_ble_gatt_discover(&s,&db)==ESP_OK&&db.service_count==1&&db.characteristic_count==1&&!db.truncated);
uint8_t b[3];size_t len=0;assert(esphome_ble_gatt_read(&s,3,b,sizeof(b),&len)==ESP_OK&&len==3&&b[2]==3);assert(esphome_ble_gatt_subscribe(&s,3,4,false,cb,NULL)==ESP_OK&&ctxof(&s)->notify_enabled);uint8_t nv[2]={9,8};ctxof(&s)->notify(ctxof(&s)->owner,3,nv,2,false);assert(notify_count==1);assert(esphome_ble_gatt_unsubscribe(&s,3)==ESP_OK&&!ctxof(&s)->notify_enabled);
assert(esphome_ble_gatt_disconnect(&s)==ESP_OK&&r.resumes==1&&r.token_seen==0x1234);
assert(esphome_ble_gatt_connect(&s,&p)==ESP_OK);ctxof(&s)->lose_next=true;assert(esphome_ble_gatt_read(&s,3,b,sizeof(b),&len)==ESP_FAIL&&r.resumes==2);
ctxof(&s)->fail_connect=true;assert(esphome_ble_gatt_connect(&s,&p)==ESP_FAIL&&r.suspends==3&&r.resumes==3);ctxof(&s)->fail_connect=false;
assert(esphome_ble_gatt_connect(&s,&p)==ESP_OK);ctxof(&s)->timeout_next=true;assert(esphome_ble_gatt_read(&s,3,b,sizeof(b),&len)==ESP_ERR_TIMEOUT);assert(ctxof(&s)->cancels>=1&&!esphome_ble_gatt_is_connected(&s)&&r.resumes==4);
assert(esphome_ble_gatt_connect(&s,&p)==ESP_OK);assert(esphome_ble_gatt_cancel(&s)==ESP_OK&&!esphome_ble_gatt_is_connected(&s)&&r.resumes==5);
/* A cancel while a read is outstanding: the backend still answers ESP_OK with no
 * data, and that must not be reported as a successful read. */
assert(esphome_ble_gatt_connect(&s,&p)==ESP_OK);ctxof(&s)->session=&s;ctxof(&s)->cancel_during_read=true;len=0;{int before=ctxof(&s)->reads;assert(esphome_ble_gatt_read(&s,3,b,sizeof(b),&len)==ESP_ERR_INVALID_STATE);assert(ctxof(&s)->reads==before+1&&len==0);}ctxof(&s)->cancel_during_read=false;
/* A cancelled discovery must not claim an empty database. */
assert(esphome_ble_gatt_connect(&s,&p)==ESP_OK);ctxof(&s)->cancel_during_discover=true;db.service_count=7;db.characteristic_count=7;assert(esphome_ble_gatt_discover(&s,&db)==ESP_ERR_INVALID_STATE);ctxof(&s)->cancel_during_discover=false;
/*
 * A deinit while a read is outstanding.
 *
 * The backend tears the session down from inside the call and still answers ESP_OK
 * with no data - which is what the NimBLE backend can do once its completion
 * semaphore has been deleted underneath a blocked caller. The call must not report
 * success, and the session must be left wiped rather than half-cleared with a live
 * backend pointer. This is the lifecycle case the cancel tests do not reach: a
 * cancel keeps the session alive, a deinit does not.
 */
assert(esphome_ble_gatt_connect(&s,&p)==ESP_OK);
ctxof(&s)->session=&s;
ctxof(&s)->deinit_during_read=true;
{
    esp_err_t e;
    int reads_seen;

    g_deinit_seen_reads=-1;
    g_deinit_seen_deinits=-1;
    len=77;
    e=esphome_ble_gatt_read(&s,3,b,sizeof(b),&len);
    reads_seen=g_deinit_seen_reads;
    assert(e==ESP_ERR_INVALID_STATE);
    /* Proven to have reached the situation rather than passed by avoiding it: the
     * backend ran this read and then deinitialised the session. The read counter is
     * cumulative for the session, so this asserts it moved. deinits is read after
     * the teardown zeroed it - which is itself the proof that the teardown ran and
     * that the backend's storage is gone by the time the caller resumes. */
    assert(reads_seen>0);
    assert(g_deinit_seen_deinits==0);
}
/* A wiped session must refuse work rather than run it against a stale backend.
 * init_with_backend() clears the mock again, so the flag cannot leak forward. */
assert(esphome_ble_gatt_init_with_backend(&s,&cfg,&ops)==ESP_OK);
assert(esphome_ble_gatt_connect(&s,&p)==ESP_OK);
assert(esphome_ble_gatt_is_connected(&s));
esphome_ble_gatt_deinit(&s);
/* And after a clean deinit the session is refused outright. */
assert(esphome_ble_gatt_connect(&s,&p)==ESP_ERR_INVALID_STATE);

esphome_ble_gatt_config_t bad={.radio_suspend=rs};assert(esphome_ble_gatt_init_with_backend(&s,&bad,&ops)==ESP_ERR_INVALID_ARG);puts("gatt tests: ok");return 0;}
