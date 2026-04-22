#include "solar.h"
#include "fram_layout.h"
#include "fram_fm24cl64b.h"
#include "esp_log.h"
#include <math.h>
#include <string.h>
#include <time.h>

/* ★ Décommenter pour activer le mode test (cycle 2 min) */
#define SOLAR_TEST_MODE

static const char *TAG = "solar";
#define PI         3.14159265358979323846
#define D2R(x)     ((x)*PI/180.0)
#define R2D(x)     ((x)*180.0/PI)

static solar_config_t s_cfg = {
    .latitude=45.78f, .longitude=5.93f,   /* Entrelacs 73410 */
    .offset_before_sunset=0, .offset_after_sunrise=0,
    .enabled=1, .valid=0xAA, ._pad={0}
};
static solar_times_t s_today = {0};
static int s_cache_day = -1;

/* Retourne heure décimale UTC du lever (is_rise=true) ou coucher */
static double solar_event_utc(int yr,int mo,int dy,double lat,double lon,bool is_rise)
{
    int a  = (14-mo)/12, y=yr+4800-a, m2=mo+12*a-3;
    double jdn = dy+(153*m2+2)/5+365*y+y/4-y/100+y/400-32045.0;
    double jc  = (jdn-2451545.0)/36525.0;
    double l0  = fmod(280.46646+jc*(36000.76983+jc*0.0003032),360.0);
    double m   = 357.52911+jc*(35999.05029-0.0001537*jc);
    double mr  = D2R(m);
    double c   = sin(mr)*(1.914602-jc*(0.004817+0.000014*jc))
               + sin(2*mr)*(0.019993-0.000101*jc)+sin(3*mr)*0.000289;
    double sl  = l0+c;
    double ome = 125.04-1934.136*jc;
    double lam = sl-0.00569-0.00478*sin(D2R(ome));
    double ep0 = 23.0+(26.0+(21.448-jc*(46.815+jc*(0.00059-jc*0.001813)))/60.0)/60.0;
    double eps = ep0+0.00256*cos(D2R(ome));
    double decl= R2D(asin(sin(D2R(eps))*sin(D2R(lam))));
    double lr  = D2R(lat), dr=D2R(decl);
    double cha = (cos(D2R(90.833))-sin(lr)*sin(dr))/(cos(lr)*cos(dr));
    if(cha<-1.0||cha>1.0) return -1.0;
    double ha  = R2D(acos(cha));
    double e   = 0.016708634-jc*(0.000042037+0.0000001267*jc);
    double l0r = D2R(l0);
    double yv  = tan(D2R(eps/2.0)); yv*=yv;
    double eq  = 4.0*R2D(yv*sin(2*l0r)-2*e*sin(mr)+4*e*yv*sin(mr)*cos(2*l0r)
                         -0.5*yv*yv*sin(4*l0r)-1.25*e*e*sin(2*mr));
    double noon = 720.0-4.0*lon-eq;
    double ev   = is_rise ? noon-ha*4.0 : noon+ha*4.0;
    ev = fmod(ev,1440.0); if(ev<0){ev+=1440.0;}
    return ev/60.0;
}

static int tz_offset(int yr,int mo,int dy)
{
    struct tm t={.tm_year=yr-1900,.tm_mon=mo-1,.tm_mday=dy,.tm_hour=12,.tm_isdst=-1};
    time_t lt=mktime(&t); if(lt<0)return 1;
    struct tm *g=gmtime(&lt);
    int off=t.tm_hour-g->tm_hour;
    if(off < -12) { off += 24; }
    if(off > 12)  { off -= 24; }
    return off;
}

