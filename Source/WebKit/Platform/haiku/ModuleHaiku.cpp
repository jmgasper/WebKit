/*
    Copyright (C) 2014 Haiku, Inc.

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public License
    along with this library; see the file COPYING.LIB.  If not, write to
    the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
    Boston, MA 02110-1301, USA.
 */

#include "config.h"
#include "Module.h"

#include <wtf/text/CString.h>
#include <utility>

namespace WebKit {

bool Module::load()
{
    if (m_module >= 0)
        return true;
    m_module = load_add_on(m_path.utf8().legacyCStringPointer());
    return m_module >= 0;
}

void Module::unload()
{
    auto module = std::exchange(m_module, -1);
    if (module >= 0)
        unload_add_on(module);
}

void* Module::platformFunctionPointer(const char* functionName) const
{
    void* pointer = nullptr;
    if (m_module < 0 || get_image_symbol(m_module, functionName, B_SYMBOL_TYPE_ANY, &pointer) != B_OK)
        return nullptr;
    return pointer;
}

} // namespace WebKit
