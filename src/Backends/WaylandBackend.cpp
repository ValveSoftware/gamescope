#include "backend.h"
#include "rendervulkan.hpp"
#include "wlserver.hpp"
#include "vblankmanager.hpp"
#include "steamcompmgr.hpp"
#include "edid.h"
#include "Utils/Defer.h"
#include "Utils/Algorithm.h"
#include "convar.h"
#include "refresh_rate.h"
#include "waitable.h"
#include "Utils/TempFiles.h"

#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <csignal>
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

#define WL_FRACTIONAL_SCALE_DENOMINATOR 120

extern int g_nPreferredOutputWidth;
extern int g_nPreferredOutputHeight;
extern bool g_bForceHDR10OutputDebug;
extern bool g_bBorderlessOutputWindow;
extern gamescope::ConVar<bool> cv_adaptive_sync;

extern gamescope::ConVar<bool> cv_composite_force;
extern bool g_bColorSliderInUse;
extern bool fadingOut;
extern std::string g_reshade_effect;

using namespace std::literals;

static LogScope xdg_log( "xdg_backend" );

static const char *GAMESCOPE_plane_tag = "gamescope-plane";

template <typename Func, typename... Args>
auto CallWithAllButLast(Func pFunc, Args&&... args)
{
    auto Forwarder = [&] <typename Tuple, size_t... idx> (Tuple&& tuple, std::index_sequence<idx...>)
    {
        return pFunc(std::get<idx>(std::forward<Tuple>(tuple))...);
    };
    return Forwarder(std::forward_as_tuple(args...), std::make_index_sequence<sizeof...(Args) - 1>());
}

static inline uint32_t WaylandScaleToPhysical( uint32_t pValue, uint32_t pFactor ) {
    return pValue * pFactor / WL_FRACTIONAL_SCALE_DENOMINATOR;
}
static inline uint32_t WaylandScaleToLogical( uint32_t pValue, uint32_t pFactor ) {
    return div_roundup( pValue * WL_FRACTIONAL_SCALE_DENOMINATOR, pFactor );
}

static bool IsSurfacePlane( wl_surface *pSurface ) {
    // HACK: this probably should never be called with a null pointer, but it
    // was happening after a window was closed.
    return pSurface && (wl_proxy_get_tag( (wl_proxy *)pSurface ) == &GAMESCOPE_plane_tag);
}

#define WAYLAND_NULL() []<typename... Args> ( void *pData, Args... args ) { }
#define WAYLAND_USERDATA_TO_THIS(type, name) []<typename... Args> ( void *pData, Args... args ) { type *pThing = (type *)pData; pThing->name( std::forward<Args>(args)... ); }

// Libdecor puts its userdata ptr at the end... how fun! I shouldn't have spent so long writing this total atrocity to mankind.
#define LIBDECOR_USERDATA_TO_THIS(type, name) []<typename... Args> ( Args... args ) { type *pThing = (type *)std::get<sizeof...(Args)-1>(std::forward_as_tuple(args...)); CallWithAllButLast([&]<typename... Args2>(Args2... args2){ pThing->name(std::forward<Args2>(args2)...); }, std::forward<Args>(args)...); }

extern gamescope::ConVar<bool> cv_hdr_enabled;

namespace gamescope
{
    extern std::shared_ptr<INestedHints::CursorInfo> GetX11HostCursor();

    gamescope::ConVar<bool> cv_wayland_mouse_warp_without_keyboard_focus( "wayland_mouse_warp_without_keyboard_focus", true, "Should we only forward mouse warps to the app when we have keyboard focus?" );
    gamescope::ConVar<bool> cv_wayland_mouse_relmotion_without_keyboard_focus( "wayland_mouse_relmotion_without_keyboard_focus", false, "Should we only forward mouse relative motion to the app when we have keyboard focus?" );
    gamescope::ConVar<bool> cv_wayland_use_modifiers( "wayland_use_modifiers", true, "Use DMA-BUF modifiers?" );

    gamescope::ConVar<float> cv_wayland_hdr10_saturation_scale( "wayland_hdr10_saturation_scale", 1.0, "Saturation scale for HDR10 content by gamut expansion. 1.0 - 1.2 is a good range to play with." );

    class CWaylandConnector;
    class CWaylandPlane;
    class CWaylandBackend;
    class CWaylandFb;

    struct WaylandPlaneState
    {
        wl_buffer *pBuffer;
        int32_t nDestX;
        int32_t nDestY;
        double flSrcX;
        double flSrcY;
        double flSrcWidth;
        double flSrcHeight;
        int32_t nDstWidth;
        int32_t nDstHeight;
        GamescopeAppTextureColorspace eColorspace;
        std::shared_ptr<gamescope::BackendBlob> pHDRMetadata;
        bool bOpaque;
        uint32_t uFractionalScale;
    };

    inline WaylandPlaneState ClipPlane( const WaylandPlaneState &state )
    {
        int32_t nClippedDstWidth  = std::min<int32_t>( g_nOutputWidth,  state.nDstWidth  + state.nDestX ) - state.nDestX;
        int32_t nClippedDstHeight = std::min<int32_t>( g_nOutputHeight, state.nDstHeight + state.nDestY ) - state.nDestY;
        double flClippedSrcWidth  = state.flSrcWidth  * ( nClippedDstWidth  / double( state.nDstWidth ) );
        double flClippedSrcHeight = state.flSrcHeight * ( nClippedDstHeight / double( state.nDstHeight ) );

        WaylandPlaneState outState = state;
        outState.nDstWidth   = nClippedDstWidth;
        outState.nDstHeight  = nClippedDstHeight;
        outState.flSrcWidth  = flClippedSrcWidth;
        outState.flSrcHeight = flClippedSrcHeight;
        return outState;
    }

    static int CreateShmBuffer( uint32_t uSize, void *pData )
    {
        char szShmBufferPath[ PATH_MAX ];
        int nFd = MakeTempFile( szShmBufferPath, k_szGamescopeTempShmTemplate );
        if ( nFd < 0 )
            return -1;

        if ( ftruncate( nFd, uSize ) < 0 )
        {
            close( nFd );
            return -1;
        }

        if ( pData )
        {
            void *pMappedData = mmap( nullptr, uSize, PROT_READ | PROT_WRITE, MAP_SHARED, nFd, 0 );
            if ( pMappedData == MAP_FAILED )
                return -1;
            defer( munmap( pMappedData, uSize ) );

            memcpy( pMappedData, pData, uSize );
        }

        return nFd;
    }

    struct WaylandPlaneColorState
    {
        GamescopeAppTextureColorspace eColorspace;
        std::shared_ptr<gamescope::BackendBlob> pHDRMetadata;

        bool operator ==( const WaylandPlaneColorState &other ) const = default;
        bool operator !=( const WaylandPlaneColorState &other ) const = default;
    };

    class CWaylandPlane
    {
    public:
        CWaylandPlane( CWaylandConnector *pBackend );
        ~CWaylandPlane();

        bool Init( CWaylandPlane *pParent, CWaylandPlane *pSiblingBelow );

        uint32_t GetScale() const;

        void Present( std::optional<WaylandPlaneState> oState );
        void Present( const FrameInfo_t::Layer_t *pLayer );

        void CommitLibDecor( libdecor_configuration *pConfiguration );
        void Commit();

        wl_surface *GetSurface() const { return m_pSurface; }
        libdecor_frame *GetFrame() const { return m_pFrame; }
        xdg_toplevel *GetXdgToplevel() const;

        std::optional<WaylandPlaneState> GetCurrentState() { std::unique_lock lock( m_PlaneStateLock ); return m_oCurrentPlaneState; }

        void UpdateVRRRefreshRate();

    private:

        void Wayland_Surface_Enter( wl_surface *pSurface, wl_output *pOutput );
        void Wayland_Surface_Leave( wl_surface *pSurface, wl_output *pOutput );
        static const wl_surface_listener s_SurfaceListener;

        void LibDecor_Frame_Configure( libdecor_frame *pFrame, libdecor_configuration *pConfiguration );
        void LibDecor_Frame_Close( libdecor_frame *pFrame );
        void LibDecor_Frame_Commit( libdecor_frame *pFrame );
        void LibDecor_Frame_DismissPopup( libdecor_frame *pFrame, const char *pSeatName );
        static libdecor_frame_interface s_LibDecorFrameInterface;

        void Wayland_PresentationFeedback_SyncOutput( struct wp_presentation_feedback *pFeedback, wl_output *pOutput );
        void Wayland_PresentationFeedback_Presented( struct wp_presentation_feedback *pFeedback, uint32_t uTVSecHi, uint32_t uTVSecLo, uint32_t uTVNSec, uint32_t uRefresh, uint32_t uSeqHi, uint32_t uSeqLo, uint32_t uFlags );
        void Wayland_PresentationFeedback_Discarded( struct wp_presentation_feedback *pFeedback );
        static const wp_presentation_feedback_listener s_PresentationFeedbackListener;

        void Wayland_FrogColorManagedSurface_PreferredMetadata(
            frog_color_managed_surface *pFrogSurface,
            uint32_t uTransferFunction,
            uint32_t uOutputDisplayPrimaryRedX,
            uint32_t uOutputDisplayPrimaryRedY,
            uint32_t uOutputDisplayPrimaryGreenX,
            uint32_t uOutputDisplayPrimaryGreenY,
            uint32_t uOutputDisplayPrimaryBlueX,
            uint32_t uOutputDisplayPrimaryBlueY,
            uint32_t uOutputWhitePointX,
            uint32_t uOutputWhitePointY,
            uint32_t uMaxLuminance,
            uint32_t uMinLuminance,
            uint32_t uMaxFullFrameLuminance );
        static const frog_color_managed_surface_listener s_FrogColorManagedSurfaceListener;

        void Wayland_WPColorManagementSurfaceFeedback_PreferredChanged( wp_color_management_surface_feedback_v1 *pColorManagementSurface, unsigned int data );
        static const wp_color_management_surface_feedback_v1_listener s_WPColorManagementSurfaceListener;
        void UpdateWPPreferredColorManagement();

        void Wayland_WPImageDescriptionInfo_Done( wp_image_description_info_v1 *pImageDescInfo );
        void Wayland_WPImageDescriptionInfo_ICCFile( wp_image_description_info_v1 *pImageDescInfo, int32_t nICCFd, uint32_t uICCSize );
        void Wayland_WPImageDescriptionInfo_Primaries( wp_image_description_info_v1 *pImageDescInfo, int32_t nRedX, int32_t nRedY, int32_t nGreenX, int32_t nGreenY, int32_t nBlueX, int32_t nBlueY, int32_t nWhiteX, int32_t nWhiteY );
        void Wayland_WPImageDescriptionInfo_PrimariesNamed( wp_image_description_info_v1 *pImageDescInfo, uint32_t uPrimaries );
        void Wayland_WPImageDescriptionInfo_TFPower( wp_image_description_info_v1 *pImageDescInfo, uint32_t uExp);
        void Wayland_WPImageDescriptionInfo_TFNamed( wp_image_description_info_v1 *pImageDescInfo, uint32_t uTF);
        void Wayland_WPImageDescriptionInfo_Luminances( wp_image_description_info_v1 *pImageDescInfo, uint32_t uMinLum, uint32_t uMaxLum, uint32_t uRefLum );
        void Wayland_WPImageDescriptionInfo_TargetPrimaries( wp_image_description_info_v1 *pImageDescInfo, int32_t nRedX, int32_t nRedY, int32_t nGreenX, int32_t nGreenY, int32_t nBlueX, int32_t nBlueY, int32_t nWhiteX, int32_t nWhiteY );
        void Wayland_WPImageDescriptionInfo_TargetLuminance( wp_image_description_info_v1 *pImageDescInfo, uint32_t uMinLum, uint32_t uMaxLum );
        void Wayland_WPImageDescriptionInfo_Target_MaxCLL( wp_image_description_info_v1 *pImageDescInfo, uint32_t uMaxCLL );
        void Wayland_WPImageDescriptionInfo_Target_MaxFALL( wp_image_description_info_v1 *pImageDescInfo, uint32_t uMaxFALL );
        static const wp_image_description_info_v1_listener s_ImageDescriptionInfoListener;

        void Wayland_FractionalScale_PreferredScale( wp_fractional_scale_v1 *pFractionalScale, uint32_t uScale );
        static const wp_fractional_scale_v1_listener s_FractionalScaleListener;

        CWaylandConnector *m_pConnector = nullptr;
        CWaylandBackend *m_pBackend = nullptr;

        CWaylandPlane *m_pParent = nullptr;
        wl_surface *m_pSurface = nullptr;
        wp_viewport *m_pViewport = nullptr;
        frog_color_managed_surface *m_pFrogColorManagedSurface = nullptr;
        wp_color_management_surface_v1 *m_pWPColorManagedSurface = nullptr;
        wp_color_management_surface_feedback_v1 *m_pWPColorManagedSurfaceFeedback = nullptr;
        wp_fractional_scale_v1 *m_pFractionalScale = nullptr;
        wl_subsurface *m_pSubsurface = nullptr;
        libdecor_frame *m_pFrame = nullptr;
        libdecor_window_state m_eWindowState = LIBDECOR_WINDOW_STATE_NONE;
        std::vector<wl_output *> m_pOutputs;
        bool m_bNeedsDecorCommit = false;
        uint32_t m_uFractionalScale = 120;
        bool m_bHasRecievedScale = false;

        std::optional<WaylandPlaneColorState> m_ColorState{};
        wp_image_description_v1 *m_pCurrentImageDescription = nullptr;

