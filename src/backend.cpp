#include "backend.h"
#include "vblankmanager.hpp"
#include "convar.h"
#include "wlserver.hpp"

#include "wlr_begin.hpp"
#include <wlr/types/wlr_buffer.h>
#include "wlr_end.hpp"

extern void sleep_until_nanos(uint64_t nanos);
extern bool env_to_bool(const char *env);

namespace gamescope
{
    ConVar<VirtualConnectorStrategy> cv_backend_virtual_connector_strategy( "backend_virtual_connector_strategy", VirtualConnectorStrategies::SingleApplication );

    /////////////
    // IBackend
    /////////////

    static IBackend *s_pBackend = nullptr;

    IBackend *IBackend::Get()
    {
        return s_pBackend;
    }

    bool IBackend::Set( IBackend *pBackend )
    {
        if ( s_pBackend )
        {
            delete s_pBackend;
            s_pBackend = nullptr;
        }

        if ( pBackend )
        {
            s_pBackend = pBackend;
            if ( !s_pBackend->Init() )
            {
                delete s_pBackend;
                s_pBackend = nullptr;
                return false;
            }
        }

        return true;
    }

    /////////////////
    // CBaseBackendFb
    /////////////////

    CBaseBackendFb::CBaseBackendFb()
    {
    }

    CBaseBackendFb::~CBaseBackendFb()
    {
        // I do not own the client buffer, but I released that in DecRef.
        assert( !HasLiveReferences() );
    }

    uint32_t CBaseBackendFb::IncRef()
    {
        uint32_t uRefCount = IBackendFb::IncRef();
        if ( m_pClientBuffer && !uRefCount )
        {
            wlserver_lock();
            wlr_buffer_lock( m_pClientBuffer );
            wlserver_unlock( false );
        }
        return uRefCount;
    }
    uint32_t CBaseBackendFb::DecRef()
    {
        wlr_buffer *pClientBuffer = m_pClientBuffer;

        std::shared_ptr<CReleaseTimelinePoint> pReleasePoint = std::move( m_pReleasePoint );
        m_pReleasePoint = nullptr;

        uint32_t uRefCount = IBackendFb::DecRef();
        if ( uRefCount )
        {
            if ( pReleasePoint )
                m_pReleasePoint = std::move( pReleasePoint );
        }
        else if ( pClientBuffer )
        {
            wlserver_lock();
            wlr_buffer_unlock( pClientBuffer );
            wlserver_unlock();
        }
        return uRefCount;
    }

    void CBaseBackendFb::SetBuffer( wlr_buffer *pClientBuffer )
    {
        if ( m_pClientBuffer == pClientBuffer )
            return;

        assert( m_pClientBuffer == nullptr );
        m_pClientBuffer = pClientBuffer;
        if ( GetRefCount() )
        {
            wlserver_lock();
            wlr_buffer_lock( m_pClientBuffer );
            wlserver_unlock( false );
        }

        m_pReleasePoint = nullptr;
    }

    void CBaseBackendFb::SetReleasePoint( std::shared_ptr<CReleaseTimelinePoint> pReleasePoint )
    {
        m_pReleasePoint = pReleasePoint;

        if ( m_pClientBuffer && GetRefCount() )
        {
            wlserver_lock();
            wlr_buffer_unlock( m_pClientBuffer );
            wlserver_unlock();
            m_pClientBuffer = nullptr;
        }
    }

    /////////////////////////
    // CBaseBackendConnector
    /////////////////////////

    VBlankScheduleTime CBaseBackendConnector::FrameSync()
    {
        VBlankScheduleTime schedule = GetVBlankTimer().CalcNextWakeupTime( false );
        sleep_until_nanos( schedule.ulScheduledWakeupPoint );
        return schedule;
    }

    /////////////////
    // CBaseBackend
    /////////////////

    bool CBaseBackend::NeedsFrameSync() const
    {
        const bool bForceTimerFd = env_to_bool( getenv( "GAMESCOPE_DISABLE_TIMERFD" ) );
        return bForceTimerFd;
    }

    ConVar<bool> cv_touch_external_display_trackpad( "touch_external_display_trackpad", false, "If we are using an external display, should we treat the internal display's touch as a trackpad insteaad?" );
    ConVar<TouchClickMode> cv_touch_click_mode( "touch_click_mode", TouchClickModes::Left, "The default action to perform on touch." );
    TouchClickMode CBaseBackend::GetTouchClickMode()
    {
        if ( cv_touch_external_display_trackpad && this->GetCurrentConnector() )
        {
            gamescope::GamescopeScreenType screenType = this->GetCurrentConnector()->GetScreenType();
            if ( screenType == gamescope::GAMESCOPE_SCREEN_TYPE_EXTERNAL && cv_touch_click_mode == TouchClickMode::Passthrough )
                return TouchClickMode::Trackpad;
        }

        return cv_touch_click_mode;
    }

    void CBaseBackend::DumpDebugInfo()
    {
        console_log.infof( "Uses Modifiers: %s", this->UsesModifiers() ? "true" : "false" );
        console_log.infof( "Supports Plane Hardware Cursor: %s (not relevant for nested backends)", this->SupportsPlaneHardwareCursor() ? "true" : "false" );
        console_log.infof( "Supports Tearing: %s", this->SupportsTearing() ? "true" : "false" );
        console_log.infof( "Uses Vulkan Swapchain: %s", this->UsesVulkanSwapchain() ? "true" : "false" );
        console_log.infof( "Is Session Based: %s", this->IsSessionBased() ? "true" : "false" );
        console_log.infof( "Supports Explicit Sync: %s", this->SupportsExplicitSync() ? "true" : "false" );
        console_log.infof( "Current Screen Type: %s", this->GetScreenType() == GAMESCOPE_SCREEN_TYPE_INTERNAL ? "Internal" : "External" );
        console_log.infof( "Is Visible: %s", this->IsVisible() ? "true" : "false" );
        console_log.infof( "Needs Frame Sync: %s", this->NeedsFrameSync() ? "true" : "false" );
        console_log.infof( "VRR Active: %s", this->GetCurrentConnector()->IsVRRActive() ? "true" : "false" );
        console_log.infof( "Total Presents Queued: %lu", this->GetCurrentConnector()->PresentationFeedback().TotalPresentsQueued() );
        console_log.infof( "Total Presents Completed: %lu", this->GetCurrentConnector()->PresentationFeedback().TotalPresentsCompleted() );
        console_log.infof( "Current Presents In Flight: %lu", this->GetCurrentConnector()->PresentationFeedback().CurrentPresentsInFlight() );
    }

    bool CBaseBackend::UsesVirtualConnectors()
    {
        return false;
    }
    std::shared_ptr<IBackendConnector> CBaseBackend::CreateVirtualConnector( uint64_t ulVirtualConnectorKey )
    {
        assert( false );
        return nullptr;
    }

    ConCommand cc_backend_info( "backend_info", "Dump debug info about the backend state",
    []( std::span<std::string_view> svArgs )
    {
        if ( !GetBackend() )
            return;

        GetBackend()->DumpDebugInfo();
    });
}
