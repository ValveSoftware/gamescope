#pragma once

#include <vector>
#include <memory>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#include <openvr.h>
#pragma GCC diagnostic pop

bool vr_init(int argc, char **argv);

bool vrsession_init();
bool vrsession_visible();
void vrsession_present( vr::VRVulkanTextureData_t *pTextureData );

void vrsession_append_instance_exts( std::vector<const char *>& exts );
void vrsession_append_device_exts( VkPhysicalDevice physDev, std::vector<const char *>& exts );

bool vrsession_framesync( uint32_t timeoutMS );
void vrsession_update_touch_mode();

void vrsession_title( const char *title, std::shared_ptr<std::vector<uint32_t>> icon );
bool vrsession_ime_init();

void vrsession_steam_mode( bool bSteamMode );
