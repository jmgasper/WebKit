/*
 * Copyright (C) 2024 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#if ENABLE(WK_WEB_EXTENSIONS)

#include <wtf/CallbackAggregator.h>

namespace WebKit {

template<typename Result>
using DeferredWebExtensionReply = EagerCallbackAggregator<void(Result)>;

// Pending background tasks can be discarded. Retain a reply with an explicit
// fallback until the task starts, then resume its normal completion handler.
template<typename Result>
Ref<DeferredWebExtensionReply<Result>> deferWebExtensionReply(CompletionHandler<void(Result&&)>&& reply, std::type_identity_t<Result>&& fallback)
{
    return DeferredWebExtensionReply<Result>::create(WTF::move(reply), WTF::move(fallback));
}

template<typename Result>
CompletionHandler<void(Result&&)> resumeWebExtensionReply(Ref<DeferredWebExtensionReply<Result>>&& reply)
{
    return [reply = WTF::move(reply)](Result&& result) {
        reply.get()(WTF::move(result));
    };
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS)
