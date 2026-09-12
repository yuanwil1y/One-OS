#include "esphome_ble_gatt.h"
#include "private/esphome_ble_gatt_internal.h"
#include <string.h>
#define DC 10000u
#define DO 5000u
#define DD 3000u
static esphome_ble_gatt_impl_t *I(esphome_ble_gatt_session_t*s){return (void*)s;}
static const esphome_ble_gatt_impl_t *CI(const esphome_ble_gatt_session_t*s){return (const void*)s;}
static bool ok(esphome_ble_gatt_impl_t*i){return i&&i->magic==ESPHOME_BLE_GATT_IMPL_MAGIC&&i->initialized&&i->backend;}
static void restore(esphome_ble_gatt_impl_t*i){if(i->radio_suspended){if(i->config.radio_resume)i->config.radio_resume(i->config.radio_user,i->restore_token);i->radio_suspended=false;i->restore_token=0;}}
static void clearsubs(esphome_ble_gatt_impl_t*i){memset(i->subscriptions,0,sizeof(i->subscriptions));}
static void notify(void*owner,uint16_t h,const uint8_t*d,size_t n,bool trunc){esphome_ble_gatt_impl_t*i=owner;if(!ok(i))return;for(size_t k=0;k<ESPHOME_BLE_GATT_MAX_SUBSCRIPTIONS;k++){esphome_ble_gatt_subscription_t*s=&i->subscriptions[k];if(s->active&&s->value_handle==h&&s->callback){s->callback(h,d,n,trunc,s->user);return;}}}
static esp_err_t begin(esphome_ble_gatt_impl_t*i,bool need){if(!ok(i)||i->op_active)return ESP_ERR_INVALID_STATE;if(need&&(!i->connected||!i->backend->connected(i->backend_ctx))){i->connected=false;return ESP_ERR_INVALID_STATE;}i->op_active=true;i->op_epoch_at_start=i->op_epoch;return ESP_OK;}
/* A cancel or a deinit while an operation is outstanding leaves the backend free
 * to report success for work the application gave up on: a cancel that terminates
 * the link makes a pending read complete with no error and no data, and a
 * discovery can even complete with an empty database. Every operation therefore
 * carries the epoch it was issued in, and a result that arrives after the epoch
 * moved is reported as abandoned rather than as success. */
static esp_err_t end(esphome_ble_gatt_impl_t*i,esp_err_t e){bool abandoned=(i->op_epoch!=i->op_epoch_at_start);const esphome_ble_gatt_backend_ops_t*b=i->backend;if(e==ESP_ERR_TIMEOUT&&b&&b->connected(i->backend_ctx)){int cleanup_native=0;(void)b->cancel(i->backend_ctx,i->config.disconnect_timeout_ms,&cleanup_native);}i->op_active=false;if(!b){/* The session was deinitialised under this operation, so the backend is gone:
 * there is no link left to report and nothing to tear down. The operation is
 * reported as abandoned, which is what it is. Without this the wipe turns the
 * check below into a dereference of NULL. */
 i->connected=false;return abandoned?ESP_ERR_INVALID_STATE:e;}i->connected=b->connected(i->backend_ctx);if(abandoned)e=ESP_ERR_INVALID_STATE;if(e!=ESP_OK&&!i->connected){clearsubs(i);restore(i);}return e;}
