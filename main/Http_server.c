#include "Http_server.h"
#include "solar.h"
#include "schedule.h"
#include "schedule_persist.h"
#include "av.h"
#include "bv.h"
#include "trendlog.h"
#include "rtc_fram_manager.h"
#include "fram_layout.h"
#include "fram_fm24cl64b.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <time.h>

extern void av_pwm_apply(uint32_t instance, float percent);


/* ================================================================
 * Buffer historique Trend Log — 48 points max (48h à raison de 1/h)
 * Accès thread-safe via spinlock FreeRTOS
 * ================================================================ */
#include "freertos/semphr.h"

#define TL_HIST_MAX   48   /* 48 points = 48 heures */

typedef struct {
    char  time_str[10];  /* "HH:MM" */
    char  date_str[12];  /* "MM-DD" */
    float av0;
    float av1;
    bool  valid;
} tl_hist_entry_t;

static tl_hist_entry_t s_tl_hist[TL_HIST_MAX];
static int   s_tl_head  = 0;   /* prochain index d'écriture */
static int   s_tl_count = 0;   /* entrées valides */
static SemaphoreHandle_t s_tl_mutex = NULL;

/* Appelé depuis schedule_task toutes les heures */
void http_trendlog_record(float av0, float av1)
{
    if (!s_tl_mutex) {
        s_tl_mutex = xSemaphoreCreateMutex();
    }
    if (xSemaphoreTake(s_tl_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;

    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    tl_hist_entry_t *e = &s_tl_hist[s_tl_head];
    snprintf(e->time_str, sizeof(e->time_str), "%02d:%02d",
             (int)(t->tm_hour & 0x1F), (int)(t->tm_min & 0x3F));
    snprintf(e->date_str, sizeof(e->date_str), "%02d/%02d",
             (int)(t->tm_mday & 0x1F), (int)((t->tm_mon+1) & 0x0F));
    e->av0   = av0;
    e->av1   = av1;
    e->valid = true;

    s_tl_head = (s_tl_head + 1) % TL_HIST_MAX;
    if (s_tl_count < TL_HIST_MAX) s_tl_count++;

    xSemaphoreGive(s_tl_mutex);
    ESP_LOGI("http_srv", "[TL] Point enregistré %s/%s AV0=%.1f AV1=%.1f",
             e->date_str, e->time_str, av0, av1);
}

static const char *TAG = "http_srv";

#ifndef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
#endif

/* Déclaré dans main.c */
extern void sched_force_av(int inst, float val);
static httpd_handle_t s_server = NULL;

/* ================================================================
 * Page HTML embarquée
 * ================================================================ */
static const char HTML_PAGE[] =
"<!DOCTYPE html><html lang='fr'><head>"
"<meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>USEDA ROC</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:-apple-system,sans-serif;background:#0a0e1a;color:#e8eaf0;min-height:100vh}"
"header{background:linear-gradient(135deg,#1f3864,#2e75b6);padding:14px 20px;display:flex;justify-content:space-between;align-items:center}"
"header h1{font-size:16px;font-weight:700;letter-spacing:0.5px}"
"#clock{font-size:13px;opacity:0.85;font-variant-numeric:tabular-nums}"
".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:16px;padding:16px}"
".card{background:rgba(255,255,255,0.05);border:1px solid rgba(255,255,255,0.1);border-radius:12px;padding:16px}"
".card h2{font-size:14px;font-weight:600;margin-bottom:12px;opacity:0.9}"
".stat{display:flex;justify-content:space-between;align-items:center;padding:6px 0;border-bottom:1px solid rgba(255,255,255,0.06);font-size:13px}"
".stat:last-child{border-bottom:none}"
".stat-label{opacity:0.65}"
".stat-value{font-weight:600;color:#60a5fa}"
".badge{padding:2px 10px;border-radius:20px;font-size:11px;font-weight:700}"
".badge-night{background:rgba(99,102,241,0.3);color:#a5b4fc}"
".badge-day{background:rgba(251,191,36,0.3);color:#fcd34d}"
".badge-force{background:rgba(52,199,89,0.3);color:#34C759}"
".slider-row{margin:10px 0}"
".slider-label{display:flex;justify-content:space-between;font-size:12px;margin-bottom:6px;opacity:0.8}"
"input[type=range]{width:100%;accent-color:#2e75b6;height:6px}"
".btn{padding:8px 16px;border:none;border-radius:8px;font-size:13px;font-weight:600;cursor:pointer;transition:opacity 0.2s}"
".btn:hover{opacity:0.85}"
".btn-primary{background:#2e75b6;color:#fff}"
".btn-danger{background:rgba(220,50,47,0.8);color:#fff}"
".btn-row{display:flex;gap:8px;flex-wrap:wrap;margin-top:8px}"
".info-box{background:rgba(52,199,89,0.1);border:1px solid rgba(52,199,89,0.3);border-radius:8px;padding:8px 12px;font-size:12px;color:#34C759;margin-bottom:12px}"
".se-form{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-bottom:10px}"
".se-form input,.se-form select{background:rgba(255,255,255,0.08);border:1px solid rgba(255,255,255,0.15);border-radius:6px;color:#e8eaf0;padding:6px 8px;font-size:12px;width:100%}"
".se-form label{font-size:11px;opacity:0.65;margin-bottom:2px;display:block}"
"#se-list{margin-top:10px;max-height:200px;overflow-y:auto}"
".se-item{display:flex;justify-content:space-between;align-items:center;padding:6px 0;border-bottom:1px solid rgba(255,255,255,0.07);font-size:12px}"
".se-item:last-child{border-bottom:none}"
".input-row{display:flex;gap:8px;margin-bottom:8px;align-items:flex-end}"
".input-row input,.input-row select{background:rgba(255,255,255,0.08);border:1px solid rgba(255,255,255,0.15);border-radius:6px;color:#e8eaf0;padding:6px 8px;font-size:12px;flex:1}"
".lbl{font-size:11px;opacity:0.6;margin-bottom:3px}"
".se-msg{font-size:12px;min-height:16px;margin-top:6px}"
".tl-canvas{width:100%;height:120px;background:rgba(0,0,0,0.3);border-radius:8px;margin-top:8px}"
"</style></head><body>"
"<header><h1>USEDA ROC Eclairage BACnet</h1><span id='clock'>--:--:--</span></header>"
"<div class='grid'>"
"<div class='card'><h2>Etat des zones</h2>"
"<div class='stat'><span class='stat-label'>Zone A (AV0)</span><span class='stat-value' id='av0'>--%</span></div>"
"<div class='stat'><span class='stat-label'>Zone B (AV1)</span><span class='stat-value' id='av1'>--%</span></div>"
"<div class='stat'><span class='stat-label'>Source active</span><span class='stat-value' id='src'>--</span></div>"
"<div class='stat'><span class='stat-label'>Etat nuit/jour</span><span id='night-badge' class='badge'>--</span></div>"
"<div class='stat'><span class='stat-label'>Lever / Coucher</span><span class='stat-value' id='sun-times'>--</span></div>"
"<div class='stat'><span class='stat-label'>Solaire</span><span class='stat-value' id='solar-status'>--</span></div>"
"</div>"
"<div class='card'><h2>Forcage manuel</h2>"
"<div class='info-box' id='force-info'>Forcer une valeur -- sera ecrase au lever/coucher</div>"
"<div class='slider-row'>"
"<div class='slider-label'><span>Zone A (AV0)</span><span id='lbl0'>50%</span></div>"
"<input type='range' id='sl0' min='0' max='100' value='50' oninput=\"document.getElementById('lbl0').textContent=this.value+'%'\">"
"</div>"
"<div class='slider-row'>"
"<div class='slider-label'><span>Zone B (AV1)</span><span id='lbl1'>50%</span></div>"
"<input type='range' id='sl1' min='0' max='100' value='50' oninput=\"document.getElementById('lbl1').textContent=this.value+'%'\">"
"</div>"
"<div class='btn-row'>"
"<button class='btn btn-primary' onclick='setAV(0)'>Forcer Zone A</button>"
"<button class='btn btn-primary' onclick='setAV(1)'>Forcer Zone B</button>"
"</div></div>"
"<div class='card'><h2>Config solaire</h2>"
"<div class='stat'><span class='stat-label'>Latitude</span><input id='lat' style='background:rgba(255,255,255,0.08);border:1px solid rgba(255,255,255,0.15);border-radius:6px;color:#e8eaf0;padding:4px 6px;width:100px;font-size:12px' value='45.78'></div>"
"<div class='stat'><span class='stat-label'>Longitude</span><input id='lon' style='background:rgba(255,255,255,0.08);border:1px solid rgba(255,255,255,0.15);border-radius:6px;color:#e8eaf0;padding:4px 6px;width:100px;font-size:12px' value='5.93'></div>"
"<div class='stat'><span class='stat-label'>Decalage coucher (min)</span><input id='off-bef' type='number' style='background:rgba(255,255,255,0.08);border:1px solid rgba(255,255,255,0.15);border-radius:6px;color:#e8eaf0;padding:4px 6px;width:70px;font-size:12px' value='0'></div>"
"<div class='stat'><span class='stat-label'>Decalage lever (min)</span><input id='off-aft' type='number' style='background:rgba(255,255,255,0.08);border:1px solid rgba(255,255,255,0.15);border-radius:6px;color:#e8eaf0;padding:4px 6px;width:70px;font-size:12px' value='0'></div>"
"<div class='btn-row' style='margin-top:10px'>"
"<button class='btn btn-primary' onclick='saveSolar()'>Sauvegarder</button>"
"<span id='solar-save-msg' style='font-size:12px;color:#34C759'></span>"
"</div></div>"
"<div class='card'><h2>Special Events</h2>"
"<div class='se-form'>"
"<div><label class='lbl'>Nom</label><input id='se-name' placeholder='Noel' maxlength='19'></div>"
"<div><label class='lbl'>Schedule</label><select id='se-sch'><option value='0'>SCH0 Zone A</option><option value='1'>SCH1 Zone B</option></select></div>"
"<div><label class='lbl'>Date</label><input id='se-date' type='date'></div>"
"<div><label class='lbl'>Heure debut</label><input id='se-time' type='time' value='19:00'></div>"
"<div><label class='lbl'>Valeur debut (%)</label><input id='se-value' type='number' min='0' max='100' value='75'></div>"
"<div><label class='lbl'>Heure fin (opt.)</label><input id='se-time-end' type='time'></div>"
"<div><label class='lbl'>Valeur fin (%)</label><input id='se-value-end' type='number' min='0' max='100' value='0'></div>"
"<div style='display:flex;align-items:flex-end;gap:6px'>"
"<button class='btn btn-primary' onclick='addSE()' style='flex:1'>Ajouter</button>"
"<button class='btn btn-danger' onclick='clearAllSE()' style='flex:1'>Effacer tout</button>"
"</div>"
"</div>"
"<div class='se-msg' id='se-msg'></div>"
"<div id='se-list'><i style='opacity:0.5;font-size:12px'>Chargement...</i></div>"
"</div>"
"<div class='card'><h2>Journal evenements</h2>"
"<div style='max-height:200px;overflow-y:auto'>"
"<table id='events-table' style='width:100%;font-size:11px;border-collapse:collapse'>"
"<thead><tr style='opacity:0.6'><td>#</td><td>Date</td><td>Type</td><td>Val.</td></tr></thead>"
"<tbody></tbody></table></div></div>"
"<div class='card'><h2>Historique 48h</h2>"
"<canvas id='tl-canvas' class='tl-canvas'></canvas>"
"</div>"
"</div>"
"<script>"
"function api(url,m,b){return fetch(url,{method:m||'GET',headers:{'Content-Type':'application/json'},body:b?JSON.stringify(b):undefined}).then(function(r){return r.json();});}"
"function pad(n){return n<10?'0'+n:n;}"
"function updateClock(){var n=new Date();document.getElementById('clock').textContent=pad(n.getHours())+':'+pad(n.getMinutes())+':'+pad(n.getSeconds());}"
"setInterval(updateClock,1000);updateClock();"
"function loadStatus(){"
"api('/api/status').then(function(d){"
"document.getElementById('av0').textContent=(d.av0!==undefined?d.av0.toFixed(1):'--')+'%';"
"document.getElementById('av1').textContent=(d.av1!==undefined?d.av1.toFixed(1):'--')+'%';"
"document.getElementById('src').textContent=d.source||'--';"
"var nb=document.getElementById('night-badge');"
"nb.textContent=d.is_night?'NUIT':'JOUR';"
"nb.className='badge '+(d.is_night?'badge-night':'badge-day');"
"document.getElementById('sun-times').textContent=(d.sunrise||'--')+' / '+(d.sunset||'--');"
"document.getElementById('solar-status').textContent=d.solar_enabled?'Actif':'Desactive';"
"var fi=document.getElementById('force-info');"
"if(d.force0||d.force1){fi.textContent='Forcage actif -- sera ecrase au lever/coucher';fi.style.color='#34C759';}"
"else{fi.textContent='Forcer une valeur -- sera ecrase au lever/coucher';fi.style.color='';}"
"}).catch(function(e){console.log('status err',e);});}"
"function loadSolarConfig(){"
"api('/api/solar').then(function(d){"
"if(d.latitude!==undefined)document.getElementById('lat').value=d.latitude;"
"if(d.longitude!==undefined)document.getElementById('lon').value=d.longitude;"
"if(d.offset_before!==undefined)document.getElementById('off-bef').value=d.offset_before;"
"if(d.offset_after!==undefined)document.getElementById('off-aft').value=d.offset_after;"
"}).catch(function(e){console.log('solar err',e);});}"
"function saveSolar(){"
"var b={latitude:parseFloat(document.getElementById('lat').value),"
"longitude:parseFloat(document.getElementById('lon').value),"
"offset_before:parseInt(document.getElementById('off-bef').value),"
"offset_after:parseInt(document.getElementById('off-aft').value)};"
"api('/api/solar','POST',b).then(function(d){"
"var m=document.getElementById('solar-save-msg');"
"m.textContent=d.ok?'Sauvegarde !':'Erreur';"
"setTimeout(function(){m.textContent='';},3000);"
"}).catch(function(e){console.log(e);});}"
"function setAV(inst){"
"var val=parseFloat(document.getElementById('sl'+inst).value);"
"api('/api/av','POST',{instance:inst,value:val}).then(function(d){"
"console.log('force AV'+inst+' -> '+val);"
"loadStatus();"
"}).catch(function(e){console.log(e);});}"
"function loadSE(){"
"api('/api/special_events').then(function(d){"
"var list=document.getElementById('se-list');"
"if(!list)return;"
"var sch0=d.sch0||[];var sch1=d.sch1||[];"
"if(!sch0.length&&!sch1.length){list.innerHTML='<i style=\"opacity:0.5;font-size:12px\">Aucun Special Event</i>';return;}"
"var all=sch0.map(function(e){return Object.assign({},e,{sch:0});}).concat(sch1.map(function(e){return Object.assign({},e,{sch:1});}));"
"list.innerHTML='';"
"all.forEach(function(se,i){"
"var d2=document.createElement('div');d2.className='se-item';"
"var lbl='[SCH'+(se.sch===0?'0':'1')+'] '+se.name+' | '+se.date+' '+se.time+' -> '+se.value+'%';"
"if(se.time_end)lbl+=' fin:'+se.time_end+'('+se.value_end+'%)';"
"var sp=document.createElement('span');sp.textContent=lbl;sp.style.flex='1';"
"var btn=document.createElement('button');btn.textContent='X';"
"btn.style.cssText='background:rgba(220,50,47,0.7);border:none;color:#fff;border-radius:4px;padding:2px 7px;cursor:pointer;font-size:11px';"
"(function(s,j){btn.onclick=function(){api('/api/se_delete','POST',{schedule:s,index:j}).then(function(){loadSE();});};})(se.sch,i);"
"d2.appendChild(sp);d2.appendChild(btn);list.appendChild(d2);"
"});"
"}).catch(function(e){console.log('se err',e);});}"
"function addSE(){"
"var dt=document.getElementById('se-date').value;"
"if(!dt){alert('Choisir une date');return;}"
"var p=dt.split('-').map(Number);"
"var tm=document.getElementById('se-time').value.split(':').map(Number);"
"var te=document.getElementById('se-time-end').value;"
"var nm=document.getElementById('se-name').value.replace(/[^\x20-\x7E]/g,'').substring(0,19)||'SE';"
"var body={schedule:parseInt(document.getElementById('se-sch').value),"
"year:p[0]-1900,month:p[1],day:p[2],hour:tm[0],min:tm[1],"
"value:parseFloat(document.getElementById('se-value').value)||75,"
"priority:16,name:nm,"
"has_end:te?1:0,"
"hour_end:te?parseInt(te.split(':')[0]):0,"
"min_end:te?parseInt(te.split(':')[1]):0,"
"value_end:parseFloat(document.getElementById('se-value-end').value)||0};"
"api('/api/special_events','POST',body).then(function(r){"
"var m=document.getElementById('se-msg');"
"m.style.color=r.ok?'#34C759':'#ff3b30';"
"m.textContent=r.ok?'SE ajoute !':'Erreur: '+(r.error||'?');"
"setTimeout(function(){m.textContent='';},5000);"
"if(r.ok)loadSE();"
"}).catch(function(e){console.log(e);});}"
"function clearAllSE(){"
"if(!confirm('Effacer tous les SE ?'))return;"
"api('/api/se_clear','POST',{schedule:0}).then(function(){"
"api('/api/se_clear','POST',{schedule:1}).then(function(){loadSE();});"
"}).catch(function(e){console.log(e);});}"
"function loadEvents(){"
"api('/api/events').then(function(d){"
"var tb=document.getElementById('events-table');if(!tb)return;"
"var rows=d.events||[];"
"tb.querySelector('tbody').innerHTML=rows.map(function(e,i){"
"return '<tr><td>'+(rows.length-i)+'</td><td>'+e.date+'</td><td>'+e.type+'</td><td>'+e.value+'</td></tr>';"
"}).join('');"
"}).catch(function(e){console.log('events err',e);});}"
"function drawTrendlog(data,label,color,canvas){"
"if(!canvas||!data||!data.length)return;"
"var ctx=canvas.getContext('2d');var w=canvas.width=canvas.offsetWidth;var h=canvas.height=120;"
"ctx.clearRect(0,0,w,h);"
"var vals=data.map(function(e){return e.v;});"
"var mn=Math.min.apply(null,vals);var mx=Math.max.apply(null,vals)||1;"
"ctx.strokeStyle=color;ctx.lineWidth=2;ctx.beginPath();"
"data.forEach(function(e,i){var x=i/(data.length-1||1)*w;var y=h-(e.v-mn)/(mx-mn||1)*(h-10)-5;i===0?ctx.moveTo(x,y):ctx.lineTo(x,y);});"
"ctx.stroke();"
"ctx.fillStyle='rgba(255,255,255,0.5)';ctx.font='10px sans-serif';"
"ctx.fillText(label+' | max:'+mx.toFixed(0)+'%',4,12);}"
"function loadTrendlog(){"
"api('/api/trendlog').then(function(d){"
"var c=document.getElementById('tl-canvas');if(!c)return;"
"var e=d.tl0&&d.tl0.entries||[];"
"drawTrendlog(e,'Zone A','#60a5fa',c);"
"}).catch(function(e){console.log('tl err',e);});}"
"setInterval(loadStatus,15000);"
"loadStatus();"
"setTimeout(loadSolarConfig,600);"
"setTimeout(loadEvents,1200);"
"setTimeout(loadSE,1800);"
"setTimeout(loadTrendlog,2400);"
"setInterval(loadTrendlog,60000);"
"</script></body></html>";

/* ================================================================
 * Handlers HTTP
 * ================================================================ */

/* GET / — page HTML */
static esp_err_t handler_favicon(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handler_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Connection", "close");
    /* Envoi par chunks pour eviter EAGAIN sur grande page */
    const char *ptr = HTML_PAGE;
    size_t remaining = strlen(HTML_PAGE);
    const size_t CHUNK = 1024;
    while (remaining > 0) {
        size_t to_send = (remaining > CHUNK) ? CHUNK : remaining;
        if (httpd_resp_send_chunk(req, ptr, (ssize_t)to_send) != ESP_OK) {
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_FAIL;
        }
        ptr       += to_send;
        remaining -= to_send;
    }
    httpd_resp_send_chunk(req, NULL, 0); /* fin du chunked transfer */
    return ESP_OK;
}

/* GET /api/status */
static esp_err_t handler_status(httpd_req_t *req)
{
    solar_times_t st = solar_get_today();
    const solar_config_t *scfg = solar_get_config();
    bool manual = Binary_Value_Is_Control_Enabled();
    bool is_night = solar_is_night_now();

    /* Déterminer la source active */
    const char *source = "Default";
    if (manual) source = "Manuel";
    else if (is_night && scfg->enabled) source = "Solaire";

    char rise_str[8] = "--:--";
    char set_str[8]  = "--:--";
    if (st.valid) {
        snprintf(rise_str, sizeof(rise_str), "%02d:%02d", st.sunrise_h, st.sunrise_m);
        snprintf(set_str,  sizeof(set_str),  "%02d:%02d", st.sunset_h,  st.sunset_m);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "av0",           Analog_Value_Present_Value(0));
    cJSON_AddNumberToObject(root, "av1",           Analog_Value_Present_Value(1));
    cJSON_AddBoolToObject(root,   "manual",        manual);
    cJSON_AddStringToObject(root, "source",        source);
    cJSON_AddStringToObject(root, "sunrise",       rise_str);
    cJSON_AddStringToObject(root, "sunset",        set_str);
    cJSON_AddBoolToObject(root,   "is_night",      is_night);
    cJSON_AddBoolToObject(root,   "solar_enabled", scfg->enabled);

    char *json = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

/* GET /api/solar */
static esp_err_t handler_solar_get(httpd_req_t *req)
{
    const solar_config_t *cfg = solar_get_config();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "latitude",             cfg->latitude);
    cJSON_AddNumberToObject(root, "longitude",            cfg->longitude);
    cJSON_AddNumberToObject(root, "offset_before_sunset", cfg->offset_before_sunset);
    cJSON_AddNumberToObject(root, "offset_after_sunrise", cfg->offset_after_sunrise);
    cJSON_AddBoolToObject(root,   "enabled",              cfg->enabled);
    char *json = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, json, strlen(json));
    free(json); cJSON_Delete(root);
    return ESP_OK;
}

/* Lire le body d'une requête */
static int read_body(httpd_req_t *req, char *buf, int max_len)
{
    int received = 0;
    int remaining = req->content_len;
    while (remaining > 0) {
        int ret = httpd_req_recv(req, buf + received,
                                 MIN(remaining, max_len - received - 1));
        if (ret <= 0) return -1;
        received  += ret;
        remaining -= ret;
    }
    buf[received] = '\0';
    return received;
}

/* POST /api/solar */
static esp_err_t handler_solar_post(httpd_req_t *req)
{
    char buf[256];
    if (read_body(req, buf, sizeof(buf)) < 0) return ESP_FAIL;

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON invalide");
        return ESP_OK;
    }

    solar_config_t cfg = *solar_get_config();

    cJSON *v;
    if ((v = cJSON_GetObjectItem(root, "latitude")))             cfg.latitude             = (float)v->valuedouble;
    if ((v = cJSON_GetObjectItem(root, "longitude")))            cfg.longitude            = (float)v->valuedouble;
    if ((v = cJSON_GetObjectItem(root, "offset_before_sunset"))) cfg.offset_before_sunset = (int16_t)v->valueint;
    if ((v = cJSON_GetObjectItem(root, "offset_after_sunrise"))) cfg.offset_after_sunrise = (int16_t)v->valueint;
    if ((v = cJSON_GetObjectItem(root, "enabled")))              cfg.enabled              = cJSON_IsTrue(v) ? 1 : 0;
    cJSON_Delete(root);

    solar_save_config(&cfg);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* POST /api/mode */
static esp_err_t handler_mode(httpd_req_t *req)
{
    char buf[64];
    if (read_body(req, buf, sizeof(buf)) < 0) return ESP_FAIL;
    cJSON *root = cJSON_Parse(buf);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON invalide"); return ESP_OK; }

    cJSON *v = cJSON_GetObjectItem(root, "manual");
    bool manual = cJSON_IsTrue(v);
    cJSON_Delete(root);

    Binary_Value_Present_Value_Set(0, manual ? BINARY_ACTIVE : BINARY_INACTIVE);
    rfm_save_bv_state(manual);
    ESP_LOGI(TAG, "Mode → %s (depuis page web)", manual ? "MANUEL" : "AUTO");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* POST /api/av */
static esp_err_t handler_av(httpd_req_t *req)
{
    char buf[128];
    if (read_body(req, buf, sizeof(buf)) < 0) return ESP_FAIL;
    cJSON *root = cJSON_Parse(buf);
    if (!root) return ESP_FAIL;

    uint32_t inst = cJSON_GetObjectItem(root, "instance")->valueint;
    float val = (float)cJSON_GetObjectItem(root, "value")->valuedouble;
    cJSON_Delete(root);

    // --- LES 3 LIGNES CRITIQUES ICI ---
    // 1. On écrit en priorité 8 (Manual Operator) pour être plus fort que le Schedule
    Analog_Value_Present_Value_Set(inst, val, 8);
    
    // 2. On bloque la logique auto interne pour cet index
    sched_force_av(inst, val); 

    // 3. On envoie l'ordre direct au PWM (les lampes s'allument direct)
    av_pwm_apply(inst, val);
    // ----------------------------------

    ESP_LOGI(TAG, "Forcage Manuel Web: AV%ld = %.1f%%", inst, val);

    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* GET /api/events */
static esp_err_t handler_events(httpd_req_t *req)
{
    static const char *ENAMES[] = {
        "?", "AV0_CHANGE", "AV1_CHANGE", "MODE_CHANGE",
        "BOOT", "NTP_SYNC", "4G_LOST", "SOLAR_ON", "SOLAR_OFF"
    };

    uint16_t head = 0;
    fram_read(FRAM_EVENT_HEAD_ADDR, (uint8_t *)&head, 2);
    if (head >= FRAM_EVENT_MAX_ENTRIES) head = 0;

    cJSON *root  = cJSON_CreateObject();
    cJSON *arr   = cJSON_CreateArray();

    /* Lire les 30 derniers événements */
    int shown = 0;
    for (int i = FRAM_EVENT_MAX_ENTRIES - 1; i >= 0 && shown < 30; i--) {
        uint16_t idx  = (uint16_t)((head + i) % FRAM_EVENT_MAX_ENTRIES);
        uint16_t addr = FRAM_EVENT_LOG_ADDR + idx * FRAM_EVENT_ENTRY_SIZE;
        fram_event_entry_t e;
        if (fram_read(addr, (uint8_t *)&e, sizeof(e)) != ESP_OK) continue;
        if (e.valid != 0xAA) continue;

        char date[32];
        snprintf(date, sizeof(date), "20%02d-%02d-%02d %02d:%02d:%02d",
                 (int)(e.year & 0x7F), (int)(e.month & 0x0F),
                 (int)(e.day & 0x1F),  (int)(e.hour & 0x1F),
                 (int)(e.min & 0x3F),  (int)(e.sec & 0x3F));

        uint8_t t = e.event_type;
        const char *name = (t < 9) ? ENAMES[t] : "?";

        char val_str[16];
        if (e.event_type == EVENT_MODE_CHANGE)
            snprintf(val_str, sizeof(val_str), "%s", e.extra_u8 ? "MANUEL" : "AUTO");
        else
            snprintf(val_str, sizeof(val_str), "%.1f%%", e.value);

        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "date",  date);
        cJSON_AddStringToObject(entry, "type",  name);
        cJSON_AddStringToObject(entry, "value", val_str);
        cJSON_AddItemToArray(arr, entry);
        shown++;
    }

    cJSON_AddItemToObject(root, "events", arr);
    char *json = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, json, strlen(json));
    free(json); cJSON_Delete(root);
    return ESP_OK;
}

/* ================================================================
 * Démarrage / arrêt
 * ================================================================ */

/* ================================================================
 * Handlers Special Events + TrendLog
 * ================================================================ */


static esp_err_t handler_se_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr0 = cJSON_CreateArray();
    cJSON *arr1 = cJSON_CreateArray();

    for (int sch = 0; sch < 2; sch++) {
        // On récupère l'objet Schedule (0 ou 1) mis à jour par Node-RED
        SCHEDULE_DESCR *desc = Schedule_Object(sch);
        cJSON *arr = (sch == 0) ? arr0 : arr1;
        
        if (!desc) continue;

#if BACNET_EXCEPTION_SCHEDULE_SIZE > 0
        for (int i = 0; i < (int)desc->Exception_Count; i++) {
            BACNET_SPECIAL_EVENT *se = &desc->Exception_Schedule[i];
            
            // On ne traite que les entrées de type calendrier (Date fixe)
            if (se->periodTag != BACNET_SPECIAL_EVENT_PERIOD_CALENDAR_ENTRY) continue;
            if (se->period.calendarEntry.tag != BACNET_CALENDAR_DATE) continue;

            cJSON *entry = cJSON_CreateObject();
            
            // 1. Récupération de la Date (BACnet year = Year - 1900)
           // 1. Récupération de la Date
            char date_str[16];
            int yr = se->period.calendarEntry.type.Date.year;
            int mo = se->period.calendarEntry.type.Date.month;
            int dy = se->period.calendarEntry.type.Date.day;

            // Si l'année est < 1900 (ex: 126), on ajoute 1900. Sinon on garde tel quel.
            int display_year = (yr < 1900) ? (yr + 1900) : yr;

            snprintf(date_str, sizeof(date_str), "%04d-%02d-%02d", 
                     display_year, mo, dy);
            cJSON_AddStringToObject(entry, "date", date_str);

            // 2. Récupération de l'Heure et de la Valeur (Premier point)
            if (se->timeValues.TV_Count > 0) {
                char time_str[10];
                snprintf(time_str, sizeof(time_str), "%02d:%02d", 
                         se->timeValues.Time_Values[0].Time.hour,
                         se->timeValues.Time_Values[0].Time.min);
                cJSON_AddStringToObject(entry, "time", time_str);
                cJSON_AddNumberToObject(entry, "value", se->timeValues.Time_Values[0].Value.type.Real);
            }

            // 3. Point de fin (si présent)
            if (se->timeValues.TV_Count >= 2) {
                char time_end_str[10];
                snprintf(time_end_str, sizeof(time_end_str), "%02d:%02d",
                         se->timeValues.Time_Values[1].Time.hour,
                         se->timeValues.Time_Values[1].Time.min);
                cJSON_AddStringToObject(entry, "time_end", time_end_str);
                cJSON_AddNumberToObject(entry, "value_end", se->timeValues.Time_Values[1].Value.type.Real);
            }

            cJSON_AddStringToObject(entry, "name", "BACnet Event");
            cJSON_AddItemToArray(arr, entry);
        }
#endif
    }

    cJSON_AddItemToObject(root, "sch0", arr0);
    cJSON_AddItemToObject(root, "sch1", arr1);

    char *json = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, json, strlen(json));
    
    free(json); 
    cJSON_Delete(root);
    return ESP_OK;
}
static esp_err_t handler_se_post(httpd_req_t *req)
{
    char buf[320];
    if (read_body(req, buf, sizeof(buf)) < 0) return ESP_FAIL;
    cJSON *root = cJSON_Parse(buf);
    if (!root) { 
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON invalide"); 
        return ESP_FAIL; 
    }

    // Récupération des données avec valeurs par défaut
    int   sch     = cJSON_GetObjectItem(root, "schedule")  ? cJSON_GetObjectItem(root, "schedule")->valueint  : 0;
    int   yr      = cJSON_GetObjectItem(root, "year")      ? cJSON_GetObjectItem(root, "year")->valueint      : 126; // 2026
    int   mo      = cJSON_GetObjectItem(root, "month")     ? cJSON_GetObjectItem(root, "month")->valueint     : 1;
    int   dy      = cJSON_GetObjectItem(root, "day")       ? cJSON_GetObjectItem(root, "day")->valueint       : 1;
    int   hr      = cJSON_GetObjectItem(root, "hour")      ? cJSON_GetObjectItem(root, "hour")->valueint      : 0;
    int   mn      = cJSON_GetObjectItem(root, "min")       ? cJSON_GetObjectItem(root, "min")->valueint       : 0;
    float val     = cJSON_GetObjectItem(root, "value")     ? (float)cJSON_GetObjectItem(root, "value")->valuedouble : 0.0f;
    
    // Sécurité Priorité : Si 0 ou absent, on force 16
    int   prio    = cJSON_GetObjectItem(root, "priority")  ? cJSON_GetObjectItem(root, "priority")->valueint  : 16;
    if (prio < 1 || prio > 16) prio = 16; 

    int   has_end = cJSON_GetObjectItem(root, "has_end")   ? cJSON_GetObjectItem(root, "has_end")->valueint   : 0;
    int   hr_end  = cJSON_GetObjectItem(root, "hour_end")  ? cJSON_GetObjectItem(root, "hour_end")->valueint  : 0;
    int   mn_end  = cJSON_GetObjectItem(root, "min_end")   ? cJSON_GetObjectItem(root, "min_end")->valueint   : 0;
    float val_end = cJSON_GetObjectItem(root, "value_end") ? (float)cJSON_GetObjectItem(root, "value_end")->valuedouble : 0.0f;
    
    const char *se_name = (cJSON_GetObjectItem(root, "name") && cJSON_GetObjectItem(root, "name")->valuestring)
                          ? cJSON_GetObjectItem(root, "name")->valuestring : "SE";
    
    cJSON_Delete(root);

    if (sch < 0 || sch > 1) { 
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"sch_invalid\"}"); 
        return ESP_OK; 
    }

    SCHEDULE_DESCR *desc = Schedule_Object((uint32_t)sch);
    if (!desc) { 
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"schedule_not_found\"}"); 
        return ESP_OK; 
    }

