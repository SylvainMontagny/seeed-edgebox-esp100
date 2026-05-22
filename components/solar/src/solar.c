#include "solar.h"
#include "fram_layout.h"
#include "fram_fm24cl64b.h"
#include "esp_log.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "solar";
#define PI         3.14159265358979323846
#define D2R(x)     ((x)*PI/180.0)
#define R2D(x)     ((x)*180.0/PI)


typedef struct {
    float lon_min;
    float lon_max;
    float lat_min;
    float lat_max;
    const char *posix_tz;  
} timezone_region_t;

static const timezone_region_t timezone_table[] = {
    /* Western Europe / GMT+0 regions */
    {-10, 0, 30, 65, "WET0WEST,M3.5.0,M10.5.0/3"},       /* UK, Ireland, Portugal */
    {0, 15, 35, 70, "CET-1CEST,M3.5.0,M10.5.0/3"},      /* Central Europe */
    {15, 30, 35, 70, "EET-2EEST,M3.5.0,M10.5.0/3"},     /* Eastern Europe */
    {30, 45, 35, 70, "EEST-3,M3.5.0,M10.5.0/3"},        /* Far Eastern Europe / West Asia */
    
    /* Middle East / Asia */
    {45, 60, 20, 50, "IRST-3:30IRDT,J79/24,J263/24"},   /* Iran */
    {60, 75, 20, 50, "GST-4:30"},                        /* Gulf */
    {75, 90, 20, 50, "IST-5:30"},                        /* India */
    
    /* East Asia */
    {90, 123, 15, 55, "CST-8"},                          /* China, Singapore, Malaysia */
    {123, 150, 24, 55, "JST-9"},                         /* Japan, Korea */
    {150, 180, -47, -10, "NZST-12NZDT-13,M9.1.0,M4.1.0/3"}, /* New Zealand */
    
    /* Americas */
    {-120, -105, 15, 60, "PST8PDT,M3.2.0,M11.1.0"},      /* Pacific Time */
    {-105, -90, 15, 60, "MST7MDT,M3.2.0,M11.1.0"},       /* Mountain Time */
    {-90, -75, 15, 60, "CST6CDT,M3.2.0,M11.1.0"},        /* Central Time */
    {-75, -60, 15, 60, "EST5EDT,M3.2.0,M11.1.0"},        /* Eastern Time */
    
    /* South America */
    {-75, -60, -25, 5, "ART3"},                          /* Argentina */
    {-75, -60, -35, -25, "BRST3BRDT,M10.3.0/0,M2.3.0/0"}, /* Brazil */
    
    /* Australia */
    {110, 125, -35, -10, "AWST-8"},                      /* Western Australia */
    {125, 135, -35, -10, "ACST-9:30ACDT-10:30,M10.1.0,M4.1.0/3"}, /* Central Australia */
    {135, 155, -45, -10, "AEST-10AEDT-11,M10.1.0,M4.1.0/3"}, /* Eastern Australia */
    {150, 180, -47, -10, "NZST-12NZDT-13,M9.1.0,M4.1.0/3"}, /* New Zealand */
    
    {-180, 180, -90, 90, "UTC0"}
};

static const int timezone_table_size = sizeof(timezone_table) / sizeof(timezone_region_t);
static char s_custom_tz[32] = {0};

const char* solar_get_timezone_posix(float latitude, float longitude)
{
    ESP_LOGD(TAG, "Looking up timezone for lat=%.2f lon=%.2f", latitude, longitude);
    
    /* Normalize longitude to [-180, 180) */
    while (longitude >= 180.0f) longitude -= 360.0f;
    while (longitude < -180.0f) longitude += 360.0f;
    
    /* Search for matching region */
    for (int i = 0; i < timezone_table_size - 1; i++) {
        const timezone_region_t *tz = &timezone_table[i];
        if (longitude >= tz->lon_min && longitude < tz->lon_max &&
            latitude >= tz->lat_min && latitude < tz->lat_max) {
            ESP_LOGI(TAG, "Timezone for lat=%.2f lon=%.2f: %s", latitude, longitude, tz->posix_tz);
            return tz->posix_tz;
        }
    }

    int offset = (int)floor((longitude + 7.5f) / 15.0f);
    int posix = -offset;
    if (posix == 0) {
        strncpy(s_custom_tz, "UTC0", sizeof(s_custom_tz));
    } else {
        snprintf(s_custom_tz, sizeof(s_custom_tz), "UTC%+d", posix);
    }
    ESP_LOGW(TAG, "No region match for lat=%.2f lon=%.2f, using approximate timezone %s", latitude, longitude, s_custom_tz);
    return s_custom_tz;
}

