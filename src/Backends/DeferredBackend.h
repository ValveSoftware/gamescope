#include "backend.h"
#include "refresh_rate.h"
#include "steamcompmgr.hpp"

#include <cassert>
#include <shared_mutex>

extern int g_nPreferredOutputWidth;
extern int g_nPreferredOutputHeight;

std::span<const uint64_t> GetSupportedSampleModifiers( uint32_t uDrmFormat );

namespace gamescope
{
    class CDeferredBackend;

    class CDeferredFb final : public CBaseBackendFb
    {
    public:
        CDeferredFb( CDeferredBackend *pDeferredBackend, struct wlr_dmabuf_attributes *attributes )
            : m_pDeferredBackend{ pDeferredBackend }
        {
            wlr_dmabuf_attributes_copy( &m_attributes, attributes );
        }

        ~CDeferredFb()
        {
            wlr_dmabuf_attributes_finish( &m_attributes );
        }
        
        IBackendFb *Unwrap() override;
    private:
        CDeferredBackend *m_pDeferredBackend = nullptr;
        struct wlr_dmabuf_attributes m_attributes;
        OwningRc<IBackendFb> m_pChild;
    };

	class CDeferredBackend final : public CBaseBackend
	{
	public:
		CDeferredBackend( IBackend *pChild )
            : m_pChild{ pChild }
		{
		}

		virtual ~CDeferredBackend()
		{
            if ( m_pChild )
            {
                delete m_pChild;
                m_pChild = nullptr;
            }
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
                fprintf( stderr, "Failed to initialize deferred backend\n" );
                return false;
            }

            TryInittingChild();

			return true;
		}

		virtual bool PostInit() override
		{
            {
                std::shared_lock lock{ m_mutInit };
                m_bDonePostInit = true;

                if ( m_bInittedChild )
                    return m_pChild->PostInit();
            }

            return true;
		}