#if BACNET_EXCEPTION_SCHEDULE_SIZE > 0
    if (desc->Exception_Count >= BACNET_EXCEPTION_SCHEDULE_SIZE) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"liste_pleine\"}"); 
        return ESP_OK; 
    }

    int idx = desc->Exception_Count;
    BACNET_SPECIAL_EVENT *se = &desc->Exception_Schedule[idx];
    memset(se, 0, sizeof(BACNET_SPECIAL_EVENT));

    // Configuration de l'événement
    se->periodTag = BACNET_SPECIAL_EVENT_PERIOD_CALENDAR_ENTRY;
    se->period.calendarEntry.tag             = BACNET_CALENDAR_DATE;
    se->period.calendarEntry.type.Date.year  = (uint8_t)yr;
    se->period.calendarEntry.type.Date.month = (uint8_t)mo;
    se->period.calendarEntry.type.Date.day   = (uint8_t)dy;
    se->period.calendarEntry.type.Date.wday  = 255; // N'importe quel jour de la semaine
    
    // APPLICATION DE LA PRIORITE (Correction de l'erreur)
    se->priority = (uint8_t)prio; 

    // Premier point (Début)
    se->timeValues.TV_Count = 1;
    se->timeValues.Time_Values[0].Time.hour       = (uint8_t)hr;
    se->timeValues.Time_Values[0].Time.min        = (uint8_t)mn;
    se->timeValues.Time_Values[0].Time.sec        = 0;
    se->timeValues.Time_Values[0].Time.hundredths = 0;
    se->timeValues.Time_Values[0].Value.tag       = BACNET_APPLICATION_TAG_REAL;
    se->timeValues.Time_Values[0].Value.type.Real = val;

    // Deuxième point (Fin optionnelle)
    if (has_end && hr_end >= 0 && hr_end <= 23) {
        se->timeValues.TV_Count = 2;
        se->timeValues.Time_Values[1].Time.hour       = (uint8_t)hr_end;
        se->timeValues.Time_Values[1].Time.min        = (uint8_t)mn_end;
        se->timeValues.Time_Values[1].Time.sec        = 0;
        se->timeValues.Time_Values[1].Time.hundredths = 0;
        se->timeValues.Time_Values[1].Value.tag       = BACNET_APPLICATION_TAG_REAL;
        se->timeValues.Time_Values[1].Value.type.Real = val_end;
    }

    desc->Exception_Count = (uint8_t)(idx + 1);

    // Sauvegarde en mémoire FRAM
    sched_persist_save_special_events((uint32_t)sch, desc);
    
    // Sauvegarde du nom de l'événement en FRAM
    uint16_t name_addr = (uint16_t)(0x1E00 + sch * 5 * 21 + idx * 21);
    uint8_t name_buf[21] = {0};
    strncpy((char*)name_buf, se_name, 20);
    fram_write(name_addr, name_buf, 21);

    ESP_LOGI(TAG, "SE ajouté SCH%d | Nom: %s | Prio: %d | Val: %.1f%%", sch, se_name, prio, val);