static solar_config_t s_cfg = {
    .latitude = 49.0f,
    .longitude = 2.0f,
    .offset_before_sunset = 0,
    .offset_after_sunrise = 0,
    .enabled = 1,
    .valid = 0xAA,
    ._pad = {0}
};
static solar_times_t s_today = {0};
static int s_cache_ymd = -1;
static bool s_tz_cached = false;
static float s_tz_cached_latitude = 0.0f;
static float s_tz_cached_longitude = 0.0f;
static char s_tz_cached_string[128] = {0};

static double solar_event_utc(int yr, int mo, int dy, double lat, double lon, bool is_rise)
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

static bool solar_set_timezone_env(float latitude, float longitude,
                                   char *old_tz, size_t old_tz_size)
{
    const char *current_tz = getenv("TZ");
    if (old_tz && old_tz_size > 0) {
        if (current_tz) {
            strncpy(old_tz, current_tz, old_tz_size - 1);
            old_tz[old_tz_size - 1] = '\0';
        } else {
            old_tz[0] = '\0';
        }
    }

    const char *tz_string = solar_get_timezone_posix(latitude, longitude);
    if (!tz_string) return false;

    setenv("TZ", tz_string, 1);
    tzset();
    return true;
}

static void solar_restore_timezone_env(const char *old_tz)
{
    if (old_tz && old_tz[0]) {
        setenv("TZ", old_tz, 1);
    } else {
        unsetenv("TZ");
    }
    tzset();
}

static bool solar_activate_timezone(float latitude, float longitude)
{
    if (s_tz_cached && fabsf(latitude - s_tz_cached_latitude) < 1e-6f &&
        fabsf(longitude - s_tz_cached_longitude) < 1e-6f) {
        return true;
    }

    const char *tz_string = solar_get_timezone_posix(latitude, longitude);
    if (!tz_string) {
        return false;
    }

    setenv("TZ", tz_string, 1);
    tzset();

    s_tz_cached = true;
    s_tz_cached_latitude = latitude;
    s_tz_cached_longitude = longitude;
    strncpy(s_tz_cached_string, tz_string, sizeof(s_tz_cached_string) - 1);
    s_tz_cached_string[sizeof(s_tz_cached_string) - 1] = '\0';
    return true;
}

static time_t solar_timegm(const struct tm *input_tm)
{
    struct tm tm_copy = *input_tm;
    char old_tz[128];
    const char *current_tz = getenv("TZ");
    if (current_tz) {
        strncpy(old_tz, current_tz, sizeof(old_tz) - 1);
        old_tz[sizeof(old_tz) - 1] = '\0';
    } else {
        old_tz[0] = '\0';
    }

    setenv("TZ", "UTC0", 1);
    tzset();
    time_t result = mktime(&tm_copy);
    solar_restore_timezone_env(old_tz);
    return result;
}

static double tz_offset_for_coords(int yr, int mo, int dy,
                                   double lat, double lon)
{
    char old_tz[128];
    if (!solar_set_timezone_env(lat, lon, old_tz, sizeof(old_tz))) {
        return 0.0;
    }

    struct tm local_tm = {.tm_year = yr - 1900, .tm_mon = mo - 1,
                          .tm_mday = dy, .tm_hour = 12,
                          .tm_min = 0, .tm_sec = 0, .tm_isdst = -1};
    time_t local_epoch = mktime(&local_tm);
    struct tm utc_tm = local_tm;
    time_t utc_epoch   = solar_timegm(&utc_tm);
    solar_restore_timezone_env(old_tz);

    if (local_epoch == (time_t)-1 || utc_epoch == (time_t)-1) {
        return 0.0;
    }
    return difftime(utc_epoch, local_epoch) / 3600.0;
}

