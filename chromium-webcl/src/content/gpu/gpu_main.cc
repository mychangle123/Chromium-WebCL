// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdlib.h>

#if defined(OS_WIN)
#include <CL/opencl.h>
#include <windows.h>
#endif

#include "base/debug/trace_event.h"
#include "base/message_loop.h"
#include "base/rand_util.h"
#include "base/string_number_conversions.h"
#include "base/stringprintf.h"
#include "base/threading/platform_thread.h"
#include "build/build_config.h"
#include "content/common/gpu/gpu_config.h"
#include "content/common/sandbox_linux.h"
#include "content/gpu/gpu_child_thread.h"
#include "content/gpu/gpu_info_collector.h"
#include "content/gpu/gpu_process.h"
#include "content/gpu/gpu_watchdog_thread.h"
#include "content/public/common/content_client.h"
#include "content/public/common/content_switches.h"
#include "content/public/common/gpu_switching_option.h"
#include "content/public/common/main_function_params.h"
#include "crypto/hmac.h"
#include "ui/gl/gl_implementation.h"
#include "ui/gl/gl_surface.h"
#include "ui/gl/gl_switches.h"
#include "ui/gl/gpu_switching_manager.h"

#if defined(OS_WIN)
#include "base/win/scoped_com_initializer.h"
#include "content/common/gpu/media/dxva_video_decode_accelerator.h"
#include "sandbox/win/src/sandbox.h"
#elif defined(OS_CHROMEOS) && defined(ARCH_CPU_ARMEL) && defined(USE_X11)
#include "content/common/gpu/media/exynos_video_decode_accelerator.h"
#include "content/common/gpu/media/omx_video_decode_accelerator.h"
#elif defined(OS_CHROMEOS) && defined(ARCH_CPU_X86_FAMILY) && defined(USE_X11)
#include "content/common/gpu/media/vaapi_video_decode_accelerator.h"
#endif

#if defined(USE_X11)
#include "ui/base/x/x11_util.h"
#endif

#if defined(OS_LINUX)
#include "content/public/common/sandbox_init.h"
#endif

const int kGpuTimeout = 10000;

namespace content {
namespace {
void WarmUpSandbox();
#if defined(OS_LINUX)
bool StartSandboxLinux(const GPUInfo&, GpuWatchdogThread*, bool);
#elif defined(OS_WIN)
bool StartSandboxWindows(const sandbox::SandboxInterfaceInfo*);
#endif
}

// Main function for starting the Gpu process.
int GpuMain(const MainFunctionParams& parameters) {
  TRACE_EVENT0("gpu", "GpuMain");

  cl_int errcode_ret = clGetPlatformIDs(0, NULL, NULL);

  base::Time start_time = base::Time::Now();

  const CommandLine& command_line = parameters.command_line;
  if (command_line.HasSwitch(switches::kGpuStartupDialog)) {
    ChildProcess::WaitForDebugger("Gpu");
  }

  if (!command_line.HasSwitch(switches::kSingleProcess)) {
#if defined(OS_WIN)
    // Prevent Windows from displaying a modal dialog on failures like not being
    // able to load a DLL.
    SetErrorMode(
        SEM_FAILCRITICALERRORS |
        SEM_NOGPFAULTERRORBOX |
        SEM_NOOPENFILEERRORBOX);
#elif defined(USE_X11)
    ui::SetDefaultX11ErrorHandlers();
#endif
  }

  if (command_line.HasSwitch(switches::kSupportsDualGpus) &&
      command_line.HasSwitch(switches::kGpuSwitching)) {
    std::string option = command_line.GetSwitchValueASCII(
        switches::kGpuSwitching);
    if (option == switches::kGpuSwitchingOptionNameForceDiscrete)
      ui::GpuSwitchingManager::GetInstance()->ForceUseOfDiscreteGpu();
    else if (option == switches::kGpuSwitchingOptionNameForceIntegrated)
      ui::GpuSwitchingManager::GetInstance()->ForceUseOfIntegratedGpu();
  }

  // Initialization of the OpenGL bindings may fail, in which case we
  // will need to tear down this process. However, we can not do so
  // safely until the IPC channel is set up, because the detection of
  // early return of a child process is implemented using an IPC
  // channel error. If the IPC channel is not fully set up between the
  // browser and GPU process, and the GPU process crashes or exits
  // early, the browser process will never detect it.  For this reason
  // we defer tearing down the GPU process until receiving the
  // GpuMsg_Initialize message from the browser.
  bool dead_on_arrival = false;

  MessageLoop::Type message_loop_type = MessageLoop::TYPE_IO;
#if defined(OS_WIN)
  // Unless we're running on desktop GL, we don't need a UI message
  // loop, so avoid its use to work around apparent problems with some
  // third-party software.
  if (command_line.HasSwitch(switches::kUseGL) &&
      command_line.GetSwitchValueASCII(switches::kUseGL) ==
          gfx::kGLImplementationDesktopName) {
      message_loop_type = MessageLoop::TYPE_UI;
  }
#elif defined(OS_LINUX)
  message_loop_type = MessageLoop::TYPE_DEFAULT;
#endif

  MessageLoop main_message_loop(message_loop_type);
  base::PlatformThread::SetName("CrGpuMain");

  // In addition to disabling the watchdog if the command line switch is
  // present, disable the watchdog on valgrind because the code is expected
  // to run slowly in that case.
  bool enable_watchdog =
      !CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kDisableGpuWatchdog) &&
      !RunningOnValgrind();

