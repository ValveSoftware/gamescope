#include "backend.h"
#include "rendervulkan.hpp"
#include "wlserver.hpp"
#include "steamcompmgr.hpp"
#include "edid.h"
#include "Utils/Defer.h"
#include "Utils/Algorithm.h"
#include "refresh_rate.h"

#include <cstring>
#include <unordered_map>
#include <sys/mman.h>
#include <poll.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>
#include <libdecor.h>

#include "wlr_begin.hpp"
#include <wayland-client.h>
#include <xdg-shell-client-protocol.h>
#include <linux-dmabuf-v1-client-protocol.h>
#include <viewporter-client-protocol.h>
#include <single-pixel-buffer-v1-client-protocol.h>
#include <presentation-time-client-protocol.h>
#include <frog-color-management-v1-client-protocol.h>
#include <color-management-v1-client-protocol.h>
#include <pointer-constraints-unstable-v1-client-protocol.h>
#include <relative-pointer-unstable-v1-client-protocol.h>
#include <primary-selection-unstable-v1-client-protocol.h>
#include <fractional-scale-v1-client-protocol.h>
#include <xdg-toplevel-icon-v1-client-protocol.h>
#include "wlr_end.hpp"

#include "drm_include.h"

#include "wayland/CreateShmBuffer.hpp"
#include "wayland/WaylandBackend.hpp"
#include "wayland/WaylandConnector.hpp"
#include "wayland/WaylandFb.hpp"
#include "wayland/WaylandInputThread.hpp"
#include "wayland/WaylandPlane.hpp"
#include "wayland/convars.hpp"
#include "wayland/callback_macro.hpp"
#include "wayland/externs_core.hpp"
#include "wayland/externs_gs.hpp"
#include "wayland/tag_identify.hpp"
#include "wayland/xdg_log.hpp"

namespace gamescope
{
	// WaylandPlane.hpp

    // WaylandConnector.hpp

    // WaylandFb.hpp

	// WaylandInputThread.hpp

	// WaylandBackend.hpp

    //////////////////
    // CWaylandFb
    //////////////////

    //////////////////
    // CWaylandConnector
    //////////////////

    //////////////////
    // CWaylandPlane
    //////////////////

    ////////////////
    // CWaylandBackend
    ////////////////

    // Not const... weird.
    static libdecor_interface s_LibDecorInterface =
    {
        .error = []( libdecor *pContext, libdecor_error eError, const char *pMessage )
        {
            xdg_log.errorf( "libdecor: %s", pMessage );
        },
    };

    CWaylandBackend::CWaylandBackend()
    {
    }

