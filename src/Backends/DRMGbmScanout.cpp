#include "DRMGbmScanout.h"

#include "drm_include.h"
#include "log.hpp"
#include "Utils/Algorithm.h"
#include "Utils/Defer.h"

#include <cstring>

#if HAVE_GBM
#include <gbm.h>
#endif

namespace gamescope
{
	static LogScope gbm_log( "gbm_scanout" );

	static bool IsNvidiaDrm( int nDrmFd )
	{
		bool bIsNvidia = false;
		if ( drmVersion *pVersion = drmGetVersion( nDrmFd ) )
		{
			bIsNvidia = pVersion->name && strcmp( pVersion->name, "nvidia-drm" ) == 0;
			drmFreeVersion( pVersion );
		}
		return bIsNvidia;
	}

	CGbmScanoutAllocator::~CGbmScanoutAllocator()
	{
		Shutdown();
	}

	bool CGbmScanoutAllocator::Init( int nDrmFd )
	{
		if ( !IsNvidiaDrm( nDrmFd ) )
			return false;
#if HAVE_GBM
		m_pGbmDevice = gbm_create_device( nDrmFd );
		if ( !m_pGbmDevice )
			gbm_log.errorf( "Failed to create GBM device; Vulkan scanout allocation will be used." );
#else
		gbm_log.errorf( "Gamescope was built without GBM support; Vulkan scanout allocation will be used on NVIDIA." );
#endif
		return m_pGbmDevice != nullptr;
	}

	void CGbmScanoutAllocator::Shutdown()
	{
#if HAVE_GBM
		if ( m_pGbmDevice )
		{
			gbm_device_destroy( m_pGbmDevice );
			m_pGbmDevice = nullptr;
		}
#endif
	}

	bool CGbmScanoutAllocator::CreateScanoutDmabuf( uint32_t uWidth, uint32_t uHeight, uint32_t uDrmFormat,
	                                                std::span<const uint64_t> ulModifiers,
	                                                wlr_dmabuf_attributes *pDmaBuf )
	{
		*pDmaBuf = {};
#if HAVE_GBM
		if ( !m_pGbmDevice || ulModifiers.empty() )
			return false;

		gbm_bo *pBo = gbm_bo_create_with_modifiers2(
			m_pGbmDevice, uWidth, uHeight, uDrmFormat,
			ulModifiers.data(), ulModifiers.size(),
			GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT );
		if ( !pBo )
		{
			gbm_log.errorf_errno( "Failed to allocate GBM scanout buffer" );
			return false;
		}
		defer( gbm_bo_destroy( pBo ) );

		*pDmaBuf = {
			.width = int32_t( uWidth ),
			.height = int32_t( uHeight ),
			.format = uDrmFormat,
			.modifier = gbm_bo_get_modifier( pBo ),
		};

		const int nPlanes = gbm_bo_get_plane_count( pBo );
		if ( nPlanes < 1 || nPlanes > WLR_DMABUF_MAX_PLANES ||
		     !Algorithm::Contains( ulModifiers, pDmaBuf->modifier ) )
		{
			*pDmaBuf = {};
			return false;
		}

		for ( int i = 0; i < nPlanes; i++ )
		{
			const int nFd = gbm_bo_get_fd_for_plane( pBo, i );
			if ( nFd < 0 )
			{
				gbm_log.errorf_errno( "Failed to export GBM scanout buffer" );
				wlr_dmabuf_attributes_finish( pDmaBuf );
				*pDmaBuf = {};
				return false;
			}

			pDmaBuf->fd[i] = nFd;
			pDmaBuf->offset[i] = gbm_bo_get_offset( pBo, i );
			pDmaBuf->stride[i] = gbm_bo_get_stride_for_plane( pBo, i );
			pDmaBuf->n_planes = i + 1;
		}

		return true;
#else
		return false;
#endif
	}
}
