#pragma once

#include "color_helpers.h"
#include "gamescope_shared.h"
#include "vulkan_include.h"
#include "Timeline.h"
#include "convar.h"
#include "rc.h"
#include "drm_include.h"
#include "Utils/Algorithm.h"

#include <cassert>
#include <span>
#include <vector>
#include <memory>
#include <optional>
#include <atomic>
#include <variant>

struct wlr_buffer;
struct wlr_dmabuf_attributes;

struct FrameInfo_t;

extern bool steamMode;

namespace gamescope
{
    struct VBlankScheduleTime;
    class BackendBlob;
    class INestedHints;

    namespace VirtualConnectorStrategies
    {
        enum VirtualConnectorStrategy : uint32_t
        {
            SingleApplication,
            SteamControlled,
            PerAppId,
            PerWindow,

            Count,
        };
    }
    using VirtualConnectorStrategy = VirtualConnectorStrategies::VirtualConnectorStrategy;
    using VirtualConnectorKey_t = uint64_t;
    extern ConVar<VirtualConnectorStrategy> cv_backend_virtual_connector_strategy;

    static constexpr bool VirtualConnectorStrategyIsSingleOutput( VirtualConnectorStrategy eStrategy )
    {
        return eStrategy == VirtualConnectorStrategies::SingleApplication || eStrategy == VirtualConnectorStrategies::SteamControlled;
    }

    static inline bool VirtualConnectorIsSingleOutput()
    {
        return VirtualConnectorStrategyIsSingleOutput( cv_backend_virtual_connector_strategy );
    }

    static inline bool VirtualConnectorInSteamPerAppState()
    {
        return steamMode && cv_backend_virtual_connector_strategy == gamescope::VirtualConnectorStrategies::PerAppId;
    }

    static inline bool VirtualConnectorKeyIsSteam( VirtualConnectorKey_t ulKey )
    {
        return VirtualConnectorInSteamPerAppState() && ulKey == 769;
    }

    static inline std::string_view VirtualConnectorStrategyToString( VirtualConnectorStrategy eStrategy )
    {
        switch ( eStrategy )
        {
            case VirtualConnectorStrategies::SingleApplication: return "SingleApplication";
            case VirtualConnectorStrategies::SteamControlled: return "SteamControlled";
            case VirtualConnectorStrategies::PerAppId: return "PerAppId";
            case VirtualConnectorStrategies::PerWindow: return "PerWindow";
            default:
                return "Unknown";
        }
    }

    enum class InputType
    {
        Mouse,
    };

    namespace TouchClickModes
    {
        enum TouchClickMode : uint32_t
        {
            Hover,
            Left,
            Right,
            Middle,
            Passthrough,
            Disabled,
            Trackpad,
        };
    }
    using TouchClickMode = TouchClickModes::TouchClickMode;

    struct BackendConnectorHDRInfo
    {
        // We still want to set up HDR info for Steam Deck LCD with some good
        // target/mapping values for the display brightness for undocking from a HDR display,
        // but don't want to expose HDR there as it is not good.
        bool bExposeHDRSupport = false;
        bool bAlwaysPatchEdid = false;

        // The output encoding to use for HDR output.
        // For typical HDR10 displays, this will be PQ.
        // For displays doing "traditional HDR" such as Steam Deck OLED, this is Gamma 2.2.
        EOTF eOutputEncodingEOTF = EOTF_Gamma22;

        uint16_t uMaxContentLightLevel = 500;     // Nits
        uint16_t uMaxFrameAverageLuminance = 500; // Nits
        uint16_t uMinContentLightLevel = 0;       // Nits / 10000
        std::shared_ptr<BackendBlob> pDefaultMetadataBlob;

        bool IsHDRG22() const
        {
            return bExposeHDRSupport && eOutputEncodingEOTF == EOTF_Gamma22;
        }

        bool ShouldPatchEDID() const
        {
            return bAlwaysPatchEdid || IsHDRG22();
        }

