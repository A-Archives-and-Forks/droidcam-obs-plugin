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

#include <vector>
#include <mutex>

#include "ffmpeg_decode.h"
#include "plugin.h"
#include "buffer_util.h"
#include "adb_command.h"

#define FPS 10
#define MILLI_SEC 1000
#define NANO_SEC  1000000000

enum class Action {
    None,
    Activate,
    Deactivate,
};

struct droidcam_obs_plugin {
    obs_source_t *source;
    os_event_t *stop_signal;
    pthread_t audio_thread;
    pthread_t video_thread;
    pthread_t worker_thread;
    enum video_range_type range;
    struct obs_source_audio obs_audio_frame;
    struct obs_source_frame2 obs_video_frame;
    struct ffmpeg_decode video_decoder;
    struct ffmpeg_decode audio_decoder;
    uint64_t time_start;

    std::mutex actions_lock;
    std::vector<Action> actions;

    inline void add_action(Action action) {
        actions_lock.lock();
        actions.push_back(action);
        actions_lock.unlock();
    }

    inline Action next_action(void) {
        Action action = Action::None;
        if (actions.size()) {
            actions_lock.lock();
            action = actions.front();
            actions.erase(actions.begin());
            actions_lock.unlock();
        }
        return action;
    }
};

#define MAXCONFIG 1024
static int read_frame(FILE *fp, struct ffmpeg_decode *decoder, uint64_t *pts) {
    uint8_t header[HEADER_SIZE];
    uint8_t config[MAXCONFIG];
    size_t r;
    size_t len, config_len = 0;

AGAIN:
    r = fread(header, 1, HEADER_SIZE, fp);
    if (r < HEADER_SIZE) {
        dlog("read_frame: header read only %ld bytes", r);
        return 0;
    }

    *pts = buffer_read64be(header);
    len = buffer_read32be(&header[8]);
    // dlog("read_frame: header: pts=%ld len=%d", *pts, len);
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

        r = fread(config, 1, len, fp);
        dlog("read_frame: read %ld bytes config", r);
        if (r < len) {
            return 0;
        }
        config_len = len;
        goto AGAIN;
    }

    uint8_t *p = ffmpeg_decode_get_buffer(decoder, config_len + len);
    if (config_len) {
        memcpy(p, config, config_len);
        p += config_len;
    }

    r = fread(p, 1, len, fp);
    if (r < len) {
        dlog("error: read_frame: read %ld bytes wanted %ld", r, len);
        return 0;
    }

    return config_len + len;
}

static void do_video_frame(FILE *fp, struct droidcam_obs_plugin *plugin) {
    uint64_t pts;
    struct ffmpeg_decode *decoder = &plugin->video_decoder;

    if (ffmpeg_decode_valid(decoder) && decoder->codec->id != AV_CODEC_ID_H264) {
        ffmpeg_decode_free(decoder);
    }

    if (!ffmpeg_decode_valid(decoder)) {
        if (ffmpeg_decode_init_video(decoder, AV_CODEC_ID_H264) < 0) {
            elog("could not initialize video decoder");
            return;
        }
    }

    int len = read_frame(fp, decoder, &pts);
    if (len == 0)
        return;

    bool got_output;
    if (!ffmpeg_decode_video(decoder, &pts, len, VIDEO_RANGE_DEFAULT, &plugin->obs_video_frame, &got_output)) {
        elog("error decoding video");
        return;
    }

    if (got_output) {
        plugin->obs_video_frame.timestamp = pts * 100;
        //if (flip) plugin->obs_video_frame.flip = !plugin->obs_video_frame.flip;
#if 1
        dlog("output video: %dx%d %lu",
            plugin->obs_video_frame.width,
            plugin->obs_video_frame.height,
            plugin->obs_video_frame.timestamp);
#endif
        obs_source_output_video2(plugin->source, &plugin->obs_video_frame);
    }
}

// https://obsproject.com/docs/reference-sources.html#c.obs_source_output_video
static void *video_thread(void *data) {
    droidcam_obs_plugin *plugin = reinterpret_cast<droidcam_obs_plugin *>(data);
    uint64_t cur_time = os_gettime_ns();

    FILE *fp = fopen("/home/user/dev/droidcam-obs/rec.h264", "rb");
    if (!fp) {
        elog("failed to open file");
        return NULL;
    }

    while (os_event_try(plugin->stop_signal) == EAGAIN) {
        if (!feof(fp)) {
            test_image(&plugin->obs_video_frame, 320);
            plugin->obs_video_frame.timestamp = (cur_time += (NANO_SEC/FPS));
            obs_source_output_video2(plugin->source, &plugin->obs_video_frame);
            // do_video_frame(fp, plugin);
        } else {
            rewind(fp);
        }

        os_sleep_ms(MILLI_SEC / FPS);
    }

    fclose(fp);

    return NULL;
}

