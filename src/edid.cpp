#include "edid.h"

#include "backend.h"
#include "log.hpp"
#include "hdmi.h"

#include <cassert>
#include <cstring>
#include <cstdlib>

extern "C"
{
#include "libdisplay-info/info.h"
#include "libdisplay-info/edid.h"
#include "libdisplay-info/cta.h"
}

static LogScope edid_log("edid");

namespace gamescope
{
    static constexpr uint32_t EDID_MAX_BLOCK_COUNT = 256;
    static constexpr uint32_t EDID_BLOCK_SIZE = 128;
    static constexpr uint32_t EDID_MAX_STANDARD_TIMING_COUNT = 8;
    static constexpr uint32_t EDID_BYTE_DESCRIPTOR_COUNT = 4;
    static constexpr uint32_t EDID_BYTE_DESCRIPTOR_SIZE = 18;
    static constexpr uint32_t EDID_MAX_DESCRIPTOR_STANDARD_TIMING_COUNT = 6;
    static constexpr uint32_t EDID_MAX_DESCRIPTOR_COLOR_POINT_COUNT = 2;
    static constexpr uint32_t EDID_MAX_DESCRIPTOR_ESTABLISHED_TIMING_III_COUNT = 44;
    static constexpr uint32_t EDID_MAX_DESCRIPTOR_CVT_TIMING_CODES_COUNT = 4;

    static inline uint8_t get_bit_range(uint8_t val, size_t high, size_t low)
    {
        size_t n;
        uint8_t bitmask;

        assert(high <= 7 && high >= low);

        n = high - low + 1;
        bitmask = (uint8_t) ((1 << n) - 1);
        return (uint8_t) (val >> low) & bitmask;
    }

    static inline void set_bit_range(uint8_t *val, size_t high, size_t low, uint8_t bits)
    {
        size_t n;
        uint8_t bitmask;

        assert(high <= 7 && high >= low);

        n = high - low + 1;
        bitmask = (uint8_t) ((1 << n) - 1);
        assert((bits & ~bitmask) == 0);

        *val &= ~(bitmask << low);
        *val |= (uint8_t)(bits << low);
    }


    static inline void patch_edid_checksum(uint8_t* block)
    {
        uint8_t sum = 0;
        for (uint32_t i = 0; i < EDID_BLOCK_SIZE - 1; i++)
            sum += block[i];

        uint8_t checksum = uint32_t(256) - uint32_t(sum);

        block[127] = checksum;
    }

    static bool validate_block_checksum(const uint8_t* data)
    {
        uint8_t sum = 0;
        size_t i;

        for (i = 0; i < EDID_BLOCK_SIZE; i++) {
            sum += data[i];
        }

        return sum == 0;
    }

    static uint8_t encode_max_luminance(float nits)
    {
        if (nits == 0.0f)
            return 0;

        return ceilf((logf(nits / 50.0f) / logf(2.0f)) * 32.0f);
    }

