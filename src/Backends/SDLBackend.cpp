// For the nested case, reads input from the SDL window and send to wayland

#include <X11/Xlib.h>
#include <thread>
#include <mutex>
#include <string>
#include <optional>

#include <linux/input-event-codes.h>
#include <signal.h>

#include "SDL_clipboard.h"
#include "SDL_events.h"
#include "gamescope_shared.h"
#include "main.hpp"
#include "wlserver.hpp"
#include <SDL.h>
#include <SDL_vulkan.h>
#include "rendervulkan.hpp"
#include "steamcompmgr.hpp"
#include "Utils/Defer.h"
#include "refresh_rate.h"

#include "sdlscancodetable.hpp"

static int g_nOldNestedRefresh = 0;
static bool g_bWindowFocused = true;

static int g_nOutputWidthPts = 0;
static int g_nOutputHeightPts = 0;

extern bool g_bForceHDR10OutputDebug;
extern bool steamMode;
extern bool g_bFirstFrame;
extern int g_nPreferredOutputWidth;
extern int g_nPreferredOutputHeight;

namespace gamescope
{
	extern std::shared_ptr<INestedHints::CursorInfo> GetX11HostCursor();

	enum class SDLInitState
	{
		SDLInit_Waiting,
		SDLInit_Success,
		SDLInit_Failure,
	};

	enum SDLCustomEvents
	{
		GAMESCOPE_SDL_EVENT_TITLE,
		GAMESCOPE_SDL_EVENT_ICON,
		GAMESCOPE_SDL_EVENT_VISIBLE,
		GAMESCOPE_SDL_EVENT_GRAB,
		GAMESCOPE_SDL_EVENT_CURSOR,

		GAMESCOPE_SDL_EVENT_COUNT,
	};

	class CSDLConnector final : public IBackendConnector
	{
	public:
		CSDLConnector();
		virtual bool Init();

		virtual ~CSDLConnector();

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
			return "SDLWindow";
		}
        virtual const char *GetMake() const override
		{
			return "Gamescope";
		}
        virtual const char *GetModel() const override
		{
			return "Virtual Display";
		}

		//--