        bool IsHDR10() const
        {
            // PQ output encoding is always HDR10 (PQ + 2020) for us.
            // If that assumption changes, update me.
            return bExposeHDRSupport && eOutputEncodingEOTF == EOTF_PQ;
        }
    };

    struct BackendMode
    {
        uint32_t uWidth;
        uint32_t uHeight;
        uint32_t uRefresh; // Hz
    };

    struct BackendPresentFeedback
    {
    public:
        uint64_t CurrentPresentsInFlight() const { return TotalPresentsQueued() - TotalPresentsCompleted(); }

        // Across the lifetime of the backend.
        uint64_t TotalPresentsQueued() const { return m_uQueuedPresents.load(); }
        uint64_t TotalPresentsCompleted() const { return m_uCompletedPresents.load(); }

        std::atomic<uint64_t> m_uQueuedPresents = { 0u };
        std::atomic<uint64_t> m_uCompletedPresents = { 0u };
    };

    class IBackendConnector
    {
    public:
        virtual ~IBackendConnector() {}

        virtual uint64_t GetConnectorID() const = 0;

        virtual GamescopeScreenType GetScreenType() const = 0;
        virtual GamescopePanelOrientation GetCurrentOrientation() const = 0;
        virtual bool SupportsHDR() const = 0;
        virtual bool IsHDRActive() const = 0;
        virtual const BackendConnectorHDRInfo &GetHDRInfo() const = 0;
        virtual bool IsVRRActive() const = 0;
        virtual std::span<const BackendMode> GetModes() const = 0;

        virtual bool SupportsVRR() const = 0;

        virtual std::span<const uint8_t> GetRawEDID() const = 0;
        virtual std::span<const uint32_t> GetValidDynamicRefreshRates() const = 0;

        virtual void GetNativeColorimetry(
            bool bHDR10,
            displaycolorimetry_t *displayColorimetry, EOTF *displayEOTF,
            displaycolorimetry_t *outputEncodingColorimetry, EOTF *outputEncodingEOTF ) const = 0;

        virtual const char *GetName() const = 0;
        virtual const char *GetMake() const = 0;
        virtual const char *GetModel() const = 0;

        virtual int Present( const FrameInfo_t *pFrameInfo, bool bAsync ) = 0;
        virtual VBlankScheduleTime FrameSync() = 0;
        virtual BackendPresentFeedback& PresentationFeedback() = 0;

        virtual uint64_t GetVirtualConnectorKey() const = 0;

        virtual INestedHints *GetNestedHints() = 0;
    };

    class CBaseBackendConnector : public IBackendConnector
    {
    public:
        CBaseBackendConnector()
        {
            AssignConnectorId();
        }
        CBaseBackendConnector( uint64_t ulVirtualConnectorKey )
            : m_ulVirtualConnectorKey{ ulVirtualConnectorKey }
        {
            AssignConnectorId();
        }

        virtual ~CBaseBackendConnector()
        {

        }

        virtual uint64_t GetConnectorID() const override { return m_ulConnectorId; }
        virtual VBlankScheduleTime FrameSync() override;
        virtual BackendPresentFeedback& PresentationFeedback() override { return m_PresentFeedback; }
        virtual uint64_t GetVirtualConnectorKey() const override { return m_ulVirtualConnectorKey; }
        virtual INestedHints *GetNestedHints() override { return nullptr; }
    protected:
        uint64_t m_ulConnectorId = 0;
        uint64_t m_ulVirtualConnectorKey = 0;
        BackendPresentFeedback m_PresentFeedback{};

    private:
        void AssignConnectorId()
        {
            static uint64_t s_ulLastConnectorId = 0;
            m_ulConnectorId = ++s_ulLastConnectorId;
        }
    };

    class INestedHints
    {
    public:
        virtual ~INestedHints() {}

        struct CursorInfo
        {
            std::vector<uint32_t> pPixels;
            uint32_t uWidth;
            uint32_t uHeight;
            uint32_t uXHotspot;
            uint32_t uYHotspot;
        };