    bool CWaylandBackend::Init()
    {
        g_nOutputWidth = g_nPreferredOutputWidth;
        g_nOutputHeight = g_nPreferredOutputHeight;
        g_nOutputRefresh = g_nNestedRefresh;

        // TODO: Dedupe the init of this stuff,
        // maybe move it away from globals for multi-display...
        if ( g_nOutputHeight == 0 )
        {
            if ( g_nOutputWidth != 0 )
            {
                fprintf( stderr, "Cannot specify -W without -H\n" );
                return false;
            }
            g_nOutputHeight = 720;
        }
        if ( g_nOutputWidth == 0 )
            g_nOutputWidth = g_nOutputHeight * 16 / 9;
        if ( g_nOutputRefresh == 0 )
            g_nOutputRefresh = ConvertHztomHz( 60 );

        if ( !( m_pDisplay = wl_display_connect( nullptr ) ) )
        {
            xdg_log.errorf( "Couldn't connect to Wayland display." );
            return false;
        }

        wl_registry *pRegistry;
        if ( !( pRegistry = wl_display_get_registry( m_pDisplay ) ) )
        {
            xdg_log.errorf( "Couldn't create Wayland registry." );
            return false;
        }

        wl_registry_add_listener( pRegistry, &s_RegistryListener, this );
        wl_display_roundtrip( m_pDisplay );

        if ( !m_pCompositor || !m_pSubcompositor || !m_pXdgWmBase || !m_pLinuxDmabuf || !m_pViewporter || !m_pPresentation || !m_pRelativePointerManager || !m_pPointerConstraints || !m_pShm )
        {
            xdg_log.errorf( "Couldn't create Wayland objects." );
            return false;
        }

		m_pEmptyRegion = wl_compositor_create_region( m_pCompositor );
		m_pFullRegion = wl_compositor_create_region( m_pCompositor );
		wl_region_add( m_pFullRegion, 0, 0, INT32_MAX, INT32_MAX );

        // Grab stuff from any extra bindings/listeners we set up, eg. format/modifiers.
        wl_display_roundtrip( m_pDisplay );

        wl_registry_destroy( pRegistry );
        pRegistry = nullptr;

        if ( m_pWPColorManager )
        {
            m_WPColorManagerFeatures.bSupportsGamescopeColorManagement = [this]() -> bool
            {
                // Features
                if ( !Algorithm::Contains( m_WPColorManagerFeatures.eFeatures, WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC ) )
                    return false;
                if ( !Algorithm::Contains( m_WPColorManagerFeatures.eFeatures, WP_COLOR_MANAGER_V1_FEATURE_SET_PRIMARIES ) )
                    return false;
                if ( !Algorithm::Contains( m_WPColorManagerFeatures.eFeatures, WP_COLOR_MANAGER_V1_FEATURE_SET_MASTERING_DISPLAY_PRIMARIES ) )
                    return false;
                if ( !Algorithm::Contains( m_WPColorManagerFeatures.eFeatures, WP_COLOR_MANAGER_V1_FEATURE_EXTENDED_TARGET_VOLUME ) )
                    return false;
                if ( !Algorithm::Contains( m_WPColorManagerFeatures.eFeatures, WP_COLOR_MANAGER_V1_FEATURE_SET_LUMINANCES ) )
                    return false;
                if ( !Algorithm::Contains( m_WPColorManagerFeatures.eFeatures, WP_COLOR_MANAGER_V1_FEATURE_WINDOWS_SCRGB ) )
                    return false;

                // Transfer Functions
                if ( !Algorithm::Contains( m_WPColorManagerFeatures.eTransferFunctions, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB ) )
                    return false;
                if ( !Algorithm::Contains( m_WPColorManagerFeatures.eTransferFunctions, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ ) )
                    return false;

                // Primaries
                if ( !Algorithm::Contains( m_WPColorManagerFeatures.ePrimaries, WP_COLOR_MANAGER_V1_PRIMARIES_SRGB ) )
                    return false;
                if ( !Algorithm::Contains( m_WPColorManagerFeatures.ePrimaries, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020 ) )
                    return false;

                return true;
            }();

            if ( m_WPColorManagerFeatures.bSupportsGamescopeColorManagement )
            {
                // HDR10.
                {
                    wp_image_description_creator_params_v1 *pParams = wp_color_manager_v1_create_parametric_creator( m_pWPColorManager );
                    wp_image_description_creator_params_v1_set_primaries_named( pParams, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020 );
                    wp_image_description_creator_params_v1_set_tf_named( pParams, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ );
                    m_pWPImageDescriptions[ GAMESCOPE_APP_TEXTURE_COLORSPACE_HDR10_PQ ] = wp_image_description_creator_params_v1_create( pParams );
                }

                // scRGB
                {
                    m_pWPImageDescriptions[ GAMESCOPE_APP_TEXTURE_COLORSPACE_SCRGB ] = wp_color_manager_v1_create_windows_scrgb( m_pWPColorManager );
                }
            }
        }

        m_pLibDecor = libdecor_new( m_pDisplay, &s_LibDecorInterface );
        if ( !m_pLibDecor )
        {
            xdg_log.errorf( "Failed to init libdecor." );
            return false;
        }

        if ( !vulkan_init( vulkan_get_instance(), VK_NULL_HANDLE ) )
        {
            return false;
        }
        
        if ( !wlsession_init() )
        {
            xdg_log.errorf( "Failed to initialize Wayland session" );
            return false;
        }

        if ( !m_InputThread.Init( this ) )
        {
            xdg_log.errorf( "Failed to initialize input thread" );
            return false;
        }

        xdg_log.infof( "Initted Wayland backend" );

        return true;
    }

    bool CWaylandBackend::PostInit()
    {
        if ( m_pSinglePixelBufferManager )
        {
            wl_buffer *pBlackBuffer = wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer( m_pSinglePixelBufferManager, 0, 0, 0, ~0u );
            m_pOwnedBlackFb = new CWaylandFb( this, pBlackBuffer );
            m_BlackFb = m_pOwnedBlackFb.get();
        }
        else
        {
            m_pBlackTexture = vulkan_create_flat_texture( 1, 1, 0, 0, 0, 255 );
            if ( !m_pBlackTexture )
            {
                xdg_log.errorf( "Failed to create dummy black texture." );
                return false;
            }
            m_BlackFb = static_cast<CWaylandFb *>( m_pBlackTexture->GetBackendFb() );
        }

        if ( m_BlackFb == nullptr )
        {
            xdg_log.errorf( "Failed to create 1x1 black buffer." );
            return false;
        }

        m_pDefaultCursorInfo = GetX11HostCursor();
        m_pDefaultCursorSurface = CursorInfoToSurface( m_pDefaultCursorInfo );

        xdg_log.infof( "Post-Initted Wayland backend" );

        return true;
    }

