/*
 * Copyright (c) 2019-2025 Valve Corporation
 * Copyright (c) 2019-2025 LunarG, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "state_tracker/buffer_state.h"
#include "state_tracker/cmd_buffer_state.h"
#include "sync/sync_common.h"

extern const ResourceAccessRange kFullRange(std::numeric_limits<VkDeviceSize>::min(), std::numeric_limits<VkDeviceSize>::max());

template <typename Flags, typename Map>
SyncAccessFlags AccessScopeImpl(Flags flag_mask, const Map& map) {
    SyncAccessFlags scope;
    for (const auto& bit_scope : map) {
        if (flag_mask < bit_scope.first) break;

        if (flag_mask & bit_scope.first) {
            scope |= bit_scope.second;
        }
    }
    return scope;
}

SyncAccessFlags SyncStageAccess::AccessScopeByStage(VkPipelineStageFlags2 stages) {
    return AccessScopeImpl(stages, syncAccessMaskByStageBit());
}

SyncAccessFlags SyncStageAccess::AccessScopeByAccess(VkAccessFlags2 accesses) {
    SyncAccessFlags sync_accesses = AccessScopeImpl(sync_utils::ExpandAccessFlags(accesses), syncAccessMaskByAccessBit());

    // The above access expansion replaces SHADER_READ meta access with atomic accesses as defined by the specification.
    // ACCELERATION_STRUCTURE_BUILD and MICROMAP_BUILD stages are special in a way that they use SHADER_READ access directly.
    // It is an implementation detail of how SHADER_READ is used by the driver, and we cannot make assumption about specific
    // atomic accesses. If we make such assumption then it can be a problem when after applying synchronization we won't be
    // able to get full SHADER_READ access back, but only a subset of accesses, for example, only SHADER_STORAGE_READ.
    // It would mean we made (incorrect) assumption how the driver represents SHADER_READ in the context of AS build.
    //
    // Handle special cases that use non-expanded meta accesses.
    if (accesses & VK_ACCESS_2_SHADER_READ_BIT) {
        sync_accesses |= SYNC_ACCELERATION_STRUCTURE_BUILD_SHADER_READ_BIT;
        sync_accesses |= SYNC_MICROMAP_BUILD_EXT_SHADER_READ_BIT;
    }

    return sync_accesses;
}

// Getting from stage mask and access mask to stage/access masks is something we need to be good at...
SyncAccessFlags SyncStageAccess::AccessScope(VkPipelineStageFlags2 stages, VkAccessFlags2 accesses) {
    // The access scope is the intersection of all stage/access types possible for the enabled stages and the enables
    // accesses (after doing a couple factoring of common terms the union of stage/access intersections is the intersections
    // of the union of all stage/access types for all the stages and the same unions for the access mask...
    return AccessScopeByStage(stages) & AccessScopeByAccess(accesses);
}
ResourceAccessRange MakeRange(VkDeviceSize start, VkDeviceSize size) { return ResourceAccessRange(start, (start + size)); }

ResourceAccessRange MakeRange(const vvl::Buffer& buffer, VkDeviceSize offset, VkDeviceSize size) {
    return MakeRange(offset, buffer.GetRegionSize(offset, size));
}

ResourceAccessRange MakeRange(const vvl::BufferView& buf_view_state) {
    return MakeRange(*buf_view_state.buffer_state.get(), buf_view_state.create_info.offset, buf_view_state.create_info.range);
}

ResourceAccessRange MakeRange(VkDeviceSize offset, uint32_t first_index, uint32_t count, uint32_t stride) {
    const VkDeviceSize range_start = offset + (first_index * stride);
    const VkDeviceSize range_size = count * stride;
    return MakeRange(range_start, range_size);
}
