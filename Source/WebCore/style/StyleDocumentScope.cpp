/*
 * Copyright (C) 1999 Lars Knoll (knoll@kde.org)
 *           (C) 1999 Antti Koivisto (koivisto@kde.org)
 *           (C) 2001 Dirk Mueller (mueller@kde.org)
 *           (C) 2006 Alexey Proskuryakov (ap@webkit.org)
 * Copyright (C) 2004-2026 Apple Inc. All rights reserved.
 * Copyright (C) 2008, 2009 Torch Mobile Inc. All rights reserved. (http://www.torchmobile.com/)
 * Copyright (C) 2008, 2009, 2011, 2012 Google Inc. All rights reserved.
 * Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies)
 * Copyright (C) Research In Motion Limited 2010-2011. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "config.h"
#include "StyleDocumentScope.h"

#include "CSSCounterStyleRegistry.h"
#include "CSSFontSelector.h"
#include "CSSKeyframesRule.h"
#include "CSSStyleSheet.h"
#include "ContainerNodeInlines.h"
#include "DocumentInlines.h"
#include "DocumentView.h"
#include "Element.h"
#include "ElementAncestorIteratorInlines.h"
#include "ElementChildIteratorInlines.h"
#include "ElementRareData.h"
#include "InspectorInstrumentation.h"
#include "MatchResultCache.h"
#include "RenderBoxInlines.h"
#include "RenderElementStyleInlines.h"
#include "RenderLayer.h"
#include "RenderObjectInlines.h"
#include "RenderView.h"
#include "RuleSet.h"
#include "ShadowRoot.h"
#include "StyleableInlines.h"
#include "StyleCustomPropertyRegistry.h"
#include "StyleEnvironmentVariables.h"
#include "StyleInvalidator.h"
#include "StyleResolver.h"
#include "StyleRule.h"
#include "StyleSheetContents.h"
#include "CSSSerializationContext.h"
#include "CSSValuePool.h"
#include "HTMLNames.h"
#include "StyleExtractor.h"
#include "StyleExtractorState.h"
#include "TypedElementDescendantIteratorInlines.h"
#include <wtf/SetForScope.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/TZoneMallocInlines.h>

namespace WebCore {

namespace Style {

WTF_MAKE_TZONE_ALLOCATED_IMPL(DocumentScope);

DocumentScope::DocumentScope(Document& document)
    : Scope(document)
{
}

DocumentScope::~DocumentScope() = default;

void DocumentScope::createDocumentResolver()
{
    ASSERT(!m_resolver);
    ASSERT(!m_shadowRoot);

    SetForScope isUpdatingStyleResolver { m_isUpdatingStyleResolver, true };

    m_resolver = Resolver::create(m_document, Resolver::ScopeType::Document);

    if (!m_dynamicViewTransitionsStyle)
        m_dynamicViewTransitionsStyle = RuleSet::create();

    m_resolver->ruleSets().setDynamicViewTransitionsStyle(m_dynamicViewTransitionsStyle.get());

    for (auto& keyframes : m_viewTransitionKeyframes)
        m_resolver->addKeyframeStyle(keyframes.copyRef());

    protect(m_document->fontSelector())->buildStarted();

    m_resolver->ruleSets().initializeUserStyle();
    m_resolver->addCurrentSVGFontFaceRules();
    m_resolver->appendAuthorStyleSheets(m_activeStyleSheets);

    protect(m_document->fontSelector())->buildCompleted();
}

void DocumentScope::clearViewTransitionStyles()
{
    clearResolver();
    m_dynamicViewTransitionsStyle = nullptr;
    m_viewTransitionKeyframes.clear();
}

void DocumentScope::addViewTransitionKeyframes(Ref<StyleRuleKeyframes>&& rule)
{
    // Add to the existing resolver, if any, and retain the keyframes so they can be
    // re-established when the resolver is recreated (e.g. by a stylesheet mutation)
    // during the transition.
    if (RefPtr resolver = resolverIfExists())
        resolver->addKeyframeStyle(rule.copyRef());
    m_viewTransitionKeyframes.append(WTF::move(rule));
}

void DocumentScope::releaseMemory()
{
    for (auto& descendantShadowRoot : m_document->inDocumentShadowRoots())
        const_cast<ShadowRoot&>(descendantShadowRoot).styleScope().releaseMemory();

    Scope::releaseMemory();

    m_sharedShadowTreeResolvers.clear();
    m_matchResultCache = { };
}

void DocumentScope::setPreferredStylesheetSetName(const WTF::String& name)
{
    if (m_preferredStylesheetSetName == name)
        return;
    m_preferredStylesheetSetName = name;
    didChangeActiveStyleSheetCandidates();
}

auto DocumentScope::mediaQueryViewportStateForDocument(const Document& document) -> MediaQueryViewportState
{
    // These things affect evaluation of viewport dependent media queries.
    return { document.view()->layoutSize(), document.frame()->pageZoomFactor(), document.printing() };
}

void DocumentScope::evaluateMediaQueriesForViewportChange()
{
    auto viewportState = mediaQueryViewportStateForDocument(m_document);

    if (m_viewportStateOnPreviousMediaQueryEvaluation && *m_viewportStateOnPreviousMediaQueryEvaluation == viewportState)
        return;
    // This doesn't need to be invalidated as any changes to the rules will compute their media queries to correct values.
    m_viewportStateOnPreviousMediaQueryEvaluation = viewportState;

    evaluateMediaQueries([] (Resolver& resolver) {
        return resolver.evaluateDynamicMediaQueries();
    });
}

void DocumentScope::evaluateMediaQueriesForAccessibilitySettingsChange()
{
    evaluateMediaQueries([] (Resolver& resolver) {
        return resolver.evaluateDynamicMediaQueries();
    });
}

void DocumentScope::evaluateMediaQueriesForAppearanceChange()
{
    evaluateMediaQueries([] (Resolver& resolver) {
        return resolver.evaluateDynamicMediaQueries();
    });
}

auto DocumentScope::collectResolverScopes() -> ResolverScopes
{
    ResolverScopes resolverScopes;

    if (RefPtr resolver = resolverIfExists())
        resolverScopes.add(*resolver, Vector<WeakPtr<Scope>> { this });

    for (Ref shadowRoot : m_document->inDocumentShadowRoots()) {
        auto& scope = const_cast<ShadowRoot&>(shadowRoot.get()).styleScope();

        if (RefPtr resolver = scope.resolverIfExists())
            resolverScopes.add(*resolver, Vector<WeakPtr<Scope>> { }).iterator->value.append(&scope);
    }
    return resolverScopes;
}

template <typename TestFunction>
void DocumentScope::evaluateMediaQueries(TestFunction&& testFunction)
{
    bool hadChanges = false;

    auto resolverScopes = collectResolverScopes();
    for (auto& [resolver, scopes] : resolverScopes) {
        auto evaluationChanges = testFunction(resolver.get());
        if (!evaluationChanges)
            continue;
        hadChanges = true;

        for (auto& scope : scopes) {
            switch (evaluationChanges->type) {
            case DynamicMediaQueryEvaluationChanges::Type::InvalidateStyle: {
                Invalidator invalidator(evaluationChanges->invalidationRuleSets);
                invalidator.invalidateStyle(*scope);
                break;
            }
            case DynamicMediaQueryEvaluationChanges::Type::ResetStyle:
                scope->scheduleUpdate(UpdateType::ContentsOrInterpretation);
                break;
            }
        }
    }

    if (hadChanges)
        InspectorInstrumentation::mediaQueryResultChanged(m_document);
}

void DocumentScope::didChangeStyleSheetEnvironment()
{
    RELEASE_ASSERT(!m_isUpdatingStyleResolver);
    RELEASE_ASSERT(!m_document->isResolvingTreeStyle());

    m_sharedShadowTreeResolvers.clear();

    for (auto& descendantShadowRoot : m_document->inDocumentShadowRoots())
        const_cast<ShadowRoot&>(descendantShadowRoot).styleScope().scheduleUpdate(UpdateType::ContentsOrInterpretation);

    m_document->invalidateCachedCSSParserContext();
    m_document->invalidateCachedInitialStyle();

    scheduleUpdate(UpdateType::ContentsOrInterpretation);
}

#if PLATFORM(HAIKU)
} // namespace Style

// A style sheet an extension adds to a page (tabs.insertCSS(); uBlock Origin
// adds its cosmetic filters this way, four to six times while a page loads)
// used to throw away every style resolver of the document, those of its
// shadow trees included, and have the style of every element worked out
// again: 80 to 100 ms a time on YouTube and Reddit. When the sheet only holds
// style rules (and keyframes) and comes last among the page's injected sheets, its rules are
// added to the resolver in place and only the elements its selectors can match
// are restyled, as for a style sheet the page itself adds.
// SUMMIT_INCREMENTAL_EXTENSION_CSS=0 goes the old way; =verify then also goes
// the old way and reports every element whose style came out different.
// Returns false when the caller has to go the old way.
static bool summitAddExtensionStyleSheetToResolvers(Document&, CSSStyleSheet&);

static void summitForEachElementWithStyle(Document& document, NOESCAPE const Function<void(Element&, const Style::ComputedStyle&)>& function)
{
    auto visit = [&](ContainerNode& root) {
        for (Ref element : descendantsOfType<Element>(root)) {
            if (auto* style = element->existingComputedStyle())
                function(element, *style);
        }
    };
    visit(document);
    for (auto& shadowRoot : document.inDocumentShadowRoots())
        visit(const_cast<ShadowRoot&>(shadowRoot));
}

bool summitAddExtensionStyleSheetIncrementally(Document& document, CSSStyleSheet& sheet)
{
    static const char* mode = std::getenv("SUMMIT_INCREMENTAL_EXTENSION_CSS");
    if (mode && !strcmp(mode, "0"))
        return false;
    if (!summitAddExtensionStyleSheetToResolvers(document, sheet))
        return false;
    if (!mode || strcmp(mode, "verify"))
        return true;

    // Styles as wholes compare unequal after everything was resolved again (their fonts refer to
    // a newer font selector), so the values of the properties that style sheets of extensions set are compared.
    static constexpr CSSPropertyID properties[] = { CSSPropertyDisplay, CSSPropertyVisibility, CSSPropertyOpacity, CSSPropertyPosition,
        CSSPropertyFloat, CSSPropertyColor, CSSPropertyBackgroundColor, CSSPropertyWidth, CSSPropertyHeight, CSSPropertyMaxHeight,
        CSSPropertyOverflowX, CSSPropertyPointerEvents, CSSPropertyFontSize, CSSPropertyZIndex, CSSPropertyTop, CSSPropertyLeft,
        CSSPropertyMarginTop, CSSPropertyPaddingTop, CSSPropertyBorderTopWidth, CSSPropertyTransform, CSSPropertyFilter };
    auto valuesOf = [&](Element& element, const Style::ComputedStyle& style) {
        Style::Extractor extractor(&element);
        StringBuilder values;
        for (auto property : properties)
            values.append(extractor.propertyValueSerializationInStyle(style, property, CSS::defaultSerializationContext(), CSSValuePool::singleton(), nullptr, Style::ExtractorState::PropertyValueType::Computed), ';');
        return values.toString();
    };

    Ref protectedDocument = document;
    document.updateStyleIfNeeded();
    HashMap<Ref<Element>, String> incrementalStyles;
    summitForEachElementWithStyle(document, [&](Element& element, const Style::ComputedStyle& style) {
        incrementalStyles.add(element, valuesOf(element, style));
    });
    document.styleScope().didChangeExtensionStyleSheets();
    document.updateStyleIfNeeded();
    unsigned elements = 0;
    unsigned different = 0;
    summitForEachElementWithStyle(document, [&](Element& element, const Style::ComputedStyle& style) {
        ++elements;
        auto incremental = incrementalStyles.find(element);
        if (incremental == incrementalStyles.end())
            return;
        auto values = valuesOf(element, style);
        if (incremental->value == values)
            return;
        if (++different <= 5) {
            WTFLogAlways("Summit extension css MISMATCH: <%s class=\"%s\"> incremental %s full %s\n", element.localName().string().utf8().legacyCStringPointer(),
                element.getAttribute(HTMLNames::classAttr).string().left(60).utf8().legacyCStringPointer(),
                incremental->value.utf8().legacyCStringPointer(), values.utf8().legacyCStringPointer());
        }
    });
    WTFLogAlways("Summit extension css: verified %u elements (%u had style before), %u differ, sheet of %u rules\n", elements, incrementalStyles.size(), different,
        static_cast<unsigned>(sheet.contents().childRules().size()));
    return true;
}

static bool summitAddExtensionStyleSheetToResolvers(Document& document, CSSStyleSheet& sheet)
{
    // SUMMIT_EXTENSION_CSS_TRACE=1 (also read by the browser) reports which way each sheet went.
    static bool trace = [] {
        const char* value = std::getenv("SUMMIT_EXTENSION_CSS_TRACE");
        return value && value[0] == '1' && !value[1];
    }();
    auto oldWay = [&](ASCIILiteral reason) {
        if (trace)
            WTFLogAlways("Summit extension css: sheet of %u rules restyles everything: %s\n", static_cast<unsigned>(sheet.contents().childRules().size()), reason.characters());
        return false;
    };
    if (!sheet.contents().isUserStyleSheet())
        return oldWay("not a user-level sheet"_s);
    if (document.inStyleRecalc() || document.inRenderTreeUpdate())
        return oldWay("styles are being resolved"_s);

    // Font faces, properties, layers and imports are registered with more than the rule set.
    Ref contents = sheet.contents();
    if (!contents->importRules().isEmpty() || !contents->layerRulesBeforeImportRules().isEmpty() || !contents->namespaceRules().isEmpty())
        return oldWay("imports, layers or namespaces"_s);
    unsigned otherRuleType = 0;
    Function<bool(const Vector<Ref<StyleRuleBase>>&)> onlyStyleRules = [&](const Vector<Ref<StyleRuleBase>>& rules) {
        for (auto& rule : rules) {
            // Keyframes go to the resolver by name as the user rules are built again below. An element
            // that already named an animation the sheet now defines (not one of the sheet's own
            // elements, which are restyled) starts it at its next restyle.
            if (rule->isStyleRule() || rule->type() == StyleRuleType::Keyframes)
                continue;
            // Rules inside @media and @supports are added to the rule set as they are at the top level.
            if (rule->type() == StyleRuleType::Media || rule->type() == StyleRuleType::Supports) {
                if (onlyStyleRules(downcast<StyleRuleGroup>(rule.get()).childRules()))
                    continue;
                return false;
            }
            otherRuleType = static_cast<unsigned>(rule->type());
            return false;
        }
        return true;
    };
    if (!onlyStyleRules(contents->childRules())) {
        if (trace)
            WTFLogAlways("Summit extension css: a rule of type %u\n", otherRuleType);
        return oldWay("rules other than style rules"_s);
    }

    auto& documentScope = document.styleScope();
    // An update that is already due may keep the resolver and would not look at the injected sheets
    // again: it is done first.
    documentScope.flushPendingUpdate();
    if (documentScope.hasPendingUpdate())
        return oldWay("a style sheet update is pending"_s);
    RefPtr resolver = documentScope.resolverIfExists();
    if (!resolver) {
        // Nothing was built from the injected sheets yet; the resolver will read them all.
        if (trace)
            WTFLogAlways("Summit extension css: sheet of %u rules waits for the first resolver\n", static_cast<unsigned>(contents->childRules().size()));
        return true;
    }

    auto updateFeatures = [](Style::Resolver& resolver) {
        // Collects the features of all the resolver's rule sets again, the user rules among them.
        resolver.appendAuthorStyleSheets({ });
        resolver.invalidateMatchedDeclarationsCache();
    };
    resolver->ruleSets().initializeUserStyle();
    updateFeatures(*resolver);

    Style::Invalidator invalidator({ contents }, resolver->mediaQueryEvaluator());
    bool invalidateAll = !document.bodyOrFrameset() || document.hasNodesWithMissingStyle();
    auto invalidate = [&](Style::Scope& scope) {
        if (invalidateAll)
            Style::Invalidator::invalidateAllStyle(scope);
        else
            invalidator.invalidateStyle(scope);
    };
    invalidate(documentScope);

    // Shadow trees take the document's user rules (ScopeRuleSets::userStyle()); the trees of
    // built-in controls take none.
    HashSet<Style::Resolver*> updatedResolvers;
    unsigned shadowTrees = 0;
    for (auto& shadowRoot : document.inDocumentShadowRoots()) {
        if (shadowRoot.mode() == ShadowRootMode::UserAgent)
            continue;
        ++shadowTrees;
        auto& scope = const_cast<ShadowRoot&>(shadowRoot).styleScope();
        if (RefPtr shadowResolver = scope.resolverIfExists(); shadowResolver && updatedResolvers.add(shadowResolver.get()).isNewEntry)
            updateFeatures(*shadowResolver);
        invalidate(scope);
    }
    if (trace) {
        WTFLogAlways("Summit extension css: sheet of %u rules added in place: %s, %u shadow trees with %u resolvers\n", static_cast<unsigned>(contents->childRules().size()),
            invalidateAll ? "the document is not ready, everything restyled" : invalidator.dirtiesAllStyle() ? "its selectors can match anything, everything restyled" : "matching elements restyled",
            shadowTrees, updatedResolvers.size());
    }
    return true;
}

namespace Style {
#endif

void DocumentScope::didChangeExtensionStyleSheets()
{
    // Extension stylesheets may mutate in the middle of a style update when resource loading triggers
    // content extension processing. In this case we schedule an asyncronous full stylesheet update.
    // FIXME: We should defer all resource loading after style resolution completes.
    for (auto& descendantShadowRoot : m_document->inDocumentShadowRoots())
        const_cast<ShadowRoot&>(descendantShadowRoot).styleScope().scheduleUpdate(UpdateType::FullForExtensionStyleSheets);

    scheduleUpdate(UpdateType::FullForExtensionStyleSheets);
}

bool DocumentScope::invalidateForLayoutDependencies(LayoutDependencyUpdateContext& context)
{
    auto didInvalidate = false;
    didInvalidate |= invalidateForContainerDependencies(context);
    didInvalidate |= invalidateForAnchorDependencies(context);
    didInvalidate |= invalidateForPositionTryFallbacks(context);
    return didInvalidate;
}

bool DocumentScope::invalidateForContainerDependencies(LayoutDependencyUpdateContext& context)
{
    if (!m_document->renderView())
        return false;

    auto previousQueryContainerDimensions = WTF::move(m_queryContainerDimensionsOnLastUpdate);
    m_queryContainerDimensionsOnLastUpdate.clear();

    Vector<CheckedPtr<Element>> containersToInvalidate;

    for (auto& containerRenderer : m_document->renderView()->containerQueryBoxes()) {
        CheckedPtr containerElement = containerRenderer.element();

        // Invalidation uses real elements, replace ::before/::after with its host.
        if (auto* pseudoElement = dynamicDowncast<PseudoElement>(containerElement.get()))
            containerElement = pseudoElement->hostElement();

        if (!containerElement)
            continue;

        auto size = containerRenderer.logicalSize();

        auto sizeChanged = [&](LayoutSize oldSize) {
            auto& type = containerRenderer.style().containerType();
            if (type.hasInlineSize())
                return size.width() != oldSize.width();
            if (type.hasSize())
                return size != oldSize;
            RELEASE_ASSERT_NOT_REACHED();
        };

        auto it = previousQueryContainerDimensions.find(*containerElement);
        bool changed = it == previousQueryContainerDimensions.end() || sizeChanged(it->value);
        // Protect against unstable layout by invalidating only once per container.
        if (changed && context.invalidatedContainers.add(*containerElement).isNewEntry)
            containersToInvalidate.append(containerElement);
        m_queryContainerDimensionsOnLastUpdate.add(*containerElement, size);
    }

    for (auto& toInvalidate : containersToInvalidate)
        toInvalidate->invalidateForQueryContainerSizeChange();

    return !containersToInvalidate.isEmpty();
}

bool DocumentScope::invalidateForAnchorDependencies(LayoutDependencyUpdateContext& context)
{
    if (!m_document->renderView())
        return false;

    auto previousAnchorPositions = WTF::move(m_anchorPositionsOnLastUpdate);
    m_anchorPositionsOnLastUpdate.clear();

    Vector<CheckedRef<Element>> anchoredElementsToInvalidate;

    if (m_document->renderView()->anchors().isEmptyIgnoringNullReferences())
        return false;

    auto anchorMap = AnchorPositionEvaluator::makeAnchorPositionedForAnchorMap(m_anchorPositionedToAnchorMap);

    auto makeAnchorPosition = [&](const RenderBoxModelObject& anchorRenderer) {
        AnchorPosition result;
        result.absoluteRect = anchorRenderer.absoluteBoundingBoxRect();
        // Include containing block sizes as anchor function insets may be computed against any side and if they change
        // we need to invalidate.
        for (auto* containingBlock = anchorRenderer.containingBlock(); containingBlock; containingBlock = containingBlock->containingBlock()) {
            if (containingBlock->canContainAbsolutelyPositionedObjects())
                result.containingBlockSizes.append(containingBlock->contentBoxSize());
        }
        return result;
    };

    for (auto& anchorRenderer : m_document->renderView()->anchors()) {
        auto anchorPosition = makeAnchorPosition(anchorRenderer);
        m_anchorPositionsOnLastUpdate.add(anchorRenderer, anchorPosition);

        auto it = previousAnchorPositions.find(anchorRenderer);
        bool changed = it == previousAnchorPositions.end() || it->value != anchorPosition;
        if (!changed)
            continue;

        auto anchoredElements = anchorMap.getOptional(anchorRenderer);
        if (!anchoredElements)
            continue;

        for (auto& anchoredElement : *anchoredElements) {
            if (!context.invalidatedAnchorPositioned.add(anchoredElement.get()).isNewEntry)
                continue;
            anchoredElementsToInvalidate.append(anchoredElement);
        }
    }

    for (auto& toInvalidate : anchoredElementsToInvalidate) {
        CheckedPtr renderer = toInvalidate->renderer();
        if (renderer && AnchorPositionEvaluator::isLayoutTimeAnchorPositioned(renderer->style()))
            renderer->setNeedsLayout();
        toInvalidate->invalidateForAnchorRectChange();
    }

    return !anchoredElementsToInvalidate.isEmpty();
}

bool DocumentScope::invalidateForPositionTryFallbacks(LayoutDependencyUpdateContext& context)
{
    if (!m_document->renderView())
        return false;

    bool invalidated = false;

    for (auto& box : m_document->renderView()->positionTryBoxes()) {
        if (!AnchorPositionEvaluator::overflowsInsetModifiedContainingBlock(box))
            continue;

        CheckedPtr element = box.element();
        if (auto* pseudoElement = dynamicDowncast<PseudoElement>(element.get()))
            element = pseudoElement->hostElement();

        if (element) {
            if (!context.invalidatedAnchorPositioned.add(*element).isNewEntry)
                continue;
            element->invalidateForAnchorRectChange();
            invalidated = true;
        }
    }

    return invalidated;
}

MatchResultCache& DocumentScope::matchResultCache()
{
    if (!m_matchResultCache)
        m_matchResultCache = makeUnique<MatchResultCache>();
    return *m_matchResultCache;
}

EnvironmentVariables& DocumentScope::environmentVariables() const
{
    if (!m_environmentVariables) {
        auto& thisScope = const_cast<DocumentScope&>(*this);
        thisScope.m_environmentVariables = makeUnique<EnvironmentVariables>(m_document.get());
    }
    return *m_environmentVariables;
}

void DocumentScope::updateAnchorPositioningStateAfterStyleResolution()
{
    if (CheckedPtr renderView = m_document->renderView())
        AnchorPositionEvaluator::updateScrollAdjustments(*renderView); // Is this necessary? Or will the combination of layout and scroll invalidation handle it sufficiently?

    m_anchorPositionedToAnchorMap.removeIf([](auto& elementAndState) {
        return elementAndState.value.anchors.isEmpty();
    });
}

std::optional<size_t> DocumentScope::lastSuccessfulPositionOptionIndexFor(const Styleable& styleable)
{
    return m_lastSuccessfulPositionOptionIndexes.getOptional(styleable);
}

void DocumentScope::setLastSuccessfulPositionOptionIndexMap(HashMap<WeakStyleable, size_t>&& map)
{
    m_lastSuccessfulPositionOptionIndexes = WTF::move(map);
}

void DocumentScope::forgetLastSuccessfulPositionOptionIndex(const Styleable& styleable)
{
    m_lastSuccessfulPositionOptionIndexes.remove(styleable);
}

}
}