        virtual void SetCursorImage( std::shared_ptr<CursorInfo> info ) = 0;
        virtual void SetRelativeMouseMode( bool bRelative ) = 0;
        virtual void SetVisible( bool bVisible ) = 0;
        virtual void SetTitle( std::shared_ptr<std::string> szTitle ) = 0;
        virtual void SetIcon( std::shared_ptr<std::vector<uint32_t>> uIconPixels ) = 0;
        virtual void SetSelection( std::shared_ptr<std::string> szContents, GamescopeSelection eSelection ) = 0;
    };

    class IBackendFb : public IRcObject
    {
    public:
        virtual void SetBuffer( wlr_buffer *pClientBuffer ) = 0;
        virtual void SetReleasePoint( std::shared_ptr<CReleaseTimelinePoint> pReleasePoint ) = 0;
    };

    class CBaseBackendFb : public IBackendFb
    {
    public:
        CBaseBackendFb();
        virtual ~CBaseBackendFb();

        uint32_t IncRef() override;
        uint32_t DecRef() override;

        void SetBuffer( wlr_buffer *pClientBuffer ) override;
        void SetReleasePoint( std::shared_ptr<CReleaseTimelinePoint> pReleasePoint ) override;

    private:
        wlr_buffer *m_pClientBuffer = nullptr;
        std::shared_ptr<CReleaseTimelinePoint> m_pReleasePoint;
    };

    class IBackend
    {
    public:
        virtual ~IBackend() {}

        virtual bool Init() = 0;
        virtual bool PostInit() = 0;
        virtual std::span<const char *const> GetInstanceExtensions() const = 0;
        virtual std::span<const char *const> GetDeviceExtensions( VkPhysicalDevice pVkPhysicalDevice ) const = 0;
        virtual VkImageLayout GetPresentLayout() const = 0;
        virtual void GetPreferredOutputFormat( uint32_t *pPrimaryPlaneFormat, uint32_t *pOverlayPlaneFormat ) const = 0;
        virtual bool ValidPhysicalDevice( VkPhysicalDevice pVkPhysicalDevice ) const = 0;

        virtual void DirtyState( bool bForce = false, bool bForceModeset = false ) = 0;
        virtual bool PollState() = 0;

        virtual std::shared_ptr<BackendBlob> CreateBackendBlob( const std::type_info &type, std::span<const uint8_t> data ) = 0;
        template <typename T>
        std::shared_ptr<BackendBlob> CreateBackendBlob( const T& thing )
        {
            const uint8_t *pBegin = reinterpret_cast<const uint8_t *>( &thing );
            const uint8_t *pEnd = pBegin + sizeof( T );
            return CreateBackendBlob( typeid( T ), std::span<const uint8_t>( pBegin, pEnd ) );
        }

        // For DRM, this is
        // dmabuf -> fb_id.
        //
        // shared_ptr owns the structure.
        // Rc manages acquire/release of buffer to/from client while imported.
        virtual OwningRc<IBackendFb> ImportDmabufToBackend( wlr_buffer *pBuffer, wlr_dmabuf_attributes *pDmaBuf ) = 0;

        virtual bool UsesModifiers() const = 0;
        virtual std::span<const uint64_t> GetSupportedModifiers( uint32_t uDrmFormat ) const = 0;
        inline bool SupportsFormat( uint32_t uDrmFormat, bool invalidOnly ) const
        {
            if ( invalidOnly )
                return Algorithm::Contains( this->GetSupportedModifiers( uDrmFormat ), DRM_FORMAT_MOD_INVALID );
            return !this->GetSupportedModifiers( uDrmFormat ).empty();
        }

        virtual IBackendConnector *GetCurrentConnector() = 0;
        virtual IBackendConnector *GetConnector( GamescopeScreenType eScreenType ) = 0;

        virtual bool SupportsPlaneHardwareCursor() const = 0;
        virtual bool SupportsTearing() const = 0;

