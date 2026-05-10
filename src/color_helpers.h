#pragma once

#define GLM_ENABLE_EXPERIMENTAL 1

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <vector>
#include <memory>

#include <glm/vec2.hpp> // glm::vec2
#include <glm/vec3.hpp> // glm::vec3
#include <glm/mat3x3.hpp> // glm::mat3
#include <glm/gtx/component_wise.hpp>

// Color utils
inline int quantize( float fVal, float fMaxVal )
{
    return std::max( 0.f, std::min( fMaxVal, rintf( fVal * fMaxVal ) ) );
}

inline uint16_t quantize_lut_value_16bit( float flValue )
{
    return (uint16_t)quantize( flValue, (float)UINT16_MAX );
}

inline float clamp01( float val )
{
	return std::max( 0.f, std::min( 1.f, val ) );
}

inline float clamp( float val, float lo, float hi )
{
	return std::max( lo, std::min( hi, val ) );
}

inline float cfit( float x, float i1, float i2, float f1, float f2 )
{
    return f1+(f2-f1)*clamp01( (x-i1)/(i2-i1) );
}

inline float srgb_to_linear( float fVal )
{
    return ( fVal < 0.04045f ) ? fVal / 12.92f : std::pow( ( fVal + 0.055f) / 1.055f, 2.4f );
}

inline float linear_to_srgb( float fVal )
{
    return ( fVal < 0.0031308f ) ? fVal * 12.92f : std::pow( fVal, 1.0f / 2.4f ) * 1.055f - 0.055f;
}

template <typename T>
inline T pq_to_nits( const T& pq )
{
    const float c1 = 0.8359375f;
    const float c2 = 18.8515625f;
    const float c3 = 18.6875f;

    const float oo_m1 = 1.0f / 0.1593017578125f;
    const float oo_m2 = 1.0f / 78.84375f;

    T num = glm::max(glm::pow(pq, T(oo_m2)) - c1, T(0.0f));
    T den = c2 - c3 * glm::pow(pq, T(oo_m2));

    return glm::pow(num / den, T(oo_m1)) * 10000.0f;
}

template <typename T>
inline T nits_to_pq( const T& nits )
{
    T y = glm::clamp(nits / 10000.0f, T(0.0f), T(1.0f));
    const float c1 = 0.8359375f;
    const float c2 = 18.8515625f;
    const float c3 = 18.6875f;
    const float m1 = 0.1593017578125f;
    const float m2 = 78.84375f;
    T num = c1 + c2 * glm::pow(y, T(m1));
    T den = T(1.0) + c3 * glm::pow(y, T(m1));
    T n = glm::pow(num / den, T(m2));
    return n;
}

struct tonemap_info_t
{
	bool operator == (const tonemap_info_t&) const = default;
	bool operator != (const tonemap_info_t&) const = default;

	float flBlackPointNits = 0.f;
	float flWhitePointNits = 0.f;

	bool BIsValid() const
	{
		return ( flWhitePointNits > flBlackPointNits );
	}

	void reset()
	{
		flBlackPointNits = 0.f;
		flWhitePointNits = 0.f;
	}
};

// Apply an HDR tonemapping according to eetf 2390 (R-REP-BT.2390-8-2020-PDF-E.pdf)
// sourceXXX == "Mastering Display" == Lw, Lb (in the paper)
// targetXXX == "Target Display" == Lmin, Lmax (in the paper)
// Be warned... PQ in, PQ out, for ALL params [0,1]
// This does not imply this function has anything to do with PQ
// (it's quite sensible to apply it to linear values created in other ways... you just have to
// PQ all params first, and undo the output)
// Values outside of 0-1 are not defined

struct eetf_2390_t
{
	void init( const tonemap_info_t & source, const tonemap_info_t & target )
	{
		init_pq(
			nits_to_pq( source.flBlackPointNits ),
			nits_to_pq( source.flWhitePointNits ),
			nits_to_pq( target.flBlackPointNits ),
			nits_to_pq( target.flWhitePointNits ) );
	}

	void init_pq( float sourceBlackPQ, float sourceWhitePQ, float targetBlackPQ, float targetWhitePQ )
	{
		m_sourceBlackPQ = sourceBlackPQ;
		m_sourcePQScale = sourceWhitePQ - sourceBlackPQ;
		m_invSourcePQScale = m_sourcePQScale > 0.f ? 1.f / m_sourcePQScale : 0.f;
    	m_minLumPQ = ( targetBlackPQ - sourceBlackPQ ) * m_invSourcePQScale;
    	m_maxLumPQ = ( targetWhitePQ - sourceBlackPQ ) * m_invSourcePQScale;
		m_ks = 1.5 * m_maxLumPQ - 0.5;  // TODO : return false if ks == 1.f?
	}

