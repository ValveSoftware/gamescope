#include <vector>
#include <memory>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <linux/input-event-codes.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#include <openvr.h>
#pragma GCC diagnostic pop

#include "backend.h"
#include "main.hpp"
#include "openvr.h"
#include "steamcompmgr.hpp"
#include "wlserver.hpp"
#include "log.hpp"
#include "ime.hpp"
#include "refresh_rate.h"
#include "edid.h"
#include "Ratio.h"

#include <signal.h>
#include <string.h>
#include <thread>
#include <mutex>

struct wlserver_input_method;

extern bool steamMode;
extern int g_argc;
extern char **g_argv;

extern int g_nPreferredOutputWidth;
extern int g_nPreferredOutputHeight;

extern int g_nPreferredOutputWidth;
extern int g_nPreferredOutputHeight;
extern bool g_bForceHDR10OutputDebug;
extern bool g_bBorderlessOutputWindow;

extern gamescope::ConVar<bool> cv_composite_force;
extern bool g_bColorSliderInUse;
extern bool fadingOut;
extern std::string g_reshade_effect;

extern gamescope::ConVar<bool> cv_hdr_enabled;

extern uint64_t g_SteamCompMgrLimitedAppRefreshCycle;

static LogScope openvr_log("openvr");

static bool GetVulkanInstanceExtensionsRequired( std::vector< std::string > &outInstanceExtensionList );
static bool GetVulkanDeviceExtensionsRequired( VkPhysicalDevice pPhysicalDevice, std::vector< std::string > &outDeviceExtensionList );

gamescope::ConVar<bool> cv_vr_always_warp_cursor( "vr_always_warp_cursor", true, "Whether or not we should always warp the cursor, even if it is invisible so we get hover events." );
gamescope::ConVar<bool> cv_vr_use_modifiers( "vr_use_modifiers", true, "Use DMA-BUF modifiers?" );
gamescope::ConVar<bool> cv_vr_transparent_backing( "vr_transparent_backing", true, "Should backing be transparent or not?" );
gamescope::ConVar<bool> cv_vr_use_window_icons( "vr_use_window_icons", true, "Should we use window icons if they are available?" );
gamescope::ConVar<bool> cv_vr_trackpad_hide_laser( "vr_trackpad_hide_laser", false, "Hide laser mouse when we are in trackpad mode." );
gamescope::ConVar<bool> cv_vr_trackpad_relative_mouse_mode( "vr_trackpad_relative_mouse_mode", true, "If we are in relative mouse mode, treat the screen like a big trackpad?" );
gamescope::ConVar<float> cv_vr_trackpad_sensitivity( "vr_trackpad_sensitivity", 1500.f, "Sensitivity for VR Trackpad Mode" );
gamescope::ConVar<uint64_t> cv_vr_trackpad_click_time( "vr_trackpad_click_time", 250'000'000ul, "Time to consider a 'click' vs a 'drag' when using trackpad mode. In nanoseconds." );
gamescope::ConVar<float> cv_vr_trackpad_click_max_delta( "vr_trackpad_click_max_delta", 0.14f, "Max amount the cursor can move before not clicking." );

// Just below half of 120Hz, so we always at least poll input once per frame, regardless of cadence/cycles.
gamescope::ConVar<uint64_t> cv_vr_poll_rate( "vr_poll_rate", 4'000'000ul, "Time between input polls. In nanoseconds." );

// Not in public headers yet.
namespace vr
{
    const VROverlayFlags VROverlayFlags_EnableControlBarSteamUI = (VROverlayFlags)(1 << 26);

    const EVRButtonId k_EButton_Steam = (EVRButtonId)(50);
    const EVRButtonId k_EButton_QAM = (EVRButtonId)(51);
}

///////////////////////////////////////////////
// Josh:
// GetVulkanInstanceExtensionsRequired and GetVulkanDeviceExtensionsRequired return *space separated* exts :(
// I am too lazy to write that myself.
// This is stolen verbatim from hellovr_vulkan with the .clear removed.
// If it is broken, blame the samples.

static bool GetVulkanInstanceExtensionsRequired( std::vector< std::string > &outInstanceExtensionList )
{
    if ( !vr::VRCompositor() )
    {
        openvr_log.errorf( "GetVulkanInstanceExtensionsRequired: Failed to get VRCompositor" );
        return false;
    }

    uint32_t nBufferSize = vr::VRCompositor()->GetVulkanInstanceExtensionsRequired( nullptr, 0 );
    if ( nBufferSize > 0 )
    {
        // Allocate memory for the space separated list and query for it
        char *pExtensionStr = new char[ nBufferSize ];
        pExtensionStr[0] = 0;
        vr::VRCompositor()->GetVulkanInstanceExtensionsRequired( pExtensionStr, nBufferSize );

        // Break up the space separated list into entries on the CUtlStringList
        std::string curExtStr;
        uint32_t nIndex = 0;
        while ( pExtensionStr[ nIndex ] != 0 && ( nIndex < nBufferSize ) )
        {
            if ( pExtensionStr[ nIndex ] == ' ' )
            {
                outInstanceExtensionList.push_back( curExtStr );
                curExtStr.clear();
            }
            else
            {
                curExtStr += pExtensionStr[ nIndex ];
            }
            nIndex++;
        }
        if ( curExtStr.size() > 0 )
        {
            outInstanceExtensionList.push_back( curExtStr );
        }

        delete [] pExtensionStr;
    }

    return true;
}

static bool GetVulkanDeviceExtensionsRequired( VkPhysicalDevice pPhysicalDevice, std::vector< std::string > &outDeviceExtensionList )
{
    if ( !vr::VRCompositor() )
    {
        openvr_log.errorf( "GetVulkanDeviceExtensionsRequired: Failed to get VRCompositor" );
        return false;
    }

    uint32_t nBufferSize = vr::VRCompositor()->GetVulkanDeviceExtensionsRequired( ( VkPhysicalDevice_T * ) pPhysicalDevice, nullptr, 0 );
    if ( nBufferSize > 0 )
    {
        // Allocate memory for the space separated list and query for it
        char *pExtensionStr = new char[ nBufferSize ];
        pExtensionStr[0] = 0;
        vr::VRCompositor()->GetVulkanDeviceExtensionsRequired( ( VkPhysicalDevice_T * ) pPhysicalDevice, pExtensionStr, nBufferSize );

        // Break up the space separated list into entries on the CUtlStringList
        std::string curExtStr;
        uint32_t nIndex = 0;
        while ( pExtensionStr[ nIndex ] != 0 && ( nIndex < nBufferSize ) )
        {
            if ( pExtensionStr[ nIndex ] == ' ' )
            {
                outDeviceExtensionList.push_back( curExtStr );
                curExtStr.clear();
            }
            else
            {
                curExtStr += pExtensionStr[ nIndex ];
            }
            nIndex++;
        }
        if ( curExtStr.size() > 0 )
        {
            outDeviceExtensionList.push_back( curExtStr );
        }

        delete [] pExtensionStr;
    }

    return true;
}

