/*
 * Surge XT - a free and open source hybrid synthesizer,
 * built by Surge Synth Team
 *
 * Learn more at https://surge-synthesizer.github.io/
 *
 * Copyright 2018-2026, various authors, as described in the GitHub
 * transaction log.
 *
 * Surge XT is released under the GNU General Public Licence v3
 * or later (GPL-3.0-or-later). The license is found in the "LICENSE"
 * file in the root of this repository, or at
 * https://www.gnu.org/licenses/gpl-3.0.en.html
 *
 * Surge was a commercial product from 2004-2018, copyright and ownership
 * held by Claes Johanson at Vember Audio during that period.
 * Claes made Surge open source in September 2018.
 *
 * All source for Surge XT is available at
 * https://github.com/surge-synthesizer/surge
 */

/*
 * The main module for the WASM demo page. It hosts the Surge XT CLAP side
 * module and flattens the parts the demo UI needs into a small C API
 * (sh_* functions below) which the JavaScript in index.html drives through
 * cwrap. Events from the UI are queued here and delivered to the plugin at
 * the start of the next sh_render() call.
 */

#include <emscripten.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#include <clap/clap.h>

static const clap_plugin_entry_t *entry = NULL;
static const clap_plugin_t *plugin = NULL;
static const clap_plugin_params_t *params = NULL;

/* signals from the plugin back to the UI, drained by sh_poll() */
static uint32_t pending_rescan_flags = 0;
static int callback_requested = 0;

static void host_params_rescan(const clap_host_t *h, clap_param_rescan_flags flags)
{
    (void)h;
    pending_rescan_flags |= flags;
}
static void host_params_clear(const clap_host_t *h, clap_id param_id, clap_param_clear_flags flags)
{
    (void)h;
    (void)param_id;
    (void)flags;
}
static void host_params_request_flush(const clap_host_t *h) { (void)h; }

static const clap_host_params_t host_params = {host_params_rescan, host_params_clear,
                                               host_params_request_flush};

static const void *host_get_extension(const struct clap_host *h, const char *eid)
{
    (void)h;
    if (!strcmp(eid, CLAP_EXT_PARAMS))
        return &host_params;
    return NULL;
}
static void host_request_restart(const struct clap_host *h) { (void)h; }
static void host_request_process(const struct clap_host *h) { (void)h; }
static void host_request_callback(const struct clap_host *h)
{
    (void)h;
    callback_requested = 1;
}

static const clap_host_t host = {CLAP_VERSION_INIT,
                                 NULL,
                                 "Surge XT WASM Demo",
                                 "Surge Synth Team",
                                 "https://surge-synthesizer.github.io/",
                                 "1.0",
                                 host_get_extension,
                                 host_request_restart,
                                 host_request_process,
                                 host_request_callback};

/* UI events waiting for the next render; the page is single threaded, so a
 * plain array is enough */
typedef union
{
    clap_event_header_t hdr;
    clap_event_note_t note;
    clap_event_param_value_t pv;
    clap_event_midi_t midi;
} demo_event;

#define EVQ_MAX 1024
static demo_event evq[EVQ_MAX];
static uint32_t evq_n = 0;

static demo_event *evq_push(uint16_t type, uint16_t size)
{
    if (evq_n >= EVQ_MAX)
        return NULL;
    demo_event *e = &evq[evq_n++];
    memset(e, 0, sizeof(*e));
    e->hdr.size = size;
    e->hdr.time = 0;
    e->hdr.space_id = CLAP_CORE_EVENT_SPACE_ID;
    e->hdr.type = type;
    return e;
}

static uint32_t in_size(const struct clap_input_events *list)
{
    (void)list;
    return evq_n;
}
static const clap_event_header_t *in_get(const struct clap_input_events *list, uint32_t index)
{
    (void)list;
    return index < evq_n ? &evq[index].hdr : NULL;
}
static bool out_try_push(const struct clap_output_events *list, const clap_event_header_t *ev)
{
    (void)list;
    (void)ev;
    return true;
}

static const clap_input_events_t in_events = {NULL, in_size, in_get};
static const clap_output_events_t out_events = {NULL, out_try_push};

/*
 * The sh_* API used from JavaScript
 */

EMSCRIPTEN_KEEPALIVE
int sh_load(const char *path)
{
    void *handle = dlopen(path, RTLD_NOW);
    if (!handle)
    {
        fprintf(stderr, "demo: dlopen failed: %s\n", dlerror());
        return 0;
    }

    entry = (const clap_plugin_entry_t *)dlsym(handle, "clap_entry");
    if (!entry || !entry->init(path))
        return 0;

    const clap_plugin_factory_t *factory =
        (const clap_plugin_factory_t *)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    const clap_plugin_descriptor_t *desc = factory->get_plugin_descriptor(factory, 0);

    plugin = factory->create_plugin(factory, &host, desc->id);
    if (!plugin || !plugin->init(plugin))
        return 0;

    params = (const clap_plugin_params_t *)plugin->get_extension(plugin, CLAP_EXT_PARAMS);
    return params != NULL;
}