        std::mutex m_PlaneStateLock;
        std::optional<WaylandPlaneState> m_oCurrentPlaneState;
    };
    const wl_surface_listener CWaylandPlane::s_SurfaceListener =
    {
        .enter = WAYLAND_USERDATA_TO_THIS( CWaylandPlane, Wayland_Surface_Enter ),
        .leave = WAYLAND_USERDATA_TO_THIS( CWaylandPlane, Wayland_Surface_Leave ),
        .preferred_buffer_scale = WAYLAND_NULL(),
        .preferred_buffer_transform = WAYLAND_NULL(),
    };
    // Can't be const, libdecor api bad...
    libdecor_frame_interface CWaylandPlane::s_LibDecorFrameInterface =
    {
	    .configure     = LIBDECOR_USERDATA_TO_THIS( CWaylandPlane, LibDecor_Frame_Configure ),
        .close         = LIBDECOR_USERDATA_TO_THIS( CWaylandPlane, LibDecor_Frame_Close ),
        .commit        = LIBDECOR_USERDATA_TO_THIS( CWaylandPlane, LibDecor_Frame_Commit ),
        .dismiss_popup = LIBDECOR_USERDATA_TO_THIS( CWaylandPlane, LibDecor_Frame_DismissPopup ),
    };
    const wp_presentation_feedback_listener CWaylandPlane::s_PresentationFeedbackListener =
    {
        .sync_output = WAYLAND_USERDATA_TO_THIS( CWaylandPlane, Wayland_PresentationFeedback_SyncOutput ),
        .presented   = WAYLAND_USERDATA_TO_THIS( CWaylandPlane, Wayland_PresentationFeedback_Presented ),
        .discarded   = WAYLAND_USERDATA_TO_THIS( CWaylandPlane, Wayland_PresentationFeedback_Discarded ),
    };
    const frog_color_managed_surface_listener CWaylandPlane::s_FrogColorManagedSurfaceListener =
    {
        .preferred_metadata = WAYLAND_USERDATA_TO_THIS( CWaylandPlane, Wayland_FrogColorManagedSurface_PreferredMetadata ),
    };
    const wp_color_management_surface_feedback_v1_listener CWaylandPlane::s_WPColorManagementSurfaceListener =
    {
        .preferred_changed = WAYLAND_USERDATA_TO_THIS( CWaylandPlane, Wayland_WPColorManagementSurfaceFeedback_PreferredChanged ),
    };
    const wp_image_description_info_v1_listener CWaylandPlane::s_ImageDescriptionInfoListener =
    {
        .done = WAYLAND_USERDATA_TO_THIS( CWaylandPlane, Wayland_WPImageDescriptionInfo_Done ),
        .icc_file = WAYLAND_USERDATA_TO_THIS( CWaylandPlane, Wayland_WPImageDescriptionInfo_ICCFile ),
        .primaries = WAYLAND_USERDATA_TO_THIS( CWaylandPlane, Wayland_WPImageDescriptionInfo_Primaries ),
        .primaries_named = WAYLAND_USERDATA_TO_THIS( CWaylandPlane, Wayland_WPImageDescriptionInfo_PrimariesNamed ),
        .tf_power = WAYLAND_USERDATA_TO_THIS( CWaylandPlane, Wayland_WPImageDescriptionInfo_TFPower ),
        .tf_named = WAYLAND_USERDATA_TO_THIS( CWaylandPlane, Wayland_WPImageDescriptionInfo_TFNamed ),
        .luminances = WAYLAND_USERDATA_TO_THIS( CWaylandPlane, Wayland_WPImageDescriptionInfo_Luminances ),
        .target_primaries = WAYLAND_USERDATA_TO_THIS( CWaylandPlane, Wayland_WPImageDescriptionInfo_TargetPrimaries ),
        .target_luminance = WAYLAND_USERDATA_TO_THIS( CWaylandPlane, Wayland_WPImageDescriptionInfo_TargetLuminance ),
        .target_max_cll = WAYLAND_USERDATA_TO_THIS( CWaylandPlane, Wayland_WPImageDescriptionInfo_Target_MaxCLL ),
        .target_max_fall = WAYLAND_USERDATA_TO_THIS( CWaylandPlane, Wayland_WPImageDescriptionInfo_Target_MaxFALL ),
    };
    const wp_fractional_scale_v1_listener CWaylandPlane::s_FractionalScaleListener =
    {
        .preferred_scale = WAYLAND_USERDATA_TO_THIS( CWaylandPlane, Wayland_FractionalScale_PreferredScale ),
    };

    enum WaylandModifierIndex
    {
        GAMESCOPE_WAYLAND_MOD_CTRL,
        GAMESCOPE_WAYLAND_MOD_SHIFT,
        GAMESCOPE_WAYLAND_MOD_ALT,
        GAMESCOPE_WAYLAND_MOD_META, // Super
        GAMESCOPE_WAYLAND_MOD_NUM,
        GAMESCOPE_WAYLAND_MOD_CAPS,

        GAMESCOPE_WAYLAND_MOD_COUNT,
    };

    constexpr const char *WaylandModifierToXkbModifierName( WaylandModifierIndex eIndex )
    {
        switch ( eIndex )
        {
            case GAMESCOPE_WAYLAND_MOD_CTRL:
                return XKB_MOD_NAME_CTRL;
            case GAMESCOPE_WAYLAND_MOD_SHIFT:
                return XKB_MOD_NAME_SHIFT;
            case GAMESCOPE_WAYLAND_MOD_ALT:
                return XKB_MOD_NAME_ALT;
            case GAMESCOPE_WAYLAND_MOD_META:
                return XKB_MOD_NAME_LOGO;
            case GAMESCOPE_WAYLAND_MOD_NUM:
                return XKB_MOD_NAME_NUM;
            case GAMESCOPE_WAYLAND_MOD_CAPS:
                return XKB_MOD_NAME_CAPS;
            default:
                return "Unknown";
        }
    }

    struct WaylandOutputInfo
    {
        int32_t nRefresh = 60;
        int32_t nScale = 1;
    };


    class CWaylandConnector final : public CBaseBackendConnector, public INestedHints
    {
    public:
        CWaylandConnector( CWaylandBackend *pBackend, uint64_t ulVirtualConnectorKey );
        virtual ~CWaylandConnector();

        bool UpdateEdid();
        bool Init();
        void SetFullscreen( bool bFullscreen ); // Thread safe, can be called from the input thread.
        void UpdateFullscreenState();


        bool HostCompositorIsCurrentlyVRR() const { return m_bHostCompositorIsCurrentlyVRR; }
        void SetHostCompositorIsCurrentlyVRR( bool bActive ) { m_bHostCompositorIsCurrentlyVRR = bActive; }
        bool CurrentDisplaySupportsVRR() const { return HostCompositorIsCurrentlyVRR(); }
        CWaylandBackend *GetBackend() const { return m_pBackend; }

        /////////////////////
        // IBackendConnector
        /////////////////////

        virtual int Present( const FrameInfo_t *pFrameInfo, bool bAsync ) override;

        virtual gamescope::GamescopeScreenType GetScreenType() const override;
        virtual GamescopePanelOrientation GetCurrentOrientation() const override;
        virtual bool SupportsHDR() const override;
        virtual bool IsHDRActive() const override;
        virtual const BackendConnectorHDRInfo &GetHDRInfo() const override;
        virtual bool IsVRRActive() const override;
        virtual std::span<const BackendMode> GetModes() const override;

        virtual bool SupportsVRR() const override;

        virtual std::span<const uint8_t> GetRawEDID() const override;
        virtual std::span<const uint32_t> GetValidDynamicRefreshRates() const override;

        virtual void GetNativeColorimetry(
            bool bHDR10,
            displaycolorimetry_t *displayColorimetry, EOTF *displayEOTF,
            displaycolorimetry_t *outputEncodingColorimetry, EOTF *outputEncodingEOTF ) const override;

        virtual const char *GetName() const override
        {
            return "Wayland";
        }
        virtual const char *GetMake() const override
        {
            return "Gamescope";
        }
        virtual const char *GetModel() const override
        {
            return "Virtual Display";
        }

        virtual INestedHints *GetNestedHints() override
        {
            return this;
        }

        ///////////////////
        // INestedHints
        ///////////////////

        virtual void SetCursorImage( std::shared_ptr<INestedHints::CursorInfo> info ) override;
        virtual void SetRelativeMouseMode( bool bRelative ) override;
        virtual void SetVisible( bool bVisible ) override;
        virtual void SetTitle( std::shared_ptr<std::string> szTitle ) override;
        virtual void SetIcon( std::shared_ptr<std::vector<uint32_t>> uIconPixels ) override;
        virtual void SetSelection( std::shared_ptr<std::string> szContents, GamescopeSelection eSelection ) override;
    private:

        friend CWaylandPlane;

        BackendConnectorHDRInfo m_HDRInfo{};
        displaycolorimetry_t m_DisplayColorimetry = displaycolorimetry_709;
        std::vector<uint8_t> m_FakeEdid;

        CWaylandBackend *m_pBackend = nullptr;

        CWaylandPlane m_Planes[8];
        bool m_bVisible = true;
        std::atomic<bool> m_bDesiredFullscreenState = { false };

        bool m_bHostCompositorIsCurrentlyVRR = false;
    };

    class CWaylandFb final : public CBaseBackendFb
    {
    public:
        CWaylandFb( CWaylandBackend *pBackend, wl_buffer *pHostBuffer );
        ~CWaylandFb();

        void OnCompositorAcquire();
        void OnCompositorRelease();

        wl_buffer *GetHostBuffer() const { return m_pHostBuffer; }
        wlr_buffer *GetClientBuffer() const { return m_pClientBuffer; }

        void Wayland_Buffer_Release( wl_buffer *pBuffer );
        static const wl_buffer_listener s_BufferListener;

    private:
        CWaylandBackend *m_pBackend = nullptr;
        wl_buffer *m_pHostBuffer = nullptr;
        wlr_buffer *m_pClientBuffer = nullptr;
        bool m_bCompositorAcquired = false;
    };
    const wl_buffer_listener CWaylandFb::s_BufferListener =
    {
        .release = WAYLAND_USERDATA_TO_THIS( CWaylandFb, Wayland_Buffer_Release ),
    };

    class CWaylandInputThread
    {
    public:
        CWaylandInputThread();
        ~CWaylandInputThread();

        bool Init( CWaylandBackend *pBackend );

        void ThreadFunc();

        // This is only shared_ptr because it works nicely
        // with std::atomic, std::any and such and makes it very easy.
        //
        // It could be a std::unique_ptr if you added a mutex,
        // but I didn't seem it worth it.
        template <typename T>
        std::shared_ptr<T> QueueLaunder( T* pObject );

        void SetRelativePointer( bool bRelative );

    private:

        void HandleKey( uint32_t uKey, bool bPressed );

        CWaylandBackend *m_pBackend = nullptr;

        CWaiter<4> m_Waiter;

        std::thread m_Thread;
        std::atomic<bool> m_bInitted = { false };

        uint32_t m_uPointerEnterSerial = 0;
        bool m_bMouseEntered = false;
        bool m_bKeyboardEntered = false;

        wl_event_queue *m_pQueue = nullptr;
        std::shared_ptr<wl_display> m_pDisplayWrapper;

        wl_seat *m_pSeat = nullptr;
        wl_keyboard *m_pKeyboard = nullptr;
        wl_pointer *m_pPointer = nullptr;
        wl_touch *m_pTouch = nullptr;
        zwp_relative_pointer_manager_v1 *m_pRelativePointerManager = nullptr;

        uint32_t m_uFakeTimestamp = 0;

        xkb_context *m_pXkbContext = nullptr;
        xkb_keymap *m_pXkbKeymap = nullptr;

        uint32_t m_uKeyModifiers = 0;
        uint32_t m_uModMask[ GAMESCOPE_WAYLAND_MOD_COUNT ];

        double m_flScrollAccum[2] = { 0.0, 0.0 };
        uint32_t m_uAxisSource = WL_POINTER_AXIS_SOURCE_WHEEL;

        CWaylandPlane *m_pCurrentCursorPlane = nullptr;

        std::optional<wl_fixed_t> m_ofPendingCursorX;
        std::optional<wl_fixed_t> m_ofPendingCursorY;

        std::atomic<std::shared_ptr<zwp_relative_pointer_v1>> m_pRelativePointer = nullptr;
        std::unordered_set<uint32_t> m_uScancodesHeld;

        void Wayland_Registry_Global( wl_registry *pRegistry, uint32_t uName, const char *pInterface, uint32_t uVersion );
        static const wl_registry_listener s_RegistryListener;

        void Wayland_Seat_Capabilities( wl_seat *pSeat, uint32_t uCapabilities );
        void Wayland_Seat_Name( wl_seat *pSeat, const char *pName );
        static const wl_seat_listener s_SeatListener;

        void Wayland_Pointer_Enter( wl_pointer *pPointer, uint32_t uSerial, wl_surface *pSurface, wl_fixed_t fSurfaceX, wl_fixed_t fSurfaceY );
        void Wayland_Pointer_Leave( wl_pointer *pPointer, uint32_t uSerial, wl_surface *pSurface );
        void Wayland_Pointer_Motion( wl_pointer *pPointer, uint32_t uTime, wl_fixed_t fSurfaceX, wl_fixed_t fSurfaceY );
        void Wayland_Pointer_Button( wl_pointer *pPointer, uint32_t uSerial, uint32_t uTime, uint32_t uButton, uint32_t uState );
        void Wayland_Pointer_Axis( wl_pointer *pPointer, uint32_t uTime, uint32_t uAxis, wl_fixed_t fValue );
        void Wayland_Pointer_Axis_Source( wl_pointer *pPointer, uint32_t uAxisSource );
        void Wayland_Pointer_Axis_Stop( wl_pointer *pPointer, uint32_t uTime, uint32_t uAxis );
        void Wayland_Pointer_Axis_Discrete( wl_pointer *pPointer, uint32_t uAxis, int32_t nDiscrete );
        void Wayland_Pointer_Axis_Value120( wl_pointer *pPointer, uint32_t uAxis, int32_t nValue120 );
        void Wayland_Pointer_Frame( wl_pointer *pPointer );
        static const wl_pointer_listener s_PointerListener;

        void Wayland_Keyboard_Keymap( wl_keyboard *pKeyboard, uint32_t uFormat, int32_t nFd, uint32_t uSize );
        void Wayland_Keyboard_Enter( wl_keyboard *pKeyboard, uint32_t uSerial, wl_surface *pSurface, wl_array *pKeys );
        void Wayland_Keyboard_Leave( wl_keyboard *pKeyboard, uint32_t uSerial, wl_surface *pSurface );
        void Wayland_Keyboard_Key( wl_keyboard *pKeyboard, uint32_t uSerial, uint32_t uTime, uint32_t uKey, uint32_t uState );
        void Wayland_Keyboard_Modifiers( wl_keyboard *pKeyboard, uint32_t uSerial, uint32_t uModsDepressed, uint32_t uModsLatched, uint32_t uModsLocked, uint32_t uGroup );
        void Wayland_Keyboard_RepeatInfo( wl_keyboard *pKeyboard, int32_t nRate, int32_t nDelay );
        static const wl_keyboard_listener s_KeyboardListener;