    std::span<const char *const> CWaylandBackend::GetInstanceExtensions() const
    {
        return std::span<const char *const>{};
    }

    std::span<const char *const> CWaylandBackend::GetDeviceExtensions( VkPhysicalDevice pVkPhysicalDevice ) const
    {
        return std::span<const char *const>{};
    }

    VkImageLayout CWaylandBackend::GetPresentLayout() const
    {
        return VK_IMAGE_LAYOUT_GENERAL;
    }

    void CWaylandBackend::GetPreferredOutputFormat( uint32_t *pPrimaryPlaneFormat, uint32_t *pOverlayPlaneFormat ) const
    {
        // Prefer opaque for composition on the Wayland backend.

        uint32_t u8BitFormat = DRM_FORMAT_INVALID;
        if ( SupportsFormat( DRM_FORMAT_XRGB8888 ) )
            u8BitFormat = DRM_FORMAT_XRGB8888;
        else if ( SupportsFormat( DRM_FORMAT_XBGR8888 ) )
            u8BitFormat = DRM_FORMAT_XBGR8888;
        else if ( SupportsFormat( DRM_FORMAT_ARGB8888 ) )
            u8BitFormat = DRM_FORMAT_ARGB8888;
        else if ( SupportsFormat( DRM_FORMAT_ABGR8888 ) )
            u8BitFormat = DRM_FORMAT_ABGR8888;

        uint32_t u10BitFormat = DRM_FORMAT_INVALID;
        if ( SupportsFormat( DRM_FORMAT_XBGR2101010 ) )
            u10BitFormat = DRM_FORMAT_XBGR2101010;
        else if ( SupportsFormat( DRM_FORMAT_XRGB2101010 ) )
            u10BitFormat = DRM_FORMAT_XRGB2101010;
        else if ( SupportsFormat( DRM_FORMAT_ABGR2101010 ) )
            u10BitFormat = DRM_FORMAT_ABGR2101010;
        else if ( SupportsFormat( DRM_FORMAT_ARGB2101010 ) )
            u10BitFormat = DRM_FORMAT_ARGB2101010;

        assert( u8BitFormat != DRM_FORMAT_INVALID );

        *pPrimaryPlaneFormat = u10BitFormat != DRM_FORMAT_INVALID ? u10BitFormat : u8BitFormat;
        *pOverlayPlaneFormat = u8BitFormat;
    }

    bool CWaylandBackend::ValidPhysicalDevice( VkPhysicalDevice pVkPhysicalDevice ) const
    {
        return true;
    }

    void CWaylandBackend::DirtyState( bool bForce, bool bForceModeset )
    {
    }
    bool CWaylandBackend::PollState()
    {
        wl_display_flush( m_pDisplay );

        if ( wl_display_prepare_read( m_pDisplay ) == 0 )
        {
            int nRet = 0;
            pollfd pollfd =
            {
                .fd     = wl_display_get_fd( m_pDisplay ),
                .events = POLLIN,
            };

            do
            {
                nRet = poll( &pollfd, 1, 0 );
            } while ( nRet < 0 && ( errno == EINTR || errno == EAGAIN ) );

            if ( nRet > 0 )
                wl_display_read_events( m_pDisplay );
            else
                wl_display_cancel_read( m_pDisplay );
        }

        wl_display_dispatch_pending( m_pDisplay );

        return false;
    }

    std::shared_ptr<BackendBlob> CWaylandBackend::CreateBackendBlob( const std::type_info &type, std::span<const uint8_t> data )
    {
        return std::make_shared<BackendBlob>( data );
    }

