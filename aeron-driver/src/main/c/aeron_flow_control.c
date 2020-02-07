/*
 * Copyright 2014-2020 Real Logic Limited.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#if defined(__linux__)
#define _BSD_SOURCE
#define _GNU_SOURCE
#endif

#include <stdlib.h>
#include <errno.h>
#include <media/aeron_udp_channel.h>
#include "protocol/aeron_udp_protocol.h"
#include "concurrent/aeron_logbuffer_descriptor.h"
#include "util/aeron_error.h"
#include "util/aeron_dlopen.h"
#include "util/aeron_parse_util.h"
#include "aeron_flow_control.h"
#include "aeron_alloc.h"

aeron_flow_control_strategy_supplier_func_t aeron_flow_control_strategy_supplier_load(const char *strategy_name)
{
    aeron_flow_control_strategy_supplier_func_t func = NULL;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
    if ((func = (aeron_flow_control_strategy_supplier_func_t)aeron_dlsym(RTLD_DEFAULT, strategy_name)) == NULL)
    {
        aeron_set_err(EINVAL, "could not find flow control strategy %s: dlsym - %s", strategy_name, aeron_dlerror());
        return NULL;
    }
#pragma GCC diagnostic pop

    return func;
}

int64_t aeron_max_flow_control_strategy_on_idle(
    void *state,
    int64_t now_ns,
    int64_t snd_lmt,
    int64_t snd_pos,
    bool is_end_of_stream)
{
    return snd_lmt;
}

int64_t aeron_max_flow_control_strategy_on_sm(
    void *state,
    const uint8_t *sm,
    size_t length,
    struct sockaddr_storage *recv_addr,
    int64_t snd_lmt,
    int32_t initial_term_id,
    size_t position_bits_to_shift,
    int64_t now_ns)
{
    aeron_status_message_header_t *status_message_header = (aeron_status_message_header_t *)sm;

    int64_t position = aeron_logbuffer_compute_position(
        status_message_header->consumption_term_id,
        status_message_header->consumption_term_offset,
        position_bits_to_shift,
        initial_term_id);
    int64_t window_edge = position + status_message_header->receiver_window;

    return snd_lmt > window_edge ? snd_lmt : window_edge;
}

int aeron_max_flow_control_strategy_fini(aeron_flow_control_strategy_t *strategy)
{
    aeron_free(strategy->state);
    aeron_free(strategy);
    return 0;
}

int aeron_max_multicast_flow_control_strategy_supplier(
    aeron_flow_control_strategy_t **strategy,
    aeron_udp_channel_t *channel,
    int32_t stream_id,
    int64_t registration_id,
    int32_t initial_term_id,
    size_t term_length)
{
    aeron_flow_control_strategy_t *_strategy;

    if (aeron_alloc((void **)&_strategy, sizeof(aeron_flow_control_strategy_t)) < 0)
    {
        return -1;
    }

    _strategy->state = NULL;  // Max does not require any state.
    _strategy->on_idle = aeron_max_flow_control_strategy_on_idle;
    _strategy->on_status_message = aeron_max_flow_control_strategy_on_sm;
    _strategy->fini = aeron_max_flow_control_strategy_fini;

    *strategy = _strategy;

    return 0;
}

int aeron_unicast_flow_control_strategy_supplier(
    aeron_flow_control_strategy_t **strategy,
    aeron_udp_channel_t *channel,
    int32_t stream_id,
    int64_t registration_id,
    int32_t initial_term_id,
    size_t term_length)
{
    return aeron_max_multicast_flow_control_strategy_supplier(
        strategy, channel, stream_id, registration_id, initial_term_id, term_length);
}

aeron_flow_control_strategy_supplier_func_table_entry_t aeron_flow_control_strategy_supplier_table[] =
{
    { AERON_UNICAST_MAX_FLOW_CONTROL_STRATEGY_NAME, aeron_unicast_flow_control_strategy_supplier },
    { AERON_MULTICAST_MAX_FLOW_CONTROL_STRATEGY_NAME, aeron_max_multicast_flow_control_strategy_supplier },
    { AERON_MULTICAST_MIN_FLOW_CONTROL_STRATEGY_NAME, aeron_min_flow_control_strategy_supplier }
};

aeron_flow_control_strategy_supplier_func_t aeron_flow_control_strategy_supplier_by_name(const char *name)
{
    size_t entries = sizeof(aeron_flow_control_strategy_supplier_table) /
        sizeof(aeron_flow_control_strategy_supplier_func_table_entry_t);

    for (size_t i = 0; i < entries; i++)
    {
        aeron_flow_control_strategy_supplier_func_table_entry_t *entry = &aeron_flow_control_strategy_supplier_table[i];

        if (strncmp(entry->name, name, strlen(entry->name)) == 0)
        {
            return entry->supplier_func;
        }
    }

    return NULL;
}

int aeron_default_multicast_flow_control_strategy_supplier(
    aeron_flow_control_strategy_t **strategy,
    aeron_flow_control_strategy_supplier_func_t fallback_flow_control_supplier,
    aeron_udp_channel_t *channel,
    int32_t stream_id,
    int64_t registration_id,
    int32_t initial_term_id,
    size_t term_length)
{
    const char *max_strategy_name = "max";
    const char *min_strategy_name = "min";

    const char *flow_control_options = aeron_uri_find_param_value(&channel->uri.params.udp.additional_params, "fc");
    if (NULL == flow_control_options)
    {
        return fallback_flow_control_supplier(
            strategy, channel, stream_id, registration_id, initial_term_id, term_length);
    }

    aeron_flow_control_preferred_options_t preferred_options;
    if (aeron_flow_control_parse_preferred_options(
        strlen(flow_control_options), flow_control_options, &preferred_options) < 0)
    {
        return -1;
    }

    if (0 == preferred_options.strategy_name_length || NULL == preferred_options.strategy_name)
    {
        aeron_set_err(
            EINVAL, "No flow control strategy name specified, URI: %.*s", channel->uri_length, channel->original_uri);
        return -1;
    }

    if (strlen(max_strategy_name) == preferred_options.strategy_name_length &&
        0 == strncmp(max_strategy_name, preferred_options.strategy_name, strlen(max_strategy_name)))
    {
        return aeron_max_multicast_flow_control_strategy_supplier(
            strategy, channel, stream_id, registration_id, initial_term_id, term_length);
    }

    if (strlen(min_strategy_name) == preferred_options.strategy_name_length &&
        0 == strncmp(min_strategy_name, preferred_options.strategy_name, strlen(min_strategy_name)))
    {
        if (preferred_options.has_receiver_tag)
        {
            return aeron_preferred_flow_control_strategy_supplier(
                strategy, channel, stream_id, registration_id, initial_term_id, term_length);
        }
        else
        {
            return aeron_min_flow_control_strategy_supplier(
                strategy, channel, stream_id, registration_id, initial_term_id, term_length);
        }
    }

    aeron_set_err(
        EINVAL, "Invalid flow control strategy name: %.*s from URI: %.*s",
        preferred_options.strategy_name_length, preferred_options.strategy_name,
        channel->uri_length, channel->original_uri);

    return -1;
}


#define AERON_FLOW_CONTROL_NUMBER_BUFFER_LEN (64)

int aeron_flow_control_parse_preferred_options(
    const size_t options_length,
    const char *options,
    aeron_flow_control_preferred_options_t *flow_control_options)
{
    flow_control_options->strategy_name = NULL;
    flow_control_options->strategy_name_length = 0;
    flow_control_options->timeout_ns = 0;
    flow_control_options->has_receiver_tag = false;
    flow_control_options->receiver_tag = -1;

    char number_buffer[AERON_FLOW_CONTROL_NUMBER_BUFFER_LEN];
    memset(number_buffer, 0, AERON_FLOW_CONTROL_NUMBER_BUFFER_LEN);

    const char* current_option = options;
    size_t remaining = options_length;

    const char *next_option;
    do
    {
        next_option = (const char *)memchr(current_option, ',', remaining);

        ptrdiff_t current_option_length;

        if (NULL == next_option)
        {
            current_option_length = remaining;
        }
        else
        {
            current_option_length = next_option - current_option;

            // Skip the comma.
            next_option++;
            remaining -= (current_option_length + 1);
        }

        if (NULL == flow_control_options->strategy_name)
        {
            flow_control_options->strategy_name = current_option;
            flow_control_options->strategy_name_length = current_option_length;
        }
        else if (current_option_length > 2 &&
            ('g' == current_option[0] || 't' == current_option[0]) &&
            ':' == current_option[1])
        {
            const size_t value_length = current_option_length - 2;
            const char *value = current_option + 2;

            if (AERON_FLOW_CONTROL_NUMBER_BUFFER_LEN <= value_length)
            {
                aeron_set_err(
                    -EINVAL,
                    "Flow control options - number field too long (found %d, max %d), field: %.*s, options: %.*s",
                    value_length, (AERON_FLOW_CONTROL_NUMBER_BUFFER_LEN - 1),
                    value_length, value,
                    options_length, options);

                return -EINVAL;
            }
            strncpy(number_buffer, value, value_length);

            if ('g' == current_option[0])
            {
                int32_t receiver_tag;
                char *end_ptr = "";
                errno = 0;

                receiver_tag = strtol(number_buffer, &end_ptr, 10);

                if (0 == errno && '\0' == *end_ptr)
                {
                    flow_control_options->has_receiver_tag = true;
                    flow_control_options->receiver_tag = receiver_tag;
                }
                else
                {
                    aeron_set_err(
                        -EINVAL,
                        "Flow control options - invalid group, field: %.*s, options: %.*s",
                        current_option_length, current_option,
                        options_length, options);

                    return -EINVAL;
                }
            }
            else if ('t' == current_option[0])
            {
                uint64_t timeout_ns;
                if (0 <= aeron_parse_duration_ns(number_buffer, &timeout_ns))
                {
                    flow_control_options->timeout_ns = timeout_ns;
                }
                else
                {
                    aeron_set_err(
                        -EINVAL,
                        "Flow control options - invalid timeout, field: %.*s, options: %.*s",
                        current_option_length, current_option,
                        options_length, options);

                    return -EINVAL;
                }
            }
        }
        else
        {
            aeron_set_err(
                -EINVAL,
                "Flow control options - unrecognised option, field: %.*s, options: %.*s",
                current_option_length, current_option,
                options_length, options);

            return -EINVAL;
        }

        current_option = next_option;
    }
    while (NULL != current_option && 0 < remaining);

    return 0;
}