esp_err_t esphome_ble_gatt_init_with_backend(esphome_ble_gatt_session_t*s,const esphome_ble_gatt_config_t*c,const esphome_ble_gatt_backend_ops_t*b){if(!s||!b||!b->init||!b->connect||!b->discover||!b->read||!b->write||!b->set_notify||!b->cancel||!b->disconnect||!b->connected)return ESP_ERR_INVALID_ARG;memset(s,0,sizeof(*s));esphome_ble_gatt_impl_t*i=I(s);i->magic=ESPHOME_BLE_GATT_IMPL_MAGIC;i->backend=b;if(c)i->config=*c;if(!i->config.connect_timeout_ms)i->config.connect_timeout_ms=DC;if(!i->config.operation_timeout_ms)i->config.operation_timeout_ms=DO;if(!i->config.disconnect_timeout_ms)i->config.disconnect_timeout_ms=DD;if((i->config.radio_suspend==NULL)!=(i->config.radio_resume==NULL)){memset(s,0,sizeof(*s));return ESP_ERR_INVALID_ARG;}esp_err_t e=b->init(i->backend_ctx,i,notify,&i->last_native_error);if(e!=ESP_OK){memset(s,0,sizeof(*s));return e;}i->initialized=true;return ESP_OK;}
esp_err_t esphome_ble_gatt_init(esphome_ble_gatt_session_t*s,const esphome_ble_gatt_config_t*c){return esphome_ble_gatt_init_with_backend(s,c,&ESPHOME_BLE_GATT_BACKEND);}
void esphome_ble_gatt_deinit(esphome_ble_gatt_session_t*s){if(!s)return;esphome_ble_gatt_impl_t*i=I(s);if(!ok(i)){memset(s,0,sizeof(*s));return;}/* Invalidate any outstanding operation before the context goes away, so a
 * caller blocked in one reports an abandoned operation instead of writing into
 * a session that no longer exists.
 *
 * The caller is blocked inside the backend call, on another task, and will run
 * `end()` with this same session pointer once it returns. So the teardown has to
 * leave a state that `end()` can survive: bumping op_epoch alone is not enough,
 * because `end()` also asks the backend whether the link is still up, and by then
 * the storage has been wiped and the backend pointer is NULL. Setting it to NULL
 * BEFORE the wipe - and before backend->deinit(), which for NimBLE deletes the
 * completion semaphore - is what makes that read a comparison instead of a
 * dereference. The backend context is captured first because backend->deinit()
 * wipes the storage this function is reading. */const esphome_ble_gatt_backend_ops_t*b=i->backend;void*bctx=(void*)i->backend_ctx;i->op_epoch++;if(b->connected(bctx))(void)b->disconnect(bctx,i->config.disconnect_timeout_ms,&i->last_native_error);i->connected=false;clearsubs(i);restore(i);i->backend=NULL;if(b->deinit)b->deinit(bctx);memset(s,0,sizeof(*s));/* Abandon the outstanding operation across the wipe.
 *
 * Bumping op_epoch is not enough on its own: the memset above clears op_epoch_at_start
 * as well, so the caller's comparison in end() would read 0 == 0 and the operation
 * would report the backend's own ESP_OK - a successful read of nothing. Writing an
 * epoch that no operation can hold makes the comparison in end() fail for any
 * operation that started before this point, whatever the wipe did to the field.
 * UINT32_MAX is unreachable in practice because it would take that many deinits
 * on one session, and a spurious "abandoned" is the safe direction: it reports a
 * failure, which is true. */I(s)->op_epoch_at_start=UINT32_MAX;}