    OwningRc<IBackendFb> CWaylandBackend::ImportDmabufToBackend( wlr_dmabuf_attributes *pDmaBuf )
    {
        zwp_linux_buffer_params_v1 *pBufferParams = zwp_linux_dmabuf_v1_create_params( m_pLinuxDmabuf );
        if ( !pBufferParams )
        {
            xdg_log.errorf( "Failed to create imported dmabuf params" );
            return nullptr;
        }

        for ( int i = 0; i < pDmaBuf->n_planes; i++ )
        {
            zwp_linux_buffer_params_v1_add(
                pBufferParams,
                pDmaBuf->fd[i],
                i,
                pDmaBuf->offset[i],
                pDmaBuf->stride[i],
                pDmaBuf->modifier >> 32,
                pDmaBuf->modifier & 0xffffffff);
        }

        wl_buffer *pImportedBuffer = zwp_linux_buffer_params_v1_create_immed(
            pBufferParams,
            pDmaBuf->width,
            pDmaBuf->height,
            pDmaBuf->format,
            0u );

        if ( !pImportedBuffer )
        {
            xdg_log.errorf( "Failed to import dmabuf" );
            return nullptr;
        }

        zwp_linux_buffer_params_v1_destroy( pBufferParams );

        return new CWaylandFb{ this, pImportedBuffer };
    }

    bool CWaylandBackend::UsesModifiers() const
    {
        if ( !cv_wayland_use_modifiers )
            return false;

        return m_bCanUseModifiers;
    }
    std::span<const uint64_t> CWaylandBackend::GetSupportedModifiers( uint32_t uDrmFormat ) const
    {
        auto iter = m_FormatModifiers.find( uDrmFormat );
        if ( iter == m_FormatModifiers.end() )
            return std::span<const uint64_t>{};

        return std::span<const uint64_t>{ iter->second.begin(), iter->second.end() };
    }

    IBackendConnector *CWaylandBackend::GetCurrentConnector()
    {
        return m_pFocusConnector;
    }
    IBackendConnector *CWaylandBackend::GetConnector( GamescopeScreenType eScreenType )
    {
        if ( eScreenType == GAMESCOPE_SCREEN_TYPE_INTERNAL )
            return GetCurrentConnector();

        return nullptr;
    }

    bool CWaylandBackend::SupportsPlaneHardwareCursor() const
    {
        // We use the nested hints cursor stuff.
        // Not our own plane.
        return false;
    }

    bool CWaylandBackend::SupportsTearing() const
    {
        return false;
    }
    bool CWaylandBackend::UsesVulkanSwapchain() const
    {
        return false;
    }

    bool CWaylandBackend::IsSessionBased() const
    {
        return false;
    }

    bool CWaylandBackend::SupportsExplicitSync() const
    {
        return true;
    }

    bool CWaylandBackend::IsPaused() const
    {
        return false;
    }

    bool CWaylandBackend::IsVisible() const
    {
        return true;
    }

    glm::uvec2 CWaylandBackend::CursorSurfaceSize( glm::uvec2 uvecSize ) const
    {
        return uvecSize;
    }

    void CWaylandBackend::HackUpdatePatchedEdid()
    {
        if ( !GetCurrentConnector() )
            return;

        // XXX: We should do this a better way that handles per-window and appid stuff
        // down the line
        if ( cv_hdr_enabled && GetCurrentConnector()->GetHDRInfo().bExposeHDRSupport )
        {
            setenv( "DXVK_HDR", "1", true );
        }
        else
        {
            setenv( "DXVK_HDR", "0", true );
        }

        WritePatchedEdid( GetCurrentConnector()->GetRawEDID(), GetCurrentConnector()->GetHDRInfo(), false );
    }

    bool CWaylandBackend::UsesVirtualConnectors()
    {
        return true;
    }
    std::shared_ptr<IBackendConnector> CWaylandBackend::CreateVirtualConnector( uint64_t ulVirtualConnectorKey )
    {
        std::shared_ptr<CWaylandConnector> pConnector = std::make_shared<CWaylandConnector>( this, ulVirtualConnectorKey );
        m_pFocusConnector = pConnector.get();

        if ( !pConnector->Init() )
        {
            return nullptr;
        }

        return pConnector;
    }

    ///////////////////
    // INestedHints
    ///////////////////

    void CWaylandBackend::OnBackendBlobDestroyed( BackendBlob *pBlob )
    {
        // Do nothing.
    }

    wl_surface *CWaylandBackend::CursorInfoToSurface( const std::shared_ptr<INestedHints::CursorInfo> &info )
    {
        if ( !info )
            return nullptr;

        uint32_t uStride = info->uWidth * 4;
        uint32_t uSize = uStride * info->uHeight;

        int32_t nFd = CreateShmBuffer( uSize, info->pPixels.data() );
        if ( nFd < 0 )
            return nullptr;
        defer( close( nFd ) );

        wl_shm_pool *pPool = wl_shm_create_pool( m_pShm, nFd, uSize );
        defer( wl_shm_pool_destroy( pPool ) );

        wl_buffer *pBuffer = wl_shm_pool_create_buffer( pPool, 0, info->uWidth, info->uHeight, uStride, WL_SHM_FORMAT_ARGB8888 );
        defer( wl_buffer_destroy( pBuffer ) );

        wl_surface *pCursorSurface = wl_compositor_create_surface( m_pCompositor );
        wl_surface_attach( pCursorSurface, pBuffer, 0, 0 );
        wl_surface_damage( pCursorSurface, 0, 0, INT32_MAX, INT32_MAX );
        wl_surface_commit( pCursorSurface );

        return pCursorSurface;
    }

