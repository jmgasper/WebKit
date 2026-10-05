/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

// KunanyiOS has no platform authenticator: no Touch ID equivalent, no CTAP
// transport for USB or NFC security keys. The Web Authentication API is still
// built, because password managers extend it from their content scripts, and
// they can only do that when the page really exposes `navigator.credentials`
// and `PublicKeyCredential`. Those extensions answer the page themselves and
// never reach this proxy; a request that does reach it has no authenticator to
// satisfy it and is declined the way a browser without one declines it.

#include "config.h"
#include "WebAuthenticatorCoordinatorProxy.h"

#if ENABLE(WEB_AUTHN) && PLATFORM(HAIKU)

#include "WebAuthenticatorCoordinatorProxyMessages.h"
#include "WebPageProxy.h"
#include "WebProcessProxy.h"
#include <WebCore/AllAcceptedCredentialsOptions.h>
#include <WebCore/AuthenticatorAttachment.h>
#include <WebCore/AuthenticatorResponseData.h>
#include <WebCore/CurrentUserDetailsOptions.h>
#include <WebCore/ExceptionData.h>
#include <WebCore/PublicKeyCredentialCreationOptions.h>
#include <WebCore/PublicKeyCredentialRequestOptions.h>
#include <WebCore/SecurityOriginData.h>
#include <WebCore/UnknownCredentialOptions.h>
#include <wtf/TZoneMallocInlines.h>

namespace WebKit {
using namespace WebCore;

WTF_MAKE_TZONE_ALLOCATED_IMPL(WebAuthenticatorCoordinatorProxy);

Ref<WebAuthenticatorCoordinatorProxy> WebAuthenticatorCoordinatorProxy::create(WebPageProxy& webPageProxy)
{
    return adoptRef(*new WebAuthenticatorCoordinatorProxy(webPageProxy));
}

WebAuthenticatorCoordinatorProxy::WebAuthenticatorCoordinatorProxy(WebPageProxy& webPageProxy)
    : m_webPageProxy(webPageProxy)
{
    protect(webPageProxy.legacyMainFrameProcess())->addMessageReceiver(Messages::WebAuthenticatorCoordinatorProxy::messageReceiverName(), webPageProxy.webPageIDInMainFrameProcess(), *this);
}

WebAuthenticatorCoordinatorProxy::~WebAuthenticatorCoordinatorProxy()
{
    if (RefPtr webPageProxy = m_webPageProxy.get())
        protect(webPageProxy->legacyMainFrameProcess())->removeMessageReceiver(Messages::WebAuthenticatorCoordinatorProxy::messageReceiverName(), webPageProxy->webPageIDInMainFrameProcess());
}

std::optional<SharedPreferencesForWebProcess> WebAuthenticatorCoordinatorProxy::sharedPreferencesForWebProcess() const
{
    auto* webPageProxy = m_webPageProxy.get();
    return webPageProxy ? webPageProxy->legacyMainFrameProcess().sharedPreferencesForWebProcess() : std::nullopt;
}

// The spec asks for NotAllowedError whenever no authenticator can serve a
// request, so a page cannot tell from the error which authenticators exist.
static ExceptionData noAuthenticatorAvailable()
{
    return ExceptionData { ExceptionCode::NotAllowedError, "No authenticator is available on this device."_s };
}

void WebAuthenticatorCoordinatorProxy::makeCredential(IPC::Connection&, FrameIdentifier, FrameInfoData&&, PublicKeyCredentialCreationOptions&&, MediationRequirement, RequestCompletionHandler&& handler)
{
    handler({ }, AuthenticatorAttachment::Platform, noAuthenticatorAvailable());
}

void WebAuthenticatorCoordinatorProxy::getAssertion(IPC::Connection&, FrameIdentifier, FrameInfoData&&, PublicKeyCredentialRequestOptions&&, MediationRequirement, IPC::Untrusted<std::optional<SecurityOriginData>>&&, RequestCompletionHandler&& handler)
{
    handler({ }, AuthenticatorAttachment::Platform, noAuthenticatorAvailable());
}

void WebAuthenticatorCoordinatorProxy::isUserVerifyingPlatformAuthenticatorAvailable(IPC::Untrusted<SecurityOriginData>&&, QueryCompletionHandler&& handler)
{
    handler(false);
}

void WebAuthenticatorCoordinatorProxy::isConditionalMediationAvailable(IPC::Untrusted<SecurityOriginData>&&, QueryCompletionHandler&& handler)
{
    handler(false);
}

void WebAuthenticatorCoordinatorProxy::getClientCapabilities(IPC::Untrusted<SecurityOriginData>&&, CapabilitiesCompletionHandler&& handler)
{
    // Report the capabilities this client understands, all unsupported, rather
    // than an empty map: a page can then tell a browser that knows about a
    // capability and lacks it from one that has never heard of it.
    Vector<KeyValuePair<String, bool>> capabilities {
        { "conditionalCreate"_s, false },
        { "conditionalGet"_s, false },
        { "hybridTransport"_s, false },
        { "passkeyPlatformAuthenticator"_s, false },
        { "userVerifyingPlatformAuthenticator"_s, false },
        { "relatedOrigins"_s, false },
        { "signalAllAcceptedCredentials"_s, false },
        { "signalCurrentUserDetails"_s, false },
        { "signalUnknownCredential"_s, false },
    };
    handler(WTF::move(capabilities));
}

// The signal methods keep a platform credential store in step with the relying
// party. There is no such store here, so they report success without doing
// anything, which is what the spec asks of a client that stores no credentials.
void WebAuthenticatorCoordinatorProxy::signalUnknownCredential(IPC::Untrusted<SecurityOriginData>&&, UnknownCredentialOptions&&, CompletionHandler<void(std::optional<ExceptionData>)>&& handler)
{
    handler(std::nullopt);
}

void WebAuthenticatorCoordinatorProxy::signalAllAcceptedCredentials(IPC::Untrusted<SecurityOriginData>&&, AllAcceptedCredentialsOptions&&, CompletionHandler<void(std::optional<ExceptionData>)>&& handler)
{
    handler(std::nullopt);
}

void WebAuthenticatorCoordinatorProxy::signalCurrentUserDetails(IPC::Untrusted<SecurityOriginData>&&, CurrentUserDetailsOptions&&, CompletionHandler<void(std::optional<ExceptionData>)>&& handler)
{
    handler(std::nullopt);
}

void WebAuthenticatorCoordinatorProxy::cancel(CompletionHandler<void()>&& handler)
{
    handler();
}

} // namespace WebKit

#endif // ENABLE(WEB_AUTHN) && PLATFORM(HAIKU)
