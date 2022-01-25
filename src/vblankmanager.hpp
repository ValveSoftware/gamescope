// Try to figure out when vblank is and notify steamcompmgr to render some time before it

int vblank_init( void );

void vblank_mark_possible_vblank( uint64_t nanos );
