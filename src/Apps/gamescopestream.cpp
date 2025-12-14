///////////////////////////////////////////////////////////////////////////////////////////////////////
// Gracefully butchered from https://docs.pipewire.org/spa_2examples_2adapter-control_8c-example.html
// by Wim Taymans under MIT
///////////////////////////////////////////////////////////////////////////////////////////////////////
 
#include <cstdio>
#include <cstdint>
#include <cassert>

#include <poll.h>
#include <unistd.h>
#include <signal.h>
#include <libdrm/drm_fourcc.h>
 
#include <spa/utils/result.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/props.h>
#include <spa/debug/format.h>
 
#include <pipewire/pipewire.h>
 
#define DEFAULT_WIDTH  1280
#define DEFAULT_HEIGHT 720
 
#define MAX_BUFFERS     64
 
#include <wayland-client.h>
#include <linux-dmabuf-v1-client-protocol.h>
#include <libdecor.h>

#define WAYLAND_NULL() []<typename... Args> ( void *pData, Args... args ) { }

#include <unordered_map>
#include <vector>

#include "pipewire_gamescope.hpp"
#include "log.hpp"

static LogScope s_StreamLog( "stream" );
 
void spa_gamescopestream_log( struct spa_debug_context *ctx, const char *fmt, ... )
{
    va_list args;
    va_start( args, fmt );
    s_StreamLog.vlogf( LOG_DEBUG, fmt, args );
    va_end( args );
}

struct spa_debug_context s_SpaDebugContext =
{
    .log = spa_gamescopestream_log,
};

struct pw_version {
  int major;
  int minor;
  int micro;
};

struct data {
    const char *path;
 
    wl_display *pDisplay = nullptr;
    wl_compositor *pCompositor = nullptr;
    zwp_linux_dmabuf_v1 *pLinuxDmabuf = nullptr;
    libdecor *pDecor = nullptr;
    libdecor_frame *pFrame = nullptr;
    wl_surface *pSurface = nullptr;
    wl_buffer *pWaylandBuffer = nullptr;

    struct pw_main_loop *loop;
    struct spa_source *reneg;
 
    struct pw_stream *stream;
    struct spa_hook stream_listener;
 
    struct spa_video_info format;
    int32_t stride;
    struct spa_rectangle size;

    bool needs_decor_commit;

    uint32_t appid;
 
    std::unordered_map<uint32_t, std::vector<uint64_t>> m_FormatModifiers;
 
    int counter;
};
 
static void handle_events( struct data *pData )
{
    wl_display_flush( pData->pDisplay );

    if ( wl_display_prepare_read( pData->pDisplay ) == 0 )
    {
        int nRet = 0;
        pollfd pollfd =
        {
            .fd     = wl_display_get_fd( pData->pDisplay ),
            .events = POLLIN,
        };

        do
        {
            nRet = poll( &pollfd, 1, 0 );
        } while ( nRet < 0 && ( errno == EINTR || errno == EAGAIN ) );

        if ( nRet > 0 )
            wl_display_read_events( pData->pDisplay );
        else
            wl_display_cancel_read( pData->pDisplay );
    }

    wl_display_dispatch_pending( pData->pDisplay );
}
 
static struct spa_pod *build_format(struct data *data, struct spa_pod_builder *b, enum spa_video_format format, uint64_t *modifiers, int modifier_count)
{
    struct spa_pod_frame f[3];
    int i, c;
 
    spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add(b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
    spa_pod_builder_add(b, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
    /* format */
    spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format, SPA_POD_Id(format), 0);
    /* modifiers */
    if (modifier_count == 1 && modifiers[0] == DRM_FORMAT_MOD_INVALID) {
            // we only support implicit modifiers, use shortpath to skip fixation phase
            spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
            spa_pod_builder_long(b, modifiers[0]);
    } else if (modifier_count > 0) {
            // build an enumeration of modifiers
            spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
            spa_pod_builder_push_choice(b, &f[1], SPA_CHOICE_Enum, 0);
            // modifiers from the array
            for (i = 0, c = 0; i < modifier_count; i++) {
                    spa_pod_builder_long(b, modifiers[i]);
                    if (c++ == 0)
                            spa_pod_builder_long(b, modifiers[i]);
            }
            spa_pod_builder_pop(b, &f[1]);
    }

    spa_rectangle default_size = SPA_RECTANGLE(DEFAULT_WIDTH, DEFAULT_HEIGHT);
    spa_rectangle min_size = SPA_RECTANGLE(1,1);
    spa_rectangle max_size = SPA_RECTANGLE(65535, 65535);

    spa_fraction frac1 = SPA_FRACTION(25,1);
    spa_fraction frac2 = SPA_FRACTION(0,1);
    spa_fraction frac3 = SPA_FRACTION(30,1);

    spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size,
        SPA_POD_CHOICE_RANGE_Rectangle( &default_size, &min_size, &max_size),
        0);
    spa_pod_builder_add(b, SPA_FORMAT_VIDEO_framerate,
        SPA_POD_CHOICE_RANGE_Fraction(
            &frac1,
            &frac2,
            &frac3),
        0);
    
    if (data->appid)
        spa_pod_builder_add(b, SPA_FORMAT_VIDEO_gamescope_focus_appid, SPA_POD_Long(uint64_t(data->appid)), 0);

    return (spa_pod *)spa_pod_builder_pop(b, &f[0]);
}

void commit_libdecor( struct data *data, libdecor_configuration *pConfiguration )
{
    uint32_t uWidth = data->format.info.raw.size.width;
    uint32_t uHeight = data->format.info.raw.size.height;
    uWidth = uWidth ? uWidth : 1280;
    uHeight = uHeight ? uHeight : 720;

    libdecor_state *pState = libdecor_state_new( uWidth, uHeight );
    libdecor_frame_commit( data->pFrame, pState, pConfiguration );
    libdecor_state_free( pState );

    data->needs_decor_commit = false;
}
 
/* our data processing function is in general:
 *
 *  struct pw_buffer *b;
 *  b = pw_stream_dequeue_buffer(stream);
 *
 *  .. do stuff with buffer ...
 *
 *  pw_stream_queue_buffer(stream, b);
 */
static void
on_process(void *_data)
{
    struct data *data = (struct data *)_data;
    struct pw_stream *stream = data->stream;
    struct pw_buffer *b;
    struct spa_buffer *buf;
 
    b = nullptr;
    /* dequeue and queue old buffers, use the last available
     * buffer */
    while (true) {
        struct pw_buffer *t;
        if ((t = pw_stream_dequeue_buffer(stream)) == nullptr)
            break;
        if (b)
            pw_stream_queue_buffer(stream, b);
        b = t;
    }
    if (b == nullptr) {
        pw_log_warn("out of buffers: %m");
        return;
    }
 
    buf = b->buffer;
 
    pw_log_info("new buffer %p", buf);
 
    handle_events(data);

    zwp_linux_buffer_params_v1 *pBufferParams = zwp_linux_dmabuf_v1_create_params( data->pLinuxDmabuf );
    if ( !pBufferParams )
    {
        pw_stream_queue_buffer(stream, b);
        return;
    }

    for ( uint32_t i = 0; i < buf->n_datas; i++ )
    {
        zwp_linux_buffer_params_v1_add(
            pBufferParams,
            buf->datas[i].fd,
            i,
            buf->datas[i].chunk->offset,
            buf->datas[i].chunk->stride,
            data->format.info.raw.modifier >> 32,
            data->format.info.raw.modifier & 0xffffffff);
    }

    uint32_t uDrmFormat = spa_format_to_drm(data->format.info.raw.format);
    
    wl_buffer *pImportedBuffer = zwp_linux_buffer_params_v1_create_immed(
        pBufferParams,
        data->format.info.raw.size.width,
        data->format.info.raw.size.height,
        uDrmFormat,
        0u );

    assert( pImportedBuffer );

    struct StreamBuffer
    {
        struct data *pData = nullptr;
        wl_buffer *pWaylandBuffer = nullptr;
        pw_buffer *pPipewireBuffer = nullptr;
    };

    StreamBuffer *pStreamBuffer = new StreamBuffer
    {
        .pData = data,
        .pWaylandBuffer = pImportedBuffer,
        .pPipewireBuffer = b,
    };
    static constexpr wl_buffer_listener s_BufferListener =
    {
        .release = []( void *pData, wl_buffer *pBuffer )
        {
            StreamBuffer *pStreamBuffer = ( StreamBuffer * )pData;
            pw_stream_queue_buffer( pStreamBuffer->pData->stream, pStreamBuffer->pPipewireBuffer );
            wl_buffer_destroy( pStreamBuffer->pWaylandBuffer );
            delete pStreamBuffer;
        },
    };
    wl_buffer_add_listener( pImportedBuffer, &s_BufferListener, pStreamBuffer );

    wl_surface_attach( data->pSurface, pImportedBuffer, 0, 0 );
    wl_surface_damage( data->pSurface, 0, 0, INT32_MAX, INT32_MAX );
    wl_surface_set_buffer_scale( data->pSurface, 1 );

    if (data->needs_decor_commit)
        commit_libdecor( data, nullptr );
    wl_surface_commit( data->pSurface );

    wl_display_flush( data->pDisplay );
}
 
