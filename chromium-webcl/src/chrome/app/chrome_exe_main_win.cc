// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <windows.h>
#include <tchar.h>

#include "base/at_exit.h"
#include "base/command_line.h"
#include "chrome/app/breakpad_win.h"
#include "chrome/app/client_util.h"
#include "chrome/app/metro_driver_win.h"
#include "content/public/app/startup_helper_win.h"
#include "content/public/common/result_codes.h"
#include "sandbox/win/src/sandbox_factory.h"

int RunChrome(HINSTANCE instance) {
  bool exit_now = true;
  // We restarted because of a previous crash. Ask user if we should relaunch.
  if (ShowRestartDialogIfCrashed(&exit_now)) {
    if (exit_now)
      return content::RESULT_CODE_NORMAL_EXIT;
  }

  // Initialize the sandbox services.
  sandbox::SandboxInterfaceInfo sandbox_info = {0};
  content::InitializeSandboxInfo(&sandbox_info);

  // Load and launch the chrome dll. *Everything* happens inside.
  MainDllLoader* loader = MakeMainDllLoader();
  int rc = loader->Launch(instance, &sandbox_info);
  loader->RelaunchChromeBrowserWithNewCommandLineIfNeeded();
  delete loader;
  return rc;
}


#include <CL/cl.h>
#include <iostream>
#include <string>
#include <fstream>

#define SUCCESS 0
#define FAILURE 1

using namespace std;

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE prev, wchar_t*, int) {

#ifdef TEST_OPENCL

	/*Step1: Getting platforms and choose an available one.*/
	cl_uint numPlatforms;	//the NO. of platforms
	cl_platform_id platform = NULL;	//the chosen platform
	cl_int	status = clGetPlatformIDs(0, NULL, &numPlatforms);
	if (status != CL_SUCCESS)
	{
		cout << "Error: Getting platforms!" << endl;
		return FAILURE;
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
		cout << "No GPU device available." << endl;
		cout << "Choose CPU as default device." << endl;
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
	cout << "input string:" << endl;
	cout << input << endl;
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
	cout << "\noutput string:" << endl;
	cout << output << endl;

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

	std::cout<<"Passed!\n";

#endif


  // Initialize the commandline singleton from the environment.
  CommandLine::Init(0, NULL);
  // The exit manager is in charge of calling the dtors of singletons.
  base::AtExitManager exit_manager;

  MetroDriver metro_driver;
  if (metro_driver.in_metro_mode())
    return metro_driver.RunInMetro(instance, &RunChrome);
  // Not in metro mode, proceed as normal.
  return RunChrome(instance);
}
