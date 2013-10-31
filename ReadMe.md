Build Instructions of Chromium-WebCL project
=====

Chromium-WebCL is a project that adding WebCL support to the Chromium Web
 Browser (Chrome). With the new feature, we can get a better image/video
 support in Web Pages.

This project can be build and run on Windows, and the followings are the
 build instructions.

Our design reference Samsung's WebCL implementation for WebKit on Mac OSX,
 which is on:

https://github.com/SRA-SiliconValley/webkit-webcl

We also implemented a Windows version of WebKit-WebCL, which is on:

https://github.com/amd/webkit-webcl/tree/WebCL-on-Windows


Build Environment Requirements
-----
### Prerequisite Software

* **Install Windows 7 or later**<br/>
A 64 bit OS is **highly** recommended as building on 32 bit OS is constantly
 becoming harder, is a lot slower and is not actively maintained.<br/>
At least 60 GB of free space in an NTFS volume. Tip: having the chromium
 source in a SSD drive greatly speeds build times.

* **Install Visual Studio 2010 Professional**<br/>
Make sure "X64 Compilers and Tools" are installed.<br/>
Install VS2010 SP1. Get it from:<br/>
https://www.microsoft.com/en-us/download/details.aspx?id=23691.<br/>

* **Install Windows 8 SDK**<br/>
Get it from:<br/>
http://msdn.microsoft.com/en-us/windows/hardware/hh852363.aspx<br/>
Note: If you install the SDK in a path different than<br/>
C:\Program Files (x86)\Windows Kits\8.0<br/>
you need to set the following following environment variable:<br/>
GYP_DEFINES=windows_sdk_path="path to sdk"

* **Install June 2010 DirectX SDK**<br/>
Get it from:<br/>
http://www.microsoft.com/download/en/details.aspx?displaylang=en&id=6812<br/>
Note: If your install fails with the "Error Code: S1023" you may need to<br/>
 uninstall "Microsoft Visual C++ 2010 x64 Redistributable".<br/>
See this tip from stackoverflow:<br/>
http://stackoverflow.com/questions/4102259/directx-sdk-june-2010-installation-problems-error-code-s1023

* **Install AMD Driver**<br/>
Get it from AMD offical website.<br/>
http://support.amd.com/us/gpudownload/Pages/index.aspx.

* **AMD APP SDK**<br/>
Get it from AMD offical website.<br/>
http://developer.amd.com/tools-and-sdks/heterogeneous-computing/amd-accelerated-parallel-processing-app-sdk/<br/>
By default Accelerated Parallel Processing(APP) SDK will install to<br/>
C:\Program Files(x86)\<br/>
then copy<br/>
C:\Program Files(x86)\include\CL to Microsoft Visual Studio 10.0\VC\include<br/>
and copy "C:\Program Files(x86)\lib\x86(x86_64)\OpenCL.lib" to "Microsoft Visual Studio 10.0\Vc\lib\"

* **Path the Windows 8 SDK to build with Visual C++ 2010**<br/>
Parts of Chromium build using the winrt headers included with the 
Windows 8 SDK. All the headers we use, including the WRL, can be 
compiled with Visual C++ 2010 with the exception of one file, asyncinfo.h. 
This file uses a strongly typed enum which the VS2010 compiler doesn't 
understand. To workaround this for the time being, a small patch needs to be 
applied to the Windows 8 SDK to build with the winrt headers in VS2010:<br/>
**Patch for Include\winrt\asyncinfo.h**

		Index: asyncinfo.h
		      ---  asyncinfo.
		+++  asyncinfo.h
		@@  -63,f + 63,7  @@
		  #pragma once
		  #ifdef  __cplusplus
		  namespace ABI { namespace Windows { namespce
		 Foundation {
		   -enum class AsyncStatus {
		   +enum /*class*/ AsyncStatus {
		       Started = 0,
		 Completed, 
		 Canceled,

This patch should be applied to the file "Include\winrt\asyncinfo.h" located 
in your Windows 8 SDK directory. If this patch is not applied, the parts of 
Chromium that use the Winrt headers will not compile.<br/>
Note: By default the Windows 8 SDK will install to<br/>
C:\Program Files (x86)\Windows Kits\8.0\.<br/>
This directory will require admin privileges to write to. Easiest way to do 
apply this patch is to start an administrative command prompt, cd to<br/>
C:\Program Files (x86)\Windows Kits\8.0\Include\winrt\ <br/>,
run notepad.exe asyncinfo.h and comment out or delete the word "class" 
on line 66.<br/>
Note: For Googlers, this patch has already been applied to your SDK,
everything should Just Work.



Building Chromium
-----

* **Get the Chromium depot tools from and add it to the path**:

```
  http://www.chromium.org/developers/how-tos/install-depot-tools.
```

* **running "gclient runhooks --force" in a cmd window.**<br/>
Run this command in the Chromium/src folder.

* **open the build/all.sln solution file by Visual Studio 2010.**<br/>
In Chromium/src/build folder.

* **Build**<br/>
Set the "Chrome.proj" as Startup Project. And then build. 
This can take from 10 minutes to 2 hours. More likely 1 hours.

* **Run the Test**<br/>
Run "test\_case\webcl\example2.html", note that we only support this example in opencl\_with\_sandbox branch.

