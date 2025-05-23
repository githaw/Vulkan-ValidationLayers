/* Copyright (c) 2015-2025 The Khronos Group Inc.
 * Copyright (c) 2015-2025 Valve Corporation
 * Copyright (c) 2015-2025 LunarG, Inc.
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
#include "logging.h"

#include <csignal>
#include <cstring>
#ifdef VK_USE_PLATFORM_WIN32_KHR
#include <debugapi.h>
#endif
#ifdef VK_USE_PLATFORM_ANDROID_KHR
#include "vk_layer_config.h"
#endif

#include <vulkan/vk_enum_string_helper.h>
#include <vulkan/utility/vk_safe_struct.hpp>
#include "generated/vk_object_types.h"
#include "generated/vk_validation_error_messages.h"
#include "error_location.h"
#include "utils/hash_util.h"
#include "utils/text_utils.h"
#include "error_message/log_message_type.h"

[[maybe_unused]] const char *kVUIDUndefined = "VUID_Undefined";

static inline void DebugReportFlagsToAnnotFlags(VkDebugReportFlagsEXT dr_flags, VkDebugUtilsMessageSeverityFlagsEXT *da_severity,
                                                VkDebugUtilsMessageTypeFlagsEXT *da_type) {
    *da_severity = 0;
    *da_type = 0;
    // If it's explicitly listed as a performance warning, treat it as a performance message. Otherwise, treat it as a validation
    // issue.
    if ((dr_flags & kPerformanceWarningBit) != 0) {
        *da_type |= VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        *da_severity |= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    }
    if ((dr_flags & kVerboseBit) != 0) {
        *da_type |= VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        *da_severity |= VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT;
    }
    if ((dr_flags & kInformationBit) != 0) {
        *da_type |= VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        *da_severity |= VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;
    }
    if ((dr_flags & kWarningBit) != 0) {
        *da_type |= VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        *da_severity |= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    }
    if ((dr_flags & kErrorBit) != 0) {
        *da_type |= VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        *da_severity |= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    }
}

void DebugReport::SetDebugUtilsSeverityFlags(std::vector<VkLayerDbgFunctionState> &callbacks) {
    // For all callback in list, return their complete set of severities and modes
    for (const auto &item : callbacks) {
        if (item.IsUtils()) {
            active_msg_severities |= item.debug_utils_msg_flags;
            active_msg_types |= item.debug_utils_msg_type;
        } else {
            VkFlags severities = 0;
            VkFlags types = 0;
            DebugReportFlagsToAnnotFlags(item.debug_report_msg_flags, &severities, &types);
            active_msg_severities |= severities;
            active_msg_types |= types;
        }
    }
}

void DebugReport::RemoveDebugUtilsCallback(uint64_t callback) {
    std::vector<VkLayerDbgFunctionState> &callbacks = debug_callback_list;
    auto item = callbacks.begin();
    for (item = callbacks.begin(); item != callbacks.end(); item++) {
        if (item->IsUtils()) {
            if (item->debug_utils_callback_object == CastToHandle<VkDebugUtilsMessengerEXT>(callback)) break;
        } else {
            if (item->debug_report_callback_object == CastToHandle<VkDebugReportCallbackEXT>(callback)) break;
        }
    }
    if (item != callbacks.end()) {
        callbacks.erase(item);
    }
    SetDebugUtilsSeverityFlags(callbacks);
}

// We try to return as early as we can if we know we don't need to spend time logging the message
bool DebugReport::LogMessage(VkFlags msg_flags, std::string_view vuid_text, const LogObjectList &objects, const Location &loc,
                             const std::string &main_message) {
    // Convert the info to the VK_EXT_debug_utils format
    VkDebugUtilsMessageSeverityFlagsEXT msg_severity;
    VkDebugUtilsMessageTypeFlagsEXT msg_type;
    DebugReportFlagsToAnnotFlags(msg_flags, &msg_severity, &msg_type);
    if (!(active_msg_severities & msg_severity) || !(active_msg_types & msg_type)) {
        return false;
    }

    // If message is in filter list, bail out very early
    const uint32_t vuid_hash = hash_util::VuidHash(vuid_text);
    if (filter_message_ids.find(vuid_hash) != filter_message_ids.end()) {
        return false;
    }

    // We have a few speical VUID we never actually want to suppress.
    // If a new VUID is added here, make sure to add it in VkLayerTest.VuidHashStability test as well.
    const bool skip_checking_limit =
        // We want to print DebugPrintf message forever, otherwise user will mistake duplicate limit for things not printing
        (vuid_hash == 0x4fe1fef9) ||
        // GPU-AV gives lots of warnings on setup to inform user which settings we are adjusting under them
        (vuid_hash == 0x24b5c69f);

    // Count for this particular message is over the limit, ignore it
    bool at_message_limit = false;
    if (duplicate_message_limit > 0 && !skip_checking_limit) {
        auto vuid_count_it = duplicate_message_count_map.find(vuid_hash);
        if (vuid_count_it == duplicate_message_count_map.end()) {
            duplicate_message_count_map.emplace(vuid_hash, 1);
        } else if (vuid_count_it->second >= duplicate_message_limit) {
            return false;
        } else {
            // In theory we have not locked the mutex yet
            // 1. Its hard to have 2 exact VUID be called at same time
            // 2. This limit is rarely, if ever, used to get an exact amount and really just there to stop spamming the user
            vuid_count_it->second++;
            if (vuid_count_it->second >= duplicate_message_limit) {
                at_message_limit = true;
            }
        }
    }

    std::unique_lock<std::mutex> lock(debug_output_mutex);

    std::vector<VkDebugUtilsLabelEXT> queue_labels;
    std::vector<VkDebugUtilsLabelEXT> cmd_buf_labels;

    std::vector<std::string> object_labels;
    // Ensures that push_back will not reallocate, thereby providing pointer
    // stability for pushed strings.
    object_labels.reserve(objects.object_list.size());

    std::vector<VkDebugUtilsObjectNameInfoEXT> object_name_infos;
    object_name_infos.reserve(objects.object_list.size());
    for (uint32_t i = 0; i < objects.object_list.size(); i++) {
        const VulkanTypedHandle &current_object = objects.object_list[i];
        // If only one VkDevice was created, it is just noise to print it out in the error message.
        // Also avoid printing unknown objects, likely if new function is calling error with null LogObjectList
        if ((current_object.type == kVulkanObjectTypeDevice && device_created <= 1) ||
            (current_object.type == kVulkanObjectTypeUnknown) || (current_object.handle == 0)) {
            continue;
        }

        VkDebugUtilsObjectNameInfoEXT object_name_info = vku::InitStructHelper();
        object_name_info.objectType = ConvertVulkanObjectToCoreObject(current_object.type);
        object_name_info.objectHandle = current_object.handle;
        object_name_info.pObjectName = nullptr;

        std::string object_label = {};
        // Look for any debug utils or marker names to use for this object
        // NOTE: the lock (debug_output_mutex) is held by the caller (LogMsg)
        object_label = GetUtilsObjectNameNoLock(current_object.handle);
        if (object_label.empty()) {
            object_label = GetMarkerObjectNameNoLock(current_object.handle);
        }
        if (!object_label.empty()) {
            object_labels.push_back(std::move(object_label));
            object_name_info.pObjectName = object_labels.back().c_str();
        }

        // If this is a queue, add any queue labels to the callback data.
        if (VK_OBJECT_TYPE_QUEUE == object_name_info.objectType) {
            auto label_iter = debug_utils_queue_labels.find(reinterpret_cast<VkQueue>(object_name_info.objectHandle));
            if (label_iter != debug_utils_queue_labels.end()) {
                label_iter->second->Export(queue_labels);
            }
            // If this is a command buffer, add any command buffer labels to the callback data.
        } else if (VK_OBJECT_TYPE_COMMAND_BUFFER == object_name_info.objectType) {
            auto label_iter = debug_utils_cmd_buffer_labels.find(reinterpret_cast<VkCommandBuffer>(object_name_info.objectHandle));
            if (label_iter != debug_utils_cmd_buffer_labels.end()) {
                label_iter->second->Export(cmd_buf_labels);
            }
        }

        object_name_infos.push_back(object_name_info);
    }

    VkDebugUtilsMessengerCallbackDataEXT callback_data = vku::InitStructHelper();
    callback_data.flags = 0;
    callback_data.pMessageIdName = vuid_text.data();
    callback_data.messageIdNumber = vvl_bit_cast<int32_t>(vuid_hash);
    callback_data.pMessage = nullptr;
    callback_data.queueLabelCount = static_cast<uint32_t>(queue_labels.size());
    callback_data.pQueueLabels = queue_labels.empty() ? nullptr : queue_labels.data();
    callback_data.cmdBufLabelCount = static_cast<uint32_t>(cmd_buf_labels.size());
    callback_data.pCmdBufLabels = cmd_buf_labels.empty() ? nullptr : cmd_buf_labels.data();
    callback_data.objectCount = static_cast<uint32_t>(object_name_infos.size());
    callback_data.pObjects = object_name_infos.data();

    // The text format is more minimal and will have other information in the callback, the JSON is designed to contain everything
    std::string full_message = message_format_settings.json ? CreateMessageJson(msg_flags, loc, object_name_infos, vuid_hash,
                                                                                vuid_text, main_message, at_message_limit)
                                                            : CreateMessageText(loc, vuid_text, main_message, at_message_limit);

    const auto callback_list = &debug_callback_list;
    // We only output to default callbacks if there are no non-default callbacks
    bool use_default_callbacks = true;
    for (const auto &current_callback : *callback_list) {
        use_default_callbacks &= current_callback.IsDefault();
    }

#ifdef VK_USE_PLATFORM_ANDROID_KHR
    if (force_default_log_callback) {
        use_default_callbacks = true;
    }
#endif

    const char *layer_prefix = "Validation";
    bool bail = false;
    for (const auto &current_callback : *callback_list) {
        // Skip callback if it's a default callback and there are non-default callbacks present
        if (current_callback.IsDefault() && !use_default_callbacks) continue;

        // VK_EXT_debug_utils callback
        if (current_callback.IsUtils() && (current_callback.debug_utils_msg_flags & msg_severity) &&
            (current_callback.debug_utils_msg_type & msg_type)) {
            callback_data.pMessage = full_message.c_str();
            if (current_callback.debug_utils_callback_function_ptr(
                    static_cast<VkDebugUtilsMessageSeverityFlagBitsEXT>(msg_severity), msg_type, &callback_data,
                    current_callback.pUserData)) {
                bail = true;
            }
        } else if (!current_callback.IsUtils() && (current_callback.debug_report_msg_flags & msg_flags)) {
            // VK_EXT_debug_report callback (deprecated)
            if (object_name_infos.empty()) {
                VkDebugUtilsObjectNameInfoEXT null_object_name = {VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT, nullptr,
                                                                  VK_OBJECT_TYPE_UNKNOWN, 0, nullptr};
                // need to have at least one object
                object_name_infos.emplace_back(null_object_name);
            }
            if (current_callback.debug_report_callback_function_ptr(
                    msg_flags, ConvertCoreObjectToDebugReportObject(object_name_infos[0].objectType),
                    object_name_infos[0].objectHandle, vuid_hash, 0, layer_prefix, full_message.c_str(),
                    current_callback.pUserData)) {
                bail = true;
            }
        }
    }
    return bail;
}

std::string DebugReport::CreateMessageText(const Location &loc, std::string_view vuid_text, const std::string &main_message,
                                           bool at_message_limit) {
    std::ostringstream oss;

#if defined(BUILD_SELF_VVL)
    oss << "[Self Validation] ";  // How we know if the error is from Self Validation when debugging GPU-AV
#endif

    if (message_format_settings.display_application_name && !message_format_settings.application_name.empty()) {
        oss << "[AppName: " << message_format_settings.application_name << "] ";
    }

    if (at_message_limit) {
        oss << "(Warning - This VUID has now been reported " << duplicate_message_limit
            << " times, which is the duplicated_message_limit value, this will be the last time reporting it).\n";
    }

    oss << loc.Message() << " " << main_message;

    // Append the spec error text to the error message, unless it contains a word treated as special
    if ((vuid_text.find("VUID-") != std::string::npos)) {
        // Linear search makes no assumptions about the layout of the string table. This is not fast, but it does not need to be at
        // this point in the error reporting path
        uint32_t num_vuids = sizeof(vuid_spec_text) / sizeof(vuid_spec_text_pair);
        const char *spec_text = nullptr;
        // Only the Antora site will make use of the sections
        const char *spec_url_section = nullptr;
        for (uint32_t i = 0; i < num_vuids; i++) {
            if (0 == strncmp(vuid_text.data(), vuid_spec_text[i].vuid, vuid_text.size())) {
                spec_text = vuid_spec_text[i].spec_text;
                spec_url_section = vuid_spec_text[i].url_id;
                break;
            }
        }

        // Construct and append the specification text and link to the appropriate version of the spec
        if (spec_text && spec_url_section) {
#ifdef ANNOTATED_SPEC_LINK
            const char *spec_url_base = ANNOTATED_SPEC_LINK;
#else
            const char *spec_url_base = "https://docs.vulkan.org/spec/latest/";
#endif

            // Add period at end if forgotten
            // This provides better seperation between error message and spec text
            if (main_message.back() != '.' && main_message.back() != '\n') {
                oss << '.';
            }

            // Start Vulkan spec text with a new line to make it easier visually
            if (main_message.back() != '\n') {
                oss << '\n';
            }

            oss << "The Vulkan spec states: " << spec_text << " (" << spec_url_base << spec_url_section << "#" << vuid_text << ")";
        }
    }

    return oss.str();
}

std::string DebugReport::CreateMessageJson(VkFlags msg_flags, const Location &loc,
                                           const std::vector<VkDebugUtilsObjectNameInfoEXT> &object_name_infos,
                                           const uint32_t vuid_hash, std::string_view vuid_text, const std::string &main_message,
                                           bool at_message_limit) {
    std::ostringstream oss;
    // For now we just list each JSON field as a new line as it is "pretty-print enough".
    // For Android, things get logged in logcat and having the JSON as a single line is easier to grab from the terminal.
#ifdef VK_USE_PLATFORM_ANDROID_KHR
    char new_line = ' ';
    char line_start = ' ';
#else
    char new_line = '\n';
    char line_start = '\t';
#endif

    oss << "{" << new_line;

    if (message_format_settings.display_application_name && !message_format_settings.application_name.empty()) {
        oss << line_start << "\"AppName\" : \"" << message_format_settings.application_name << "\"," << new_line;
    }

    {
        oss << line_start << "\"Severity\" : \"";
        if (msg_flags & kErrorBit) {
            oss << "Error";
        } else if (msg_flags & kWarningBit) {
            oss << "Warning";
        } else if (msg_flags & kPerformanceWarningBit) {
            oss << "Performance Warning";
        } else if (msg_flags & kInformationBit) {
            oss << "Information";
        } else if (msg_flags & kVerboseBit) {
            oss << "Verbose";
        }
        oss << "\"," << new_line;
    }

    { oss << line_start << "\"VUID\" : \"" << vuid_text << "\"," << new_line; }

    {
        oss << line_start << "\"Objects\" : [" << new_line;
        for (uint32_t i = 0; i < object_name_infos.size(); i++) {
            const VkDebugUtilsObjectNameInfoEXT &src_object = object_name_infos[i];

            oss << line_start << line_start;
            oss << "{\"type\" : \"" << string_VkObjectTypeHandleName(src_object.objectType) << "\", \"handle\" : \"";
            if (0 != src_object.objectHandle) {
                oss << "0x" << std::hex << src_object.objectHandle;
                oss << "\", \"name\" : \"";
                if (src_object.pObjectName) {
                    oss << src_object.pObjectName;
                }
                oss << "\"}";
            } else {
                oss << "VK_NULL_HANDLE\", \"name\" : \"\"}";
            }
            if (i + 1 != object_name_infos.size()) {
                oss << ",";
            }
            oss << new_line;
        }
        oss << line_start << "]," << new_line;
    }

    { oss << line_start << "\"MessageID\" : \"0x" << std::hex << vuid_hash << "\"," << new_line; }
    { oss << line_start << "\"Function\" : \"" << loc.StringFunc() << "\"," << new_line; }
    { oss << line_start << "\"Location\" : \"" << loc.Fields() << "\"," << new_line; }
    {
        oss << line_start << "\"MainMessage\" : \"";

        if (at_message_limit) {
            oss << "(Warning - This VUID has now been reported " << duplicate_message_limit
                << " times, which is the duplicated_message_limit value, this will be the last time reporting it). ";
        }

        // For cases were where have multi-lines in the message, we need to escape them.
        // The idea is the JSON is machine readable and when someone prints the value out, the new lines will resolve then.
        for (char c : main_message) {
            if (c == '\n') {
                oss << "\\n";
            } else {
                oss << c;
            }
        }
        oss << "\"," << new_line;
    }
    {
        oss << line_start << "\"DebugRegion\" : \"";
        if (loc.debug_region && !loc.debug_region->empty()) {
            oss << loc.debug_region;
        }
        oss << "\"," << new_line;
    }

    if ((vuid_text.find("VUID-") != std::string::npos)) {
        // Linear search makes no assumptions about the layout of the string table. This is not fast, but it does not need to be at
        // this point in the error reporting path
        uint32_t num_vuids = sizeof(vuid_spec_text) / sizeof(vuid_spec_text_pair);
        const char *spec_text = nullptr;
        // Only the Antora site will make use of the sections
        const char *spec_url_section = nullptr;
        for (uint32_t i = 0; i < num_vuids; i++) {
            if (0 == strncmp(vuid_text.data(), vuid_spec_text[i].vuid, vuid_text.size())) {
                spec_text = vuid_spec_text[i].spec_text;
                spec_url_section = vuid_spec_text[i].url_id;
                break;
            }
        }

        if (spec_text) {
            oss << line_start << "\"SpecText\" : \"" << spec_text << "\"," << new_line;
        } else {
            oss << line_start << "\"SpecText\" : \"\"," << new_line;
        }

        // Construct and append the specification text and link to the appropriate version of the spec
        if (spec_text && spec_url_section) {
#ifdef ANNOTATED_SPEC_LINK
            std::string spec_url_base = ANNOTATED_SPEC_LINK;
#else
            std::string spec_url_base = "https://docs.vulkan.org/spec/latest/";
#endif
            oss << line_start << "\"SpecUrl\" : \"" << spec_url_base << spec_url_section << "#" << vuid_text << "\"" << new_line;

        } else {
            oss << line_start << "\"SpecUrl\" : \"\"" << new_line;
        }
    } else {
        oss << line_start << "\"SpecText\" : \"\"," << new_line;
        oss << line_start << "\"SpecUrl\" : \"\"" << new_line;
    }
    oss << "}";
    return oss.str();
}

void DebugReport::SetUtilsObjectName(const VkDebugUtilsObjectNameInfoEXT *pNameInfo) {
    std::unique_lock<std::mutex> lock(debug_output_mutex);
    if (pNameInfo->pObjectName) {
        debug_utils_object_name_map[pNameInfo->objectHandle] = pNameInfo->pObjectName;
    } else {
        debug_utils_object_name_map.erase(pNameInfo->objectHandle);
    }
}

void DebugReport::SetMarkerObjectName(const VkDebugMarkerObjectNameInfoEXT *pNameInfo) {
    std::unique_lock<std::mutex> lock(debug_output_mutex);
    if (pNameInfo->pObjectName) {
        debug_object_name_map[pNameInfo->object] = pNameInfo->pObjectName;
    } else {
        debug_object_name_map.erase(pNameInfo->object);
    }
}

// NoLock suffix means that the function itself does not hold debug_output_mutex lock,
// and it's **mandatory responsibility** of the caller to hold this lock.
std::string DebugReport::GetUtilsObjectNameNoLock(const uint64_t object) const {
    std::string label = "";
    const auto utils_name_iter = debug_utils_object_name_map.find(object);
    if (utils_name_iter != debug_utils_object_name_map.end()) {
        label = utils_name_iter->second;
    }
    return label;
}

// NoLock suffix means that the function itself does not hold debug_output_mutex lock,
// and it's **mandatory responsibility** of the caller to hold this lock.
std::string DebugReport::GetMarkerObjectNameNoLock(const uint64_t object) const {
    std::string label = "";
    const auto marker_name_iter = debug_object_name_map.find(object);
    if (marker_name_iter != debug_object_name_map.end()) {
        label = marker_name_iter->second;
    }
    return label;
}

std::string DebugReport::FormatHandle(const char *handle_type_name, uint64_t handle) const {
    std::unique_lock<std::mutex> lock(debug_output_mutex);
    std::string handle_name = GetUtilsObjectNameNoLock(handle);
    if (handle_name.empty()) {
        handle_name = GetMarkerObjectNameNoLock(handle);
    }

    std::ostringstream str;
    str << handle_type_name << " ";
    str << "0x" << std::hex << handle;

    if (!handle_name.empty()) {
        str << "[" << handle_name.c_str() << "]";
    }
    return str.str();
}

template <typename Map>
static LoggingLabelState *GetLoggingLabelState(Map *map, typename Map::key_type key, bool insert) {
    auto iter = map->find(key);
    LoggingLabelState *label_state = nullptr;
    if (iter == map->end()) {
        if (insert) {
            // Add a label state if not present
            auto inserted = map->emplace(key, std::unique_ptr<LoggingLabelState>(new LoggingLabelState()));
            assert(inserted.second);
            iter = inserted.first;
            label_state = iter->second.get();
        }
    } else {
        label_state = iter->second.get();
    }
    return label_state;
}

void DebugReport::BeginQueueDebugUtilsLabel(VkQueue queue, const VkDebugUtilsLabelEXT *label_info) {
    std::unique_lock<std::mutex> lock(debug_output_mutex);
    if (nullptr != label_info && nullptr != label_info->pLabelName) {
        auto *label_state = GetLoggingLabelState(&debug_utils_queue_labels, queue, /* insert */ true);
        assert(label_state);
        label_state->labels.push_back(label_info);

        // TODO: Determine if this is the correct semantics for insert label vs. begin/end, perserving existing semantics for now
        label_state->insert_label.Reset();
    }
}

