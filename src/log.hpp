#pragma once

#include <cstdarg>
#include <cstdint>

#include <functional>
#include <memory>
#include <string_view>

#ifdef __GNUC__
    #define ATTRIB_PRINTF( start, end )                                        \
        __attribute__(( format( printf, start, end ) ))
#else
    #define ATTRIB_PRINTF( start, end )
#endif

enum LogPriority
{
    LOG_SILENT,
    LOG_ERROR,
    LOG_WARNING,
    LOG_INFO,
    LOG_DEBUG,
};

struct LogConVar_t;

class LogScope
{
public:
    LogScope( std::string_view psvName, LogPriority eMaxPriority = LOG_INFO );
    LogScope(
        std::string_view psvName,
        std::string_view psvPrefix,
        LogPriority      eMaxPriority = LOG_INFO );
    ~LogScope( );

    bool Enabled( LogPriority ePriority ) const;
    void SetPriority( LogPriority ePriority ) { m_eMaxPriority = ePriority; }

    void vlogf( enum LogPriority priority, const char *fmt, va_list args )
        ATTRIB_PRINTF( 3, 0 );
    void log( enum LogPriority priority, std::string_view psvText );

    void warnf( const char *fmt, ... ) ATTRIB_PRINTF( 2, 3 );
    void errorf( const char *fmt, ... ) ATTRIB_PRINTF( 2, 3 );
    void infof( const char *fmt, ... ) ATTRIB_PRINTF( 2, 3 );
    void debugf( const char *fmt, ... ) ATTRIB_PRINTF( 2, 3 );

    void errorf_errno( const char *fmt, ... ) ATTRIB_PRINTF( 2, 3 );

    bool bPrefixEnabled = true;

    using LoggingListenerFunc = std::function<void(
        LogPriority      ePriority,
        std::string_view psvScope,
        std::string_view psvText )>;
    std::unordered_map<uintptr_t, LoggingListenerFunc> m_LoggingListeners;

private:
    void vprintf( enum LogPriority priority, const char *fmt, va_list args )
        ATTRIB_PRINTF( 3, 0 );
    void logf( enum LogPriority priority, const char *fmt, ... )
        ATTRIB_PRINTF( 3, 4 );

    std::string_view m_psvName;
    std::string_view m_psvPrefix;

    LogPriority m_eMaxPriority = LOG_INFO;

    std::unique_ptr<LogConVar_t> m_pEnableConVar;
};
