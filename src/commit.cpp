#include "wlserver.hpp"
#include "rendervulkan.hpp"
#include "steamcompmgr.hpp"
#include "commit.h"

#include "gpuvis_trace_utils.h"

extern gamescope::CAsyncWaiter<gamescope::Rc<commit_t>> g_ImageWaiter;

commit_t::commit_t()
{
    static uint64_t maxCommmitID = 0;
    commitID = ++maxCommmitID;
}
commit_t::~commit_t()
{
    {
        std::unique_lock lock( m_WaitableCommitStateMutex );
        CloseFenceInternal();
    }

    if ( vulkanTex != nullptr )
        vulkanTex = nullptr;

    wlserver_lock();
    if (!presentation_feedbacks.empty())
    {
        wlserver_presentation_feedback_discard(surf, presentation_feedbacks);
        // presentation_feedbacks cleared by wlserver_presentation_feedback_discard
    }
    wlr_buffer_unlock( buf );
    wlserver_unlock();
}

GamescopeAppTextureColorspace commit_t::colorspace() const
{
    VkColorSpaceKHR colorspace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    if (feedback && vulkanTex)
        colorspace = feedback->vk_colorspace;

    if (!vulkanTex)
        return GAMESCOPE_APP_TEXTURE_COLORSPACE_LINEAR;

    return VkColorSpaceToGamescopeAppTextureColorSpace(vulkanTex->format(), colorspace);
}

int commit_t::GetFD()
{
    return m_nCommitFence;
}

void commit_t::OnPollIn()
{
    gpuvis_trace_end_ctx_printf( commitID, "wait fence" );

    {
        std::unique_lock lock( m_WaitableCommitStateMutex );
        if ( !CloseFenceInternal() )
            return;
    }

    Signal();

    nudge_steamcompmgr();
}

void commit_t::Signal()
{
    uint64_t now = get_time_in_nanos();
    present_time = now;

    uint64_t frametime;
    if ( m_bMangoNudge )
    {
        static uint64_t lastFrameTime = now;
        frametime = now - lastFrameTime;
        lastFrameTime = now;
    }

    // TODO: Move this so it's called in the main loop.
    // Instead of looping over all the windows like before.
    // When we get the new IWaitable stuff in there.
    {
        std::unique_lock< std::mutex > lock( m_pDoneCommits->listCommitsDoneLock );
        m_pDoneCommits->listCommitsDone.push_back( CommitDoneEntry_t{
            .winSeq = win_seq,
            .commitID = commitID,
            .desiredPresentTime = desired_present_time,
            .fifo = fifo,
        } );
    }

    if ( m_bMangoNudge )
        mangoapp_update( IsPerfOverlayFIFO() ? uint64_t(~0ull) : frametime, frametime, uint64_t(~0ull) );
}

void commit_t::OnPollHangUp()
{
    std::unique_lock lock( m_WaitableCommitStateMutex );
    CloseFenceInternal();
}

bool commit_t::IsPerfOverlayFIFO()
{
    return fifo || is_steam;
}

// Returns true if we had a fence that was closed.
bool commit_t::CloseFenceInternal()
{
    if ( m_nCommitFence < 0 )
        return false;

    // Will automatically remove from epoll!
    g_ImageWaiter.RemoveWaitable( this );
    close( m_nCommitFence );
    m_nCommitFence = -1;
    return true;
}

void commit_t::SetFence( int nFence, bool bMangoNudge, CommitDoneList_t *pDoneCommits )
{
    std::unique_lock lock( m_WaitableCommitStateMutex );
    CloseFenceInternal();

    m_nCommitFence = nFence;
    m_bMangoNudge = bMangoNudge;
    m_pDoneCommits = pDoneCommits;
}

void calc_scale_factor(float &out_scale_x, float &out_scale_y, float sourceWidth, float sourceHeight);

bool commit_t::ShouldPreemptivelyUpscale()
{
    // Don't pre-emptively upscale if we are not a FIFO commit.
    // Don't want to FSR upscale 1000fps content.
    if ( !fifo )
        return false;

    // If we support the upscaling filter in hardware, don't
    // pre-emptively do it via shaders.
    if ( DoesHardwareSupportUpscaleFilter( g_upscaleFilter ) )
        return false;

    if ( !vulkanTex )
        return false;

    float flScaleX = 1.0f;
    float flScaleY = 1.0f;
    // I wish this function was more programatic with its inputs, but it does do exactly what we want right now...
    // It should also return a std::pair or a glm uvec
    calc_scale_factor( flScaleX, flScaleY, vulkanTex->width(), vulkanTex->height() );

    return !close_enough( flScaleX, 1.0f ) || !close_enough( flScaleY, 1.0f );
}