	inline float apply( float inputNits ) const
	{
		return pq_to_nits( apply_pq( nits_to_pq( inputNits ) ) );
	}

	// Raw PQ transfer function
	inline float apply_pq( float valuePQ ) const
	{
		// normalize PQ based on the mastering (source) display (E1)
		float e1 = ( valuePQ - m_sourceBlackPQ ) * m_invSourcePQScale;

		// Apply high end rolloff
		float e2 = e1 < m_ks ? e1 : _eetf_2390_spline( e1, m_ks, m_maxLumPQ );

		// Apply low end pedestal
		float one_min_e2 = 1.f - e2;
		float one_min_e2_sq = one_min_e2 * one_min_e2;
		float e3 = e2 + m_minLumPQ * one_min_e2_sq * one_min_e2_sq;

		// Re-apply mastering (source) transform
		return e3 * m_sourcePQScale + m_sourceBlackPQ;
	}

	// "Max RGB" approach, as defined in "Color Volume and Hue-Preservation in HDR Tone Mapping"
	// Digital Object Identifier 10.5594/JMI.2020.2984046
	// Date of publication: 4 May 2020
	inline glm::vec3 apply_max_rgb( const glm::vec3 & inputNits ) const
	{
		float input_scalar_nits = std::max( inputNits.r, std::max( inputNits.g, inputNits.b ) );
		float output_scalar_nits = pq_to_nits( apply_pq( nits_to_pq( input_scalar_nits ) ) );
		float gain = input_scalar_nits > 0.f ? output_scalar_nits / input_scalar_nits : 0.f;
		return inputNits * gain;
	}

	inline glm::vec3 apply_luma_rgb( const glm::vec3 & inputNits ) const
	{
		float input_scalar_nits = 0.2627f * inputNits.r + 0.6780f * inputNits.g + 0.0593f * inputNits.b;
		float output_scalar_nits = pq_to_nits( apply_pq( nits_to_pq( input_scalar_nits ) ) );
		float gain = input_scalar_nits > 0.f ? output_scalar_nits / input_scalar_nits : 0.f;
		return inputNits * gain;
	}

	inline glm::vec3 apply_independent_rgb( const glm::vec3 & inputNits ) const
	{
		glm::vec3 inputPQ = nits_to_pq( inputNits );
		glm::vec3 outputPQ = { apply_pq( inputPQ.r ), apply_pq( inputPQ.g ), apply_pq( inputPQ.b ) };
		return pq_to_nits( outputPQ );
	}

	private:
	float m_sourceBlackPQ = 0.f;
	float m_sourcePQScale = 0.f;
	float m_invSourcePQScale = 0.f;
	float m_minLumPQ = 0.f;
	float m_maxLumPQ = 0.f;
	float m_ks = 0.f;

	inline float _eetf_2390_spline( float value, float ks, float maxLum ) const
	{
		float t = ( value - ks ) / ( 1.f - ks ); // TODO : guard against ks == 1.f?
		float t_sq = t*t;
		float t_cub = t_sq*t;
		float v1 = ( 2.f * t_cub - 3.f * t_sq + 1.f ) * ks;
		float v2 = ( t_cub - 2.f * t_sq + t ) * ( 1.f - ks );
		float v3 = (-2.f * t_cub + 3.f * t_sq ) * maxLum;
		return v1 + v2 + v3;
	}
};

inline float flerp( float a, float b, float t )
{
    return a + t * (b - a);
}

inline float safe_pow(float x, float y)
{
	// Avoids pow(x, 1.0f) != x.
	if (y == 1.0f)
		return x;

	return std::pow(std::max(x, 0.0f), y);
}

inline float positive_mod( float flX, float flPeriod )
{
	float flVal = fmodf( flX, flPeriod );
	return ( flVal < 0 ) ? flVal + flPeriod : fabsf( flVal ); // fabs fixes -0
}

// Colorimetry functions related to color space conversions
struct primaries_t
{
	bool operator == (const primaries_t&) const = default;
	bool operator != (const primaries_t&) const = default;

	glm::vec2 r;
	glm::vec2 g;
	glm::vec2 b;
};

enum EOTF
{
	EOTF_Gamma22 = 0,
	EOTF_PQ = 1,

	EOTF_Count = 2,
};

struct displaycolorimetry_t
{
	bool operator == (const displaycolorimetry_t&) const = default;
	bool operator != (const displaycolorimetry_t&) const = default;
	primaries_t primaries;
	glm::vec2 white;
};

struct nightmode_t
{
	bool operator == (const nightmode_t&) const = default;
	bool operator != (const nightmode_t&) const = default;