solar_times_t solar_calc(int yr,int mo,int dy)
{
    solar_times_t r={0}; r.valid=false;
    if(!s_cfg.enabled)return r;
    double lat=s_cfg.latitude, lon=s_cfg.longitude;
    double rise_u=solar_event_utc(yr,mo,dy,lat,lon,true);
    double set_u =solar_event_utc(yr,mo,dy,lat,lon,false);
    if(rise_u<0||set_u<0){ESP_LOGW(TAG,"Soleil polaire");return r;}
    int tz=tz_offset(yr,mo,dy);
    double rise_l=rise_u+tz+(double)s_cfg.offset_after_sunrise/60.0;
    double set_l =set_u +tz-(double)s_cfg.offset_before_sunset/60.0;
    if(rise_l<0){rise_l+=24;} if(rise_l>=24){rise_l-=24;}
    if(set_l<0){set_l+=24;} if(set_l>=24){set_l-=24;}
    r.sunrise_h=(uint8_t)(int)rise_l;
    r.sunrise_m=(uint8_t)((rise_l-(int)rise_l)*60.0);
    r.sunset_h =(uint8_t)(int)set_l;
    r.sunset_m =(uint8_t)((set_l -(int)set_l )*60.0);
    r.valid=true;
    ESP_LOGI(TAG,"Solaire %04d-%02d-%02d : lever %02d:%02d / coucher %02d:%02d",
             yr,mo,dy,r.sunrise_h,r.sunrise_m,r.sunset_h,r.sunset_m);
    return r;
}

solar_times_t solar_get_today(void)
{
    time_t now=time(NULL); struct tm *t=localtime(&now);
    if(t->tm_mday != s_cache_day) {
        s_today=solar_calc(t->tm_year+1900,t->tm_mon+1,t->tm_mday);
        s_cache_day=t->tm_mday;
    }
    return s_today;
}

void solar_invalidate_cache(void){s_cache_day=-1;}

bool solar_is_night_now(void)
{
    if(!s_cfg.enabled)return false;

#ifdef SOLAR_TEST_MODE
    /* MODE TEST — cycle 1 minute
     * sec 0-29 = NUIT (100%) | sec 30-59 = JOUR (0%)
     * Commenter #define SOLAR_TEST_MODE pour la production */
    time_t now_t=time(NULL); struct tm *tt=localtime(&now_t);
    /* Cycle 1 minute : sec 0-29 = NUIT, sec 30-59 = JOUR */
   int cycle_sec = (tt->tm_min % 10) * 60 + tt->tm_sec;
bool is_night_test = (cycle_sec < 300); // 0-299s = NUIT, 300-599s = JOUR
if (tt->tm_sec == 0 && (tt->tm_min % 5 == 0)) {
        ESP_LOGI("solar", "[TEST] %02d:%02d:%02d → %s",
                 tt->tm_hour, tt->tm_min, tt->tm_sec,
                 is_night_test ? "NUIT" : "JOUR");
    }
    return is_night_test;
#else
    solar_times_t st=solar_get_today();
    if(!st.valid)return false;
    time_t now=time(NULL); struct tm *t=localtime(&now);
    int cur=t->tm_hour*60+t->tm_min;
    int ss =st.sunset_h *60+st.sunset_m;
    int sr =st.sunrise_h*60+st.sunrise_m;
    if(ss>sr) return(cur>=ss||cur<sr);
    else      return(cur>=ss&&cur<sr);
#endif
}

esp_err_t solar_save_config(const solar_config_t *cfg)
{
    if(!cfg)return ESP_ERR_INVALID_ARG;
    s_cfg=*cfg; s_cfg.valid=0xAA;
    solar_invalidate_cache();
    esp_err_t r=fram_write(FRAM_SOLAR_CONFIG_ADDR,(uint8_t*)&s_cfg,sizeof(s_cfg));
    if(r==ESP_OK)ESP_LOGI(TAG,"Sauvegardé lat=%.4f lon=%.4f",s_cfg.latitude,s_cfg.longitude);
    return r;
}

const solar_config_t *solar_get_config(void){return &s_cfg;}

esp_err_t solar_init(void)
{
    solar_config_t cfg;
    esp_err_t r=fram_read(FRAM_SOLAR_CONFIG_ADDR,(uint8_t*)&cfg,sizeof(cfg));
    if(r==ESP_OK&&cfg.valid==0xAA){
        s_cfg=cfg;
        ESP_LOGI(TAG,"Restauré lat=%.4f lon=%.4f enabled=%d",s_cfg.latitude,s_cfg.longitude,s_cfg.enabled);
    } else {
        ESP_LOGW(TAG,"Pas de config FRAM — défaut Paris (48.85, 2.35)");
    }
    return ESP_OK;
}