	    void Wayland_RelativePointer_RelativeMotion( zwp_relative_pointer_v1 *pRelativePointer, uint32_t uTimeHi, uint32_t uTimeLo, wl_fixed_t fDx, wl_fixed_t fDy, wl_fixed_t fDxUnaccel, wl_fixed_t fDyUnaccel );
        static const zwp_relative_pointer_v1_listener s_RelativePointerListener;
    };
    const wl_registry_listener CWaylandInputThread::s_RegistryListener =
    {
        .global        = WAYLAND_USERDATA_TO_THIS( CWaylandInputThread, Wayland_Registry_Global ),
        .global_remove = WAYLAND_NULL(),
    };
    const wl_seat_listener CWaylandInputThread::s_SeatListener =
    {
        .capabilities = WAYLAND_USERDATA_TO_THIS( CWaylandInputThread, Wayland_Seat_Capabilities ),
        .name         = WAYLAND_USERDATA_TO_THIS( CWaylandInputThread, Wayland_Seat_Name ),
    };
    const wl_pointer_listener CWaylandInputThread::s_PointerListener =
    {
        .enter         = WAYLAND_USERDATA_TO_THIS( CWaylandInputThread, Wayland_Pointer_Enter ),
        .leave         = WAYLAND_USERDATA_TO_THIS( CWaylandInputThread, Wayland_Pointer_Leave ),
        .motion        = WAYLAND_USERDATA_TO_THIS( CWaylandInputThread, Wayland_Pointer_Motion ),
        .button        = WAYLAND_USERDATA_TO_THIS( CWaylandInputThread, Wayland_Pointer_Button ),
        .axis          = WAYLAND_USERDATA_TO_THIS( CWaylandInputThread, Wayland_Pointer_Axis ),
        .frame         = WAYLAND_USERDATA_TO_THIS( CWaylandInputThread, Wayland_Pointer_Frame ),
        .axis_source   = WAYLAND_USERDATA_TO_THIS( CWaylandInputThread, Wayland_Pointer_Axis_Source ),
        .axis_stop     = WAYLAND_USERDATA_TO_THIS( CWaylandInputThread, Wayland_Pointer_Axis_Stop ),
        .axis_discrete = WAYLAND_USERDATA_TO_THIS( CWaylandInputThread, Wayland_Pointer_Axis_Discrete ),
        .axis_value120 = WAYLAND_USERDATA_TO_THIS( CWaylandInputThread, Wayland_Pointer_Axis_Value120 ),
    };
    const wl_keyboard_listener CWaylandInputThread::s_KeyboardListener =
    {
        .keymap        = WAYLAND_USERDATA_TO_THIS( CWaylandInputThread, Wayland_Keyboard_Keymap ),
        .enter         = WAYLAND_USERDATA_TO_THIS( CWaylandInputThread, Wayland_Keyboard_Enter ),
        .leave         = WAYLAND_USERDATA_TO_THIS( CWaylandInputThread, Wayland_Keyboard_Leave ),
        .key           = WAYLAND_USERDATA_TO_THIS( CWaylandInputThread, Wayland_Keyboard_Key ),
        .modifiers     = WAYLAND_USERDATA_TO_THIS( CWaylandInputThread, Wayland_Keyboard_Modifiers ),
        .repeat_info   = WAYLAND_USERDATA_TO_THIS( CWaylandInputThread, Wayland_Keyboard_RepeatInfo ),
    };
    const zwp_relative_pointer_v1_listener CWaylandInputThread::s_RelativePointerListener =
    {
        .relative_motion = WAYLAND_USERDATA_TO_THIS( CWaylandInputThread, Wayland_RelativePointer_RelativeMotion ),
    };

    class CWaylandBackend : public CBaseBackend
    {
    public:
        CWaylandBackend();

        /////////////
        // IBackend
        /////////////

        virtual bool Init() override;
        virtual bool PostInit() override;
        virtual std::span<const char *const> GetInstanceExtensions() const override;
        virtual std::span<const char *const> GetDeviceExtensions( VkPhysicalDevice pVkPhysicalDevice ) const override;
        virtual VkImageLayout GetPresentLayout() const override;
        virtual void GetPreferredOutputFormat( uint32_t *pPrimaryPlaneFormat, uint32_t *pOverlayPlaneFormat ) const override;
        virtual bool ValidPhysicalDevice( VkPhysicalDevice pVkPhysicalDevice ) const override;

        virtual void DirtyState( bool bForce = false, bool bForceModeset = false ) override;
        virtual bool PollState() override;

        virtual std::shared_ptr<BackendBlob> CreateBackendBlob( const std::type_info &type, std::span<const uint8_t> data ) override;

        virtual OwningRc<IBackendFb> ImportDmabufToBackend( wlr_buffer *pBuffer, wlr_dmabuf_attributes *pDmaBuf ) override;
        virtual bool UsesModifiers() const override;
        virtual std::span<const uint64_t> GetSupportedModifiers( uint32_t uDrmFormat ) const override;

        virtual IBackendConnector *GetCurrentConnector() override;
        virtual IBackendConnector *GetConnector( GamescopeScreenType eScreenType ) override;

        virtual bool SupportsPlaneHardwareCursor() const override;

        virtual bool SupportsTearing() const override;
        virtual bool UsesVulkanSwapchain() const override;

        virtual bool IsSessionBased() const override;
        virtual bool SupportsExplicitSync() const override;

        virtual bool IsPaused() const override;
        virtual bool IsVisible() const override;

        virtual glm::uvec2 CursorSurfaceSize( glm::uvec2 uvecSize ) const override;
        virtual void HackUpdatePatchedEdid() override;

        virtual bool UsesVirtualConnectors() override;
        virtual std::shared_ptr<IBackendConnector> CreateVirtualConnector( uint64_t ulVirtualConnectorKey ) override;
    protected:
        virtual void OnBackendBlobDestroyed( BackendBlob *pBlob ) override;

        wl_surface *CursorInfoToSurface( const std::shared_ptr<INestedHints::CursorInfo> &info );

        bool SupportsColorManagement() const;

        void SetCursorImage( std::shared_ptr<INestedHints::CursorInfo> info );
        void SetRelativeMouseMode( wl_surface *pSurface, bool bRelative );
        void UpdateCursor();

        friend CWaylandConnector;
        friend CWaylandPlane;
        friend CWaylandInputThread;
        friend CWaylandFb;

        wl_display *GetDisplay() const { return m_pDisplay; }
        wl_shm *GetShm() const { return m_pShm; }
        wl_compositor *GetCompositor() const { return m_pCompositor; }
        wp_single_pixel_buffer_manager_v1 *GetSinglePixelBufferManager() const { return m_pSinglePixelBufferManager; }
        wl_subcompositor *GetSubcompositor() const { return m_pSubcompositor; }
        zwp_linux_dmabuf_v1 *GetLinuxDmabuf() const { return m_pLinuxDmabuf; }
        xdg_wm_base *GetXDGWMBase() const { return m_pXdgWmBase; }
        wp_viewporter *GetViewporter() const { return m_pViewporter; }
        wp_presentation *GetPresentation() const { return m_pPresentation; }
        frog_color_management_factory_v1 *GetFrogColorManagementFactory() const { return m_pFrogColorMgmtFactory; }
        wp_color_manager_v1 *GetWPColorManager() const { return m_pWPColorManager; }
        wp_image_description_v1 *GetWPImageDescription( GamescopeAppTextureColorspace eColorspace ) const { return m_pWPImageDescriptions[ (uint32_t)eColorspace ]; }
        wp_fractional_scale_manager_v1 *GetFractionalScaleManager() const { return m_pFractionalScaleManager; }
        xdg_toplevel_icon_manager_v1 *GetToplevelIconManager() const { return m_pToplevelIconManager; }
        libdecor *GetLibDecor() const { return m_pLibDecor; }

        void UpdateFullscreenState();

        WaylandOutputInfo *GetOutputInfo( wl_output *pOutput )
        {
            auto iter = m_pOutputs.find( pOutput );
            if ( iter == m_pOutputs.end() )
                return nullptr;

            return &iter->second;
        }

        wl_region *GetFullRegion() const { return m_pFullRegion; }
        CWaylandFb *GetBlackFb() const { return m_BlackFb.get(); }

        void OnConnectorDestroyed( CWaylandConnector *pConnector )
        {
            m_pFocusConnector.compare_exchange_strong( pConnector, nullptr );
        }

    private:

        void Wayland_Registry_Global( wl_registry *pRegistry, uint32_t uName, const char *pInterface, uint32_t uVersion );
        static const wl_registry_listener s_RegistryListener;

        void Wayland_Modifier( zwp_linux_dmabuf_v1 *pDmabuf, uint32_t uFormat, uint32_t uModifierHi, uint32_t uModifierLo );

        void Wayland_Output_Geometry( wl_output *pOutput, int32_t nX, int32_t nY, int32_t nPhysicalWidth, int32_t nPhysicalHeight, int32_t nSubpixel, const char *pMake, const char *pModel, int32_t nTransform );
        void Wayland_Output_Mode( wl_output *pOutput, uint32_t uFlags, int32_t nWidth, int32_t nHeight, int32_t nRefresh );
        void Wayland_Output_Done( wl_output *pOutput );
        void Wayland_Output_Scale( wl_output *pOutput, int32_t nFactor );
        void Wayland_Output_Name( wl_output *pOutput, const char *pName );
        void Wayland_Output_Description( wl_output *pOutput, const char *pDescription );
        static const wl_output_listener s_OutputListener;

        void Wayland_Seat_Capabilities( wl_seat *pSeat, uint32_t uCapabilities );
        static const wl_seat_listener s_SeatListener;

        void Wayland_Pointer_Enter( wl_pointer *pPointer, uint32_t uSerial, wl_surface *pSurface, wl_fixed_t fSurfaceX, wl_fixed_t fSurfaceY );
        void Wayland_Pointer_Leave( wl_pointer *pPointer, uint32_t uSerial, wl_surface *pSurface );
        static const wl_pointer_listener s_PointerListener;

        void Wayland_Keyboard_Enter( wl_keyboard *pKeyboard, uint32_t uSerial, wl_surface *pSurface, wl_array *pKeys );
        void Wayland_Keyboard_Leave( wl_keyboard *pKeyboard, uint32_t uSerial, wl_surface *pSurface );
        static const wl_keyboard_listener s_KeyboardListener;

        void Wayland_WPColorManager_SupportedIntent( wp_color_manager_v1 *pWPColorManager, uint32_t uRenderIntent );
        void Wayland_WPColorManager_SupportedFeature( wp_color_manager_v1 *pWPColorManager, uint32_t uFeature );
        void Wayland_WPColorManager_SupportedTFNamed( wp_color_manager_v1 *pWPColorManager, uint32_t uTF );
        void Wayland_WPColorManager_SupportedPrimariesNamed( wp_color_manager_v1 *pWPColorManager, uint32_t uPrimaries );
        void Wayland_WPColorManager_ColorManagerDone( wp_color_manager_v1 *pWPColorManager );
        static const wp_color_manager_v1_listener s_WPColorManagerListener;

        void Wayland_DataSource_Send( struct wl_data_source *pSource, const char *pMime, int nFd );
        void Wayland_DataSource_Cancelled( struct wl_data_source *pSource );
        static const wl_data_source_listener s_DataSourceListener;

        void Wayland_PrimarySelectionSource_Send( struct zwp_primary_selection_source_v1 *pSource, const char *pMime, int nFd );
        void Wayland_PrimarySelectionSource_Cancelled( struct zwp_primary_selection_source_v1 *pSource );
        static const zwp_primary_selection_source_v1_listener s_PrimarySelectionSourceListener;

        CWaylandInputThread m_InputThread;

        wl_display *m_pDisplay = nullptr;
        wl_shm *m_pShm = nullptr;
        wl_compositor *m_pCompositor = nullptr;
        wp_single_pixel_buffer_manager_v1 *m_pSinglePixelBufferManager = nullptr;
        wl_subcompositor *m_pSubcompositor = nullptr;
        zwp_linux_dmabuf_v1 *m_pLinuxDmabuf = nullptr;
        xdg_wm_base *m_pXdgWmBase = nullptr;
        wp_viewporter *m_pViewporter = nullptr;
        wl_region *m_pFullRegion = nullptr;
        Rc<CWaylandFb> m_BlackFb;
        OwningRc<CWaylandFb> m_pOwnedBlackFb;
        OwningRc<CVulkanTexture> m_pBlackTexture;
        wp_presentation *m_pPresentation = nullptr;
        frog_color_management_factory_v1 *m_pFrogColorMgmtFactory = nullptr;
        wp_color_manager_v1 *m_pWPColorManager = nullptr;
        wp_image_description_v1 *m_pWPImageDescriptions[ GamescopeAppTextureColorspace_Count ]{};
        zwp_pointer_constraints_v1 *m_pPointerConstraints = nullptr;
        zwp_relative_pointer_manager_v1 *m_pRelativePointerManager = nullptr;
        wp_fractional_scale_manager_v1 *m_pFractionalScaleManager = nullptr;
        xdg_toplevel_icon_manager_v1 *m_pToplevelIconManager = nullptr;

        // TODO: Restructure and remove the need for this.
        std::atomic<CWaylandConnector *> m_pFocusConnector;

        wl_data_device_manager *m_pDataDeviceManager = nullptr;
        wl_data_device *m_pDataDevice = nullptr;
        std::shared_ptr<std::string> m_pClipboard = nullptr;

        zwp_primary_selection_device_manager_v1 *m_pPrimarySelectionDeviceManager = nullptr;
        zwp_primary_selection_device_v1 *m_pPrimarySelectionDevice = nullptr;
        std::shared_ptr<std::string> m_pPrimarySelection = nullptr;

        struct
        {
            std::vector<wp_color_manager_v1_primaries> ePrimaries;
            std::vector<wp_color_manager_v1_transfer_function> eTransferFunctions;
            std::vector<wp_color_manager_v1_render_intent> eRenderIntents;
            std::vector<wp_color_manager_v1_feature> eFeatures;

            bool bSupportsGamescopeColorManagement = false; // Has everything we want and need?
        } m_WPColorManagerFeatures;

        std::unordered_map<wl_output *, WaylandOutputInfo> m_pOutputs;

        libdecor *m_pLibDecor = nullptr;

        wl_seat *m_pSeat = nullptr;
        wl_keyboard *m_pKeyboard = nullptr;
        wl_pointer *m_pPointer = nullptr;
        wl_touch *m_pTouch = nullptr;
        zwp_locked_pointer_v1 *m_pLockedPointer = nullptr;
        wl_surface *m_pLockedSurface = nullptr;
        zwp_relative_pointer_v1 *m_pRelativePointer = nullptr;

        bool m_bCanUseModifiers = false;
        std::unordered_map<uint32_t, std::vector<uint64_t>> m_FormatModifiers;
        std::unordered_map<uint32_t, wl_buffer *> m_ImportedFbs;

        uint32_t m_uPointerEnterSerial = 0;
        bool m_bMouseEntered = false;
        uint32_t m_uKeyboardEnterSerial = 0;
        bool m_bKeyboardEntered = false;