  // Disable the watchdog in debug builds because they tend to only be run by
  // developers who will not appreciate the watchdog killing the GPU process.
#ifndef NDEBUG
  enable_watchdog = false;
#endif

  bool delayed_watchdog_enable = false;

#if defined(OS_CHROMEOS)
  // Don't start watchdog immediately, to allow developers to switch to VT2 on
  // startup.
  delayed_watchdog_enable = true;
#endif

  scoped_refptr<GpuWatchdogThread> watchdog_thread;

  // Start the GPU watchdog only after anything that is expected to be time
  // consuming has completed, otherwise the process is liable to be aborted.
  if (enable_watchdog && !delayed_watchdog_enable) {
    watchdog_thread = new GpuWatchdogThread(kGpuTimeout);
    watchdog_thread->Start();
  }

  GPUInfo gpu_info;
  // Get vendor_id, device_id, driver_version from browser process through
  // commandline switches.
  DCHECK(command_line.HasSwitch(switches::kGpuVendorID) &&
         command_line.HasSwitch(switches::kGpuDeviceID) &&
         command_line.HasSwitch(switches::kGpuDriverVersion));
  bool success = base::HexStringToInt(
      command_line.GetSwitchValueASCII(switches::kGpuVendorID),
      reinterpret_cast<int*>(&(gpu_info.gpu.vendor_id)));
  DCHECK(success);
  success = base::HexStringToInt(
      command_line.GetSwitchValueASCII(switches::kGpuDeviceID),
      reinterpret_cast<int*>(&(gpu_info.gpu.device_id)));
  DCHECK(success);
  gpu_info.driver_vendor =
      command_line.GetSwitchValueASCII(switches::kGpuDriverVendor);
  gpu_info.driver_version =
      command_line.GetSwitchValueASCII(switches::kGpuDriverVersion);
  GetContentClient()->SetGpuInfo(gpu_info);

  // Warm up resources that don't need access to GPUInfo.
  WarmUpSandbox();

#if defined(OS_LINUX)
  bool initialized_sandbox = false;
  bool initialized_gl_context = false;
  bool should_initialize_gl_context = false;
#if defined(OS_CHROMEOS) && defined(ARCH_CPU_ARMEL)
  // On Chrome OS ARM, GPU driver userspace creates threads when initializing
  // a GL context, so start the sandbox early.
  gpu_info.sandboxed = StartSandboxLinux(gpu_info, watchdog_thread.get(),
                                         should_initialize_gl_context);
  initialized_sandbox = true;
#endif
#endif  // defined(OS_LINUX)

