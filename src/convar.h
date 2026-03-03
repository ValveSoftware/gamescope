#pragma once

#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <optional>
#include <charconv>
#include <type_traits>
#include <cstdint>
#include <functional>
#include <cassert>

#include "Utils/Dict.h"

#include "log.hpp"

extern LogScope console_log;

namespace gamescope
{
    class ConCommand;

    template <typename T>
    inline std::string ToString( const T &thing )
    {
        return std::to_string( thing );
    }

    template <>
    inline std::string ToString( const std::string &sThing )
    {
        return sThing;
    }

    template <>
    inline std::string ToString( const std::string_view &svThing )
    {
        return std::string( svThing );
    }

    template <typename T>
    inline std::optional<T> Parse( std::string_view chars )
    {
        T obj;
        auto result = std::from_chars( chars.begin(), chars.end(), obj );
        if ( result.ec == std::errc{} )
            return obj;
        else
            return std::nullopt;
    }

    template <>
    inline std::optional<bool> Parse( std::string_view chars )
    {
        std::optional<uint32_t> oNumber = Parse<uint32_t>( chars );
        if ( oNumber )
            return !!*oNumber;

        if ( chars == "true" )
            return true;
        else
            return false;
    }

    inline void Split( std::vector<std::string_view> &tokens, std::string_view string, std::string_view delims = " " )
    {
        size_t end = 0;
        for ( size_t start = 0; start < string.size() && end != std::string_view::npos; start = end + 1 )
        {
            end = string.find_first_of( delims, start );

            if ( start != end )
                tokens.emplace_back( string.substr( start, end-start ) );
        }
    }

    inline std::vector<std::string_view> Split( std::string_view string, std::string_view delims = " " )
    {
        std::vector<std::string_view> tokens;
        Split( tokens, string, delims );
        return tokens;
    }

    namespace detail { struct ConVarScriptRegistrar; }

    class ConCommand
    {
        friend struct detail::ConVarScriptRegistrar;
        using ConCommandFunc = std::function<void( std::span<std::string_view> )>;

    public:
        ConCommand( std::string_view pszName, std::string_view pszDescription, ConCommandFunc func, bool bRegisterScript = true );
        ~ConCommand();

        void Invoke( std::span<std::string_view> args )
        {
            if ( m_Func )
                m_Func( args );
        }

        // Calls it with space separated args.
        void CallWithArgString( std::string_view args )
        {
            std::vector<std::string_view> sArgs;
            sArgs.push_back( m_pszName );
            Split( sArgs, args, " " );

            Invoke( sArgs );
        }

        static bool Exec( std::span<std::string_view> args );

        std::string_view GetName() const { return m_pszName; }
        std::string_view GetDescription() const { return m_pszDescription; }

        static Dict<ConCommand *>& GetCommands();
        static void RegisterScript( std::string_view name, ConCommand *cmd );
    protected:
        std::string_view m_pszName;
        std::string_view m_pszDescription;
        ConCommandFunc m_Func;
    };


    template <typename T>
    class ConVar : public ConCommand
    {
        friend struct detail::ConVarScriptRegistrar;
        using ConVarCallbackFunc = std::function<void(ConVar<T> &)>;
    public:
        ConVar( std::string_view pszName, T defaultValue = T{}, std::string_view pszDescription = "", ConVarCallbackFunc func = nullptr, bool bRunCallbackAtStartup = false, bool bRegisterScript = true )
            : ConCommand( pszName, pszDescription, [this]( std::span<std::string_view> pArgs ){ this->InvokeFunc( pArgs ); }, false )
            , m_Value{ defaultValue }
            , m_Callback{ func }
        {
            if ( bRunCallbackAtStartup )
            {
                RunCallback();
            }

            if ( bRegisterScript )
            {
                RegisterScript( pszName, this );
            }
        }

        const T& Get() const
        {
            return m_Value;
        }

        template <typename J>
        void SetValue( const J &newValue )
        {
            m_Value = T{ newValue };

            RunCallback();
        }

        void RunCallback()
        {
            if ( !m_bInCallback && m_Callback )
            {
                m_bInCallback = true;
                m_Callback( *this );
                m_bInCallback = false;
            }
        }

        template <typename J>
        ConVar<T>& operator =( const J &newValue ) { SetValue<J>( newValue ); return *this; }

        operator T() const { return m_Value; }

        // SFINAE for std::string...
        operator std::string_view() const { return m_Value; }

        template <typename J> bool operator == ( const J &other ) const { return m_Value ==  other; }
        template <typename J> bool operator != ( const J &other ) const { return m_Value !=  other; }
        template <typename J> auto operator <=>( const J &other ) const { return m_Value <=> other; }

        template <typename J>  bool operator == ( const ConVar<J> &other ) const { return *this ==  other.Get(); }
        template <typename J>  bool operator != ( const ConVar<J> &other ) const { return *this !=  other.Get(); }
        template <typename J>  auto operator <=>( const ConVar<J> &other ) const { return *this <=> other.Get(); }

        T  operator | (T other) { return m_Value | other; }
        T &operator |=(T other) { return m_Value |= other; }
        T  operator & (T other) { return m_Value & other; }
        T &operator &=(T other) { return m_Value &= other; }

        void InvokeFunc( std::span<std::string_view> pArgs )
        {
            if ( pArgs.size() == 1 )
            {
                // We should move to std format for logging and stuff.
                // This is kinda gross and grody!
                std::string sValue = ToString( m_Value );
                console_log.infof( "%.*s: %.*s\n%.*s",
                    (int)m_pszName.length(), m_pszName.data(),
                    (int)sValue.length(), sValue.data(),
                    (int)m_pszDescription.length(), m_pszDescription.data() );

                return;
            }

            if ( pArgs.size() != 2 )
                return;

            if constexpr ( std::is_enum<T>::value )
            {
                using Underlying = std::underlying_type<T>::type;
                std::optional<Underlying> oResult = Parse<Underlying>( pArgs[1] );
                SetValue( oResult ? static_cast<T>( *oResult ) : T{} );
            }
            else if constexpr ( std::is_integral<T>::value || std::is_floating_point<T>::value )
            {
                std::optional<T> oResult = Parse<T>( pArgs[1] );
                SetValue( oResult ? *oResult : T{} );
            }
            else
            {
                SetValue( pArgs[1] );
            }
        }
    private:
        T m_Value{};
        ConVarCallbackFunc m_Callback;
        bool m_bInCallback;
    };


}
