Chromium EADME.md

========================
In this project, We using the OpenCl and OpenGl under the Chrome web browser. with this We can get a better image/video support.This project create in Visual Studio 2010 compiler and running.Build environment under windows.


## Requirements ##
* Windows 7 or later( A 64 bit OS is highly recommended as building on 32 bit OS is constantly   becoming harder,is a lot slower and is not actively maintained. At least 60 GB of free space   in an NTFS volume).
* Visual Staudio 2010 Profession or Standard.
* Windows 8 SDK.
* June 2010 DirectX SDK.
* (Optional) Cygwin.
* AMD Driver.
* AMD APP SDK.


### How to set up the environment ###
* Install Visual Studio 2010. Make sure `X64 Compilers and Tools` are installed.* Install   VS2010 SP1 
```https://www.microsoft.com/en-us/download/details.aspx?id=23691```
* Install the Windows 8 SDK   If you install the SDK in a path differnt than `C:\Program Files   (x86)\Windows Kits\8.0` you need to set the following environment     variable:`GYP_DEFINES=windows_sdk_path="path to sdk"`download the Windows 8 SDK from:
```http://msdn.microsoft.com/en-us/windows/hardware/hh852363.aspx```

* Install the June 2010 DirectX SDK
```http://www.microsoft.com/en-us/down/details.aspx?display=en&id=6812```
note: If you install fails with the `Error Code: S1023` you may need to uninstall `Microsoft         Visual C++ 2010 x64 Redistributable` see this tip     from       stackoverfilow:`http://stackoverflow.com/questions/4102259/directx-sdk-june-2010-      installation-problems-error-code-s1023`.
* Path the Windows 8 SDK to build with Visual C++ 2010  0.Parts of Chromium build using the      winrt headers included with the Windows 8 SDK. All the headers we use, include the WRL, can   be compiled with Visual C++ 2010 with the exception of one file, asyncinfo.h. This file uses   a strongly typed enum which the VS2010 compiler doesn't understand. To workaround this for     the time being. a small patch needs to be applied to the Windows 8 SDK to build with the       winrt headers in VS2010 

```Patch for Include\winrt\asyncinfo.h```
```Index: asyncinfo.h```
```    ---  asyncinfo.h```
```    +++  asyncinfo.h``` 
```    @@  -63,f + 63,7  @@ ``` 
```    #pragma once``` 
```    #ifdef  __cplusplus```
```    namespace ABI { namespace Windows { namespce Foundation {	```  
```   -enum class AsyncStatus {```
```   +enum /*class*/ AsyncStatus {```
```	    Started  =  0,```
```	    Completed,```
```	    Canceled,```

   This patch should be applied to the file `include\winrt\asyncinfo.h` located in your    Windows 8 SDK directory. If this patch is not applied. the parts of Charomium that use the    Winrt headers will not compile. Note: By default the Windows 8 SDK will install to to `C:   \Program Files (x86)\Windows Kits\8.0\.` This directory will require admin privileges to    write to. Easiest way to do apply this patch is to start an administrative command prompt: 

```http://technet.microsoft.com/en-us/library/cc947813%28v=ws.10%29.aspx```
cd to `C:\Program Files (x86)\Windows Kits\8.0\8.0\Include\winrt\` run notepad.exe `asyncinfo.h` and comment out or delete the word `class` on line 66. Note: For Googlers, this patch has already been applied to your SDK* Install AMD Drivers 

```http://support.amd.com/us/gpudownload/Pages/index.aspx```

* Install Accelerated Parallel Processing(APP) SDK

 ```http://developer.amd.com/tools-and-sdks/heterogeneous-computing/amd-accelerated-parallel-processing-app-sdk/``` 

Note: We need to install in tur AMD Driver and AMD APP SDK. By default Accelerated Parallel       Processing(APP) SDK will install to `C:\Program Files(x86)\` then copy `C:\Program       Files(x86)\include\CL` to the `Microsoft Visual Studio 10.0\VC\include` and copy `C:      \Program Files(x86)\lib\x86(x86_64)\OpenCL.lib` to the `Microsoft Visual Studio 10.0\Vc      \lib\.` everything should Just Work.


##Building Chromium##
* Get the Chromium depot tools from: 

```http://www.chromium.org/developers/how-tos/install-depot-tools```

* running `gclient runhooks --force` in a cygwin/cmd window
* open the build/all.sln solution file in Visual Studio
* Set the Chrome.proj as Startup Project. And then build. This can take from 10 minutes to 2     hours. More likely 1 hours.