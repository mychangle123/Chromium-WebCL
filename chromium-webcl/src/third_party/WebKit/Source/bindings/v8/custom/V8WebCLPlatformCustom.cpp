/*
 * Copyright (C) 2012 Intel Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS
 * IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE
 * INC. OR ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#if ENABLE(WEBCL)

#include "V8WebCLCustom.h"
#include "V8WebCLPlatform.h"

namespace WebCore {

v8::Handle<v8::Value> V8WebCLPlatform::getInfoMethodCustom(const v8::Arguments& args)
{

    if (args.Length() != 1)
        return throwNotEnoughArgumentsError(args.GetIsolate());

    ExceptionCode ec = 0;
    WebCLPlatform* platform = V8WebCLPlatform::toNative(args.Holder()); 
    int platform_index = toInt32(args[0]);
    WebCLGetInfo info = platform->getInfo(platform_index, ec);
    if (ec)
        return setDOMException(ec, args.GetIsolate());

	return toV8Object(info, args.Holder(),args.GetIsolate());
}

v8::Handle<v8::Value> V8WebCLPlatform::getSupportedExtensionsMethodCustom(const v8::Arguments& args)
{
    
    ExceptionCode ec = 0;
    WebCLPlatform* platform = V8WebCLPlatform::toNative(args.Holder());
    Vector<String> extensions = platform->getSupportedExtensions(ec);
    if (ec)
        return setDOMException(ec, args.GetIsolate());

    v8::Local<v8::Array> array = v8::Array::New(extensions.size());
    for (size_t i = 0; i < extensions.size(); i++)
        array->Set(v8::Integer::New(i), v8::String::New(fromWebCoreString(extensions[i]), extensions[i].length()));
    return array;
}

} // namespace WebCore

#endif // ENABLE(WEBCL)