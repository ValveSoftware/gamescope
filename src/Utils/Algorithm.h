#pragma once

#include <iterator>
#include <span>
#include <vector>

namespace gamescope::Algorithm
{
    template<typename TObj> constexpr TObj *Begin( std::span<TObj> span )
    { return span.data( ); }

    template<typename TObj> constexpr TObj *End( std::span<TObj> span )
    { return Begin( span ) + span.size( ); }

    template<typename TObj>
    constexpr const TObj *Begin( const std::vector<TObj> &vec )
    { return vec.data( ); }

    template<typename TObj>
    constexpr const TObj *End( const std::vector<TObj> &vec )
    { return Begin( vec ) + vec.size( ); }

    template<typename TIter, typename TObj>
    constexpr TIter FindSimple( TIter pFirst, TIter pEnd, const TObj &obj )
    {
        while ( pFirst != pEnd && *pFirst != obj )
            ++pFirst;

        return pFirst;
    }

    template<typename TIter, typename TObj>
    constexpr TIter FindByFour( TIter pFirst, TIter pEnd, const TObj &obj )
    {
        typename std::iterator_traits<TIter>::difference_type ulTripCount =
            ( pEnd - pFirst ) >> 2;

        while ( ulTripCount-- > 0 )
        {
            if ( pFirst[ 0 ] == obj ) return &pFirst[ 0 ];

            if ( pFirst[ 1 ] == obj ) return &pFirst[ 1 ];

            if ( pFirst[ 2 ] == obj ) return &pFirst[ 2 ];

            if ( pFirst[ 3 ] == obj ) return &pFirst[ 3 ];

            pFirst += 4;
        }

        switch ( pEnd - pFirst )
        {
            case 3:
            {
                if ( pFirst[ 0 ] == obj ) return &pFirst[ 0 ];

                if ( pFirst[ 1 ] == obj ) return &pFirst[ 1 ];

                if ( pFirst[ 2 ] == obj ) return &pFirst[ 2 ];

                return pEnd;
            }
            case 2:
            {
                if ( pFirst[ 0 ] == obj ) return &pFirst[ 0 ];

                if ( pFirst[ 1 ] == obj ) return &pFirst[ 1 ];

                return pEnd;
            }
            case 1:
            {
                if ( pFirst[ 0 ] == obj ) return &pFirst[ 0 ];

                return pEnd;
            }
            case 0:
            {
                return pEnd;
            }
            default:
            {
                __builtin_unreachable( );
            }
        }
    }

    template<typename TIter, typename TObj>
    constexpr TIter Find( TIter pFirst, TIter pEnd, const TObj &obj )
    { return FindSimple( pFirst, pEnd, obj ); }

    template<typename TIter, typename TObj>
    constexpr TIter Find( std::span<TObj> span, const TObj &obj )
    { return Find( Begin( span ), End( span ), obj ); }

    template<typename TIter, typename TObj>
    constexpr TIter Find( const std::vector<TObj> &vec, const TObj &obj )
    { return Find( Begin( vec ), End( vec ), obj ); }

    template<typename TIter, typename TObj>
    constexpr bool ContainsShortcut( TIter pFirst, TIter pEnd, const TObj &obj )
    { return Find( pFirst, pEnd, obj ) != pEnd; }

    template<typename TIter, typename TObj>
    constexpr bool
    ContainsNoShortcut( TIter pFirst, TIter pEnd, const TObj &obj )
    {
        bool bFound = false;

        typename std::iterator_traits<TIter>::difference_type ulTripCount =
            ( pEnd - pFirst ) >> 2;

        while ( ulTripCount-- > 0 )
        {
            bFound |= pFirst[ 0 ] == obj || pFirst[ 1 ] == obj ||
                      pFirst[ 2 ] == obj || pFirst[ 3 ] == obj;

            pFirst += 4;
        }

        switch ( pEnd - pFirst )
        {
            case 3:
            {
                bFound |= pFirst[ 0 ] == obj || pFirst[ 1 ] == obj ||
                          pFirst[ 2 ] == obj;
                break;
            }
            case 2:
            {
                bFound |= pFirst[ 0 ] == obj || pFirst[ 1 ] == obj;
                break;
            }
            case 1:
            {
                bFound |= pFirst[ 0 ] == obj;
                break;
            }
            case 0:
            {
                break;
            }
            default:
            {
                __builtin_unreachable( );
            }
        }

        return bFound;
    }

    template<typename TIter, typename TObj>
    constexpr bool Contains( TIter pFirst, TIter pEnd, const TObj &obj )
    { return ContainsNoShortcut( pFirst, pEnd, obj ); }

    template<typename TSpanObj, typename TObj>
    constexpr bool Contains( std::span<TSpanObj> span, const TObj &obj )
    { return Contains( Begin( span ), End( span ), obj ); }

    template<typename TVectorObj, typename TObj>
    constexpr bool
    Contains( const std::vector<TVectorObj> &vec, const TObj &obj )
    { return Contains( Begin( vec ), End( vec ), obj ); }
} // namespace gamescope::Algorithm