    bool CWaylandBackend::SupportsColorManagement() const
    {
        return m_pFrogColorMgmtFactory != nullptr || ( m_pWPColorManager != nullptr && m_WPColorManagerFeatures.bSupportsGamescopeColorManagement );
    }

    void CWaylandBackend::SetCursorImage( std::shared_ptr<INestedHints::CursorInfo> info )
    {
        m_pCursorInfo = info;

        if ( m_pCursorSurface )
        {
            wl_surface_destroy( m_pCursorSurface );
            m_pCursorSurface = nullptr;
        }

        m_pCursorSurface = CursorInfoToSurface( info );

        UpdateCursor();
    }
    void CWaylandBackend::SetRelativeMouseMode( wl_surface *pSurface, bool bRelative )
    {
        if ( !m_pPointer )
            return;

        if ( !!bRelative != !!m_pLockedPointer || ( pSurface != m_pLockedSurface && bRelative ) )
        {
            if ( m_pLockedPointer )
            {
                assert( m_pRelativePointer );

                zwp_locked_pointer_v1_destroy( m_pLockedPointer );
                m_pLockedPointer = nullptr;
                m_bPointerLocked = false;

                zwp_relative_pointer_v1_destroy( m_pRelativePointer );
                m_pRelativePointer = nullptr;

                m_pLockedSurface = nullptr;
            }

			if ( bRelative )
			{
				m_pLockedPointer = zwp_pointer_constraints_v1_lock_pointer( m_pPointerConstraints, pSurface, m_pPointer, nullptr, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT );
				zwp_locked_pointer_v1_add_listener( m_pLockedPointer, &s_LockedPointerListener, this );

				m_pRelativePointer = zwp_relative_pointer_manager_v1_get_relative_pointer( m_pRelativePointerManager, m_pPointer );

				m_pLockedSurface = pSurface;
			}

            m_InputThread.SetRelativePointer( bRelative );

            UpdateCursor();
        }
    }

    void CWaylandBackend::UpdateCursor()
    {
        bool bUseHostCursor = false;

        if ( !m_pPointer )
            return;

		if ( cv_wayland_mouse_warp_without_keyboard_focus )
			bUseHostCursor = m_bPointerLocked && !m_bKeyboardEntered && m_pDefaultCursorSurface;
		else
			bUseHostCursor = !m_bKeyboardEntered && m_pDefaultCursorSurface;

        if ( bUseHostCursor )
        {
            wl_pointer_set_cursor( m_pPointer, m_uPointerEnterSerial, m_pDefaultCursorSurface, m_pDefaultCursorInfo->uXHotspot, m_pDefaultCursorInfo->uYHotspot );
        }
        else
        {
			bool bHideCursor = m_bPointerLocked || !m_pCursorSurface;

            if ( bHideCursor )
                wl_pointer_set_cursor( m_pPointer, m_uPointerEnterSerial, nullptr, 0, 0 );
            else
                wl_pointer_set_cursor( m_pPointer, m_uPointerEnterSerial, m_pCursorSurface, m_pCursorInfo->uXHotspot, m_pCursorInfo->uYHotspot );
        }
    }

    /////////////////////
    // Wayland Callbacks
    /////////////////////

