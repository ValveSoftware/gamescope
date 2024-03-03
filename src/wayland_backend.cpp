#include "backend.h"
#include "rendervulkan.hpp"
#include "wlserver.hpp"
#include "vblankmanager.hpp"
#include "steamcompmgr.hpp"
#include "edid.h"
#include "defer.hpp"
#include "convar.h"

#include <cstring>
#include <unordered_map>
#include <csignal>
#include <sys/mman.h>

#include "wlr_begin.hpp"
#include <wayland-client.h>
#include <xdg-shell-client-protocol.h>
#include <linux-dmabuf-v1-client-protocol.h>
#include <viewporter-client-protocol.h>
#include <single-pixel-buffer-v1-client-protocol.h>
#include <presentation-time-client-protocol.h>
#include <frog-color-management-v1-client-protocol.h>
#include <pointer-constraints-unstable-v1-client-protocol.h>
#include <relative-pointer-unstable-v1-client-protocol.h>
#include "wlr_end.hpp"

#include "drm_include.h"

extern int g_nPreferredOutputWidth;
extern int g_nPreferredOutputHeight;
extern bool g_bForceHDR10OutputDebug;

extern bool alwaysComposite;
extern bool g_bColorSliderInUse;
extern bool fadingOut;
extern std::string g_reshade_effect;

using namespace std::literals;

static LogScope xdg_log( "xdg_backend" );

#define WAYLAND_NULL() []<typename... Args> ( void *pData, Args... args ) { }
#define WAYLAND_USERDATA_TO_THIS(type, name) []<typename... Args> ( void *pData, Args... args ) { type *pThing = (type *)pData; pThing->name( std::forward<Args>(args)... ); }

extern gamescope::ConVar<bool> cv_hdr_enabled;

namespace gamescope
{

    class CWaylandConnector;
    class CWaylandPlane;
    class CWaylandBackend;

    class CWaylandConnector final : public IBackendConnector
    {
    public:
        CWaylandConnector( CWaylandBackend *pBackend );
        bool UpdateEdid();

        virtual ~CWaylandConnector();

        /////////////////////
        // IBackendConnector
        /////////////////////

        virtual gamescope::GamescopeScreenType GetScreenType() const override;
        virtual GamescopePanelOrientation GetCurrentOrientation() const override;
        virtual bool SupportsHDR() const override;
        virtual bool IsHDRActive() const override;
        virtual const BackendConnectorHDRInfo &GetHDRInfo() const override;
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
    private:

        friend CWaylandPlane;

        BackendConnectorHDRInfo m_HDRInfo{};
        std::vector<uint8_t> m_FakeEdid;

        CWaylandBackend *m_pBackend = nullptr;
    };

    class CWaylandPlane
    {
    public:
        CWaylandPlane( CWaylandBackend *pBackend );
        ~CWaylandPlane();

        bool Init( CWaylandPlane *pParent, CWaylandPlane *pSiblingBelow );

        void Present( wl_buffer *pBuffer, int32_t nDestX, int32_t nDestY, double flSrcX, double flSrcY, double flSrcWidth, double flSrcHeight, int32_t nDstWidth, int32_t nDstHeight, GamescopeAppTextureColorspace eColorspace );
        void Present( const FrameInfo_t::Layer_t *pLayer );

        void Commit();

        wl_surface *GetSurface() const { return m_pSurface; }
        xdg_surface *GetXdgSurface() const { return m_pXdgSurface; }
        xdg_toplevel *GetXdgTopLevel() const { return m_pXdgToplevel; }

    private:

        void Wayland_XDGSurface_Configure( xdg_surface *pXDGSurface, uint32_t uSerial );
        static const xdg_surface_listener s_XDGSurfaceListener;

        void Wayland_XDGToplevel_Configure( xdg_toplevel *pXDGTopLevel, int32_t nWidth, int32_t nHeight, wl_array *pStates );
        void Wayland_XDGToplevel_Close( xdg_toplevel *pXDGTopLevel );
        static const xdg_toplevel_listener s_XDGToplevelListener;

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

        CWaylandBackend *m_pBackend = nullptr;