        std::shared_ptr<INestedHints::CursorInfo> m_pCursorInfo;
        wl_surface *m_pCursorSurface = nullptr;
        std::shared_ptr<INestedHints::CursorInfo> m_pDefaultCursorInfo;
        wl_surface *m_pDefaultCursorSurface = nullptr;
    };
    const wl_registry_listener CWaylandBackend::s_RegistryListener =
    {
        .global        = WAYLAND_USERDATA_TO_THIS( CWaylandBackend, Wayland_Registry_Global ),
        .global_remove = WAYLAND_NULL(),
    };
    const wl_output_listener CWaylandBackend::s_OutputListener =
    {
        .geometry    = WAYLAND_USERDATA_TO_THIS( CWaylandBackend, Wayland_Output_Geometry ),
        .mode        = WAYLAND_USERDATA_TO_THIS( CWaylandBackend, Wayland_Output_Mode ),
        .done        = WAYLAND_USERDATA_TO_THIS( CWaylandBackend, Wayland_Output_Done ),
        .scale       = WAYLAND_USERDATA_TO_THIS( CWaylandBackend, Wayland_Output_Scale ),
        .name        = WAYLAND_USERDATA_TO_THIS( CWaylandBackend, Wayland_Output_Name ),
        .description = WAYLAND_USERDATA_TO_THIS( CWaylandBackend, Wayland_Output_Description ),
    };
    const wl_seat_listener CWaylandBackend::s_SeatListener =
    {
        .capabilities = WAYLAND_USERDATA_TO_THIS( CWaylandBackend, Wayland_Seat_Capabilities ),
        .name         = WAYLAND_NULL(),
    };
    const wl_pointer_listener CWaylandBackend::s_PointerListener =
    {
        .enter         = WAYLAND_USERDATA_TO_THIS( CWaylandBackend, Wayland_Pointer_Enter ),
        .leave         = WAYLAND_USERDATA_TO_THIS( CWaylandBackend, Wayland_Pointer_Leave ),
        .motion        = WAYLAND_NULL(),
        .button        = WAYLAND_NULL(),
        .axis          = WAYLAND_NULL(),
        .frame         = WAYLAND_NULL(),
        .axis_source   = WAYLAND_NULL(),
        .axis_stop     = WAYLAND_NULL(),
        .axis_discrete = WAYLAND_NULL(),
        .axis_value120 = WAYLAND_NULL(),
    };
    const wl_keyboard_listener CWaylandBackend::s_KeyboardListener =
    {
        .keymap        = WAYLAND_NULL(),
        .enter         = WAYLAND_USERDATA_TO_THIS( CWaylandBackend, Wayland_Keyboard_Enter ),
        .leave         = WAYLAND_USERDATA_TO_THIS( CWaylandBackend, Wayland_Keyboard_Leave ),
        .key           = WAYLAND_NULL(),
        .modifiers     = WAYLAND_NULL(),
        .repeat_info   = WAYLAND_NULL(),
    };

    const wp_color_manager_v1_listener CWaylandBackend::s_WPColorManagerListener
    {
        .supported_intent          = WAYLAND_USERDATA_TO_THIS( CWaylandBackend, Wayland_WPColorManager_SupportedIntent ),
        .supported_feature         = WAYLAND_USERDATA_TO_THIS( CWaylandBackend, Wayland_WPColorManager_SupportedFeature ),
        .supported_tf_named        = WAYLAND_USERDATA_TO_THIS( CWaylandBackend, Wayland_WPColorManager_SupportedTFNamed ),
        .supported_primaries_named = WAYLAND_USERDATA_TO_THIS( CWaylandBackend, Wayland_WPColorManager_SupportedPrimariesNamed ),
        .done        = WAYLAND_USERDATA_TO_THIS( CWaylandBackend, Wayland_WPColorManager_ColorManagerDone ),
    };
    const wl_data_source_listener CWaylandBackend::s_DataSourceListener =
    {
        .target             = WAYLAND_NULL(),
        .send               = WAYLAND_USERDATA_TO_THIS( CWaylandBackend, Wayland_DataSource_Send ),
        .cancelled          = WAYLAND_USERDATA_TO_THIS( CWaylandBackend, Wayland_DataSource_Cancelled ),
        .dnd_drop_performed = WAYLAND_NULL(),
        .dnd_finished       = WAYLAND_NULL(),
        .action             = WAYLAND_NULL(),
    };
    const zwp_primary_selection_source_v1_listener CWaylandBackend::s_PrimarySelectionSourceListener =
    {
        .send      = WAYLAND_USERDATA_TO_THIS( CWaylandBackend, Wayland_PrimarySelectionSource_Send ),
        .cancelled = WAYLAND_USERDATA_TO_THIS( CWaylandBackend, Wayland_PrimarySelectionSource_Cancelled ),
    };

    //////////////////
    // CWaylandFb
    //////////////////

    CWaylandFb::CWaylandFb( CWaylandBackend *pBackend, wl_buffer *pHostBuffer )
        : CBaseBackendFb()
        , m_pBackend     { pBackend }
        , m_pHostBuffer  { pHostBuffer }
    {
        wl_buffer_add_listener( pHostBuffer, &s_BufferListener, this );
    }

    CWaylandFb::~CWaylandFb()
    {
        // I own the pHostBuffer.
        wl_buffer_destroy( m_pHostBuffer );
        m_pHostBuffer = nullptr;
    }

    void CWaylandFb::OnCompositorAcquire()
    {
        // If the compositor has acquired us, track that
        // and increment the ref count.
        if ( !m_bCompositorAcquired )
        {
            m_bCompositorAcquired = true;
            IncRef();
        }
    }

    void CWaylandFb::OnCompositorRelease()
    {
        // Compositor has released us, decrement rc.
        //assert( m_bCompositorAcquired );

        if ( m_bCompositorAcquired )
        {
            m_bCompositorAcquired = false;
            DecRef();
        }
        else
        {
            xdg_log.errorf( "Compositor released us but we were not acquired. Oh no." );
        }
    }

    void CWaylandFb::Wayland_Buffer_Release( wl_buffer *pBuffer )
    {
        assert( m_pHostBuffer );
        assert( m_pHostBuffer == pBuffer );

        xdg_log.debugf( "buffer_release: %p", pBuffer );

        OnCompositorRelease();
    }

    //////////////////
    // CWaylandConnector
    //////////////////

    CWaylandConnector::CWaylandConnector( CWaylandBackend *pBackend, uint64_t ulVirtualConnectorKey )
        : CBaseBackendConnector{ ulVirtualConnectorKey }
        , m_pBackend( pBackend )
        , m_Planes{ this, this, this, this, this, this, this, this }
    {
        m_HDRInfo.bAlwaysPatchEdid = true;
    }

    CWaylandConnector::~CWaylandConnector()
    {
        m_pBackend->OnConnectorDestroyed( this );
    }

    bool CWaylandConnector::UpdateEdid()
    {
        m_FakeEdid = GenerateSimpleEdid( g_nNestedWidth, g_nNestedHeight );

        return true;
    }

    bool CWaylandConnector::Init()
    {
        for ( uint32_t i = 0; i < 8; i++ )
        {
            bool bSuccess = m_Planes[i].Init( i == 0 ? nullptr : &m_Planes[0], i == 0 ? nullptr : &m_Planes[ i - 1 ] );
            if ( !bSuccess )
                return false;
        }

        if ( g_bFullscreen )
        {
            m_bDesiredFullscreenState = true;
            g_bFullscreen = false;
            UpdateFullscreenState();
        }

        UpdateEdid();
        m_pBackend->HackUpdatePatchedEdid();

        if ( g_bForceRelativeMouse )
            this->SetRelativeMouseMode( true );

        return true;
    }

    void CWaylandConnector::SetFullscreen( bool bFullscreen )
    {
        m_bDesiredFullscreenState = bFullscreen;
    }

    void CWaylandConnector::UpdateFullscreenState()
    {
        if ( !m_bVisible )
            g_bFullscreen = false;

        if ( m_bDesiredFullscreenState != g_bFullscreen && m_bVisible )
        {
            if ( m_bDesiredFullscreenState )
                libdecor_frame_set_fullscreen( m_Planes[0].GetFrame(), nullptr );
            else
                libdecor_frame_unset_fullscreen( m_Planes[0].GetFrame() );

            g_bFullscreen = m_bDesiredFullscreenState;
        }
    }

    int CWaylandConnector::Present( const FrameInfo_t *pFrameInfo, bool bAsync )
    {
        UpdateFullscreenState();

        bool bNeedsFullComposite = false;

        if ( !m_bVisible )
        {
            uint32_t uCurrentPlane = 0;
            for ( int i = 0; i < 8 && uCurrentPlane < 8; i++ )
                m_Planes[uCurrentPlane++].Present( nullptr );
        }
        else
        {
            // TODO: Dedupe some of this composite check code between us and drm.cpp
            bool bLayer0ScreenSize = close_enough(pFrameInfo->layers[0].scale.x, 1.0f) && close_enough(pFrameInfo->layers[0].scale.y, 1.0f);

            bool bNeedsCompositeFromFilter = (g_upscaleFilter == GamescopeUpscaleFilter::NEAREST || g_upscaleFilter == GamescopeUpscaleFilter::PIXEL) && !bLayer0ScreenSize;

            bNeedsFullComposite |= cv_composite_force;
            bNeedsFullComposite |= pFrameInfo->useFSRLayer0;
            bNeedsFullComposite |= pFrameInfo->useNISLayer0;
            bNeedsFullComposite |= pFrameInfo->blurLayer0;
            bNeedsFullComposite |= bNeedsCompositeFromFilter;
            bNeedsFullComposite |= g_bColorSliderInUse;
            bNeedsFullComposite |= pFrameInfo->bFadingOut;
            bNeedsFullComposite |= !g_reshade_effect.empty();

            if ( g_bOutputHDREnabled )
                bNeedsFullComposite |= g_bHDRItmEnable;

            if ( !m_pBackend->SupportsColorManagement() )
                bNeedsFullComposite |= ColorspaceIsHDR( pFrameInfo->layers[0].colorspace );

            bNeedsFullComposite |= !!(g_uCompositeDebug & CompositeDebugFlag::Heatmap);

            if ( !bNeedsFullComposite )
            {
                bool bNeedsBacking = true;
                if ( pFrameInfo->layerCount >= 1 )
                {
                    if ( pFrameInfo->layers[0].isScreenSize() && !pFrameInfo->layers[0].hasAlpha() )
                        bNeedsBacking = false;
                }

                uint32_t uCurrentPlane = 0;
                if ( bNeedsBacking )
                {
                    m_pBackend->GetBlackFb()->OnCompositorAcquire();

                    CWaylandPlane *pPlane = &m_Planes[uCurrentPlane++];
                    pPlane->Present(
                        WaylandPlaneState
                        {
                            .pBuffer     = m_pBackend->GetBlackFb()->GetHostBuffer(),
                            .flSrcWidth  = 1.0,
                            .flSrcHeight = 1.0,
                            .nDstWidth   = int32_t( g_nOutputWidth ),
                            .nDstHeight  = int32_t( g_nOutputHeight ),
                            .eColorspace = GAMESCOPE_APP_TEXTURE_COLORSPACE_PASSTHRU,
                            .bOpaque     = true,
                            .uFractionalScale = pPlane->GetScale(),
                        } );
                }

                for ( int i = 0; i < 8 && uCurrentPlane < 8; i++ )
                    m_Planes[uCurrentPlane++].Present( i < pFrameInfo->layerCount ? &pFrameInfo->layers[i] : nullptr );
            }
            else
            {
                std::optional oCompositeResult = vulkan_composite( (FrameInfo_t *)pFrameInfo, nullptr, false );

                if ( !oCompositeResult )
                {
                    xdg_log.errorf( "vulkan_composite failed" );
                    return -EINVAL;
                }

                vulkan_wait( *oCompositeResult, true );

                FrameInfo_t::Layer_t compositeLayer{};
                compositeLayer.scale.x = 1.0;
                compositeLayer.scale.y = 1.0;
                compositeLayer.opacity = 1.0;
                compositeLayer.zpos = g_zposBase;

                compositeLayer.tex = vulkan_get_last_output_image( false, false );
                compositeLayer.applyColorMgmt = false;

                compositeLayer.filter = GamescopeUpscaleFilter::NEAREST;
                compositeLayer.ctm = nullptr;
                compositeLayer.colorspace = pFrameInfo->outputEncodingEOTF == EOTF_PQ ? GAMESCOPE_APP_TEXTURE_COLORSPACE_HDR10_PQ : GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB;

                m_Planes[0].Present( &compositeLayer );

                for ( int i = 1; i < 8; i++ )
                    m_Planes[i].Present( nullptr );
            }
        }

        for ( int i = 7; i >= 0; i-- )
            m_Planes[i].Commit();

        wl_display_flush( m_pBackend->GetDisplay() );

        GetVBlankTimer().UpdateWasCompositing( bNeedsFullComposite );
        GetVBlankTimer().UpdateLastDrawTime( get_time_in_nanos() - g_SteamCompMgrVBlankTime.ulWakeupTime );

        m_pBackend->PollState();

        return 0;
    }

    GamescopeScreenType CWaylandConnector::GetScreenType() const
    {
        return gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL;
    }
    GamescopePanelOrientation CWaylandConnector::GetCurrentOrientation() const
    {
        return GAMESCOPE_PANEL_ORIENTATION_0;
    }
    bool CWaylandConnector::SupportsHDR() const
    {
        return GetHDRInfo().IsHDR10();
    }
    bool CWaylandConnector::IsHDRActive() const
    {
        // XXX: blah
        return false;
    }
    const BackendConnectorHDRInfo &CWaylandConnector::GetHDRInfo() const
    {
        return m_HDRInfo;
    }
    bool CWaylandConnector::IsVRRActive() const
    {
        return cv_adaptive_sync && m_bHostCompositorIsCurrentlyVRR;
    }
    std::span<const BackendMode> CWaylandConnector::GetModes() const
    {
        return std::span<const BackendMode>{};
    }

    bool CWaylandConnector::SupportsVRR() const
    {
        return CurrentDisplaySupportsVRR();
    }

    std::span<const uint8_t> CWaylandConnector::GetRawEDID() const
    {
        return std::span<const uint8_t>{ m_FakeEdid.begin(), m_FakeEdid.end() };
    }
    std::span<const uint32_t> CWaylandConnector::GetValidDynamicRefreshRates() const
    {
        return std::span<const uint32_t>{};
    }

    void CWaylandConnector::GetNativeColorimetry(
        bool bHDR10,
        displaycolorimetry_t *displayColorimetry, EOTF *displayEOTF,
        displaycolorimetry_t *outputEncodingColorimetry, EOTF *outputEncodingEOTF ) const
    {
        *displayColorimetry = m_DisplayColorimetry;
        *displayEOTF = EOTF_Gamma22;

        if ( bHDR10 && GetHDRInfo().IsHDR10() )
        {
            // For HDR10 output, expected content colorspace != native colorspace.
            *outputEncodingColorimetry = displaycolorimetry_2020;
            *outputEncodingEOTF = GetHDRInfo().eOutputEncodingEOTF;
        }
        else
        {
            // We always use default 'perceptual' intent, so
            // this should be correct for SDR content.
            *outputEncodingColorimetry = m_DisplayColorimetry;
            *outputEncodingEOTF = EOTF_Gamma22;
        }
    }


