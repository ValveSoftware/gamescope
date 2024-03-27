#pragma once

#include <span>
#include <cstdint>
#include <optional>
#include <vector>

namespace gamescope
{
    struct BackendConnectorHDRInfo;

    const char *GetPatchedEdidPath();
    void WritePatchedEdid( std::span<const uint8_t> pEdid, const BackendConnectorHDRInfo &hdrInfo, bool bRotate );
    std::vector<uint8_t> GenerateSimpleEdid( uint32_t uWidth, uint32_t uHeight );

    std::optional<std::vector<uint8_t>> PatchEdid( std::span<const uint8_t> pEdid, const BackendConnectorHDRInfo &hdrInfo, bool bRotate );
}