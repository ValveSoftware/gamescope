#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <span>
#include <optional>
#include "convar.h"
#include "Utils/Version.h"

#include <wayland-client.h>
#include <gamescope-control-client-protocol.h>
#include <gamescope-private-client-protocol.h>

// TODO: Consolidate
#define WAYLAND_NULL() []<typename... Args> ( void *pData, Args... args ) { }
#define WAYLAND_USERDATA_TO_THIS(type, name) []<typename... Args> ( void *pData, Args... args ) { type *pThing = (type *)pData; pThing->name( std::forward<Args>(args)... ); }

namespace gamescope
{
    struct GamescopeFeature
    {
        gamescope_control_feature eFeature;
        uint32_t uVersion;
        uint32_t uFlags;
    };

    struct GamescopeActiveDisplayInfo
    {
        std::string szConnectorName;
        std::string szDisplayMake;
        std::string szDisplayModel;
        uint32_t uDisplayFlags;
        std::vector<uint32_t> ValidRefreshRates;
    };

    class GamescopeCtl
    {
    public:
        GamescopeCtl();
        ~GamescopeCtl();

        bool Init( bool bInitControl, bool bInitPrivate );
        bool Execute( std::span<std::string_view> args );

        std::span<GamescopeFeature> GetFeatures() { return std::span<GamescopeFeature>{ m_Features }; }
        const std::optional<GamescopeActiveDisplayInfo> &GetActiveDisplayInfo() { return m_ActiveDisplayInfo; }
    private:
        bool m_bInitControl = false;
        bool m_bInitPrivate = false;

        wl_display *m_pDisplay = nullptr;
        gamescope_control *m_pGamescopeControl = nullptr;
        gamescope_private *m_pGamescopePrivate = nullptr;

        uint32_t m_uCommandCount = 0;

        std::vector<GamescopeFeature> m_Features;
        std::optional<GamescopeActiveDisplayInfo> m_ActiveDisplayInfo;

        void Wayland_Registry_Global( wl_registry *pRegistry, uint32_t uName, const char *pInterface, uint32_t uVersion );
        static const wl_registry_listener s_RegistryListener;

        void Wayland_GamescopeControl_FeatureSupport( gamescope_control *pGamescopeControl, uint32_t uFeature, uint32_t uVersion, uint32_t uFlags );
        void Wayland_GamescopeControl_ActiveDisplayInfo( gamescope_control *pGamescopeControl, const char *pConnectorName, const char *pDisplayMake, const char *pDisplayModel, uint32_t uDisplayFlags, wl_array *pValidRefreshRatesArray );
        void Wayland_GamescopeControl_ScreenshotTaken( gamescope_control *pGamescopeControl, const char *pPath );
        static const gamescope_control_listener s_GamescopeControlListener;

        void Wayland_GamescopePrivate_Log( gamescope_private *pGamescopePrivate, const char *pText );
        void Wayland_GamescopePrivate_CommandExecuted( gamescope_private *pGamescopePrivate );
        static const gamescope_private_listener s_GamescopePrivateListener;
    };

    GamescopeCtl::GamescopeCtl()
    {
    }

    GamescopeCtl::~GamescopeCtl()
    {
    }

    bool GamescopeCtl::Init( bool bInitControl, bool bInitPrivate )
    {
        m_bInitControl = bInitControl;
        m_bInitPrivate = bInitPrivate;

        const char *pDisplayName = getenv( "GAMESCOPE_WAYLAND_DISPLAY" );
        if ( !pDisplayName || !*pDisplayName )
            pDisplayName = "gamescope-0";

        if ( !( m_pDisplay = wl_display_connect( pDisplayName ) ) )
        {
            fprintf( stderr, "Failed to open GAMESCOPE_WAYLAND_DISPLAY.\n" );
            return false;
        }

        {
            wl_registry *pRegistry;
            if ( !( pRegistry = wl_display_get_registry( m_pDisplay ) ) )
            {
                fprintf( stderr, "Failed to get wl_registry.\n" );
                return false;
            }

            wl_registry_add_listener( pRegistry, &s_RegistryListener, (void *)this );
            wl_display_roundtrip( m_pDisplay );
            wl_display_roundtrip( m_pDisplay );

            if ( !( !m_bInitControl || m_pGamescopeControl ) || !( !m_bInitPrivate || m_pGamescopePrivate ) )
            {
                fprintf( stderr, "Failed to get Gamescope interfaces\n" );
                return false;
            }

            wl_registry_destroy( pRegistry );
        }

        return true;
    }