static uint64_t do_audio_frame(FILE *fp, struct droidcam_obs_plugin *plugin) {
    uint64_t pts;
    struct ffmpeg_decode *decoder = &plugin->audio_decoder;

    int len = read_frame(fp, decoder, &pts);
    if (len == 0)
        return 0;

    if (ffmpeg_decode_valid(decoder) && decoder->codec->id != AV_CODEC_ID_AAC) {
        ffmpeg_decode_free(decoder);
    }

    if (!ffmpeg_decode_valid(decoder)) {
        // XXX need to read (and store?) initial header and not try to decode it
        // aac header parsing seems buggy in ffmpeg
        if (ffmpeg_decode_init_audio(decoder, decoder->packet_buffer, AV_CODEC_ID_AAC) < 0) {
            elog("could not initialize audio decoder");
            return 0;
        }

        return 0;
    }

    bool got_output;
    if (!ffmpeg_decode_audio(decoder, &plugin->obs_audio_frame, &got_output, len)) {
        elog("error decoding audio");
        return 0;
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
        return ((uint64_t)plugin->obs_audio_frame.frames * MILLI_SEC / (uint64_t)plugin->obs_audio_frame.samples_per_sec);
    }

    return 0;
}

static void *audio_thread(void *data) {
    droidcam_obs_plugin *plugin = reinterpret_cast<droidcam_obs_plugin *>(data);
    uint64_t duration;

    FILE *fp = fopen("/home/user/dev/droidcam-obs/rec.aac", "rb");
    if (!fp) {
        elog("failed to open file");
        return NULL;
    }

    while (os_event_try(plugin->stop_signal) == EAGAIN) {
        if (!feof(fp)) {
            duration = do_audio_frame(fp, plugin);
        } else {
            rewind(fp);
            duration = 0;
        }
        if (duration == 0)
            duration = MILLI_SEC;

        os_sleep_ms(duration);
    }

    fclose(fp);
    return NULL;
}

static void *worker_thread(void *data) {
    droidcam_obs_plugin *plugin = reinterpret_cast<droidcam_obs_plugin *>(data);
    AdbMgr adbMgr;

    while (os_event_try(plugin->stop_signal) == EAGAIN) {
        Action action = plugin->next_action();
        switch (action) {
            case Action::None:
                break;
            case Action::Activate:
                dlog("Got Activate action");
                adbMgr.reload();
                break;
            default:;
        }

        os_sleep_ms(MILLI_SEC);
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

    // FIXME need this?
    //obs_source_set_async_unbuffered(source, true);
    //obs_source_set_async_decoupled(source, true);

    if (os_event_init(&plugin->stop_signal, OS_EVENT_TYPE_MANUAL) != 0) {
        plugin_destroy(plugin);
        return NULL;
    }

    if (pthread_create(&plugin->video_thread, NULL, video_thread, plugin) != 0) {
        plugin_destroy(plugin);
        return NULL;
    }

    // if (pthread_create(&plugin->audio_thread, NULL, audio_thread, plugin) != 0) {
    //     plugin_destroy(plugin);
    //     return NULL;
    // }

    if (pthread_create(&plugin->worker_thread, NULL, worker_thread, plugin) != 0) {
        plugin_destroy(plugin);
        return NULL;
    }

    plugin->time_start = os_gettime_ns() / 100;
    plugin->add_action(Action::Activate); // FIXME test

    UNUSED_PARAMETER(settings);
    return plugin;
}

static const char *plugin_getname(void *x) {
    UNUSED_PARAMETER(x);
    return obs_module_text("PluginName");
}

struct obs_source_info droidcam_obs_info;

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("droidcam-obs", "en-US")
MODULE_EXPORT const char *obs_module_description(void) {
    return "Android camera source";
}

bool obs_module_load(void) {
    memset(&droidcam_obs_info, 0, sizeof(struct obs_source_info));

    droidcam_obs_info.id           = "droidcam_obs";
    droidcam_obs_info.type         = OBS_SOURCE_TYPE_INPUT;
    droidcam_obs_info.output_flags = OBS_SOURCE_DO_NOT_DUPLICATE | OBS_SOURCE_AUDIO | OBS_SOURCE_ASYNC_VIDEO;
    droidcam_obs_info.get_name     = plugin_getname;
    droidcam_obs_info.create       = plugin_create;
    droidcam_obs_info.destroy      = plugin_destroy;
    droidcam_obs_info.icon_type    = OBS_ICON_TYPE_CAMERA;
    /*
    .update = vlcs_update,
    .get_defaults = vlcs_defaults,
    .get_properties = vlcs_properties,
    .activate = vlcs_activate,
    .deactivate = vlcs_deactivate,
    */
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

