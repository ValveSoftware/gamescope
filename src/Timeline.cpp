#include <xf86drm.h>
#include <sys/eventfd.h>

#include "Timeline.h"
#include "wlserver.hpp"
#include "rendervulkan.hpp"

#include "wlr_begin.hpp"
#include <wlr/render/drm_syncobj.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include "wlr_end.hpp"

namespace gamescope
{
    static LogScope s_TimelineLog( "timeline" );

    static uint32_t SyncobjFdToHandle( int32_t nFd )
    {
        int32_t nRet;
        uint32_t uHandle = 0;
        if ( ( nRet = drmSyncobjFDToHandle( g_device.drmRenderFd(), nFd, &uHandle ) ) < 0 )
            return 0;

        return uHandle;
    }

    /*static*/ std::shared_ptr<CTimeline> CTimeline::Create( const TimelineCreateDesc_t &desc )
    {
        int nSyncobjFd = -1;
        uint32_t uSyncobjHandle = 0;
        uint64_t ulStartingPoint = desc.ulStartingPoint;

        if ( drmSyncobjCreate( g_device.drmRenderFd(), 0, &uSyncobjHandle ) != 0 )
        {
            return nullptr;
        }

        if ( drmSyncobjTimelineSignal( g_device.drmRenderFd(), &uSyncobjHandle, &ulStartingPoint, 1 ) != 0 )
        {
            drmSyncobjDestroy( g_device.drmRenderFd(), uSyncobjHandle );
            return nullptr;
        }

        if ( drmSyncobjHandleToFD( g_device.drmRenderFd(), uSyncobjHandle, &nSyncobjFd ) != 0 )
        {
            drmSyncobjDestroy( g_device.drmRenderFd(), uSyncobjHandle );
            return nullptr;
        }

        return std::make_shared<CTimeline>( nSyncobjFd, uSyncobjHandle );
    }

    CTimeline::CTimeline( int32_t nSyncobjFd )
        : CTimeline( nSyncobjFd, SyncobjFdToHandle( nSyncobjFd ) )
    {
    }

    CTimeline::CTimeline( int32_t nSyncobjFd, uint32_t uSyncobjHandle )
        : m_nSyncobjFd{ nSyncobjFd }
        , m_uSyncobjHandle{ uSyncobjHandle }
    {
    }

    CTimeline::~CTimeline()
    {
        if ( m_uSyncobjHandle )
            drmSyncobjDestroy( GetDrmRenderFD(), m_uSyncobjHandle );
        if ( m_nSyncobjFd >= 0 )
            close( m_nSyncobjFd );
    }

    int32_t CTimeline::GetDrmRenderFD()
    {
        return g_device.drmRenderFd();
    }

    bool CTimeline::ImportSyncFd( int nSyncFd, uint64_t ulPoint )
    {
        uint32_t uTempHandle = 0;
        int nRet = 0;

        nRet = drmSyncobjCreate( g_device.drmRenderFd(), 0, &uTempHandle );
        if ( nRet != 0 )
        {
            s_TimelineLog.errorf_errno( "drmSyncobjCreate failed (%d)", nRet );
            goto done_ImportSyncFd;
        }

        nRet = drmSyncobjImportSyncFile( g_device.drmRenderFd(), uTempHandle, nSyncFd );
        if ( nRet != 0 )
        {
            s_TimelineLog.errorf_errno( "drmSyncobjImportSyncFile failed (%d)", nRet );
            goto done_ImportSyncFd;
        }

        nRet = drmSyncobjTransfer( g_device.drmRenderFd(), m_uSyncobjHandle, ulPoint, uTempHandle, 0, 0 );
        if ( nRet != 0 )
        {
            s_TimelineLog.errorf_errno( "drmSyncobjTransfer failed (%d)", nRet );
            goto done_ImportSyncFd;
        }

done_ImportSyncFd:
        drmSyncobjDestroy( g_device.drmRenderFd(), uTempHandle );

        return nRet == 0;
    }
    // CTimelinePoint

    template <TimelinePointType Type>
    CTimelinePoint<Type>::CTimelinePoint( std::shared_ptr<CTimeline> pTimeline, uint64_t ulPoint  )
        : m_pTimeline{ std::move( pTimeline ) }
        , m_ulPoint{ ulPoint }
    {
    }

    template <TimelinePointType Type>
    CTimelinePoint<Type>::~CTimelinePoint()
    {
        if ( ShouldSignalOnDestruction() )
        {
            const uint32_t uHandle = m_pTimeline->GetSyncobjHandle();

            drmSyncobjTimelineSignal( m_pTimeline->GetDrmRenderFD(), &uHandle, &m_ulPoint, 1 );
        }
    }

