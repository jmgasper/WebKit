/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionTabScriptParser.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include <cmath>
namespace WebKit {
std::expected<WebExtensionTabScriptParameters, String> parseWebExtensionTabScript(const JSON::Value* input)
{
    RefPtr object = input ? input->asObject() : nullptr;
    if (!object)
        return makeUnexpected("script details must be an object"_s);
    WebExtensionTabScriptParameters result;
    for (auto& key : object->keys()) {
        Ref value = object->getValue(key).releaseNonNull();
        if (key == "code"_s || key == "file"_s) {
            if (value->type() != JSON::Value::Type::String)
                return makeUnexpected("script code and file must be strings"_s);
            if (key == "code"_s)
                result.code = value->asString();
            else {
                auto path = value->asString();
                if (path.isEmpty() || path.contains('\0'))
                    return makeUnexpected("script file must be a nonempty resource path without NUL"_s);
                result.file = WTF::move(path);
            }
        } else if (key == "allFrames"_s || key == "matchAboutBlank"_s) {
            auto flag = value->asBoolean();
            if (!flag)
                return makeUnexpected("script frame flags must be booleans"_s);
            if (key == "allFrames"_s)
                result.allFrames = *flag;
            else
                result.matchAboutBlank = *flag;
        } else if (key == "frameId"_s) {
            auto number = value->asDouble();
            if (!number || !std::isfinite(*number) || std::trunc(*number) != *number || *number < 0 || *number > 9007199254740991.0)
                return makeUnexpected("frameId must be a nonnegative safe integer"_s);
            result.frameIdentifier = toWebExtensionFrameIdentifier(*number);
            if (!isValid(result.frameIdentifier))
                return makeUnexpected("frameId is not a valid frame identifier"_s);
        } else if (key == "documentId"_s) {
            if (value->type() != JSON::Value::Type::String)
                return makeUnexpected("documentId must be a UUID string"_s);
            result.documentIdentifier = WTF::UUID::parse(value->asString());
            if (!result.documentIdentifier)
                return makeUnexpected("documentId must be a valid UUID"_s);
        } else if (key == "runAt"_s) {
            auto when = value->asString();
            if (when == "document_start"_s)
                result.runAt = WebExtensionTabScriptRunAt::DocumentStart;
            else if (when == "document_end"_s)
                result.runAt = WebExtensionTabScriptRunAt::DocumentEnd;
            else if (when == "document_idle"_s)
                result.runAt = WebExtensionTabScriptRunAt::DocumentIdle;
            else
                return makeUnexpected("runAt must be document_start, document_end or document_idle"_s);
        } else
            return makeUnexpected("unsupported script detail field"_s);
    }
    if (result.code.has_value() == result.file.has_value())
        return makeUnexpected("script details must specify exactly one of code or file"_s);
    if ((result.allFrames && (result.frameIdentifier || result.documentIdentifier))
        || (result.frameIdentifier && result.documentIdentifier))
        return makeUnexpected("allFrames, frameId and documentId cannot select competing targets"_s);
    if (!result.allFrames && !result.frameIdentifier && !result.documentIdentifier)
        result.frameIdentifier = WebExtensionFrameConstants::MainFrameIdentifier;
    return result;
}
}
#endif
