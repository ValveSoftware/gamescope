#pragma once

#include <utility>

namespace gamescope
{
    template <typename Func>
    class DeferHelper
    {
    public:
        DeferHelper( Func fnFunc )
            : m_fnFunc{ std::move( fnFunc ) }
        {
        }

        ~DeferHelper()
        {
            m_fnFunc();
        }

    private:
        Func m_fnFunc;
    };

}

#define DEFER_1(x, y) x##y
#define DEFER_2(x, y) DEFER_1(x, y)
#define DEFER_3(x)    DEFER_2(x, __COUNTER__)
#define defer(code)   auto DEFER_3(_defer_) = ::gamescope::DeferHelper( [&](){ code; } )