        virtual std::span<const char *const> GetInstanceExtensions() const override
		{
            // Basically what's needed to support SDL + OpenVR.
            static const std::array<const char *const, 8> requiredInstanceExts
            {
                VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
                VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
                VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME,
                VK_KHR_SURFACE_EXTENSION_NAME,
                "VK_KHR_xcb_surface",
                "VK_KHR_xlib_surface",
                "VK_KHR_wayland_surface",
                VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
            };
			return std::span<const char *const>{ requiredInstanceExts };
		}
        virtual std::span<const char *const> GetDeviceExtensions( VkPhysicalDevice pVkPhysicalDevice ) const override
		{
            // Basically what's needed to support OpenVR.
            static const std::array<const char *const, 8> requiredDeviceExts
            {
                VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
                VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
                VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
                VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
                VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
                VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
                VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
            };
			return std::span<const char *const>{ requiredDeviceExts };
		}
        virtual VkImageLayout GetPresentLayout() const override
		{
            {
                std::shared_lock lock{ m_mutInit };
                if ( m_bInittedChild )
                    return m_pChild->GetPresentLayout();
            }

			return VK_IMAGE_LAYOUT_GENERAL;
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

		virtual void DirtyState( bool bForce, bool bForceModeset ) override
		{
            {
                std::shared_lock lock{ m_mutInit };
                if ( m_bInittedChild )
                    return m_pChild->DirtyState( bForce, bForceModeset );
            }
		}

		virtual bool PollState() override
		{
            TryInittingChild();
            {
                std::shared_lock lock{ m_mutInit };
                if ( m_bInittedChild )
                    return m_pChild->PollState() || m_bJustInittedPoll;
            }
			return false;
		}

		virtual std::shared_ptr<BackendBlob> CreateBackendBlob( const std::type_info &type, std::span<const uint8_t> data ) override
		{
            // Only dummy backend blobs supported.
			return std::make_shared<BackendBlob>( data );
		}

		virtual OwningRc<IBackendFb> ImportDmabufToBackend( wlr_dmabuf_attributes *pDmaBuf ) override
		{
			return new CDeferredFb( this, pDmaBuf );
		}

		virtual bool UsesModifiers() const override
		{
            return true;
		}
		virtual std::span<const uint64_t> GetSupportedModifiers( uint32_t uDrmFormat ) const override
		{
			return GetSupportedSampleModifiers( uDrmFormat );
		}

		virtual IBackendConnector *GetCurrentConnector() override
		{
            {
                std::shared_lock lock{ m_mutInit };
                if ( m_bInittedChild )
                    return m_pChild->GetCurrentConnector();
            }

            return nullptr;
		}
		virtual IBackendConnector *GetConnector( GamescopeScreenType eScreenType ) override
		{
            {
                std::shared_lock lock{ m_mutInit };
                if ( m_bInittedChild )
                    return m_pChild->GetConnector( eScreenType );
            }
            
			return nullptr;
		}

		virtual bool SupportsPlaneHardwareCursor() const override
		{
            // Doesn't need to be 'initted' for this check.
            return m_pChild->SupportsPlaneHardwareCursor();
		}

		virtual bool SupportsTearing() const override
		{
            {
                std::shared_lock lock{ m_mutInit };
                if ( m_bInittedChild )
                    return m_pChild->SupportsTearing();
            }
            
			return false;
		}

		virtual bool UsesVulkanSwapchain() const override
		{
            // Doesn't need to be 'initted' for this check.
            return m_pChild->UsesVulkanSwapchain();
		}

        virtual bool IsSessionBased() const override
		{
            // Doesn't need to be 'initted' for this check.
            return m_pChild->IsSessionBased();
		}

		virtual bool SupportsExplicitSync() const override
		{
            // Doesn't need to be 'initted' for this check.
            return m_pChild->SupportsExplicitSync();
		}

		virtual bool IsPaused() const override
		{
            {
                std::shared_lock lock{ m_mutInit };
                if ( m_bInittedChild )
                    return m_pChild->IsPaused();
            }

            // We are always "paused" when not initted.
            // Don't do any commits!
			return true;
		}

		virtual bool IsVisible() const override
		{
            {
                std::shared_lock lock{ m_mutInit };
                if ( m_bInittedChild )
                    return m_pChild->IsVisible();
            }

			return true;
		}

		virtual glm::uvec2 CursorSurfaceSize( glm::uvec2 uvecSize ) const override
		{
            {
                std::shared_lock lock{ m_mutInit };
                if ( m_bInittedChild )
                    return m_pChild->CursorSurfaceSize( uvecSize );
            }

			return uvecSize;
		}

		virtual bool HackTemporarySetDynamicRefresh( int nRefresh ) override
		{
            {
                std::shared_lock lock{ m_mutInit };
                if ( m_bInittedChild )
                    return m_pChild->HackTemporarySetDynamicRefresh( nRefresh );
            }

			return false;
		}

		virtual void HackUpdatePatchedEdid() override
		{
            {
                std::shared_lock lock{ m_mutInit };
                if ( m_bInittedChild )
                    return m_pChild->HackUpdatePatchedEdid();
            }
		}

        virtual bool NeedsFrameSync() const override
        {
            // Deferred backends do not support frame sync.
            return false;
        }

        virtual TouchClickMode GetTouchClickMode() override
        {
            // Doesn't need to be 'initted' for this check.
            return m_pChild->GetTouchClickMode();
        }

        virtual void DumpDebugInfo() override
        {
            // Doesn't need to be 'initted' for this check.
            return m_pChild->DumpDebugInfo();
        }

        virtual bool UsesVirtualConnectors() override
        {
            // Doesn't need to be 'initted' for this check.
            return m_pChild->UsesVirtualConnectors();
        }
        virtual std::shared_ptr<IBackendConnector> CreateVirtualConnector( uint64_t ulVirtualConnectorKey ) override
        {
            {
                std::shared_lock lock{ m_mutInit };
                if ( m_bInittedChild )
                    return m_pChild->CreateVirtualConnector( ulVirtualConnectorKey );
            }

            return nullptr;
        }

        virtual void NotifyPhysicalInput( InputType eInputType ) override
        {
            {
                std::shared_lock lock{ m_mutInit };
                if ( m_bInittedChild )
                    return m_pChild->NotifyPhysicalInput( eInputType );
            }
        }

        virtual bool SupportsVROverlayForwarding() override
        {
            // Doesn't need to be 'initted' for this check.
            return m_pChild->SupportsVROverlayForwarding();
        }
        virtual void ForwardFramebuffer( std::shared_ptr<IBackendPlane> &pPlane, IBackendFb *pFramebuffer, const void *pData ) override
        {
            {
                std::shared_lock lock{ m_mutInit };
                if ( m_bInittedChild )
                    return m_pChild->ForwardFramebuffer( pPlane, pFramebuffer, pData );
            }
        }

        bool IsChildInitted()
        {
            return m_bInittedChild;
        }

        IBackend *GetChild()
        {
            return m_pChild;
        }

        bool NewlyInitted() override
        {
            return m_bJustInittedClient.exchange( false );
        }

	protected:

		virtual void OnBackendBlobDestroyed( BackendBlob *pBlob ) override
		{
		}

	private:

        void TryInittingChild()
        {
            if ( !m_bInittedChild )
            {
                std::unique_lock lock{ m_mutInit };
                if ( !m_bInittedChild )
                {
                    if ( m_pChild->Init() )
                    {
                        m_bInittedChild = true;

                        if ( m_bDonePostInit )
                        {
                            bool bRet = m_pChild->PostInit();
                            assert( bRet && "PostInit failed!" );
                        }

                        m_bJustInittedClient = true;
                        m_bJustInittedPoll = true;
                    }
                }
            }
        }

        IBackend *m_pChild = nullptr;
        mutable std::shared_mutex m_mutInit;
        bool m_bDonePostInit = false;

        std::atomic<bool> m_bInittedChild = { false };
        std::atomic<bool> m_bJustInittedClient = { false };
        std::atomic<bool> m_bJustInittedPoll = { false };
	};

    IBackendFb *CDeferredFb::Unwrap()
    {
        assert( m_pDeferredBackend->IsChildInitted() );

        if ( !m_pChild )
        {
            m_pChild = m_pDeferredBackend->GetChild()->ImportDmabufToBackend( &m_attributes );
        }

        return m_pChild.get();
    }

}