  // Load and initialize the GL implementation and locate the GL entry points.
  if (gfx::GLSurface::InitializeOneOff()) {
    // We need to collect GL strings (VENDOR, RENDERER) for blacklisting
    // purposes. However, on Mac we don't actually use them. As documented in
    // crbug.com/222934, due to some driver issues, glGetString could take
    // multiple seconds to finish, which in turn cause the GPU process to crash.
    // By skipping the following code on Mac, we don't really lose anything,
    // because the basic GPU information is passed down from browser process
    // and we already registered them through SetGpuInfo() above.
#if !defined(OS_MACOSX)
    if (!gpu_info_collector::CollectContextGraphicsInfo(&gpu_info))
      VLOG(1) << "gpu_info_collector::CollectGraphicsInfo failed";
    GetContentClient()->SetGpuInfo(gpu_info);

#if defined(OS_LINUX)
    initialized_gl_context = true;
#if !defined(OS_CHROMEOS)
    if (gpu_info.gpu.vendor_id == 0x10de &&  // NVIDIA
        gpu_info.driver_vendor == "NVIDIA") {
      base::ThreadRestrictions::AssertIOAllowed();
      if (access("/dev/nvidiactl", R_OK) != 0) {
        VLOG(1) << "NVIDIA device file /dev/nvidiactl access denied";
        gpu_info.gpu_accessible = false;
        dead_on_arrival = true;
      }
    }
#endif  // !defined(OS_CHROMEOS)
#endif  // defined(OS_LINUX)
#endif  // !defined(OS_MACOSX)
  } else {
    VLOG(1) << "gfx::GLSurface::InitializeOneOff failed";
    gpu_info.gpu_accessible = false;
    gpu_info.finalized = true;
    dead_on_arrival = true;
  }

  if (enable_watchdog && delayed_watchdog_enable) {
    watchdog_thread = new GpuWatchdogThread(kGpuTimeout);
    watchdog_thread->Start();
  }

  // OSMesa is expected to run very slowly, so disable the watchdog in that
  // case.
  if (enable_watchdog &&
      gfx::GetGLImplementation() == gfx::kGLImplementationOSMesaGL) {
    watchdog_thread->Stop();

    watchdog_thread = NULL;
  }

#if defined(OS_LINUX)
  should_initialize_gl_context = !initialized_gl_context &&
                                 !dead_on_arrival;

  if (!initialized_sandbox)
    gpu_info.sandboxed = StartSandboxLinux(gpu_info, watchdog_thread.get(),
                                           should_initialize_gl_context);
#elif defined(OS_WIN)
  gpu_info.sandboxed = StartSandboxWindows(parameters.sandbox_info);
#endif

  GpuProcess gpu_process;

  GpuChildThread* child_thread = new GpuChildThread(watchdog_thread.get(),
                                                    dead_on_arrival, gpu_info);

  child_thread->Init(start_time);

  gpu_process.set_main_thread(child_thread);

  {
    TRACE_EVENT0("gpu", "Run Message Loop");
    main_message_loop.Run();
  }

  child_thread->StopWatchdog();

  return 0;
}