    bool GamescopeCtl::Execute( std::span<std::string_view> args )
    {
        if ( args.size() < 1 )
        {
            fprintf( stderr, "No command to execute\n" );
            return false;
        }

        std::string szArg1 = std::string{ args[0] };
        std::string szArg2 = args.size() == 1 ? "" : std::string{ args[1] };

        gamescope_private_execute( m_pGamescopePrivate, szArg1.c_str(), szArg2.c_str() );
        wl_display_roundtrip( m_pDisplay );

        return true;
    }

    void GamescopeCtl::Wayland_Registry_Global( wl_registry *pRegistry, uint32_t uName, const char *pInterface, uint32_t uVersion )
    {
        if ( m_bInitControl && !strcmp( pInterface, gamescope_control_interface.name ) )
        {
            m_pGamescopeControl = (decltype(m_pGamescopeControl)) wl_registry_bind( pRegistry, uName, &gamescope_control_interface, uVersion );
            gamescope_control_add_listener( m_pGamescopeControl, &s_GamescopeControlListener, this );
        }
        else if ( m_bInitPrivate && !strcmp( pInterface, gamescope_private_interface.name ) )
        {
            m_pGamescopePrivate = (decltype(m_pGamescopePrivate))  wl_registry_bind( pRegistry, uName, &gamescope_private_interface, uVersion );
            gamescope_private_add_listener( m_pGamescopePrivate, &s_GamescopePrivateListener, this );
        }
    }

    const wl_registry_listener GamescopeCtl::s_RegistryListener =
    {
        .global        = WAYLAND_USERDATA_TO_THIS( GamescopeCtl, Wayland_Registry_Global ),
        .global_remove = WAYLAND_NULL(),
    };

    void GamescopeCtl::Wayland_GamescopeControl_FeatureSupport( gamescope_control *pGamescopeControl, uint32_t uFeature, uint32_t uVersion, uint32_t uFlags )
    {
        gamescope_control_feature eFeature = static_cast<gamescope_control_feature>( uFeature );
        if ( eFeature == GAMESCOPE_CONTROL_FEATURE_DONE )
            return;
        m_Features.emplace_back( GamescopeFeature
        {
            .eFeature = eFeature,
            .uVersion = uVersion,
            .uFlags   = uFlags
        } );
    }
    void GamescopeCtl::Wayland_GamescopeControl_ActiveDisplayInfo( gamescope_control *pGamescopeControl, const char *pConnectorName, const char *pDisplayMake, const char *pDisplayModel, uint32_t uDisplayFlags, wl_array *pValidRefreshRatesArray )
    {
        const uint32_t *pValidRefreshRates = reinterpret_cast<const uint32_t*>( pValidRefreshRatesArray->data );
        std::vector<uint32_t> validRefreshRates;
        for ( size_t i = 0; i < pValidRefreshRatesArray->size / sizeof( uint32_t ); i++ )
            validRefreshRates.push_back( pValidRefreshRates[i] );

        m_ActiveDisplayInfo = GamescopeActiveDisplayInfo
        {
            .szConnectorName = pConnectorName,
            .szDisplayMake   = pDisplayMake,
            .szDisplayModel  = pDisplayModel,
            .uDisplayFlags   = uDisplayFlags,
            .ValidRefreshRates = std::move( validRefreshRates ),
        };
    }
    void GamescopeCtl::Wayland_GamescopeControl_ScreenshotTaken( gamescope_control *pGamescopeControl, const char *pPath )
    {
        fprintf( stderr, "Screenshot taken to: %s\n", pPath );
    }

    const gamescope_control_listener GamescopeCtl::s_GamescopeControlListener =
    {
        .feature_support     = WAYLAND_USERDATA_TO_THIS( GamescopeCtl, Wayland_GamescopeControl_FeatureSupport ),
        .active_display_info = WAYLAND_USERDATA_TO_THIS( GamescopeCtl, Wayland_GamescopeControl_ActiveDisplayInfo ),
        .screenshot_taken    = WAYLAND_USERDATA_TO_THIS( GamescopeCtl, Wayland_GamescopeControl_ScreenshotTaken ),
    };

    void GamescopeCtl::Wayland_GamescopePrivate_Log( gamescope_private *pGamescopePrivate, const char *pText )
    {
        fprintf( stderr, "%s\n", pText );
    }

