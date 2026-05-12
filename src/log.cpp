#include <cerrno>
#include <cstdio>
#include <cstring>

#include <format>

#include "Utils/Defer.h"
#include "Utils/Process.h"
#include "convar.h"
#include "log.hpp"

static constexpr std::string_view GetLogPriorityText( LogPriority ePriority )
{
    switch ( ePriority )
    {
        case LOG_SILENT:
            return "[\e[0;37m"
                   "Shh.."
                   "\e[0m]";
        case LOG_ERROR:
            return "[\e[0;31m"
                   "Error"
                   "\e[0m]";
        case LOG_WARNING:
            return "[\e[0;33m"
                   "Warn"
                   "\e[0m] ";
        case LOG_DEBUG:
            return "[\e[0;35m"
                   "Debug"
                   "\e[0m]";
        default:
        case LOG_INFO:
            return "[\e[0;34m"
                   "Info"
                   "\e[0m] ";
    }
}

static constexpr std::string_view GetLogName( LogPriority ePriority )
{
    switch ( ePriority )
    {
        case LOG_SILENT:
            return "silent";
        case LOG_ERROR:
            return "error";
        case LOG_WARNING:
            return "warning";
        case LOG_DEBUG:
            return "debug";
        default:
        case LOG_INFO:
            return "info";
    }
}

static constexpr LogPriority GetPriorityFromString( std::string_view psvScope )
{
    if ( psvScope == "silent" ) return LOG_SILENT;
    else if ( psvScope == "error" )
        return LOG_ERROR;
    else if ( psvScope == "warning" )
        return LOG_WARNING;
    else if ( psvScope == "debug" )
        return LOG_DEBUG;
    else
        return LOG_INFO;
}

struct LogConVar_t
{
    LogConVar_t(
        LogScope        *pScope,
        std::string_view psvName,
        LogPriority      eDefaultPriority ) :
        sName{ std::format( "log_{}", psvName ) },
        sDescription{ std::format(
            "Max logging priority for the {} channel. Valid options are: [ "
            "silent, error, warning, debug, info ].",
            psvName ) },
        convar{
            sName,
            std::string( GetLogName( eDefaultPriority ) ),
            sDescription,
            [ pScope ]( gamescope::ConVar<std::string> &cvar )
            { pScope->SetPriority( GetPriorityFromString( cvar ) ); },
        }
    {}
    std::string sName;
    std::string sDescription;

    gamescope::ConVar<std::string> convar;
};

LogScope::LogScope( std::string_view psvName, LogPriority eMaxPriority ) :
    LogScope( psvName, psvName, eMaxPriority )
{}

LogScope::LogScope(
    std::string_view psvName,
    std::string_view psvPrefix,
    LogPriority      eMaxPriority ) :
    m_psvName{ psvName },
    m_psvPrefix{ psvPrefix },
    m_eMaxPriority{ eMaxPriority },
    m_pEnableConVar{ std::make_unique<LogConVar_t>(
        this, psvName, eMaxPriority ) }
{}

LogScope::~LogScope( ) {}

bool LogScope::Enabled( LogPriority ePriority ) const
{ return ePriority <= m_eMaxPriority; }

void LogScope::vlogf( enum LogPriority priority, const char *fmt, va_list args )
{
    if ( !Enabled( priority ) ) return;

    char *buf = nullptr;
    vasprintf( &buf, fmt, args );
    if ( !buf ) return;
    defer( free( buf ); );

    std::string_view svBuf = buf;
    log( priority, svBuf );
}

void LogScope::log( enum LogPriority priority, std::string_view psvText )
{
    if ( !Enabled( priority ) ) return;

    for ( auto &listener : m_LoggingListeners )
        listener.second( priority, m_psvPrefix, psvText );

    std::string_view psvLogName = GetLogPriorityText( priority );
    if ( bPrefixEnabled )
        fprintf(
            stderr,
            "[%s] %.*s \e[0;37m%.*s:\e[0m %.*s\n",
            gamescope::Process::GetProcessName( ),
            ( int )psvLogName.size( ),
            psvLogName.data( ),
            ( int )this->m_psvPrefix.size( ),
            this->m_psvPrefix.data( ),
            ( int )psvText.size( ),
            psvText.data( ) );
    else
        fprintf( stderr, "%.*s\n", ( int )psvText.size( ), psvText.data( ) );
}

void LogScope::logf( enum LogPriority priority, const char *fmt, ... )
{
    va_list args;
    va_start( args, fmt );
    this->vlogf( priority, fmt, args );
    va_end( args );
}

void LogScope::warnf( const char *fmt, ... )
{
    va_list args;
    va_start( args, fmt );
    this->vlogf( LOG_WARNING, fmt, args );
    va_end( args );
}

void LogScope::errorf( const char *fmt, ... )
{
    va_list args;
    va_start( args, fmt );
    this->vlogf( LOG_ERROR, fmt, args );
    va_end( args );
}

void LogScope::infof( const char *fmt, ... )
{
    va_list args;
    va_start( args, fmt );
    this->vlogf( LOG_INFO, fmt, args );
    va_end( args );
}

void LogScope::debugf( const char *fmt, ... )
{
    va_list args;
    va_start( args, fmt );
    this->vlogf( LOG_DEBUG, fmt, args );
    va_end( args );
}

void LogScope::errorf_errno( const char *fmt, ... )
{
    const char *err = strerror( errno );

    static char buf[ 1024 ];
    va_list     args;
    va_start( args, fmt );
    vsnprintf( buf, sizeof( buf ), fmt, args );
    va_end( args );

    this->logf( LOG_ERROR, "%s: %s", buf, err );
}
