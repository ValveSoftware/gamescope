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

#include "log.hpp"

extern LogScope console_log;

namespace gamescope
{
    class ConCommand;

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

    inline std::vector<std::string_view> Split( std::string_view string, std::string_view delims = " " )
    {
        std::vector<std::string_view> tokens;
        
        size_t end = 0;
        for ( size_t start = 0; start < string.size() && end != std::string_view::npos; start = end + 1 )
        {
            end = string.find_first_of( delims, start );

            if ( start != end )
                tokens.emplace_back( string.substr( start, end-start ) );
        }

        return tokens;
    }

    struct StringHash
    {
        using is_transparent = void;
        [[nodiscard]] size_t operator()( const char *string )        const { return std::hash<std::string_view>{}( string ); }
        [[nodiscard]] size_t operator()( std::string_view string )   const { return std::hash<std::string_view>{}( string ); }
        [[nodiscard]] size_t operator()( const std::string &string ) const { return std::hash<std::string>{}( string ); }
    };

    template <typename T>
    using Dict = std::unordered_map<std::string, T, StringHash, std::equal_to<>>;

    class ConCommand
    {
        using ConCommandFunc = std::function<void( std::span<std::string_view> )>;

    public:
        ConCommand( std::string_view pszName, std::string_view pszDescription, ConCommandFunc func )
            : m_pszName{ pszName }
            , m_pszDescription{ pszDescription }
            , m_Func{ func }
        {
            assert( !GetCommands().contains( pszName ) );
            GetCommands()[ std::string( pszName ) ] = this;
        }

        ~ConCommand()
        {
            GetCommands().erase( GetCommands().find( m_pszName ) );
        }

        void Invoke( std::span<std::string_view> args )
        {
            if ( m_Func )
                m_Func( args );
        }

        static bool Exec( std::span<std::string_view> args );

        std::string_view GetName() const { return m_pszName; }
        std::string_view GetDescription() const { return m_pszDescription; }

        static Dict<ConCommand *>& GetCommands();
    protected:
        std::string_view m_pszName;
        std::string_view m_pszDescription;
        ConCommandFunc m_Func;
    };

    template <typename T>
    class ConVar : public ConCommand
    {
        using ConVarCallbackFunc = std::function<void()>;
    public:
        ConVar( std::string_view pszName, T defaultValue = T{}, std::string_view pszDescription = "", ConVarCallbackFunc func = nullptr )
            : ConCommand( pszName, pszDescription, [this]( std::span<std::string_view> pArgs ){ this->InvokeFunc( pArgs ); } )
            , m_Value{ defaultValue }
            , m_Callback{ func }
        {
        }

        const T& Get() const
        {
            return m_Value;
        }

        template <typename J>
        void SetValue( const J &newValue )
        {
            m_Value = T{ newValue };

            if ( !m_bInCallback && m_Callback )
            {
                m_bInCallback = true;
                m_Callback();
                m_bInCallback = false;
            }
        }

        template <typename J>
        ConVar<T>& operator =( const J &newValue ) { SetValue<J>( newValue ); return *this; }

        operator T() const { return m_Value; }

        template <typename J> bool operator == ( const J &other ) const { return m_Value ==  other; }
        template <typename J> bool operator != ( const J &other ) const { return m_Value !=  other; }
        template <typename J> bool operator <=>( const J &other ) const { return m_Value <=> other; }

        T  operator | (T other) { return m_Value | other; }
        T &operator |=(T other) { return m_Value |= other; }
        T  operator & (T other) { return m_Value & other; }
        T &operator &=(T other) { return m_Value &= other; }

        void InvokeFunc( std::span<std::string_view> pArgs )
        {
            if ( pArgs.size() != 2 )
                return;

            if constexpr ( std::is_enum<T>::value )
            {
                using Underlying = std::underlying_type<T>::type;
                std::optional<Underlying> oResult = Parse<Underlying>( pArgs[1] );
                SetValue( oResult ? static_cast<T>( *oResult ) : T{} );
            }
            else if constexpr ( std::is_integral<T>::value )
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
