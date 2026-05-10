#include "WaylandProtocol.h"
#include "WaylandServerLegacy.h"
#include "../Timeline.h"

#include "linux-drm-syncobj-v1-protocol.h"

namespace gamescope::WaylandServer
{

	class CLinuxDrmSyncobjTimeline : public CWaylandResource
	{
	public:
		WL_PROTO_DEFINE( wp_linux_drm_syncobj_timeline_v1, 1 );

		CLinuxDrmSyncobjTimeline( WaylandResourceDesc_t desc, std::shared_ptr<CTimeline> pTimeline )
			: CWaylandResource( desc )
			, m_pTimeline{ std::move( pTimeline ) }
		{
		}

		std::shared_ptr<CTimeline> GetTimeline() const
		{
			return m_pTimeline;
		}

	private:
		std::shared_ptr<CTimeline> m_pTimeline;
	};

	const struct wp_linux_drm_syncobj_timeline_v1_interface CLinuxDrmSyncobjTimeline::Implementation = 
	{
		.destroy = WL_PROTO_DESTROY(),
	};

	class CLinuxDrmSyncobjSurface : public CWaylandResource
	{
	public:
		WL_PROTO_DEFINE( wp_linux_drm_syncobj_surface_v1, 1 );

		CLinuxDrmSyncobjSurface( WaylandResourceDesc_t desc, wlserver_wl_surface_info *pWlSurfaceInfo )
			: CWaylandResource( desc )
			, m_pWlSurfaceInfo{ pWlSurfaceInfo }
		{
		}

		~CLinuxDrmSyncobjSurface()
		{
			Detach();
		}

		bool HasExplicitSync() const
		{
			return m_pAcquireTimeline || m_pReleaseTimeline;
		}

		std::shared_ptr<CAcquireTimelinePoint> ExtractAcquireTimelinePoint() const
		{
			if ( !m_pAcquireTimeline )
				return nullptr;

			return std::make_shared<CAcquireTimelinePoint>( m_pAcquireTimeline, m_ulAcquirePoint );
		}

		std::shared_ptr<CReleaseTimelinePoint> ExtractReleaseTimelinePoint() const
		{
			if ( !m_pReleaseTimeline )
				return nullptr;

			return std::make_shared<CReleaseTimelinePoint>( m_pReleaseTimeline, m_ulReleasePoint );
		}

		void Detach()
		{
			if ( m_pWlSurfaceInfo )
			{
				assert( m_pWlSurfaceInfo->pSyncobjSurface == this );
				m_pWlSurfaceInfo->pSyncobjSurface = nullptr;
			}

			m_pWlSurfaceInfo = nullptr;
		}

	protected:

		void SetAcquirePoint( wl_resource *pWlTimeline, uint32_t uPointHi, uint32_t uPointLo )
		{
			if ( !pWlTimeline )
			{
				m_pAcquireTimeline = nullptr;
				return;
			}

			CLinuxDrmSyncobjTimeline *pTimeline = CWaylandResource::FromWlResource<CLinuxDrmSyncobjTimeline>( pWlTimeline );
			m_pAcquireTimeline = pTimeline->GetTimeline();
			m_ulAcquirePoint = ToUint64( uPointHi, uPointLo );
		}

		void SetReleasePoint( wl_resource *pWlTimeline, uint32_t uPointHi, uint32_t uPointLo )
		{
			if ( !pWlTimeline )
			{
				m_pReleaseTimeline = nullptr;
				return;
			}

			CLinuxDrmSyncobjTimeline *pTimeline = CWaylandResource::FromWlResource<CLinuxDrmSyncobjTimeline>( pWlTimeline );
			m_pReleaseTimeline = pTimeline->GetTimeline();
			m_ulReleasePoint = ToUint64( uPointHi, uPointLo );
		}

	private:
		wlserver_wl_surface_info *m_pWlSurfaceInfo = nullptr;

		std::shared_ptr<CTimeline> m_pAcquireTimeline;
		uint64_t m_ulAcquirePoint = 0;

		std::shared_ptr<CTimeline> m_pReleaseTimeline;
		uint64_t m_ulReleasePoint = 0;
	};

	const struct wp_linux_drm_syncobj_surface_v1_interface CLinuxDrmSyncobjSurface::Implementation = 
	{
		.destroy = WL_PROTO_DESTROY(),
		.set_acquire_point = WL_PROTO( CLinuxDrmSyncobjSurface, SetAcquirePoint ),
		.set_release_point = WL_PROTO( CLinuxDrmSyncobjSurface, SetReleasePoint ),
	};

	class CLinuxDrmSyncobjManager : public CWaylandResource
	{
	public:
		WL_PROTO_DEFINE( wp_linux_drm_syncobj_manager_v1, 1 );
		WL_PROTO_DEFAULT_CONSTRUCTOR();

		void GetSurface( uint32_t uId, wl_resource *pSurfaceResource )
		{
			struct wlr_surface *pWlrSurface = wlr_surface_from_resource( pSurfaceResource );
			struct wlserver_wl_surface_info *pWlSurfaceInfo = get_wl_surface_info( pWlrSurface );
			if ( !pWlSurfaceInfo )
			{
				wl_client_post_implementation_error( m_pClient, "No wl_surface" );
				return;
			}

			if ( pWlSurfaceInfo->pSyncobjSurface )
			{
				wl_resource_post_error( m_pResource,
					WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_ERROR_SURFACE_EXISTS,
					"wp_linux_drm_syncobj_surface_v1 already created for this surface" );
				return;
			}

			pWlSurfaceInfo->pSyncobjSurface = CWaylandResource::Create<CLinuxDrmSyncobjSurface>( m_pClient, m_uVersion, uId, pWlSurfaceInfo );
		}

		void ImportTimeline( uint32_t uId, int32_t nFd )
		{
			// Transfer nFd to a new CTimeline.
			std::shared_ptr<CTimeline> pTimeline = std::make_shared<CTimeline>( nFd );
			if ( !CheckAllocation( pTimeline ) )
				return;

            if ( !pTimeline->IsValid() )
            {
                wl_resource_post_error( m_pResource,
                    WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_ERROR_INVALID_TIMELINE,
                    "Failed to import syncobj timeline" );
                return;
            }

			CWaylandResource::Create<CLinuxDrmSyncobjTimeline>( m_pClient, m_uVersion, uId, std::move( pTimeline ) );
		}

	};

	const struct wp_linux_drm_syncobj_manager_v1_interface CLinuxDrmSyncobjManager::Implementation =
	{
		.destroy = WL_PROTO_DESTROY(),
		.get_surface = WL_PROTO( CLinuxDrmSyncobjManager, GetSurface ),
		.import_timeline = WL_PROTO( CLinuxDrmSyncobjManager, ImportTimeline ),
	};

}