namespace {

#if defined(OS_LINUX)
void CreateDummyGlContext() {
  scoped_refptr<gfx::GLSurface> surface(
      gfx::GLSurface::CreateOffscreenGLSurface(false, gfx::Size(1, 1)));
  if (!surface) {
    VLOG(1) << "gfx::GLSurface::CreateOffscreenGLSurface failed";
    return;
  }

  // On Linux, this is needed to make sure /dev/nvidiactl has
  // been opened and its descriptor cached.
  scoped_refptr<gfx::GLContext> context(
      gfx::GLContext::CreateGLContext(NULL,
                                      surface,
                                      gfx::PreferDiscreteGpu));
  if (!context) {
    VLOG(1) << "gfx::GLContext::CreateGLContext failed";
    return;
  }

  // Similarly, this is needed for /dev/nvidia0.
  if (context->MakeCurrent(surface)) {
    context->ReleaseCurrent(surface.get());
  } else {
    VLOG(1)  << "gfx::GLContext::MakeCurrent failed";
  }
}
#endif

#define SUCCESS 0
#define FAILURE 1

using namespace std;

void WarmUpSandbox() {
  {
    TRACE_EVENT0("gpu", "Warm up rand");
    // Warm up the random subsystem, which needs to be done pre-sandbox on all
    // platforms.
    (void) base::RandUint64();
  }
  {
    TRACE_EVENT0("gpu", "Warm up HMAC");
    // Warm up the crypto subsystem, which needs to done pre-sandbox on all
    // platforms.
    crypto::HMAC hmac(crypto::HMAC::SHA256);
    unsigned char key = '\0';
    bool ret = hmac.Init(&key, sizeof(key));
    (void) ret;
  }

#if defined(OS_CHROMEOS) && defined(ARCH_CPU_ARMEL) && defined(USE_X11)
  if (CommandLine::ForCurrentProcess()->HasSwitch(switches::kUseExynosVda))
    ExynosVideoDecodeAccelerator::PreSandboxInitialization();
  else
    OmxVideoDecodeAccelerator::PreSandboxInitialization();
#elif defined(OS_CHROMEOS) && defined(ARCH_CPU_X86_FAMILY) && defined(USE_X11)
  VaapiVideoDecodeAccelerator::PreSandboxInitialization();
#endif

#if defined(OS_WIN)
  {
    TRACE_EVENT0("gpu", "Preload setupapi.dll");
    // Preload this DLL because the sandbox prevents it from loading.
    LoadLibrary(L"setupapi.dll");
  }

  {
    TRACE_EVENT0("gpu", "Initialize DXVA");
    // Initialize H/W video decoding stuff which fails in the sandbox.
    DXVAVideoDecodeAccelerator::PreSandboxInitialization();
  }
#endif

#ifdef TEST_OPENCL
	/*Step1: Getting platforms and choose an available one.*/
	cl_uint numPlatforms;	//the NO. of platforms
	cl_platform_id platform = NULL;	//the chosen platform
	cl_int	status = clGetPlatformIDs(0, NULL, &numPlatforms);
	if (status != CL_SUCCESS)
	{
		//cout << "Error: Getting platforms!" << endl;
		return;
	}

	/*For clarity, choose the first available platform. */
	if(numPlatforms > 0)
	{
		cl_platform_id* platforms = (cl_platform_id* )malloc(numPlatforms* sizeof(cl_platform_id));
		status = clGetPlatformIDs(numPlatforms, platforms, NULL);
		platform = platforms[0];
		free(platforms);
	}

	/*Step 2:Query the platform and choose the first GPU device if has one.Otherwise use the CPU as device.*/
	cl_uint				numDevices = 0;
	cl_device_id        *devices;
	status = clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 0, NULL, &numDevices);	
	if (numDevices == 0)	//no GPU available.
	{
		//cout << "No GPU device available." << endl;
		//cout << "Choose CPU as default device." << endl;
		status = clGetDeviceIDs(platform, CL_DEVICE_TYPE_CPU, 0, NULL, &numDevices);	
		devices = (cl_device_id*)malloc(numDevices * sizeof(cl_device_id));
		status = clGetDeviceIDs(platform, CL_DEVICE_TYPE_CPU, numDevices, devices, NULL);
	}
	else
	{
		devices = (cl_device_id*)malloc(numDevices * sizeof(cl_device_id));
		status = clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, numDevices, devices, NULL);
	}
	

	/*Step 3: Create context.*/
	cl_context context = clCreateContext(NULL,1, devices,NULL,NULL,NULL);
	
	/*Step 4: Creating command queue associate with the context.*/
	cl_command_queue commandQueue = clCreateCommandQueue(context, devices[0], 0, NULL);

	/*Step 5: Create program object */
	const char *source =