    void CWaylandConnector::SetCursorImage( std::shared_ptr<INestedHints::CursorInfo> info )
    {
        m_pBackend->SetCursorImage( std::move( info ) );
    }
    void CWaylandConnector::SetRelativeMouseMode( bool bRelative )
    {
        // TODO: Do more tracking across multiple connectors, and activity here if we ever want to use this.
        m_pBackend->SetRelativeMouseMode( m_Planes[0].GetSurface(), bRelative );
    }
    void CWaylandConnector::SetVisible( bool bVisible )
    {
        if ( m_bVisible == bVisible )
            return;

        m_bVisible = bVisible;
        force_repaint();
    }
    void CWaylandConnector::SetTitle( std::shared_ptr<std::string> pAppTitle )
    {
        std::string szTitle = pAppTitle ? *pAppTitle : "gamescope";
        if ( g_bGrabbed )
            szTitle += " (grabbed)";
        libdecor_frame_set_title( m_Planes[0].GetFrame(), szTitle.c_str() );
    }
    void CWaylandConnector::SetIcon( std::shared_ptr<std::vector<uint32_t>> uIconPixels )
    {
        if ( !m_pBackend->GetToplevelIconManager() )
            return;

        if ( uIconPixels && uIconPixels->size() >= 3 )
        {
            xdg_toplevel_icon_v1 *pIcon = xdg_toplevel_icon_manager_v1_create_icon( m_pBackend->GetToplevelIconManager() );
            if ( !pIcon )
            {
                xdg_log.errorf( "Failed to create xdg_toplevel_icon_v1" );
                return;
            }
            defer( xdg_toplevel_icon_v1_destroy( pIcon ) );

            const uint32_t uWidth  = ( *uIconPixels )[0];
            const uint32_t uHeight = ( *uIconPixels )[1];

            const uint32_t uStride = uWidth * 4;
            const uint32_t uSize   = uStride * uHeight;
            int32_t nFd = CreateShmBuffer( uSize, &( *uIconPixels )[2] );
            if ( nFd < 0 )
            {
                xdg_log.errorf( "Failed to create/map shm buffer" );
                return;
            }
            defer( close( nFd ) );

            wl_shm_pool *pPool = wl_shm_create_pool( m_pBackend->GetShm(), nFd, uSize );
            defer( wl_shm_pool_destroy( pPool ) );

            wl_buffer *pBuffer = wl_shm_pool_create_buffer( pPool, 0, uWidth, uHeight, uStride, WL_SHM_FORMAT_ARGB8888 );
            defer( wl_buffer_destroy( pBuffer ) );

            xdg_toplevel_icon_v1_add_buffer( pIcon, pBuffer, 1 );

            xdg_toplevel_icon_manager_v1_set_icon( m_pBackend->GetToplevelIconManager(), m_Planes[0].GetXdgToplevel(), pIcon );
        }
        else
        {
            xdg_toplevel_icon_manager_v1_set_icon( m_pBackend->GetToplevelIconManager(), m_Planes[0].GetXdgToplevel(), nullptr );
        }
    }

    void CWaylandConnector::SetSelection( std::shared_ptr<std::string> szContents, GamescopeSelection eSelection )
    {
        if ( m_pBackend->m_pDataDeviceManager && !m_pBackend->m_pDataDevice )
            m_pBackend->m_pDataDevice = wl_data_device_manager_get_data_device( m_pBackend->m_pDataDeviceManager, m_pBackend->m_pSeat );

        if ( m_pBackend->m_pPrimarySelectionDeviceManager && !m_pBackend->m_pPrimarySelectionDevice )
            m_pBackend->m_pPrimarySelectionDevice = zwp_primary_selection_device_manager_v1_get_device( m_pBackend->m_pPrimarySelectionDeviceManager, m_pBackend->m_pSeat );

        if ( eSelection == GAMESCOPE_SELECTION_CLIPBOARD && m_pBackend->m_pDataDevice )
        {
            m_pBackend->m_pClipboard = szContents;
            wl_data_source *source = wl_data_device_manager_create_data_source( m_pBackend->m_pDataDeviceManager );
            wl_data_source_add_listener( source, &m_pBackend->s_DataSourceListener, m_pBackend );
            wl_data_source_offer( source, "text/plain" );
            wl_data_source_offer( source, "text/plain;charset=utf-8" );
            wl_data_source_offer( source, "TEXT" );
            wl_data_source_offer( source, "STRING" );
            wl_data_source_offer( source, "UTF8_STRING" );
            wl_data_device_set_selection( m_pBackend->m_pDataDevice, source, m_pBackend->m_uKeyboardEnterSerial );
        }
        else if ( eSelection == GAMESCOPE_SELECTION_PRIMARY && m_pBackend->m_pPrimarySelectionDevice )
        {
            m_pBackend->m_pPrimarySelection = szContents;
            zwp_primary_selection_source_v1 *source = zwp_primary_selection_device_manager_v1_create_source( m_pBackend->m_pPrimarySelectionDeviceManager );
            zwp_primary_selection_source_v1_add_listener( source, &m_pBackend->s_PrimarySelectionSourceListener, m_pBackend );
            zwp_primary_selection_source_v1_offer( source, "text/plain" );
            zwp_primary_selection_source_v1_offer( source, "text/plain;charset=utf-8" );
            zwp_primary_selection_source_v1_offer( source, "TEXT" );
            zwp_primary_selection_source_v1_offer( source, "STRING" );
            zwp_primary_selection_source_v1_offer( source, "UTF8_STRING" );
            zwp_primary_selection_device_v1_set_selection( m_pBackend->m_pPrimarySelectionDevice, source, m_pBackend->m_uPointerEnterSerial );
        }
    }

    //////////////////
    // CWaylandPlane
    //////////////////

    CWaylandPlane::CWaylandPlane( CWaylandConnector *pConnector )
        : m_pConnector{ pConnector }
        , m_pBackend{ pConnector->GetBackend() }
    {
    }

    CWaylandPlane::~CWaylandPlane()
    {
        std::scoped_lock lock{ m_PlaneStateLock };

        m_eWindowState = LIBDECOR_WINDOW_STATE_NONE;
        m_pOutputs.clear();
        m_bNeedsDecorCommit = false;

        m_oCurrentPlaneState = std::nullopt;

        if ( m_pFrame )
            libdecor_frame_unref( m_pFrame ); // Ew.

        if ( m_pSubsurface )
            wl_subsurface_destroy( m_pSubsurface );
        if ( m_pFractionalScale )
            wp_fractional_scale_v1_destroy( m_pFractionalScale );
        if ( m_pWPColorManagedSurface )
            wp_color_management_surface_v1_destroy( m_pWPColorManagedSurface );
        if ( m_pWPColorManagedSurfaceFeedback )
            wp_color_management_surface_feedback_v1_destroy( m_pWPColorManagedSurfaceFeedback );
        if ( m_pFrogColorManagedSurface )
            frog_color_managed_surface_destroy( m_pFrogColorManagedSurface );
        if ( m_pViewport )
            wp_viewport_destroy( m_pViewport );
        if ( m_pSurface )
            wl_surface_destroy( m_pSurface );
    }

    bool CWaylandPlane::Init( CWaylandPlane *pParent, CWaylandPlane *pSiblingBelow )
    {
        m_pParent = pParent;
        m_pSurface = wl_compositor_create_surface( m_pBackend->GetCompositor() );
        wl_proxy_set_tag( (wl_proxy *)m_pSurface, &GAMESCOPE_plane_tag );
        wl_surface_set_user_data( m_pSurface, this );
        wl_surface_add_listener( m_pSurface, &s_SurfaceListener, this );

        m_pViewport = wp_viewporter_get_viewport( m_pBackend->GetViewporter(), m_pSurface );

        if ( m_pBackend->GetWPColorManager() )
        {
            m_pWPColorManagedSurface = wp_color_manager_v1_get_surface( m_pBackend->GetWPColorManager(), m_pSurface );
            m_pWPColorManagedSurfaceFeedback = wp_color_manager_v1_get_surface_feedback( m_pBackend->GetWPColorManager(), m_pSurface );

            // Only add the listener for the toplevel to avoid useless spam.
            if ( !pParent )
                wp_color_management_surface_feedback_v1_add_listener( m_pWPColorManagedSurfaceFeedback, &s_WPColorManagementSurfaceListener, this );

            UpdateWPPreferredColorManagement();
        }
        else if ( m_pBackend->GetFrogColorManagementFactory() )
        {
            m_pFrogColorManagedSurface = frog_color_management_factory_v1_get_color_managed_surface( m_pBackend->GetFrogColorManagementFactory(), m_pSurface );

            // Only add the listener for the toplevel to avoid useless spam.
            if ( !pParent )
                frog_color_managed_surface_add_listener( m_pFrogColorManagedSurface, &s_FrogColorManagedSurfaceListener, this );
        }

        if ( m_pBackend->GetFractionalScaleManager() )
        {
            m_pFractionalScale = wp_fractional_scale_manager_v1_get_fractional_scale( m_pBackend->GetFractionalScaleManager(), m_pSurface );

            if ( !pParent )
                wp_fractional_scale_v1_add_listener( m_pFractionalScale, &s_FractionalScaleListener, this );
        }

        if ( !pParent )
        {
            m_pFrame = libdecor_decorate( m_pBackend->GetLibDecor(), m_pSurface, &s_LibDecorFrameInterface, this );
            libdecor_frame_set_title( m_pFrame, "Gamescope" );
            libdecor_frame_set_app_id( m_pFrame, "gamescope" );
            libdecor_frame_map( m_pFrame );
        }
        else
        {
            m_pSubsurface = wl_subcompositor_get_subsurface( m_pBackend->GetSubcompositor(), m_pSurface, pParent->GetSurface() );
            wl_subsurface_place_above( m_pSubsurface, pSiblingBelow->GetSurface() );
            wl_subsurface_set_sync( m_pSubsurface );
        }

        wl_surface_commit( m_pSurface );
        wl_display_roundtrip( m_pBackend->GetDisplay() );

        if ( m_pFrame )
            libdecor_frame_set_visibility( m_pFrame, !g_bBorderlessOutputWindow );

        return true;
    }

    uint32_t CWaylandPlane::GetScale() const
    {
        if ( m_pParent )
            return m_pParent->GetScale();

        return m_uFractionalScale;
    }

    void CWaylandPlane::Present( std::optional<WaylandPlaneState> oState )
    {
        {
            std::unique_lock lock( m_PlaneStateLock );
            m_oCurrentPlaneState = oState;
        }

        if ( oState )
        {
            assert( oState->pBuffer );

            if ( m_pFrame )
            {
                struct wp_presentation_feedback *pFeedback = wp_presentation_feedback( m_pBackend->GetPresentation(), m_pSurface );
                wp_presentation_feedback_add_listener( pFeedback, &s_PresentationFeedbackListener, this );
            }

            if ( m_pWPColorManagedSurface )
            {
                WaylandPlaneColorState colorState =
                {
                    .eColorspace  = oState->eColorspace,
                    .pHDRMetadata = oState->pHDRMetadata,
                };

                if ( !m_ColorState || *m_ColorState != colorState )
                {
                    m_ColorState = colorState;

                    if ( m_pCurrentImageDescription )
                    {
                        wp_image_description_v1_destroy( m_pCurrentImageDescription );
                        m_pCurrentImageDescription = nullptr;
                    }

                    if ( oState->eColorspace == GAMESCOPE_APP_TEXTURE_COLORSPACE_SCRGB )
                    {
                        m_pCurrentImageDescription = wp_color_manager_v1_create_windows_scrgb( m_pBackend->GetWPColorManager() );
                    }
                    else if ( oState->eColorspace == GAMESCOPE_APP_TEXTURE_COLORSPACE_HDR10_PQ )
                    {
                        wp_image_description_creator_params_v1 *pParams = wp_color_manager_v1_create_parametric_creator( m_pBackend->GetWPColorManager() );

                        double flScale = cv_wayland_hdr10_saturation_scale;
                        if ( close_enough( flScale, 1.0f ) )
                        {
                            wp_image_description_creator_params_v1_set_primaries_named( pParams, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020 );
                        }
                        else
                        {
                            wp_image_description_creator_params_v1_set_primaries( pParams,
                                (int32_t)(0.708 * flScale * 1'000'000.0),
                                (int32_t)(0.292 / flScale * 1'000'000.0),
                                (int32_t)(0.170 / flScale * 1'000'000.0),
                                (int32_t)(0.797 * flScale * 1'000'000.0),
                                (int32_t)(0.131 / flScale * 1'000'000.0),
                                (int32_t)(0.046 / flScale * 1'000'000.0),
                                (int32_t)(0.3127 * 1'000'000.0),
                                (int32_t)(0.3290 * 1'000'000.0) );
                        }
                        wp_image_description_creator_params_v1_set_tf_named( pParams, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ );
                        if ( m_ColorState->pHDRMetadata )
                        {
                            const hdr_metadata_infoframe *pInfoframe = &m_ColorState->pHDRMetadata->View<hdr_output_metadata>().hdmi_metadata_type1;

                            wp_image_description_creator_params_v1_set_mastering_display_primaries( pParams,
                                // Rescale...
                                (((int32_t)pInfoframe->display_primaries[0].x) * 1'000'000) / 0xC350,
                                (((int32_t)pInfoframe->display_primaries[0].y) * 1'000'000) / 0xC350,
                                (((int32_t)pInfoframe->display_primaries[1].x) * 1'000'000) / 0xC350,
                                (((int32_t)pInfoframe->display_primaries[1].y) * 1'000'000) / 0xC350,
                                (((int32_t)pInfoframe->display_primaries[2].x) * 1'000'000) / 0xC350,
                                (((int32_t)pInfoframe->display_primaries[2].y) * 1'000'000) / 0xC350,
                                (((int32_t)pInfoframe->white_point.x) * 1'000'000) / 0xC350,
                                (((int32_t)pInfoframe->white_point.y) * 1'000'000) / 0xC350);

                            wp_image_description_creator_params_v1_set_mastering_luminance( pParams,
                                pInfoframe->min_display_mastering_luminance,
                                pInfoframe->max_display_mastering_luminance );

                            wp_image_description_creator_params_v1_set_max_cll( pParams,
                                pInfoframe->max_cll );

                            wp_image_description_creator_params_v1_set_max_fall( pParams,
                                pInfoframe->max_fall );
                        }
                        m_pCurrentImageDescription = wp_image_description_creator_params_v1_create( pParams );
                    }
                }

                if ( m_pCurrentImageDescription )
                {
                    wp_color_management_surface_v1_set_image_description( m_pWPColorManagedSurface, m_pCurrentImageDescription, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL );
                }
                else
                {
                    wp_color_management_surface_v1_unset_image_description( m_pWPColorManagedSurface );
                }
            }
            else if ( m_pFrogColorManagedSurface )
            {
                frog_color_managed_surface_set_render_intent( m_pFrogColorManagedSurface, FROG_COLOR_MANAGED_SURFACE_RENDER_INTENT_PERCEPTUAL );
                switch ( oState->eColorspace )
                {
                    default:
                    case GAMESCOPE_APP_TEXTURE_COLORSPACE_PASSTHRU:
                        frog_color_managed_surface_set_known_container_color_volume( m_pFrogColorManagedSurface, FROG_COLOR_MANAGED_SURFACE_PRIMARIES_UNDEFINED );
                        frog_color_managed_surface_set_known_container_color_volume( m_pFrogColorManagedSurface, FROG_COLOR_MANAGED_SURFACE_TRANSFER_FUNCTION_UNDEFINED );
                        break;
                    case GAMESCOPE_APP_TEXTURE_COLORSPACE_LINEAR:
                    case GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB:
                        frog_color_managed_surface_set_known_container_color_volume( m_pFrogColorManagedSurface, FROG_COLOR_MANAGED_SURFACE_PRIMARIES_REC709 );
                        frog_color_managed_surface_set_known_transfer_function( m_pFrogColorManagedSurface, FROG_COLOR_MANAGED_SURFACE_TRANSFER_FUNCTION_GAMMA_22 );
                        break;
                    case GAMESCOPE_APP_TEXTURE_COLORSPACE_HDR10_PQ:
                        frog_color_managed_surface_set_known_container_color_volume( m_pFrogColorManagedSurface, FROG_COLOR_MANAGED_SURFACE_PRIMARIES_REC2020 );
                        frog_color_managed_surface_set_known_transfer_function( m_pFrogColorManagedSurface, FROG_COLOR_MANAGED_SURFACE_TRANSFER_FUNCTION_ST2084_PQ );
                        break;
                    case GAMESCOPE_APP_TEXTURE_COLORSPACE_SCRGB:
                        frog_color_managed_surface_set_known_container_color_volume( m_pFrogColorManagedSurface, FROG_COLOR_MANAGED_SURFACE_PRIMARIES_REC709 );
                        frog_color_managed_surface_set_known_transfer_function( m_pFrogColorManagedSurface, FROG_COLOR_MANAGED_SURFACE_TRANSFER_FUNCTION_SCRGB_LINEAR );
                        break;
                }
            }

            // Fraction with denominator of 120 per. spec
            const uint32_t uScale = oState->uFractionalScale;

            wp_viewport_set_source(
                m_pViewport,
                wl_fixed_from_double( oState->flSrcX ),
                wl_fixed_from_double( oState->flSrcY ),
                wl_fixed_from_double( oState->flSrcWidth ),
                wl_fixed_from_double( oState->flSrcHeight ) );
            wp_viewport_set_destination(
                m_pViewport,
                WaylandScaleToLogical( oState->nDstWidth, uScale ),
                WaylandScaleToLogical( oState->nDstHeight, uScale ) );

            if ( m_pSubsurface )
            {
                wl_subsurface_set_position(
                    m_pSubsurface,
                    WaylandScaleToLogical( oState->nDestX, uScale ),
                    WaylandScaleToLogical( oState->nDestY, uScale ) );
            }
            // The x/y here does nothing? Why? What is it for...
            // Use the subsurface set_position thing instead.
            wl_surface_attach( m_pSurface, oState->pBuffer, 0, 0 );
            wl_surface_damage( m_pSurface, 0, 0, INT32_MAX, INT32_MAX );
            wl_surface_set_opaque_region( m_pSurface, oState->bOpaque ? m_pBackend->GetFullRegion() : nullptr );
            wl_surface_set_buffer_scale( m_pSurface, 1 );
        }
        else
        {
            wl_surface_attach( m_pSurface, nullptr, 0, 0 );
            wl_surface_damage( m_pSurface, 0, 0, INT32_MAX, INT32_MAX );
        }
    }