        virtual bool UsesVulkanSwapchain() const = 0;
        virtual bool IsSessionBased() const = 0;

        virtual bool SupportsExplicitSync() const = 0;

        // Dumb helper we should remove to support multi display someday.
        gamescope::GamescopeScreenType GetScreenType()
        {
            if ( GetCurrentConnector() )
                return GetCurrentConnector()->GetScreenType();

            return gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL;
        }

        virtual bool IsPaused() const = 0;
        virtual bool IsVisible() const = 0;
        virtual glm::uvec2 CursorSurfaceSize( glm::uvec2 uvecSize ) const = 0;

        // This will move to the connector and be deprecated soon.
        virtual bool HackTemporarySetDynamicRefresh( int nRefresh ) = 0;
        virtual void HackUpdatePatchedEdid() = 0;

        virtual bool NeedsFrameSync() const = 0;

        virtual TouchClickMode GetTouchClickMode() = 0;

        virtual void DumpDebugInfo() = 0;

        virtual bool UsesVirtualConnectors() = 0;
        virtual std::shared_ptr<IBackendConnector> CreateVirtualConnector( uint64_t ulVirtualConnectorKey ) = 0;

        virtual void NotifyPhysicalInput( InputType eInputType ) = 0;

        static IBackend *Get();
        template <typename T>
        static bool Set();

        static bool Set( IBackend *pBackend );
    protected:
        friend BackendBlob;

        virtual void OnBackendBlobDestroyed( BackendBlob *pBlob ) = 0;
    private:
    };


    class CBaseBackend : public IBackend
    {
    public:
        virtual bool HackTemporarySetDynamicRefresh( int nRefresh ) override { return false; }
        virtual void HackUpdatePatchedEdid() override {}

        virtual bool NeedsFrameSync() const override;

        virtual TouchClickMode GetTouchClickMode() override;

        virtual void DumpDebugInfo() override;

        virtual bool UsesVirtualConnectors() override;
        virtual std::shared_ptr<IBackendConnector> CreateVirtualConnector( uint64_t ulVirtualConnectorKey ) override;

        virtual void NotifyPhysicalInput( InputType eInputType ) override {}
    };

    // This is a blob of data that may be associated with
    // a backend if it needs to be.
    // Currently on non-DRM backends this is basically a
    // no-op.
    class BackendBlob
    {
    public:
        BackendBlob()
        {
        }

        BackendBlob( std::span<const uint8_t> data )
            : m_Data( data.begin(), data.end() )
        {
        }

        BackendBlob( std::span<const uint8_t> data, uint32_t uBlob, bool bOwned )
            : m_Data( data.begin(), data.end() )
            , m_uBlob( uBlob )
            , m_bOwned( bOwned )
        {
        }

        ~BackendBlob()
        {
            if ( m_bOwned )
            {
                IBackend *pBackend = IBackend::Get();
                if ( pBackend )
                    pBackend->OnBackendBlobDestroyed( this );
            }
        }

        // No copy constructor, because we can't duplicate the blob handle.
        BackendBlob( const BackendBlob& ) = delete;
        BackendBlob& operator=( const BackendBlob& ) = delete;
        // No move constructor, because we use shared_ptr anyway, but can be added if necessary.
        BackendBlob( BackendBlob&& ) = delete;
        BackendBlob& operator=( BackendBlob&& ) = delete;

        std::span<const uint8_t> GetData() const { return std::span<const uint8_t>( m_Data.begin(), m_Data.end() ); }
        template <typename T>
        const T& View() const
        {
            assert( sizeof( T ) == m_Data.size() );
            return *reinterpret_cast<const T*>( m_Data.data() );
        }
        uint32_t GetBlobValue() const { return m_uBlob; }

    private:
        std::vector<uint8_t> m_Data;
        uint32_t m_uBlob = 0;
        bool m_bOwned = false;
    };

    extern ConVar<TouchClickMode> cv_touch_click_mode;
}

inline gamescope::IBackend *GetBackend()
{
    return gamescope::IBackend::Get();
}