    void CWaylandBackend::Wayland_Registry_Global( wl_registry *pRegistry, uint32_t uName, const char *pInterface, uint32_t uVersion )
    {
        if ( !strcmp( pInterface, wl_compositor_interface.name ) && uVersion >= 4u )
        {
            m_pCompositor = (wl_compositor *)wl_registry_bind( pRegistry, uName, &wl_compositor_interface, 4u );
        }
        if ( !strcmp( pInterface, wp_single_pixel_buffer_manager_v1_interface.name ) )
        {
            m_pSinglePixelBufferManager = (wp_single_pixel_buffer_manager_v1 *)wl_registry_bind( pRegistry, uName, &wp_single_pixel_buffer_manager_v1_interface, 1u );
        }
        else if ( !strcmp( pInterface, wl_subcompositor_interface.name ) )
        {
            m_pSubcompositor = (wl_subcompositor *)wl_registry_bind( pRegistry, uName, &wl_subcompositor_interface, 1u );
        }
        else if ( !strcmp( pInterface, xdg_wm_base_interface.name ) && uVersion >= 1u )
        {
            static constexpr xdg_wm_base_listener s_Listener =
            {
                .ping = []( void *pData, xdg_wm_base *pXdgWmBase, uint32_t uSerial )
                {
                    xdg_wm_base_pong( pXdgWmBase, uSerial );
                }
            };
            m_pXdgWmBase = (xdg_wm_base *)wl_registry_bind( pRegistry, uName, &xdg_wm_base_interface, 1u );
            xdg_wm_base_add_listener( m_pXdgWmBase, &s_Listener, this );
        }
        else if ( !strcmp( pInterface, zwp_linux_dmabuf_v1_interface.name ) && uVersion >= 3 )
        {
            m_pLinuxDmabuf = (zwp_linux_dmabuf_v1 *)wl_registry_bind( pRegistry, uName, &zwp_linux_dmabuf_v1_interface, 3u );
            static constexpr zwp_linux_dmabuf_v1_listener s_Listener =
            {
                .format   = WAYLAND_NULL(), // Formats are also advertised by the modifier event, ignore them here.
                .modifier = WAYLAND_USERDATA_TO_THIS( CWaylandBackend, Wayland_Modifier ),
            };
            zwp_linux_dmabuf_v1_add_listener( m_pLinuxDmabuf, &s_Listener, this );
        }
        else if ( !strcmp( pInterface, wp_viewporter_interface.name ) )
        {
            m_pViewporter = (wp_viewporter *)wl_registry_bind( pRegistry, uName, &wp_viewporter_interface, 1u );
        }
        else if ( !strcmp( pInterface, wl_seat_interface.name ) && uVersion >= 8u )
        {
            m_pSeat = (wl_seat *)wl_registry_bind( pRegistry, uName, &wl_seat_interface, 8u );
            wl_seat_add_listener( m_pSeat, &s_SeatListener, this );
        }
        else if ( !strcmp( pInterface, wp_presentation_interface.name ) )
        {
            m_pPresentation = (wp_presentation *)wl_registry_bind( pRegistry, uName, &wp_presentation_interface, 1u );
        }
        else if ( !strcmp( pInterface, wl_output_interface.name ) )
        {
            wl_output *pOutput  = (wl_output *)wl_registry_bind( pRegistry, uName, &wl_output_interface, 4u );
            wl_output_add_listener( pOutput , &s_OutputListener, this );
            m_pOutputs.emplace( std::make_pair<struct wl_output *, WaylandOutputInfo>( std::move( pOutput ), WaylandOutputInfo{} ) );
        }
        else if ( !strcmp( pInterface, frog_color_management_factory_v1_interface.name ) )
        {
            m_pFrogColorMgmtFactory = (frog_color_management_factory_v1 *)wl_registry_bind( pRegistry, uName, &frog_color_management_factory_v1_interface, 1u );
        }
        else if ( !strcmp( pInterface, wp_color_manager_v1_interface.name ) )
        {
            m_pWPColorManager = (wp_color_manager_v1 *)wl_registry_bind( pRegistry, uName, &wp_color_manager_v1_interface, 1u );
            wp_color_manager_v1_add_listener( m_pWPColorManager, &s_WPColorManagerListener, this );
        }
        else if ( !strcmp( pInterface, zwp_pointer_constraints_v1_interface.name ) )
        {
            m_pPointerConstraints = (zwp_pointer_constraints_v1 *)wl_registry_bind( pRegistry, uName, &zwp_pointer_constraints_v1_interface, 1u );
        }
        else if ( !strcmp( pInterface, zwp_relative_pointer_manager_v1_interface.name ) )
        {
            m_pRelativePointerManager = (zwp_relative_pointer_manager_v1 *)wl_registry_bind( pRegistry, uName, &zwp_relative_pointer_manager_v1_interface, 1u );
        }
        else if ( !strcmp( pInterface, wp_fractional_scale_manager_v1_interface.name ) )
        {
            m_pFractionalScaleManager = (wp_fractional_scale_manager_v1 *)wl_registry_bind( pRegistry, uName, &wp_fractional_scale_manager_v1_interface, 1u );
        }
        else if ( !strcmp( pInterface, wl_shm_interface.name ) )
        {
            m_pShm = (wl_shm *)wl_registry_bind( pRegistry, uName, &wl_shm_interface, 1u );
        }
        else if ( !strcmp( pInterface, xdg_toplevel_icon_manager_v1_interface.name ) )
        {
            m_pToplevelIconManager = (xdg_toplevel_icon_manager_v1 *)wl_registry_bind( pRegistry, uName, &xdg_toplevel_icon_manager_v1_interface, 1u );
        }
        else if ( !strcmp( pInterface, wl_data_device_manager_interface.name ) )
        {
            m_pDataDeviceManager = (wl_data_device_manager *)wl_registry_bind( pRegistry, uName, &wl_data_device_manager_interface, 3u );
        }
        else if ( !strcmp( pInterface, zwp_primary_selection_device_manager_v1_interface.name ) )
        {
            m_pPrimarySelectionDeviceManager = (zwp_primary_selection_device_manager_v1 *)wl_registry_bind( pRegistry, uName, &zwp_primary_selection_device_manager_v1_interface, 1u );
        }
    }