static void on_stream_state_changed(void *_data, enum pw_stream_state old,
                    enum pw_stream_state state, const char *error)
{
    struct data *data = (struct data *)_data;
    s_StreamLog.debugf( "stream state: \"%s\"", pw_stream_state_as_string(state) );
    if ( error )
        s_StreamLog.errorf( "error: \"%s\"", error );
    switch (state) {
    case PW_STREAM_STATE_UNCONNECTED:
        pw_main_loop_quit(data->loop);
        break;
    case PW_STREAM_STATE_PAUSED:
        break;
    case PW_STREAM_STATE_STREAMING:
    default:
        break;
    }
}
 
/* Be notified when the stream param changes. We're only looking at the
 * format changes.
 *
 * We are now supposed to call pw_stream_finish_format() with success or
 * failure, depending on if we can support the format. Because we gave
 * a list of supported formats, this should be ok.
 *
 * As part of pw_stream_finish_format() we can provide parameters that
 * will control the buffer memory allocation. This includes the metadata
 * that we would like on our buffer, the size, alignment, etc.
 */
static void
on_stream_param_changed(void *_data, uint32_t id, const struct spa_pod *param)
{
    struct data *data = (struct data *)_data;
    struct pw_stream *stream = data->stream;
    uint8_t params_buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
    const struct spa_pod *params[1];
 
    /* nullptr means to clear the format */
    if (param == nullptr || id != SPA_PARAM_Format)
        return;
 
    s_StreamLog.debugf( "got format:" );
    spa_debugc_format(&s_SpaDebugContext, 2, nullptr, param);
 
    if (spa_format_parse(param, &data->format.media_type, &data->format.media_subtype) < 0)
        return;
 
    if (data->format.media_type != SPA_MEDIA_TYPE_video ||
        data->format.media_subtype != SPA_MEDIA_SUBTYPE_raw)
        return;
 
    /* call a helper function to parse the format for us. */
    spa_format_video_raw_parse(param, &data->format.info.raw);
    data->size = data->format.info.raw.size;
 
    uint32_t drm_format = spa_format_to_drm(data->format.info.raw.format);
    if (drm_format == DRM_FORMAT_INVALID) {
        pw_stream_set_error(stream, -EINVAL, "unknown pixel format");
        return;
    }
    if (data->size.width == 0 || data->size.height == 0) {
        pw_stream_set_error(stream, -EINVAL, "invalid size");
        return;
    }

    data->stride = SPA_ROUND_UP_N( data->size.width * 4, 4 );
 
    /* a SPA_TYPE_OBJECT_ParamBuffers object defines the acceptable size,
     * number, stride etc of the buffers */
    params[0] = (const struct spa_pod *) spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, MAX_BUFFERS),
        SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
        SPA_PARAM_BUFFERS_size,    SPA_POD_Int(data->stride * data->size.height),
        SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(data->stride),
        SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1<<SPA_DATA_DmaBuf)));
 
    /* we are done */
    pw_stream_update_params(stream, params, 1);
}
 
