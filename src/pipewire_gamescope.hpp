#pragma once

#include <cstdint>
#include <spa/param/video/format-utils.h>

enum {
    SPA_FORMAT_VIDEO_requested_size = 0x70000,
    SPA_FORMAT_VIDEO_gamescope_focus_appid = 0x70001,
};

enum {
    SPA_META_requested_size_scale = 0x70000
};

struct spa_gamescope
{
    spa_rectangle requested_size;
    uint64_t focus_appid;
};

static inline int
spa_format_video_raw_parse_with_gamescope(const struct spa_pod *format, struct spa_video_info_raw *info, spa_gamescope *gamescope_info)
{
    return spa_pod_parse_object(format,
        SPA_TYPE_OBJECT_Format, NULL,
        SPA_FORMAT_VIDEO_format,                SPA_POD_Id(&info->format),
        SPA_FORMAT_VIDEO_modifier,              SPA_POD_OPT_Long(&info->modifier),
        SPA_FORMAT_VIDEO_size,                  SPA_POD_Rectangle(&info->size),
        SPA_FORMAT_VIDEO_framerate,             SPA_POD_Fraction(&info->framerate),
        SPA_FORMAT_VIDEO_maxFramerate,          SPA_POD_OPT_Fraction(&info->max_framerate),
        SPA_FORMAT_VIDEO_views,                 SPA_POD_OPT_Int(&info->views),
        SPA_FORMAT_VIDEO_interlaceMode,         SPA_POD_OPT_Id(&info->interlace_mode),
        SPA_FORMAT_VIDEO_pixelAspectRatio,      SPA_POD_OPT_Fraction(&info->pixel_aspect_ratio),
        SPA_FORMAT_VIDEO_multiviewMode,         SPA_POD_OPT_Id(&info->multiview_mode),
        SPA_FORMAT_VIDEO_multiviewFlags,        SPA_POD_OPT_Id(&info->multiview_flags),
        SPA_FORMAT_VIDEO_chromaSite,            SPA_POD_OPT_Id(&info->chroma_site),
        SPA_FORMAT_VIDEO_colorRange,            SPA_POD_OPT_Id(&info->color_range),
        SPA_FORMAT_VIDEO_colorMatrix,           SPA_POD_OPT_Id(&info->color_matrix),
        SPA_FORMAT_VIDEO_transferFunction,      SPA_POD_OPT_Id(&info->transfer_function),
        SPA_FORMAT_VIDEO_colorPrimaries,        SPA_POD_OPT_Id(&info->color_primaries),
        SPA_FORMAT_VIDEO_requested_size,        SPA_POD_OPT_Rectangle(&gamescope_info->requested_size),
        SPA_FORMAT_VIDEO_gamescope_focus_appid, SPA_POD_OPT_Long(&gamescope_info->focus_appid));
}

static inline uint32_t spa_format_to_drm(const enum spa_video_format spa_format)
{
	switch (spa_format)
	{
		case SPA_VIDEO_FORMAT_BGRx: return DRM_FORMAT_XRGB8888;
		case SPA_VIDEO_FORMAT_NV12: return DRM_FORMAT_NV12;
		default: return DRM_FORMAT_INVALID;
	}
}
