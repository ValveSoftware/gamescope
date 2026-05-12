#if HAVE_SCRIPTING

    #include "Script/Script.h"
    #include "backend.h"
    #include "convar.h"

namespace gamescope
{
    namespace detail
    {
        struct ConVarScriptRegistrar
        {
            static sol::usertype<ConCommand> RegisterConCommand( )
            {
                return CScriptScopedLock( )->new_usertype<ConCommand>(
                    "concommand",
                    "name",
                    &ConCommand::m_pszName,
                    "description",
                    &ConCommand::m_pszDescription,
                    "call",
                    &ConCommand::CallWithArgString );
            }

            template<typename T>
            static sol::usertype<ConVar<T>> RegisterConVarType( )
            {
                return CScriptScopedLock( )->new_usertype<ConVar<T>>(
                    typeid( ConVar<T> ).name( ),
                    "name",
                    &ConVar<T>::m_pszName,
                    "description",
                    &ConVar<T>::m_pszDescription,
                    "call",
                    &ConVar<T>::CallWithArgString,
                    "value",
                    &ConVar<T>::m_Value );
            }
        };
    } // namespace detail

    void ConCommand::RegisterScript( std::string_view name, ConCommand *cmd )
    { CScriptScopedLock( ).Manager( ).Gamescope( ).Convars.Base[ name ] = cmd; }

    template<typename T>
    void ConVar<T>::RegisterScript( std::string_view name, ConVar<T> *cv )
    { CScriptScopedLock( ).Manager( ).Gamescope( ).Convars.Base[ name ] = cv; }

    #define REGISTER_CONVAR_TYPE_IMPL( T, N )                                  \
        static auto s_ConVarType_##N =                                         \
            detail::ConVarScriptRegistrar::RegisterConVarType<T>( );           \
        template void ConVar<T>::RegisterScript(                               \
            std::string_view, ConVar<T> * );

    #define REGISTER_CONVAR_TYPE_EXPAND( T, N )                                \
        REGISTER_CONVAR_TYPE_IMPL( T, N )
    #define REGISTER_CONVAR_TYPE( T )                                          \
        REGISTER_CONVAR_TYPE_EXPAND( T, __COUNTER__ )

    static auto s_ConCommandType =
        detail::ConVarScriptRegistrar::RegisterConCommand( );
    REGISTER_CONVAR_TYPE( bool )
    REGISTER_CONVAR_TYPE( int )
    REGISTER_CONVAR_TYPE( float )
    REGISTER_CONVAR_TYPE( uint32_t )
    REGISTER_CONVAR_TYPE( uint64_t )
    REGISTER_CONVAR_TYPE( std::string )
    REGISTER_CONVAR_TYPE( VirtualConnectorStrategy )
    REGISTER_CONVAR_TYPE( TouchClickMode )
} // namespace gamescope

#endif