/* these are the stream events we listen for */
static const struct pw_stream_events stream_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = on_stream_state_changed,
    .param_changed = on_stream_param_changed,
    .process = on_process,
};
 
static int build_formats(struct data *data, struct spa_pod_builder *b, const struct spa_pod **params)
{
    int n_params = 0;
 
    if (data->m_FormatModifiers.contains(DRM_FORMAT_XRGB8888))
        params[n_params++] = build_format( data, b, SPA_VIDEO_FORMAT_BGRx, data->m_FormatModifiers[DRM_FORMAT_XRGB8888].data(), uint32_t( data->m_FormatModifiers[DRM_FORMAT_XRGB8888].size() ) );
    params[n_params++] = build_format( data, b, SPA_VIDEO_FORMAT_BGRx, nullptr, 0 );
 
    for (int i=0; i < n_params; i++)
        spa_debugc_format(&s_SpaDebugContext, 2, NULL, params[i]);

    return n_params;
}
 
static void reneg_format(void *_data, uint64_t expiration)
{
    struct data *data = (struct data*) _data;
    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *params[2];
    uint32_t n_params;
 
    if (data->format.info.raw.format == 0)
        return;
 
    s_StreamLog.debugf( "renegotiate formats:" );
    n_params = build_formats(data, &b, params);
 
    pw_stream_update_params(data->stream, params, n_params);
}
 
static void do_quit(void *userdata, int signal_number)
{
    struct data *data = (struct data *)userdata;
    pw_main_loop_quit(data->loop);
}
 