    std::optional<std::vector<uint8_t>> PatchEdid( std::span<const uint8_t> pEdid, const BackendConnectorHDRInfo &hdrInfo, bool bRotate )
    {
        // A zero length indicates that the edid parsing failed.
        if ( pEdid.empty() )
            return std::nullopt;

        std::vector<uint8_t> edid( pEdid.begin(), pEdid.end() );

        if ( bRotate )
        {
            // Patch width, height.
            edid_log.infof("Patching dims %ux%u -> %ux%u", edid[0x15], edid[0x16], edid[0x16], edid[0x15]);
            std::swap(edid[0x15], edid[0x16]);

            for (uint32_t i = 0; i < EDID_BYTE_DESCRIPTOR_COUNT; i++)
            {
                uint8_t *byte_desc_data = &edid[0x36 + i * EDID_BYTE_DESCRIPTOR_SIZE];
                if (byte_desc_data[0] || byte_desc_data[1])
                {
                    uint32_t horiz = (get_bit_range(byte_desc_data[4], 7, 4) << 8) | byte_desc_data[2];
                    uint32_t vert  = (get_bit_range(byte_desc_data[7], 7, 4) << 8) | byte_desc_data[5];
                    edid_log.infof("Patching res %ux%u -> %ux%u", horiz, vert, vert, horiz);
                    std::swap(byte_desc_data[4], byte_desc_data[7]);
                    std::swap(byte_desc_data[2], byte_desc_data[5]);
                    break;
                }
            }

            patch_edid_checksum(&edid[0]);
        }

        // If we are debugging HDR support lazily on a regular Deck,
        // just hotpatch the edid for the game so we get values we want as if we had
        // an external display attached.
        // (Allows for debugging undocked fallback without undocking/redocking)
        if ( !hdrInfo.ShouldPatchEDID() )
            return bRotate ? std::optional( edid ) : std::nullopt;

        // TODO: Allow for override of min luminance
#if 0
        float flMaxPeakLuminance = g_ColorMgmt.pending.hdrTonemapDisplayMetadata.BIsValid() ? 
            g_ColorMgmt.pending.hdrTonemapDisplayMetadata.flWhitePointNits :
            g_ColorMgmt.pending.flInternalDisplayBrightness;
#endif
        // TODO(JoshA): Need to resolve flInternalDisplayBrightness vs new connector hdrinfo mechanism.

        edid_log.infof("[edid] Patching HDR static metadata:\n"
            "    - Max peak luminance = %u nits\n"
            "    - Max frame average luminance = %u nits",
            hdrInfo.uMaxContentLightLevel, hdrInfo.uMaxFrameAverageLuminance );
        const uint8_t new_hdr_static_metadata_block[]
        {
            (1 << HDMI_EOTF_SDR) | (1 << HDMI_EOTF_TRADITIONAL_HDR) | (1 << HDMI_EOTF_ST2084), /* supported eotfs */
            1, /* type 1 */
            encode_max_luminance( float( hdrInfo.uMaxContentLightLevel ) ), /* desired content max peak luminance */
            encode_max_luminance( float( hdrInfo.uMaxFrameAverageLuminance ) ), /* desired content max frame avg luminance */
            0, /* desired content min luminance -- 0 is technically "undefined" */
        };

        int ext_count = int(edid.size() / EDID_BLOCK_SIZE) - 1;
        assert(ext_count == edid[0x7E]);
        bool has_cta_block = false;
        bool has_hdr_metadata_block = false;

        for (int i = 0; i < ext_count; i++)
        {
            uint8_t *ext_data = &edid[EDID_BLOCK_SIZE + i * EDID_BLOCK_SIZE];
            uint8_t tag = ext_data[0];
            if (tag == DI_EDID_EXT_CEA)
            {
                has_cta_block = true;
                uint8_t dtd_start = ext_data[2];
                uint8_t flags = ext_data[3];
                if (dtd_start == 0)
                {
                    edid_log.infof("Hmmmm.... dtd start is 0. Interesting... Not going further! :-(");
                    continue;
                }
                if (flags != 0)
                {
                    edid_log.infof("Hmmmm.... non-zero CTA flags. Interesting... Not going further! :-(");
                    continue;
                }

                const int CTA_HEADER_SIZE = 4;
                int j = CTA_HEADER_SIZE;
                while (j < dtd_start)
                {
                    uint8_t data_block_header = ext_data[j];
                    uint8_t data_block_tag = get_bit_range(data_block_header, 7, 5);
                    uint8_t data_block_size = get_bit_range(data_block_header, 4, 0);

                    if (j + 1 + data_block_size > dtd_start)
                    {
                        edid_log.infof("Hmmmm.... CTA malformatted. Interesting... Not going further! :-(");
                        break;
                    }

                    uint8_t *data_block = &ext_data[j + 1];
                    if (data_block_tag == 7) // extended
                    {
                        uint8_t extended_tag = data_block[0];
                        uint8_t *extended_block = &data_block[1];
                        uint8_t extended_block_size = data_block_size - 1;

                        if (extended_tag == 6) // hdr static
                        {
                            if (extended_block_size >= sizeof(new_hdr_static_metadata_block))
                            {
                                edid_log.infof("Patching existing HDR Metadata with our own!");
                                memcpy(extended_block, new_hdr_static_metadata_block, sizeof(new_hdr_static_metadata_block));
                                has_hdr_metadata_block = true;
                            }
                        }
                    }

                    j += 1 + data_block_size; // account for header size.
                }

                if (!has_hdr_metadata_block)
                {
                    const int hdr_metadata_block_size_plus_headers = sizeof(new_hdr_static_metadata_block) + 2; // +1 for header, +1 for extended header -> +2
                    edid_log.infof("No HDR metadata block to patch... Trying to insert one.");

                    // Assert that the end of the data blocks == dtd_start
                    if (dtd_start != j)
                    {
                        edid_log.infof("dtd_start != end of blocks. Giving up patching. I'm too scared to attempt it.");
                    }

                    // Move back the dtd to make way for our block at the end.
                    uint8_t *dtd = &ext_data[dtd_start];
                    memmove(dtd + hdr_metadata_block_size_plus_headers, dtd, hdr_metadata_block_size_plus_headers);
                    dtd_start += hdr_metadata_block_size_plus_headers;

                    // Data block is where the dtd was.
                    uint8_t *data_block = dtd;

                    // header
                    data_block[0] = 0;
                    set_bit_range(&data_block[0], 7, 5, 7); // extended tag
                    set_bit_range(&data_block[0], 4, 0, sizeof(new_hdr_static_metadata_block) + 1); // size (+1 for extended header, does not include normal header)

                    // extended header
                    data_block[1] = 6; // hdr metadata extended tag
                    memcpy(&data_block[2], new_hdr_static_metadata_block, sizeof(new_hdr_static_metadata_block));
                }

                patch_edid_checksum(ext_data);
                bool sum_valid = validate_block_checksum(ext_data);
                edid_log.infof("CTA Checksum valid? %s", sum_valid ? "Y" : "N");
            }
        }

        if (!has_cta_block)
        {
            edid_log.infof("Couldn't patch for HDR metadata as we had no CTA block! Womp womp =c");
        }

        bool sum_valid = validate_block_checksum(&edid[0]);
        edid_log.infof("BASE Checksum valid? %s", sum_valid ? "Y" : "N");

        return edid;
    }

