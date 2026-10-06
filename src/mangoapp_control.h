#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

// mangohudctl's message, as mangoapp_proto.h lays it out.
struct mangoapp_ctrl_header {
	long msg_type;
	uint32_t ctrl_msg_type;
	uint32_t version;
} __attribute__((packed));

struct mangoapp_ctrl_msgid1_v1 {
	struct mangoapp_ctrl_header hdr;

	uint8_t no_display;      // 0 = ignore, 1 = hide, 2 = show, 3 = toggle
	uint8_t log_session;
	char log_session_name[64];
	uint8_t reload_config;   // 0 = ignore, 1 or 3 = reload
} __attribute__((packed));

// What one drain of the shared control type asked of the overlay, folded in order.
struct MangoappControlRelay_t
{
	uint32_t uHeldBack = 0;
	// The last explicit no_display set since the last reload, true hides.
	std::optional<bool> obHide;
	// A toggle left over after the last set, or with no set at all.
	bool bToggle = false;
	bool bReloadConfig = false;
	// The same for log_session, a reload stops logging in mangoapp.
	std::optional<bool> obLogging;
	bool bToggleLogging = false;
};

// Folds a whole queue message the way mangoapp's control thread applies it, log_session, reload, then no_display.
static inline void mangoapp_fold_control( const void *pMsg, size_t size, MangoappControlRelay_t &relay )
{
	if ( size < sizeof( mangoapp_ctrl_msgid1_v1 ) )
		return;

	const auto *pCtrl = static_cast<const mangoapp_ctrl_msgid1_v1 *>( pMsg );
	if ( pCtrl->hdr.ctrl_msg_type != 1 )
		return;

	switch ( pCtrl->log_session )
	{
		case 1: relay.obLogging = true;  relay.bToggleLogging = false; break;
		case 2: relay.obLogging = false; relay.bToggleLogging = false; break;
		case 3:
			if ( relay.obLogging )
				relay.obLogging = !*relay.obLogging;
			else
				relay.bToggleLogging = !relay.bToggleLogging;
			break;
		default: break;
	}

	if ( pCtrl->reload_config == 1 || pCtrl->reload_config == 3 )
	{
		relay.bReloadConfig = true;
		relay.obHide.reset();
		relay.bToggle = false;
		relay.obLogging = false;
		relay.bToggleLogging = false;
	}

	switch ( pCtrl->no_display )
	{
		case 1: relay.obHide = true;  relay.bToggle = false; break;
		case 2: relay.obHide = false; relay.bToggle = false; break;
		case 3:
			if ( relay.obHide )
				relay.obHide = !*relay.obHide;
			else
				relay.bToggle = !relay.bToggle;
			break;
		default: break;
	}
}
