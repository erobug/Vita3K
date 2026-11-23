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

#include <ngs/modules/pauser.h>
#include <util/log.h>

namespace ngs {

// [ngs-audio] Pauser module: pass-through.
//
// The pauser sits between the generators and the rest of the chain so a voice can be muted
// without dismantling its routing. Its guest-facing state block (SceNgsPauserStates) is not
// publicly documented, and inventing a layout risks corrupting guest memory.
//
// A pauser is transparent unless the title actively pauses the voice, so forwarding the block
// is both the safe and the useful behaviour. Voice-level pause is already handled by the
// scheduler and the VoiceState machinery.
bool PauserModule::process(KernelState &kern, const MemState &mem, const SceUID thread_id, ModuleData &data, std::unique_lock<std::recursive_mutex> &scheduler_lock, std::unique_lock<std::mutex> &voice_lock) {
    // Audio already lives in products[0] for this voice chain; nothing to do.
    return false;
}
} // namespace ngs