EMSCRIPTEN_KEEPALIVE
const char *sh_plugin_name(void)
{
    static char buf[256];
    const clap_plugin_factory_t *factory =
        (const clap_plugin_factory_t *)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    const clap_plugin_descriptor_t *desc = factory->get_plugin_descriptor(factory, 0);
    snprintf(buf, sizeof(buf), "%s %s", desc->name, desc->version);
    return buf;
}

EMSCRIPTEN_KEEPALIVE
int sh_start(double sample_rate, int max_frames)
{
    if (!plugin->activate(plugin, sample_rate, 32, (uint32_t)max_frames))
        return 0;
    return plugin->start_processing(plugin);
}

EMSCRIPTEN_KEEPALIVE
int sh_param_count(void) { return (int)params->count(plugin); }

static clap_param_info_t info_scratch;

EMSCRIPTEN_KEEPALIVE
unsigned int sh_param_id(int index)
{
    if (!params->get_info(plugin, (uint32_t)index, &info_scratch))
        return CLAP_INVALID_ID;
    return info_scratch.id;
}

EMSCRIPTEN_KEEPALIVE
const char *sh_param_name(int index)
{
    if (!params->get_info(plugin, (uint32_t)index, &info_scratch))
        return "";
    return info_scratch.name;
}

EMSCRIPTEN_KEEPALIVE
const char *sh_param_module(int index)
{
    if (!params->get_info(plugin, (uint32_t)index, &info_scratch))
        return "";
    return info_scratch.module;
}

EMSCRIPTEN_KEEPALIVE
double sh_param_value(unsigned int id)
{
    double v = 0;
    params->get_value(plugin, id, &v);
    return v;
}

EMSCRIPTEN_KEEPALIVE
const char *sh_param_text(unsigned int id, double value)
{
    static char buf[256];
    if (!params->value_to_text(plugin, id, value, buf, sizeof(buf)))
        buf[0] = 0;
    return buf;
}

EMSCRIPTEN_KEEPALIVE
void sh_set_param(unsigned int id, double value)
{
    demo_event *e = evq_push(CLAP_EVENT_PARAM_VALUE, sizeof(clap_event_param_value_t));
    if (!e)
        return;
    e->pv.param_id = id;
    e->pv.cookie = NULL;
    e->pv.note_id = -1;
    e->pv.port_index = -1;
    e->pv.channel = -1;
    e->pv.key = -1;
    e->pv.value = value;
}

EMSCRIPTEN_KEEPALIVE
void sh_note(int key, int velocity, int on)
{
    demo_event *e =
        evq_push(on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF, sizeof(clap_event_note_t));
    if (!e)
        return;
    e->note.note_id = -1;
    e->note.port_index = 0;
    e->note.channel = 0;
    e->note.key = key;
    e->note.velocity = velocity / 127.0;
}

EMSCRIPTEN_KEEPALIVE
void sh_midi(int b0, int b1, int b2)
{
    demo_event *e = evq_push(CLAP_EVENT_MIDI, sizeof(clap_event_midi_t));
    if (!e)
        return;
    e->midi.port_index = 0;
    e->midi.data[0] = (uint8_t)b0;
    e->midi.data[1] = (uint8_t)b1;
    e->midi.data[2] = (uint8_t)b2;
}

EMSCRIPTEN_KEEPALIVE
int sh_render(float *out_l, float *out_r, int nframes)
{
    float *chans[2] = {out_l, out_r};

    clap_audio_buffer_t output;
    memset(&output, 0, sizeof(output));
    output.data32 = chans;
    output.channel_count = 2;

    clap_process_t process;
    memset(&process, 0, sizeof(process));
    process.frames_count = (uint32_t)nframes;
    process.audio_outputs = &output;
    process.audio_outputs_count = 1;
    process.in_events = &in_events;
    process.out_events = &out_events;

    clap_process_status st = plugin->process(plugin, &process);
    evq_n = 0;
    return st != CLAP_PROCESS_ERROR;
}

/* Drain plugin-to-host traffic. Returns accumulated CLAP_PARAM_RESCAN_*
 * flags (0 when there is nothing for the UI to do). */
EMSCRIPTEN_KEEPALIVE
unsigned int sh_poll(void)
{
    if (callback_requested)
    {
        callback_requested = 0;
        plugin->on_main_thread(plugin);
    }
    unsigned int flags = pending_rescan_flags;
    pending_rescan_flags = 0;
    return flags;
}