"__kernel void helloworld(__global char* in, __global char* out) \n"
"{ \n"
"	int num = get_global_id(0); \n"
"	out[num] = in[num] + 1; \n"
"} \n"
;
	size_t sourceSize[] = {strlen(source)};
	cl_program program = clCreateProgramWithSource(context, 1, &source, sourceSize, NULL);
	
	/*Step 6: Build program. */
	status=clBuildProgram(program, 1,devices,NULL,NULL,NULL);

	/*Step 7: Initial input,output for the host and create memory objects for the kernel*/
	const char* input = "GdkknVnqkc";
	size_t strlength = strlen(input);
	//cout << "input string:" << endl;
	//cout << input << endl;
	char *output = (char*) malloc(strlength + 1);

	cl_mem inputBuffer = clCreateBuffer(context, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR, (strlength + 1) * sizeof(char),(void *) input, NULL);
	cl_mem outputBuffer = clCreateBuffer(context, CL_MEM_WRITE_ONLY , (strlength + 1) * sizeof(char), NULL, NULL);

	/*Step 8: Create kernel object */
	cl_kernel kernel = clCreateKernel(program,"helloworld", NULL);

	/*Step 9: Sets Kernel arguments.*/
	status = clSetKernelArg(kernel, 0, sizeof(cl_mem), (void *)&inputBuffer);
	status = clSetKernelArg(kernel, 1, sizeof(cl_mem), (void *)&outputBuffer);
	
	/*Step 10: Running the kernel.*/
	size_t global_work_size[1] = {strlength};
	status = clEnqueueNDRangeKernel(commandQueue, kernel, 1, NULL, global_work_size, NULL, 0, NULL, NULL);

	/*Step 11: Read the cout put back to host memory.*/
	status = clEnqueueReadBuffer(commandQueue, outputBuffer, CL_TRUE, 0, strlength * sizeof(char), output, 0, NULL, NULL);
	
	output[strlength] = '\0';	//Add the terminal character to the end of output.
	//cout << "\noutput string:" << endl;
	//cout << output << endl;

	/*Step 12: Clean the resources.*/
	status = clReleaseKernel(kernel);				//Release kernel.
	status = clReleaseProgram(program);				//Release the program object.
	status = clReleaseMemObject(inputBuffer);		//Release mem object.
	status = clReleaseMemObject(outputBuffer);
	status = clReleaseCommandQueue(commandQueue);	//Release  Command queue.
	status = clReleaseContext(context);				//Release context.

	if (output != NULL)
	{
		free(output);
		output = NULL;
	}

	if (devices != NULL)
	{
		free(devices);
		devices = NULL;
	}
#endif
}

#if defined(OS_LINUX)
void WarmUpSandboxNvidia(const GPUInfo& gpu_info,
                         bool should_initialize_gl_context) {
  // We special case Optimus since the vendor_id we see may not be Nvidia.
  bool uses_nvidia_driver = (gpu_info.gpu.vendor_id == 0x10de &&  // NVIDIA.
                             gpu_info.driver_vendor == "NVIDIA") ||
                            gpu_info.optimus;
  if (uses_nvidia_driver && should_initialize_gl_context) {
    // We need this on Nvidia to pre-open /dev/nvidiactl and /dev/nvidia0.
    CreateDummyGlContext();
  }
}

bool StartSandboxLinux(const GPUInfo& gpu_info,
                       GpuWatchdogThread* watchdog_thread,
                       bool should_initialize_gl_context) {
  TRACE_EVENT0("gpu", "Initialize sandbox");

  bool res = false;

  WarmUpSandboxNvidia(gpu_info, should_initialize_gl_context);

  if (watchdog_thread)
    watchdog_thread->Stop();
  // LinuxSandbox::InitializeSandbox() must always be called
  // with only one thread.
  res = LinuxSandbox::InitializeSandbox();
  if (watchdog_thread)
    watchdog_thread->Start();

  return res;
}
#endif  // defined(OS_LINUX)

#if defined(OS_WIN)
bool StartSandboxWindows(const sandbox::SandboxInterfaceInfo* sandbox_info) {
  TRACE_EVENT0("gpu", "Lower token");

  // For Windows, if the target_services interface is not zero, the process
  // is sandboxed and we must call LowerToken() before rendering untrusted
  // content.
  sandbox::TargetServices* target_services = sandbox_info->target_services;
  if (target_services) {
    //ScalableVision: uncomment when AMD driver works in sandbox
    //target_services->LowerToken();
    return true;
  }

  return false;
}
#endif  // defined(OS_WIN)

}  // namespace.

}  // namespace content
