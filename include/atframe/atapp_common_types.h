// Copyright 2026 atframework
// Created by owent on 2026-01-30

#pragma once

#include <gsl/select-gsl.h>
#include <time/jiffies_timer.h>

#include "atframe/atapp_config.h"

LIBATAPP_MACRO_NAMESPACE_BEGIN

using jiffies_timer_t = atfw::util::time::jiffies_timer<8, 3, 9>;
using jiffies_timer_handle_t = jiffies_timer_t::timer_callback_fn_t;
using jiffies_timer_watcher_t = jiffies_timer_t::timer_wptr_t;

LIBATAPP_MACRO_NAMESPACE_END