        wl_surface *m_pSurface = nullptr;
        wp_viewport *m_pViewport = nullptr;
        xdg_surface *m_pXdgSurface = nullptr;
        xdg_toplevel *m_pXdgToplevel = nullptr;
        wl_subsurface *m_pSubsurface = nullptr;
        frog_color_managed_surface *m_pFrogColorManagedSurface = nullptr;
    };
    const xdg_surface_listener CWaylandPlane::s_XDGSurfaceListener =
    {
        .configure = WAYLAND_USERDATA_TO_THIS( CWaylandPlane, Wayland_XDGSurface_Configure ),
    };
    const xdg_toplevel_listener CWaylandPlane::s_XDGToplevelListener =
    {
        .configure = WAYLAND_USERDATA_TO_THIS( CWaylandPlane, Wayland_XDGToplevel_Configure ),
        .close     = WAYLAND_USERDATA_TO_THIS( CWaylandPlane, Wayland_XDGToplevel_Close ),
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

    struct WaylandOutputInfo
    {
        int32_t nRefresh = 60;
    };

    class CWaylandBackend : public CBaseBackend, public INestedHints
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
        virtual void GetPreferredOutputFormat( VkFormat *pPrimaryPlaneFormat, VkFormat *pOverlayPlaneFormat ) const override;
        virtual bool ValidPhysicalDevice( VkPhysicalDevice pVkPhysicalDevice ) const override;

        virtual int Present( const FrameInfo_t *pFrameInfo, bool bAsync ) override;
        virtual void DirtyState( bool bForce = false, bool bForceModeset = false ) override;
        virtual bool PollState() override;

        virtual std::shared_ptr<BackendBlob> CreateBackendBlob( const std::type_info &type, std::span<const uint8_t> data ) override;

        virtual uint32_t ImportDmabufToBackend( wlr_buffer *pBuffer, wlr_dmabuf_attributes *pDmaBuf ) override;
        virtual void LockBackendFb( uint32_t uFbId ) override;
        virtual void UnlockBackendFb( uint32_t uFbId ) override;
        virtual void DropBackendFb( uint32_t uFbId ) override;
        virtual bool UsesModifiers() const override;
        virtual std::span<const uint64_t> GetSupportedModifiers( uint32_t uDrmFormat ) const override;

        virtual IBackendConnector *GetCurrentConnector() override;
        virtual IBackendConnector *GetConnector( GamescopeScreenType eScreenType ) override;

        virtual bool IsVRRActive() const override;
        virtual bool SupportsPlaneHardwareCursor() const override;

        virtual bool SupportsTearing() const override;
        virtual bool UsesVulkanSwapchain() const override;

        virtual bool IsSessionBased() const override;

        virtual bool IsVisible() const override;

        virtual glm::uvec2 CursorSurfaceSize( glm::uvec2 uvecSize ) const override;
        virtual void HackUpdatePatchedEdid() override;

        virtual INestedHints *GetNestedHints() override;

        ///////////////////
        // INestedHints
        ///////////////////

        virtual void SetCursorImage( std::shared_ptr<INestedHints::CursorInfo> info ) override;
        virtual void SetRelativeMouseMode( bool bRelative ) override;
        virtual void SetVisible( bool bVisible ) override;
        virtual void SetTitle( std::shared_ptr<std::string> szTitle ) override;
        virtual void SetIcon( std::shared_ptr<std::vector<uint32_t>> uIconPixels ) override;
        virtual std::optional<INestedHints::CursorInfo> GetHostCursor() override;
    protected:
        virtual void OnBackendBlobDestroyed( BackendBlob *pBlob ) override;

        bool SupportsColorManagement() const;
        void UpdateCursor();

        friend CWaylandConnector;
        friend CWaylandPlane;

        wl_display *GetDisplay() const { return m_pDisplay; }
        wl_registry *GetRegistry() const { return m_pRegistry; }
        wl_shm *GetShm() const { return m_pShm; }
        wl_compositor *GetCompositor() const { return m_pCompositor; }
        wp_single_pixel_buffer_manager_v1 *GetSinglePixelBufferManager() const { return m_pSinglePixelBufferManager; }
        wl_subcompositor *GetSubcompositor() const { return m_pSubcompositor; }
        zwp_linux_dmabuf_v1 *GetLinuxDmabuf() const { return m_pLinuxDmabuf; }
        xdg_wm_base *GetXDGWMBase() const { return m_pXdgWmBase; }
        wp_viewporter *GetViewporter() const { return m_pViewporter; }
        wp_presentation *GetPresentation() const { return m_pPresentation; }
        frog_color_management_factory_v1 *GetFrogColorManagementFactory() const { return m_pFrogColorMgmtFactory; }
        zwp_pointer_constraints_v1 *GetPointerConstraints() const { return m_pPointerConstraints; }
        zwp_relative_pointer_manager_v1 *GetRelativePointerManager() const { return m_pRelativePointerManager; }

        uint32_t ImportWlBuffer( wl_buffer *pBuffer );
        wl_buffer *FbIdToBuffer( uint32_t uFbId );

    private:

        void Wayland_Global( wl_registry *pRegistry, uint32_t uName, const char *pInterface, uint32_t uVersion );
        void Wayland_Modifier( zwp_linux_dmabuf_v1 *pDmabuf, uint32_t uFormat, uint32_t uModifierHi, uint32_t uModifierLo );

        void Wayland_Output_Geometry( wl_output *pOutput, int32_t nX, int32_t nY, int32_t nPhysicalWidth, int32_t nPhysicalHeight, int32_t nSubpixel, const char *pMake, const char *pModel, int32_t nTransform );
        void Wayland_Output_Mode( wl_output *pOutput, uint32_t uFlags, int32_t nWidth, int32_t nHeight, int32_t nRefresh );
        void Wayland_Output_Done( wl_output *pOutput );
        void Wayland_Output_Scale( wl_output *pOutput, int32_t nFactor );
        void Wayland_Output_Name( wl_output *pOutput, const char *pName );
        void Wayland_Output_Description( wl_output *pOutput, const char *pDescription );
        static const wl_output_listener s_OutputListener;

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

        CWaylandConnector m_Connector;
        CWaylandPlane m_Planes[8];

        wl_display *m_pDisplay = nullptr;
        wl_registry *m_pRegistry = nullptr;
        wl_shm *m_pShm = nullptr;
        wl_compositor *m_pCompositor = nullptr;
        wp_single_pixel_buffer_manager_v1 *m_pSinglePixelBufferManager = nullptr;
        wl_subcompositor *m_pSubcompositor = nullptr;
        zwp_linux_dmabuf_v1 *m_pLinuxDmabuf = nullptr;
        xdg_wm_base *m_pXdgWmBase = nullptr;
        wp_viewporter *m_pViewporter = nullptr;
        wl_buffer *m_pBlackBuffer = nullptr;
        std::shared_ptr<CVulkanTexture> m_pBlackTexture;
        wp_presentation *m_pPresentation = nullptr;
        frog_color_management_factory_v1 *m_pFrogColorMgmtFactory = nullptr;
        zwp_pointer_constraints_v1 *m_pPointerConstraints = nullptr;
        zwp_relative_pointer_manager_v1 *m_pRelativePointerManager = nullptr;
        std::unordered_map<wl_output *, WaylandOutputInfo> m_pOutputs;

        wl_seat *m_pSeat = nullptr;
        wl_keyboard *m_pKeyboard = nullptr;
        wl_pointer *m_pPointer = nullptr;
        wl_touch *m_pTouch = nullptr;
        zwp_locked_pointer_v1 *m_pLockedPointer = nullptr;
        zwp_relative_pointer_v1 *m_pRelativePointer = nullptr;

        std::unordered_map<uint32_t, std::vector<uint64_t>> m_FormatModifiers;
        std::unordered_map<uint32_t, wl_buffer *> m_ImportedFbs;

        // Track base plane surface size with last commit
        // so we are in the right scale for resizing + mouse location, etc.
        glm::uvec2 m_uSurfaceSize = { 1280, 720 };

        uint32_t m_uPointerEnterSerial = 0;
        bool m_bMouseEntered = false;
        bool m_bKeyboardEntered = false;
        std::shared_ptr<INestedHints::CursorInfo> m_pCursorInfo;
        wl_surface *m_pCursorSurface = nullptr;
    };
    const wl_seat_listener CWaylandBackend::s_SeatListener =
    {
        .capabilities = WAYLAND_USERDATA_TO_THIS( CWaylandBackend, Wayland_Seat_Capabilities ),
        .name         = WAYLAND_USERDATA_TO_THIS( CWaylandBackend, Wayland_Seat_Name ),
    };
    const wl_pointer_listener CWaylandBackend::s_PointerListener =
    {
        .enter         = WAYLAND_USERDATA_TO_THIS( CWaylandBackend, Wayland_Pointer_Enter ),
        .leave         = WAYLAND_USERDATA_TO_THIS( CWaylandBackend, Wayland_Pointer_Leave ),
        .motion        = WAYLAND_USERDATA_TO_THIS( CWaylandBackend, Wayland_Pointer_Motion ),
        .button        = WAYLAND_USERDATA_TO_THIS( CWaylandBackend, Wayland_Pointer_Button ),
        .axis          = WAYLAND_USERDATA_TO_THIS( CWaylandBackend, Wayland_Pointer_Axis ),
        .frame         = WAYLAND_USERDATA_TO_THIS( CWaylandBackend, Wayland_Pointer_Frame ),
        .axis_source   = WAYLAND_USERDATA_TO_THIS( CWaylandBackend, Wayland_Pointer_Axis_Source ),
        .axis_stop     = WAYLAND_USERDATA_TO_THIS( CWaylandBackend, Wayland_Pointer_Axis_Stop ),
        .axis_discrete = WAYLAND_USERDATA_TO_THIS( CWaylandBackend, Wayland_Pointer_Axis_Discrete ),
    };
    const wl_keyboard_listener CWaylandBackend::s_KeyboardListener =
    {
        .keymap        = WAYLAND_USERDATA_TO_THIS( CWaylandBackend, Wayland_Keyboard_Keymap ),
        .enter         = WAYLAND_USERDATA_TO_THIS( CWaylandBackend, Wayland_Keyboard_Enter ),
        .leave         = WAYLAND_USERDATA_TO_THIS( CWaylandBackend, Wayland_Keyboard_Leave ),
        .key           = WAYLAND_USERDATA_TO_THIS( CWaylandBackend, Wayland_Keyboard_Key ),
        .modifiers     = WAYLAND_USERDATA_TO_THIS( CWaylandBackend, Wayland_Keyboard_Modifiers ),
        .repeat_info   = WAYLAND_USERDATA_TO_THIS( CWaylandBackend, Wayland_Keyboard_RepeatInfo ),
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
    const zwp_relative_pointer_v1_listener CWaylandBackend::s_RelativePointerListener =
    {
        .relative_motion = WAYLAND_USERDATA_TO_THIS( CWaylandBackend, Wayland_RelativePointer_RelativeMotion ),
    };

    //////////////////
    // CWaylandConnector
    //////////////////

    CWaylandConnector::CWaylandConnector( CWaylandBackend *pBackend )
        : m_pBackend( pBackend )
    {
    }

    CWaylandConnector::~CWaylandConnector()
    {
    }

    bool CWaylandConnector::UpdateEdid()
    {
        m_FakeEdid = GenerateSimpleEdid( g_nNestedWidth, g_nNestedHeight );

        return true;
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
    std::span<const BackendMode> CWaylandConnector::GetModes() const
    {
        return std::span<const BackendMode>{};
    }

    bool CWaylandConnector::SupportsVRR() const
    {
        return false;
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
        if ( g_bForceHDR10OutputDebug )
        {
            *displayColorimetry = displaycolorimetry_2020;
            *displayEOTF = EOTF_PQ;
            *outputEncodingColorimetry = displaycolorimetry_2020;
            *outputEncodingEOTF = EOTF_PQ;
        }
        else
        {
            *displayColorimetry = displaycolorimetry_709;
            *displayEOTF = EOTF_Gamma22;
            *outputEncodingColorimetry = displaycolorimetry_709;
            *outputEncodingEOTF = EOTF_Gamma22;
        }
    }

    //////////////////
    // CWaylandPlane
    //////////////////

    CWaylandPlane::CWaylandPlane( CWaylandBackend *pBackend )
        : m_pBackend( pBackend )
    {
    }

    CWaylandPlane::~CWaylandPlane()
    {
    }

    bool CWaylandPlane::Init( CWaylandPlane *pParent, CWaylandPlane *pSiblingBelow )
    {
        m_pSurface = wl_compositor_create_surface( m_pBackend->GetCompositor() );
        m_pViewport = wp_viewporter_get_viewport( m_pBackend->GetViewporter(), m_pSurface );

        if ( m_pBackend->GetFrogColorManagementFactory() )
        {
            m_pFrogColorManagedSurface = frog_color_management_factory_v1_get_color_managed_surface( m_pBackend->GetFrogColorManagementFactory(), m_pSurface );

            // Only add the listener for the toplevel to avoid useless spam.
            if ( !pParent )
                frog_color_managed_surface_add_listener( m_pFrogColorManagedSurface, &s_FrogColorManagedSurfaceListener, this );
        }

        if ( !pParent )
        {
            m_pXdgSurface = xdg_wm_base_get_xdg_surface( m_pBackend->GetXDGWMBase(), m_pSurface );
            xdg_surface_add_listener( m_pXdgSurface, &s_XDGSurfaceListener, this );
            m_pXdgToplevel = xdg_surface_get_toplevel( m_pXdgSurface );
            xdg_toplevel_add_listener( m_pXdgToplevel, &s_XDGToplevelListener, this );
            xdg_toplevel_set_title( m_pXdgToplevel, "Gamescope" );
            xdg_toplevel_set_app_id( m_pXdgToplevel, "gamescope" );
        }
        else
        {
            m_pSubsurface = wl_subcompositor_get_subsurface( m_pBackend->GetSubcompositor(), m_pSurface, pParent->GetSurface() );
            wl_subsurface_place_above( m_pSubsurface, pSiblingBelow->GetSurface() );
            wl_subsurface_set_sync( m_pSubsurface );
        }

        wl_surface_commit( m_pSurface );
        wl_display_roundtrip( m_pBackend->GetDisplay() );

        return true;
    }

    void CWaylandPlane::Present( wl_buffer *pBuffer, int32_t nDestX, int32_t nDestY, double flSrcX, double flSrcY, double flSrcWidth, double flSrcHeight, int32_t nDstWidth, int32_t nDstHeight, GamescopeAppTextureColorspace eColorspace )
    {
        if ( pBuffer )
        {
            if ( m_pXdgToplevel )
            {
                struct wp_presentation_feedback *pFeedback = wp_presentation_feedback( m_pBackend->GetPresentation(), m_pSurface );
                wp_presentation_feedback_add_listener( pFeedback, &s_PresentationFeedbackListener, this );
            }

            if ( m_pFrogColorManagedSurface )
            {
                frog_color_managed_surface_set_render_intent( m_pFrogColorManagedSurface, FROG_COLOR_MANAGED_SURFACE_RENDER_INTENT_PERCEPTUAL );
                switch ( eColorspace )
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

            int32_t nClippedDstWidth  = std::min<int32_t>( g_nOutputWidth,  nDstWidth   + nDestX ) - nDestX;
            int32_t nClippedDstHeight = std::min<int32_t>( g_nOutputHeight, nDstHeight  + nDestY ) - nDestY;
            double flClippedSrcWidth  = flSrcWidth * ( nClippedDstWidth / double( nDstWidth ) );
            double flClippedSrcHeight = flSrcHeight * ( nClippedDstHeight / double( nDstHeight ) );

            wp_viewport_set_source(
                m_pViewport,
                wl_fixed_from_double( flSrcX ),
                wl_fixed_from_double( flSrcY ),
                wl_fixed_from_double( flClippedSrcWidth ),
                wl_fixed_from_double( flClippedSrcHeight ) );
            wp_viewport_set_destination(
                m_pViewport,
                nClippedDstWidth,
                nClippedDstHeight );
            if ( m_pSubsurface )
                wl_subsurface_set_position( m_pSubsurface, nDestX, nDestY );
            // The x/y here does nothing? Why? What is it for...
            // Use the subsurface set_position thing instead.
            wl_surface_attach( m_pSurface, pBuffer, 0, 0 );
            wl_surface_damage( m_pSurface, 0, 0, INT32_MAX, INT32_MAX );
        }
        else
        {
            if ( !m_pXdgToplevel )
            {
                // Why does this error *sometimes*, I see other apps do it!
                //wl_surface_attach( m_pSurface, nullptr, 0, 0 );
                //wl_surface_damage( m_pSurface, 0, 0, INT32_MAX, INT32_MAX );
            }
        }
    }

    void CWaylandPlane::Commit()
    {
        wl_surface_commit( m_pSurface );
    }

    void CWaylandPlane::Present( const FrameInfo_t::Layer_t *pLayer )
    {
        wl_buffer *pBuffer = pLayer ? m_pBackend->FbIdToBuffer( pLayer->fbid ) : nullptr;

        if ( pBuffer )
        {
            Present(
                pBuffer,
                -pLayer->offset.x,
                -pLayer->offset.y,
                0.0,
                0.0,
                pLayer->tex->width(),
                pLayer->tex->height(),
                pLayer->tex->width() / double( pLayer->scale.x ),
                pLayer->tex->height() / double( pLayer->scale.y ),
                pLayer->colorspace );
        }
        else
        {
            Present( nullptr, 0, 0, -1.0, -1.0, -1.0, -1.0, -1, -1, GAMESCOPE_APP_TEXTURE_COLORSPACE_PASSTHRU );
        }
    }

    void CWaylandPlane::Wayland_XDGSurface_Configure( xdg_surface *pXDGSurface, uint32_t uSerial )
    {
        xdg_surface_ack_configure( pXDGSurface, uSerial );
    }

    void CWaylandPlane::Wayland_XDGToplevel_Configure( xdg_toplevel *pXDGTopLevel, int32_t nWidth, int32_t nHeight, wl_array *pStates )
    {
        // Compositor is deferring to us.
        if ( nWidth == 0 || nHeight == 0 )
            return;

        g_nOutputWidth = std::max( nWidth, 1 );
        g_nOutputHeight = std::max( nHeight, 1 );
    }

    void CWaylandPlane::Wayland_XDGToplevel_Close( xdg_toplevel *pXDGTopLevel )
    {
        raise( SIGTERM );
    }

    void CWaylandPlane::Wayland_PresentationFeedback_SyncOutput( struct wp_presentation_feedback *pFeedback, wl_output *pOutput )
    {
    }
    void CWaylandPlane::Wayland_PresentationFeedback_Presented( struct wp_presentation_feedback *pFeedback, uint32_t uTVSecHi, uint32_t uTVSecLo, uint32_t uTVNSec, uint32_t uRefresh, uint32_t uSeqHi, uint32_t uSeqLo, uint32_t uFlags )
    {
        uint64_t ulTime = ( ( ( uint64_t( uTVSecHi ) << 32ul ) | uTVSecLo ) * 1'000'000'000lu ) +
                          ( uint64_t( uTVNSec ) );

        // TODO: Someday move g_nOutputRefresh to MHz...
        int nRefresh = ( 1'000'000'000 + uRefresh - 1 ) / uRefresh;
        if ( nRefresh && nRefresh != g_nOutputRefresh )
        {
            xdg_log.infof( "Changed refresh to: %d", nRefresh );
            g_nOutputRefresh = nRefresh;
        }

        GetVBlankTimer().MarkVBlank( ulTime, true );
        wp_presentation_feedback_destroy( pFeedback );
    }
    void CWaylandPlane::Wayland_PresentationFeedback_Discarded( struct wp_presentation_feedback *pFeedback )
    {
        wp_presentation_feedback_destroy( pFeedback );
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
        auto *pHDRInfo = &m_pBackend->m_Connector.m_HDRInfo;
        pHDRInfo->bExposeHDRSupport         = ( cv_hdr_enabled && uTransferFunction == FROG_COLOR_MANAGED_SURFACE_TRANSFER_FUNCTION_ST2084_PQ );
        pHDRInfo->eOutputEncodingEOTF       = ( cv_hdr_enabled && uTransferFunction == FROG_COLOR_MANAGED_SURFACE_TRANSFER_FUNCTION_ST2084_PQ ) ? EOTF_PQ : EOTF_Gamma22;
        pHDRInfo->uMaxContentLightLevel     = uMaxLuminance;
        pHDRInfo->uMaxFrameAverageLuminance = uMaxFullFrameLuminance;
        pHDRInfo->uMinContentLightLevel     = uMinLuminance;

        xdg_log.infof( "PreferredMetadata: Red: %g %g, Green: %g %g, Blue: %g %g, White: %g %g, Max Luminance: %u nits, Min Luminance: %g nits, Max Full Frame Luminance: %u nits",
            uOutputDisplayPrimaryRedX * 0.00002, uOutputDisplayPrimaryRedY * 0.00002,
            uOutputDisplayPrimaryGreenX * 0.00002, uOutputDisplayPrimaryGreenY * 0.00002,
            uOutputDisplayPrimaryBlueX * 0.00002, uOutputDisplayPrimaryBlueY * 0.00002,
            uOutputWhitePointX * 0.00002, uOutputWhitePointY * 0.00002,
            uint32_t( uMaxLuminance ),
            uMinLuminance * 0.0001,
            uint32_t( uMaxFullFrameLuminance ) );
    }

    ////////////////
    // CWaylandBackend
    ////////////////

    CWaylandBackend::CWaylandBackend()
        : m_Connector( this )
        , m_Planes{ this, this, this, this, this, this, this, this }
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
            g_nOutputRefresh = 60;

        m_uSurfaceSize.x = g_nOutputWidth;
        m_uSurfaceSize.y = g_nOutputHeight;

        if ( !( m_pDisplay = wl_display_connect( nullptr ) ) )
        {
            xdg_log.errorf( "Couldn't connect to Wayland display." );
            return false;
        }

        if ( !( m_pRegistry = wl_display_get_registry( m_pDisplay ) ) )
        {
            xdg_log.errorf( "Couldn't create Wayland registry." );
            return false;
        }

        static constexpr wl_registry_listener s_RegistryListener =
        {
            .global = []( void *pData, wl_registry *pRegistry, uint32_t uName, const char *pInterface, uint32_t uVersion )
            {
                CWaylandBackend *pState = (CWaylandBackend *)pData;
                pState->Wayland_Global( pRegistry, uName, pInterface, uVersion );
            },
            .global_remove = []( void *pData, wl_registry *pRegistry, uint32_t uName )
            {
            },
        };
        wl_registry_add_listener( m_pRegistry, &s_RegistryListener, this );
        wl_display_roundtrip( m_pDisplay );

        if ( !m_pCompositor || !m_pSubcompositor || !m_pXdgWmBase || !m_pLinuxDmabuf || !m_pViewporter || !m_pPresentation || !m_pRelativePointerManager || !m_pPointerConstraints || !m_pShm )
        {
            xdg_log.errorf( "Couldn't create Wayland objects." );
            return false;
        }

        // Grab stuff from any extra bindings/listeners we set up, eg. format/modifiers.
        wl_display_roundtrip( m_pDisplay );

        for ( uint32_t i = 0; i < 8; i++ )
            m_Planes[i].Init( i == 0 ? nullptr : &m_Planes[0], i == 0 ? nullptr : &m_Planes[ i - 1 ] );

        if ( !vulkan_init( vulkan_get_instance(), VK_NULL_HANDLE ) )
        {
            return false;
        }

        if ( !wlsession_init() )
        {
            fprintf( stderr, "Failed to initialize Wayland session\n" );
            return false;
        }

        return true;
    }

    bool CWaylandBackend::PostInit()
    {
        m_Connector.UpdateEdid();
        this->HackUpdatePatchedEdid();

        if ( cv_hdr_enabled && m_Connector.GetHDRInfo().bExposeHDRSupport )
        {
            setenv( "DXVK_HDR", "1", false );
        }

        if ( m_pSinglePixelBufferManager )
        {
            m_pBlackBuffer = wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer( m_pSinglePixelBufferManager, 0, 0, 0, ~0u );
        }
        else
        {
            m_pBlackTexture = vulkan_create_flat_texture( 1, 1, 0, 0, 0, 255 );
            if ( !m_pBlackTexture )
            {
                xdg_log.errorf( "Failed to create dummy black texture." );
                return false;
            }
            m_pBlackBuffer = FbIdToBuffer( m_pBlackTexture->fbid() );
        }

        if ( !m_pBlackBuffer )
        {
            xdg_log.errorf( "Failed to create 1x1 black buffer." );
            return false;
        }

        if ( g_bForceRelativeMouse )
            this->SetRelativeMouseMode( true );

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

    void CWaylandBackend::GetPreferredOutputFormat( VkFormat *pPrimaryPlaneFormat, VkFormat *pOverlayPlaneFormat ) const
    {
        *pPrimaryPlaneFormat = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        *pOverlayPlaneFormat = VK_FORMAT_B8G8R8A8_UNORM;
    }

    bool CWaylandBackend::ValidPhysicalDevice( VkPhysicalDevice pVkPhysicalDevice ) const
    {
        return true;
    }

    int CWaylandBackend::Present( const FrameInfo_t *pFrameInfo, bool bAsync )
    {
        // TODO: Dedupe some of this composite check code between us and drm.cpp
        bool bLayer0ScreenSize = close_enough(pFrameInfo->layers[0].scale.x, 1.0f) && close_enough(pFrameInfo->layers[0].scale.y, 1.0f);

        bool bNeedsCompositeFromFilter = (g_upscaleFilter == GamescopeUpscaleFilter::NEAREST || g_upscaleFilter == GamescopeUpscaleFilter::PIXEL) && !bLayer0ScreenSize;

        bool bNeedsFullComposite = false;
        bNeedsFullComposite |= alwaysComposite;
        bNeedsFullComposite |= pFrameInfo->useFSRLayer0;
        bNeedsFullComposite |= pFrameInfo->useNISLayer0;
        bNeedsFullComposite |= pFrameInfo->blurLayer0;
        bNeedsFullComposite |= bNeedsCompositeFromFilter;
        bNeedsFullComposite |= g_bColorSliderInUse;
        bNeedsFullComposite |= pFrameInfo->bFadingOut;
        bNeedsFullComposite |= !g_reshade_effect.empty();

        if ( g_bOutputHDREnabled )
            bNeedsFullComposite |= g_bHDRItmEnable;

        if ( !SupportsColorManagement() )
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
                m_Planes[uCurrentPlane++].Present( m_pBlackBuffer, 0, 0, 0.0, 0.0, 1.0, 1.0, g_nOutputWidth, g_nOutputHeight, GAMESCOPE_APP_TEXTURE_COLORSPACE_PASSTHRU );

            for ( int i = 0; i < 8 && uCurrentPlane < 8; i++ )
                m_Planes[uCurrentPlane++].Present( i < pFrameInfo->layerCount ? &pFrameInfo->layers[i] : nullptr );

            if ( bNeedsBacking )
            {
                m_uSurfaceSize = glm::uvec2{ g_nOutputWidth, g_nOutputHeight };
            }
            else
            {
                m_uSurfaceSize = glm::uvec2{ pFrameInfo->layers[0].tex->width(), pFrameInfo->layers[0].tex->height() };
            }
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
            compositeLayer.fbid = compositeLayer.tex->fbid();
            compositeLayer.applyColorMgmt = false;

            compositeLayer.filter = GamescopeUpscaleFilter::NEAREST;
            compositeLayer.ctm = nullptr;
            compositeLayer.colorspace = pFrameInfo->outputEncodingEOTF == EOTF_PQ ? GAMESCOPE_APP_TEXTURE_COLORSPACE_HDR10_PQ : GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB;

            m_Planes[0].Present( &compositeLayer );

            m_uSurfaceSize = glm::uvec2{ g_nOutputWidth, g_nOutputHeight };
        }

        for ( int i = 7; i >= 0; i-- )
            m_Planes[i].Commit();

        wl_display_flush( m_pDisplay );

        this->PollState();

        return 0;
    }
    void CWaylandBackend::DirtyState( bool bForce, bool bForceModeset )
    {
    }
    bool CWaylandBackend::PollState()
    {
        wl_display_dispatch_pending( m_pDisplay );
        wl_display_prepare_read( m_pDisplay );
        wl_display_read_events( m_pDisplay );
        wl_display_dispatch_pending( m_pDisplay );

        return false;
    }

    std::shared_ptr<BackendBlob> CWaylandBackend::CreateBackendBlob( const std::type_info &type, std::span<const uint8_t> data )
    {
        return std::make_shared<BackendBlob>( data );
    }

    uint32_t CWaylandBackend::ImportDmabufToBackend( wlr_buffer *pClientBuffer, wlr_dmabuf_attributes *pDmaBuf )
    {
        zwp_linux_buffer_params_v1 *pBufferParams = zwp_linux_dmabuf_v1_create_params( m_pLinuxDmabuf );
        if ( !pBufferParams )
        {
            xdg_log.errorf( "Failed to create imported dmabuf params" );
            return 0;
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
            return 0;
        }

        zwp_linux_buffer_params_v1_destroy( pBufferParams );

        return ImportWlBuffer( pImportedBuffer );
    }
    void CWaylandBackend::LockBackendFb( uint32_t uFbId )
    {
        // No need to refcount on this backend.
    }
    void CWaylandBackend::UnlockBackendFb( uint32_t uFbId )
    {
        // No need to refcount on this backend.
    }
    void CWaylandBackend::DropBackendFb( uint32_t uFbId )
    {
        assert( m_ImportedFbs.contains( uFbId ) );
        auto iter = m_ImportedFbs.find( uFbId );
        wl_buffer_destroy( iter->second );
        m_ImportedFbs.erase( iter );
    }

    bool CWaylandBackend::UsesModifiers() const
    {
        return true;
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
        return &m_Connector;
    }
    IBackendConnector *CWaylandBackend::GetConnector( GamescopeScreenType eScreenType )
    {
        if ( eScreenType == GAMESCOPE_SCREEN_TYPE_INTERNAL )
            return &m_Connector;

        return nullptr;
    }
    bool CWaylandBackend::IsVRRActive() const
    {
        return false;
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

        WritePatchedEdid( GetCurrentConnector()->GetRawEDID(), GetCurrentConnector()->GetHDRInfo(), false );
    }

    INestedHints *CWaylandBackend::GetNestedHints()
    {
        return this;
    }

    ///////////////////
    // INestedHints
    ///////////////////

    static int CreateShmBuffer( uint32_t uSize )
    {
        static constexpr char szTemplate[] = "/gamescope-shared-XXXXXX";

        const char *pXDGPath = getenv( "XDG_RUNTIME_DIR" );
        if ( !pXDGPath || !*pXDGPath )
            return -1;

        char szPath[ PATH_MAX ];
        snprintf( szPath, PATH_MAX, "%s%s", pXDGPath, szTemplate );

        int nFd = mkostemp( szPath, O_CLOEXEC );
        if ( nFd < 0 )
            return -1;

        if ( ftruncate( nFd, uSize ) < 0 )
        {
            close( nFd );
            return -1;
        }

        return nFd;
    }

    void CWaylandBackend::SetCursorImage( std::shared_ptr<INestedHints::CursorInfo> info )
    {
        m_pCursorInfo = info;
        if ( !m_pPointer )
            return;

        uint32_t uStride = info->uWidth * 4;
        uint32_t uSize = uStride * info->uHeight;

        int32_t nFd = CreateShmBuffer( uSize );
        if ( nFd < 0 )
            return;
        defer( close( nFd ) );

        void *pData = mmap( nullptr, uSize, PROT_READ | PROT_WRITE, MAP_SHARED, nFd, 0 );
        if ( pData == MAP_FAILED )
            return;

        memcpy( pData, info->pPixels.data(), uSize );

        wl_shm_pool *pPool = wl_shm_create_pool( m_pShm, nFd, uSize );
        defer( wl_shm_pool_destroy( pPool ) );

        wl_buffer *pBuffer = wl_shm_pool_create_buffer( pPool, 0, info->uWidth, info->uHeight, uStride, WL_SHM_FORMAT_ARGB8888 );
        defer( wl_buffer_destroy( pBuffer ) );

        m_pCursorSurface = wl_compositor_create_surface( m_pCompositor );
        wl_surface_attach( m_pCursorSurface, pBuffer, 0, 0 );
        wl_surface_damage( m_pCursorSurface, 0, 0, INT32_MAX, INT32_MAX );
        wl_surface_commit( m_pCursorSurface );

        UpdateCursor();
    }
    void CWaylandBackend::SetRelativeMouseMode( bool bRelative )
    {
        if ( !m_pPointer )
            return;

        if ( !!bRelative != !!m_pLockedPointer )
        {
            if ( m_pLockedPointer )
            {
                assert( m_pRelativePointer );

                zwp_locked_pointer_v1_destroy( m_pLockedPointer );
                m_pLockedPointer = nullptr;

                zwp_relative_pointer_v1_destroy( m_pRelativePointer );
                m_pRelativePointer = nullptr;
            }
            else
            {
                assert( !m_pRelativePointer );

                m_pLockedPointer = zwp_pointer_constraints_v1_lock_pointer( m_pPointerConstraints, m_Planes[0].GetSurface(), m_pPointer, nullptr, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT );
                m_pRelativePointer = zwp_relative_pointer_manager_v1_get_relative_pointer( m_pRelativePointerManager, m_pPointer );
                zwp_relative_pointer_v1_add_listener( m_pRelativePointer, &s_RelativePointerListener, this );
            }

            UpdateCursor();
        }
    }
    void CWaylandBackend::SetVisible( bool bVisible )
    {
    }
    void CWaylandBackend::SetTitle( std::shared_ptr<std::string> pAppTitle )
    {
        std::string szTitle = pAppTitle ? *pAppTitle : "gamescope";
        if ( g_bGrabbed )
            szTitle += " (grabbed)";
        xdg_toplevel_set_title( m_Planes[0].GetXdgTopLevel(), szTitle.c_str() );
    }
    void CWaylandBackend::SetIcon( std::shared_ptr<std::vector<uint32_t>> uIconPixels )
    {
        // Oh gee, it'd be so cool if we had something to do this...
        // *cough cough*
        // *cough cough*
        // @_@ c'mon guys
    }

    extern std::optional<INestedHints::CursorInfo> GetX11HostCursor();

    std::optional<INestedHints::CursorInfo> CWaylandBackend::GetHostCursor()
    {
        return GetX11HostCursor();
    }

    void CWaylandBackend::OnBackendBlobDestroyed( BackendBlob *pBlob )
    {
        // Do nothing.
    }

    bool CWaylandBackend::SupportsColorManagement() const
    {
        return m_pFrogColorMgmtFactory != nullptr;
    }

    void CWaylandBackend::UpdateCursor()
    {
        bool bHideCursor = m_pLockedPointer || !m_pCursorSurface;

        if ( bHideCursor )
            wl_pointer_set_cursor( m_pPointer, m_uPointerEnterSerial, nullptr, 0, 0 );
        else
            wl_pointer_set_cursor( m_pPointer, m_uPointerEnterSerial, m_pCursorSurface, m_pCursorInfo->uXHotspot, m_pCursorInfo->uYHotspot );
    }

    //

    uint32_t CWaylandBackend::ImportWlBuffer( wl_buffer *pBuffer )
    {
        static uint32_t s_uFbIdCounter = 0;
        uint32_t uFbId = ++s_uFbIdCounter;
        // Handle wraparound incase we ever have 4mil surfaces lol.
        if ( uFbId == 0 )
            uFbId++;

        m_ImportedFbs[uFbId] = pBuffer;
        return uFbId;
    }

    wl_buffer *CWaylandBackend::FbIdToBuffer( uint32_t uFbId )
    {
        if ( uFbId == 0 )
            return nullptr;

        assert( m_ImportedFbs.contains( uFbId ) );
        return m_ImportedFbs[uFbId];
    }

    /////////////////////
    // Wayland Callbacks
    /////////////////////

    void CWaylandBackend::Wayland_Global( wl_registry *pRegistry, uint32_t uName, const char *pInterface, uint32_t uVersion )
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
        else if ( !strcmp( pInterface, wl_seat_interface.name ) )
        {
            m_pSeat = (wl_seat *)wl_registry_bind( m_pRegistry, uName, &wl_seat_interface, 7 );
            wl_seat_add_listener( m_pSeat, &s_SeatListener, this );
        }
        else if ( !strcmp( pInterface, wp_presentation_interface.name ) )
        {
            m_pPresentation = (wp_presentation *)wl_registry_bind( m_pRegistry, uName, &wp_presentation_interface, 1u );
        }
        else if ( !strcmp( pInterface, wl_output_interface.name ) )
        {
            wl_output *pOutput  = (wl_output *)wl_registry_bind( m_pRegistry, uName, &wl_output_interface, 4u );
            wl_output_add_listener( pOutput , &s_OutputListener, this );
            m_pOutputs.emplace( std::make_pair<struct wl_output *, WaylandOutputInfo>( std::move( pOutput ), WaylandOutputInfo{} ) );
        }
        else if ( !strcmp( pInterface, frog_color_management_factory_v1_interface.name ) )
        {
            m_pFrogColorMgmtFactory = (frog_color_management_factory_v1 *)wl_registry_bind( m_pRegistry, uName, &frog_color_management_factory_v1_interface, 1u );
        }
        else if ( !strcmp( pInterface, zwp_pointer_constraints_v1_interface.name ) )
        {
            m_pPointerConstraints = (zwp_pointer_constraints_v1 *)wl_registry_bind( m_pRegistry, uName, &zwp_pointer_constraints_v1_interface, 1u );
        }
        else if ( !strcmp( pInterface, zwp_relative_pointer_manager_v1_interface.name ) )
        {
            m_pRelativePointerManager = (zwp_relative_pointer_manager_v1 *)wl_registry_bind( m_pRegistry, uName, &zwp_relative_pointer_manager_v1_interface, 1u );
        }
        else if ( !strcmp( pInterface, wl_shm_interface.name ) )
        {
            m_pShm = (wl_shm *)wl_registry_bind( m_pRegistry, uName, &wl_shm_interface, 1u );
        }
    }

    void CWaylandBackend::Wayland_Modifier( zwp_linux_dmabuf_v1 *pDmabuf, uint32_t uFormat, uint32_t uModifierHi, uint32_t uModifierLo )
    {
        uint64_t ulModifier = ( uint64_t( uModifierHi ) << 32 ) | uModifierLo;
        //xdg_log.infof( "Modifier: %s (0x%" PRIX32 ") %lx", drmGetFormatName( uFormat ), uFormat, ulModifier );
        if ( ulModifier != DRM_FORMAT_MOD_INVALID )
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

    void CWaylandBackend::Wayland_Seat_Name( wl_seat *pSeat, const char *pName )
    {
        xdg_log.infof( "Seat name: %s", pName );
    }

    // Pointer

    void CWaylandBackend::Wayland_Pointer_Enter( wl_pointer *pPointer, uint32_t uSerial, wl_surface *pSurface, wl_fixed_t fSurfaceX, wl_fixed_t fSurfaceY )
    {
        m_bMouseEntered = true;
        m_uPointerEnterSerial = uSerial;
        UpdateCursor();
    }
    void CWaylandBackend::Wayland_Pointer_Leave( wl_pointer *pPointer, uint32_t uSerial, wl_surface *pSurface )
    {
        m_bMouseEntered = false;
    }
    void CWaylandBackend::Wayland_Pointer_Motion( wl_pointer *pPointer, uint32_t uTime, wl_fixed_t fSurfaceX, wl_fixed_t fSurfaceY )
    {
        if ( m_pRelativePointer )
            return;

        // Don't do any motion/movement stuff if we don't have kb focus
        if ( !m_bKeyboardEntered )
            return;

        wlserver_lock();
        wlserver_touchmotion( wl_fixed_to_double( fSurfaceX ) / double( m_uSurfaceSize.x ), wl_fixed_to_double( fSurfaceY ) / double( m_uSurfaceSize.y ), 0, uTime );
        wlserver_unlock();
    }
    void CWaylandBackend::Wayland_Pointer_Button( wl_pointer *pPointer, uint32_t uSerial, uint32_t uTime, uint32_t uButton, uint32_t uState )
    {
        // Don't do any motion/movement stuff if we don't have kb focus
        if ( !m_bKeyboardEntered )
            return;

        wlserver_lock();
        wlserver_mousebutton( uButton, uState == WL_POINTER_BUTTON_STATE_PRESSED, uTime );
        wlserver_unlock();
    }
    void CWaylandBackend::Wayland_Pointer_Axis( wl_pointer *pPointer, uint32_t uTime, uint32_t uAxis, wl_fixed_t fValue )
    {
        // XXX(JoshA): TODO scroll wheel, there's so much stuff!
    }
    void CWaylandBackend::Wayland_Pointer_Axis_Source( wl_pointer *pPointer, uint32_t uAxisSource )
    {
    }
    void CWaylandBackend::Wayland_Pointer_Axis_Stop( wl_pointer *pPointer, uint32_t uTime, uint32_t uAxis )
    {
    }
    void CWaylandBackend::Wayland_Pointer_Axis_Discrete( wl_pointer *pPointer, uint32_t uAxis, int32_t nDiscrete )
    {
    }
    void CWaylandBackend::Wayland_Pointer_Frame( wl_pointer *pPointer )
    {
        // We could handle it here....... but why?
    }

    // Keyboard

    void CWaylandBackend::Wayland_Keyboard_Keymap( wl_keyboard *pKeyboard, uint32_t uFormat, int32_t nFd, uint32_t uSize )
    {
        // We are not doing anything with the keymap, we pass keycodes thru.
        // Ideally we'd use this to influence our keymap to clients, eg. x server.
    }
    void CWaylandBackend::Wayland_Keyboard_Enter( wl_keyboard *pKeyboard, uint32_t uSerial, wl_surface *pSurface, wl_array *pKeys )
    {
        m_bKeyboardEntered = true;
    }
    void CWaylandBackend::Wayland_Keyboard_Leave( wl_keyboard *pKeyboard, uint32_t uSerial, wl_surface *pSurface )
    {
        m_bKeyboardEntered = false;
    }
    void CWaylandBackend::Wayland_Keyboard_Key( wl_keyboard *pKeyboard, uint32_t uSerial, uint32_t uTime, uint32_t uKey, uint32_t uState )
    {
        if ( !m_bKeyboardEntered )
            return;

        wlserver_lock();
        wlserver_key( uKey, uState == WL_KEYBOARD_KEY_STATE_PRESSED, uTime );
        wlserver_unlock();
    }
    void CWaylandBackend::Wayland_Keyboard_Modifiers( wl_keyboard *pKeyboard, uint32_t uSerial, uint32_t uModsDepressed, uint32_t uModsLatched, uint32_t uModsLocked, uint32_t uGroup )
    {
        // This seems to already be handled fine by xserver getting ctrl keycode etc.
        // Come back to me if there is something that doesn't work as expected.
    }
    void CWaylandBackend::Wayland_Keyboard_RepeatInfo( wl_keyboard *pKeyboard, int32_t nRate, int32_t nDelay )
    {
    }

    // Relative Pointer

    void CWaylandBackend::Wayland_RelativePointer_RelativeMotion( zwp_relative_pointer_v1 *pRelativePointer, uint32_t uTimeHi, uint32_t uTimeLo, wl_fixed_t fDx, wl_fixed_t fDy, wl_fixed_t fDxUnaccel, wl_fixed_t fDyUnaccel )
    {
        // Don't do any motion/movement stuff if we don't have kb focus
        if ( !m_bKeyboardEntered )
            return;

        wlserver_lock();
        wlserver_mousemotion( wl_fixed_to_double( fDxUnaccel ), wl_fixed_to_double( fDyUnaccel ), uTimeLo );
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
