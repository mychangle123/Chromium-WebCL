/*
* Copyright (C) 2011 Samsung Electronics Corporation. All rights reserved.
* 
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided the following conditions
* are met:
* 
* 1.  Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
* 
* 2.  Redistributions in binary form must reproduce the above copyright
*     notice, this list of conditions and the following disclaimer in the
*     documentation and/or other materials provided with the distribution.
* 
* THIS SOFTWARE IS PROVIDED BY SAMSUNG ELECTRONICS CORPORATION AND ITS
* CONTRIBUTORS "AS IS", AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING
* BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
* FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL SAMSUNG
* ELECTRONICS CORPORATION OR ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
* INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES(INCLUDING
* BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
* DATA, OR PROFITS, OR BUSINESS INTERRUPTION), HOWEVER CAUSED AND ON ANY THEORY
* OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT(INCLUDING
* NEGLIGENCE OR OTHERWISE ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
* EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "config.h"

#if ENABLE(WEBCL)

#include "WebCLPlatformList.h"
#include "WebCL.h"

namespace WebCore {

WebCLPlatformList::WebCLPlatformList()
{
}

WebCLPlatformList::~WebCLPlatformList()
{
}

PassRefPtr<WebCLPlatformList> WebCLPlatformList::create(WebCL* ctx)
{
	
	return adoptRef(new WebCLPlatformList(ctx));
}

WebCLPlatformList::WebCLPlatformList(WebCL* ctx) : m_context(ctx)
{
	
	cl_int err = 0;
	
	err = clGetPlatformIDs(0, NULL, &m_num_platforms);
	if (err != CL_SUCCESS) {
		// TODO (siba samal) Error handling
	}
	
	m_cl_platforms = new cl_platform_id[m_num_platforms];
	err = clGetPlatformIDs(m_num_platforms, m_cl_platforms, NULL);
	if (err != CL_SUCCESS) {
		// TODO (siba samal) Error handling
	}
	
	for (unsigned int i = 0 ; i < m_num_platforms; i++) {
		RefPtr<WebCLPlatform> o = WebCLPlatform::create(m_context, m_cl_platforms[i]);
		if (o != NULL) {
			m_platform_id_list.append(o);
		} else {
			// TODO (siba samal) Error handling
		}
	}
	
}

cl_platform_id WebCLPlatformList::getCLPlatforms()
{
	return *m_cl_platforms;
}

unsigned WebCLPlatformList::length() const
{
	return m_num_platforms;
}

WebCLPlatform* WebCLPlatformList::item(unsigned index)
{
	if (index >= m_num_platforms) {
		return 0;
	}
	WebCLPlatform* ret = (m_platform_id_list[index]).get();
	return ret;
}


} // namespace WebCore

#endif // ENABLE(WEBCL)
