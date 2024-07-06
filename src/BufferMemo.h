#pragma once

#include "rc.h"
#include "rendervulkan.hpp"

#include <unordered_map>
#include <mutex>

struct wl_listener;
struct wlr_buffer;

// TODO: Move to common code when we want it more.
#define WAYLAND_LISTENER( member_listener, func ) \
    wl_listener { .notify = []( wl_listener *pListener, void *pUserData ) { decltype( this ) pObject = wl_container_of( pListener, pObject, member_listener ); pObject->func( pUserData ); } }

namespace gamescope
{
    class CBufferMemoizer;
    class CBufferMemo;

    class CBufferMemo
    {
    public:
        CBufferMemo( CBufferMemoizer *pMemoizer, wlr_buffer *pBuffer, OwningRc<CVulkanTexture> pTexture );
        ~CBufferMemo();

        void Finalize();

        CBufferMemoizer *GetMemoizer() const { return m_pMemoizer; }

        const OwningRc<CVulkanTexture> &GetVulkanTexture() const { return m_pVulkanTexture; }

        void OnBufferDestroyed( void *pUserData );
    private:
        CBufferMemoizer *m_pMemoizer = nullptr;
        wlr_buffer *m_pBuffer = nullptr;
        wl_listener m_DeleteListener = WAYLAND_LISTENER( m_DeleteListener, OnBufferDestroyed );

        // OwningRc to have a private reference:
        // So we can keep the CVulkanTexture, as public references
        // determine when we give the texture/buffer back to the app.
        OwningRc<CVulkanTexture> m_pVulkanTexture;
    };

    class CBufferMemoizer
    {
    public:
        // Must return an OwningRc for the locking to make sense and not deadlock.
        OwningRc<CVulkanTexture> LookupVulkanTexture( wlr_buffer *pBuffer ) const;

        void MemoizeBuffer( wlr_buffer *pBuffer, OwningRc<CVulkanTexture> pTexture );
        void UnmemoizeBuffer( wlr_buffer *pBuffer );
    private:
        mutable std::mutex m_mutBufferMemos;
        std::unordered_map<wlr_buffer *, CBufferMemo> m_BufferMemos;
    };

}
