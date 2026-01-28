 #pragma once
 
 #include <coreinit/debug.h>
 #include <string.h>
 #include <whb/log.h>
 
 #ifdef __cplusplus
 extern "C" {
 #endif
 
 #define LOG_APP_TYPE "P"
 #define LOG_APP_NAME "HomebrewLoader"
 
 #define __FILENAME__ ({                                \
     const char *__filename = __FILE__;                 \
     const char *__pos      = strrchr(__filename, '/'); \
     if (!__pos) __pos = strrchr(__filename, '\\');     \
     __pos ? __pos + 1 : __filename;                    \
 })
 
 #ifdef DEBUG
 #define LOG_EX_DEFAULT(function, prefix, postfix, FMT, ARGS...) function(prefix "[%s %s] %s: " FMT postfix, LOG_APP_TYPE, LOG_APP_NAME, __FILENAME__, ##ARGS)
 #define LOG_EX(FILENAME, FUNCTION, LINE, function, prefix, postfix, FMT, ARGS...) function(prefix "[%s %s] %s %s@%04d: " FMT postfix, LOG_APP_TYPE, LOG_APP_NAME, FILENAME, FUNCTION, LINE, ##ARGS)
 
 #define LOG(function, FMT, ARGS...)                                       function("[%s %s] %s: " FMT, LOG_APP_TYPE, LOG_APP_NAME, __FILENAME__, ##ARGS)
 #define LOG_WRITE(function, FMT, ARGS...)                                 function("[%s %s] %s: " FMT, LOG_APP_TYPE, LOG_APP_NAME, __FILENAME__, ##ARGS)
 
 #define DEBUG_FUNCTION_LINE_VERBOSE(FMT, ARGS...) while (0)
 #define DEBUG_FUNCTION_LINE_VERBOSE_EX(FMT, ARGS...) while (0)
 #else
 #define LOG_EX_DEFAULT(function, prefix, postfix, FMT, ARGS...) while (0)
 #define LOG_EX(FILENAME, FUNCTION, LINE, function, prefix, postfix, FMT, ARGS...) while (0)
 
 #define LOG(function, FMT, ARGS...) while (0)
 #define LOG_WRITE(function, FMT, ARGS...) while (0)
 
 #define DEBUG_FUNCTION_LINE_VERBOSE(FMT, ARGS...) while (0)
 #define DEBUG_FUNCTION_LINE_VERBOSE_EX(FMT, ARGS...) while (0)
 #endif
 
 #define DEBUG_FUNCTION_LINE(FMT, ARGS...)                                      LOG(WHBLogPrintf, FMT, ##ARGS)
 
 #define DEBUG_FUNCTION_LINE_WRITE(FMT, ARGS...)                                LOG(WHBLogWritef, FMT, ##ARGS)
 
 #define DEBUG_FUNCTION_LINE_ERR(FMT, ARGS...)                                  LOG_EX_DEFAULT(WHBLogPrintf, "##ERROR## ", "", FMT, ##ARGS)
 #define DEBUG_FUNCTION_LINE_WARN(FMT, ARGS...)                                 LOG_EX_DEFAULT(WHBLogPrintf, "##WARN ## ", "", FMT, ##ARGS)
 #define DEBUG_FUNCTION_LINE_INFO(FMT, ARGS...)                                 LOG_EX_DEFAULT(WHBLogPrintf, "##INFO ## ", "", FMT, ##ARGS)
 
 #define DEBUG_FUNCTION_LINE_ERR_LAMBDA(FILENAME, FUNCTION, LINE, FMT, ARGS...) LOG_EX(FILENAME, FUNCTION, LINE, WHBLogPrintf, "##ERROR## ", "", FMT, ##ARGS);
 
 #ifdef __cplusplus
 }
 #endif
 
 void initLogging();
 
 void deinitLogging();
 