void DebugReport::EndQueueDebugUtilsLabel(VkQueue queue) {
    std::unique_lock<std::mutex> lock(debug_output_mutex);
    auto *label_state = GetLoggingLabelState(&debug_utils_queue_labels, queue, /* insert */ false);
    if (label_state) {
        // Pop the normal item
        if (!label_state->labels.empty()) {
            label_state->labels.pop_back();
        }

        // TODO: Determine if this is the correct semantics for insert label vs. begin/end, perserving existing semantics for now
        label_state->insert_label.Reset();
    }
}

void DebugReport::InsertQueueDebugUtilsLabel(VkQueue queue, const VkDebugUtilsLabelEXT *label_info) {
    std::unique_lock<std::mutex> lock(debug_output_mutex);
    auto *label_state = GetLoggingLabelState(&debug_utils_queue_labels, queue, /* insert */ true);

    // TODO: Determine if this is the correct semantics for insert label vs. begin/end, perserving existing semantics for now
    label_state->insert_label = LoggingLabel(label_info);
}

void DebugReport::BeginCmdDebugUtilsLabel(VkCommandBuffer command_buffer, const VkDebugUtilsLabelEXT *label_info) {
    std::unique_lock<std::mutex> lock(debug_output_mutex);
    if (nullptr != label_info && nullptr != label_info->pLabelName) {
        auto *label_state = GetLoggingLabelState(&debug_utils_cmd_buffer_labels, command_buffer, /* insert */ true);
        assert(label_state);
        label_state->labels.push_back(label_info);

        // TODO: Determine if this is the correct semantics for insert label vs. begin/end, perserving existing semantics for now
        label_state->insert_label.Reset();
    }
}