    float amount; // [0 = disabled, 1.f = on]
    float hue; // [0,1]
    float saturation; // [0,1]
};

struct colormapping_t
{
	bool operator == (const colormapping_t&) const = default;
	bool operator != (const colormapping_t&) const = default;

	float blendEnableMinSat;
	float blendEnableMaxSat;
	float blendAmountMin;
	float blendAmountMax;
};

displaycolorimetry_t lerp( const displaycolorimetry_t & a, const displaycolorimetry_t & b, float t );
colormapping_t lerp( const colormapping_t & a, const colormapping_t & b, float t );

// These values are directly exposed to steam
// Values must be stable over time
enum ETonemapOperator
{
	ETonemapOperator_None = 0,
	ETonemapOperator_EETF2390_Luma = 1,
	ETonemapOperator_EETF2390_Independent = 2,
	ETonemapOperator_EETF2390_MaxChan = 3,
};

struct tonemapping_t
{
	bool bUseShaper = true;
	float g22_luminance = 1.f; // what luminance should be applied for g22 EOTF conversions?
	ETonemapOperator eOperator = ETonemapOperator_None;
	eetf_2390_t eetf2390;

	inline glm::vec3 apply( const glm::vec3 & inputNits ) const
	{
		switch ( eOperator )
		{
			case ETonemapOperator_EETF2390_Luma:
				return eetf2390.apply_luma_rgb( inputNits );
			case ETonemapOperator_EETF2390_Independent:
				return eetf2390.apply_independent_rgb( inputNits );
			case ETonemapOperator_EETF2390_MaxChan:
				return eetf2390.apply_max_rgb( inputNits );
			default:
				return inputNits;
		}
	}
};

// Exposed in external atoms.  Dont change the values
enum EChromaticAdaptationMethod
{
    k_EChromaticAdapatationMethod_XYZ = 0,
    k_EChromaticAdapatationMethod_Bradford = 1,
};

glm::mat3 chromatic_adaptation_matrix( const glm::vec3 & sourceWhiteXYZ, const glm::vec3 & destWhiteXYZ,
	EChromaticAdaptationMethod eMethod );

struct lut1d_t
{
	int lutSize = 0;
	std::vector<float> dataR;
	std::vector<float> dataG;
	std::vector<float> dataB;

	// Some LUTs start with a flat section...
	// Where does the non-flat part start?
	// (impacts the inverse computation)
	int startIndexR = -1;
	int startIndexG = -1;
	int startIndexB = -1;

	void finalize(); // calculates start indicies

	void resize( int lutSize_ )
	{
		lutSize = lutSize_;
		dataR.resize( lutSize_ );
		dataG.resize( lutSize_ );
		dataB.resize( lutSize_ );
		startIndexR = -1;
		startIndexG = -1;
		startIndexB = -1;
	}
};

struct lut3d_t
{
	int lutEdgeSize = 0;
	std::vector<glm::vec3> data; // R changes fastest

	void resize( int lutEdgeSize_ )
	{
		lutEdgeSize = lutEdgeSize_;
		data.resize( lutEdgeSize_ * lutEdgeSize_ * lutEdgeSize_ );
	}
};

std::shared_ptr<lut3d_t> LoadCubeLut( FILE *pFile, bool &bRaisesBlackLevelFloor );
std::shared_ptr<lut3d_t> LoadCubeLut( const char *pchFileName, bool &bRaisesBlackLevelFloor );

// Generate a color transform from the source colorspace, to the dest colorspace,
// nLutSize1d is the number of color entries in the shaper lut
// I.e., for a shaper lut with 256 input colors  nLutSize1d = 256, countof(pRgbxData1d) = 1024
// nLutEdgeSize3d is the number of color entries, per edge, in the 3d lut
// I.e., for a 17x17x17 lut nLutEdgeSize3d = 17, countof(pRgbxData3d) = 19652
//
// If the white points differ, this performs an absolute colorimetric match
// Look luts are optional, but if specified applied in the sourceEOTF space

template <uint32_t lutEdgeSize3d>
void calcColorTransform( lut1d_t * pShaper, int nLutSize1d,
	lut3d_t * pLut3d,
	const displaycolorimetry_t & source, EOTF sourceEOTF,
	const displaycolorimetry_t & dest,  EOTF destEOTF,
	const glm::vec2 & destVirtualWhite, EChromaticAdaptationMethod eMethod,
	const colormapping_t & mapping, const nightmode_t & nightmode, const tonemapping_t & tonemapping,
	const lut3d_t * pLook, float flGain );

