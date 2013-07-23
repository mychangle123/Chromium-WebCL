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

#include "..\V8Binding.h"
#include "V8WebCL.h"
#include "V8WebCLImage.h"

#include "modules/webcl/WebCLImage.h"

#include <wtf/RefPtr.h>

namespace WebCore {

	
v8::Handle<v8::Value> V8WebCL::getImageInfoMethodCustom(const v8::Arguments& args)
{
   
    if (args.Length() != 2)
        return throwNotEnoughArgumentsError(args.GetIsolate());

    ExceptionCode ec = 0;
    WebCL* webcl = V8WebCL::toNative(args.Holder());
    WebCLImage* image = V8WebCLImage::toNative(v8::Handle<v8::Object>::Cast(args[0]));
    int image_index = toInt32(args[1]);
    WebCLGetInfo info = webcl->getImageInfo(image, image_index, ec);
    if (ec)
        return setDOMException(ec, args.GetIsolate());

    return toV8Object(info,args.Holder(), args.GetIsolate());
}

v8::Handle<v8::Value> V8WebCL::constructorCustom(const v8::Arguments& args)
{

    if (!args.IsConstructCall())
		return throwTypeError("DOM object constructor cannot be called as a function.",args.GetIsolate());

    if (ConstructorMode::current() == ConstructorMode::WrapExistingObject)
        return args.Holder();

    // Get the script execution context.
    ScriptExecutionContext* context = getScriptExecutionContext();
   // if (!context)
        //return throwError(ReferenceError, "WebCL constructor's associated frame is not available", args.GetIsolate());

    RefPtr<WebCL> computeContext = WebCL::create(context);
    V8DOMWrapper::setNativeInfo(args.Holder(), &info, computeContext.get());
	//lifetime LT;
    //V8DOMWrapper::setJSWrapperForActiveDOMObject(computeContext.release(), v8::Persistent<v8::Object>::New(args.Holder()), args.GetIsolate());
	V8DOMWrapper::associateObjectWithWrapper(computeContext.release(),&info, /*v8::Persistent<v8::Object>::New(*/args.Holder()/*)*/, args.GetIsolate(),WrapperConfiguration::Dependent );//≈·”®
    return args.Holder();
}

} // namespace WebCore

#endif // ENABLE(WEBCL)