    void CWaylandPlane::CommitLibDecor( libdecor_configuration *pConfiguration )
    {
        int32_t uScale = GetScale();
        libdecor_state *pState = libdecor_state_new(
            WaylandScaleToLogical( g_nOutputWidth, uScale ),
            WaylandScaleToLogical( g_nOutputHeight, uScale ) );
        libdecor_frame_commit( m_pFrame, pState, pConfiguration );
        libdecor_state_free( pState );
    }

    void CWaylandPlane::Commit()
    {
        if ( m_bNeedsDecorCommit )
        {
            CommitLibDecor( nullptr );
            m_bNeedsDecorCommit = false;
        }

        wl_surface_commit( m_pSurface );
    }

    xdg_toplevel *CWaylandPlane::GetXdgToplevel() const
    {
        if ( !m_pFrame )
            return nullptr;

        return libdecor_frame_get_xdg_toplevel( m_pFrame );
    }

    void CWaylandPlane::Present( const FrameInfo_t::Layer_t *pLayer )
    {
        CWaylandFb *pWaylandFb = pLayer && pLayer->tex != nullptr ? static_cast<CWaylandFb*>( pLayer->tex->GetBackendFb() ) : nullptr;
        wl_buffer *pBuffer = pWaylandFb ? pWaylandFb->GetHostBuffer() : nullptr;

        if ( pBuffer )
        {
            pWaylandFb->OnCompositorAcquire();

            Present(
                ClipPlane( WaylandPlaneState
                {
                    .pBuffer     = pBuffer,
                    .nDestX      = int32_t( -pLayer->offset.x ),
                    .nDestY      = int32_t( -pLayer->offset.y ),
                    .flSrcX      = 0.0,
                    .flSrcY      = 0.0,
                    .flSrcWidth  = double( pLayer->tex->width() ),
                    .flSrcHeight = double( pLayer->tex->height() ),
                    .nDstWidth   = int32_t( ceil( pLayer->tex->width() / double( pLayer->scale.x ) ) ),
                    .nDstHeight  = int32_t( ceil( pLayer->tex->height() / double( pLayer->scale.y ) ) ),
                    .eColorspace = pLayer->colorspace,
                    .pHDRMetadata = pLayer->hdr_metadata_blob,
                    .bOpaque     = pLayer->zpos == g_zposBase,
                    .uFractionalScale = GetScale(),
                } ) );
        }
        else
        {
            Present( std::nullopt );
        }
    }

    void CWaylandPlane::UpdateVRRRefreshRate()
    {
        if ( m_pParent )
            return;

        if ( !m_pConnector->HostCompositorIsCurrentlyVRR() )
            return;

        if ( m_pOutputs.empty() )
            return;

        int32_t nLargestRefreshRateMhz = 0;
        for ( wl_output *pOutput : m_pOutputs )
        {
            WaylandOutputInfo *pOutputInfo = m_pBackend->GetOutputInfo( pOutput );
            if ( !pOutputInfo )
                continue;

            nLargestRefreshRateMhz = std::max( nLargestRefreshRateMhz, pOutputInfo->nRefresh );
        }

        if ( nLargestRefreshRateMhz && nLargestRefreshRateMhz != g_nOutputRefresh )
        {
            // TODO(strategy): We should pick the largest refresh rate.
            xdg_log.infof( "Changed refresh to: %.3fhz", ConvertmHzToHz( (float) nLargestRefreshRateMhz ) );
            g_nOutputRefresh = nLargestRefreshRateMhz;
        }
    }

    void CWaylandPlane::Wayland_Surface_Enter( wl_surface *pSurface, wl_output *pOutput )
    {
        if ( !IsSurfacePlane( pSurface ) )
            return;

        m_pOutputs.emplace_back( pOutput );

        UpdateVRRRefreshRate();
    }
    void CWaylandPlane::Wayland_Surface_Leave( wl_surface *pSurface, wl_output *pOutput )
    {
        if ( !IsSurfacePlane( pSurface ) )
            return;

        std::erase( m_pOutputs, pOutput );

        UpdateVRRRefreshRate();
    }

    void CWaylandPlane::LibDecor_Frame_Configure( libdecor_frame *pFrame, libdecor_configuration *pConfiguration )
    {
	    if ( !libdecor_configuration_get_window_state( pConfiguration, &m_eWindowState ) )
		    m_eWindowState = LIBDECOR_WINDOW_STATE_NONE;

        int32_t uScale = GetScale();

        int nWidth, nHeight;
        if ( !libdecor_configuration_get_content_size( pConfiguration, m_pFrame, &nWidth, &nHeight ) )
        {
            // XXX(virtual connector): Move g_nOutputWidth etc to connector.
            // Right now we are doubling this up when we should not be.
            //
            // Which is causing problems.
            nWidth  = WaylandScaleToLogical( g_nOutputWidth, uScale );
            nHeight = WaylandScaleToLogical( g_nOutputHeight, uScale );
        }
        g_nOutputWidth  = WaylandScaleToPhysical( nWidth, uScale );
        g_nOutputHeight = WaylandScaleToPhysical( nHeight, uScale );

        CommitLibDecor( pConfiguration );

        force_repaint();
	}
    void CWaylandPlane::LibDecor_Frame_Close( libdecor_frame *pFrame )
    {
        raise( SIGTERM );
    }
    void CWaylandPlane::LibDecor_Frame_Commit( libdecor_frame *pFrame )
    {
        m_bNeedsDecorCommit = true;
        force_repaint();
    }
    void CWaylandPlane::LibDecor_Frame_DismissPopup( libdecor_frame *pFrame, const char *pSeatName )
    {
    }