solar_times_t solar_calc_by_coords(int yr, int mo, int dy,
                                   float latitude, float longitude,
                                   int16_t offset_before_sunset,
                                   int16_t offset_after_sunrise)
{
    solar_times_t r={0}; r.valid=false;
    double rise_u=solar_event_utc(yr,mo,dy,latitude,longitude,true);
    double set_u =solar_event_utc(yr,mo,dy,latitude,longitude,false);
    if(rise_u<0||set_u<0){ESP_LOGW(TAG,"Polar day/night - sun never rises or sets");return r;}
    double tz = tz_offset_for_coords(yr,mo,dy,latitude,longitude);
    double rise_l=rise_u+tz+(double)offset_after_sunrise/60.0;
    double set_l =set_u +tz-(double)offset_before_sunset/60.0;
    if(rise_l<0){rise_l+=24;} if(rise_l>=24){rise_l-=24;}
    if(set_l<0){set_l+=24;} if(set_l>=24){set_l-=24;}
    r.sunrise_h=(uint8_t)(int)rise_l;
    r.sunrise_m=(uint8_t)((rise_l-(int)rise_l)*60.0);
    r.sunset_h =(uint8_t)(int)set_l;
    r.sunset_m =(uint8_t)((set_l-(int)set_l)*60.0);
    r.valid=true;
    ESP_LOGI(TAG,"%04d-%02d-%02d : Sunrise %02d:%02d / Sunset %02d:%02d",
             yr,mo,dy,r.sunrise_h,r.sunrise_m,r.sunset_h,r.sunset_m);
    return r;
}

solar_times_t solar_calc(int yr,int mo,int dy)
{
    return solar_calc_by_coords(yr, mo, dy,
                                s_cfg.latitude, s_cfg.longitude,
                                s_cfg.offset_before_sunset,
                                s_cfg.offset_after_sunrise);
}

bool solar_get_local_time(float latitude, float longitude, struct tm *out)
{
    if (!out) return false;
    if (!solar_activate_timezone(latitude, longitude)) {
        return false;
    }

    time_t now = time(NULL);
    struct tm *local = localtime(&now);
    if (!local) {
        return false;
    }
    *out = *local;
    return true;
}

