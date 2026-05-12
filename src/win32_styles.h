#pragma once

static constexpr uint32_t WS_OVERLAPPED       = 0x00000000u;
static constexpr uint32_t WS_POPUP            = 0x80000000u;
static constexpr uint32_t WS_CHILD            = 0x40000000u;
static constexpr uint32_t WS_MINIMIZE         = 0x20000000u;
static constexpr uint32_t WS_VISIBLE          = 0x10000000u;
static constexpr uint32_t WS_DISABLED         = 0x08000000u;
static constexpr uint32_t WS_CLIPSIBLINGS     = 0x04000000u;
static constexpr uint32_t WS_CLIPCHILDREN     = 0x02000000u;
static constexpr uint32_t WS_MAXIMIZE         = 0x01000000u;
static constexpr uint32_t WS_BORDER           = 0x00800000u;
static constexpr uint32_t WS_DLGFRAME         = 0x00400000u;
static constexpr uint32_t WS_VSCROLL          = 0x00200000u;
static constexpr uint32_t WS_HSCROLL          = 0x00100000u;
static constexpr uint32_t WS_SYSMENU          = 0x00080000u;
static constexpr uint32_t WS_THICKFRAME       = 0x00040000u;
static constexpr uint32_t WS_GROUP            = 0x00020000u;
static constexpr uint32_t WS_TABSTOP          = 0x00010000u;
static constexpr uint32_t WS_MINIMIZEBOX      = 0x00020000u;
static constexpr uint32_t WS_MAXIMIZEBOX      = 0x00010000u;
static constexpr uint32_t WS_CAPTION          = WS_BORDER | WS_DLGFRAME;
static constexpr uint32_t WS_TILED            = WS_OVERLAPPED;
static constexpr uint32_t WS_ICONIC           = WS_MINIMIZE;
static constexpr uint32_t WS_SIZEBOX          = WS_THICKFRAME;
static constexpr uint32_t WS_OVERLAPPEDWINDOW = WS_OVERLAPPED | WS_CAPTION |
                                                WS_SYSMENU | WS_THICKFRAME |
                                                WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
static constexpr uint32_t WS_POPUPWINDOW = WS_POPUP | WS_BORDER | WS_SYSMENU;
static constexpr uint32_t WS_CHILDWINDOW = WS_CHILD;
static constexpr uint32_t WS_TILEDWINDOW = WS_OVERLAPPEDWINDOW;

static constexpr uint32_t WS_EX_DLGMODALFRAME   = 0x00000001u;
static constexpr uint32_t WS_EX_DRAGDETECT      = 0x00000002u; // Undocumented
static constexpr uint32_t WS_EX_NOPARENTNOTIFY  = 0x00000004u;
static constexpr uint32_t WS_EX_TOPMOST         = 0x00000008u;
static constexpr uint32_t WS_EX_ACCEPTFILES     = 0x00000010u;
static constexpr uint32_t WS_EX_TRANSPARENT     = 0x00000020u;
static constexpr uint32_t WS_EX_MDICHILD        = 0x00000040u;
static constexpr uint32_t WS_EX_TOOLWINDOW      = 0x00000080u;
static constexpr uint32_t WS_EX_WINDOWEDGE      = 0x00000100u;
static constexpr uint32_t WS_EX_CLIENTEDGE      = 0x00000200u;
static constexpr uint32_t WS_EX_CONTEXTHELP     = 0x00000400u;
static constexpr uint32_t WS_EX_RIGHT           = 0x00001000u;
static constexpr uint32_t WS_EX_LEFT            = 0x00000000u;
static constexpr uint32_t WS_EX_RTLREADING      = 0x00002000u;
static constexpr uint32_t WS_EX_LTRREADING      = 0x00000000u;
static constexpr uint32_t WS_EX_LEFTSCROLLBAR   = 0x00004000u;
static constexpr uint32_t WS_EX_RIGHTSCROLLBAR  = 0x00000000u;
static constexpr uint32_t WS_EX_CONTROLPARENT   = 0x00010000u;
static constexpr uint32_t WS_EX_STATICEDGE      = 0x00020000u;
static constexpr uint32_t WS_EX_APPWINDOW       = 0x00040000u;
static constexpr uint32_t WS_EX_LAYERED         = 0x00080000u;
static constexpr uint32_t WS_EX_NOINHERITLAYOUT = 0x00100000u;
static constexpr uint32_t WS_EX_NOREDIRECTIONBITMAP = 0x00200000u;
static constexpr uint32_t WS_EX_LAYOUTRTL           = 0x00400000u;
static constexpr uint32_t WS_EX_COMPOSITED          = 0x02000000u;
static constexpr uint32_t WS_EX_NOACTIVATE          = 0x08000000u;
