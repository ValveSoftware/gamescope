#include "backend.h"
#include "rendervulkan.hpp"
#include "wlserver.hpp"
#include "refresh_rate.h"

extern int g_nPreferredOutputWidth;
extern int g_nPreferredOutputHeight;

namespace gamescope
{
    class CHeadlessConnector final : public CBaseBackendConnector
    {
    public:
        CHeadlessConnector()
        {
        }
        virtual ~CHeadlessConnector()
        {
        }

        virtual gamescope::GamescopeScreenType GetScreenType() const override
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
		virtual bool IsVRRActive() const override
		{
			return false;
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
            return "Headless";
        }
        virtual const char *GetMake() const override
        {
            return "Gamescope";
        }
        virtual const char *GetModel() const override
        {
            return "Virtual Display";
        }

		virtual int Present( const FrameInfo_t *pFrameInfo, bool bAsync ) override
		{
            return 0;
		}

    private:
        BackendConnectorHDRInfo m_HDRInfo{};
    };

	class CHeadlessBackend final : public CBaseBackend
	{
	public:
		CHeadlessBackend()
		{
		}

		virtual ~CHeadlessBackend()
		{
		}

		virtual bool Init() override
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
				g_nOutputRefresh = ConvertHztomHz( 60 );

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

		virtual bool PostInit() override
		{
			return true;
		}

        virtual std::span<const char *const> GetInstanceExtensions() const override
		{
			return std::span<const char *const>{};
		}
        virtual std::span<const char *const> GetDeviceExtensions( VkPhysicalDevice pVkPhysicalDevice ) const override
		{
			return std::span<const char *const>{};
		}
        virtual VkImageLayout GetPresentLayout() const override
		{
			return VK_IMAGE_LAYOUT_GENERAL;
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
			return new CBaseBackendFb();
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
			return true;
		}

		virtual bool IsVisible() const override
		{
			return true;
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

	protected:

		virtual void OnBackendBlobDestroyed( BackendBlob *pBlob ) override
		{
		}

	private:

        CHeadlessConnector m_Connector;
	};

	/////////////////////////
	// Backend Instantiator
	/////////////////////////

	template <>
	bool IBackend::Set<CHeadlessBackend>()
	{
		return Set( new CHeadlessBackend{} );
	}

}