esp_err_t esphome_ble_gatt_connect(esphome_ble_gatt_session_t*s,const esphome_ble_peer_t*p){if(!s||!p||p->address_type>3)return ESP_ERR_INVALID_ARG;esphome_ble_gatt_impl_t*i=I(s);esp_err_t e=begin(i,false);if(e!=ESP_OK)return e;if(i->connected||i->backend->connected(i->backend_ctx))return end(i,ESP_ERR_INVALID_STATE);if(i->config.radio_suspend){e=i->config.radio_suspend(i->config.radio_user,&i->restore_token);if(e!=ESP_OK)return end(i,e);i->radio_suspended=true;}i->last_native_error=0;e=i->backend->connect(i->backend_ctx,p,i->config.connect_timeout_ms,&i->last_native_error);if(e==ESP_OK)i->connected=true;else restore(i);return end(i,e);}
esp_err_t esphome_ble_gatt_discover(esphome_ble_gatt_session_t*s,esphome_ble_gatt_db_t*db){if(!s||!db||(db->service_capacity&&!db->services)||(db->characteristic_capacity&&!db->characteristics)||(db->descriptor_capacity&&!db->descriptors))return ESP_ERR_INVALID_ARG;esphome_ble_gatt_impl_t*i=I(s);esp_err_t e=begin(i,true);if(e!=ESP_OK)return e;db->service_count=db->characteristic_count=db->descriptor_count=0;db->truncated=false;i->last_native_error=0;e=i->backend->discover(i->backend_ctx,db,i->config.operation_timeout_ms,&i->last_native_error);return end(i,e);}
esp_err_t esphome_ble_gatt_read(esphome_ble_gatt_session_t*s,uint16_t h,uint8_t*out,size_t cap,size_t*len){if(!s||!h||!len||(cap&&!out))return ESP_ERR_INVALID_ARG;esphome_ble_gatt_impl_t*i=I(s);esp_err_t e=begin(i,true);if(e!=ESP_OK)return e;*len=0;i->last_native_error=0;e=i->backend->read(i->backend_ctx,h,out,cap,len,i->config.operation_timeout_ms,&i->last_native_error);return end(i,e);}
esp_err_t esphome_ble_gatt_write(esphome_ble_gatt_session_t*s,uint16_t h,const uint8_t*d,size_t n,bool resp){if(!s||!h||(n&&!d)||n>UINT16_MAX)return ESP_ERR_INVALID_ARG;esphome_ble_gatt_impl_t*i=I(s);esp_err_t e=begin(i,true);if(e!=ESP_OK)return e;i->last_native_error=0;e=i->backend->write(i->backend_ctx,h,d,n,resp,i->config.operation_timeout_ms,&i->last_native_error);return end(i,e);}
esp_err_t esphome_ble_gatt_subscribe(esphome_ble_gatt_session_t*s,uint16_t vh,uint16_t cccd,bool ind,esphome_ble_gatt_notify_fn cb,void*u){if(!s||!vh||!cccd||!cb)return ESP_ERR_INVALID_ARG;esphome_ble_gatt_impl_t*i=I(s);esp_err_t e=begin(i,true);if(e!=ESP_OK)return e;esphome_ble_gatt_subscription_t*slot=NULL;for(size_t k=0;k<ESPHOME_BLE_GATT_MAX_SUBSCRIPTIONS;k++){if(i->subscriptions[k].active&&i->subscriptions[k].value_handle==vh)return end(i,ESP_ERR_INVALID_STATE);if(!slot&&!i->subscriptions[k].active)slot=&i->subscriptions[k];}if(!slot)return end(i,ESP_ERR_NO_MEM);i->last_native_error=0;e=i->backend->set_notify(i->backend_ctx,cccd,true,ind,i->config.operation_timeout_ms,&i->last_native_error);if(e==ESP_OK)*slot=(esphome_ble_gatt_subscription_t){.value_handle=vh,.cccd_handle=cccd,.indications=ind,.active=true,.callback=cb,.user=u};return end(i,e);}
esp_err_t esphome_ble_gatt_unsubscribe(esphome_ble_gatt_session_t*s,uint16_t vh){if(!s||!vh)return ESP_ERR_INVALID_ARG;esphome_ble_gatt_impl_t*i=I(s);esp_err_t e=begin(i,true);if(e!=ESP_OK)return e;esphome_ble_gatt_subscription_t*slot=NULL;for(size_t k=0;k<ESPHOME_BLE_GATT_MAX_SUBSCRIPTIONS;k++)if(i->subscriptions[k].active&&i->subscriptions[k].value_handle==vh){slot=&i->subscriptions[k];break;}if(!slot)return end(i,ESP_ERR_NOT_FOUND);i->last_native_error=0;e=i->backend->set_notify(i->backend_ctx,slot->cccd_handle,false,slot->indications,i->config.operation_timeout_ms,&i->last_native_error);if(e==ESP_OK)memset(slot,0,sizeof(*slot));return end(i,e);}
esp_err_t esphome_ble_gatt_cancel(esphome_ble_gatt_session_t*s){if(!s)return ESP_ERR_INVALID_ARG;esphome_ble_gatt_impl_t*i=I(s);if(!ok(i))return ESP_ERR_INVALID_STATE;i->last_native_error=0;/* Move the epoch first: from here on any operation still outstanding reports
 * an abandoned operation, whatever the backend returns for it. */i->op_epoch++;esp_err_t e=i->backend->cancel(i->backend_ctx,i->config.disconnect_timeout_ms,&i->last_native_error);i->op_active=false;i->connected=i->backend->connected(i->backend_ctx);if(!i->connected){clearsubs(i);restore(i);}return e;}
esp_err_t esphome_ble_gatt_disconnect(esphome_ble_gatt_session_t*s){if(!s)return ESP_ERR_INVALID_ARG;esphome_ble_gatt_impl_t*i=I(s);if(!ok(i)||i->op_active)return ESP_ERR_INVALID_STATE;i->last_native_error=0;esp_err_t e=ESP_OK;if(i->connected||i->backend->connected(i->backend_ctx))e=i->backend->disconnect(i->backend_ctx,i->config.disconnect_timeout_ms,&i->last_native_error);i->connected=i->backend->connected(i->backend_ctx);if(!i->connected){clearsubs(i);restore(i);}return e;}
bool esphome_ble_gatt_is_connected(const esphome_ble_gatt_session_t*s){if(!s)return false;const esphome_ble_gatt_impl_t*i=CI(s);return i->magic==ESPHOME_BLE_GATT_IMPL_MAGIC&&i->initialized&&i->connected&&i->backend&&i->backend->connected(i->backend_ctx);}
int esphome_ble_gatt_last_native_error(const esphome_ble_gatt_session_t*s){if(!s)return 0;const esphome_ble_gatt_impl_t*i=CI(s);return i->magic==ESPHOME_BLE_GATT_IMPL_MAGIC?i->last_native_error:0;}
