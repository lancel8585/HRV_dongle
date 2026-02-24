#ifndef _GLOBAL_DEF_H_
#define _GLOBAL_DEF_H_

#include "Arduino.h"

//--------------------------------------------
//GPIO 33-37 is used to PSRAM(Don't use)

//uart define
#define debug_uart                   Serial//Serial0
#define BAUDRATE_115200              115200
#define BAUDRATE_9600                9600
#define BAUDARTE_4800                4800
#define DEBUG_LOG_BAUDRATE           BAUDRATE_115200

//other
#define DEFAULT_CORE_ID              1
#define ANTHOR_CORE_ID               0

//--------------------------------------------
typedef struct {
    //system parameter
    String device_name                          = "TEST";
}param_t;

//--------------------------------------------
extern param_t param;

//--------------------------------------------
#endif
