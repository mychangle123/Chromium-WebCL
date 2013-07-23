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

#ifndef WebCLCommandQueue_h
#define WebCLCommandQueue_h

#if OS(DARWIN)
#include <OpenCL/opencl.h>
#else
#include <CL/opencl.h>
#endif

#include <wtf/PassRefPtr.h>
#include <wtf/RefCounted.h>
#include <wtf/Vector.h>
#include <wtf/ArrayBufferView.h>
#include <wtf/Uint8ClampedArray.h>

#include "WebCLGetInfo.h"
#include "WebCLFinishCallback.h"
#include "WebCLProgram.h"
#include "WebCLKernel.h"
#include "WebCLMem.h"
#include "WebCLEvent.h"
#include "WebCLSampler.h"
#include "core/html/ImageData.h"
#include "core/html/HTMLCanvasElement.h"

namespace WebCore {

class WebCL;
class WebCLEventList;

class WebCLCommandQueue : public RefCounted<WebCLCommandQueue> {
public:
	virtual ~WebCLCommandQueue();
	static PassRefPtr<WebCLCommandQueue> create(WebCL*, cl_command_queue);
	WebCLGetInfo getInfo(int, ExceptionCode&);
	
	void enqueueWriteBuffer(WebCLMem*, bool, int, int, ArrayBufferView*, WebCLEventList* ,WebCLEvent* , ExceptionCode&);
	
	void enqueueWriteBuffer(WebCLMem* mem, bool blocking_write, int offset, int buffer_size, ArrayBufferView* ptr, 
									WebCLEventList* events, ExceptionCode& ec) {
		return(enqueueWriteBuffer(mem, blocking_write, offset, buffer_size, ptr, events, NULL, ec));
	}

	void enqueueWriteBuffer(WebCLMem* mem, bool blocking_write, int offset, int buffer_size, ArrayBufferView* ptr, 
												 ExceptionCode& ec) {
		return(enqueueWriteBuffer(mem, blocking_write, offset, buffer_size, ptr, NULL, NULL, ec));
	}
	PassRefPtr<WebCLEvent> enqueueWriteBuffer(WebCLMem*, bool, int, int, 
		ImageData*, int, ExceptionCode&);

	PassRefPtr<WebCLEvent>  enqueueReadBuffer(WebCLMem*, bool, int, int, 
		ImageData*, int, ExceptionCode&);
	
	void enqueueReadBuffer(WebCLMem*, bool, int, int, ArrayBufferView*, WebCLEventList* ,WebCLEvent* , ExceptionCode&);
	void enqueueReadBuffer(WebCLMem* mem, bool blocking_read, int offset, int buffer_size, ArrayBufferView* ptr, 
									WebCLEventList* events, ExceptionCode& ec) {
		return(enqueueReadBuffer(mem, blocking_read, offset, buffer_size, ptr, events, NULL, ec));
	}

	void enqueueReadBuffer(WebCLMem* mem, bool blocking_read, int offset, int buffer_size, ArrayBufferView* ptr, 
												 ExceptionCode& ec) {
		return(enqueueReadBuffer(mem, blocking_read, offset, buffer_size, ptr, NULL, NULL, ec));
	}

	void  enqueueNDRangeKernel(WebCLKernel* ,Int32Array* ,
		Int32Array* ,Int32Array* ,WebCLEventList* ,WebCLEvent* , ExceptionCode&);
	void  enqueueNDRangeKernel(WebCLKernel* kernel, Int32Array* offsets,
			Int32Array* global_work_size, Int32Array* local_work_size, WebCLEventList* events, ExceptionCode& ec) {
			return (enqueueNDRangeKernel(kernel ,offsets ,global_work_size ,local_work_size ,events, NULL, ec)); 	
		}
	void  enqueueNDRangeKernel(WebCLKernel* kernel, Int32Array* offsets,
			Int32Array* global_work_size, Int32Array* local_work_size, ExceptionCode& ec) {
			return (enqueueNDRangeKernel(kernel ,offsets ,global_work_size ,local_work_size , NULL,	NULL, ec)); 	
		}
	
	void finish(ExceptionCode&);
	void flush( ExceptionCode&);
	void releaseCL( ExceptionCode&);
	PassRefPtr<WebCLEvent> enqueueWriteImage(WebCLMem*, bool, Int32Array*, 
		Int32Array*, HTMLCanvasElement*, int, ExceptionCode&);
	//long enqueueReadImage(WebCLMem*, bool, Int32Array*, 
	//		Int32Array*, HTMLCanvasElement*, int, ExceptionCode&);
	void enqueueAcquireGLObjects(WebCLMem* ,WebCLEventList* ,WebCLEvent*, ExceptionCode&);
	void enqueueAcquireGLObjects(WebCLMem* mem, WebCLEventList* events, ExceptionCode& ec) {
		return(enqueueAcquireGLObjects(mem,  events,  NULL, ec));
	}
	void enqueueAcquireGLObjects(WebCLMem* mem, ExceptionCode& ec) {
		return(enqueueAcquireGLObjects(mem,  NULL, NULL, ec));
	}
	
	void enqueueReleaseGLObjects(WebCLMem*, WebCLEventList* ,WebCLEvent*, ExceptionCode&);
	void enqueueReleaseGLObjects(WebCLMem* mem,WebCLEventList* events, ExceptionCode& ec) {
		return(enqueueReleaseGLObjects(mem,  events,  NULL, ec));
	}
	void enqueueReleaseGLObjects(WebCLMem* mem, ExceptionCode& ec) {
		return(enqueueReleaseGLObjects(mem,  NULL, NULL, ec));
	}

	void enqueueCopyBuffer(WebCLMem*, WebCLMem*, int, ExceptionCode&);
	void enqueueBarrier( ExceptionCode&);
	void enqueueMarker(WebCLEvent*, ExceptionCode&);
	void enqueueWaitForEvents(WebCLEventList*, ExceptionCode&);
	PassRefPtr<WebCLEvent> enqueueTask( WebCLKernel* ,int, ExceptionCode&);
	cl_command_queue getCLCommandQueue();	
private:
	WebCLCommandQueue(WebCL*, cl_command_queue);	
	WebCL* m_context;
	cl_command_queue m_cl_command_queue;
	RefPtr<WebCLFinishCallback> m_finishCallback;
	RefPtr<WebCLCommandQueue> m_command_queue;
	
	
	long m_num_events;
	long m_num_commandqueues;
	long m_num_mems;
	Vector<RefPtr<WebCLEvent> > m_event_list;
	Vector<RefPtr<WebCLCommandQueue> > m_commandqueue_list;
	Vector<RefPtr<WebCLMem> > m_mem_list;
};

} // namespace WebCore
#endif // WebCLCommandQueue_h