    template <TimelinePointType Type>
    bool CTimelinePoint<Type>::Wait( int64_t lTimeout )
    {
        uint32_t uHandle = m_pTimeline->GetSyncobjHandle();

        int nRet = drmSyncobjTimelineWait(
            m_pTimeline->GetDrmRenderFD(),
            &uHandle,
            &m_ulPoint,
            1,
            lTimeout,
            DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL,
            nullptr );

        return nRet == 0;
    }

    //
    // Fence flags tl;dr
    // 0                                      -> Wait for signal on a materialized fence, -ENOENT if not materialized
    // DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE  -> Wait only for materialization
    // DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT -> Wait for materialization + signal
    //
    template <TimelinePointType Type>
    std::pair<int32_t, bool> CTimelinePoint<Type>::CreateEventFd()
    {
        assert( Type == TimelinePointType::Acquire );

        uint32_t uHandle = m_pTimeline->GetSyncobjHandle();
        uint64_t ulSignalledPoint = 0;
        int nRet = drmSyncobjQuery( m_pTimeline->GetDrmRenderFD(), &uHandle, &ulSignalledPoint, 1u );
        if ( nRet != 0 )
        {
            s_TimelineLog.errorf_errno( "drmSyncobjQuery failed (%d)", nRet );
            return k_InvalidEvent;
        }

        if ( ulSignalledPoint >= m_ulPoint )
        {
            return k_AlreadySignalledEvent;
        }
        else
        {
            const int32_t nExplicitSyncEventFd = eventfd( 0, EFD_CLOEXEC );
            if ( nExplicitSyncEventFd < 0 )
            {
                s_TimelineLog.errorf_errno( "Failed to create eventfd (%d)", nExplicitSyncEventFd );
                return k_InvalidEvent;
            }

            drm_syncobj_eventfd syncobjEventFd =
            {
                .handle = m_pTimeline->GetSyncobjHandle(),
                // Only valid flags are: DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE
                // -> Wait for fence materialization rather than signal.
                .flags  = 0u,
                .point  = m_ulPoint,
                .fd     = nExplicitSyncEventFd,
            };

            if ( drmIoctl( m_pTimeline->GetDrmRenderFD(), DRM_IOCTL_SYNCOBJ_EVENTFD, &syncobjEventFd ) != 0 )
            {
                s_TimelineLog.errorf_errno( "DRM_IOCTL_SYNCOBJ_EVENTFD failed" );
                close( nExplicitSyncEventFd );
                return k_InvalidEvent;
            }

            return { nExplicitSyncEventFd, false };
        }

    }

    template <TimelinePointType Type>
    std::shared_ptr<VulkanSemaphore_t> CTimelinePoint<Type>::ToVkSemaphore()
    {
        uint32_t uSyncobjHandle = m_pTimeline->GetSyncobjHandle();
        uint32_t uTmpSyncobj = 0;
        int nSyncFd = -1;
        int nRet = 0;

        nRet = drmSyncobjCreate( g_device.drmRenderFd(), 0, &uTmpSyncobj );
        if ( nRet != 0 )
        {
            s_TimelineLog.errorf_errno( "drmSyncobjCreate failed (%d)", nRet );
            goto done_ToVkSemaphore;
        }

        nRet = drmSyncobjTimelineWait(
            m_pTimeline->GetDrmRenderFD(),
            &uSyncobjHandle,
            &m_ulPoint,
            1,
            std::numeric_limits<int64_t>::max(),
            DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE,
            nullptr );
        if ( nRet != 0 )
        {
            s_TimelineLog.errorf_errno( "drmSyncobjTimelineWait failed (%d)", nRet );
            goto done_ToVkSemaphore;
        }

        nRet = drmSyncobjTransfer( g_device.drmRenderFd(), uTmpSyncobj, 0, uSyncobjHandle, m_ulPoint, 0 );
        if ( nRet != 0)
        {
            s_TimelineLog.errorf_errno( "drmSyncobjTransfer failed (%d)", nRet );
            goto done_ToVkSemaphore;
        }

        nRet = drmSyncobjExportSyncFile( g_device.drmRenderFd(), uTmpSyncobj, &nSyncFd );
        if ( nRet != 0 )
        {
            s_TimelineLog.errorf_errno( "drmSyncobjExportSyncFile failed (%d)", nRet );
            goto done_ToVkSemaphore;
        }

done_ToVkSemaphore:
        drmSyncobjDestroy( g_device.drmRenderFd(), uTmpSyncobj );

        return g_device.ImportSyncFd( nSyncFd );
    }

    template class CTimelinePoint<TimelinePointType::Acquire>;
    template class CTimelinePoint<TimelinePointType::Release>;

}
