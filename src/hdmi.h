#pragma once

#include <cstdint>

/* from CTA-861-G */
#define HDMI_EOTF_SDR 0
#define HDMI_EOTF_TRADITIONAL_HDR 1
#define HDMI_EOTF_ST2084 2
#define HDMI_EOTF_HLG 3

#if HAVE_DRM
#include <drm_mode.h>
#else
/**
 * struct hdr_metadata_infoframe - HDR Metadata Infoframe Data.
 *
 * HDR Metadata Infoframe as per CTA 861.G spec. This is expected
 * to match exactly with the spec.
 *
 * Userspace is expected to pass the metadata information as per
 * the format described in this structure.
 */
struct hdr_metadata_infoframe {
	/**
	 * @eotf: Electro-Optical Transfer Function (EOTF)
	 * used in the stream.
	 */
	__u8 eotf;
	/**
	 * @metadata_type: Static_Metadata_Descriptor_ID.
	 */
	__u8 metadata_type;
	/**
	 * @display_primaries: Color Primaries of the Data.
	 * These are coded as unsigned 16-bit values in units of
	 * 0.00002, where 0x0000 represents zero and 0xC350
	 * represents 1.0000.
	 * @display_primaries.x: X coordinate of color primary.
	 * @display_primaries.y: Y coordinate of color primary.
	 */
	struct {
		uint16_t x, y;
	} display_primaries[3];
	/**
	 * @white_point: White Point of Colorspace Data.
	 * These are coded as unsigned 16-bit values in units of
	 * 0.00002, where 0x0000 represents zero and 0xC350
	 * represents 1.0000.
	 * @white_point.x: X coordinate of whitepoint of color primary.
	 * @white_point.y: Y coordinate of whitepoint of color primary.
	 */
	struct {
		uint16_t x, y;
	} white_point;
	/**
	 * @max_display_mastering_luminance: Max Mastering Display Luminance.
	 * This value is coded as an unsigned 16-bit value in units of 1 cd/m2,
	 * where 0x0001 represents 1 cd/m2 and 0xFFFF represents 65535 cd/m2.
	 */
	uint16_t max_display_mastering_luminance;
	/**
	 * @min_display_mastering_luminance: Min Mastering Display Luminance.
	 * This value is coded as an unsigned 16-bit value in units of
	 * 0.0001 cd/m2, where 0x0001 represents 0.0001 cd/m2 and 0xFFFF
	 * represents 6.5535 cd/m2.
	 */
	uint16_t min_display_mastering_luminance;
	/**
	 * @max_cll: Max Content Light Level.
	 * This value is coded as an unsigned 16-bit value in units of 1 cd/m2,
	 * where 0x0001 represents 1 cd/m2 and 0xFFFF represents 65535 cd/m2.
	 */
	uint16_t max_cll;
	/**
	 * @max_fall: Max Frame Average Light Level.
	 * This value is coded as an unsigned 16-bit value in units of 1 cd/m2,
	 * where 0x0001 represents 1 cd/m2 and 0xFFFF represents 65535 cd/m2.
	 */
	uint16_t max_fall;
};

/**
 * struct hdr_output_metadata - HDR output metadata
 *
 * Metadata Information to be passed from userspace
 */
struct hdr_output_metadata {
	/**
	 * @metadata_type: Static_Metadata_Descriptor_ID.
	 */
	uint32_t metadata_type;
	/**
	 * @hdmi_metadata_type1: HDR Metadata Infoframe.
	 */
	union {
		struct hdr_metadata_infoframe hdmi_metadata_type1;
	};
};

#endif