#define REGISTER_LUT_EDGE_SIZE(size) template void calcColorTransform<(size)>( lut1d_t * pShaper, int nLutSize1d, \
	lut3d_t * pLut3d,                                                                                                   \
	const displaycolorimetry_t & source, EOTF sourceEOTF,                                                               \
	const displaycolorimetry_t & dest,  EOTF destEOTF,                                                                  \
	const glm::vec2 & destVirtualWhite, EChromaticAdaptationMethod eMethod,                                             \
	const colormapping_t & mapping, const nightmode_t & nightmode, const tonemapping_t & tonemapping,                   \
	const lut3d_t * pLook, float flGain )

// Build colorimetry and a gamut mapping for the given SDR configuration
// Note: the output colorimetry will use the native display's white point
// Only the color gamut will change
void buildSDRColorimetry( displaycolorimetry_t * pColorimetry, colormapping_t *pMapping,
	float flSDRGamutWideness, const displaycolorimetry_t & nativeDisplayOutput );

// Build colorimetry and a gamut mapping for the given PQ configuration
void buildPQColorimetry( displaycolorimetry_t * pColorimetry, colormapping_t *pMapping, const displaycolorimetry_t & nativeDisplayOutput );

// Colormetry helper functions for DRM, kindly taken from Weston:
// https://gitlab.freedesktop.org/wayland/weston/-/blob/main/libweston/backend-drm/kms-color.c
// Licensed under MIT.

// Josh: I changed the asserts to clamps here (going to 0, rather than 1) to deal better with
// bad EDIDs (that have 0'ed out metadata) and naughty clients.

static inline uint16_t
color_xy_to_u16(float v)
{
	//assert(v >= 0.0f);
	//assert(v <= 1.0f);
	v = std::clamp(v, 0.0f, 1.0f);

    // CTA-861-G
    // 6.9.1 Static Metadata Type 1
    // chromaticity coordinate encoding
	return (uint16_t)round(v * 50000.0f);
}

static inline float
color_xy_from_u16(uint16_t v)
{
	return v / 50000.0f;
}

static inline uint16_t
nits_to_u16(float nits)
{
	//assert(nits >= 1.0f);
	//assert(nits <= 65535.0f);
	nits = std::clamp(nits, 0.0f, 65535.0f);

	// CTA-861-G
	// 6.9.1 Static Metadata Type 1
	// max display mastering luminance, max content light level,
	// max frame-average light level
	return (uint16_t)round(nits);
}

static inline float
nits_from_u16(uint16_t v)
{
	return float(v);
}

static inline uint16_t
nits_to_u16_dark(float nits)
{
	//assert(nits >= 0.0001f);
	//assert(nits <= 6.5535f);
	nits = std::clamp(nits, 0.0f, 6.5535f);

	// CTA-861-G
	// 6.9.1 Static Metadata Type 1
	// min display mastering luminance
	return (uint16_t)round(nits * 10000.0f);
}

static inline float
nits_from_u16_dark(uint16_t v)
{
	return v / 10000.0f;
}

static constexpr displaycolorimetry_t displaycolorimetry_steamdeck_spec
{
	.primaries = { { 0.602f, 0.355f }, { 0.340f, 0.574f }, { 0.164f, 0.121f } },
	.white = { 0.3070f, 0.3220f },  // not D65
};

static constexpr displaycolorimetry_t displaycolorimetry_steamdeck_measured
{
	.primaries = { { 0.603f, 0.349f }, { 0.335f, 0.571f }, { 0.163f, 0.115f } },
	.white = { 0.296f, 0.307f }, // not D65
};

static constexpr displaycolorimetry_t displaycolorimetry_709
{
	.primaries = { { 0.64f, 0.33f }, { 0.30f, 0.60f }, { 0.15f, 0.06f } },
	.white = { 0.3127f, 0.3290f },  // D65
};

// Our "saturated SDR target", per jeremys
static constexpr displaycolorimetry_t displaycolorimetry_widegamutgeneric
{
	.primaries = { { 0.6825f, 0.3165f }, { 0.241f, 0.719f }, { 0.138f, 0.050f } },
	.white = { 0.3127f, 0.3290f },  // D65
};

static constexpr displaycolorimetry_t displaycolorimetry_2020
{
	.primaries = { { 0.708f, 0.292f }, { 0.170f, 0.797f }, { 0.131f, 0.046f } },
	.white = { 0.3127f, 0.3290f },  // D65
};


extern const glm::mat3 k_xyz_from_709;
extern const glm::mat3 k_709_from_xyz;

extern const glm::mat3 k_xyz_from_2020;
extern const glm::mat3 k_2020_from_xyz;

extern const glm::mat3 k_2020_from_709;
