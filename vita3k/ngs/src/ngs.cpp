// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <cpu/state.h>
#include <kernel/state.h>

#include <ngs/modules/atrac9.h>
#include <ngs/state.h>
#include <ngs/system.h>
#include <util/lock_and_find.h>

#include <util/log.h>
#include <util/vector_utils.h>

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace ngs {
Rack::Rack(System *mama, const Ptr<void> memspace, const uint32_t memspace_size)
    : MempoolObject(memspace, memspace_size)
    , system(mama) {}

// [ngs-fix] Host-side state that cannot live inside guest memory.
// Rack/System/Voice are all placement-new'ed into the guest memspace, so any host container or
// mutex stored in them can be overwritten by the guest; their destructors then corrupt the host
// heap (fastfail) or the next access dereferences garbage. Everything below is keyed by the
// guest address of the owning object.
namespace {
std::mutex host_tables_mutex;

/**
 * @brief Key for the host-side side tables.
 *
 * The full host pointer, not its low 32 bits. Two live host objects can share a low word -- the
 * allocator only has to hand out addresses that differ somewhere in the 64-bit space -- and keying
 * on the truncated value would make them share (and corrupt) one entry. The pointer is unique for
 * as long as the object exists, which is exactly the lifetime the table entry needs.
 */
using HostKey = uintptr_t;

std::unordered_map<HostKey, Rack::PatchTable> patch_tables;
std::unordered_map<HostKey, std::shared_ptr<std::mutex>> voice_mutexes;
std::unordered_map<HostKey, std::vector<ModuleData>> voice_datas;

struct SystemHostState {
    std::vector<Rack *> racks;
    VoiceScheduler scheduler;
};
std::unordered_map<HostKey, std::unique_ptr<SystemHostState>> system_states;

HostKey key_of(const void *host_ptr) {
    return reinterpret_cast<HostKey>(host_ptr);
}
} // namespace

Rack::PatchTable &Rack::patch_table_for(const Ptr<Voice> voice, const MemState &mem) {
    const std::lock_guard<std::mutex> guard(host_tables_mutex);
    return patch_tables[key_of(voice.get(mem))];
}

Rack::PatchTable *Rack::find_patch_table(const Ptr<Voice> voice, const MemState &mem) {
    const std::lock_guard<std::mutex> guard(host_tables_mutex);
    auto it = patch_tables.find(key_of(voice.get(mem)));
    return it == patch_tables.end() ? nullptr : &it->second;
}

// [ngs-fix] Release every host-side entry belonging to one voice.
//
// The side tables are keyed by the host address of the Voice (see key_of), so a voice that goes
// away leaves its module data, mutex and routing table behind unless something removes them.
// Two things go wrong when they linger: the host memory is never reclaimed, and a later voice
// placed at the same address inherits the previous voice's routing table, mixing its audio into
// the old destinations.
//
// Taking the host pointer, not the guest Ptr, is deliberate: the accessors below key on
// key_of(this) over the host pointer, and Ptr::address() is host pointer minus the memspace base.
// The two are only equal when the memspace happens to be mapped with a zero low word, so using
// the Ptr here would clear different slots than the ones that were filled. Both teardown paths
// (release_rack and release_system) come through here.
void Voice::release_host_state(const Voice *self) {
    if (!self)
        return;
    const auto k = key_of(self);
    const std::lock_guard<std::mutex> guard(host_tables_mutex);
    voice_datas.erase(k);
    voice_mutexes.erase(k);
    patch_tables.erase(k);
}

void System::release_host_state() {
    const auto k = key_of(this);
    const std::lock_guard<std::mutex> guard(host_tables_mutex);
    system_states.erase(k);
}

std::vector<Rack *> &System::racks() {
    const auto k = key_of(this);
    const std::lock_guard<std::mutex> guard(host_tables_mutex);
    auto &slot = system_states[k];
    if (!slot)
        slot = std::make_unique<SystemHostState>();
    return slot->racks;
}

VoiceScheduler &System::voice_scheduler() {
    const auto k = key_of(this);
    const std::lock_guard<std::mutex> guard(host_tables_mutex);
    auto &slot = system_states[k];
    if (!slot)
        slot = std::make_unique<SystemHostState>();
    return slot->scheduler;
}

std::mutex &Voice::mutex() {
    const auto k = key_of(this);
    std::shared_ptr<std::mutex> mtx;
    {
        const std::lock_guard<std::mutex> guard(host_tables_mutex);
        auto &slot = voice_mutexes[k];
        if (!slot)
            slot = std::make_shared<std::mutex>();
        mtx = slot;
    }
    return *mtx;
}

std::mutex *Voice::find_mutex() {
    const auto k = key_of(this);
    const std::lock_guard<std::mutex> guard(host_tables_mutex);
    auto it = voice_mutexes.find(k);
    return it == voice_mutexes.end() ? nullptr : it->second.get();
}

Voice::ModuleDatas &Voice::datas() {
    const auto k = key_of(this);
    const std::lock_guard<std::mutex> guard(host_tables_mutex);
    return voice_datas[k];
}

const Voice::ModuleDatas &Voice::datas() const {
    const auto k = key_of(this);
    const std::lock_guard<std::mutex> guard(host_tables_mutex);
    return voice_datas[k];
}

System::System(const Ptr<void> memspace, const uint32_t memspace_size)
    : MempoolObject(memspace, memspace_size)
    , max_voices(0)
    , granularity(0)
    , sample_rate(0) {}

bool Patch::is_active() const {
    return output_sub_index != -1;
}

void VoiceInputManager::init(const uint32_t granularity, const uint16_t total_input) {
    inputs.resize(total_input);

    for (auto &input : inputs) {
        // FLTP and maximum channel count
        input.resize(granularity * 8);
    }

    reset_inputs();
}

void VoiceInputManager::reset_inputs() {
    for (auto &input : inputs) {
        std::ranges::fill(input, 0);
    }
}

VoiceInputManager::PCMInput *VoiceInputManager::get_input_buffer_queue(const int32_t index) {
    if (index >= inputs.size()) {
        return nullptr;
    }

    return &inputs[index];
}

int32_t VoiceInputManager::receive(const MemState &mem, ngs::Patch *patch, const VoiceProduct &product) {
    Voice *source = patch->source.get(mem);
    Voice *dest = patch->dest.get(mem);
    if (!source || !dest) {
        return -1;
    }

    PCMInput *input = get_input_buffer_queue(patch->dest_index);

    if (!input) {
        return -1;
    }

    float *dest_buffer = reinterpret_cast<float *>(input->data());
    const float *data_to_mix_in = reinterpret_cast<const float *>(product.data);

    float volume_matrix[2][2];
    memcpy(volume_matrix, patch->volume_matrix, sizeof(volume_matrix));

    // we always use stereo internally, so make sure not to add too many channels
    if (source->rack->channels_per_voice == 1) {
        volume_matrix[1][0] = 0.0f;
        volume_matrix[1][1] = 0.0f;
    }

    if (dest->rack->channels_per_voice == 1) {
        volume_matrix[0][1] = 0.0f;
        volume_matrix[1][1] = 0.0f;
    }

    // Try mixing, also with the use of this volume matrix
    // Dest is our voice to receive this data.
    for (int32_t k = 0; k < dest->rack->system->granularity; k++) {
        dest_buffer[k * 2] = std::clamp(dest_buffer[k * 2] + data_to_mix_in[k * 2] * volume_matrix[0][0]
                + data_to_mix_in[k * 2 + 1] * volume_matrix[1][0],
            -1.0f, 1.0f);
        dest_buffer[k * 2 + 1] = std::clamp(dest_buffer[k * 2 + 1] + data_to_mix_in[k * 2] * volume_matrix[0][1] + data_to_mix_in[k * 2 + 1] * volume_matrix[1][1], -1.0f, 1.0f);
    }

    return 0;
}

ModuleData::ModuleData()
    : callback(0)
    , user_data(0)
    , is_bypassed(false)
    , flags(0) {
}

SceNgsBufferInfo *ModuleData::lock_params(const MemState &mem) {
    const std::lock_guard<std::mutex> guard(parent->mutex());

    // Save a copy of previous set of data
    if (flags & PARAMS_LOCK) {
        return nullptr;
    }

    const uint8_t *current_data = info.data.cast<const uint8_t>().get(mem);
    last_info.resize(info.size);
    memcpy(last_info.data(), current_data, info.size);

    flags |= PARAMS_LOCK;

    return &info;
}

bool ModuleData::unlock_params(const MemState &mem) {
    const std::lock_guard<std::mutex> guard(parent->mutex());

    parent->rack->modules[index]->on_param_change(mem, *this);

    if (flags & PARAMS_LOCK) {
        flags &= ~PARAMS_LOCK;
        return true;
    }

    return false;
}

void ModuleData::invoke_callback(KernelState &kernel, const MemState &mem, const SceUID thread_id, const uint32_t reason1,
    const uint32_t reason2, Address reason_ptr) {
    return parent->invoke_callback(kernel, mem, thread_id, callback, user_data, parent->rack->modules[index]->module_id(),
        reason1, reason2, reason_ptr);
}

void Voice::init(Rack *mama) {
    rack = mama;
    state = VoiceState::VOICE_STATE_AVAILABLE;
    is_pending = false;
    is_paused = false;
    is_keyed_off = false;

    datas().resize(mama->modules.size());

    // [ngs-fix] the routing table is registered host-side by init_rack, not stored here

    inputs.init(rack->system->granularity, 1);
    mutex(); // pre-create the host-side mutex
}

Ptr<Patch> Voice::patch(const MemState &mem, const int32_t index, int32_t subindex, int32_t dest_index, Ptr<Voice> source, Ptr<Voice> dest) {
    if (!rack) {
        return {};
    }
    const std::lock_guard<std::mutex> guard(mutex());

    if (index >= MAX_OUTPUT_PORT) {
        // We don't have enough port for you!
        return {};
    }

    // [ngs-fix] routing table lives host-side, keyed by this voice's guest address
    auto &ports = rack->patch_table_for(Ptr<Voice>(this, mem), mem);

    // Look if another patch has already been there
    if (subindex == -1) {
        for (int32_t i = 0; i < ports[index].size(); i++) {
            if (!ports[index][i] || (ports[index])[i].get(mem)->output_sub_index == -1) {
                subindex = i;
                break;
            }
        }
    }

    if (subindex >= ports[index].size()) {
        return {};
    }

    if (ports[index][subindex] && ports[index][subindex].get(mem)->output_sub_index != -1) {
        // You just hit an occupied subindex! You won't get to eat, stay in detention.
        return {};
    }

    if (!ports[index][subindex]) {
        // Create the patch incase it's not yet existed
        ports[index][subindex] = rack->alloc_and_init<Patch>(mem);
        if (!ports[index][subindex]) {
            // [ngs-fix] pool could not satisfy the allocation; bail instead of dereferencing null
            return {};
        }
    }

    Patch *patch = ports[index][subindex].get(mem);
    if (!patch) {
        return {};
    }

    patch->output_sub_index = subindex;
    patch->output_index = index;
    patch->dest_index = dest_index;
    patch->dest = dest;
    patch->source = source;

    // Initialize the matrix
    memset(patch->volume_matrix, 0, sizeof(patch->volume_matrix));

    return ports[index][subindex];
}

bool Voice::remove_patch(const MemState &mem, const Ptr<Patch> patch) {
    if (!patch || !rack) {
        return false;
    }
    const std::lock_guard<std::mutex> guard(mutex());
    auto *table = rack->find_patch_table(Ptr<Voice>(this, mem), mem);
    if (!table) {
        return false;
    }
    bool found = false;
    for (auto &patches_1 : *table) {
        if (std::ranges::contains(patches_1, patch)) {
            found = true;
            break;
        }
    }
    if (!found)
        return false;

    // Try to unroute. Free the destination index
    patch.get(mem)->output_sub_index = -1;

    return true;
}

ModuleData *Voice::module_storage(const uint32_t index) {
    if (index >= datas().size()) {
        return nullptr;
    }

    return &datas()[index];
}

void Voice::transition(const MemState &mem, const VoiceState new_state) {
    const VoiceState old = state;
    state = new_state;

    for (size_t i = 0; i < datas().size(); i++) {
        rack->modules[i]->on_state_change(mem, datas()[i], old);
    }
}

bool Voice::parse_params(const MemState &mem, const SceNgsModuleParamHeader *header) {
    ModuleData *storage = module_storage(header->module_id);

    if (!storage)
        return false;

    if (storage->flags & ModuleData::PARAMS_LOCK)
        return false;

    const auto *descr = reinterpret_cast<const SceNgsParamsDescriptor *>(header + 1);
    if (descr->size > storage->info.size)
        return false;

    memcpy(storage->info.data.get(mem), descr, descr->size);

    return true;
}

SceInt32 Voice::parse_params_block(const MemState &mem, const SceNgsModuleParamHeader *header, const SceUInt32 size) {
    const SceUInt8 *data = reinterpret_cast<const SceUInt8 *>(header);
    const SceUInt8 *data_end = data + size;

    SceInt32 num_error = 0;

    // after first loop, check if other module exist
    while (data < data_end) {
        if (!parse_params(mem, header))
            num_error++;

        // increment by the size of the header alone + the descriptor size
        data += sizeof(SceNgsModuleParamHeader) + reinterpret_cast<const SceNgsParamsDescriptor *>(header + 1)->size;

        // set new header for next module
        header = reinterpret_cast<const SceNgsModuleParamHeader *>(data);
    }

    return num_error;
}

bool Voice::set_preset(const MemState &mem, const SceNgsVoicePreset *preset) {
    // we ignore the name for now
    const uint8_t *data_origin = reinterpret_cast<const uint8_t *>(preset);

    if (preset->preset_data_offset) {
        const auto *preset_data = reinterpret_cast<const SceNgsModuleParamHeader *>(data_origin + preset->preset_data_offset);
        auto nb_errors = parse_params_block(mem, preset_data, preset->preset_data_size);
        if (nb_errors > 0)
            return false;
    }

    if (preset->bypass_flags_offset) {
        const auto *bypass_flags = reinterpret_cast<const SceUInt32 *>(data_origin + preset->bypass_flags_offset);
        // should we disable bypass on all modules first?
        for (SceUInt32 i = 0; i < preset->bypass_flags_nb; i++) {
            ModuleData *module_data = module_storage(*bypass_flags);
            if (!module_data)
                return false;
            module_data->is_bypassed = true;

            bypass_flags++;
        }
    }

    return true;
}

void Voice::invoke_callback(KernelState &kernel, const MemState &mem, const SceUID thread_id, Ptr<void> callback, Ptr<void> user_data,
    const uint32_t module_id, const uint32_t reason1, const uint32_t reason2, Address reason_ptr) {
    if (!callback) {
        return;
    }

    const ThreadStateRef thread = kernel.get_thread(thread_id);
    const Address callback_info_addr = thread->cpu->stack_alloc(sizeof(SceNgsCallbackInfo));

    SceNgsCallbackInfo *info = Ptr<SceNgsCallbackInfo>(callback_info_addr).get(mem);
    info->rack_handle = Ptr<void>(rack, mem);
    info->voice_handle = Ptr<void>(this, mem);
    info->module_id = module_id;
    info->callback_reason = reason1;
    info->callback_reason_2 = reason2;
    info->callback_ptr = Ptr<void>(reason_ptr);
    info->userdata = user_data;

    thread->call_guest(callback.address(), { callback_info_addr });
    thread->cpu->stack_free(sizeof(SceNgsCallbackInfo));
}

uint32_t System::get_required_memspace_size(SceNgsSystemInitParams *parameters) {
    return sizeof(System);
}

uint32_t Rack::get_required_memspace_size(MemState &mem, SceNgsRackDescription *description) {
    // [ngs-fix] mirror exactly what init_rack allocates in the guest memspace: one Rack header,
    // one Voice per voice_count, each module's parameter buffer (single copy -- the VoiceLock
    // duplicate is gone), and one Patch per (output port, sub-index). The old formula used the
    // maximum module parameter total and multiplied it by two, which disagreed with the real
    // per-voice cost and left small racks without room for their parameter buffer or patches
    // (observed: 280 B budget vs rack 128 + voice 152, then alloc(76) -> pool exhausted).
    uint32_t per_voice_data = 0;
    if (description->definition) {
        std::vector<std::unique_ptr<Module>> mods;
        apply_voice_definition(description->definition.get(mem), mods);
        for (const auto &mod : mods) {
            if (mod)
                per_voice_data += mod->get_buffer_parameter_size();
        }
    }

    // [ngs-layout] Budget patches by the definition's output_count, not by MAX_OUTPUT_PORT.
    // A game lays its racks out back to back using exactly the size we report here, and it
    // places the System block independently -- if a rack is over-reported it grows past the
    // System block and both end up overlapping (observed: rack #2 reported 23,664 B while only
    // 22,064 B were available before System at 0x83865700, so System landed inside rack #2).
    // Voice::patch only ever indexes ports below the definition's output count, so budgeting
    // MAX_OUTPUT_PORT ports wasted (MAX_OUTPUT_PORT - output_count) slots per voice.
    const uint32_t output_count = description->definition ? description->definition.get(mem)->output_count : 0;
    const uint32_t patch_bytes = description->patches_per_output
        * output_count * static_cast<uint32_t>(sizeof(ngs::Patch));

    return sizeof(ngs::Rack) + description->voice_count * (sizeof(ngs::Voice) + per_voice_data + patch_bytes);
}

bool init(State &ngs, MemState &mem) {
    voice_definition_init(ngs, mem);

    return true;
}

void deinit(State &ngs, MemState &mem) {
    while (!ngs.systems.empty()) {
        release_system(ngs, mem, ngs.systems.back());
    }

    Atrac9Module::free_swr_contexts();

    ngs.definitions = Ptr<VoiceDefinition>(0);
}

bool init_system(State &ngs, const MemState &mem, SceNgsSystemInitParams *parameters, Ptr<void> memspace, const uint32_t memspace_size) {
    // Reserve first memory allocation for our System struct
    System *sys = memspace.cast<System>().get(mem);
    sys = new (sys) System(memspace, memspace_size);


    sys->racks().resize(parameters->max_racks);

    sys->max_voices = parameters->max_voices;
    sys->granularity = parameters->granularity;
    sys->sample_rate = parameters->sample_rate;

    // Alloc first block for System struct
    if (!sys->alloc_raw(sizeof(System))) {
        return false;
    }

    ngs.systems.push_back(sys);
    return true;
}

void release_system(State &ngs, const MemState &mem, System *system) {
    // this function assumes no ngs mutex is being held
    for (Rack *rack : system->racks()) {
        if (!rack)
            continue;
        for (const auto &voice : rack->voices) {
            system->voice_scheduler().deque_voice(voice.get(mem));
            // Same teardown as release_rack: let each module release its own resources before
            // the host tables go away. Player and ATRAC9 both destroy resamplers here, so
            // dropping ModuleData without this would leak them.
            Voice *v = voice.get(mem);
            for (size_t i = 0; i < rack->modules.size() && i < v->datas().size(); i++) {
                if (rack->modules[i])
                    rack->modules[i]->cleanup_voice_state(v->datas()[i]);
            }
            Voice::release_host_state(v);
            v->~Voice();
        }
        rack->~Rack();
    }

    system->racks().clear();

    vector_utils::erase_first(ngs.systems, system);
    // Drop this system's host-side scheduler and rack list, keyed by its guest address.
    system->release_host_state();
    system->~System();
}

bool init_rack(State &ngs, const MemState &mem, System *system, SceNgsBufferInfo *init_info, const SceNgsRackDescription *description) {
    Rack *rack = init_info->data.cast<Rack>().get(mem);
    rack = new (rack) Rack(system, init_info->data, init_info->size);

    // Alloc first block for Rack
    if (!rack->alloc<Rack>()) {
        return false;
    }

    if (description->definition)
        apply_voice_definition(description->definition.get(mem), rack->modules);
    else
        rack->modules.clear();

    // Initialize voice definition
    rack->channels_per_voice = description->channels_per_voice;
    rack->max_patches_per_input = description->max_patches_per_input;
    rack->patches_per_output = description->patches_per_output;

    // Alloc spaces for voice
    rack->voices.resize(description->voice_count);
    rack->vdef = description->definition.get(mem);

    for (auto &voice : rack->voices) {
        voice = rack->alloc<Voice>();

        if (!voice) {
            return false;
        }

        Voice *v = voice.get(mem);
        new (v) Voice();
        v->init(rack);

        // [ngs-fix] register this voice's routing table host-side (it used to be a Voice member,
        // and Voice lives in guest memory whose size games are billed for). Only the ports the
        // definition actually declares are provisioned, matching get_required_memspace_size.
        auto &table = rack->patch_table_for(voice, mem);
        // A recycled guest address can still hold the previous voice's routing here, and resize()
        // keeps existing elements when the size is unchanged -- stale Patch handles would then
        // survive and this voice would mix into the old destinations. Clear before provisioning.
        for (auto &patches : table)
            patches.clear();
        const uint32_t port_count = std::min<uint32_t>(MAX_OUTPUT_PORT, rack->vdef ? rack->vdef->output_count : 0);
        for (uint32_t port = 0; port < port_count; port++)
            table[port].resize(rack->patches_per_output);

        // Allocate parameter buffer info for each voice
        for (size_t i = 0; i < rack->modules.size(); i++) {
            v->datas()[i].info.size = rack->modules[i]->get_buffer_parameter_size();
            v->datas()[i].info.data = rack->alloc_raw(v->datas()[i].info.size);
            // [ngs-fix] the duplicate VoiceLock copy is no longer allocated in guest memory:
            // it doubled per-voice cost and overflowed the game's audio carve-out.

            v->datas()[i].parent = v;
            v->datas()[i].index = static_cast<uint32_t>(i);
            rack->modules[i]->initialize_voice_data(v->datas()[i]);
        }
    }

    system->racks().push_back(rack);

    return true;
}

void release_rack(State &ngs, const MemState &mem, System *system, Rack *rack) {
    // this function should only be called outside of ngs update and with the scheduler mutex acquired (except when releasing the system)
    if (!rack)
        return;

    // remove all queued voices
    for (const auto &voice : rack->voices) {
        Voice *v = voice.get(mem);
        system->voice_scheduler().deque_voice(voice.get(mem));
        // clean up host resources per voice before destroying
        for (size_t i = 0; i < rack->modules.size() && i < v->datas().size(); i++) {
            if (rack->modules[i])
                rack->modules[i]->cleanup_voice_state(v->datas()[i]);
        }
        // Drop the host-side tables keyed by this voice's guest address. Doing it here rather
        // than leaving it to the guest object's destructor is deliberate: the side tables are
        // host memory and the Voice itself lives in guest memory.
        Voice::release_host_state(v);
        // no need to free the voice from the rack
        v->~Voice();
    }

    // remove from system
    vector_utils::erase_first(system->racks(), rack);

    // free pointer memory
    rack->~Rack();
}

Ptr<VoiceDefinition> get_voice_definition(State &ngs, MemState &mem, ngs::BussType type) {
    return ngs.definitions + static_cast<int>(type);
}
} // namespace ngs