    void CWaylandBackend::Wayland_Modifier( zwp_linux_dmabuf_v1 *pDmabuf, uint32_t uFormat, uint32_t uModifierHi, uint32_t uModifierLo )
    {
        uint64_t ulModifier = ( uint64_t( uModifierHi ) << 32 ) | uModifierLo;

#if 0
        const char *pszExtraModifierName = "";
        if ( ulModifier == DRM_FORMAT_MOD_INVALID )
            pszExtraModifierName = " (Invalid)";
        if ( ulModifier == DRM_FORMAT_MOD_LINEAR )
            pszExtraModifierName = " (Invalid)";

        xdg_log.infof( "Modifier: %s (0x%" PRIX32 ") %lx%s", drmGetFormatName( uFormat ), uFormat, ulModifier, pszExtraModifierName );
#endif

        if ( ulModifier != DRM_FORMAT_MOD_INVALID )
            m_bCanUseModifiers = true;

        m_FormatModifiers[uFormat].emplace_back( ulModifier );
    }

    // Output

    void CWaylandBackend::Wayland_Output_Geometry( wl_output *pOutput, int32_t nX, int32_t nY, int32_t nPhysicalWidth, int32_t nPhysicalHeight, int32_t nSubpixel, const char *pMake, const char *pModel, int32_t nTransform )
    {
    }
    void CWaylandBackend::Wayland_Output_Mode( wl_output *pOutput, uint32_t uFlags, int32_t nWidth, int32_t nHeight, int32_t nRefresh )
    {
        m_pOutputs[ pOutput ].nRefresh = nRefresh;
    }
    void CWaylandBackend::Wayland_Output_Done( wl_output *pOutput )
    {
    }
    void CWaylandBackend::Wayland_Output_Scale( wl_output *pOutput, int32_t nFactor )
    {
        m_pOutputs[ pOutput ].nScale = nFactor;
    }
    void CWaylandBackend::Wayland_Output_Name( wl_output *pOutput, const char *pName )
    {
    }
    void CWaylandBackend::Wayland_Output_Description( wl_output *pOutput, const char *pDescription )
    {
    }

    // Seat

    void CWaylandBackend::Wayland_Seat_Capabilities( wl_seat *pSeat, uint32_t uCapabilities )
    {
        if ( !!( uCapabilities & WL_SEAT_CAPABILITY_POINTER ) != !!m_pPointer )
        {
            if ( m_pPointer )
            {
                wl_pointer_release( m_pPointer );
                m_pPointer = nullptr;
            }
            else
            {
                m_pPointer = wl_seat_get_pointer( m_pSeat );
                wl_pointer_add_listener( m_pPointer, &s_PointerListener, this );
            }
        }

        if ( !!( uCapabilities & WL_SEAT_CAPABILITY_KEYBOARD ) != !!m_pKeyboard )
        {
            if ( m_pKeyboard )
            {
                wl_keyboard_release( m_pKeyboard );
                m_pKeyboard = nullptr;
            }
            else
            {
                m_pKeyboard = wl_seat_get_keyboard( m_pSeat );
                wl_keyboard_add_listener( m_pKeyboard, &s_KeyboardListener, this );
            }
        }
    }

    // Pointer