		SDL_Window *GetSDLWindow() const { return m_pWindow; }
		VkSurfaceKHR GetVulkanSurface() const { return m_pVkSurface; }
	private:
		SDL_Window *m_pWindow = nullptr;
		VkSurfaceKHR m_pVkSurface = VK_NULL_HANDLE;
		BackendConnectorHDRInfo m_HDRInfo{};
	};

	class CSDLBackend : public CBaseBackend, public INestedHints
	{
	public:
		CSDLBackend();

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

        virtual int Present( const FrameInfo_t *pFrameInfo, bool bAsync ) override;
        virtual void DirtyState( bool bForce = false, bool bForceModeset = false ) override;
        virtual bool PollState() override;

		virtual std::shared_ptr<BackendBlob> CreateBackendBlob( const std::type_info &type, std::span<const uint8_t> data ) override;

        virtual OwningRc<IBackendFb> ImportDmabufToBackend( wlr_buffer *pBuffer, wlr_dmabuf_attributes *pDmaBuf ) override;
		virtual bool UsesModifiers() const override;
		virtual std::span<const uint64_t> GetSupportedModifiers( uint32_t uDrmFormat ) const override;

        virtual IBackendConnector *GetCurrentConnector() override;
		virtual IBackendConnector *GetConnector( GamescopeScreenType eScreenType ) override;

        virtual bool IsVRRActive() const override;
		virtual bool SupportsPlaneHardwareCursor() const override;

        virtual bool SupportsTearing() const override;
		virtual bool UsesVulkanSwapchain() const override;

		virtual bool IsSessionBased() const override;
		virtual bool SupportsExplicitSync() const override;

		virtual bool IsVisible() const override;

		virtual glm::uvec2 CursorSurfaceSize( glm::uvec2 uvecSize ) const override;

		virtual INestedHints *GetNestedHints() override;

		///////////////////
		// INestedHints
		///////////////////

        virtual void SetCursorImage( std::shared_ptr<INestedHints::CursorInfo> info ) override;
        virtual void SetRelativeMouseMode( bool bRelative ) override;
        virtual void SetVisible( bool bVisible ) override;
        virtual void SetTitle( std::shared_ptr<std::string> szTitle ) override;
        virtual void SetIcon( std::shared_ptr<std::vector<uint32_t>> uIconPixels ) override;
        virtual void SetSelection( std::shared_ptr<std::string> szContents, GamescopeSelection eSelection ) override;
        virtual std::shared_ptr<INestedHints::CursorInfo> GetHostCursor() override;
	protected:
		virtual void OnBackendBlobDestroyed( BackendBlob *pBlob ) override;
	private:
		void SDLThreadFunc();

		uint32_t GetUserEventIndex( SDLCustomEvents eEvent ) const;
		void PushUserEvent( SDLCustomEvents eEvent );

		bool m_bShown = false;
		CSDLConnector m_Connector; // Window.
		uint32_t m_uUserEventIdBase = 0u;
		std::vector<const char *> m_pszInstanceExtensions;

		std::thread m_SDLThread;
		std::atomic<SDLInitState> m_eSDLInit = { SDLInitState::SDLInit_Waiting };

		std::atomic<bool> m_bApplicationGrabbed = { false };
		std::atomic<bool> m_bApplicationVisible = { false };
		std::atomic<std::shared_ptr<INestedHints::CursorInfo>> m_pApplicationCursor;
		std::atomic<std::shared_ptr<std::string>> m_pApplicationTitle;
		std::atomic<std::shared_ptr<std::vector<uint32_t>>> m_pApplicationIcon;
		SDL_Surface *m_pIconSurface = nullptr;
		SDL_Surface *m_pCursorSurface = nullptr;
		SDL_Cursor *m_pCursor = nullptr;
	};

	//////////////////
	// CSDLConnector
	//////////////////

	CSDLConnector::CSDLConnector()
	{
	}

	CSDLConnector::~CSDLConnector()
	{
		if ( m_pWindow )
			SDL_DestroyWindow( m_pWindow );
	}

	bool CSDLConnector::Init()
	{
		g_nOutputWidth = g_nPreferredOutputWidth;
		g_nOutputHeight = g_nPreferredOutputHeight;
		g_nOutputRefresh = g_nNestedRefresh;

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
			g_nOutputRefresh = gamescope::ConvertHztomHz( 60 );

		uint32_t uSDLWindowFlags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_ALLOW_HIGHDPI;

		if ( g_bBorderlessOutputWindow == true )
			uSDLWindowFlags |= SDL_WINDOW_BORDERLESS;

		if ( g_bFullscreen == true )
			uSDLWindowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

		if ( g_bGrabbed == true )
			uSDLWindowFlags |= SDL_WINDOW_KEYBOARD_GRABBED;

		m_pWindow = SDL_CreateWindow(
			"gamescope",
			SDL_WINDOWPOS_UNDEFINED_DISPLAY( g_nNestedDisplayIndex ),
			SDL_WINDOWPOS_UNDEFINED_DISPLAY( g_nNestedDisplayIndex ),
			g_nOutputWidth,
			g_nOutputHeight,
			uSDLWindowFlags );

		if ( m_pWindow == nullptr )
			return false;

		if ( !SDL_Vulkan_CreateSurface( m_pWindow, vulkan_get_instance(), &m_pVkSurface ) )
		{
			fprintf(stderr, "SDL_Vulkan_CreateSurface failed: %s", SDL_GetError() );
			return false;
		}

		return true;
	}

	GamescopeScreenType CSDLConnector::GetScreenType() const
	{
		return gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL;
	}
	GamescopePanelOrientation CSDLConnector::GetCurrentOrientation() const
	{
		return GAMESCOPE_PANEL_ORIENTATION_0;
	}
	bool CSDLConnector::SupportsHDR() const
	{
		return GetHDRInfo().IsHDR10();
	}
	bool CSDLConnector::IsHDRActive() const
	{
		// XXX: blah
		return false;
	}
	const BackendConnectorHDRInfo &CSDLConnector::GetHDRInfo() const
	{
		return m_HDRInfo;
	}
	std::span<const BackendMode> CSDLConnector::GetModes() const
	{
		return std::span<const BackendMode>{};
	}

	bool CSDLConnector::SupportsVRR() const
	{
		return false;
	}

	std::span<const uint8_t> CSDLConnector::GetRawEDID() const
	{
		return std::span<const uint8_t>{};
	}
	std::span<const uint32_t> CSDLConnector::GetValidDynamicRefreshRates() const
	{
		return std::span<const uint32_t>{};
	}

	void CSDLConnector::GetNativeColorimetry(
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

	////////////////
	// CSDLBackend
	////////////////

	CSDLBackend::CSDLBackend()
		: m_SDLThread{ [this](){ this->SDLThreadFunc(); } }
	{
	}

	bool CSDLBackend::Init()
	{
		m_eSDLInit.wait( SDLInitState::SDLInit_Waiting );

		return m_eSDLInit == SDLInitState::SDLInit_Success;
	}

	bool CSDLBackend::PostInit()
	{
		return true;
	}

	std::span<const char *const> CSDLBackend::GetInstanceExtensions() const
	{
		return std::span<const char *const>{ m_pszInstanceExtensions.begin(), m_pszInstanceExtensions.end() };
	}

	std::span<const char *const> CSDLBackend::GetDeviceExtensions( VkPhysicalDevice pVkPhysicalDevice ) const
	{
		return std::span<const char *const>{};
	}

	VkImageLayout CSDLBackend::GetPresentLayout() const
	{
		return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	}

	void CSDLBackend::GetPreferredOutputFormat( uint32_t *pPrimaryPlaneFormat, uint32_t *pOverlayPlaneFormat ) const
	{
		*pPrimaryPlaneFormat = VulkanFormatToDRM( VK_FORMAT_A2B10G10R10_UNORM_PACK32 );
		*pOverlayPlaneFormat = VulkanFormatToDRM( VK_FORMAT_B8G8R8A8_UNORM );
	}

	bool CSDLBackend::ValidPhysicalDevice( VkPhysicalDevice pVkPhysicalDevice ) const
	{
		return true;
	}

	int CSDLBackend::Present( const FrameInfo_t *pFrameInfo, bool bAsync )
	{
		// TODO: Resolve const crap
		std::optional oCompositeResult = vulkan_composite( (FrameInfo_t *)pFrameInfo, nullptr, false );
		if ( !oCompositeResult )
			return -EINVAL;

		vulkan_present_to_window();

		// TODO: Hook up PresentationFeedback.

		// Wait for the composite result on our side *after* we
		// commit the buffer to the compositor to avoid a bubble.
		vulkan_wait( *oCompositeResult, true );

		GetVBlankTimer().UpdateWasCompositing( true );
		GetVBlankTimer().UpdateLastDrawTime( get_time_in_nanos() - g_SteamCompMgrVBlankTime.ulWakeupTime );

		return 0;
	}
	void CSDLBackend::DirtyState( bool bForce, bool bForceModeset )
	{
	}
	bool CSDLBackend::PollState()
	{
		return false;
	}

	std::shared_ptr<BackendBlob> CSDLBackend::CreateBackendBlob( const std::type_info &type, std::span<const uint8_t> data )
	{
		return std::make_shared<BackendBlob>( data );
	}

	OwningRc<IBackendFb> CSDLBackend::ImportDmabufToBackend( wlr_buffer *pBuffer, wlr_dmabuf_attributes *pDmaBuf )
	{
		return new CBaseBackendFb();
	}

	bool CSDLBackend::UsesModifiers() const
	{
		return false;
	}
	std::span<const uint64_t> CSDLBackend::GetSupportedModifiers( uint32_t uDrmFormat ) const
	{
		return std::span<const uint64_t>{};
	}

	IBackendConnector *CSDLBackend::GetCurrentConnector()
	{
		return &m_Connector;
	}
	IBackendConnector *CSDLBackend::GetConnector( GamescopeScreenType eScreenType )
	{
		if ( eScreenType == GAMESCOPE_SCREEN_TYPE_INTERNAL )
			return &m_Connector;

		return nullptr;
	}
	bool CSDLBackend::IsVRRActive() const
	{
		return false;
	}

	bool CSDLBackend::SupportsPlaneHardwareCursor() const
	{
		// We use the nested hints cursor stuff.
		// Not our own plane.
		return false;
	}

	bool CSDLBackend::SupportsTearing() const
	{
		return false;
	}
	bool CSDLBackend::UsesVulkanSwapchain() const
	{
		return true;
	}

	bool CSDLBackend::IsSessionBased() const
	{
		return false;
	}

	bool CSDLBackend::SupportsExplicitSync() const
	{
		// We use a Vulkan swapchain, so yes.
		return true;
	}

	bool CSDLBackend::IsVisible() const
	{
		return true;
	}

	glm::uvec2 CSDLBackend::CursorSurfaceSize( glm::uvec2 uvecSize ) const
	{
		return uvecSize;
	}

	INestedHints *CSDLBackend::GetNestedHints()
	{
		return this;
	}

	///////////////////
	// INestedHints
	///////////////////

	void CSDLBackend::SetCursorImage( std::shared_ptr<INestedHints::CursorInfo> info )
	{
		m_pApplicationCursor = info;
		PushUserEvent( GAMESCOPE_SDL_EVENT_CURSOR );
	}
	void CSDLBackend::SetRelativeMouseMode( bool bRelative )
	{
		m_bApplicationGrabbed = bRelative;
		PushUserEvent( GAMESCOPE_SDL_EVENT_GRAB );
	}
	void CSDLBackend::SetVisible( bool bVisible )
	{
		m_bApplicationVisible = bVisible;
		PushUserEvent( GAMESCOPE_SDL_EVENT_VISIBLE );
	}
	void CSDLBackend::SetTitle( std::shared_ptr<std::string> szTitle )
	{
		m_pApplicationTitle = szTitle;
		PushUserEvent( GAMESCOPE_SDL_EVENT_TITLE );
	}
	void CSDLBackend::SetIcon( std::shared_ptr<std::vector<uint32_t>> uIconPixels )
	{
		m_pApplicationIcon = uIconPixels;
		PushUserEvent( GAMESCOPE_SDL_EVENT_ICON );
	}
	void CSDLBackend::SetSelection( std::shared_ptr<std::string> szContents, GamescopeSelection eSelection )
	{
		if (eSelection == GAMESCOPE_SELECTION_CLIPBOARD)
			SDL_SetClipboardText(szContents->c_str());
		else if (eSelection == GAMESCOPE_SELECTION_PRIMARY)
			SDL_SetPrimarySelectionText(szContents->c_str());
	}
	std::shared_ptr<INestedHints::CursorInfo> CSDLBackend::GetHostCursor()
	{
		return GetX11HostCursor();
	}

	void CSDLBackend::OnBackendBlobDestroyed( BackendBlob *pBlob )
	{
		// Do nothing.
	}

	void CSDLBackend::SDLThreadFunc()
	{
		pthread_setname_np( pthread_self(), "gamescope-sdl" );

		m_uUserEventIdBase = SDL_RegisterEvents( GAMESCOPE_SDL_EVENT_COUNT );

		SDL_SetHint( SDL_HINT_APP_NAME, "Gamescope" );
		SDL_SetHint( SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1" );

		if ( SDL_Init( SDL_INIT_VIDEO | SDL_INIT_EVENTS ) != 0 )
		{
			m_eSDLInit = SDLInitState::SDLInit_Failure;
			m_eSDLInit.notify_all();
			return;
		}

		if ( SDL_Vulkan_LoadLibrary( nullptr ) != 0 )
		{
			fprintf(stderr, "SDL_Vulkan_LoadLibrary failed: %s\n", SDL_GetError());
			m_eSDLInit = SDLInitState::SDLInit_Failure;
			m_eSDLInit.notify_all();
			return;
		}

		unsigned int uExtCount = 0;
		SDL_Vulkan_GetInstanceExtensions( nullptr, &uExtCount, nullptr );
		m_pszInstanceExtensions.resize( uExtCount );
		SDL_Vulkan_GetInstanceExtensions( nullptr, &uExtCount, m_pszInstanceExtensions.data() );

		if ( !m_Connector.Init() )
		{
			m_eSDLInit = SDLInitState::SDLInit_Failure;
			m_eSDLInit.notify_all();
			return;
		}

		if ( !vulkan_init( vulkan_get_instance(), m_Connector.GetVulkanSurface() ) )
		{
			m_eSDLInit = SDLInitState::SDLInit_Failure;
			m_eSDLInit.notify_all();
			return;
		}

		if ( !wlsession_init() )
		{
			fprintf( stderr, "Failed to initialize Wayland session\n" );
			m_eSDLInit = SDLInitState::SDLInit_Failure;
			m_eSDLInit.notify_all();
			return;
		}

		// Update g_nOutputWidthPts.
		{
			int width, height;
			SDL_GetWindowSize( m_Connector.GetSDLWindow(), &width, &height );
			g_nOutputWidthPts = width;
			g_nOutputHeightPts = height;

		#if SDL_VERSION_ATLEAST(2, 26, 0)
			SDL_GetWindowSizeInPixels( m_Connector.GetSDLWindow(), &width, &height );
		#endif
			g_nOutputWidth = width;
			g_nOutputHeight = height;
		}

		if ( g_bForceRelativeMouse )
		{
			SDL_SetRelativeMouseMode( SDL_TRUE );
			m_bApplicationGrabbed = true;
		}

		SDL_SetHint( SDL_HINT_TOUCH_MOUSE_EVENTS, "0" );

		g_nOldNestedRefresh = g_nNestedRefresh;

		m_eSDLInit = SDLInitState::SDLInit_Success;
		m_eSDLInit.notify_all();

		static uint32_t fake_timestamp = 0;

		wlserver.bWaylandServerRunning.wait( false );

		SDL_Event event;
		while( SDL_WaitEvent( &event ) )
		{
			fake_timestamp++;

			switch( event.type )
			{
				case SDL_CLIPBOARDUPDATE:
				{
					char *pClipBoard = SDL_GetClipboardText();
					char *pPrimarySelection = SDL_GetPrimarySelectionText();

					gamescope_set_selection(pClipBoard, GAMESCOPE_SELECTION_CLIPBOARD);
					gamescope_set_selection(pPrimarySelection, GAMESCOPE_SELECTION_PRIMARY);

					SDL_free(pClipBoard);
					SDL_free(pPrimarySelection);
				}
				break;

				case SDL_MOUSEMOTION:
				{
					if ( m_bApplicationGrabbed )
					{
						if ( g_bWindowFocused )
						{
							wlserver_lock();
							wlserver_mousemotion( event.motion.xrel, event.motion.yrel, fake_timestamp );
							wlserver_unlock();
						}
					}
					else
					{
						wlserver_lock();
						wlserver_touchmotion(
							event.motion.x / float(g_nOutputWidthPts),
							event.motion.y / float(g_nOutputHeightPts),
							0,
							fake_timestamp );
						wlserver_unlock();
					}
				}
				break;

				case SDL_MOUSEBUTTONDOWN:
				case SDL_MOUSEBUTTONUP:
				{
					wlserver_lock();
					wlserver_mousebutton( SDLButtonToLinuxButton( event.button.button ),
										event.button.state == SDL_PRESSED,
										fake_timestamp );
					wlserver_unlock();
				}
				break;

				case SDL_MOUSEWHEEL:
				{
					wlserver_lock();
					wlserver_mousewheel( -event.wheel.x, -event.wheel.y, fake_timestamp );
					wlserver_unlock();
				}
				break;

				case SDL_FINGERMOTION:
				{
					wlserver_lock();
					wlserver_touchmotion( event.tfinger.x, event.tfinger.y, event.tfinger.fingerId, fake_timestamp );
					wlserver_unlock();
				}
				break;

				case SDL_FINGERDOWN:
				{
					wlserver_lock();
					wlserver_touchdown( event.tfinger.x, event.tfinger.y, event.tfinger.fingerId, fake_timestamp );
					wlserver_unlock();
				}
				break;

				case SDL_FINGERUP:
				{
					wlserver_lock();
					wlserver_touchup( event.tfinger.fingerId, fake_timestamp );
					wlserver_unlock();
				}
				break;

				case SDL_KEYDOWN:
				{
					// If this keydown event is super + one of the shortcut keys, consume the keydown event, since the corresponding keyup
					// event will be consumed by the next case statement when the user releases the key
					if ( event.key.keysym.mod & KMOD_LGUI )
					{
						uint32_t key = SDLScancodeToLinuxKey( event.key.keysym.scancode );
						const uint32_t shortcutKeys[] = {KEY_F, KEY_N, KEY_B, KEY_U, KEY_Y, KEY_I, KEY_O, KEY_S, KEY_G};
						const bool isShortcutKey = std::find(std::begin(shortcutKeys), std::end(shortcutKeys), key) != std::end(shortcutKeys);
						if ( isShortcutKey )
						{
							break;
						}
					}
				}
				[[fallthrough]];
				case SDL_KEYUP:
				{
					uint32_t key = SDLScancodeToLinuxKey( event.key.keysym.scancode );

					if ( event.type == SDL_KEYUP && ( event.key.keysym.mod & KMOD_LGUI ) )
					{
						bool handled = true;
						switch ( key )
						{
							case KEY_F:
								g_bFullscreen = !g_bFullscreen;
								SDL_SetWindowFullscreen( m_Connector.GetSDLWindow(), g_bFullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0 );
								break;
							case KEY_N:
								g_wantedUpscaleFilter = GamescopeUpscaleFilter::PIXEL;
								break;
							case KEY_B:
								g_wantedUpscaleFilter = GamescopeUpscaleFilter::LINEAR;
								break;
							case KEY_K:
								g_wantedDownscaleFilter = (g_wantedDownscaleFilter == GamescopeDownscaleFilter::BICUBIC) ?
								GamescopeDownscaleFilter::LINEAR : GamescopeDownscaleFilter::BICUBIC;
								break;
							case KEY_U:
								g_wantedUpscaleFilter = (g_wantedUpscaleFilter == GamescopeUpscaleFilter::FSR) ?
									GamescopeUpscaleFilter::LINEAR : GamescopeUpscaleFilter::FSR;
								break;
							case KEY_Y:
								g_wantedUpscaleFilter = (g_wantedUpscaleFilter == GamescopeUpscaleFilter::NIS) ? 
									GamescopeUpscaleFilter::LINEAR : GamescopeUpscaleFilter::NIS;
								break;
							case KEY_I:
								g_upscaleFilterSharpness = std::min(20, g_upscaleFilterSharpness + 1);
								break;
							case KEY_O:
								g_upscaleFilterSharpness = std::max(0, g_upscaleFilterSharpness - 1);
								break;
							case KEY_S:
								gamescope::CScreenshotManager::Get().TakeScreenshot( true );
								break;
							case KEY_G:
								g_bGrabbed = !g_bGrabbed;
								SDL_SetWindowKeyboardGrab( m_Connector.GetSDLWindow(), g_bGrabbed ? SDL_TRUE : SDL_FALSE );

								SDL_Event event;
								event.type = GetUserEventIndex( GAMESCOPE_SDL_EVENT_TITLE );
								SDL_PushEvent( &event );
								break;
							default:
								handled = false;
						}
						if ( handled )
						{
							break;
						}
					}

					// On Wayland, clients handle key repetition
					if ( event.key.repeat )
						break;

					wlserver_lock();
					wlserver_key( key, event.type == SDL_KEYDOWN, fake_timestamp );
					wlserver_unlock();
				}
				break;

				case SDL_WINDOWEVENT:
				{
					switch( event.window.event )
					{
						case SDL_WINDOWEVENT_CLOSE:
							raise( SIGTERM );
							break;
						default:
							break;
						case SDL_WINDOWEVENT_SIZE_CHANGED:
							int width, height;
							SDL_GetWindowSize( m_Connector.GetSDLWindow(), &width, &height );
							g_nOutputWidthPts = width;
							g_nOutputHeightPts = height;

#if SDL_VERSION_ATLEAST(2, 26, 0)
							SDL_GetWindowSizeInPixels( m_Connector.GetSDLWindow(), &width, &height );
#endif
							g_nOutputWidth = width;
							g_nOutputHeight = height;

						[[fallthrough]];
						case SDL_WINDOWEVENT_MOVED:
						case SDL_WINDOWEVENT_SHOWN:
							{
								int display_index = 0;
								SDL_DisplayMode mode = { SDL_PIXELFORMAT_UNKNOWN, 0, 0, 0, 0 };

								display_index = SDL_GetWindowDisplayIndex( m_Connector.GetSDLWindow() );
								if ( SDL_GetDesktopDisplayMode( display_index, &mode ) == 0 )
								{
									g_nOutputRefresh = ConvertHztomHz( mode.refresh_rate );
								}
							}
							break;
						case SDL_WINDOWEVENT_FOCUS_LOST:
							g_nNestedRefresh = g_nNestedUnfocusedRefresh;
							g_bWindowFocused = false;
							break;
						case SDL_WINDOWEVENT_FOCUS_GAINED:
							g_nNestedRefresh = g_nOldNestedRefresh;
							g_bWindowFocused = true;
							break;
						case SDL_WINDOWEVENT_EXPOSED:
							force_repaint();
							break;
					}
				}
				break;

				default:
				{
					if ( event.type == GetUserEventIndex( GAMESCOPE_SDL_EVENT_VISIBLE ) )
					{
						bool bVisible = m_bApplicationVisible;

						// If we are Steam Mode in nested, show the window
						// whenever we have had a first frame to match
						// what we do in embedded with Steam for testing
						// held commits, etc.
						if ( steamMode )
							bVisible |= !g_bFirstFrame;

						if ( m_bShown != bVisible )
						{
							m_bShown = bVisible;

							if ( m_bShown )
							{
								SDL_ShowWindow( m_Connector.GetSDLWindow() );
							}
							else
							{
								SDL_HideWindow( m_Connector.GetSDLWindow() );
							}
						}
					}
					else if ( event.type == GetUserEventIndex( GAMESCOPE_SDL_EVENT_TITLE ) )
					{
						std::shared_ptr<std::string> pAppTitle = m_pApplicationTitle;

						std::string szTitle = pAppTitle ? *pAppTitle : "gamescope";
						if ( g_bGrabbed )
							szTitle += " (grabbed)";
						SDL_SetWindowTitle( m_Connector.GetSDLWindow(), szTitle.c_str() );

						szTitle = "Title: " + szTitle;
						SDL_SetHint(SDL_HINT_SCREENSAVER_INHIBIT_ACTIVITY_NAME, szTitle.c_str() );
						SDL_DisableScreenSaver();
					}
					else if ( event.type == GetUserEventIndex( GAMESCOPE_SDL_EVENT_ICON ) )
					{
						std::shared_ptr<std::vector<uint32_t>> pIcon = m_pApplicationIcon;

						if ( m_pIconSurface )
						{
							SDL_FreeSurface( m_pIconSurface );
							m_pIconSurface = nullptr;
						}

						if ( pIcon && pIcon->size() >= 3 )
						{
							const uint32_t uWidth = (*pIcon)[0];
							const uint32_t uHeight = (*pIcon)[1];

							m_pIconSurface = SDL_CreateRGBSurfaceFrom(
								&(*pIcon)[2],
								uWidth, uHeight,
								32, uWidth * sizeof(uint32_t),
								0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
						}

						SDL_SetWindowIcon( m_Connector.GetSDLWindow(), m_pIconSurface );
					}
					else if ( event.type == GetUserEventIndex( GAMESCOPE_SDL_EVENT_GRAB ) )
					{
						SDL_SetRelativeMouseMode( m_bApplicationGrabbed ? SDL_TRUE : SDL_FALSE );
					}
					else if ( event.type == GetUserEventIndex( GAMESCOPE_SDL_EVENT_CURSOR ) )
					{
						std::shared_ptr<INestedHints::CursorInfo> pCursorInfo = m_pApplicationCursor;

						if ( m_pCursorSurface )
						{
							SDL_FreeSurface( m_pCursorSurface );
							m_pCursorSurface = nullptr;
						}

						if ( m_pCursor )
						{
							SDL_FreeCursor( m_pCursor );
							m_pCursor = nullptr;
						}

						if ( pCursorInfo )
						{
							m_pCursorSurface = SDL_CreateRGBSurfaceFrom(
								pCursorInfo->pPixels.data(),
								pCursorInfo->uWidth,
								pCursorInfo->uHeight,
								32,
								pCursorInfo->uWidth * sizeof(uint32_t),
								0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);

							m_pCursor = SDL_CreateColorCursor( m_pCursorSurface, pCursorInfo->uXHotspot, pCursorInfo->uYHotspot );
						}

						SDL_SetCursor( m_pCursor );
					}
				}
				break;
			}
		}
	}

	uint32_t CSDLBackend::GetUserEventIndex( SDLCustomEvents eEvent ) const
	{
		return m_uUserEventIdBase + uint32_t( eEvent );
	}

	void CSDLBackend::PushUserEvent( SDLCustomEvents eEvent )
	{
		SDL_Event event =
		{
			.user =
			{
				.type = GetUserEventIndex( eEvent ),
			},
		};
		SDL_PushEvent( &event );
	}

	/////////////////////////
	// Backend Instantiator
	/////////////////////////

	template <>
	bool IBackend::Set<CSDLBackend>()
	{
		return Set( new CSDLBackend{} );
	}
}
