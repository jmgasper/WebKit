/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include <wtf/Function.h>

namespace WebKit {

class WebPageProxy;

// Answers a beforeunload panel by the view's SetBeforeUnloadBehavior(), and
// returns true, unless the view asks the user (returns false). Main thread.
bool answerBeforeUnloadHaiku(WebPageProxy&, Function<void(bool)>& completion);

} // namespace WebKit
