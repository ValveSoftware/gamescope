#pragma once

namespace gamescope
{

    class NonCopyable
    {
    public:
        NonCopyable( )                     = default;
        NonCopyable( const NonCopyable & ) = delete;

        void operator=( const NonCopyable & ) = delete;
    };

} // namespace gamescope
