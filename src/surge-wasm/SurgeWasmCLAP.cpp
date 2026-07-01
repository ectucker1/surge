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
 * A JUCE-free CLAP wrapper around the Surge XT engine, used to build the
 * WebAssembly side module. It exposes the same C API as the regular CLAP
 * plugin (a single exported clap_entry), minus the features which are
 * disabled in the WASM build: there is no GUI extension (the module is
 * driven by a custom UI), no preset discovery factory, and no bundled
 * factory content - the engine starts on the init patch and patches are
 * exchanged through the CLAP state extension.
 *
 * The processing and event handling here closely follows
 * SurgeSynthProcessor::clap_direct_process() in src/surge-xt, so the two
 * front ends behave the same from a host's point of view.
 */

#include "SurgeSynthesizer.h"
#include "SurgeStorage.h"
#include "version.h"

#include <clap/clap.h>
#include <clap/helpers/plugin.hh>
#include <clap/helpers/plugin.hxx>
#include <clap/helpers/host-proxy.hh>
#include <clap/helpers/host-proxy.hxx>

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace surge::wasm_clap
{

static const char *features[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER,
                                 CLAP_PLUGIN_FEATURE_STEREO, "free and open source", nullptr};

static const clap_plugin_descriptor descriptor = {
    CLAP_VERSION_INIT,
    "org.surge-synth-team.surge-xt",
    "Surge XT",
    "Surge Synth Team",
    "https://surge-synth-team.org/",
    "",
    "",
    Surge::Build::FullVersionStr,
    "Surge XT engine (headless WASM build)",
    features};

using PluginBase = clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Ignore,
                                         clap::helpers::CheckingLevel::Minimal>;

struct SurgeWasmPlugin : public PluginBase, public SurgeSynthesizer::PluginLayer
{
    // Macros are not Parameters in the engine, so give them ids out of the
    // way of the synth-side parameter ids, which start at 0
    static constexpr clap_id macroIdBase = 0x10000000;

    struct ParamSlot
    {
        Parameter *param{nullptr};
        int macro{-1};
        clap_id id{0};
    };

    std::unique_ptr<SurgeSynthesizer> synth;
    std::vector<ParamSlot> slots;
    std::unordered_map<clap_id, ParamSlot *> slotsById;

    int blockPos{0};
    bool isProcessing{false};
    std::atomic<bool> rescanRequested{false};

    // Parameter changes initiated inside the engine (MIDI learn, patch jog
    // etc.) which still have to be announced to the host, as normalized values
    std::vector<std::pair<clap_id, double>> pendingHostNotifications;
    std::mutex pendingHostNotificationMutex;

    SurgeWasmPlugin(const clap_host *host) : PluginBase(&descriptor, host)
    {
        // The sentinel suppresses the factory/user content scan; in the WASM
        // build no content is bundled and patches arrive via the state API
        synth = std::make_unique<SurgeSynthesizer>(
            this, SurgeStorage::skipPatchLoadDataPathSentinel);
        synth->time_data.ppqPos = 0;
        synth->time_data.tempo = 120;

        for (auto *p : synth->storage.getPatch().param_ptr)
        {
            if (p)
                slots.push_back({p, -1, static_cast<clap_id>(p->id)});
        }
        for (int m = 0; m < n_customcontrollers; ++m)
        {
            slots.push_back({nullptr, m, macroIdBase + m});
        }
        for (auto &s : slots)
            slotsById[s.id] = &s;
    }

    /*
     * SurgeSynthesizer::PluginLayer
     */
    void surgeParameterUpdated(const SurgeSynthesizer::ID &id, float f) override
    {
        std::lock_guard<std::mutex> g(pendingHostNotificationMutex);
        pendingHostNotifications.emplace_back(static_cast<clap_id>(id.getSynthSideId()), f);
    }

    void surgeMacroUpdated(long macroNum, float f) override
    {
        std::lock_guard<std::mutex> g(pendingHostNotificationMutex);
        pendingHostNotifications.emplace_back(macroIdBase + static_cast<clap_id>(macroNum), f);
    }

    /*
     * clap_plugin
     */
    bool activate(double sampleRate, uint32_t, uint32_t) noexcept override
    {
        synth->setSamplerate(static_cast<float>(sampleRate));
        return true;
    }

    void deactivate() noexcept override { synth->audio_processing_active = false; }

    bool startProcessing() noexcept override
    {
        isProcessing = true;
        return true;
    }

    void stopProcessing() noexcept override
    {
        isProcessing = false;
        synth->audio_processing_active = false;
    }

    void reset() noexcept override
    {
        blockPos = 0;
        synth->allNotesOff();
    }

    /*
     * Parameters
     */
    bool implementsParams() const noexcept override { return true; }

    uint32_t paramsCount() const noexcept override
    {
        return static_cast<uint32_t>(slots.size());
    }

    bool paramsInfo(uint32_t paramIndex, clap_param_info *info) const noexcept override
    {
        if (paramIndex >= slots.size())
            return false;

        const auto &slot = slots[paramIndex];

        info->id = slot.id;
        info->cookie = const_cast<ParamSlot *>(&slot);
        info->min_value = 0;
        info->max_value = 1;

        if (slot.macro >= 0)
        {
            info->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_MODULATABLE;
            info->default_value = slot.macro == 0 ? 0.5 : 0.0;
            snprintf(info->name, CLAP_NAME_SIZE, "Macro %d", slot.macro + 1);
            snprintf(info->module, CLAP_PATH_SIZE, "%s", "Macros");
        }
        else
        {
            auto *p = slot.param;
            info->flags = CLAP_PARAM_IS_AUTOMATABLE;
            if (p->can_be_nondestructively_modulated())
            {
                info->flags |= CLAP_PARAM_IS_MODULATABLE;
                if (p->per_voice_processing)
                    info->flags |= CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID |
                                   CLAP_PARAM_IS_MODULATABLE_PER_KEY |
                                   CLAP_PARAM_IS_MODULATABLE_PER_CHANNEL;
            }
            info->default_value = p->get_default_value_f01();

            char txt[TXT_SIZE];
            synth->getParameterNameExtendedByFXGroup(synth->idForParameter(p), txt);
            snprintf(info->name, CLAP_NAME_SIZE, "%s", txt);
            snprintf(info->module, CLAP_PATH_SIZE, "%s", clumpName(p));
        }

        return true;
    }

    bool paramsValue(clap_id paramId, double *value) noexcept override
    {
        auto *slot = slotForId(paramId);
        if (!slot)
            return false;

        if (slot->macro >= 0)
            *value = synth->getMacroParameter01(slot->macro);
        else
            *value = synth->getParameter01(synth->idForParameter(slot->param));

        return true;
    }

    bool paramsValueToText(clap_id paramId, double value, char *display,
                           uint32_t size) noexcept override
    {
        auto *slot = slotForId(paramId);
        if (!slot)
            return false;

        if (slot->macro >= 0)
        {
            snprintf(display, size, "%.2f %%", value * 100.0);
        }
        else
        {
            // Don't go via SurgeSynthesizer::getParameterDisplay(id, txt, x);
            // that overload drops x and formats the current value instead
            char txt[TXT_SIZE];
            slot->param->get_display(txt, true, static_cast<float>(value));
            snprintf(display, size, "%s", txt);
        }
        return true;
    }

    bool paramsTextToValue(clap_id paramId, const char *display, double *value) noexcept override
    {
        auto *slot = slotForId(paramId);
        if (!slot)
            return false;

        if (slot->macro >= 0)
        {
            auto v = std::atof(display) / 100.0;
            *value = std::clamp(v, 0.0, 1.0);
            return true;
        }

        float f{0};
        if (synth->stringToNormalizedValue(synth->idForParameter(slot->param), display, f))
        {
            *value = f;
            return true;
        }
        return false;
    }

    void paramsFlush(const clap_input_events *in, const clap_output_events *out) noexcept override
    {
        auto sz = in->size(in);
        for (uint32_t i = 0; i < sz; ++i)
            processEvent(in->get(in, i));

        notifyPendingParams(out, 0);

        if (!isProcessing)
        {
            // Setting params can change internal state, so give the synth a
            // chance to react if the audio engine isn't running
            synth->process();
        }
    }

    /*
     * Audio and note ports; this mirrors the bus layout of the JUCE-wrapped
     * CLAP: stereo main out, scene A and B outs, and a stereo sidechain in
     */
    bool implementsAudioPorts() const noexcept override { return true; }

    uint32_t audioPortsCount(bool isInput) const noexcept override { return isInput ? 1 : 3; }

    bool audioPortsInfo(uint32_t index, bool isInput,
                        clap_audio_port_info *info) const noexcept override
    {
        if (index >= audioPortsCount(isInput))
            return false;

        info->channel_count = 2;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = CLAP_INVALID_ID;
        info->flags = 0;

        if (isInput)
        {
            info->id = 0x1000;
            info->flags = CLAP_AUDIO_PORT_IS_MAIN;
            snprintf(info->name, CLAP_NAME_SIZE, "%s", "Sidechain");
        }
        else
        {
            info->id = 0x2000 + index;
            static const char *names[] = {"Output", "Scene A", "Scene B"};
            if (index == 0)
                info->flags = CLAP_AUDIO_PORT_IS_MAIN;
            snprintf(info->name, CLAP_NAME_SIZE, "%s", names[index]);
        }
        return true;
    }

    bool implementsNotePorts() const noexcept override { return true; }

    uint32_t notePortsCount(bool isInput) const noexcept override { return isInput ? 1 : 0; }

    bool notePortsInfo(uint32_t index, bool isInput,
                       clap_note_port_info *info) const noexcept override
    {
        if (!isInput || index != 0)
            return false;

        info->id = 0x3000;
        info->supported_dialects =
            CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_MIDI_MPE;
        info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
        snprintf(info->name, CLAP_NAME_SIZE, "%s", "Note Input");
        return true;
    }

    /*
     * State: the raw Surge patch stream, same payload the other plugin
     * formats exchange
     */
    bool implementsState() const noexcept override { return true; }

    bool stateSave(const clap_ostream *stream) noexcept override
    {
        synth->populateDawExtraState();

        void *data{nullptr}; // the synth owns this on return
        auto size = synth->saveRaw(&data);

        auto *bytes = static_cast<const char *>(data);
        uint64_t written = 0;
        while (written < size)
        {
            auto n = stream->write(stream, bytes + written, size - written);
            if (n <= 0)
                return false;
            written += n;
        }
        return true;
    }

    bool stateLoad(const clap_istream *stream) noexcept override
    {
        std::vector<char> data;
        char buffer[8192];
        while (true)
        {
            auto n = stream->read(stream, buffer, sizeof(buffer));
            if (n < 0)
                return false;
            if (n == 0)
                break;
            data.insert(data.end(), buffer, buffer + n);
        }

        if (data.empty())
            return false;

        synth->enqueuePatchForLoad(data.data(), static_cast<int>(data.size()));
        synth->processAudioThreadOpsWhenAudioEngineUnavailable();
        return true;
    }

    /*
     * Processing; a port of SurgeSynthProcessor::clap_direct_process()
     */
    clap_process_status process(const clap_process *process) noexcept override
    {
        if (process->audio_outputs_count == 0 || process->audio_outputs_count > 3)
            return CLAP_PROCESS_ERROR;
        if (process->audio_outputs[0].channel_count > 2 ||
            process->audio_outputs[0].channel_count == 0)
            return CLAP_PROCESS_ERROR;

        if (!synth->audio_processing_active)
        {
            // I am just becoming active. There may be lingering notes from
            // when I was deactivated so
            synth->stopSound();
        }
        synth->audio_processing_active = true;

        applyTransport(process->transport);
        notifyPendingParams(process->out_events, 0);

        auto ev = process->in_events;
        auto evtsz = ev->size(ev);
        uint32_t currev = 0;
        int64_t nextevtime = -1;

        if (evtsz > 0)
            nextevtime = ev->get(ev, 0)->time;

        float *outL{nullptr}, *outR{nullptr}, *inL{nullptr}, *inR{nullptr};
        outL = process->audio_outputs[0].data32[0];
        outR = outL;
        if (process->audio_outputs[0].channel_count == 2)
            outR = process->audio_outputs[0].data32[1];

        float *sceneAL{nullptr}, *sceneAR{nullptr}, *sceneBL{nullptr}, *sceneBR{nullptr};
        bool haveSceneOut{false};

        if (process->audio_inputs_count >= 1 && process->audio_inputs[0].channel_count >= 1 &&
            process->audio_inputs[0].data32)
        {
            synth->process_input = true;
            inL = process->audio_inputs[0].data32[0];
            inR = inL;
            if (process->audio_inputs[0].channel_count == 2)
                inR = process->audio_inputs[0].data32[1];
        }
        else
        {
            synth->process_input = false;
        }

        if (process->audio_outputs_count == 3 && process->audio_outputs[1].channel_count == 2 &&
            process->audio_outputs[2].channel_count == 2)
        {
            haveSceneOut = true;
            sceneAL = process->audio_outputs[1].data32[0];
            sceneAR = process->audio_outputs[1].data32[1];
            sceneBL = process->audio_outputs[2].data32[0];
            sceneBR = process->audio_outputs[2].data32[1];

            if (!sceneAL || !sceneAR || !sceneBL || !sceneBR)
                haveSceneOut = false;
        }

        for (uint32_t s = 0; s < process->frames_count; ++s)
        {
            if (blockPos == 0)
            {
                while (nextevtime >= 0 && nextevtime < s + BLOCK_SIZE && currev < evtsz)
                {
                    processEvent(ev->get(ev, currev));

                    currev++;
                    if (currev < evtsz)
                        nextevtime = ev->get(ev, currev)->time;
                    else
                        nextevtime = -1;
                }

                if (inL && inR)
                {
                    memcpy(&(synth->input[0][0]), inL, BLOCK_SIZE * sizeof(float));
                    memcpy(&(synth->input[1][0]), inR, BLOCK_SIZE * sizeof(float));
                    inL += BLOCK_SIZE;
                    inR += BLOCK_SIZE;
                }

                synth->process();
                synth->time_data.ppqPos +=
                    (double)BLOCK_SIZE * synth->time_data.tempo / (60. * synth->storage.samplerate);

                if (synth->hostNoteEndedDuringBlockCount > 0)
                {
                    auto ov = process->out_events;
                    for (int v = 0; v < synth->hostNoteEndedDuringBlockCount; ++v)
                    {
                        auto evt = clap_event_note();
                        evt.header.size = sizeof(clap_event_note);
                        evt.header.type = (uint16_t)CLAP_EVENT_NOTE_END;
                        evt.header.time = s;
                        evt.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                        evt.header.flags = 0;

                        evt.port_index = 0;
                        evt.channel = synth->endedHostNoteOriginalChannel[v];
                        evt.key = synth->endedHostNoteOriginalKey[v];
                        evt.note_id = synth->endedHostNoteIds[v];
                        evt.velocity = 0.0;

                        ov->try_push(ov, reinterpret_cast<const clap_event_header *>(&evt));
                    }
                }
            }

            *outL = synth->output[0][blockPos];
            *outR = synth->output[1][blockPos];
            outL++;
            outR++;

            if (haveSceneOut)
            {
                *sceneAL = synth->sceneout[0][0][blockPos];
                *sceneAR = synth->sceneout[0][1][blockPos];
                *sceneBL = synth->sceneout[1][0][blockPos];
                *sceneBR = synth->sceneout[1][1][blockPos];

                sceneAL++;
                sceneAR++;
                sceneBL++;
                sceneBR++;
            }

            blockPos = (blockPos + 1) & (BLOCK_SIZE - 1);
        }

        // just in case
        while (currev < evtsz)
        {
            processEvent(ev->get(ev, currev));
            currev++;
        }

        // A patch load (through the state extension or otherwise) re-means
        // every parameter, so have the host pull everything again
        if (std::atomic_exchange(&synth->patchChanged, false))
        {
            rescanRequested = true;
            _host.requestCallback();
        }

        return CLAP_PROCESS_CONTINUE;
    }

    void onMainThread() noexcept override
    {
        if (std::atomic_exchange(&rescanRequested, false) && _host.canUseParams())
        {
            _host.paramsRescan(CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT |
                               CLAP_PARAM_RESCAN_INFO);
        }
    }

  private:
    ParamSlot *slotForId(clap_id id) const
    {
        auto it = slotsById.find(id);
        return it == slotsById.end() ? nullptr : it->second;
    }

    const char *clumpName(const Parameter *p) const
    {
        parametermeta pm;
        synth->getParameterMeta(synth->idForParameter(p), pm);

        switch (pm.clump)
        {
        case 2:
            return "Global & FX";
        case 3:
            return "A Common";
        case 4:
            return "A Oscillators";
        case 5:
            return "A Mixer";
        case 6:
            return "A Filters";
        case 7:
            return "A Envelopes";
        case 8:
            return "A LFOs";
        case 9:
            return "B Common";
        case 10:
            return "B Oscillators";
        case 11:
            return "B Mixer";
        case 12:
            return "B Filters";
        case 13:
            return "B Envelopes";
        case 14:
            return "B LFOs";
        }
        return "";
    }

    void applyTransport(const clap_event_transport *t)
    {
        if (!t)
        {
            synth->time_data.tempo = 120;
            synth->time_data.timeSigNumerator = 4;
            synth->time_data.timeSigDenominator = 4;
            synth->resetStateFromTimeData();
            return;
        }

        if (t->flags & CLAP_TRANSPORT_HAS_TEMPO)
            synth->time_data.tempo = t->tempo;

        if ((t->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) &&
            (t->flags & CLAP_TRANSPORT_IS_PLAYING))
            synth->time_data.ppqPos = t->song_pos_beats / (double)CLAP_BEATTIME_FACTOR;

        if (t->flags & CLAP_TRANSPORT_HAS_TIME_SIGNATURE)
        {
            synth->time_data.timeSigNumerator = t->tsig_num;
            synth->time_data.timeSigDenominator = t->tsig_denom;
        }
        synth->resetStateFromTimeData();
    }

    void notifyPendingParams(const clap_output_events *ov, uint32_t time)
    {
        std::vector<std::pair<clap_id, double>> notify;
        {
            std::lock_guard<std::mutex> g(pendingHostNotificationMutex);
            std::swap(notify, pendingHostNotifications);
        }

        for (const auto &[id, value] : notify)
        {
            auto evt = clap_event_param_value();
            evt.header.size = sizeof(clap_event_param_value);
            evt.header.type = (uint16_t)CLAP_EVENT_PARAM_VALUE;
            evt.header.time = time;
            evt.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            evt.header.flags = 0;

            evt.param_id = id;
            evt.cookie = slotForId(id);
            evt.note_id = -1;
            evt.port_index = -1;
            evt.channel = -1;
            evt.key = -1;
            evt.value = value;

            ov->try_push(ov, reinterpret_cast<const clap_event_header *>(&evt));
        }
    }

    void processEvent(const clap_event_header_t *evt)
    {
        if (evt->space_id != CLAP_CORE_EVENT_SPACE_ID)
            return;

        switch (evt->type)
        {
        case CLAP_EVENT_NOTE_ON:
        {
            auto nevt = reinterpret_cast<const clap_event_note *>(evt);

            if (nevt->velocity != 0)
                synth->playNote(nevt->channel, nevt->key, 127 * nevt->velocity, 0, nevt->note_id);
            else
                synth->releaseNote(nevt->channel, nevt->key, 127 * nevt->velocity, nevt->note_id);
        }
        break;
        case CLAP_EVENT_NOTE_CHOKE:
        {
            auto nevt = reinterpret_cast<const clap_event_note *>(evt);
            synth->chokeNote(nevt->channel, nevt->key, 127 * nevt->velocity, nevt->note_id);
        }
        break;
        case CLAP_EVENT_NOTE_OFF:
        {
            auto nevt = reinterpret_cast<const clap_event_note *>(evt);
            synth->releaseNote(nevt->channel, nevt->key, 127 * nevt->velocity, nevt->note_id);
        }
        break;
        case CLAP_EVENT_MIDI:
        {
            auto mevt = reinterpret_cast<const clap_event_midi *>(evt);
            applyMidi(mevt->data);
        }
        break;
        case CLAP_EVENT_PARAM_VALUE:
        {
            auto pevt = reinterpret_cast<const clap_event_param_value *>(evt);
            auto slot = static_cast<ParamSlot *>(pevt->cookie);
            if (!slot) // unlikely
                slot = slotForId(pevt->param_id);
            if (!slot)
                break;

            if (slot->macro >= 0)
                synth->setMacroParameter01(slot->macro, pevt->value);
            else
                synth->setParameter01(synth->idForParameter(slot->param), pevt->value, true);
        }
        break;
        case CLAP_EVENT_PARAM_MOD:
        {
            auto pevt = reinterpret_cast<const clap_event_param_mod *>(evt);
            auto slot = static_cast<ParamSlot *>(pevt->cookie);
            if (!slot) // unlikely
                slot = slotForId(pevt->param_id);
            if (!slot)
                break;

            if (slot->macro >= 0)
            {
                synth->applyMacroMonophonicModulation(slot->macro, pevt->amount);
                break;
            }

            auto p = slot->param;
            if (!p->can_be_nondestructively_modulated())
                break;

            if ((pevt->note_id == -1 && pevt->channel == -1 && pevt->key == -1) ||
                !p->per_voice_processing)
            {
                synth->applyParameterMonophonicModulation(p, pevt->amount);
            }
            else
            {
                synth->applyParameterPolyphonicModulation(p, pevt->note_id, pevt->key,
                                                          pevt->channel, pevt->amount);
            }
        }
        break;
        case CLAP_EVENT_NOTE_EXPRESSION:
        {
            auto pevt = reinterpret_cast<const clap_event_note_expression *>(evt);
            SurgeVoice::NoteExpressionType net = SurgeVoice::UNKNOWN;
            switch (pevt->expression_id)
            {
            // with 0 < x <= 4, plain = 20 * log(x)
            case CLAP_NOTE_EXPRESSION_VOLUME:
                net = SurgeVoice::VOLUME;
                break;
            // pan, 0 left, 0.5 center, 1 right
            case CLAP_NOTE_EXPRESSION_PAN:
                net = SurgeVoice::PAN;
                break;
            // relative tuning in semitone, from -120 to +120
            case CLAP_NOTE_EXPRESSION_TUNING:
                net = SurgeVoice::PITCH;
                break;
            // 0..1
            case CLAP_NOTE_EXPRESSION_BRIGHTNESS:
                net = SurgeVoice::TIMBRE;
                break;
            case CLAP_NOTE_EXPRESSION_PRESSURE:
                net = SurgeVoice::PRESSURE;
                break;
            case CLAP_NOTE_EXPRESSION_VIBRATO:
            case CLAP_NOTE_EXPRESSION_EXPRESSION:
                break;
            }
            if (net != SurgeVoice::UNKNOWN)
                synth->setNoteExpression(net, pevt->note_id, pevt->key, pevt->channel, pevt->value);
        }
        break;
        case CLAP_EVENT_TRANSPORT:
        {
            applyTransport(reinterpret_cast<const clap_event_transport *>(evt));
        }
        break;
        default:
            break;
        }
    }

    void applyMidi(const uint8_t data[3])
    {
        const int ch = data[0] & 0x0F;

        switch (data[0] & 0xF0)
        {
        case 0x90: // note on
            if (data[2] != 0)
                synth->playNote(ch, data[1], data[2], 0, -1);
            else
                synth->releaseNote(ch, data[1], data[2], -1);
            break;
        case 0x80: // note off
            synth->releaseNote(ch, data[1], data[2]);
            break;
        case 0xA0: // poly aftertouch
            synth->polyAftertouch(ch, data[1], data[2]);
            break;
        case 0xB0: // control change
            synth->channelController(ch, data[1], data[2]);
            break;
        case 0xC0: // program change
            synth->programChange(ch, data[1]);
            break;
        case 0xD0: // channel aftertouch
            synth->channelAftertouch(ch, data[1]);
            break;
        case 0xE0: // pitch bend
            synth->pitchBend(ch, ((data[2] << 7) | data[1]) - 8192);
            break;
        default:
            break;
        }
    }
};

/*
 * clap_plugin_factory
 */
static uint32_t factory_get_plugin_count(const clap_plugin_factory *) { return 1; }

static const clap_plugin_descriptor *factory_get_plugin_descriptor(const clap_plugin_factory *,
                                                                   uint32_t index)
{
    return index == 0 ? &descriptor : nullptr;
}

static const clap_plugin *factory_create_plugin(const clap_plugin_factory *, const clap_host *host,
                                                const char *plugin_id)
{
    if (!plugin_id || strcmp(plugin_id, descriptor.id) != 0)
        return nullptr;

    try
    {
        auto p = new SurgeWasmPlugin(host);
        return p->clapPlugin();
    }
    catch (const std::exception &e)
    {
        fprintf(stderr, "Surge XT: engine startup failed: %s\n", e.what());
        return nullptr;
    }
}

static const clap_plugin_factory plugin_factory = {
    factory_get_plugin_count,
    factory_get_plugin_descriptor,
    factory_create_plugin,
};

/*
 * clap_plugin_entry
 */
static bool entry_init(const char *) { return true; }
static void entry_deinit() {}
static const void *entry_get_factory(const char *factory_id)
{
    if (factory_id && strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID) == 0)
        return &plugin_factory;
    return nullptr;
}

} // namespace surge::wasm_clap

extern "C" CLAP_EXPORT const clap_plugin_entry clap_entry = {
    CLAP_VERSION_INIT,
    surge::wasm_clap::entry_init,
    surge::wasm_clap::entry_deinit,
    surge::wasm_clap::entry_get_factory,
};