int main(int argc, char *argv[])
{
    struct data data = { 0, };
    const struct spa_pod *params[2];
    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    struct pw_properties *props;
    int res, n_params;
 
    pw_init(&argc, &argv);
 
    /* create a main loop */
    data.loop = pw_main_loop_new(nullptr);
 
    pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGINT, do_quit, &data);
    pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGTERM, do_quit, &data);
 
    /* create a simple stream, the simple stream manages to core and remote
     * objects for you if you don't need to deal with them
     *
     * If you plan to autoconnect your stream, you need to provide at least
     * media, category and role properties
     *
     * Pass your events and a user_data pointer as the last arguments. This
     * will inform you about the stream state. The most important event
     * you need to listen to is the process event where you need to consume
     * the data provided to you.
     */
    props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Video",
            PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_ROLE, "Camera",
            nullptr),
    data.appid = argc > 1 ? atoi(argv[1]) : 0;
    data.path = argc > 2 ? argv[2] : "gamescope";
    if (data.path)
        /* Set stream target if given on command line */
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, data.path);
 
    data.stream = pw_stream_new_simple(
            pw_main_loop_get_loop(data.loop),
            "video-play-fixate",
            props,
            &stream_events,
            &data);
 
    //

    if ( !( data.pDisplay = wl_display_connect( nullptr ) ) )
        return -1;

    wl_registry *pRegistry;
    if ( !( pRegistry = wl_display_get_registry( data.pDisplay ) ) )
        return -1;

    static constexpr wl_registry_listener s_RegistryListener =
    {
        .global = [] ( void *pUserData, wl_registry *pRegistry, uint32_t uName, const char *pInterface, uint32_t uVersion )
        {
            struct data *pData = (struct data *)pUserData;
            if ( !strcmp( pInterface, wl_compositor_interface.name ) && uVersion >= 4u )
            {
                pData->pCompositor = (wl_compositor *)wl_registry_bind( pRegistry, uName, &wl_compositor_interface, 4u );
            }
            else if ( !strcmp( pInterface, zwp_linux_dmabuf_v1_interface.name ) && uVersion >= 3 )
            {
                pData->pLinuxDmabuf = (zwp_linux_dmabuf_v1 *)wl_registry_bind( pRegistry, uName, &zwp_linux_dmabuf_v1_interface, 3u );
                static constexpr zwp_linux_dmabuf_v1_listener s_Listener =
                {
                    .format   = WAYLAND_NULL(), // Formats are also advertised by the modifier event, ignore them here.
                    .modifier = [] ( void *pUserData, zwp_linux_dmabuf_v1 *pDmabuf, uint32_t uFormat, uint32_t uModifierHi, uint32_t uModifierLo )
                    {
                        uint64_t ulModifier = ( uint64_t( uModifierHi ) << 32 ) | uModifierLo;

                        struct data *pData = (struct data *)pUserData;
                        if ( ulModifier != DRM_FORMAT_MOD_INVALID )
                            pData->m_FormatModifiers[ uFormat ].emplace_back( ulModifier );
                    },
                };
                zwp_linux_dmabuf_v1_add_listener( pData->pLinuxDmabuf, &s_Listener, pData );
            }
        },
        .global_remove = WAYLAND_NULL(),
    };

    wl_registry_add_listener( pRegistry, &s_RegistryListener, (void *)&data );
    wl_display_roundtrip( data.pDisplay );

    if ( !data.pCompositor || !data.pLinuxDmabuf )
        return -1;

    // Grab stuff from any extra bindings/listeners we set up, eg. format/modifiers.
    wl_display_roundtrip( data.pDisplay );

    wl_registry_destroy( pRegistry );
    pRegistry = nullptr;

    static libdecor_interface s_LibDecorInterface =
    {
        .error = []( libdecor *pContext, libdecor_error eError, const char *pMessage )
        {
            s_StreamLog.errorf( "libdecor: %s", pMessage );
        },
    };
    data.pDecor = libdecor_new( data.pDisplay, &s_LibDecorInterface );
    if ( !data.pDecor )
        return -1;

    static libdecor_frame_interface s_LibDecorFrameInterface
    {
	    .configure     = []( libdecor_frame *pFrame, libdecor_configuration *pConfiguration, void *pUserData )
        {
            struct data *pData = (struct data *)pUserData;
            commit_libdecor( pData, pConfiguration );
        },
        .close         = []( libdecor_frame *pFrame, void *pUserData )
        {
            raise( SIGTERM );
        },
        .commit        = []( libdecor_frame *pFrame, void *pUserData )
        {
            struct data *pData = (struct data *)pUserData;
            pData->needs_decor_commit = true;
        },
        .dismiss_popup = []( libdecor_frame *pFrame, const char *pSeatName, void *pUserData )
        {
        },
    };
    data.pSurface = wl_compositor_create_surface( data.pCompositor );
    data.pFrame = libdecor_decorate( data.pDecor, data.pSurface, &s_LibDecorFrameInterface, &data );
    libdecor_frame_set_title( data.pFrame, "Gamescope Pipewire Stream" );
    libdecor_frame_set_app_id( data.pFrame, "gamescopestream" );
    libdecor_frame_map( data.pFrame );
    wl_surface_commit( data.pSurface );
    wl_display_roundtrip( data.pDisplay );

    //
 
    /* build the extra parameters to connect with. To connect, we can provide
     * a list of supported formats.  We use a builder that writes the param
     * object to the stack. */
    s_StreamLog.debugf( "supported formats:" );
    n_params = build_formats(&data, &b, params);
 
    /* now connect the stream, we need a direction (input/output),
     * an optional target node to connect to, some flags and parameters
     */
    if ((res = pw_stream_connect(data.stream,
              PW_DIRECTION_INPUT,
              PW_ID_ANY,
              pw_stream_flags(
              PW_STREAM_FLAG_AUTOCONNECT |  /* try to automatically connect this stream */
              PW_STREAM_FLAG_MAP_BUFFERS),   /* mmap the buffer data for us */
              params, n_params))        /* extra parameters, see above */ < 0) {
        s_StreamLog.errorf( "can't connect: %s\n", spa_strerror(res) );
        return -1;
    }
 
    data.reneg = pw_loop_add_event(pw_main_loop_get_loop(data.loop), reneg_format, &data);
 
    /* do things until we quit the mainloop */
    pw_main_loop_run(data.loop);
 
    pw_stream_destroy(data.stream);
    pw_main_loop_destroy(data.loop);
 
    // TODO: cleanup wayland

    pw_deinit();
 
    return 0;
}