namespace gamescope
{
    class CVROverlayConnector final : public IBackendConnector
    {
    public:

        //////////////////////
        // IBackendConnector
        //////////////////////

        CVROverlayConnector()
        {
        }
        virtual ~CVROverlayConnector()
        {
        }

        virtual GamescopeScreenType GetScreenType() const override
        {
            return GAMESCOPE_SCREEN_TYPE_INTERNAL;
        }
        virtual GamescopePanelOrientation GetCurrentOrientation() const override
        {
            return GAMESCOPE_PANEL_ORIENTATION_0;
        }
        virtual bool SupportsHDR() const override
        {
            return false;
        }
        virtual bool IsHDRActive() const override
        {
            return false;
        }
        virtual const BackendConnectorHDRInfo &GetHDRInfo() const override
        {
            return m_HDRInfo;
        }
        virtual std::span<const BackendMode> GetModes() const override
        {
            return std::span<const BackendMode>{};
        }

        virtual bool SupportsVRR() const override
        {
            return false;
        }

        virtual std::span<const uint8_t> GetRawEDID() const override
        {
            return std::span<const uint8_t>{ m_FakeEdid.begin(), m_FakeEdid.end() };
        }
        virtual std::span<const uint32_t> GetValidDynamicRefreshRates() const override
        {
            return std::span<const uint32_t>{};
        }

        virtual void GetNativeColorimetry(
            bool bHDR10,
            displaycolorimetry_t *displayColorimetry, EOTF *displayEOTF,
            displaycolorimetry_t *outputEncodingColorimetry, EOTF *outputEncodingEOTF ) const override
        {
			*displayColorimetry = displaycolorimetry_709;
			*displayEOTF = EOTF_Gamma22;
			*outputEncodingColorimetry = displaycolorimetry_709;
			*outputEncodingEOTF = EOTF_Gamma22;
        }

        virtual const char *GetName() const override
        {
            return "OpenVR";
        }
        virtual const char *GetMake() const override
        {
            return "Gamescope";
        }
        virtual const char *GetModel() const override
        {
            return "Virtual Display";
        }

        bool UpdateEdid()
        {
            m_FakeEdid = GenerateSimpleEdid( g_nNestedWidth, g_nNestedHeight );

            return true;
        }

    private:
        BackendConnectorHDRInfo m_HDRInfo{};
        std::vector<uint8_t> m_FakeEdid;
    };


    class COpenVRBackend;

    class COpenVRFb final : public CBaseBackendFb
    {
    public:
        COpenVRFb( COpenVRBackend *pBackend, vr::SharedTextureHandle_t ulHandle );
        ~COpenVRFb();

        vr::SharedTextureHandle_t GetSharedTextureHandle() const { return m_ulHandle; }
    private:
        COpenVRBackend *m_pBackend = nullptr;
        vr::SharedTextureHandle_t m_ulHandle = 0;
    };

    // TODO: Merge with WaylandPlaneState
    struct OpenVRPlaneState
    {
        CVulkanTexture *pTexture;
        int32_t nDestX;
        int32_t nDestY;
        double flSrcX;
        double flSrcY;
        double flSrcWidth;
        double flSrcHeight;
        int32_t nDstWidth;
        int32_t nDstHeight;
        GamescopeAppTextureColorspace eColorspace;
        bool bOpaque;
        float flAlpha = 1.0f;
    };

    class COpenVRPlane
    {
    public:
        COpenVRPlane( COpenVRBackend *pBackend );
        ~COpenVRPlane();

        bool Init( COpenVRPlane *pParent, COpenVRPlane *pSiblingBelow );

        void Present( std::optional<OpenVRPlaneState> oState );
        void Present( const FrameInfo_t::Layer_t *pLayer );

        vr::VROverlayHandle_t GetOverlay() const { return m_hOverlay; }
        vr::VROverlayHandle_t GetOverlayThumbnail() const { return m_hOverlayThumbnail; }

        uint32_t GetSortOrder() const { return m_uSortOrder; }
        bool IsSubview() const { return m_bIsSubview; }

    private:
        COpenVRBackend *m_pBackend = nullptr;

        bool m_bIsSubview = false;
        uint32_t m_uSortOrder = 0;
        vr::VROverlayHandle_t m_hOverlay = vr::k_ulOverlayHandleInvalid;
        vr::VROverlayHandle_t m_hOverlayThumbnail = vr::k_ulOverlayHandleInvalid;
    };


	class COpenVRBackend final : public CBaseBackend, public INestedHints
	{
	public:
		COpenVRBackend()
            : m_Planes{ this, this, this, this, this, this, this, this }
		{
		}

		virtual ~COpenVRBackend()
		{
		}

		/////////////
		// IBackend
		/////////////

