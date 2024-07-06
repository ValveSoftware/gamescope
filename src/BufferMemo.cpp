#include "BufferMemo.h"

#include "wlserver.hpp"

namespace gamescope
{
    /////////////////
    // CBufferMemo
    /////////////////

    CBufferMemo::CBufferMemo( CBufferMemoizer *pMemoizer, wlr_buffer *pBuffer, OwningRc<CVulkanTexture> pTexture )
        : m_pMemoizer{ pMemoizer }
        , m_pBuffer{ pBuffer }
        , m_pVulkanTexture{ std::move( pTexture ) }
    {
    }

    CBufferMemo::~CBufferMemo()
    {
        wl_list_remove( &m_DeleteListener.link );
    }

    void CBufferMemo::Finalize()
    {
        wlserver_lock();
	    wl_signal_add( &m_pBuffer->events.destroy, &m_DeleteListener );
	    wlserver_unlock();
    }

    void CBufferMemo::OnBufferDestroyed( void *pUserData )
    {
        assert( m_pVulkanTexture->GetRefCount() == 0 );
        m_pMemoizer->UnmemoizeBuffer( m_pBuffer );
    }

    ///////////////////
    // CBufferMemoizer
    ///////////////////

    OwningRc<CVulkanTexture> CBufferMemoizer::LookupVulkanTexture( wlr_buffer *pBuffer ) const
    {
        std::scoped_lock lock{ m_mutBufferMemos };
        auto iter = m_BufferMemos.find( pBuffer );
        if ( iter == m_BufferMemos.end() )
            return nullptr;

        return iter->second.GetVulkanTexture();
    }

    void CBufferMemoizer::MemoizeBuffer( wlr_buffer *pBuffer, OwningRc<CVulkanTexture> pTexture )
    {
        // Can't hold m_mutBufferMemos while we finalize link from pMemo to buffer
        // as we can't have wlserver_lock held otherwise we can deadlock when
        // adding the wl_signal.
        //
        // This is fine as the lookups only happen on one thread, that calls this
        // or LookupVulkanTexture.
        CBufferMemo *pMemo = nullptr;
        {
            std::scoped_lock lock{ m_mutBufferMemos };
            auto [ iter, bSuccess ] = m_BufferMemos.emplace( std::piecewise_construct,
                std::forward_as_tuple( pBuffer ),
                std::forward_as_tuple( this, pBuffer, std::move( pTexture ) ) );

            assert( bSuccess );
            pMemo = &iter->second;
        }
        pMemo->Finalize();
    }

    void CBufferMemoizer::UnmemoizeBuffer( wlr_buffer *pBuffer )
    {
        std::scoped_lock lock{ m_mutBufferMemos };
        auto iter = m_BufferMemos.find( pBuffer );
        assert( iter != m_BufferMemos.end() );
        m_BufferMemos.erase( iter );
    }
}