solar_times_t solar_get_today(void)
{
    struct tm local;
    if (!solar_get_local_time(s_cfg.latitude, s_cfg.longitude, &local)) {
        return s_today;
    }

    int ymd = (local.tm_year + 1900) * 10000 + (local.tm_mon + 1) * 100 + local.tm_mday;
    if (ymd != s_cache_ymd) {
        s_today = solar_calc(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
        s_cache_ymd = ymd;
    }
    return s_today;
}

void solar_invalidate_cache(void){s_cache_ymd=-1;}

bool solar_is_night_now(void)
{
    if(!s_cfg.enabled) return false;
    solar_times_t st = solar_get_today();
    if(!st.valid) return false;

    struct tm local;
    if (!solar_get_local_time(s_cfg.latitude, s_cfg.longitude, &local)) {
        return false;
    }

    int cur = local.tm_hour * 60 + local.tm_min;
    int ss = st.sunset_h * 60 + st.sunset_m;
    int sr = st.sunrise_h * 60 + st.sunrise_m;
    if (ss > sr) return (cur >= ss || cur < sr);
    else         return (cur >= ss && cur < sr);
}

esp_err_t solar_save_config(const solar_config_t *cfg)
{
    if(!cfg)return ESP_ERR_INVALID_ARG;
    s_cfg=*cfg; s_cfg.valid=0xAA;
    solar_invalidate_cache();
    esp_err_t r=fram_write(FRAM_SOLAR_CONFIG_ADDR,(uint8_t*)&s_cfg,sizeof(s_cfg));
    if(r==ESP_OK)ESP_LOGI(TAG,"Saved lat=%.4f lon=%.4f",s_cfg.latitude,s_cfg.longitude);
    return r;
}

const solar_config_t *solar_get_config(void){return &s_cfg;}

esp_err_t solar_init(void)
{
    solar_config_t cfg;
    esp_err_t r=fram_read(FRAM_SOLAR_CONFIG_ADDR,(uint8_t*)&cfg,sizeof(cfg));
    if(r==ESP_OK&&cfg.valid==0xAA){
        s_cfg=cfg;
        ESP_LOGI(TAG,"Restored lat=%.4f lon=%.4f enabled=%d",s_cfg.latitude,s_cfg.longitude,s_cfg.enabled);
    } else {
#ifdef CONFIG_SOLAR_DEFAULT_LATITUDE
    s_cfg.latitude = (float)atof(CONFIG_SOLAR_DEFAULT_LATITUDE);
#else
    s_cfg.latitude = 48.85f;
#endif
#ifdef CONFIG_SOLAR_DEFAULT_LONGITUDE
    s_cfg.longitude = (float)atof(CONFIG_SOLAR_DEFAULT_LONGITUDE);
#else
    s_cfg.longitude = 2.35f;
#endif
#ifdef CONFIG_SOLAR_DEFAULT_ENABLED
    s_cfg.enabled = CONFIG_SOLAR_DEFAULT_ENABLED;
#else
    s_cfg.enabled = 1;
#endif
    s_cfg.offset_before_sunset = 0;
    s_cfg.offset_after_sunrise = 0;
    s_cfg.valid = 0xAA;
    ESP_LOGI(TAG, "No FRAM config found - using defaults lat=%.4f lon=%.4f enabled=%d", s_cfg.latitude, s_cfg.longitude, s_cfg.enabled);
    }

/* If configured, overwrite any FRAM values with menuconfig defaults on boot */
#ifdef CONFIG_SOLAR_OVERRIDE_FRAM_ON_BOOT
    {
#ifdef CONFIG_SOLAR_DEFAULT_LATITUDE
        s_cfg.latitude = (float)atof(CONFIG_SOLAR_DEFAULT_LATITUDE);
#endif
#ifdef CONFIG_SOLAR_DEFAULT_LONGITUDE
        s_cfg.longitude = (float)atof(CONFIG_SOLAR_DEFAULT_LONGITUDE);
#endif
#ifdef CONFIG_SOLAR_DEFAULT_ENABLED
        s_cfg.enabled = CONFIG_SOLAR_DEFAULT_ENABLED ? 1 : 0;
#endif
        /* Persist the overridden configuration to FRAM */
        esp_err_t w = solar_save_config(&s_cfg);
        if (w == ESP_OK) {
            ESP_LOGI(TAG, "Overwrote FRAM solar config with menuconfig values");
        } else {
            ESP_LOGW(TAG, "Failed to write overridden solar config to FRAM (%d)", w);
        }
    }
#endif
    return ESP_OK;
}

void solar_update_timezone_env(void)
{
    if (solar_activate_timezone(s_cfg.latitude, s_cfg.longitude)) {
        ESP_LOGI(TAG, "Updating timezone to: %s (lat=%.2f lon=%.2f)",
                 s_tz_cached_string, s_cfg.latitude, s_cfg.longitude);
    } else {
        ESP_LOGW(TAG, "Failed to get timezone for update");
    }
}

void solar_get_offsets(int16_t *offset_before_sunset, int16_t *offset_after_sunrise)
{
    if (!offset_before_sunset || !offset_after_sunrise) {
        return;
    }

    *offset_before_sunset = s_cfg.offset_before_sunset;
    *offset_after_sunrise = s_cfg.offset_after_sunrise;
}

void solar_set_offsets(int16_t offset_before_sunset, int16_t offset_after_sunrise)
{
    if (s_cfg.offset_before_sunset != offset_before_sunset ||
        s_cfg.offset_after_sunrise != offset_after_sunrise) {
        s_cfg.offset_before_sunset = offset_before_sunset;
        s_cfg.offset_after_sunrise = offset_after_sunrise;
    
        fram_write(FRAM_SOLAR_CONFIG_ADDR, (uint8_t*)&s_cfg, sizeof(s_cfg));
        solar_invalidate_cache();
        ESP_LOGI(TAG, "Offsets updated: before_sunset=%d, after_sunrise=%d",
                 offset_before_sunset, offset_after_sunrise);
    }
}