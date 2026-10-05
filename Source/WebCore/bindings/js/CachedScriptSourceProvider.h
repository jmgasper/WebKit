/*
 * Copyright (C) 2008-2018 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#pragma once

#include "CachedResourceClient.h"
#include "CachedResourceHandle.h"
#include "CachedScript.h"
#include "CachedScriptFetcher.h"
#include <JavaScriptCore/SourceProvider.h>

namespace WebCore {

class CachedScriptSourceProvider final : public JSC::SourceProvider, public CachedResourceClient {
    WTF_MAKE_TZONE_ALLOCATED(CachedScriptSourceProvider);
public:
    static Ref<CachedScriptSourceProvider> create(CachedScript* cachedScript, JSC::SourceProviderSourceType sourceType, Ref<CachedScriptFetcher>&& scriptFetcher) { return adoptRef(*new CachedScriptSourceProvider(cachedScript, sourceType, WTF::move(scriptFetcher))); }

    virtual ~CachedScriptSourceProvider()
    {
#if PLATFORM(HAIKU)
        discardPendingBytecodeHaiku();
#endif
        protect(m_cachedScript)->removeClient(*this);
    }

    // CachedResourceClient.
    void ref() const final { JSC::SourceProvider::ref(); }
    void deref() const final { JSC::SourceProvider::deref(); }

    unsigned hash() const final;
    StringView source() const final;

    JSC::CodeBlockHash codeBlockHashConcurrently(int startOffset, int endOffset, JSC::CodeSpecializationKind kind) override
    {
        // We cannot protect m_cachedScript here since this function gets called on the GC thread.
        SUPPRESS_UNCOUNTED_ARG return m_cachedScript->codeBlockHashConcurrently(startOffset, endOffset, kind, isModuleType() ? CachedScript::ShouldDecodeAsUTF8Only::Yes : CachedScript::ShouldDecodeAsUTF8Only::No);
    }

#if PLATFORM(HAIKU)
    // Compiled code kept on disk between visits (CachedScriptSourceProviderHaiku.cpp).
    RefPtr<JSC::CachedBytecode> cachedBytecode() const final;
    void cacheBytecode(const JSC::BytecodeCacheGenerator&) const final;
    void updateCache(const JSC::UnlinkedFunctionExecutable*, const JSC::SourceCode&, JSC::CodeSpecializationKind, const JSC::UnlinkedFunctionCodeBlock*) const final;
    void commitCachedBytecode() const final;
#endif

private:
#if PLATFORM(HAIKU)
    struct PendingFunctionsHaiku;
    String bytecodeCachePathHaiku() const;
    void scheduleBytecodeCommitHaiku() const;
    bool encodePendingFunctionsHaiku(double budgetMilliseconds) const;
    void commitStepHaiku() const;
    void discardPendingBytecodeHaiku();

    mutable RefPtr<JSC::CachedBytecode> m_cachedBytecodeHaiku;
    // Functions compiled since, encoded when the file is written.
    mutable PendingFunctionsHaiku* m_pendingFunctionsHaiku { nullptr };
    mutable double m_encodeMillisecondsHaiku { 0 };
    mutable size_t m_cachedBytecodeFileSizeHaiku { 0 };
    mutable bool m_triedBytecodeLoadHaiku { false };
    mutable bool m_bytecodeCommitScheduledHaiku { false };
#endif

    CachedScriptSourceProvider(CachedScript* cachedScript, JSC::SourceProviderSourceType sourceType, Ref<CachedScriptFetcher>&& scriptFetcher)
        : SourceProvider(JSC::SourceOrigin { cachedScript->response().url(), WTF::move(scriptFetcher) }, String(cachedScript->response().url().string()), cachedScript->response().isRedirected() ? String(cachedScript->url().string()) : String(), cachedScript->requiresPrivacyProtections() ? JSC::SourceTaintedOrigin::KnownTainted : JSC::SourceTaintedOrigin::Untainted, TextPosition(), sourceType)
        , m_cachedScript(cachedScript)
    {
        cachedScript->addClient(*this);
    }

    const CachedResourceHandle<CachedScript> m_cachedScript;
};

inline unsigned CachedScriptSourceProvider::hash() const
{
    // Modules should always be decoded as UTF-8.
    // https://html.spec.whatwg.org/multipage/webappapis.html#fetch-a-single-module-script
    return protect(m_cachedScript)->scriptHash(isModuleType() ? CachedScript::ShouldDecodeAsUTF8Only::Yes : CachedScript::ShouldDecodeAsUTF8Only::No);
}

inline StringView CachedScriptSourceProvider::source() const
{
    // Modules should always be decoded as UTF-8.
    // https://html.spec.whatwg.org/multipage/webappapis.html#fetch-a-single-module-script
    return protect(m_cachedScript)->script(isModuleType() ? CachedScript::ShouldDecodeAsUTF8Only::Yes : CachedScript::ShouldDecodeAsUTF8Only::No);
}

} // namespace WebCore
