#ifndef _DEBUG_DEF_H_
#define _DEBUG_DEF_H_

#include "global_def.h"

//--------------------------------------------
#if (SYS_DEBUG_MODE)
    #define debug_log_println               debug_uart.println
    #define debug_log_printf                debug_uart.printf
    #define debug_log_print                 debug_uart.print
#else
    #define debug_log_println(...)          ;
    #define debug_log_printf(...)           ;
    #define debug_log_print(...)            ;
#endif

//--------------------------------------------
#endif