    const char *GetPatchedEdidPath()
    {
        const char *pszPatchedEdidPath = getenv( "GAMESCOPE_PATCHED_EDID_FILE" );
        if ( !pszPatchedEdidPath || !*pszPatchedEdidPath )
            return nullptr;

        return pszPatchedEdidPath;
    }

    void WritePatchedEdid( std::span<const uint8_t> pEdid, const BackendConnectorHDRInfo &hdrInfo, bool bRotate )
    {
        const char *pszPatchedEdidPath = GetPatchedEdidPath();
        if ( !pszPatchedEdidPath )
            return;

        std::span<const uint8_t> pEdidToWrite = pEdid;

        auto oPatchedEdid = PatchEdid( pEdid, hdrInfo, bRotate );
        if ( oPatchedEdid )
            pEdidToWrite = std::span<const uint8_t>{ oPatchedEdid->begin(), oPatchedEdid->end() };

        char szTmpFilename[PATH_MAX];
        snprintf( szTmpFilename, sizeof( szTmpFilename ), "%s.tmp", pszPatchedEdidPath );

        FILE *pFile = fopen( szTmpFilename, "wb" );
        if ( !pFile )
        {
            edid_log.errorf( "Couldn't open file: %s", szTmpFilename );
            return;
        }

        fwrite( pEdidToWrite.data(), 1, pEdidToWrite.size(), pFile );
        fflush( pFile );
        fclose( pFile );

        // Flip it over.
        rename( szTmpFilename, pszPatchedEdidPath );
        edid_log.infof( "Wrote new edid to: %s", pszPatchedEdidPath );
    }

    // From gamescope_base_edid.bin
    // Fake little thing we can patch.
    // It has one modeline of 90Hz, like Deck OLED.
    static constexpr uint8_t s_GamescopeBaseEdid[] =
    {
        0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x1a, 0x47, 0x69, 0x69,
        0x38, 0xf4, 0x01, 0x00, 0xff, 0x20, 0x01, 0x04, 0xa5, 0x0a, 0x10, 0x78,
        0x17, 0x3c, 0x71, 0xae, 0x51, 0x3c, 0xb9, 0x23, 0x0c, 0x50, 0x54, 0x00,
        0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
        0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x08, 0x34, 0x20, 0x48, 0x31, 0x00,
        0x20, 0x50, 0x14, 0x08, 0x91, 0x40, 0x64, 0xa0, 0x00, 0x00, 0x00, 0x1e,
        0x00, 0x00, 0x00, 0xfc, 0x00, 0x47, 0x61, 0x6d, 0x65, 0x73, 0x63, 0x6f,
        0x70, 0x65, 0x0a, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x2d,
        0x5a, 0x76, 0x77, 0x0e, 0x00, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
        0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02, 0x03, 0x00, 0x00,
        0xe6, 0x06, 0x01, 0x01, 0x6a, 0x6a, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x36
    };
    std::vector<uint8_t> GenerateSimpleEdid( uint32_t uWidth, uint32_t uHeight )
    {
        uWidth  = std::min<uint32_t>( uWidth,  3840 );
        uHeight = std::min<uint32_t>( uHeight, 3840 );

        // Does not patch refresh, nothing has cared about this yet.
        std::vector<uint8_t> edid( s_GamescopeBaseEdid, s_GamescopeBaseEdid + std::size( s_GamescopeBaseEdid ) );

        for (uint32_t i = 0; i < EDID_BYTE_DESCRIPTOR_COUNT; i++)
        {
            uint8_t *byte_desc_data = &edid[0x36 + i * EDID_BYTE_DESCRIPTOR_SIZE];
            if (byte_desc_data[0] || byte_desc_data[1])
            {
                uint32_t oldHoriz = (get_bit_range(byte_desc_data[4], 7, 4) << 8) | byte_desc_data[2];
                uint32_t oldVert  = (get_bit_range(byte_desc_data[7], 7, 4) << 8) | byte_desc_data[5];

                set_bit_range( &byte_desc_data[4], 7, 4, ( uWidth >> 8 ) & 0xff );
                byte_desc_data[2] = uWidth & 0xff;

                set_bit_range( &byte_desc_data[7], 7, 4, ( uHeight >> 8 ) & 0xff );
                byte_desc_data[5] = uHeight & 0xff;

                uint32_t horiz = (get_bit_range(byte_desc_data[4], 7, 4) << 8) | byte_desc_data[2];
                uint32_t vert  = (get_bit_range(byte_desc_data[7], 7, 4) << 8) | byte_desc_data[5];
                edid_log.infof( "Patching res %ux%u -> %ux%u", oldHoriz, oldVert, horiz, vert );
                break;
            }
        }

        patch_edid_checksum(&edid[0]);

        return edid;
    }
}
