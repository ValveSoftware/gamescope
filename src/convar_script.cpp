#include "convar.h"
#include "Script/Script.h"
#include "backend.h"

namespace gamescope
{
#if HAVE_SCRIPTING
    namespace detail
    {
        struct ConVarScriptRegistrar
        {
            static sol::usertype<ConCommand> RegisterConCommand()
            {
                return CScriptScopedLock()->new_usertype<ConCommand>( "concommand",
                    "name", &ConCommand::m_pszName,
                    "description", &ConCommand::m_pszDescription,
                    "call", &ConCommand::CallWithArgString );
            }

            template <typename T>
            static sol::usertype<ConVar<T>> RegisterConVarType()
            {
                return CScriptScopedLock()->new_usertype<ConVar<T>>(
                    typeid( ConVar<T> ).name(),
                    "name", &ConVar<T>::m_pszName,
                    "description", &ConVar<T>::m_pszDescription,
                    "call", &ConVar<T>::CallWithArgString,
                    "value", &ConVar<T>::m_Value );
            }
        };
    }

    static auto s_ConCommandType = detail::ConVarScriptRegistrar::RegisterConCommand();
    static auto s_CVBool   = detail::ConVarScriptRegistrar::RegisterConVarType<bool>();
    static auto s_CVInt    = detail::ConVarScriptRegistrar::RegisterConVarType<int>();
    static auto s_CVFloat  = detail::ConVarScriptRegistrar::RegisterConVarType<float>();
    static auto s_CVU32    = detail::ConVarScriptRegistrar::RegisterConVarType<uint32_t>();
    static auto s_CVU64    = detail::ConVarScriptRegistrar::RegisterConVarType<uint64_t>();
    static auto s_CVString = detail::ConVarScriptRegistrar::RegisterConVarType<std::string>();
    static auto s_CVVCS    = detail::ConVarScriptRegistrar::RegisterConVarType<VirtualConnectorStrategy>();
    static auto s_CVTCM    = detail::ConVarScriptRegistrar::RegisterConVarType<TouchClickMode>();

    void ConCommand::RegisterScript( std::string_view name, ConCommand *cmd )
    {
        CScriptScopedLock().Manager().Gamescope().Convars.Base[name] = cmd;
    }
#else
    void ConCommand::RegisterScript( std::string_view, ConCommand * )
    {
    }
#endif
}
