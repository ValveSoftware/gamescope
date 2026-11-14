#pragma once

#include <xkbcommon/xkbcommon.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <span>
#include <optional>
#include <unordered_set>
#include <functional>
#include <memory>

#include "log.hpp"
#include "convar.h"

#if HAVE_SCRIPTING
#include <sol/sol.hpp>
#endif

extern LogScope log_binding;

uint64_t get_time_in_nanos();

namespace gamescope
{
    struct Keybind_t
    {
        std::unordered_set<xkb_keysym_t> setKeySyms;
        std::string sDebugName;
    };

    std::string ComputeDebugName( const std::unordered_set<xkb_keysym_t> &syms );
    xkb_keysym_t NormalizeKeysymForHotkey( xkb_keysym_t uKeySym );
    std::optional<std::unordered_set<xkb_keysym_t>> ParseHotkeyStringToKeysyms( std::string_view svCombo );

    enum ActionBindingArmFlags : uint32_t
    {
        ActionBindingArmFlag_OneShot = 0x1,
        ActionBindingArmFlag_NoBlock = 0x2,
    };

    // Base class for an action binding in gamescope
    class CServerActionBinding
    {
    public:
        CServerActionBinding( std::string sDescription = "" );
        virtual ~CServerActionBinding();

        void SetDescription( std::string sDescription ) { m_sDescription = std::move( sDescription ); }
        const std::string &GetDescription() const { return m_sDescription; }

        void SetNestedOnly( bool bNestedOnly ) { m_bNestedOnly = bNestedOnly; }
        bool IsNestedOnly() const { return m_bNestedOnly; }

        void AddKeyboardTrigger( std::unordered_set<xkb_keysym_t> setKeySyms );
        void ClearTriggers();

        void Arm( uint32_t uArmFlags = 0 );
        void Disarm();

        bool IsArmed() const { return m_ouArmFlags != std::nullopt; }
        std::span<const Keybind_t> GetKeyboardTriggers() const { return m_KeyboardTriggers; }

        virtual bool Execute() = 0;

        static std::span<CServerActionBinding *> GetBindings();

    protected:
        std::string m_sDescription;
        std::vector<Keybind_t> m_KeyboardTriggers;
        std::optional<uint32_t> m_ouArmFlags;
        bool m_bNestedOnly = false;

        static std::vector<CServerActionBinding *> s_Bindings;
    };

    class CNativeActionBinding : public CServerActionBinding
    {
    public:
        enum class ActionType
        {
            Command,
            CppFunc,
#if HAVE_SCRIPTING
            LuaFunc,
#endif
        };

        CNativeActionBinding( std::string sDescription, std::string sCommand );
        CNativeActionBinding( std::string sDescription, std::function<void()> fnCallback );
#if HAVE_SCRIPTING
        CNativeActionBinding( std::string sDescription, sol::function luaCallback );
#endif
        virtual ~CNativeActionBinding() = default;

        virtual bool Execute() override;

    private:
        ActionType m_eType;
        std::string m_sCommand;
        std::function<void()> m_fnCallback;
#if HAVE_SCRIPTING
        sol::function m_luaCallback;
#endif
    };

    void RegisterNativeBinding( std::unique_ptr<CNativeActionBinding> pBinding );
    void ClearNativeBindings();
    void RegisterHotkeysFromScript();
}
