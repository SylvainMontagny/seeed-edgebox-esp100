#ifndef FRAM_LAYOUT_H
#define FRAM_LAYOUT_H
#include <stdint.h>

#define FRAM_MAGIC_ADDR        0x0000
#define FRAM_MAGIC_VALUE       0xBA0C4E01

#define FRAM_AV_STATE_ADDR     0x0010
typedef struct { float av0; float av1; uint8_t valid; uint8_t _pad[3]; } fram_av_state_t;

#define FRAM_BV_STATE_ADDR     0x0020
typedef struct { uint8_t manual_mode; uint8_t valid; uint8_t _pad[2]; } fram_bv_state_t;

#define FRAM_RTC_BACKUP_ADDR   0x0030
typedef struct { uint8_t year,month,day,hour,min,sec,valid,_pad; } fram_rtc_backup_t;

#define FRAM_EVENT_HEAD_ADDR   0x00F0
#define FRAM_EVENT_LOG_ADDR    0x0100
#define FRAM_EVENT_LOG_SIZE    0x1000
#define FRAM_EVENT_ENTRY_SIZE  16
#define FRAM_EVENT_MAX_ENTRIES (FRAM_EVENT_LOG_SIZE/FRAM_EVENT_ENTRY_SIZE)

typedef enum {
    EVENT_AV0_CHANGE=0x01, EVENT_AV1_CHANGE=0x02, EVENT_MODE_CHANGE=0x03,
    EVENT_BOOT=0x04, EVENT_NTP_SYNC=0x05, EVENT_4G_LOST=0x06,
    EVENT_SOLAR_ON=0x07, EVENT_SOLAR_OFF=0x08,
} fram_event_type_t;

typedef struct __attribute__((packed)) {
    uint8_t year,month,day,hour,min,sec,event_type,extra_u8;
    float value; uint8_t valid; uint8_t _pad[3];
} fram_event_entry_t;

#define FRAM_WEEKLY_SCH_ADDR   0x1100
#define FRAM_SCH_MAX_TV        10
#define FRAM_SCH_DAYS          7
#define FRAM_SCH_TV_SIZE       8
#define FRAM_SCH_DAY_SIZE      (1+FRAM_SCH_MAX_TV*FRAM_SCH_TV_SIZE)
#define FRAM_SCH_ONE_SIZE      (FRAM_SCH_DAYS*FRAM_SCH_DAY_SIZE)
#define FRAM_SCH_TOTAL_SIZE    (2*FRAM_SCH_ONE_SIZE)
#define FRAM_SCH_ADDR(i)       (FRAM_WEEKLY_SCH_ADDR+(i)*FRAM_SCH_ONE_SIZE)
typedef struct __attribute__((packed)) { uint8_t hour,min,sec,hundredths; float value; } fram_tv_entry_t;

#define FRAM_SE_COUNT_ADDR(i)     (0x1560+(i))
#define FRAM_SPECIAL_EVENTS_ADDR  0x1570
#define FRAM_SE_MAX_COUNT         5
#define FRAM_SE_MAX_TV            10
#define FRAM_SE_ONE_SIZE          88
#define FRAM_SE_SCH_SIZE          (FRAM_SE_MAX_COUNT*FRAM_SE_ONE_SIZE)
#define FRAM_SE_TOTAL_SIZE        (2*FRAM_SE_SCH_SIZE)
#define FRAM_SE_ADDR(i)           (FRAM_SPECIAL_EVENTS_ADDR+(i)*FRAM_SE_SCH_SIZE)
#define FRAM_SE_ENTRY_ADDR(i,j)   (FRAM_SE_ADDR(i)+(j)*FRAM_SE_ONE_SIZE)
typedef struct __attribute__((packed)) {
    uint8_t valid,period_tag,year,month,day,wday,tv_count,priority;
    fram_tv_entry_t tv[FRAM_SE_MAX_TV];
} fram_se_entry_t;

/* Zone config solaire — 16 bytes */
#define FRAM_SOLAR_CONFIG_ADDR   0x1C60

#endif