    void GamescopeCtl::Wayland_GamescopePrivate_CommandExecuted( gamescope_private *pGamescopePrivate )
    {
        m_uCommandCount++;
    }

    const gamescope_private_listener GamescopeCtl::s_GamescopePrivateListener =
    {
        .log              = WAYLAND_USERDATA_TO_THIS( GamescopeCtl, Wayland_GamescopePrivate_Log ),
        .command_executed = WAYLAND_USERDATA_TO_THIS( GamescopeCtl, Wayland_GamescopePrivate_CommandExecuted ),
    };

    static std::string_view GetFeatureName( gamescope_control_feature eFeature )
    {
        switch( eFeature )
        {
            case GAMESCOPE_CONTROL_FEATURE_DONE:
                return "Done (dummy)";
            case GAMESCOPE_CONTROL_FEATURE_RESHADE_SHADERS:
                return "Reshade Shaders";
            case GAMESCOPE_CONTROL_FEATURE_DISPLAY_INFO:
                return "Display Info";
            case GAMESCOPE_CONTROL_FEATURE_PIXEL_FILTER:
                return "Pixel Filter";
            case GAMESCOPE_CONTROL_FEATURE_REFRESH_CYCLE_ONLY_CHANGE_REFRESH_RATE:
                return "Refresh Cycle Only Change Refresh Rate";
            case GAMESCOPE_CONTROL_FEATURE_MURA_CORRECTION:
                return "Mura Correction";
            default:
                return "Unknown";
        }
    }

    static int RunGamescopeCtl( int argc, char *argv[] )
    {
        console_log.bPrefixEnabled = false;

        bool bInfoOnly = argc < 2;

        gamescope::GamescopeCtl gamescopeCtl;
        if ( !gamescopeCtl.Init( bInfoOnly, !bInfoOnly ) )
            return 1;

        if ( bInfoOnly )
        {
            PrintVersion();
            fprintf( stdout, "gamescope_control info:\n" );
            const auto &oActiveDisplayInfo = gamescopeCtl.GetActiveDisplayInfo();
            if ( oActiveDisplayInfo )
            {
                fprintf( stdout, "  - Connector Name: %.*s\n", (int)oActiveDisplayInfo->szConnectorName.length(), oActiveDisplayInfo->szConnectorName.data() );
                fprintf( stdout, "  - Display Make: %.*s\n", (int)oActiveDisplayInfo->szDisplayMake.length(), oActiveDisplayInfo->szDisplayMake.data() );
                fprintf( stdout, "  - Display Model: %.*s\n", (int)oActiveDisplayInfo->szDisplayModel.length(), oActiveDisplayInfo->szDisplayModel.data() );
                fprintf( stdout, "  - Display Flags: 0x%x\n", oActiveDisplayInfo->uDisplayFlags );
                fprintf( stdout, "  - ValidRefreshRates: " );
                for ( size_t i = 0; i < oActiveDisplayInfo->ValidRefreshRates.size(); i++ )
                {
                    bool bLast = i == oActiveDisplayInfo->ValidRefreshRates.size() - 1;
                    uint32_t uRate = oActiveDisplayInfo->ValidRefreshRates[i];
                    fprintf( stdout, bLast ? "%u" : "%u, ", uRate );
                }
                fprintf( stdout, "\n" );
            }
            fprintf( stdout, "  Features:\n" );
            for ( const GamescopeFeature &feature : gamescopeCtl.GetFeatures() )
            {
                std::string_view szFeatureName = GetFeatureName( feature.eFeature );
                fprintf( stdout, "  - %.*s (%u) - Version: %u - Flags: 0x%x\n", (int)szFeatureName.size(), szFeatureName.data(), uint32_t{ feature.eFeature }, feature.uVersion, feature.uFlags );
            }
            fprintf( stdout, "You can execute any debug command in Gamescope using this tool.\n" );
            fprintf( stdout, "For a list of commands and convars, use 'gamescopectl help'\n" );
            return 0;
        }

        std::vector<std::string_view> args;
        for ( int i = 1; i < argc; i++ )
            args.emplace_back( argv[i] );

        if ( !gamescopeCtl.Execute( std::span{ args } ) )
            return 1;

        return 0;
    }
}

int main( int argc, char *argv[] )
{
    return gamescope::RunGamescopeCtl( argc, argv );
}