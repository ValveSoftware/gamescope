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

static LogScope openvr_log("openvr");

static bool GetVulkanInstanceExtensionsRequired( std::vector< std::string > &outInstanceExtensionList );
static bool GetVulkanDeviceExtensionsRequired( VkPhysicalDevice pPhysicalDevice, std::vector< std::string > &outDeviceExtensionList );


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
            return std::span<const uint8_t>{};
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

    private:
        BackendConnectorHDRInfo m_HDRInfo{};
    };

	class COpenVRBackend final : public CBaseBackend, public INestedHints
	{
	public:
		COpenVRBackend()
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
                            m_pchOverlayKey = optarg;
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

            if ( !m_pchOverlayKey )
                m_pchOverlayKey = wlserver_get_wl_display_name();

            if ( !m_pchOverlayName )
                m_pchOverlayName = "Gamescope";

            if ( !vr::VROverlay() )
            {
                openvr_log.errorf( "SteamVR runtime version mismatch!\n" );
                return false;
            }

            vr::VROverlay()->CreateDashboardOverlay(
                m_pchOverlayKey,
                m_pchOverlayName,
                &m_hOverlay, &m_hOverlayThumbnail );

            vr::VROverlay()->SetOverlayInputMethod( m_hOverlay, vr::VROverlayInputMethod_Mouse );

            vr::HmdVector2_t vMouseScale = { { (float)g_nOutputWidth, (float)g_nOutputHeight } };
            vr::VROverlay()->SetOverlayMouseScale( m_hOverlay, &vMouseScale );

            vr::VROverlay()->SetOverlayFlag( m_hOverlay, vr::VROverlayFlags_IgnoreTextureAlpha,		true );
            vr::VROverlay()->SetOverlayFlag( m_hOverlay, vr::VROverlayFlags_EnableControlBar,			m_bEnableControlBar );
            vr::VROverlay()->SetOverlayFlag( m_hOverlay, vr::VROverlayFlags_EnableControlBarKeyboard,	m_bEnableControlBarKeyboard );
            vr::VROverlay()->SetOverlayFlag( m_hOverlay, vr::VROverlayFlags_EnableControlBarClose,	m_bEnableControlBarClose );
            vr::VROverlay()->SetOverlayFlag( m_hOverlay, vr::VROverlayFlags_WantsModalBehavior,	    m_bModal );
            vr::VROverlay()->SetOverlayFlag( m_hOverlay, vr::VROverlayFlags_SendVRSmoothScrollEvents, true );
            vr::VROverlay()->SetOverlayFlag( m_hOverlay, vr::VROverlayFlags_VisibleInDashboard,       false );
            vr::VROverlay()->SetOverlayFlag( m_hOverlay, vr::VROverlayFlags_HideLaserIntersection,    m_bRelativeMouse );

            vr::VROverlay()->SetOverlayWidthInMeters( m_hOverlay,  m_flPhysicalWidth );
            vr::VROverlay()->SetOverlayCurvature	( m_hOverlay,  m_flPhysicalCurvature );
            vr::VROverlay()->SetOverlayPreCurvePitch( m_hOverlay,  m_flPhysicalPreCurvePitch );

            if ( m_pchOverlayIcon )
            {
                vr::EVROverlayError err = vr::VROverlay()->SetOverlayFromFile( m_hOverlayThumbnail, m_pchOverlayIcon );
                if( err != vr::VROverlayError_None )
                {
                    openvr_log.errorf( "Unable to set thumbnail to %s: %s\n", m_pchOverlayIcon, vr::VROverlay()->GetOverlayErrorNameFromEnum( err ) );
                }
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
        virtual void GetPreferredOutputFormat( VkFormat *pPrimaryPlaneFormat, VkFormat *pOverlayPlaneFormat ) const override
        {
			*pPrimaryPlaneFormat = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
			*pOverlayPlaneFormat = VK_FORMAT_B8G8R8A8_UNORM;
        }
		virtual bool ValidPhysicalDevice( VkPhysicalDevice pVkPhysicalDevice ) const override
		{
			return true;
		}

		virtual int Present( const FrameInfo_t *pFrameInfo, bool bAsync ) override
		{
            // TODO: Resolve const crap
            std::optional oCompositeResult = vulkan_composite( (FrameInfo_t *)pFrameInfo, nullptr, false );
            if ( !oCompositeResult )
                return -EINVAL;

            vulkan_wait( *oCompositeResult, true );

            auto outputImage = vulkan_get_last_output_image( false, false );

            vr::VRVulkanTextureData_t data =
            {
                .m_nImage            = (uint64_t)(uintptr_t)outputImage->vkImage(),
                .m_pDevice           = g_device.device(),
                .m_pPhysicalDevice   = g_device.physDev(),
                .m_pInstance         = g_device.instance(),
                .m_pQueue            = g_device.queue(),
                .m_nQueueFamilyIndex = g_device.queueFamily(),
                .m_nWidth            = outputImage->width(),
                .m_nHeight           = outputImage->height(),
                .m_nFormat           = outputImage->format(),
                .m_nSampleCount      = 1,
            };

            vr::VROverlay()->SetOverlayFlag( m_hOverlay, vr::VROverlayFlags_EnableControlBarSteamUI, steamMode );

            vr::Texture_t texture = { &data, vr::TextureType_Vulkan, vr::ColorSpace_Gamma };
            vr::VROverlay()->SetOverlayTexture( m_hOverlay, &texture );
            if ( m_bNudgeToVisible )
            {
                vr::VROverlay()->ShowDashboard( m_pchOverlayKey );
                m_bNudgeToVisible = false;
            }

            GetVBlankTimer().UpdateWasCompositing( true );
            GetVBlankTimer().UpdateLastDrawTime( get_time_in_nanos() - g_SteamCompMgrVBlankTime.ulWakeupTime );

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

		virtual uint32_t ImportDmabufToBackend( wlr_buffer *pBuffer, wlr_dmabuf_attributes *pDmaBuf ) override
		{
			return 0;
		}

        virtual void LockBackendFb( uint32_t uFbId ) override
		{
			abort();
		}
        virtual void UnlockBackendFb( uint32_t uFbId ) override
		{
			abort();
		}
        virtual void DropBackendFb( uint32_t uFbId ) override
		{
			abort();
		}

		virtual bool UsesModifiers() const override
		{
			return false;
		}
		virtual std::span<const uint64_t> GetSupportedModifiers( uint32_t uDrmFormat ) const override
		{
			return std::span<const uint64_t>{};
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
            // We always composite right now, so yes.
            return true;
        }

		virtual bool IsVisible() const override
		{
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
                vr::VROverlay()->SetOverlayFlag( m_hOverlay, vr::VROverlayFlags_HideLaserIntersection, bRelative );
                m_bRelativeMouse = bRelative;
            }
        }
        virtual void SetVisible( bool bVisible ) override
        {
            vr::VROverlay()->SetOverlayFlag( m_hOverlay, vr::VROverlayFlags_VisibleInDashboard, bVisible );
        }
        virtual void SetTitle( std::shared_ptr<std::string> szTitle ) override
        {
            if ( !m_bExplicitOverlayName )
                vr::VROverlay()->SetOverlayName( m_hOverlay, szTitle ? szTitle->c_str() : m_pchOverlayName );

        }
        virtual void SetIcon( std::shared_ptr<std::vector<uint32_t>> uIconPixels ) override
        {
            if ( uIconPixels && uIconPixels->size() >= 3 )
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

                vr::VROverlay()->SetOverlayRaw( m_hOverlayThumbnail, &(*uIconPixels)[2], uWidth, uHeight, sizeof(uint32_t) );
            }
            else if ( m_pchOverlayIcon )
            {
                vr::VROverlay()->SetOverlayFromFile( m_hOverlayThumbnail, m_pchOverlayIcon );
            }
            else
            {
                vr::VROverlay()->ClearOverlayTexture( m_hOverlayThumbnail );
            }
        }
		virtual std::shared_ptr<INestedHints::CursorInfo> GetHostCursor() override
        {
            return nullptr;
        }

	protected:

		virtual void OnBackendBlobDestroyed( BackendBlob *pBlob ) override
		{
		}

	private:

        void WaitUntilVisible()
        {
            m_bOverlayVisible.wait( false );
        }

        void VRInputThread()
        {
            pthread_setname_np( pthread_self(), "gamescope-vrinp" );

            // Josh: PollNextOverlayEvent sucks.
            // I want WaitNextOverlayEvent (like SDL_WaitEvent) so this doesn't have to spin and sleep.
            while (true)
            {
                vr::VREvent_t vrEvent;
                while( vr::VROverlay()->PollNextOverlayEvent( m_hOverlay, &vrEvent, sizeof( vrEvent ) ) )
                {
                    switch( vrEvent.eventType )
                    {
                        case vr::VREvent_OverlayClosed:
                        case vr::VREvent_Quit:
                            raise( SIGTERM );
                            break;

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
                            float x = vrEvent.data.mouse.x;
                            float y = g_nOutputHeight - vrEvent.data.mouse.y;

                            wlserver_lock();
                            if ( GetTouchClickMode() == TouchClickModes::Passthrough )
                                wlserver_touchmotion( x / float( g_nOutputWidth ), y / float( g_nOutputHeight ), 0, ++m_uFakeTimestamp );
                            wlserver_mousewarp( x, y, m_uFakeTimestamp );
                            wlserver_unlock();
                            break;
                        }
                        case vr::VREvent_MouseButtonUp:
                        case vr::VREvent_MouseButtonDown:
                        {
                            float x = vrEvent.data.mouse.x;
                            float y = g_nOutputHeight - vrEvent.data.mouse.y;

                            x /= (float)g_nOutputWidth;
                            y /= (float)g_nOutputHeight;

                            wlserver_lock();
                            if ( vrEvent.eventType == vr::VREvent_MouseButtonDown )
                                wlserver_touchdown( x, y, 0, ++m_uFakeTimestamp );
                            else
                                wlserver_touchup( 0, ++m_uFakeTimestamp );
                            wlserver_unlock();
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
                            m_bOverlayVisible = vrEvent.eventType == vr::VREvent_OverlayShown;
                            m_bOverlayVisible.notify_all();
                            break;
                        }

                        default:
                            break;
                    }
                }
                sleep_for_nanos( 2'000'000ul );
            }
        }

        CVROverlayConnector m_Connector;
        const char *m_pchOverlayKey = nullptr;
        const char *m_pchOverlayName = nullptr;
        const char *m_pchOverlayIcon = nullptr;
        bool m_bExplicitOverlayName = false;
        bool m_bNudgeToVisible = false;
        bool m_bEnableControlBar = false;
        bool m_bEnableControlBarKeyboard = false;
        bool m_bEnableControlBarClose = false;
        bool m_bModal = false;
        bool m_bRelativeMouse = false;
        float m_flPhysicalWidth = 2.0f;
        float m_flPhysicalCurvature = 0.0f;
        float m_flPhysicalPreCurvePitch = 0.0f;
        float m_flScrollSpeed = 1.0f;
        vr::VROverlayHandle_t m_hOverlay = vr::k_ulOverlayHandleInvalid;
        vr::VROverlayHandle_t m_hOverlayThumbnail = vr::k_ulOverlayHandleInvalid;
        wlserver_input_method *m_pIME = nullptr;
        std::atomic<bool> m_bOverlayVisible = { false };

        std::atomic<uint32_t> m_uFakeTimestamp = { 0 };
	};

	/////////////////////////
	// Backend Instantiator
	/////////////////////////

	template <>
	bool IBackend::Set<COpenVRBackend>()
	{
		return Set( new COpenVRBackend{} );
	}
}