		virtual bool Init() override
		{
            // Setup nested stuff.

			g_nOutputWidth = g_nPreferredOutputWidth;
			g_nOutputHeight = g_nPreferredOutputHeight;

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

            vr::EVRInitError error = vr::VRInitError_None;
            VR_Init( &error, vr::VRApplication_Background );

            if ( error != vr::VRInitError_None )
            {
                openvr_log.errorf("Unable to init VR runtime: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription( error ));
                return false;
            }

			if ( !vulkan_init( vulkan_get_instance(), VK_NULL_HANDLE ) )
			{
				return false;
			}

			if ( !wlsession_init() )
			{
				fprintf( stderr, "Failed to initialize Wayland session\n" );
				return false;
			}

            // Reset getopt() state
            optind = 1;

            int o;
            int opt_index = -1;
            while ((o = getopt_long(g_argc, g_argv, gamescope_optstring, gamescope_options, &opt_index)) != -1)
            {
                const char *opt_name;
                switch (o) {
                    case 0: // long options without a short option
                        opt_name = gamescope_options[opt_index].name;
                        if (strcmp(opt_name, "vr-overlay-key") == 0) {
                            m_szOverlayKey = optarg;
                        } else if (strcmp(opt_name, "vr-overlay-explicit-name") == 0) {
                            m_pchOverlayName = optarg;
                            m_bExplicitOverlayName = true;
                        } else if (strcmp(opt_name, "vr-overlay-default-name") == 0) {
                            m_pchOverlayName = optarg;
                        } else if (strcmp(opt_name, "vr-overlay-icon") == 0) {
                            m_pchOverlayIcon = optarg;
                        } else if (strcmp(opt_name, "vr-overlay-show-immediately") == 0) {
                            m_bNudgeToVisible = true;
                        } else if (strcmp(opt_name, "vr-overlay-enable-control-bar") == 0) {
                            m_bEnableControlBar = true;
                        } else if (strcmp(opt_name, "vr-overlay-enable-control-bar-keyboard") == 0) {
                            m_bEnableControlBarKeyboard = true;
                        } else if (strcmp(opt_name, "vr-overlay-enable-control-bar-close") == 0) {
                            m_bEnableControlBarClose = true;
                        } else if (strcmp(opt_name, "vr-overlay-modal") == 0) {
                            m_bModal = true;
                        } else if (strcmp(opt_name, "vr-overlay-physical-width") == 0) {
                            m_flPhysicalWidth = atof( optarg );
                            if ( m_flPhysicalWidth <= 0.0f )
                                m_flPhysicalWidth = 2.0f;
                        } else if (strcmp(opt_name, "vr-overlay-physical-curvature") == 0) {
                            m_flPhysicalCurvature = atof( optarg );
                        } else if (strcmp(opt_name, "vr-overlay-physical-pre-curve-pitch") == 0) {
                            m_flPhysicalPreCurvePitch = atof( optarg );
                        } else if (strcmp(opt_name, "vr-scroll-speed") == 0) {
                            m_flScrollSpeed = atof( optarg );
                        }
                        break;
                    case '?':
                        assert(false); // unreachable
                }
            }

            if ( m_szOverlayKey.empty() )
                m_szOverlayKey = std::string( "gamescope." ) + wlserver_get_wl_display_name();

            if ( !m_pchOverlayName )
                m_pchOverlayName = "Gamescope";

            m_pIPCResourceManager = vr::VRIPCResourceManager();
            if ( m_pIPCResourceManager )
            {
                uint32_t uFormatCount = 0;
                m_pIPCResourceManager->GetDmabufFormats( &uFormatCount, nullptr );

                if ( uFormatCount )
                {
                    std::vector<uint32_t> uFormats;
                    uFormats.resize( uFormatCount );
                    m_pIPCResourceManager->GetDmabufFormats( &uFormatCount, uFormats.data() );

                    for ( uint32_t i = 0; i < uFormatCount; i++ )
                    {
                        uint32_t uFormat = uFormats[i];
                        uint32_t uModifierCount = 0;
                        m_pIPCResourceManager->GetDmabufModifiers( vr::VRApplication_Overlay, uFormat, &uModifierCount, nullptr );

                        if ( uModifierCount )
                        {
                            std::vector<uint64_t> ulModifiers;
                            ulModifiers.resize( uModifierCount );
                            m_pIPCResourceManager->GetDmabufModifiers( vr::VRApplication_Overlay, uFormat, &uModifierCount, ulModifiers.data() );

                            for ( uint64_t ulModifier : ulModifiers )
                            {
                                if ( ulModifier != DRM_FORMAT_MOD_INVALID )
                                    m_FormatModifiers[uFormat].emplace_back( ulModifier );
                            }
                        }
                    }
                }
            }

            if ( UsesModifiers() )
            {
                openvr_log.infof( "Using modifiers!" );
            }

            if ( !vr::VROverlay() )
            {
                openvr_log.errorf( "SteamVR runtime version mismatch!\n" );
                return false;
            }

            // Setup misc. stuff
            g_nOutputRefresh = (int32_t) ConvertHztomHz( roundf( vr::VRSystem()->GetFloatTrackedDeviceProperty( vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float ) ) );

            std::thread input_thread_vrinput( [this](){ this->VRInputThread(); } );
            input_thread_vrinput.detach();

            return true;
		}

		virtual bool PostInit() override
		{
			m_pIME = create_local_ime();
            if ( !m_pIME )
                return false;

            for ( uint32_t i = 0; i < 8; i++ )
                m_Planes[i].Init( i == 0 ? nullptr : &m_Planes[0], i == 0 ? nullptr : &m_Planes[ i - 1 ] );

            m_Connector.UpdateEdid();
            this->HackUpdatePatchedEdid();

            if ( cv_hdr_enabled && m_Connector.GetHDRInfo().bExposeHDRSupport )
                setenv( "DXVK_HDR", "1", false );

            // This breaks cursor intersection right now.
            // Come back to me later.
            //Ratio<uint32_t> aspectRatio{ g_nOutputWidth, g_nOutputHeight };
            //m_pBlackTexture = vulkan_create_flat_texture( aspectRatio.Num(), aspectRatio.Denom(), 0, 0, 0, cv_vr_transparent_backing ? 0 : 255 );
            m_pBlackTexture = vulkan_create_flat_texture( g_nOutputWidth, g_nOutputHeight, 0, 0, 0, cv_vr_transparent_backing ? 0 : 255 );
            if ( !m_pBlackTexture )
            {
                openvr_log.errorf( "Failed to create dummy black texture." );
                return false;
            }

            return true;
		}

        virtual std::span<const char *const> GetInstanceExtensions() const override
		{
            static std::vector<std::string> s_exts;
            GetVulkanInstanceExtensionsRequired( s_exts );
            static std::vector<const char *> s_extPtrs;
            for ( const std::string &ext : s_exts )
                s_extPtrs.emplace_back( ext.c_str() );
			return std::span<const char *const>{ s_extPtrs.begin(), s_extPtrs.end() };
		}
        virtual std::span<const char *const> GetDeviceExtensions( VkPhysicalDevice pVkPhysicalDevice ) const override
		{
            static std::vector<std::string> s_exts;
            GetVulkanDeviceExtensionsRequired( pVkPhysicalDevice, s_exts );
            static std::vector<const char *> s_extPtrs;
            for ( const std::string &ext : s_exts )
                s_extPtrs.emplace_back( ext.c_str() );
			return std::span<const char *const>{ s_extPtrs.begin(), s_extPtrs.end() };
		}
        virtual VkImageLayout GetPresentLayout() const override
		{
			return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		}
        virtual void GetPreferredOutputFormat( uint32_t *pPrimaryPlaneFormat, uint32_t *pOverlayPlaneFormat ) const override
        {
            *pPrimaryPlaneFormat = VulkanFormatToDRM( VK_FORMAT_A2B10G10R10_UNORM_PACK32 );
            *pOverlayPlaneFormat = VulkanFormatToDRM( VK_FORMAT_B8G8R8A8_UNORM );
        }
		virtual bool ValidPhysicalDevice( VkPhysicalDevice pVkPhysicalDevice ) const override
		{
			return true;
		}

		virtual int Present( const FrameInfo_t *pFrameInfo, bool bAsync ) override
		{
            bool bNeedsFullComposite = false;

            // TODO: Dedupe some of this composite check code between us and drm.cpp
            bool bLayer0ScreenSize = close_enough(pFrameInfo->layers[0].scale.x, 1.0f) && close_enough(pFrameInfo->layers[0].scale.y, 1.0f);

            bool bNeedsCompositeFromFilter = (g_upscaleFilter == GamescopeUpscaleFilter::NEAREST || g_upscaleFilter == GamescopeUpscaleFilter::PIXEL) && !bLayer0ScreenSize;

            bNeedsFullComposite |= cv_composite_force;
            bNeedsFullComposite |= pFrameInfo->useFSRLayer0;
            bNeedsFullComposite |= pFrameInfo->useNISLayer0;
            bNeedsFullComposite |= pFrameInfo->useBICUBICLayer0;
            bNeedsFullComposite |= pFrameInfo->blurLayer0;
            bNeedsFullComposite |= bNeedsCompositeFromFilter;
            bNeedsFullComposite |= g_bColorSliderInUse;
            bNeedsFullComposite |= pFrameInfo->bFadingOut;
            bNeedsFullComposite |= !g_reshade_effect.empty();
            bNeedsFullComposite |= !UsesModifiers();

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
                    if ( pFrameInfo->layers[0].isScreenSize() && ( !pFrameInfo->layers[0].hasAlpha() || cv_vr_transparent_backing ) )
                        bNeedsBacking = false;
                }

                uint32_t uCurrentPlane = 0;
                if ( bNeedsBacking )
                {
                    COpenVRPlane *pPlane = &m_Planes[uCurrentPlane++];
                    pPlane->Present(
                        OpenVRPlaneState
                        {
                            .pTexture    = m_pBlackTexture.get(),
                            .flSrcWidth  = double( g_nOutputWidth ),
                            .flSrcHeight = double( g_nOutputHeight ),
                            .nDstWidth   = int32_t( g_nOutputWidth ),
                            .nDstHeight  = int32_t( g_nOutputHeight ),
                            .eColorspace = GAMESCOPE_APP_TEXTURE_COLORSPACE_PASSTHRU,
                            .bOpaque     = !cv_vr_transparent_backing,
                            .flAlpha     = cv_vr_transparent_backing ? 0.0f : 1.0f,
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
                    openvr_log.errorf( "vulkan_composite failed" );
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

                GetPrimaryPlane()->Present( &compositeLayer );

                for ( int i = 1; i < 8; i++ )
                    m_Planes[i].Present( nullptr );
            }


            GetVBlankTimer().UpdateWasCompositing( true );
            GetVBlankTimer().UpdateLastDrawTime( get_time_in_nanos() - g_SteamCompMgrVBlankTime.ulWakeupTime );

            this->PollState();

            return 0;
		}

		virtual void DirtyState( bool bForce, bool bForceModeset ) override
		{
		}

		virtual bool PollState() override
		{
			return false;
		}

		virtual std::shared_ptr<BackendBlob> CreateBackendBlob( const std::type_info &type, std::span<const uint8_t> data ) override
		{
			return std::make_shared<BackendBlob>( data );
		}

		virtual OwningRc<IBackendFb> ImportDmabufToBackend( wlr_buffer *pBuffer, wlr_dmabuf_attributes *pDmaBuf ) override
		{
            if ( UsesModifiers() )
            {
                vr::DmabufAttributes_t dmabufAttributes =
                {
                    .unWidth       = uint32_t( pDmaBuf->width ),
                    .unHeight      = uint32_t( pDmaBuf->height ),
                    .unDepth       = 1,
                    .unMipLevels   = 1,
                    .unArrayLayers = 1,
                    .unSampleCount = 1,
                    .unFormat      = pDmaBuf->format,
                    .ulModifier    = pDmaBuf->modifier,
                    .unPlaneCount  = uint32_t( pDmaBuf->n_planes ),
                    .plane         =
                    {
                        {
                            .unOffset = pDmaBuf->offset[0],
                            .unStride = pDmaBuf->stride[0],
                            .nFd      = pDmaBuf->fd[0],
                        },
                        {
                            .unOffset = pDmaBuf->offset[1],
                            .unStride = pDmaBuf->stride[1],
                            .nFd      = pDmaBuf->fd[1],
                        },
                        {
                            .unOffset = pDmaBuf->offset[2],
                            .unStride = pDmaBuf->stride[2],
                            .nFd      = pDmaBuf->fd[2],
                        },
                        {
                            .unOffset = pDmaBuf->offset[3],
                            .unStride = pDmaBuf->stride[3],
                            .nFd      = pDmaBuf->fd[3],
                        },
                    }
                };

                vr::SharedTextureHandle_t ulSharedHandle = 0;
                if ( !m_pIPCResourceManager->ImportDmabuf( vr::VRApplication_Overlay, &dmabufAttributes, &ulSharedHandle ) )
                    return nullptr;
                assert( ulSharedHandle != 0 );

                // Take the first reference!
                if ( !m_pIPCResourceManager->RefResource( ulSharedHandle, nullptr ) )
                    return nullptr;

                return new COpenVRFb{ this, ulSharedHandle };
            }
            else
            {
                return new COpenVRFb{ this, 0 };
            }
		}

		virtual bool UsesModifiers() const override
		{
            if ( !cv_vr_use_modifiers )
                return false;

            if ( !m_pIPCResourceManager )
                return false;

			return !m_FormatModifiers.empty();
		}
		virtual std::span<const uint64_t> GetSupportedModifiers( uint32_t uDrmFormat ) const override
		{
            if ( !UsesModifiers() )
                return std::span<const uint64_t>{};

            auto iter = m_FormatModifiers.find( uDrmFormat );
            if ( iter == m_FormatModifiers.end() )
                return std::span<const uint64_t>{};

            return std::span<const uint64_t>{ iter->second.begin(), iter->second.end() };
		}

		virtual IBackendConnector *GetCurrentConnector() override
		{
			return &m_Connector;
		}
		virtual IBackendConnector *GetConnector( GamescopeScreenType eScreenType ) override
		{
			if ( eScreenType == GAMESCOPE_SCREEN_TYPE_INTERNAL )
				return &m_Connector;

			return nullptr;
		}

		virtual bool IsVRRActive() const override
		{
			return false;
		}

		virtual bool SupportsPlaneHardwareCursor() const override
		{
			return false;
		}

		virtual bool SupportsTearing() const override
		{
			return false;
		}

		virtual bool UsesVulkanSwapchain() const override
		{
			return false;
		}

        virtual bool IsSessionBased() const override
		{
			return false;
		}

        virtual bool SupportsExplicitSync() const override
        {
            // We only forward done DMA-BUFs, so this should be fine.
            // SteamVR does not do any wait/poll/sync on these.
            return true;
        }

		virtual bool IsVisible() const override
		{
            if ( ShouldNudgeToVisible() )
                return true;

            return m_bOverlayVisible.load();
		}

		virtual glm::uvec2 CursorSurfaceSize( glm::uvec2 uvecSize ) const override
		{
			return uvecSize;
		}

		virtual bool HackTemporarySetDynamicRefresh( int nRefresh ) override
		{
			return false;
		}

		virtual void HackUpdatePatchedEdid() override
		{
            if ( !GetCurrentConnector() )
                return;

            WritePatchedEdid( GetCurrentConnector()->GetRawEDID(), GetCurrentConnector()->GetHDRInfo(), false );
		}

        virtual bool NeedsFrameSync() const override
        {
            return true;
        }
        virtual VBlankScheduleTime FrameSync() override
        {
            WaitUntilVisible();

            if ( vr::VROverlay()->WaitFrameSync( ~0u ) != vr::VROverlayError_None )
                openvr_log.errorf( "WaitFrameSync failed!" );

            uint64_t ulNow = get_time_in_nanos();
            return VBlankScheduleTime
            {
                .ulTargetVBlank  = ulNow + 3'000'000, // Not right. just a stop-gap for now.
                .ulScheduledWakeupPoint = ulNow,
            };
        }

        virtual TouchClickMode GetTouchClickMode() override
        {
            if ( cv_vr_trackpad_relative_mouse_mode && m_bRelativeMouse )
            {
                return TouchClickModes::Trackpad;
            }

            return CBaseBackend::GetTouchClickMode();
        }

		virtual INestedHints *GetNestedHints() override
        {
            return this;
        }

		///////////////////
		// INestedHints
		///////////////////

        virtual void SetCursorImage( std::shared_ptr<INestedHints::CursorInfo> info ) override
        {
        }
        virtual void SetRelativeMouseMode( bool bRelative ) override
        {
            if ( bRelative != m_bRelativeMouse )
            {
                for ( COpenVRPlane &plane : m_Planes )
                {
                    vr::VROverlay()->SetOverlayFlag( plane.GetOverlay(), vr::VROverlayFlags_HideLaserIntersection, cv_vr_trackpad_hide_laser && bRelative );
                }
                m_bRelativeMouse = bRelative;
            }
        }
        virtual void SetVisible( bool bVisible ) override
        {
            vr::VROverlay()->SetOverlayFlag( GetPrimaryPlane()->GetOverlay(), vr::VROverlayFlags_VisibleInDashboard, bVisible );
        }
        virtual void SetTitle( std::shared_ptr<std::string> szTitle ) override
        {
            if ( !m_bExplicitOverlayName )
                vr::VROverlay()->SetOverlayName( GetPrimaryPlane()->GetOverlay(), szTitle ? szTitle->c_str() : m_pchOverlayName );

        }
        virtual void SetIcon( std::shared_ptr<std::vector<uint32_t>> uIconPixels ) override
        {
            if ( cv_vr_use_window_icons && uIconPixels && uIconPixels->size() >= 3 )
            {
                const uint32_t uWidth = (*uIconPixels)[0];
                const uint32_t uHeight = (*uIconPixels)[1];

                struct rgba_t
                {
                    uint8_t r,g,b,a;
                };

                for ( uint32_t& val : *uIconPixels )
                {
                    rgba_t rgb = *((rgba_t*)&val);
                    std::swap(rgb.r, rgb.b);
                    val = *((uint32_t*)&rgb);
                }

                vr::VROverlay()->SetOverlayRaw( GetPrimaryPlane()->GetOverlayThumbnail(), &(*uIconPixels)[2], uWidth, uHeight, sizeof(uint32_t) );
            }
            else if ( m_pchOverlayIcon )
            {
                vr::VROverlay()->SetOverlayFromFile( GetPrimaryPlane()->GetOverlayThumbnail(), m_pchOverlayIcon );
            }
            else
            {
                vr::VROverlay()->ClearOverlayTexture( GetPrimaryPlane()->GetOverlayThumbnail() );
            }
        }
        virtual void SetSelection( std::shared_ptr<std::string> szContents, GamescopeSelection eSelection ) override
        {
            // Do nothing.
        }
        virtual std::shared_ptr<INestedHints::CursorInfo> GetHostCursor() override
        {
            return nullptr;
        }

        vr::IVRIPCResourceManagerClient *GetIPCResourceManager()
        {
            return m_pIPCResourceManager;
        }

        bool SupportsColorManagement() const
        {
            return false;
        }

        const char *GetOverlayKey() const { return m_szOverlayKey.c_str(); }
        const char *GetOverlayName() const { return m_pchOverlayName; }
        const char *GetOverlayIcon() const { return m_pchOverlayIcon; }
        bool ShouldEnableControlBar() const { return m_bEnableControlBar; }
        bool ShouldEnableControlBarKeyboard() const { return m_bEnableControlBarKeyboard; }
        bool ShouldEnableControlBarClose() const { return m_bEnableControlBarClose; }
        bool IsModal() const { return m_bModal; }
        float GetPhysicalWidth() const { return m_flPhysicalWidth; }
        float GetPhysicalCurvature() const { return m_flPhysicalCurvature; }
        float GetPhysicalPreCurvePitch() const { return m_flPhysicalPreCurvePitch; }
        float GetScrollSpeed() const { return m_flScrollSpeed; }
        bool IsRelativeMouse() const { return m_bRelativeMouse; }

        bool ShouldNudgeToVisible() const { return m_bNudgeToVisible; }
        bool ConsumeNudgeToVisible() { return std::exchange( m_bNudgeToVisible, false ); }

	protected:

		virtual void OnBackendBlobDestroyed( BackendBlob *pBlob ) override
		{
		}

	private:

        COpenVRPlane *GetPrimaryPlane()
        {
            return &m_Planes[0];
        }

        void WaitUntilVisible()
        {
            if ( ShouldNudgeToVisible() )
                return;

            m_bOverlayVisible.wait( false );
        }

        void VRInputThread()
        {
            pthread_setname_np( pthread_self(), "gamescope-vrinp" );

            // Josh: PollNextOverlayEvent sucks.
            // I want WaitNextOverlayEvent (like SDL_WaitEvent) so this doesn't have to spin and sleep.
            while (true)
            {
                for ( COpenVRPlane &plane : m_Planes )
                {
                    vr::VREvent_t vrEvent;
                    while( vr::VROverlay()->PollNextOverlayEvent( plane.GetOverlay(), &vrEvent, sizeof( vrEvent ) ) )
                    {
                        switch( vrEvent.eventType )
                        {
                            case vr::VREvent_OverlayClosed:
                            case vr::VREvent_Quit:
                            {
                                if ( !plane.IsSubview() )
                                {
                                    raise( SIGTERM );
                                }
                                break;
                            }

                            case vr::VREvent_KeyboardCharInput:
                            {
                                if (m_pIME)
                                {
                                    type_text(m_pIME, vrEvent.data.keyboard.cNewInput);
                                }
                                break;
                            }

                            case vr::VREvent_MouseMove:
                            {
                                float flX = vrEvent.data.mouse.x / float( g_nOutputWidth );
                                float flY = ( g_nOutputHeight - vrEvent.data.mouse.y ) / float( g_nOutputHeight );

                                TouchClickMode eMode = GetTouchClickMode();
                                // Always warp a cursor, even if it's invisible, so we get hover events.
                                bool bAlwaysMoveCursor = eMode == TouchClickModes::Passthrough && cv_vr_always_warp_cursor;

                                if ( eMode == TouchClickModes::Trackpad )
                                {
                                    glm::vec2 vOldTrackpadPos = m_vScreenTrackpadPos;
                                    m_vScreenTrackpadPos = glm::vec2{ flX, flY };

                                    if ( m_bMouseDown )
                                    {
                                        glm::vec2 vDelta = ( m_vScreenTrackpadPos - vOldTrackpadPos );
                                        // We are based off normalized coords, so we need to fix the aspect ratio
                                        // or we get different sensitivities on X and Y.
                                        vDelta.y *= ( (float)g_nOutputHeight / (float)g_nOutputWidth );

                                        vDelta *= float( cv_vr_trackpad_sensitivity );

                                        wlserver_lock();
                                        wlserver_mousemotion( vDelta.x, vDelta.y, ++m_uFakeTimestamp );
                                        wlserver_unlock();
                                    }
                                }
                                else
                                {
                                    wlserver_lock();
                                    wlserver_touchmotion( flX, flY , 0, ++m_uFakeTimestamp, bAlwaysMoveCursor );
                                    wlserver_unlock();
                                }
                                break;
                            }
                            case vr::VREvent_MouseButtonUp:
                            case vr::VREvent_MouseButtonDown:
                            {
                                float flX = vrEvent.data.mouse.x / float( g_nOutputWidth );
                                float flY = ( g_nOutputHeight - vrEvent.data.mouse.y ) / float( g_nOutputHeight );

                                uint64_t ulNow = get_time_in_nanos();

                                if ( vrEvent.eventType == vr::VREvent_MouseButtonDown )
                                {
                                    m_ulMouseDownTime = ulNow;
                                    m_bMouseDown = true;
                                }
                                else
                                {
                                    m_bMouseDown = false;
                                }

                                TouchClickMode eMode = GetTouchClickMode();
                                if ( eMode == TouchClickModes::Trackpad )
                                {
                                    m_vScreenTrackpadPos = glm::vec2{ flX, flY };

                                    if ( vrEvent.eventType == vr::VREvent_MouseButtonUp )
                                    {
                                        glm::vec2 vTotalDelta = ( m_vScreenTrackpadPos - m_vScreenStartTrackpadPos );
                                        vTotalDelta.y *= ( (float)g_nOutputHeight / (float)g_nOutputWidth );
                                        float flMaxAbsTotalDelta = std::max<float>( std::abs( vTotalDelta.x ), std::abs( vTotalDelta.y ) );

                                        uint64_t ulClickTime = ulNow - m_ulMouseDownTime;
                                        if ( ulClickTime <= cv_vr_trackpad_click_time && flMaxAbsTotalDelta <= cv_vr_trackpad_click_max_delta )
                                        {
                                            wlserver_lock();
                                            wlserver_mousebutton( BTN_LEFT, true, ++m_uFakeTimestamp );
                                            wlserver_unlock();

                                            sleep_for_nanos( g_SteamCompMgrLimitedAppRefreshCycle + 1'000'000 );

                                            wlserver_lock();
                                            wlserver_mousebutton( BTN_LEFT, false, ++m_uFakeTimestamp );
                                            wlserver_unlock();
                                        }
                                        else
                                        {
                                            m_vScreenStartTrackpadPos = m_vScreenTrackpadPos;
                                        }
                                    }
                                }
                                else
                                {
                                    wlserver_lock();
                                    if ( vrEvent.eventType == vr::VREvent_MouseButtonDown )
                                        wlserver_touchdown( flX, flY, 0, ++m_uFakeTimestamp );
                                    else
                                        wlserver_touchup( 0, ++m_uFakeTimestamp );
                                    wlserver_unlock();
                                }
                                break;
                            }

                            case vr::VREvent_ScrollSmooth:
                            {
                                float flX = -vrEvent.data.scroll.xdelta * m_flScrollSpeed;
                                float flY = -vrEvent.data.scroll.ydelta * m_flScrollSpeed;
                                wlserver_lock();
                                wlserver_mousewheel( flX, flY, ++m_uFakeTimestamp );
                                wlserver_unlock();
                                break;
                            }

                            case vr::VREvent_ButtonPress:
                            {
                                vr::EVRButtonId button = (vr::EVRButtonId)vrEvent.data.controller.button;

                                if (button != vr::k_EButton_Steam && button != vr::k_EButton_QAM)
                                    break;

                                if (button == vr::k_EButton_Steam)
                                    openvr_log.infof("STEAM button pressed.");
                                else
                                    openvr_log.infof("QAM button pressed.");

                                wlserver_open_steam_menu( button == vr::k_EButton_QAM );
                                break;
                            }

                            case vr::VREvent_OverlayShown:
                            case vr::VREvent_OverlayHidden:
                            {
                                // Only handle this for the base plane.
                                // Subviews can be hidden if we hide them ourselves,
                                // or for other reasons.
                                if ( !plane.IsSubview() )
                                {
                                    m_bOverlayVisible = vrEvent.eventType == vr::VREvent_OverlayShown;
                                    m_bOverlayVisible.notify_all();
                                }
                                break;
                            }

                            default:
                                break;
                        }
                    }
                }
                sleep_for_nanos( cv_vr_poll_rate );
            }
        }

        CVROverlayConnector m_Connector;
        std::string m_szOverlayKey;
        const char *m_pchOverlayName = nullptr;
        const char *m_pchOverlayIcon = nullptr;
        bool m_bExplicitOverlayName = false;
        bool m_bNudgeToVisible = false;
        bool m_bEnableControlBar = false;
        bool m_bEnableControlBarKeyboard = false;
        bool m_bEnableControlBarClose = false;
        bool m_bModal = false;
        std::atomic<bool> m_bRelativeMouse = false;
        float m_flPhysicalWidth = 2.0f;
        float m_flPhysicalCurvature = 0.0f;
        float m_flPhysicalPreCurvePitch = 0.0f;
        float m_flScrollSpeed = 1.0f;

        COpenVRPlane m_Planes[8];

        wlserver_input_method *m_pIME = nullptr;

        OwningRc<CVulkanTexture> m_pBlackTexture;

        std::atomic<bool> m_bOverlayVisible = { false };

        vr::IVRIPCResourceManagerClient *m_pIPCResourceManager = nullptr;
        std::unordered_map<uint32_t, std::vector<uint64_t>> m_FormatModifiers;

        std::atomic<uint32_t> m_uFakeTimestamp = { 0 };

        bool m_bMouseDown = false;
        uint64_t m_ulMouseDownTime = 0;
        // Fake "trackpad" tracking for the whole overlay panel.
        glm::vec2 m_vScreenTrackpadPos{};
        glm::vec2 m_vScreenStartTrackpadPos{};
	};

	/////////////////////////
	// COpenVRFb
	/////////////////////////

    COpenVRFb::COpenVRFb( COpenVRBackend *pBackend, vr::SharedTextureHandle_t ulHandle )
        : CBaseBackendFb{}
        , m_pBackend{ pBackend }
        , m_ulHandle{ ulHandle }
    {
    }

    COpenVRFb::~COpenVRFb()
    {
        if ( m_ulHandle != 0 )
            m_pBackend->GetIPCResourceManager()->UnrefResource( m_ulHandle );
        m_ulHandle = 0;
    }

	/////////////////////////
	// COpenVRPlane
	/////////////////////////

    COpenVRPlane::COpenVRPlane( COpenVRBackend *pBackend )
        : m_pBackend{ pBackend }
    {
    }
    COpenVRPlane::~COpenVRPlane()
    {
    }

    bool COpenVRPlane::Init( COpenVRPlane *pParent, COpenVRPlane *pSiblingBelow )
    {
        m_bIsSubview = pParent != nullptr;

        if ( pSiblingBelow )
        {
            m_uSortOrder = pSiblingBelow->GetSortOrder() + 1;
        }

        if ( !m_bIsSubview )
        {
            vr::VROverlay()->CreateDashboardOverlay(
                m_pBackend->GetOverlayKey(),
                m_pBackend->GetOverlayName(),
                &m_hOverlay, &m_hOverlayThumbnail );

            vr::VROverlay()->SetOverlayFlag( m_hOverlay, vr::VROverlayFlags_EnableControlBar,		  m_pBackend->ShouldEnableControlBar() );
            vr::VROverlay()->SetOverlayFlag( m_hOverlay, vr::VROverlayFlags_EnableControlBarKeyboard, m_pBackend->ShouldEnableControlBarKeyboard() );
            vr::VROverlay()->SetOverlayFlag( m_hOverlay, vr::VROverlayFlags_EnableControlBarClose,	  m_pBackend->ShouldEnableControlBarClose() );
            vr::VROverlay()->SetOverlayFlag( m_hOverlay, vr::VROverlayFlags_WantsModalBehavior,	      m_pBackend->IsModal() );
            vr::VROverlay()->SetOverlayFlag( m_hOverlay, vr::VROverlayFlags_SendVRSmoothScrollEvents, true );
            vr::VROverlay()->SetOverlayFlag( m_hOverlay, vr::VROverlayFlags_VisibleInDashboard,       false );
            vr::VROverlay()->SetOverlayFlag( m_hOverlay, vr::VROverlayFlags_HideLaserIntersection,    cv_vr_trackpad_hide_laser && m_pBackend->IsRelativeMouse() );

            vr::VROverlay()->SetOverlayWidthInMeters( m_hOverlay,  m_pBackend->GetPhysicalWidth() );
            vr::VROverlay()->SetOverlayCurvature	( m_hOverlay,  m_pBackend->GetPhysicalCurvature() );
            vr::VROverlay()->SetOverlayPreCurvePitch( m_hOverlay,  m_pBackend->GetPhysicalPreCurvePitch() );

            if ( m_pBackend->GetOverlayIcon() )
            {
                vr::EVROverlayError err = vr::VROverlay()->SetOverlayFromFile( m_hOverlayThumbnail, m_pBackend->GetOverlayIcon() );
                if( err != vr::VROverlayError_None )
                {
                    openvr_log.errorf( "Unable to set thumbnail to %s: %s\n", m_pBackend->GetOverlayIcon(), vr::VROverlay()->GetOverlayErrorNameFromEnum( err ) );
                }
            }
        }
        else
        {
            std::string szSubviewName = m_pBackend->GetOverlayKey() + std::string(".layer") + std::to_string( (uintptr_t)this );
            vr::VROverlay()->CreateSubviewOverlay( pParent->GetOverlay(), szSubviewName.c_str(), "Gamescope Layer", &m_hOverlay );
        }

        vr::VROverlay()->SetOverlayFlag( m_hOverlay, vr::VROverlayFlags_IsPremultiplied, true );
        vr::VROverlay()->SetOverlayInputMethod( m_hOverlay, vr::VROverlayInputMethod_Mouse );
        vr::VROverlay()->SetOverlaySortOrder( m_hOverlay, m_uSortOrder );

        return true;
    }

    void COpenVRPlane::Present( std::optional<OpenVRPlaneState> oState )
    {
        if ( !m_bIsSubview )
        {
            vr::VROverlay()->SetOverlayFlag( m_hOverlay, vr::VROverlayFlags_EnableControlBarSteamUI, steamMode );
        }

        if ( oState )
        {
            vr::VROverlay()->SetOverlayAlpha( m_hOverlay, oState->flAlpha );

            if ( m_pBackend->UsesModifiers() )
            {
                vr::VROverlay()->SetOverlayFlag( m_hOverlay, vr::VROverlayFlags_IgnoreTextureAlpha,	oState->bOpaque || !DRMFormatHasAlpha( oState->pTexture->drmFormat() ) );

                vr::HmdVector2_t vMouseScale =
                {
                    float( oState->nDstWidth ),
                    float( oState->nDstHeight ),
                };
                vr::VROverlay()->SetOverlayMouseScale( m_hOverlay, &vMouseScale );
                vr::VRTextureBounds_t vTextureBounds =
                {
                    float( ( oState->flSrcX ) / double( oState->pTexture->width() ) ),
                    float( ( oState->flSrcY ) / double( oState->pTexture->height() ) ),
                    float( ( oState->flSrcX + oState->flSrcWidth ) / double( oState->pTexture->width() ) ),
                    float( ( oState->flSrcY + oState->flSrcHeight ) / double( oState->pTexture->height() ) ),
                };
                vr::VROverlay()->SetOverlayTextureBounds( m_hOverlay, &vTextureBounds );
                if ( m_bIsSubview )
                {
                    vr::VROverlay()->SetSubviewPosition( m_hOverlay, oState->nDestX, oState->nDestY );
                    vr::VROverlay()->ShowOverlay( m_hOverlay );
                }

                COpenVRFb *pFb = static_cast<COpenVRFb *>( oState->pTexture->GetBackendFb() );
                vr::SharedTextureHandle_t ulHandle = pFb->GetSharedTextureHandle();

                vr::Texture_t texture = { (void *)&ulHandle, vr::TextureType_SharedTextureHandle, vr::ColorSpace_Gamma };
                vr::VROverlay()->SetOverlayTexture( m_hOverlay, &texture );
            }
            else
            {
                assert( !m_bIsSubview );
                
                vr::VRVulkanTextureData_t data =
                {
                    .m_nImage            = (uint64_t)(uintptr_t)oState->pTexture->vkImage(),
                    .m_pDevice           = g_device.device(),
                    .m_pPhysicalDevice   = g_device.physDev(),
                    .m_pInstance         = g_device.instance(),
                    .m_pQueue            = g_device.queue(),
                    .m_nQueueFamilyIndex = g_device.queueFamily(),
                    .m_nWidth            = oState->pTexture->width(),
                    .m_nHeight           = oState->pTexture->height(),
                    .m_nFormat           = oState->pTexture->format(),
                    .m_nSampleCount      = 1,
                };

                vr::Texture_t texture = { &data, vr::TextureType_Vulkan, vr::ColorSpace_Gamma };
                vr::VROverlay()->SetOverlayTexture( m_hOverlay, &texture );
            }

            if ( !m_bIsSubview && m_pBackend->ConsumeNudgeToVisible() )
            {
                vr::VROverlay()->ShowDashboard( m_pBackend->GetOverlayKey() );
            }
        }
        else
        {
            if ( m_bIsSubview )
            {
                vr::VROverlay()->HideOverlay( m_hOverlay );
            }
        }
    }

    void COpenVRPlane::Present( const FrameInfo_t::Layer_t *pLayer )
    {
        if ( pLayer && pLayer->tex )
        {
            Present(
                OpenVRPlaneState
                {
                    .pTexture    = pLayer->tex.get(),
                    .nDestX      = int32_t( -pLayer->offset.x ),
                    .nDestY      = int32_t( -pLayer->offset.y ),
                    .flSrcX      = 0.0,
                    .flSrcY      = 0.0,
                    .flSrcWidth  = double( pLayer->tex->width() ),
                    .flSrcHeight = double( pLayer->tex->height() ),
                    .nDstWidth   = int32_t( pLayer->tex->width() / double( pLayer->scale.x ) ),
                    .nDstHeight  = int32_t( pLayer->tex->height() / double( pLayer->scale.y ) ),
                    .eColorspace = pLayer->colorspace,
                    .bOpaque     = pLayer->zpos == g_zposBase && !cv_vr_transparent_backing,
                    .flAlpha     = pLayer->opacity,
                } );
        }
        else
        {
            Present( std::nullopt );
        }
    }

	/////////////////////////
	// Backend Instantiator
	/////////////////////////

	template <>
	bool IBackend::Set<COpenVRBackend>()
	{
		return Set( new COpenVRBackend{} );
	}
}
