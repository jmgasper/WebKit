/*
 * Copyright (C) 2026 KunanyiOS contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "WebExtensionDeclarativeNetRequestURLFilter.h"

#if ENABLE(WK_WEB_EXTENSIONS) && ENABLE(CONTENT_EXTENSIONS)

#include <WebCore/CombinedURLFilters.h>
#include <WebCore/URLFilterParser.h>
#include <wtf/ASCIICType.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace WebKit {

std::expected<WebExtensionDeclarativeNetRequestURLFilter, String> WebExtensionDeclarativeNetRequestURLFilter::parse(const JSON::Object& condition, WebExtensionDeclarativeNetRequestURLFilterLimits limits)
{
    WebExtensionDeclarativeNetRequestURLFilter result;
    if (auto sensitive = condition.getValue("isUrlFilterCaseSensitive"_s)) {
        auto boolean = sensitive->asBoolean();
        if (!boolean)
            return makeUnexpected("isUrlFilterCaseSensitive must be a boolean."_s);
        result.caseSensitive = *boolean;
    }

    auto urlFilter = condition.getValue("urlFilter"_s);
    auto regexFilter = condition.getValue("regexFilter"_s);
    if (urlFilter && regexFilter)
        return makeUnexpected("Only one of urlFilter and regexFilter may be specified."_s);

    String input;
    if (urlFilter || regexFilter) {
        input = (urlFilter ? urlFilter : regexFilter)->asString();
        if (input.isEmpty())
            return makeUnexpected("URL filters must be non-empty strings."_s);
        if (input.length() > limits.maximumInputLength)
            return makeUnexpected("URL filter exceeds the native input limit."_s);
        for (auto character : StringView(input).codeUnits()) {
            if (!isASCII(character) || !character)
                return makeUnexpected("URL filters must contain ASCII characters without NUL."_s);
        }
    }

    size_t outputLength = 0;
    auto append = [&](String pattern) -> bool {
        if (result.patterns.size() >= limits.maximumPatternCount || pattern.length() > limits.maximumOutputLength - outputLength)
            return false;
        outputLength += pattern.length();
        result.patterns.append(WTF::move(pattern));
        return true;
    };
    constexpr auto limitError = "URL filter exceeds the native translation limit."_s;

    if (!urlFilter || regexFilter) {
        result.regularExpression = !!regexFilter;
        if (regexFilter) {
            // URLFilterParser uses Yarr's legacy mode with backreferences
            // disabled. It otherwise accepts \1 as octal and \k as literal k,
            // silently changing expressions supplied by an extension.
            for (size_t index = 0; index < input.length(); ++index) {
                if (input[index] != '\\' || index + 1 == input.length())
                    continue;
                auto escaped = input[++index];
                if (isASCIIDigit(escaped) || escaped == 'k' || escaped == 'g')
                    return makeUnexpected("Backreferences and numeric escapes are not supported in regexFilter."_s);
                if (isASCIIAlpha(escaped) && !StringView("fnrtvdDsSwWbBx"_s).contains(escaped))
                    return makeUnexpected("Unsupported escape in regexFilter."_s);
                if (escaped == 'x') {
                    if (index + 2 >= input.length() || !isASCIIHexDigit(input[index + 1]) || !isASCIIHexDigit(input[index + 2]))
                        return makeUnexpected("Hex escapes in regexFilter require two hexadecimal digits."_s);
                    if (input[index + 1] == '0' && input[index + 2] == '0')
                        return makeUnexpected("NUL escapes are not supported in regexFilter."_s);
                    index += 2;
                }
            }
        }
        if (!append(regexFilter ? WTF::move(input) : String(".*"_s)))
            return makeUnexpected(limitError);
    } else {
        if (input.startsWith("||*"_s))
            return makeUnexpected("A urlFilter must not begin with ||*."_s);

        StringView body(input);
        bool domainAnchor = body.startsWith("||"_s);
        bool startAnchor = !domainAnchor && body.startsWith('|');
        if (domainAnchor)
            body = body.substring(2);
        else if (startAnchor)
            body = body.substring(1);
        bool endAnchor = body.endsWith('|');
        if (endAnchor)
            body = body.substring(0, body.length() - 1);

        // A separator can consume no character only if the rest of the pattern
        // can also finish at end-of-URL. Only '*' and '^' permit that. Emitting
        // these suffix cases avoids unsupported disjunctions/lookarounds.
        size_t nullableSuffix = body.length();
        while (nullableSuffix && (body[nullableSuffix - 1] == '*' || body[nullableSuffix - 1] == '^'))
            --nullableSuffix;

        auto appendLiteral = [](StringBuilder& builder, char16_t character) {
            if (StringView("?+[(){}$|\\."_s).contains(character))
                builder.append('\\');
            builder.append(character);
        };
        auto translateSuffix = [&](const String& prefix, size_t start) -> bool {
            StringBuilder expression;
            expression.append(prefix);
            bool previousWasWildcard = false;
            for (size_t index = start; index < body.length(); ++index) {
                auto character = body[index];
                if (character == '^') {
                    if (index >= nullableSuffix && !append(makeString(expression.toString(), '$')))
                        return false;
                    expression.append("[^a-zA-Z0-9_.%\\-]"_s);
                } else if (character == '*') {
                    if (!previousWasWildcard)
                        expression.append(".*"_s);
                } else
                    appendLiteral(expression, character);
                previousWasWildcard = character == '*';
                if (expression.length() > limits.maximumOutputLength - outputLength)
                    return false;
            }
            if (endAnchor)
                expression.append('$');
            if (expression.isEmpty())
                expression.append(".*"_s);
            return append(expression.toString());
        };

        if (!domainAnchor) {
            if (!translateSuffix(startAnchor ? String("^"_s) : emptyString(), 0))
                return makeUnexpected(limitError);
        } else {
            // The start must be in the host, not in credentials. Merely making
            // userinfo optional is insufficient: a regex can choose the branch
            // without userinfo and match a credential prefix. Until the first
            // '/', '?' or '#', prohibit '@' in both the pattern and the rest of
            // the authority. Enumerate the points where a wildcard/separator
            // could cross that boundary, then translate the suffix normally.
            StringBuilder authority;
            authority.append("^[a-zA-Z][a-zA-Z0-9+.-]*://([^/?#@]*@)?([^/?#@:.]+\\.)*"_s);
            bool finished = false;
            bool previousWasWildcard = false;
            for (size_t index = 0; index < body.length(); ++index) {
                auto character = body[index];
                if (character == '/' || character == '?' || character == '#') {
                    appendLiteral(authority, character);
                    if (!translateSuffix(authority.toString(), index + 1))
                        return makeUnexpected(limitError);
                    finished = true;
                    break;
                }
                if (character == '@') {
                    // Earlier wildcard branches may still match '@' in a path.
                    finished = true;
                    break;
                }
                if (character == '*') {
                    if (!previousWasWildcard) {
                        if (!translateSuffix(makeString(authority.toString(), "[^/?#@]*[/?#].*"_s), index + 1))
                            return makeUnexpected(limitError);
                        authority.append("[^/?#@]*"_s);
                    }
                } else if (character == '^') {
                    if (index >= nullableSuffix && !append(makeString(authority.toString(), '$')))
                        return makeUnexpected(limitError);
                    if (!translateSuffix(makeString(authority.toString(), "[/?#]"_s), index + 1))
                        return makeUnexpected(limitError);
                    authority.append("[^a-zA-Z0-9_.%\\-@/?#]"_s);
                } else
                    appendLiteral(authority, character);
                previousWasWildcard = character == '*';
                if (authority.length() > limits.maximumOutputLength - outputLength)
                    return makeUnexpected(limitError);
            }
            if (!finished) {
                authority.append(endAnchor ? "$"_s : "[^/?#@]*([/?#].*)?$"_s);
                if (!append(authority.toString()))
                    return makeUnexpected(limitError);
            }
        }
    }

    // Use the same parser as content-rule compilation. A valid JavaScript/RE2
    // expression need not belong to WebCore's supported regular-expression
    // subset. Failure must not turn a restricted rule into a match-all rule.
    using namespace WebCore::ContentExtensions;
    for (auto& pattern : result.patterns) {
        CombinedURLFilters filters;
        URLFilterParser parser(filters);
        auto status = parser.addPattern(pattern, result.caseSensitive, 1);
        if (status != URLFilterParser::Ok && status != URLFilterParser::MatchesEverything)
            return makeUnexpected(makeString("Unsupported content-rule URL filter: "_s, URLFilterParser::statusString(status)));
    }
    return result;
}

} // namespace WebKit

#endif