    void CWaylandPlane::Wayland_PresentationFeedback_SyncOutput( struct wp_presentation_feedback *pFeedback, wl_output *pOutput )
    {
    }
    void CWaylandPlane::Wayland_PresentationFeedback_Presented( struct wp_presentation_feedback *pFeedback, uint32_t uTVSecHi, uint32_t uTVSecLo, uint32_t uTVNSec, uint32_t uRefreshCycle, uint32_t uSeqHi, uint32_t uSeqLo, uint32_t uFlags )
    {
        uint64_t ulTime = ( ( ( uint64_t( uTVSecHi ) << 32ul ) | uTVSecLo ) * 1'000'000'000lu ) +
                          ( uint64_t( uTVNSec ) );

        if ( uRefreshCycle )
        {
            int32_t nRefresh = RefreshCycleTomHz( uRefreshCycle );
            if ( nRefresh && nRefresh != g_nOutputRefresh )
            {
                xdg_log.infof( "Changed refresh to: %.3fhz", ConvertmHzToHz( (float) nRefresh ) );
                g_nOutputRefresh = nRefresh;
            }

            m_pConnector->SetHostCompositorIsCurrentlyVRR( false );
        }
        else
        {
            m_pConnector->SetHostCompositorIsCurrentlyVRR( true );

            UpdateVRRRefreshRate();
        }

        GetVBlankTimer().MarkVBlank( ulTime, true );
        wp_presentation_feedback_destroy( pFeedback );

        // Nudge so that steamcompmgr releases commits.
        nudge_steamcompmgr();
    }
    void CWaylandPlane::Wayland_PresentationFeedback_Discarded( struct wp_presentation_feedback *pFeedback )
    {
        wp_presentation_feedback_destroy( pFeedback );

        // Nudge so that steamcompmgr releases commits.
        nudge_steamcompmgr();
    }

    void CWaylandPlane::Wayland_FrogColorManagedSurface_PreferredMetadata(
        frog_color_managed_surface *pFrogSurface,
        uint32_t uTransferFunction,
        uint32_t uOutputDisplayPrimaryRedX,
        uint32_t uOutputDisplayPrimaryRedY,
        uint32_t uOutputDisplayPrimaryGreenX,
        uint32_t uOutputDisplayPrimaryGreenY,
        uint32_t uOutputDisplayPrimaryBlueX,
        uint32_t uOutputDisplayPrimaryBlueY,
        uint32_t uOutputWhitePointX,
        uint32_t uOutputWhitePointY,
        uint32_t uMaxLuminance,
        uint32_t uMinLuminance,
        uint32_t uMaxFullFrameLuminance )
    {
        auto *pHDRInfo = &m_pConnector->m_HDRInfo;
        pHDRInfo->bExposeHDRSupport         = ( cv_hdr_enabled && uTransferFunction == FROG_COLOR_MANAGED_SURFACE_TRANSFER_FUNCTION_ST2084_PQ );
        pHDRInfo->eOutputEncodingEOTF       = ( cv_hdr_enabled && uTransferFunction == FROG_COLOR_MANAGED_SURFACE_TRANSFER_FUNCTION_ST2084_PQ ) ? EOTF_PQ : EOTF_Gamma22;
        pHDRInfo->uMaxContentLightLevel     = uMaxLuminance;
        pHDRInfo->uMaxFrameAverageLuminance = uMaxFullFrameLuminance;
        pHDRInfo->uMinContentLightLevel     = uMinLuminance;

        auto *pDisplayColorimetry = &m_pConnector->m_DisplayColorimetry;
        pDisplayColorimetry->primaries.r = glm::vec2{ uOutputDisplayPrimaryRedX * 0.00002f, uOutputDisplayPrimaryRedY * 0.00002f };
        pDisplayColorimetry->primaries.g = glm::vec2{ uOutputDisplayPrimaryGreenX * 0.00002f, uOutputDisplayPrimaryGreenY * 0.00002f };
        pDisplayColorimetry->primaries.b = glm::vec2{ uOutputDisplayPrimaryBlueX * 0.00002f, uOutputDisplayPrimaryBlueY * 0.00002f };
        pDisplayColorimetry->white = glm::vec2{ uOutputWhitePointX * 0.00002f, uOutputWhitePointY * 0.00002f };

        xdg_log.infof( "PreferredMetadata: Red: %g %g, Green: %g %g, Blue: %g %g, White: %g %g, Max Luminance: %u nits, Min Luminance: %g nits, Max Full Frame Luminance: %u nits",
            uOutputDisplayPrimaryRedX * 0.00002, uOutputDisplayPrimaryRedY * 0.00002,
            uOutputDisplayPrimaryGreenX * 0.00002, uOutputDisplayPrimaryGreenY * 0.00002,
            uOutputDisplayPrimaryBlueX * 0.00002, uOutputDisplayPrimaryBlueY * 0.00002,
            uOutputWhitePointX * 0.00002, uOutputWhitePointY * 0.00002,
            uint32_t( uMaxLuminance ),
            uMinLuminance * 0.0001,
            uint32_t( uMaxFullFrameLuminance ) );
    }

    //

    void CWaylandPlane::Wayland_WPColorManagementSurfaceFeedback_PreferredChanged( wp_color_management_surface_feedback_v1 *pColorManagementSurface, unsigned int data)
    {
        UpdateWPPreferredColorManagement();
    }

    void CWaylandPlane::UpdateWPPreferredColorManagement()
    {
        if ( m_pParent )
            return;

        wp_image_description_v1 *pImageDescription = wp_color_management_surface_feedback_v1_get_preferred( m_pWPColorManagedSurfaceFeedback );
        wp_image_description_info_v1 *pImageDescInfo = wp_image_description_v1_get_information( pImageDescription );
        wp_image_description_info_v1_add_listener( pImageDescInfo, &s_ImageDescriptionInfoListener, this );
        wl_display_roundtrip( m_pBackend->GetDisplay() );

        wp_image_description_info_v1_destroy( pImageDescInfo );
        wp_image_description_v1_destroy( pImageDescription );
    }

    void CWaylandPlane::Wayland_WPImageDescriptionInfo_Done( wp_image_description_info_v1 *pImageDescInfo )
    {

    }
    void CWaylandPlane::Wayland_WPImageDescriptionInfo_ICCFile( wp_image_description_info_v1 *pImageDescInfo, int32_t nICCFd, uint32_t uICCSize )
    {
        if ( nICCFd >= 0 )
            close( nICCFd );
    }
    void CWaylandPlane::Wayland_WPImageDescriptionInfo_Primaries( wp_image_description_info_v1 *pImageDescInfo, int32_t nRedX, int32_t nRedY, int32_t nGreenX, int32_t nGreenY, int32_t nBlueX, int32_t nBlueY, int32_t nWhiteX, int32_t nWhiteY )
    {

    }
    void CWaylandPlane::Wayland_WPImageDescriptionInfo_PrimariesNamed( wp_image_description_info_v1 *pImageDescInfo, uint32_t uPrimaries )
    {

    }
    void CWaylandPlane::Wayland_WPImageDescriptionInfo_TFPower( wp_image_description_info_v1 *pImageDescInfo, uint32_t uExp)
    {

    }
    static const char *TFToString( uint32_t uTF )
    {
        switch ( (wp_color_manager_v1_transfer_function) uTF )
        {
            case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_BT1886: return "BT1886";
            case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22: return "GAMMA22";
            case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA28: return "GAMMA28";
            case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST240: return "ST240";
            case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR: return "EXT_LINEAR";
            case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_LOG_100: return "LOG_100";
            case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_LOG_316: return "LOG_316";
            case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_XVYCC: return "XVYCC";
            case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB: return "SRGB";
            case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_SRGB: return "EXT_SRGB";
            case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ: return "ST2084_PQ";
            case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST428: return "ST428";
            case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_HLG: return "HLG";
            default: return "Unknown";
        }
    }
    void CWaylandPlane::Wayland_WPImageDescriptionInfo_TFNamed( wp_image_description_info_v1 *pImageDescInfo, uint32_t uTF)
    {
        auto *pHDRInfo = &m_pConnector->m_HDRInfo;
        pHDRInfo->bExposeHDRSupport   = ( cv_hdr_enabled && uTF == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ );
        pHDRInfo->eOutputEncodingEOTF = ( cv_hdr_enabled && uTF == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ ) ? EOTF_PQ : EOTF_Gamma22;

        xdg_log.infof( "HDR INFO" );
        xdg_log.infof( "  cv_hdr_enabled: %s", cv_hdr_enabled ? "true" : "false" );
        xdg_log.infof( "  uTF: %s", TFToString( uTF ) );
        xdg_log.infof( "  bExposeHDRSupport: %s", pHDRInfo->bExposeHDRSupport ? "true" : "false" );
    }
    void CWaylandPlane::Wayland_WPImageDescriptionInfo_Luminances( wp_image_description_info_v1 *pImageDescInfo, uint32_t uMinLum, uint32_t uMaxLum, uint32_t uRefLum )
    {

    }
    void CWaylandPlane::Wayland_WPImageDescriptionInfo_TargetPrimaries( wp_image_description_info_v1 *pImageDescInfo, int32_t nRedX, int32_t nRedY, int32_t nGreenX, int32_t nGreenY, int32_t nBlueX, int32_t nBlueY, int32_t nWhiteX, int32_t nWhiteY )
    {
        auto *pDisplayColorimetry = &m_pConnector->m_DisplayColorimetry;
        pDisplayColorimetry->primaries.r = glm::vec2{ nRedX / 1000000.0f, nRedY / 1000000.0f };
        pDisplayColorimetry->primaries.g = glm::vec2{ nGreenX / 1000000.0f, nGreenY / 1000000.0f };
        pDisplayColorimetry->primaries.b = glm::vec2{ nBlueX / 1000000.0f, nBlueY / 1000000.0f };
        pDisplayColorimetry->white = glm::vec2{ nWhiteX / 1000000.0f, nWhiteY / 1000000.0f };
    }
    void CWaylandPlane::Wayland_WPImageDescriptionInfo_TargetLuminance( wp_image_description_info_v1 *pImageDescInfo, uint32_t uMinLum, uint32_t uMaxLum )
    {

    }
    void CWaylandPlane::Wayland_WPImageDescriptionInfo_Target_MaxCLL( wp_image_description_info_v1 *pImageDescInfo, uint32_t uMaxCLL )
    {
        auto *pHDRInfo = &m_pConnector->m_HDRInfo;
        pHDRInfo->uMaxContentLightLevel = uMaxCLL;
        xdg_log.infof( "uMaxContentLightLevel: %u", uMaxCLL );
    }
    void CWaylandPlane::Wayland_WPImageDescriptionInfo_Target_MaxFALL( wp_image_description_info_v1 *pImageDescInfo, uint32_t uMaxFALL )
    {
        auto *pHDRInfo = &m_pConnector->m_HDRInfo;
        pHDRInfo->uMaxFrameAverageLuminance = uMaxFALL;
    }

    //

    void CWaylandPlane::Wayland_FractionalScale_PreferredScale( wp_fractional_scale_v1 *pFractionalScale, uint32_t uScale )
    {
        bool bDirty = false;

        static uint32_t s_uGlobalFractionalScale = 120;
        if ( s_uGlobalFractionalScale != uScale )
        {
            if ( m_bHasRecievedScale )
            {
                g_nOutputWidth  = ( g_nOutputWidth  * uScale ) / m_uFractionalScale;
                g_nOutputHeight = ( g_nOutputHeight * uScale ) / m_uFractionalScale;
            }

            s_uGlobalFractionalScale = uScale;
            bDirty = true;
        }

        if ( m_uFractionalScale != uScale )
        {
            m_uFractionalScale = uScale;
            bDirty = true;
        }

        m_bHasRecievedScale = true;

        if ( bDirty )
            force_repaint();
    }

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

                // Transfer Functions
                if ( !Algorithm::Contains( m_WPColorManagerFeatures.eTransferFunctions, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB ) )
                    return false;
                if ( !Algorithm::Contains( m_WPColorManagerFeatures.eTransferFunctions, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ ) )
                    return false;
                // TODO: Need scRGB

                // Primaries
                if ( !Algorithm::Contains( m_WPColorManagerFeatures.ePrimaries, WP_COLOR_MANAGER_V1_PRIMARIES_SRGB ) )
                    return false;
                if ( !Algorithm::Contains( m_WPColorManagerFeatures.ePrimaries, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020 ) )
                    return false;

                return true;
            }();
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

