/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include <WebCore/PlatformKeyboardEvent.h>
#include <WebCore/WindowsKeyboardCodes.h>

namespace WebKit {

inline const char* editingCommandHaiku(const WebCore::PlatformKeyboardEvent& event, bool characterEvent)
{
    if (event.altKey() || event.metaKey())
        return nullptr;
    bool control = event.controlKey();
    bool shift = event.shiftKey();
    unsigned key = event.windowsVirtualKeyCode();
    if (characterEvent) {
        if (event.text().isEmpty() || control)
            return nullptr;
        switch (event.text()[0]) {
        case '\t': return shift ? "InsertBacktab" : "InsertTab";
        case '\r':
        case '\n': return shift ? "InsertLineBreak" : "InsertNewline";
        default: return nullptr;
        }
    }
    switch (key) {
    case VK_LEFT:
        return control ? (shift ? "MoveWordLeftAndModifySelection" : "MoveWordLeft")
            : (shift ? "MoveLeftAndModifySelection" : "MoveLeft");
    case VK_RIGHT:
        return control ? (shift ? "MoveWordRightAndModifySelection" : "MoveWordRight")
            : (shift ? "MoveRightAndModifySelection" : "MoveRight");
    case VK_UP:
        return control ? (shift ? "MoveParagraphBackwardAndModifySelection" : "MoveToBeginningOfParagraph")
            : (shift ? "MoveUpAndModifySelection" : "MoveUp");
    case VK_DOWN:
        return control ? (shift ? "MoveParagraphForwardAndModifySelection" : "MoveToEndOfParagraph")
            : (shift ? "MoveDownAndModifySelection" : "MoveDown");
    case VK_HOME:
        return control ? (shift ? "MoveToBeginningOfDocumentAndModifySelection" : "MoveToBeginningOfDocument")
            : (shift ? "MoveToBeginningOfLineAndModifySelection" : "MoveToBeginningOfLine");
    case VK_END:
        return control ? (shift ? "MoveToEndOfDocumentAndModifySelection" : "MoveToEndOfDocument")
            : (shift ? "MoveToEndOfLineAndModifySelection" : "MoveToEndOfLine");
    case VK_PRIOR: return shift ? "MovePageUpAndModifySelection" : "MovePageUp";
    case VK_NEXT: return shift ? "MovePageDownAndModifySelection" : "MovePageDown";
    case VK_BACK: return control ? "DeleteWordBackward" : "DeleteBackward";
    case VK_DELETE: return control ? "DeleteWordForward" : "DeleteForward";
    case VK_TAB: return control ? nullptr : (shift ? "InsertBacktab" : "InsertTab");
    case VK_RETURN: return control ? nullptr : (shift ? "InsertLineBreak" : "InsertNewline");
    case VK_ESCAPE: return control || shift ? nullptr : "Cancel";
    default: break;
    }
    if (!control)
        return nullptr;
    if (key == VK_Z)
        return shift ? "Redo" : "Undo";
    if (shift)
        return nullptr;
    switch (key) {
    case VK_A: return "SelectAll";
    case VK_B: return "ToggleBold";
    case VK_C: return "Copy";
    case VK_I: return "ToggleItalic";
    case VK_V: return "Paste";
    case VK_X: return "Cut";
    case VK_Y: return "Redo";
    default: return nullptr;
    }
}

} // namespace WebKit