void DebugReport::EndCmdDebugUtilsLabel(VkCommandBuffer command_buffer) {
    std::unique_lock<std::mutex> lock(debug_output_mutex);
    auto *label_state = GetLoggingLabelState(&debug_utils_cmd_buffer_labels, command_buffer, /* insert */ false);
    if (label_state) {
        // Pop the normal item
        if (!label_state->labels.empty()) {
            label_state->labels.pop_back();
        }

        // TODO: Determine if this is the correct semantics for insert label vs. begin/end, perserving existing semantics for now
        label_state->insert_label.Reset();
    }
}

void DebugReport::InsertCmdDebugUtilsLabel(VkCommandBuffer command_buffer, const VkDebugUtilsLabelEXT *label_info) {
    std::unique_lock<std::mutex> lock(debug_output_mutex);
    auto *label_state = GetLoggingLabelState(&debug_utils_cmd_buffer_labels, command_buffer, /* insert */ true);
    assert(label_state);

    // TODO: Determine if this is the correct semantics for insert label vs. begin/end, perserving existing semantics for now
    label_state->insert_label = LoggingLabel(label_info);
}

// Current tracking beyond a single command buffer scope is incorrect, and even when it is we need to be able to clean up
void DebugReport::ResetCmdDebugUtilsLabel(VkCommandBuffer command_buffer) {
    std::unique_lock<std::mutex> lock(debug_output_mutex);
    auto *label_state = GetLoggingLabelState(&debug_utils_cmd_buffer_labels, command_buffer, /* insert */ false);
    if (label_state) {
        label_state->labels.clear();
        label_state->insert_label.Reset();
    }
}

