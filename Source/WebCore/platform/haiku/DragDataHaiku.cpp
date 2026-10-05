/*
 * Copyright (C) 2007 Apple Inc.  All rights reserved.
 * Copyright (C) 2007 Ryan Leavengood <leavengood@gmail.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE COMPUTER, INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "config.h"
#include "DragData.h"

#include "Document.h"
#include "DocumentFragment.h"
#include "NotImplemented.h"
#include <wtf/URL.h>


namespace WebCore {

// Only drags from other applications reach a page (a drag that starts in a
// page is not handed to the system), and of those only files are understood:
// Tracker's drags carry refs, which the view turns into paths.

bool DragData::canSmartReplace() const
{
    return false;
}

bool DragData::containsColor() const
{
    return false;
}

bool DragData::containsFiles() const
{
    return !m_fileNames.isEmpty();
}

unsigned DragData::numberOfFiles() const
{
    return m_fileNames.size();
}

Vector<String> DragData::asFilenames() const
{
    return m_fileNames;
}

bool DragData::containsPlainText() const
{
    return false;
}

String DragData::asPlainText() const
{
    return String();
}

Color DragData::asColor() const
{
    return Color();
}

bool DragData::containsCompatibleContent(WebCore::DragData::DraggingPurpose purpose) const
{
    if (purpose == DraggingPurpose::ForFileUpload)
        return containsFiles();
    return containsColor() || containsURL() || containsPlainText() || containsFiles();
}

// A file dropped where the page takes nothing opens in the tab, as in other
// browsers: its file: URL is the drag's URL.
bool DragData::containsURL(FilenameConversionPolicy policy) const
{
    return policy == ConvertFilenames && containsFiles();
}

String DragData::asURL(FilenameConversionPolicy policy, String* title) const
{
    if (policy != ConvertFilenames || !containsFiles())
        return String();
    if (title)
        *title = m_fileNames[0].substring(m_fileNames[0].reverseFind('/') + 1);
    return URL::fileURLWithFileSystemPath(m_fileNames[0]).string();
}

bool DragData::shouldMatchStyleOnDrop() const
{
    return false;
}


} // namespace WebCore