#endif

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t handler_se_delete(httpd_req_t *req)
{
    char buf[64];
    if (read_body(req, buf, sizeof(buf)) < 0) return ESP_FAIL;
    cJSON *root = cJSON_Parse(buf);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON"); return ESP_FAIL; }
    int sch = cJSON_GetObjectItem(root, "schedule") ? cJSON_GetObjectItem(root, "schedule")->valueint : 0;
    int idx = cJSON_GetObjectItem(root, "index")    ? cJSON_GetObjectItem(root, "index")->valueint    : -1;
    cJSON_Delete(root);
#if BACNET_EXCEPTION_SCHEDULE_SIZE > 0
    SCHEDULE_DESCR *desc = Schedule_Object((uint32_t)sch);
    if (desc && idx >= 0 && idx < (int)desc->Exception_Count) {
        for (int i = idx; i < (int)desc->Exception_Count - 1; i++)
            memcpy(&desc->Exception_Schedule[i], &desc->Exception_Schedule[i+1], sizeof(BACNET_SPECIAL_EVENT));
        desc->Exception_Count--;
        sched_persist_save_special_events((uint32_t)sch, desc);
    }
#endif
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t handler_se_clear(httpd_req_t *req)
{
    char buf[32]; read_body(req, buf, sizeof(buf));
    cJSON *root = cJSON_Parse(buf);
    int sch = (root && cJSON_GetObjectItem(root, "schedule"))
              ? cJSON_GetObjectItem(root, "schedule")->valueint : 0;
    if (root) cJSON_Delete(root);
#if BACNET_EXCEPTION_SCHEDULE_SIZE > 0
    SCHEDULE_DESCR *desc = Schedule_Object((uint32_t)sch);
    if (desc) {
        desc->Exception_Count = 0;
        memset(desc->Exception_Schedule, 0, sizeof(desc->Exception_Schedule));
        sched_persist_save_special_events((uint32_t)sch, desc);
    }
#endif
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t handler_trendlog(httpd_req_t *req)
{
    if (!s_tl_mutex) s_tl_mutex = xSemaphoreCreateMutex();
    if (s_tl_count == 0) {
        extern float Analog_Value_Present_Value(uint32_t);
        http_trendlog_record(Analog_Value_Present_Value(0), Analog_Value_Present_Value(1));
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *arr0 = cJSON_CreateArray();
    cJSON *arr1 = cJSON_CreateArray();
    if (xSemaphoreTake(s_tl_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        int start = (s_tl_count < TL_HIST_MAX) ? 0 : s_tl_head;
        for (int i = 0; i < s_tl_count; i++) {
            int idx = (start + i) % TL_HIST_MAX;
            tl_hist_entry_t *e = &s_tl_hist[idx];
            if (!e->valid) continue;
            char label[24];
            snprintf(label, sizeof(label), "%s %s", e->date_str, e->time_str);
            cJSON *e0 = cJSON_CreateObject();
            cJSON_AddStringToObject(e0, "t", label);
            cJSON_AddNumberToObject(e0, "v", e->av0);
            cJSON_AddItemToArray(arr0, e0);
            cJSON *e1 = cJSON_CreateObject();
            cJSON_AddStringToObject(e1, "t", label);
            cJSON_AddNumberToObject(e1, "v", e->av1);
            cJSON_AddItemToArray(arr1, e1);
        }
        xSemaphoreGive(s_tl_mutex);
    }
    cJSON *obj0 = cJSON_CreateObject();
    cJSON_AddStringToObject(obj0, "name", "Zone A (AV0)");
    cJSON_AddNumberToObject(obj0, "count", s_tl_count);
    cJSON_AddItemToObject(obj0, "entries", arr0);
    cJSON_AddItemToObject(root, "tl0", obj0);
    cJSON *obj1 = cJSON_CreateObject();
    cJSON_AddStringToObject(obj1, "name", "Zone B (AV1)");
    cJSON_AddNumberToObject(obj1, "count", s_tl_count);
    cJSON_AddItemToObject(obj1, "entries", arr1);
    cJSON_AddItemToObject(root, "tl1", obj1);
    char *json = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, json, strlen(json));
    free(json); cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t http_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port      = 80;
    config.max_uri_handlers = 17;
    config.stack_size       = 8192;
    config.lru_purge_enable = true;
    config.max_open_sockets = 3;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Echec démarrage serveur HTTP");
        return ESP_FAIL;
    }

    httpd_uri_t routes[] = {
        { .uri="/favicon.ico",         .method=HTTP_GET,  .handler=handler_favicon    },
        { .uri="/",           .method=HTTP_GET,  .handler=handler_root    },
        { .uri="/api/status", .method=HTTP_GET,  .handler=handler_status  },
        { .uri="/api/solar",  .method=HTTP_GET,  .handler=handler_solar_get},
        { .uri="/api/solar",  .method=HTTP_POST, .handler=handler_solar_post},
        { .uri="/api/mode",   .method=HTTP_POST, .handler=handler_mode    },
        { .uri="/api/av",     .method=HTTP_POST, .handler=handler_av      },
        { .uri="/api/events",         .method=HTTP_GET,  .handler=handler_events    },
        { .uri="/api/special_events", .method=HTTP_GET,  .handler=handler_se_get    },
        { .uri="/api/special_events", .method=HTTP_POST, .handler=handler_se_post   },
        { .uri="/api/se_delete",      .method=HTTP_POST, .handler=handler_se_delete },
        { .uri="/api/se_clear",       .method=HTTP_POST, .handler=handler_se_clear  },
        { .uri="/api/trendlog",       .method=HTTP_GET,  .handler=handler_trendlog  },
    };

    for (int i = 0; i < sizeof(routes)/sizeof(routes[0]); i++) {
        httpd_register_uri_handler(s_server, &routes[i]);
    }

    ESP_LOGI(TAG, "Serveur HTTP demarré sur port 80");
    return ESP_OK;
}

void http_server_stop(void)
{
    if (s_server) { httpd_stop(s_server); s_server = NULL; }
}