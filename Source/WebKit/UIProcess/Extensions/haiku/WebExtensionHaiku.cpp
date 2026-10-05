/*
 * Copyright (C) 2026 KunanyiOS contributors.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "WebExtension.h"

#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)

#include "WebErrors.h"
#include "WebExtensionArchiveHaiku.h"
#include "WebExtensionPackageSnapshotHaiku.h"
#include "WebExtensionResourcePathsHaiku.h"
#include "WebExtensionUtilities.h"
#include <WebCore/BitmapImage.h>
#include <WebCore/Chrome.h>
#include <WebCore/ChromeClient.h>
#include <WebCore/DataURLDecoder.h>
#include <WebCore/DocumentPage.h>
#include <WebCore/FrameLoader.h>
#include <WebCore/GraphicsContext.h>
#include <WebCore/ImageBuffer.h>
#include <WebCore/LoaderStrategy.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/LocalizedStrings.h>
#include <WebCore/NativeImage.h>
#include <WebCore/NetworkLoadMetrics.h>
#include <WebCore/Page.h>
#include <WebCore/PlatformStrategies.h>
#include <WebCore/ResourceError.h>
#include <WebCore/ResourceRequest.h>
#include <WebCore/ResourceResponse.h>
#include <WebCore/SVGImage.h>
#include <WebCore/SharedBuffer.h>
#include <WebCore/SubresourceLoader.h>
#include <cmath>
#include <wtf/FileSystem.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/RunLoop.h>
#include <wtf/text/MakeString.h>

namespace WebKit {

namespace {
using namespace WebCore;

// SVGImage creates a fully sandboxed utility Page, including a FrameLoader.
// The UI process has no WebProcess or network loader. Supply an image-only
// loader whose resources are decoded from inline data before rasterization.
// CachedResourceLoader also rejects external SVG image resources; enforce the
// same restriction here so no ResourceHandle can escape that image document.
class IconLoaderStrategyHaiku final : public LoaderStrategy {
public:
    static bool canLoad(LocalFrame& frame, const ResourceRequest& request)
    {
        RefPtr page = frame.page();
        return page && page->chrome().client().isSVGImageChromeClient() && request.url().protocolIsData();
    }

    void loadResource(LocalFrame& frame, CachedResource& resource, ResourceRequest&& request, const ResourceLoaderOptions& options, CompletionHandler<void(RefPtr<SubresourceLoader>&&)>&& completionHandler) final
    {
        if (!canLoad(frame, request))
            return completionHandler(nullptr);

        SubresourceLoader::create(frame, resource, WTF::move(request), options, [completionHandler = WTF::move(completionHandler)](RefPtr<SubresourceLoader>&& loader) mutable {
            RefPtr protectedLoader = loader;
            // CachedResource must own its loader before a synchronous response
            // changes its status from Pending to Cached or LoadError.
            completionHandler(WTF::move(loader));
            if (!protectedLoader || protectedLoader->reachedTerminalState())
                return;
            auto& request = protectedLoader->request();
            if (!request.url().protocolIsData()) {
                protectedLoader->didFail(WebKit::blockedError(request));
                return;
            }
            auto decoded = DataURLDecoder::decode(request.url());
            if (!decoded) {
                protectedLoader->didFail(WebKit::decodeError(request.url()));
                return;
            }
            auto response = ResourceResponse::dataURLResponse(request.url(), *decoded);
            bool isHead = request.httpMethod() == "HEAD"_s;
            Ref data = SharedBuffer::create(WTF::move(decoded->data));
            protectedLoader->didReceiveResponse(WTF::move(response), [loader = protectedLoader.releaseNonNull(), data = WTF::move(data), isHead] {
                if (!loader->reachedTerminalState() && data->size() && !isHead)
                    loader->didReceiveBuffer(data, data->size(), DataPayloadWholeResource);
                if (!loader->reachedTerminalState())
                    loader->didFinishLoading(NetworkLoadMetrics { });
            });
        });
    }

    void loadResourceSynchronously(FrameLoader& frameLoader, ResourceLoaderIdentifier, const ResourceRequest& request, ClientCredentialPolicy, const FetchOptions&, const HTTPHeaderMap&, ResourceError& error, ResourceResponse& response, Vector<uint8_t>& data) final
    {
        response = { };
        data.clear();
        if (!canLoad(frameLoader.frame(), request)) {
            error = WebKit::blockedError(request);
            return;
        }
        auto decoded = DataURLDecoder::decode(request.url());
        if (!decoded) {
            error = WebKit::decodeError(request.url());
            return;
        }
        error = { };
        response = ResourceResponse::dataURLResponse(request.url(), *decoded);
        data = WTF::move(decoded->data);
    }

    // Inline resources finish synchronously; there is no network state or
    // pending request queue to suspend, prioritize, redirect or drain.
    void pageLoadCompleted(Page&) final { }
    void browsingContextRemoved(LocalFrame&) final { }
    void remove(ResourceLoader*) final { }
    void setDefersLoading(ResourceLoader&, bool) final { }
    void crossOriginRedirectReceived(ResourceLoader*, const URL&) final { }
    void servePendingRequests(ResourceLoadPriority) final { }
    void suspendPendingRequests() final { }
    void resumePendingRequests() final { }
    void setCaptureExtraNetworkLoadMetricsEnabled(bool) final { }
    bool isOnLine() const final { return false; }
    void addOnlineStateChangeListener(Function<void(bool)>&&) final { }
    void isResourceLoadFinished(CachedResource& resource, CompletionHandler<void(bool)>&& completionHandler) final { completionHandler(!resource.loader()); }
    void preconnectTo(FrameLoader&, ResourceRequest&& request, StoredCredentialsPolicy, ShouldPreconnectAsFirstParty, PreconnectCompletionHandler&& completionHandler) final { completionHandler(WebKit::blockedError(request)); }

    ResourceError cancelledError(const ResourceRequest& request) const final { return WebKit::cancelledError(request); }
    ResourceError blockedError(const ResourceRequest& request) const final { return WebKit::blockedError(request); }
    bool isBlockedError(const ResourceError& error) const final { return WebKit::isBlockedError(error); }
    ResourceError blockedByContentBlockerError(const ResourceRequest& request) const final { return WebKit::blockedByContentBlockerError(request); }
    ResourceError cannotShowURLError(const ResourceRequest& request) const final { return WebKit::cannotShowURLError(request); }
    ResourceError interruptedForPolicyChangeError(const ResourceRequest& request) const final { return WebKit::interruptedForPolicyChangeError(request); }
#if ENABLE(CONTENT_FILTERING)
    ResourceError blockedByContentFilterError(const ResourceRequest& request) const final { return WebKit::blockedByContentFilterError(request); }
#endif
    ResourceError cannotShowMIMETypeError(const ResourceResponse& response) const final { return WebKit::cannotShowMIMETypeError(response); }
    ResourceError fileDoesNotExistError(const ResourceResponse& response) const final { return WebKit::fileDoesNotExistError(response); }
    ResourceError httpsUpgradeRedirectLoopError(const ResourceRequest& request) const final { return WebKit::httpsUpgradeRedirectLoopError(request); }
    ResourceError httpNavigationWithHTTPSOnlyError(const ResourceRequest& request) const final { return WebKit::httpNavigationWithHTTPSOnlyError(request); }
    bool isHttpNavigationWithHTTPSOnlyError(const ResourceError& error) const final { return WebKit::isHttpNavigationWithHTTPSOnlyError(error); }
    ResourceError pluginWillHandleLoadError(const ResourceResponse& response) const final { return WebKit::pluginWillHandleLoadError(response); }
};

class IconPlatformStrategiesHaiku final : public PlatformStrategies {
public:
    IconPlatformStrategiesHaiku() = default;

private:
    LoaderStrategy* createLoaderStrategy() final { return &m_loader; }
    // The isolated image document disables script and media, and never has
    // clipboard access or a blob URL store.
    PasteboardStrategy* createPasteboardStrategy() final { return nullptr; }
    MediaStrategy* createMediaStrategy() final { return nullptr; }
    BlobRegistry* createBlobRegistry() final { return nullptr; }
#if ENABLE(DECLARATIVE_WEB_PUSH)
    PushStrategy* createPushStrategy() final { return nullptr; }
#endif
    IconLoaderStrategyHaiku m_loader;
};

void ensureIconPlatformStrategies()
{
    ASSERT(RunLoop::isMain());
    if (hasPlatformStrategies())
        return;
    static NeverDestroyed<IconPlatformStrategiesHaiku> strategies;
    setPlatformStrategies(&strategies.get());
}
}

WebExtension::WebExtension(const String& resourcePath, RefPtr<API::Error>& outError)
    : m_manifestJSON(JSON::Value::null())
{
    outError = nullptr;
    auto snapshot = prepareWebExtensionPackageHaiku(resourcePath);
    if (!snapshot) {
        outError = createError(snapshot.error() == WebExtensionPackageErrorHaiku::InvalidArchive ? Error::InvalidArchive : Error::ResourceNotFound);
        recordErrorIfNeeded(outError);
        return;
    }
    initializeNativeResourceSnapshot(WTF::move(*snapshot), outError);
}

WebExtension::WebExtension(WebExtensionPackageSnapshotHaiku&& snapshot, RefPtr<API::Error>& outError)
    : m_manifestJSON(JSON::Value::null())
{
    outError = nullptr;
    initializeNativeResourceSnapshot(WTF::move(snapshot), outError);
}

void WebExtension::initializeNativeResourceSnapshot(WebExtensionPackageSnapshotHaiku&& snapshot, RefPtr<API::Error>& outError)
{
    m_resourceBaseURL = fileURLForWebExtensionPathHaiku(makeString(snapshot.path(), '/'));
    m_nativeResourceFingerprint = snapshot.fingerprint();
    m_nativeVerifiedCRXIdentifier = snapshot.verifiedCRXIdentifier();
    if (!manifestParsedSuccessfully()) {
        ASSERT(!m_errors.isEmpty());
        outError = m_errors.last().ptr();
        m_resourceBaseURL = { };
        m_nativeResourceFingerprint = { };
        m_nativeVerifiedCRXIdentifier = { };
        return;
    }
    bool persistent = snapshot.isPersistent();
    snapshot.release();
    m_resourcesAreTemporary = !persistent;
}

void WebExtension::initializeNativeResourceSnapshot()
{
    // API::Data can wrap caller-owned memory. Copy it before hashing so later
    // reads and lazy resource caches cannot diverge from the recorded package.
    for (auto& entry : m_dataResources)
        entry.value = API::Data::create(entry.value->span());
    auto manifest = m_manifestJSON->toJSONString();
    if (auto copy = JSON::Value::parseJSON(manifest))
        m_manifestJSON = copy.releaseNonNull();
    Vector<WebExtensionMemoryResourceHaiku> data;
    for (auto& entry : m_dataResources)
        data.append({ entry.key, entry.value->span() });
    Vector<UTF8CString> ownedStrings;
    ownedStrings.reserveInitialCapacity(m_stringResources.size());
    Vector<WebExtensionMemoryResourceHaiku> strings;
    for (auto& entry : m_stringResources) {
        ownedStrings.append(entry.value.utf8());
        strings.append({ entry.key, byteCast<uint8_t>(ownedStrings.last().span()) });
    }
    m_nativeResourceFingerprint = fingerprintWebExtensionMemoryResourcesHaiku(manifest, WTF::move(data), WTF::move(strings));
}

Vector<double> availableScreenScales()
{
    // Haiku WebPage configurations currently use one backing pixel per CSS
    // pixel. Keep icon representation selection consistent with that scale.
    return { 1.0 };
}

static bool validIconSize(const WebCore::FloatSize& size)
{
    return std::isfinite(size.width()) && std::isfinite(size.height()) && !size.isEmpty();
}

std::expected<Ref<WebCore::Icon>, RefPtr<API::Error>> WebExtension::iconForPath(const String& path, WebCore::FloatSize sizeForResizing, std::optional<double> idealDisplayScale)
{
    auto fail = [&] {
        return makeUnexpected(createError(Error::InvalidIcon, WEB_UI_STRING("The extension icon could not be decoded.", "WKWebExtensionErrorInvalidIcon description for undecodable image data")));
    };
    auto scale = idealDisplayScale.value_or(largestDisplayScale());
    if (path.isEmpty() || !std::isfinite(scale) || scale <= 0 || (!sizeForResizing.isZero() && !validIconSize(sizeForResizing)))
        return fail();

    auto dataResult = resourceDataForPath(path);
    if (!dataResult)
        return makeUnexpected(dataResult.error());
    Ref data = dataResult.value();
    if (!data->size())
        return fail();

    Ref encodedData = WebCore::SharedBuffer::create(data->span());
    RefPtr<WebCore::NativeImage> nativeImage;
    if (equalLettersIgnoringASCIICase(resourceMIMETypeForPath(path), "image/svg+xml"_s)) {
        ensureIconPlatformStrategies();
        Ref svg = WebCore::SVGImage::create(nullptr);
        if (svg->setData(WTF::move(encodedData), true) != WebCore::EncodedDataStatus::Complete)
            return fail();
        // SVGImage reports Complete after creating its utility Page, even if
        // parsing failed or produced another XML document type. An empty but
        // valid SVG is an icon; an XML error document must not replace one.
        RefPtr page = svg->internalPage();
        RefPtr document = page ? page->localTopDocument() : nullptr;
        if (!document || !document->wellFormed() || !document->hasSVGRootNode())
            return fail();
        auto rasterSize = sizeForResizing.isZero() ? svg->resolvedIntrinsicSize() : sizeForResizing;
        rasterSize.scale(scale);
        if (!validIconSize(rasterSize))
            return fail();
        // SVGImage uses an isolated image document with scripting disabled.
        nativeImage = svg->nativeImage(rasterSize);
    } else {
        Ref bitmap = WebCore::BitmapImage::create();
        if (bitmap->setData(WTF::move(encodedData), true) < WebCore::EncodedDataStatus::SizeAvailable || !validIconSize(bitmap->size()))
            return fail();
        if (sizeForResizing.isZero())
            nativeImage = static_cast<WebCore::Image&>(bitmap.get()).currentPreTransformedNativeImage(WebCore::ImageOrientation::Orientation::FromImage);
        else {
            auto originalSize = bitmap->size();
            auto ratio = std::min(sizeForResizing.width() / originalSize.width(), sizeForResizing.height() / originalSize.height()) * scale;
            auto rasterSize = originalSize;
            rasterSize.scale(ratio);
            if (!validIconSize(rasterSize))
                return fail();
            RefPtr imageBuffer = WebCore::ImageBuffer::create(rasterSize, WebCore::RenderingMode::Unaccelerated, WebCore::RenderingPurpose::DOM, 1, WebCore::ColorSpace::SRGB(), WebCore::PixelFormat::BGRA8);
            if (!imageBuffer)
                return fail();
            imageBuffer->context().drawImage(bitmap, WebCore::FloatRect({ }, rasterSize));
            nativeImage = WebCore::ImageBuffer::sinkIntoNativeImage(WTF::move(imageBuffer));
        }
    }

    if (RefPtr icon = WebCore::Icon::create(WTF::move(nativeImage)))
        return icon.releaseNonNull();
    return fail();
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