void DebugReport::EraseCmdDebugUtilsLabel(VkCommandBuffer command_buffer) {
    std::unique_lock<std::mutex> lock(debug_output_mutex);
    debug_utils_cmd_buffer_labels.erase(command_buffer);
}

template <typename TCreateInfo, typename TCallback>
static void LayerCreateCallback(DebugCallbackStatusFlags callback_status, DebugReport *debug_report, const TCreateInfo *create_info,
                                TCallback *callback) {
    std::unique_lock<std::mutex> lock(debug_report->debug_output_mutex);

    debug_report->debug_callback_list.emplace_back(VkLayerDbgFunctionState());
    auto &callback_state = debug_report->debug_callback_list.back();
    callback_state.callback_status = callback_status;
    callback_state.pUserData = create_info->pUserData;

    if (callback_state.IsUtils()) {
        auto utils_create_info = reinterpret_cast<const VkDebugUtilsMessengerCreateInfoEXT *>(create_info);
        auto utils_callback = reinterpret_cast<VkDebugUtilsMessengerEXT *>(callback);
        if (!(*utils_callback)) {
            // callback constructed default callbacks have no handle -- so use struct address as unique handle
            *utils_callback = reinterpret_cast<VkDebugUtilsMessengerEXT>(&callback_state);
        }
        callback_state.debug_utils_callback_object = *utils_callback;
        callback_state.debug_utils_callback_function_ptr = utils_create_info->pfnUserCallback;
        callback_state.debug_utils_msg_flags = utils_create_info->messageSeverity;
        callback_state.debug_utils_msg_type = utils_create_info->messageType;
    } else {  // Debug report callback
        auto report_create_info = reinterpret_cast<const VkDebugReportCallbackCreateInfoEXT *>(create_info);
        auto report_callback = reinterpret_cast<VkDebugReportCallbackEXT *>(callback);
        if (!(*report_callback)) {
            // Internally constructed default callbacks have no handle -- so use struct address as unique handle
            *report_callback = reinterpret_cast<VkDebugReportCallbackEXT>(&callback_state);
        }
        callback_state.debug_report_callback_object = *report_callback;
        callback_state.debug_report_callback_function_ptr = report_create_info->pfnCallback;
        callback_state.debug_report_msg_flags = report_create_info->flags;
    }

#ifdef VK_USE_PLATFORM_ANDROID_KHR
    // On Android, if the default callback system property is set, force the default callback to be printed
    std::string force_layer_log = GetEnvironment(kForceDefaultCallbackKey);
    int force_default_callback = atoi(force_layer_log.c_str());
    if (force_default_callback == 1) {
        debug_report->force_default_log_callback = true;
    }
#endif

    debug_report->SetDebugUtilsSeverityFlags(debug_report->debug_callback_list);
}