        return true;
    }

    bool CWaylandBackend::PostInit()
    {
        m_pFullRegion = wl_compositor_create_region( m_pCompositor );
        wl_region_add( m_pFullRegion, 0, 0, INT32_MAX, INT32_MAX );

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
        for (int i = 0; i < 2 && u8BitFormat == DRM_FORMAT_INVALID; i++)
        {
            bool invalidOnly = i == 0;
            if ( SupportsFormat( DRM_FORMAT_XRGB8888, invalidOnly ) )
                u8BitFormat = DRM_FORMAT_XRGB8888;
            else if ( SupportsFormat( DRM_FORMAT_XBGR8888, invalidOnly ) )
                u8BitFormat = DRM_FORMAT_XBGR8888;
            else if ( SupportsFormat( DRM_FORMAT_ARGB8888, invalidOnly ) )
                u8BitFormat = DRM_FORMAT_ARGB8888;
            else if ( SupportsFormat( DRM_FORMAT_ABGR8888, invalidOnly ) )
                u8BitFormat = DRM_FORMAT_ABGR8888;
        }

        uint32_t u10BitFormat = DRM_FORMAT_INVALID;
        for (int i = 0; i < 2 && u10BitFormat == DRM_FORMAT_INVALID; i++)
        {
            bool invalidOnly = i == 0;
            if ( SupportsFormat( DRM_FORMAT_XBGR2101010, invalidOnly ) )
                u10BitFormat = DRM_FORMAT_XBGR2101010;
            else if ( SupportsFormat( DRM_FORMAT_XRGB2101010, invalidOnly ) )
                u10BitFormat = DRM_FORMAT_XRGB2101010;
            else if ( SupportsFormat( DRM_FORMAT_ABGR2101010, invalidOnly ) )
                u10BitFormat = DRM_FORMAT_ABGR2101010;
            else if ( SupportsFormat( DRM_FORMAT_ARGB2101010, invalidOnly ) )
                u10BitFormat = DRM_FORMAT_ARGB2101010;
        }

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

    OwningRc<IBackendFb> CWaylandBackend::ImportDmabufToBackend( wlr_buffer *pClientBuffer, wlr_dmabuf_attributes *pDmaBuf )
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

                zwp_relative_pointer_v1_destroy( m_pRelativePointer );
                m_pRelativePointer = nullptr;
            }

            if ( bRelative )
            {
                m_pLockedPointer = zwp_pointer_constraints_v1_lock_pointer( m_pPointerConstraints, pSurface, m_pPointer, nullptr, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT );
                m_pRelativePointer = zwp_relative_pointer_manager_v1_get_relative_pointer( m_pRelativePointerManager, m_pPointer );
            }

            m_InputThread.SetRelativePointer( bRelative );

            UpdateCursor();
        }
    }

    void CWaylandBackend::UpdateCursor()
    {
        bool bUseHostCursor = false;

        if ( cv_wayland_mouse_warp_without_keyboard_focus )
            bUseHostCursor = m_pRelativePointer && !m_bKeyboardEntered && m_pDefaultCursorSurface;
        else
            bUseHostCursor = !m_bKeyboardEntered && m_pDefaultCursorSurface;

        if ( bUseHostCursor )
        {
            wl_pointer_set_cursor( m_pPointer, m_uPointerEnterSerial, m_pDefaultCursorSurface, m_pDefaultCursorInfo->uXHotspot, m_pDefaultCursorInfo->uYHotspot );
        }
        else
        {
            bool bHideCursor = m_pLockedPointer || !m_pCursorSurface;

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
        if ( !IsSurfacePlane( pSurface ) )
            return;

        m_uPointerEnterSerial = uSerial;
        m_bMouseEntered = true;

        UpdateCursor();
    }
    void CWaylandBackend::Wayland_Pointer_Leave( wl_pointer *pPointer, uint32_t uSerial, wl_surface *pSurface )
    {
        if ( !IsSurfacePlane( pSurface ) )
            return;

        m_bMouseEntered = false;
    }

    // Keyboard

    void CWaylandBackend::Wayland_Keyboard_Enter( wl_keyboard *pKeyboard, uint32_t uSerial, wl_surface *pSurface, wl_array *pKeys )
    {
        if ( !IsSurfacePlane( pSurface ) )
            return;

        m_uKeyboardEnterSerial = uSerial;
        m_bKeyboardEntered = true;

        UpdateCursor();
    }
    void CWaylandBackend::Wayland_Keyboard_Leave( wl_keyboard *pKeyboard, uint32_t uSerial, wl_surface *pSurface )
    {
        if ( !IsSurfacePlane( pSurface ) )
            return;

        m_bKeyboardEntered = false;

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

    CWaylandInputThread::CWaylandInputThread()
        : m_Thread{ [this](){ this->ThreadFunc(); } }
    {
    }

    CWaylandInputThread::~CWaylandInputThread()
    {
        m_bInitted = true;
        m_bInitted.notify_all();

        m_Waiter.Shutdown();
        m_Thread.join();
    }

    bool CWaylandInputThread::Init( CWaylandBackend *pBackend )
    {
        m_pBackend = pBackend;

        if ( !( m_pXkbContext = xkb_context_new( XKB_CONTEXT_NO_FLAGS ) ) )
        {
            xdg_log.errorf( "Couldn't create xkb context." );
            return false;
        }

        if ( !( m_pQueue = wl_display_create_queue( m_pBackend->GetDisplay() ) ) )
        {
            xdg_log.errorf( "Couldn't create input thread queue." );
            return false;
        }

        if ( !( m_pDisplayWrapper = QueueLaunder( m_pBackend->GetDisplay() ) ) )
        {
            xdg_log.errorf( "Couldn't create display proxy for input thread" );
            return false;
        }

        wl_registry *pRegistry;
        if ( !( pRegistry = wl_display_get_registry( m_pDisplayWrapper.get() ) ) )
        {
            xdg_log.errorf( "Couldn't create registry for input thread" );
            return false;
        }
        wl_registry_add_listener( pRegistry, &s_RegistryListener, this );

        wl_display_roundtrip_queue( pBackend->GetDisplay(), m_pQueue );
        wl_display_roundtrip_queue( pBackend->GetDisplay(), m_pQueue );

        wl_registry_destroy( pRegistry );
        pRegistry = nullptr;

        if ( !m_pSeat || !m_pRelativePointerManager )
        {
            xdg_log.errorf( "Couldn't create Wayland input objects." );
            return false;
        }

        m_bInitted = true;
        m_bInitted.notify_all();
        return true;
    }

    void CWaylandInputThread::ThreadFunc()
    {
        m_bInitted.wait( false );

        if ( !m_Waiter.IsRunning() )
            return;

        int nFD = wl_display_get_fd( m_pBackend->GetDisplay() );
        if ( nFD < 0 )
        {
            abort();
        }

        CFunctionWaitable waitable( nFD );
        m_Waiter.AddWaitable( &waitable );

        int nRet = 0;
        while ( m_Waiter.IsRunning() )
        {
            if ( ( nRet = wl_display_dispatch_queue_pending( m_pBackend->GetDisplay(), m_pQueue ) ) < 0 )
            {
                abort();
            }

            if ( ( nRet = wl_display_prepare_read_queue( m_pBackend->GetDisplay(), m_pQueue ) ) < 0 )
            {
                if ( errno == EAGAIN || errno == EINTR )
                    continue;

                abort();
            }

            if ( ( nRet = m_Waiter.PollEvents() ) <= 0 )
            {
                wl_display_cancel_read( m_pBackend->GetDisplay() );
                if ( nRet < 0 )
                    abort();

                assert( nRet == 0 );
                continue;
            }

            if ( ( nRet = wl_display_read_events( m_pBackend->GetDisplay() ) ) < 0 )
            {
                abort();
            }
        }
    }

    template <typename T>
    std::shared_ptr<T> CWaylandInputThread::QueueLaunder( T* pObject )
    {
        if ( !pObject )
            return nullptr;

        T *pObjectWrapper = (T*)wl_proxy_create_wrapper( (void *)pObject );
        if ( !pObjectWrapper )
            return nullptr;
        wl_proxy_set_queue( (wl_proxy *)pObjectWrapper, m_pQueue );

        return std::shared_ptr<T>{ pObjectWrapper, []( T *pThing ){ wl_proxy_wrapper_destroy( (void *)pThing ); } };
    }

    void CWaylandInputThread::SetRelativePointer( bool bRelative )
    {
        if ( bRelative == !!m_pRelativePointer.load() )
            return;
        // This constructors/destructors the display's mutex, so should be safe to do across threads.
        if ( !bRelative )
        {
            m_pRelativePointer = nullptr;
        }
        else
        {
            zwp_relative_pointer_v1 *pRelativePointer = zwp_relative_pointer_manager_v1_get_relative_pointer( m_pRelativePointerManager, m_pPointer );
            m_pRelativePointer = std::shared_ptr<zwp_relative_pointer_v1>{ pRelativePointer, []( zwp_relative_pointer_v1 *pObject ){ zwp_relative_pointer_v1_destroy( pObject ); } };
            zwp_relative_pointer_v1_add_listener( pRelativePointer, &s_RelativePointerListener, this );
        }
    }

    void CWaylandInputThread::HandleKey( uint32_t uKey, bool bPressed )
    {
        if ( m_uKeyModifiers & m_uModMask[ GAMESCOPE_WAYLAND_MOD_META ] )
        {
            switch ( uKey )
            {
                case KEY_F:
                {
                    if ( !bPressed )
                    {
                        static_cast< CWaylandConnector * >( m_pBackend->GetCurrentConnector() )->SetFullscreen( !g_bFullscreen );
                    }
                    return;
                }

                case KEY_N:
                {
                    if ( !bPressed )
                    {
                        g_wantedUpscaleFilter = GamescopeUpscaleFilter::PIXEL;
                    }
                    return;
                }

                case KEY_B:
                {
                    if ( !bPressed )
                    {
                        g_wantedUpscaleFilter = GamescopeUpscaleFilter::LINEAR;
                    }
                    return;
                }

                case KEY_U:
                {
                    if ( !bPressed )
                    {
                        g_wantedUpscaleFilter = ( g_wantedUpscaleFilter == GamescopeUpscaleFilter::FSR ) ?
                            GamescopeUpscaleFilter::LINEAR : GamescopeUpscaleFilter::FSR;
                    }
                    return;
                }

                case KEY_Y:
                {
                    if ( !bPressed )
                    {
                        g_wantedUpscaleFilter = ( g_wantedUpscaleFilter == GamescopeUpscaleFilter::NIS ) ?
                            GamescopeUpscaleFilter::LINEAR : GamescopeUpscaleFilter::NIS;
                    }
                    return;
                }

                case KEY_I:
                {
                    if ( !bPressed )
                    {
                        g_upscaleFilterSharpness = std::min( 20, g_upscaleFilterSharpness + 1 );
                    }
                    return;
                }

                case KEY_O:
                {
                    if ( !bPressed )
                    {
                        g_upscaleFilterSharpness = std::max( 0, g_upscaleFilterSharpness - 1 );
                    }
                    return;
                }

                case KEY_S:
                {
                    if ( !bPressed )
                    {
                        gamescope::CScreenshotManager::Get().TakeScreenshot( true );
                    }
                    return;
                }

                default:
                    break;
            }
        }

        wlserver_lock();
        wlserver_key( uKey, bPressed, ++m_uFakeTimestamp );
        wlserver_unlock();
    }

    // Registry

    void CWaylandInputThread::Wayland_Registry_Global( wl_registry *pRegistry, uint32_t uName, const char *pInterface, uint32_t uVersion )
    {
        if ( !strcmp( pInterface, wl_seat_interface.name ) && uVersion >= 8u )
        {
            m_pSeat = (wl_seat *)wl_registry_bind( pRegistry, uName, &wl_seat_interface, 8u );
            wl_seat_add_listener( m_pSeat, &s_SeatListener, this );
        }
        else if ( !strcmp( pInterface, zwp_relative_pointer_manager_v1_interface.name ) )
        {
            m_pRelativePointerManager = (zwp_relative_pointer_manager_v1 *)wl_registry_bind( pRegistry, uName, &zwp_relative_pointer_manager_v1_interface, 1u );
        }
    }

    // Seat

    void CWaylandInputThread::Wayland_Seat_Capabilities( wl_seat *pSeat, uint32_t uCapabilities )
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

    void CWaylandInputThread::Wayland_Seat_Name( wl_seat *pSeat, const char *pName )
    {
        xdg_log.infof( "Seat name: %s", pName );
    }

    // Pointer

    void CWaylandInputThread::Wayland_Pointer_Enter( wl_pointer *pPointer, uint32_t uSerial, wl_surface *pSurface, wl_fixed_t fSurfaceX, wl_fixed_t fSurfaceY )
    {
        if ( !IsSurfacePlane( pSurface ) )
            return;

        CWaylandPlane *pPlane = (CWaylandPlane *)wl_surface_get_user_data( pSurface );
        if ( !pPlane )
            return;
        m_pCurrentCursorPlane = pPlane;
        m_bMouseEntered = true;
        m_uPointerEnterSerial = uSerial;

        Wayland_Pointer_Motion( pPointer, 0, fSurfaceX, fSurfaceY );
    }
    void CWaylandInputThread::Wayland_Pointer_Leave( wl_pointer *pPointer, uint32_t uSerial, wl_surface *pSurface )
    {
        if ( !IsSurfacePlane( pSurface ) )
            return;

        CWaylandPlane *pPlane = (CWaylandPlane *)wl_surface_get_user_data( pSurface );
        if ( !pPlane )
            return;
        if ( pPlane != m_pCurrentCursorPlane )
            return;
        m_pCurrentCursorPlane = nullptr;
        m_bMouseEntered = false;
    }
    void CWaylandInputThread::Wayland_Pointer_Motion( wl_pointer *pPointer, uint32_t uTime, wl_fixed_t fSurfaceX, wl_fixed_t fSurfaceY )
    {
        if ( m_pRelativePointer.load() != nullptr )
            return;

        if ( !cv_wayland_mouse_warp_without_keyboard_focus && !m_bKeyboardEntered )
        {
            // Don't do any motion/movement stuff if we don't have kb focus
            m_ofPendingCursorX = fSurfaceX;
            m_ofPendingCursorY = fSurfaceY;
            return;
        }

        if ( !m_pCurrentCursorPlane )
            return;

        auto oState = m_pCurrentCursorPlane->GetCurrentState();
        if ( !oState )
            return;

        uint32_t uScale = oState->uFractionalScale;

        double flX = ( wl_fixed_to_double( fSurfaceX ) * uScale / 120.0 + oState->nDestX ) / g_nOutputWidth;
        double flY = ( wl_fixed_to_double( fSurfaceY ) * uScale / 120.0 + oState->nDestY ) / g_nOutputHeight;

        wlserver_lock();
        wlserver_touchmotion( flX, flY, 0, ++m_uFakeTimestamp );
        wlserver_unlock();
    }
    void CWaylandInputThread::Wayland_Pointer_Button( wl_pointer *pPointer, uint32_t uSerial, uint32_t uTime, uint32_t uButton, uint32_t uState )
    {
        // Don't do any motion/movement stuff if we don't have kb focus
        if ( !cv_wayland_mouse_warp_without_keyboard_focus && !m_bKeyboardEntered )
            return;

        wlserver_lock();
        wlserver_mousebutton( uButton, uState == WL_POINTER_BUTTON_STATE_PRESSED, ++m_uFakeTimestamp );
        wlserver_unlock();
    }
    void CWaylandInputThread::Wayland_Pointer_Axis( wl_pointer *pPointer, uint32_t uTime, uint32_t uAxis, wl_fixed_t fValue )
    {
    }
    void CWaylandInputThread::Wayland_Pointer_Axis_Source( wl_pointer *pPointer, uint32_t uAxisSource )
    {
        m_uAxisSource = uAxisSource;
    }
    void CWaylandInputThread::Wayland_Pointer_Axis_Stop( wl_pointer *pPointer, uint32_t uTime, uint32_t uAxis )
    {
    }
    void CWaylandInputThread::Wayland_Pointer_Axis_Discrete( wl_pointer *pPointer, uint32_t uAxis, int32_t nDiscrete )
    {
    }
    void CWaylandInputThread::Wayland_Pointer_Axis_Value120( wl_pointer *pPointer, uint32_t uAxis, int32_t nValue120 )
    {
        if ( !cv_wayland_mouse_warp_without_keyboard_focus && !m_bKeyboardEntered )
            return;

        assert( uAxis == WL_POINTER_AXIS_VERTICAL_SCROLL || uAxis == WL_POINTER_AXIS_HORIZONTAL_SCROLL );

        // Vertical is first in the wl_pointer_axis enum, flip y,x -> x,y
        m_flScrollAccum[ !uAxis ] += nValue120 / 120.0;
    }
    void CWaylandInputThread::Wayland_Pointer_Frame( wl_pointer *pPointer )
    {
        defer( m_uAxisSource = WL_POINTER_AXIS_SOURCE_WHEEL );
        double flX = m_flScrollAccum[0];
        double flY = m_flScrollAccum[1];
        m_flScrollAccum[0] = 0.0;
        m_flScrollAccum[1] = 0.0;

        if ( !cv_wayland_mouse_warp_without_keyboard_focus && !m_bKeyboardEntered )
            return;

        if ( m_uAxisSource != WL_POINTER_AXIS_SOURCE_WHEEL )
            return;

        if ( flX == 0.0 && flY == 0.0 )
            return;

        wlserver_lock();
        wlserver_mousewheel( flX, flY, ++m_uFakeTimestamp );
        wlserver_unlock();
    }

    // Keyboard

    void CWaylandInputThread::Wayland_Keyboard_Keymap( wl_keyboard *pKeyboard, uint32_t uFormat, int32_t nFd, uint32_t uSize )
    {
        // We are not doing much with the keymap, we pass keycodes thru.
        // Ideally we'd use this to influence our keymap to clients, eg. x server.

        defer( close( nFd ) );
        if ( uFormat != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 )
		return;

        char *pMap = (char *)mmap( nullptr, uSize, PROT_READ, MAP_PRIVATE, nFd, 0 );
        if ( !pMap || pMap == MAP_FAILED )
        {
            xdg_log.errorf( "Failed to map keymap fd." );
            return;
        }
        defer( munmap( pMap, uSize ) );

        xkb_keymap *pKeymap = xkb_keymap_new_from_string( m_pXkbContext, pMap, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS );
        if ( !pKeymap )
        {
            xdg_log.errorf( "Failed to create xkb_keymap" );
            return;
        }

        xkb_keymap_unref( m_pXkbKeymap );
        m_pXkbKeymap = pKeymap;

        for ( uint32_t i = 0; i < GAMESCOPE_WAYLAND_MOD_COUNT; i++ )
            m_uModMask[ i ] = 1u << xkb_keymap_mod_get_index( m_pXkbKeymap, WaylandModifierToXkbModifierName( ( WaylandModifierIndex ) i ) );
    }
    void CWaylandInputThread::Wayland_Keyboard_Enter( wl_keyboard *pKeyboard, uint32_t uSerial, wl_surface *pSurface, wl_array *pKeys )
    {
        m_bKeyboardEntered = true;
        m_uScancodesHeld.clear();

        const uint32_t *pBegin = (uint32_t *)pKeys->data;
        const uint32_t *pEnd = pBegin + ( pKeys->size / sizeof(uint32_t) );
        std::span<const uint32_t> keys{ pBegin, pEnd };
        for ( uint32_t uKey : keys )
        {
            HandleKey( uKey, true );
            m_uScancodesHeld.insert( uKey );
        }

        if ( m_ofPendingCursorX )
        {
            assert( m_ofPendingCursorY.has_value() );

            Wayland_Pointer_Motion( m_pPointer, 0, *m_ofPendingCursorX, *m_ofPendingCursorY );
            m_ofPendingCursorX = std::nullopt;
            m_ofPendingCursorY = std::nullopt;
        }
    }
    void CWaylandInputThread::Wayland_Keyboard_Leave( wl_keyboard *pKeyboard, uint32_t uSerial, wl_surface *pSurface )
    {
        m_bKeyboardEntered = false;
        m_uKeyModifiers = 0;

        for ( uint32_t uKey : m_uScancodesHeld )
            HandleKey( uKey, false );

        m_uScancodesHeld.clear();
    }
    void CWaylandInputThread::Wayland_Keyboard_Key( wl_keyboard *pKeyboard, uint32_t uSerial, uint32_t uTime, uint32_t uKey, uint32_t uState )
    {
        if ( !m_bKeyboardEntered )
            return;

        const bool bPressed = uState == WL_KEYBOARD_KEY_STATE_PRESSED;
        const bool bWasPressed = m_uScancodesHeld.contains( uKey );
        if ( bWasPressed == bPressed )
            return;

        HandleKey( uKey, bPressed );

        if ( bWasPressed )
            m_uScancodesHeld.erase( uKey );
        else
            m_uScancodesHeld.emplace( uKey );
    }
    void CWaylandInputThread::Wayland_Keyboard_Modifiers( wl_keyboard *pKeyboard, uint32_t uSerial, uint32_t uModsDepressed, uint32_t uModsLatched, uint32_t uModsLocked, uint32_t uGroup )
    {
        m_uKeyModifiers = uModsDepressed | uModsLatched | uModsLocked;
    }
    void CWaylandInputThread::Wayland_Keyboard_RepeatInfo( wl_keyboard *pKeyboard, int32_t nRate, int32_t nDelay )
    {
    }

    // Relative Pointer

    void CWaylandInputThread::Wayland_RelativePointer_RelativeMotion( zwp_relative_pointer_v1 *pRelativePointer, uint32_t uTimeHi, uint32_t uTimeLo, wl_fixed_t fDx, wl_fixed_t fDy, wl_fixed_t fDxUnaccel, wl_fixed_t fDyUnaccel )
    {
        // Don't do any motion/movement stuff if we don't have kb focus
        if ( !cv_wayland_mouse_relmotion_without_keyboard_focus && !m_bKeyboardEntered )
            return;

        wlserver_lock();
        wlserver_mousemotion( wl_fixed_to_double( fDxUnaccel ), wl_fixed_to_double( fDyUnaccel ), ++m_uFakeTimestamp );
        wlserver_unlock();
    }

    /////////////////////////
    // Backend Instantiator
    /////////////////////////

    template <>
    bool IBackend::Set<CWaylandBackend>()
    {
        return Set( new CWaylandBackend{} );
    }
}
