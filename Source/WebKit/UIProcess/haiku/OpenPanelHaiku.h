/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#pragma once

namespace API {
class OpenPanelParameters;
}

namespace WebKit {

class WebOpenPanelResultListenerProxy;

// Answers an <input type=file> with a native Tracker file panel: files or a
// directory, one or several, filtered by the input's accept list. The listener
// gets the chosen paths, or a cancellation when the panel is closed. Main
// thread only; returns false when no panel could be shown.
bool runOpenPanelHaiku(API::OpenPanelParameters*, WebOpenPanelResultListenerProxy*);

} // namespace WebKit
