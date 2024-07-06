#pragma once

#include <numeric>
#include <algorithm>
#include <cstdint>
#include <string>
#include <charconv>

namespace gamescope
{
    template <typename T>
    class Ratio
    {
    public:
        Ratio(T num, T denom)
        {
            Set( num, denom );
        }

        Ratio( std::string_view view )
        {
            Set(0, 0);

            size_t colon = view.find(":");

            if ( colon == std::string_view::npos )
            return;

            std::string_view numStr   = view.substr( 0, colon );
            std::string_view denomStr = view.substr( colon + 1 );

            T num = 0, denom = 0;
            std::from_chars( numStr.data(),   numStr.data()   + numStr.size(),   num );
            std::from_chars( denomStr.data(), denomStr.data() + denomStr.size(), denom );

            Set( num, denom );
        }

        T Num()   const { return m_num; }
        T Denom() const { return m_denom; }

        bool IsUndefined() const { return m_denom == 0; }

        void Set( T num, T denom )
        {
            const T gcd = std::gcd( num, denom );

            if ( gcd == 0 )
            {
                m_num   = 0;
                m_denom = 0;

                return;
            }

            m_num = num / gcd;
            m_denom = denom / gcd;
        }

        bool operator == ( const Ratio& other ) const { return Num() == other.Num() && Denom() == other.Denom(); }
        bool operator != ( const Ratio& other ) const { return !(*this == other); }

        bool operator >= ( const Ratio& other ) const { return Num() * other.Denom() >= other.Num() * Denom(); }
        bool operator >  ( const Ratio& other ) const { return Num() * other.Denom() > other.Num() * Denom(); }

        bool operator <  ( const Ratio& other ) const { return  !( *this >= other ); }
        bool operator <= ( const Ratio& other ) const { return  !( *this > other ); }

    private:
        T m_num, m_denom;
    };
}