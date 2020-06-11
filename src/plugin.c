/*
Copyright (C) 2020 github.com/aramg

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdlib.h>
#include <util/threading.h>
#include <util/platform.h>

#include "queue.h"
#include "plugin.h"
#include "plugin_properties.h"
#include "ffmpeg_decode.h"
#include "buffer_util.h"
#include "usb_util.h"
#include "net.h"

#define FPS 10
#define MILLI_SEC 1000
#define NANO_SEC  1000000000

enum class Action {
    None,
    Activate,
    Deactivate,
};

struct droidcam_obs_plugin {
    AdbMgr *adbMgr;
    USBMux *iosMgr;
    obs_source_t *source;
    os_event_t *stop_signal;
    pthread_t audio_thread;
    pthread_t video_thread;
    pthread_t worker_thread;
    enum video_range_type range;
    bool deactivateWNS;
    bool enable_audio;
    bool audio_running;
    bool video_running;
    struct obs_source_audio obs_audio_frame;
    struct obs_source_frame2 obs_video_frame;
    struct ffmpeg_decode video_decoder;
    struct ffmpeg_decode audio_decoder;
    uint64_t time_start;

    queue<Action> action_queue;
    queue<socket_t> audio_socket_queue;
    queue<socket_t> video_socket_queue;

    inline void stop(void) {
        audio_running = false;
        video_running = false;
    }
};

int adb_port = 7173;

#if 0
test_image(&plugin->obs_video_frame, 320);
plugin->obs_video_frame.timestamp = os_gettime_ns();
obs_source_output_video2(plugin->source, &plugin->obs_video_frame);

static void test_image(struct obs_source_frame2 *frame, size_t size) {
    size_t y, x;
    uint8_t *pixels = (uint8_t *)malloc(size * size * 4);
    if (!pixels) return;

    frame->data[0] = pixels;
    frame->linesize[0] = size * 4;
    frame->format = VIDEO_FORMAT_BGRX;
    frame->width = size;
    frame->height = size;

    uint8_t *p = pixels;
    for (y = 0; y < size; y++) {
        for (x = 0; x < size/4; x++) {
            *p++ = 0; *p++ = 0; *p++ = 0xFF; p++;
        }
        for (x = 0; x < size/4; x++) {
            *p++ = 0; *p++ = 0xFF; *p++ = 0; p++;
        }
        for (x = 0; x < size/4; x++) {
            *p++ = 0xFF; *p++ = 0; *p++ = 0; p++;
        }
        for (x = 0; x < size/4; x++) {
            *p++ = 0x80; *p++ = 0x80; *p++ = 0x80; p++;
        }
    }
}
#endif

#define MAXCONFIG 1024
static int read_frame(struct ffmpeg_decode *decoder, uint64_t *pts, socket_t sock, int *has_config) {
    uint8_t header[HEADER_SIZE];
    uint8_t config[MAXCONFIG];
    size_t r;
    size_t len, config_len = 0;

AGAIN:
    r = net_recv_all(sock, header, HEADER_SIZE);
    if (r != HEADER_SIZE) {
        elog("read header recv returned %ld", r);
        return 0;
    }

    *pts = buffer_read64be(header);
    len = buffer_read32be(&header[8]);
    // dlog("read_frame: header: pts=%llu len=%d", *pts, (int)len);
    if (len == 0 || len > 1024 * 1024) {
        return 0;
    }

    if (*pts == NO_PTS) {
        if (config_len != 0) {
             elog("double config ???");
             return 0;
        }

        if (len > MAXCONFIG) {
            elog("config packet too large at %ld!", len);
            return 0;
        }

        r = net_recv_all(sock, config, len);
        if (r != len) {
            elog("read config recv returned %ld", r);
            return 0;
        }
        config_len = len;
        *has_config = 1;
        goto AGAIN;
    }

    uint8_t *p = ffmpeg_decode_get_buffer(decoder, config_len + len);
    if (config_len) {
        memcpy(p, config, config_len);
        p += config_len;
    }

    r = net_recv_all(sock, p, len);
    if (r != len) {
        elog("read_frame: read %ld bytes wanted %ld", r, len);
        return 0;
    }

    return config_len + len;
}

static bool
do_video_frame(struct droidcam_obs_plugin *plugin, socket_t sock) {
    uint64_t pts;
    struct ffmpeg_decode *decoder = &plugin->video_decoder;

    if (ffmpeg_decode_valid(decoder) && decoder->codec->id != AV_CODEC_ID_H264) {
        ffmpeg_decode_free(decoder);
    }

    if (!ffmpeg_decode_valid(decoder)) {
        if (ffmpeg_decode_init_video(decoder, AV_CODEC_ID_H264) < 0) {
            elog("could not initialize video decoder");
            return false;
        }
    }

    int has_config = 0;
    int len = read_frame(decoder, &pts, sock, &has_config);
    if (len == 0)
        return false;

    bool got_output;
    if (!ffmpeg_decode_video(decoder, &pts, len, VIDEO_RANGE_DEFAULT, &plugin->obs_video_frame, &got_output)) {
        elog("error decoding video");
        return false;
    }

    if (got_output) {
        plugin->obs_video_frame.timestamp = pts * 100;
        //if (flip) plugin->obs_video_frame.flip = !plugin->obs_video_frame.flip;
#if 0
        dlog("output video: %dx%d %lu",
            plugin->obs_video_frame.width,
            plugin->obs_video_frame.height,
            plugin->obs_video_frame.timestamp);
#endif
        obs_source_output_video2(plugin->source, &plugin->obs_video_frame);
    }
    return true;
}

static void *video_thread(void *data) {
    droidcam_obs_plugin *plugin = reinterpret_cast<droidcam_obs_plugin *>(data);
    socket_t sock = INVALID_SOCKET;
    const char *video_req = VIDEO_REQ;

    while (os_event_try(plugin->stop_signal) == EAGAIN) {
        if (plugin->video_running) {
            if (!do_video_frame(plugin, sock)) {
                plugin->video_running = false;
                continue;
            }
        } else {
            if (sock != INVALID_SOCKET) {
                dlog("closing video socket %d", sock);
                net_close(sock);
                sock = INVALID_SOCKET;
            }

            obs_source_output_video2(plugin->source, NULL);
            os_sleep_ms(MILLI_SEC / FPS);
        }

        socket_t s = plugin->video_socket_queue.next_item();
        if (s > 0) {
            if (plugin->video_running) {
                elog("dropping video socket %d, already running", s);
                net_close(s);
            } else {
                if (net_send_all(s, video_req, sizeof(VIDEO_REQ)-1) <= 0) {
                    elog("send(/video) failed");
                    net_close(s);
                } else {
                    sock = s;
                    plugin->video_running = true;
                    dlog("starting video via socket %d", s);
                }
            }
        }
    }

    plugin->video_running = false;
    if (sock != INVALID_SOCKET) net_close(sock);
    return NULL;
}

static bool
do_audio_frame(struct droidcam_obs_plugin *plugin, socket_t sock) {
    uint64_t pts;
    struct ffmpeg_decode *decoder = &plugin->audio_decoder;

    // aac decoder doesnt like parsing the header, pass to our init
    int has_config = 0;
    int len = read_frame(decoder, &pts, sock, &has_config);
    if (len == 0)
        return false;

    if (ffmpeg_decode_valid(decoder) && (decoder->codec->id != AV_CODEC_ID_AAC || has_config == 1)) {
        ffmpeg_decode_free(decoder);
    }

    if (!ffmpeg_decode_valid(decoder)) {
        if (ffmpeg_decode_init_audio(decoder, decoder->packet_buffer, AV_CODEC_ID_AAC) < 0) {
            elog("could not initialize audio decoder");
            return false;
        }

        // early out, dont pass config packet to decode
        return true;
    }

    bool got_output;
    if (!ffmpeg_decode_audio(decoder, &plugin->obs_audio_frame, &got_output, len)) {
        elog("error decoding audio");
        return false;
    }

    if (got_output) {
        plugin->obs_audio_frame.timestamp = pts * 100;
#if 0
        dlog("output audio: %d frames: %d HZ, Fmt %d, Chan %d,  pts %lu",
            plugin->obs_audio_frame.frames,
            plugin->obs_audio_frame.samples_per_sec,
            plugin->obs_audio_frame.format,
            plugin->obs_audio_frame.speakers,
            plugin->obs_audio_frame.timestamp);
#endif
        obs_source_output_audio(plugin->source, &plugin->obs_audio_frame);
        // return ((uint64_t)plugin->obs_audio_frame.frames * MILLI_SEC / (uint64_t)plugin->obs_audio_frame.samples_per_sec);
    }

    return true;
}

static void *audio_thread(void *data) {
    droidcam_obs_plugin *plugin = reinterpret_cast<droidcam_obs_plugin *>(data);
    socket_t sock = INVALID_SOCKET;
    int video_wait_counter;
    const char *audio_req = AUDIO_REQ;

    while (os_event_try(plugin->stop_signal) == EAGAIN) {
        if (plugin->audio_running) {
            if (!do_audio_frame(plugin, sock)) {
                plugin->audio_running = false;
                continue;
            }
        } else {
            if (sock != INVALID_SOCKET) {
                dlog("closing audio socket %d", sock);
                net_close(sock);
                sock = INVALID_SOCKET;
            }

            obs_source_output_audio(plugin->source, NULL);
            os_sleep_ms(MILLI_SEC / FPS);
        }

        socket_t s = plugin->audio_socket_queue.next_item();
        if (s > 0) {
            if (plugin->audio_running) {
                elog("dropping audio socket %d, already running", s);
                drop:
                net_close(s);
            } else {
                video_wait_counter = 0;
                while (!plugin->video_running) {
                    os_sleep_ms(MILLI_SEC / FPS);
                    if (++video_wait_counter > FPS) {
                        dlog("audio: waited too long for video to start, dropping socket");
                        goto drop;
                    }
                }
                if (net_send_all(s, audio_req, sizeof(AUDIO_REQ)-1) <= 0) {
                    elog("send(/audio) failed");
                    net_close(s);
                } else {
                    sock = s;
                    plugin->audio_running = true;
                    dlog("starting audio via socket %d", s);
                }
            }
        }
    }

    plugin->audio_running = false;
    if (sock != INVALID_SOCKET) net_close(sock);
    return NULL;
}

static void *worker_thread(void *data) {
    droidcam_obs_plugin *plugin = reinterpret_cast<droidcam_obs_plugin *>(data);

    while (os_event_try(plugin->stop_signal) == EAGAIN) {
        Action action = plugin->action_queue.next_item();
        switch (action) {
            case Action::None:
                break;
            case Action::Activate:
                // TODO auto connect to last device
                break;
        }

        os_sleep_ms(MILLI_SEC / 10);
    }

    return NULL;
}

static void plugin_destroy(void *data) {
    droidcam_obs_plugin *plugin = reinterpret_cast<droidcam_obs_plugin *>(data);

    if (plugin) {
        if (plugin->time_start != 0) {
            dlog("stopping");
            os_event_signal(plugin->stop_signal);
            pthread_join(plugin->video_thread, NULL);
            pthread_join(plugin->audio_thread, NULL);
            pthread_join(plugin->worker_thread, NULL);
        }

        dlog("cleanup");
        os_event_destroy(plugin->stop_signal);

        if (ffmpeg_decode_valid(&plugin->video_decoder))
            ffmpeg_decode_free(&plugin->video_decoder);

        if (ffmpeg_decode_valid(&plugin->audio_decoder))
            ffmpeg_decode_free(&plugin->audio_decoder);

        delete plugin;
    }
}

static void *plugin_create(obs_data_t *settings, obs_source_t *source) {
    dlog("create(source=%p)", source);

    droidcam_obs_plugin *plugin = new droidcam_obs_plugin();
    plugin->source = source;
    plugin->deactivateWNS = obs_data_get_bool(settings, OPT_DEACTIVATE_WNS);

    obs_source_set_async_unbuffered(source, true);

    if (os_event_init(&plugin->stop_signal, OS_EVENT_TYPE_MANUAL) != 0) {
        plugin_destroy(plugin);
        return NULL;
    }

    if (pthread_create(&plugin->video_thread, NULL, video_thread, plugin) != 0) {
        plugin_destroy(plugin);
        return NULL;
    }

    if (pthread_create(&plugin->audio_thread, NULL, audio_thread, plugin) != 0) {
        plugin_destroy(plugin);
        return NULL;
    }

    if (pthread_create(&plugin->worker_thread, NULL, worker_thread, plugin) != 0) {
        plugin_destroy(plugin);
        return NULL;
    }

    plugin->time_start = os_gettime_ns() / 100;
    bool showing = obs_source_showing(source);
    if (showing || !plugin->deactivateWNS)
        plugin->action_queue.add_item(Action::Activate);
    else
        dlog("source not showing");

    return plugin;
}

static void plugin_show(void *data) {
    dlog("show()");
}

static void plugin_hide(void *data) {
    dlog("hide()");
}

static inline void toggle_ppts(obs_properties_t *ppts, bool enable) {
    obs_property_set_enabled(obs_properties_get(ppts, OPT_REFRESH)     , enable);
    obs_property_set_enabled(obs_properties_get(ppts, OPT_DEVICE_LIST) , enable);
    obs_property_set_enabled(obs_properties_get(ppts, OPT_CONNECT_IP)  , enable);
    obs_property_set_enabled(obs_properties_get(ppts, OPT_CONNECT_PORT), enable);
    obs_property_set_enabled(obs_properties_get(ppts, OPT_ENABLE_AUDIO), enable);
}

static bool refresh_clicked(obs_properties_t *ppts, obs_property_t *p, void *data);

static bool connect_clicked(obs_properties_t *ppts, obs_property_t *p, void *data) {
    const char *ip, *device_id;
    int port;
    int use_wifi = 0;
    int is_offline;
    socket_t sock;

    droidcam_obs_plugin *plugin = reinterpret_cast<droidcam_obs_plugin *>(data);
    obs_data_t *settings = obs_source_get_settings(plugin->source);
    obs_property_t *cp = obs_properties_get(ppts, OPT_CONNECT);
    AdbDevice* dev;
    usbmuxd_device_info_t* usbmuxdev;

    AdbMgr* adbMgr = plugin->adbMgr;
    USBMux* iosMgr = plugin->iosMgr;
    if (!adbMgr || !iosMgr){
        elog("unexpected adbMgr==%p, iosMgr==%p in connect_clicked", adbMgr, iosMgr);
        goto out;
    }

    obs_property_set_enabled(cp, false);

    device_id = obs_data_get_string(settings, OPT_DEVICE_LIST);
    port = (int) obs_data_get_int(settings, OPT_CONNECT_PORT);
    if (!device_id)
        goto out;

    dlog("device id: %s", device_id);
    if (memcmp(device_id, OPT_DEVICE_ID_WIFI, sizeof(OPT_DEVICE_ID_WIFI)-1) == 0)
        use_wifi = 1;

    if (plugin->video_running) {
        plugin->stop();
        toggle_ppts(ppts, true);
        obs_property_set_description(cp, TEXT_CONNECT);
        if (!use_wifi)
            adb_forward_remove_all(device_id);

        refresh_clicked(ppts, NULL, data);
        goto out;
    }

    if (use_wifi) {
        ip = obs_data_get_string(settings, OPT_CONNECT_IP);
        goto CONNECT;
    }

    adbMgr->ResetIter();
    while ((dev = adbMgr->NextDevice(&is_offline)) != NULL) {
        dlog("checking against serial:%s state:%s\n", dev->serial, dev->state);
        if (device_id == dev->serial) {
            if (is_offline) {
                // FIXME Show error
                goto out;
            }

            dlog("ADB: mapping %d -> %d\n", adb_port, port);
            if (!adb_forward(dev->serial, adb_port++, port)) {
                // FIXME show error
                goto out;
            }

            ip = "127.0.0.1";
            goto CONNECT;
        }
    }

    iosMgr->ResetIter();
    while ((usbmuxdev = iosMgr->NextDevice()) != NULL) {
        dlog("checking against serial:%s\n", usbmuxdev->udid);
        if (device_id == usbmuxdev->udid) {
            sock = iosMgr->Connect(iosMgr->iter - 1, port);
            goto AFTER_CONNECT;
        }
    }

    // FIXME show error
    goto out;

CONNECT:
    sock = net_connect(ip, port);

AFTER_CONNECT:
    if (sock == INVALID_SOCKET) {
        elog("connect failed");
        // FIXME show dialog?
        goto out;
    }

    plugin->video_socket_queue.add_item(sock);
    obs_property_set_description(cp, TEXT_DEACTIVATE);
    toggle_ppts(ppts, false);

    if (plugin->enable_audio) {
        sock = net_connect(ip, port);
        if (sock != INVALID_SOCKET) {
            plugin->audio_socket_queue.add_item(sock);
        }
    }

out:
    obs_property_set_enabled(cp, true);
    if (settings) obs_data_release(settings);
    return true;
}

static bool refresh_clicked(obs_properties_t *ppts, obs_property_t *p, void *data) {
    droidcam_obs_plugin *plugin = reinterpret_cast<droidcam_obs_plugin *>(data);
    int is_offline;
    AdbDevice* dev;
    AdbMgr *adbMgr = plugin->adbMgr;
    obs_property_t *cp = obs_properties_get(ppts, OPT_CONNECT);
    obs_property_set_enabled(cp, false);

    p = obs_properties_get(ppts, OPT_DEVICE_LIST);
    obs_property_list_clear(p);
    obs_property_list_add_string(p, TEXT_USE_WIFI, OPT_DEVICE_ID_WIFI);

    if (!adbMgr) {
        adbMgr = new AdbMgr();
        plugin->adbMgr = adbMgr;
    }
    adbMgr->Reload();
    adbMgr->ResetIter();
    while ((dev = adbMgr->NextDevice(&is_offline)) != NULL) {
        char *label = dev->model[0] != 0 ? dev->model : dev->serial;
        size_t idx = obs_property_list_add_string(p, label, dev->serial);
        if (is_offline)
            obs_property_list_item_disable(p, idx, true);
    }

    obs_property_set_enabled(cp, true);
    return true;
}

static void plugin_update(void *data, obs_data_t *settings) {
    droidcam_obs_plugin *plugin = reinterpret_cast<droidcam_obs_plugin *>(data);
    plugin->deactivateWNS = obs_data_get_bool(settings, OPT_DEACTIVATE_WNS);
    plugin->enable_audio  = obs_data_get_bool(settings, OPT_ENABLE_AUDIO);

    bool sync_av = obs_data_get_bool(settings, OPT_SYNC_AV);
    dlog("av synced = %d", sync_av);
    obs_source_set_async_decoupled(plugin->source, !sync_av);
}

static obs_properties_t *plugin_properties(void *data) {
    droidcam_obs_plugin *plugin = reinterpret_cast<droidcam_obs_plugin *>(data);
    obs_properties_t *ppts = obs_properties_create();
    obs_property_t *cp;

    obs_properties_add_list(ppts, OPT_DEVICE_LIST, TEXT_DEVICE, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_properties_add_button(ppts, OPT_REFRESH, TEXT_REFRESH, refresh_clicked);

    obs_properties_add_text(ppts, OPT_CONNECT_IP, "WiFi IP", OBS_TEXT_DEFAULT);
    obs_properties_add_int(ppts, OPT_CONNECT_PORT, "DroidCam Port", 1025, 65535, 1);
    obs_properties_add_bool(ppts, OPT_ENABLE_AUDIO, TEXT_ENABLE_AUDIO);

    cp = obs_properties_add_button(ppts, OPT_CONNECT, TEXT_CONNECT, connect_clicked);
    obs_properties_add_bool(ppts, OPT_SYNC_AV, TEXT_SYNC_AV);
    obs_properties_add_bool(ppts, OPT_DEACTIVATE_WNS, TEXT_DWNS);

    if (plugin->video_running) {
        toggle_ppts(ppts, false);
        obs_property_set_description(cp, TEXT_DEACTIVATE);
    } else {
        refresh_clicked(ppts, NULL, data);
    }

    return ppts;
}

static void plugin_defaults(obs_data_t *settings) {
    obs_data_set_default_bool(settings, OPT_SYNC_AV, true);
    obs_data_set_default_bool(settings, OPT_ENABLE_AUDIO, true);
    obs_data_set_default_bool(settings, OPT_DEACTIVATE_WNS, false);
    obs_data_set_default_int(settings, OPT_CONNECT_PORT, 1212);
}

static const char *plugin_getname(void *x) {
    UNUSED_PARAMETER(x);
    return obs_module_text("PluginName");
}

struct obs_source_info droidcam_obs_info;

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("droidcam-obs", "en-US")
MODULE_EXPORT const char *obs_module_description(void) {
    return "Android and iOS camera source";
}

bool obs_module_load(void) {
    memset(&droidcam_obs_info, 0, sizeof(struct obs_source_info));

    droidcam_obs_info.id           = "droidcam_obs";
    droidcam_obs_info.type         = OBS_SOURCE_TYPE_INPUT;
    droidcam_obs_info.output_flags = OBS_SOURCE_DO_NOT_DUPLICATE | OBS_SOURCE_AUDIO | OBS_SOURCE_ASYNC_VIDEO;
    droidcam_obs_info.get_name     = plugin_getname;
    droidcam_obs_info.create       = plugin_create;
    droidcam_obs_info.destroy      = plugin_destroy;
    droidcam_obs_info.show         = plugin_show;
    droidcam_obs_info.hide         = plugin_hide;
    droidcam_obs_info.update       = plugin_update;
    //droidcam_obs_info.icon_type    = OBS_ICON_TYPE_CAMERA;
    droidcam_obs_info.get_defaults = plugin_defaults;
    droidcam_obs_info.get_properties = plugin_properties;
    obs_register_source(&droidcam_obs_info);
    return true;
}

/*
void obs_module_unload(void) {
    if (libvlc) libvlc_release_(libvlc);
#ifdef __APPLE__
    if (libvlc_core_module) os_dlclose(libvlc_core_module);
#endif
    if (libvlc_module) os_dlclose(libvlc_module);
}
*/