VKAPI_ATTR VkResult LayerCreateMessengerCallback(DebugReport *debug_report, bool default_callback,
                                                 const VkDebugUtilsMessengerCreateInfoEXT *create_info,
                                                 VkDebugUtilsMessengerEXT *messenger) {
    LayerCreateCallback((DEBUG_CALLBACK_UTILS | (default_callback ? DEBUG_CALLBACK_DEFAULT : 0)), debug_report, create_info,
                        messenger);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult LayerCreateReportCallback(DebugReport *debug_report, bool default_callback,
                                              const VkDebugReportCallbackCreateInfoEXT *create_info,
                                              VkDebugReportCallbackEXT *callback) {
    LayerCreateCallback((default_callback ? DEBUG_CALLBACK_DEFAULT : 0), debug_report, create_info, callback);
    return VK_SUCCESS;
}

VKAPI_ATTR void ActivateInstanceDebugCallbacks(DebugReport *debug_report) {
    auto current = debug_report->instance_pnext_chain;
    for (;;) {
        auto create_info = vku::FindStructInPNextChain<VkDebugUtilsMessengerCreateInfoEXT>(current);
        if (!create_info) break;
        current = create_info->pNext;
        VkDebugUtilsMessengerEXT utils_callback{};
        LayerCreateCallback((DEBUG_CALLBACK_UTILS | DEBUG_CALLBACK_INSTANCE), debug_report, create_info, &utils_callback);
    }
    for (;;) {
        auto create_info = vku::FindStructInPNextChain<VkDebugReportCallbackCreateInfoEXT>(current);
        if (!create_info) break;
        current = create_info->pNext;
        VkDebugReportCallbackEXT report_callback{};
        LayerCreateCallback(DEBUG_CALLBACK_INSTANCE, debug_report, create_info, &report_callback);
    }
}

VKAPI_ATTR void DeactivateInstanceDebugCallbacks(DebugReport *debug_report) {
    if (!vku::FindStructInPNextChain<VkDebugUtilsMessengerCreateInfoEXT>(debug_report->instance_pnext_chain) &&
        !vku::FindStructInPNextChain<VkDebugReportCallbackCreateInfoEXT>(debug_report->instance_pnext_chain))
        return;
    std::vector<VkDebugUtilsMessengerEXT> instance_utils_callback_handles{};
    std::vector<VkDebugReportCallbackEXT> instance_report_callback_handles{};
    for (const auto &item : debug_report->debug_callback_list) {
        if (item.IsInstance()) {
            if (item.IsUtils()) {
                instance_utils_callback_handles.push_back(item.debug_utils_callback_object);
            } else {
                instance_report_callback_handles.push_back(item.debug_report_callback_object);
            }
        }
    }
    for (const auto &item : instance_utils_callback_handles) {
        LayerDestroyCallback(debug_report, item);
    }
    for (const auto &item : instance_report_callback_handles) {
        LayerDestroyCallback(debug_report, item);
    }
}

bool DebugReport::LogMessageVaList(VkFlags msg_flags, std::string_view vuid_text, const LogObjectList &objects, const Location &loc,
                                   const char *format, va_list argptr) {
    const std::string main_message = text::VFormat(format, argptr);
    return LogMessage(msg_flags, vuid_text, objects, loc, main_message);
}

VKAPI_ATTR VkBool32 VKAPI_CALL MessengerBreakCallback([[maybe_unused]] VkDebugUtilsMessageSeverityFlagBitsEXT message_severity,
                                                      [[maybe_unused]] VkDebugUtilsMessageTypeFlagsEXT message_type,
                                                      [[maybe_unused]] const VkDebugUtilsMessengerCallbackDataEXT *callback_data,
                                                      [[maybe_unused]] void *user_data) {
    // TODO: Consider to use https://github.com/scottt/debugbreak
#ifdef VK_USE_PLATFORM_WIN32_KHR
    DebugBreak();
#else
    raise(SIGTRAP);
#endif

    return false;
}

static std::string CreateDefaultCallbackMessage(VkDebugUtilsMessageSeverityFlagBitsEXT message_severity,
                                                VkDebugUtilsMessageTypeFlagsEXT message_type,
                                                const VkDebugUtilsMessengerCallbackDataEXT &callback_data) {
    std::ostringstream oss;

    // The callback is in JSON (this is the only way the first char is '{')
    // If the user enables JSON, we only will print out JSON.
    if (callback_data.pMessage[0] == '{') {
        oss << callback_data.pMessage << '\n';
        return oss.str();
    }

    if (message_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        oss << "Validation Error: ";
    } else if (message_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        if (message_type & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT) {
            oss << "Validation Performance Warning: ";
        } else {
            oss << "Validation Warning: ";
        }
    } else if (message_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
        oss << "Validation Information: ";
    } else if (message_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) {
        oss << "Verbose Information: ";
    }

    oss << "[ " << callback_data.pMessageIdName << " ] | MessageID = 0x" << std::hex << callback_data.messageIdNumber << '\n';

    oss << callback_data.pMessage << '\n';

    if (callback_data.objectCount > 0) {
        oss << "Objects: " << callback_data.objectCount << '\n';
        for (uint32_t i = 0; i < callback_data.objectCount; i++) {
            const auto &debug_object = callback_data.pObjects[i];
            oss << "    [" << i << "] " << string_VkObjectTypeHandleName(debug_object.objectType);
            if (debug_object.objectHandle) {
                oss << " 0x" << std::hex << debug_object.objectHandle;
            } else {
                oss << " VK_NULL_HANDLE";
            }
            if (debug_object.pObjectName) {
                oss << "[" << debug_object.pObjectName << "]";
            }
            oss << '\n';
        }
    }

#ifndef VK_USE_PLATFORM_ANDROID_KHR
    oss << '\n';  // provide space between consecutive errors
#endif

    return oss.str();
}

VKAPI_ATTR VkBool32 VKAPI_CALL MessengerLogCallback(VkDebugUtilsMessageSeverityFlagBitsEXT message_severity,
                                                    VkDebugUtilsMessageTypeFlagsEXT message_type,
                                                    const VkDebugUtilsMessengerCallbackDataEXT *callback_data, void *user_data) {
    const std::string msg_buffer_str = CreateDefaultCallbackMessage(message_severity, message_type, *callback_data);

    // By default we are really just printing to stdout
    // Even if this is stdout, we still want to print for android
    // VVL testing (and probably other systems now) call freopen() to map stdout to dedicated file
    fprintf((FILE *)user_data, "%s", msg_buffer_str.c_str());
    fflush((FILE *)user_data);

#ifdef VK_USE_PLATFORM_ANDROID_KHR
    // If the user uses there own callback, we can let them fix the formatting, but as a default, some error messages will be way to
    // long for default logcat buffer. While one *can* adjust the logcat size, we assume 1024 is the max and chunk it up here. (note
    // that \n will automatically print a new line in logcat, but still counts towards the 1024 limit)
    const size_t chunk_size = 1024;
    const size_t total_size = msg_buffer_str.size();
    size_t offset = 0;
    while (offset < total_size) {
        size_t bytes_to_print = std::min(chunk_size, total_size - offset);
        __android_log_print(ANDROID_LOG_INFO, "VALIDATION", "%s", msg_buffer_str.c_str() + offset);
        offset += bytes_to_print;
    }
#endif

    return false;
}

VKAPI_ATTR VkBool32 VKAPI_CALL MessengerWin32DebugOutputMsg(VkDebugUtilsMessageSeverityFlagBitsEXT message_severity,
                                                            VkDebugUtilsMessageTypeFlagsEXT message_type,
                                                            const VkDebugUtilsMessengerCallbackDataEXT *callback_data,
                                                            [[maybe_unused]] void *user_data) {
    const std::string msg_buffer_str = CreateDefaultCallbackMessage(message_severity, message_type, *callback_data);
    [[maybe_unused]] const char *cstr = msg_buffer_str.c_str();

#ifdef VK_USE_PLATFORM_WIN32_KHR
    OutputDebugString(cstr);
#endif

    return false;
}