    void CWaylandBackend::Wayland_Pointer_Enter( wl_pointer *pPointer, uint32_t uSerial, wl_surface *pSurface, wl_fixed_t fSurfaceX, wl_fixed_t fSurfaceY )
    {
		if ( !IsGamescopeToplevel( pSurface ) )
			return;

        m_uPointerEnterSerial = uSerial;
        m_bMouseEntered = true;

        UpdateCursor();
    }
    void CWaylandBackend::Wayland_Pointer_Leave( wl_pointer *pPointer, uint32_t uSerial, wl_surface *pSurface )
    {
		if ( !IsGamescopeToplevel( pSurface ) )
			return;

        m_bMouseEntered = false;
    }

    // Keyboard

    void CWaylandBackend::Wayland_Keyboard_Enter( wl_keyboard *pKeyboard, uint32_t uSerial, wl_surface *pSurface, wl_array *pKeys )
    {
		if ( !IsGamescopeToplevel( pSurface ) )
			return;

        m_uKeyboardEnterSerial = uSerial;
        m_bKeyboardEntered = true;

        UpdateCursor();
    }
    void CWaylandBackend::Wayland_Keyboard_Leave( wl_keyboard *pKeyboard, uint32_t uSerial, wl_surface *pSurface )
    {
		if ( !IsGamescopeToplevel( pSurface ) )
			return;

        m_bKeyboardEntered = false;

        UpdateCursor();
    }

	void CWaylandBackend::Wayland_LockedPointer_Locked( zwp_locked_pointer_v1 *pLockedPointer )
	{
		m_bPointerLocked = true;
		UpdateCursor();
	}
	void CWaylandBackend::Wayland_LockedPointer_Unlocked( zwp_locked_pointer_v1 *pLockedPointer )
	{
		m_bPointerLocked = false;
		UpdateCursor();
	}

    // WP Color Manager

    void CWaylandBackend::Wayland_WPColorManager_SupportedIntent( wp_color_manager_v1 *pWPColorManager, uint32_t uRenderIntent )
    {
        m_WPColorManagerFeatures.eRenderIntents.push_back( static_cast<wp_color_manager_v1_render_intent>( uRenderIntent ) );
    }
    void CWaylandBackend::Wayland_WPColorManager_SupportedFeature( wp_color_manager_v1 *pWPColorManager, uint32_t uFeature )
    {
        m_WPColorManagerFeatures.eFeatures.push_back( static_cast<wp_color_manager_v1_feature>( uFeature ) );
    }
    void CWaylandBackend::Wayland_WPColorManager_SupportedTFNamed( wp_color_manager_v1 *pWPColorManager, uint32_t uTF )
    {
        m_WPColorManagerFeatures.eTransferFunctions.push_back( static_cast<wp_color_manager_v1_transfer_function>( uTF ) );
    }
    void CWaylandBackend::Wayland_WPColorManager_SupportedPrimariesNamed( wp_color_manager_v1 *pWPColorManager, uint32_t uPrimaries )
    {
        m_WPColorManagerFeatures.ePrimaries.push_back( static_cast<wp_color_manager_v1_primaries>( uPrimaries ) );
    }
    void CWaylandBackend::Wayland_WPColorManager_ColorManagerDone( wp_color_manager_v1 *pWPColorManager )
    {

    }

    // Data Source

    void CWaylandBackend::Wayland_DataSource_Send( struct wl_data_source *pSource, const char *pMime, int nFd )
    {
        ssize_t len = m_pClipboard->length();
        if ( write( nFd, m_pClipboard->c_str(), len ) != len )
            xdg_log.infof( "Failed to write all %zd bytes to clipboard", len );
        close( nFd );
    }
    void CWaylandBackend::Wayland_DataSource_Cancelled( struct wl_data_source *pSource )
    {
        wl_data_source_destroy( pSource );
    }

    // Primary Selection Source

    void CWaylandBackend::Wayland_PrimarySelectionSource_Send( struct zwp_primary_selection_source_v1 *pSource, const char *pMime, int nFd )
    {
	ssize_t len = m_pPrimarySelection->length();
        if ( write( nFd, m_pPrimarySelection->c_str(), len ) != len )
	    xdg_log.infof( "Failed to write all %zd bytes to clipboard", len );
        close( nFd );
    }
    void CWaylandBackend::Wayland_PrimarySelectionSource_Cancelled( struct zwp_primary_selection_source_v1 *pSource)
    {
        zwp_primary_selection_source_v1_destroy( pSource );
    }

    ///////////////////////
    // CWaylandInputThread
    ///////////////////////

    /////////////////////////
    // Backend Instantiator
    /////////////////////////

    template <>
    bool IBackend::Set<CWaylandBackend>()
    {
        return Set( new CWaylandBackend{} );
    }
}
