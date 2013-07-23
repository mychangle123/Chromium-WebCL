// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/common/gpu/client/gpu_channel_host.h"

#include "base/bind.h"
#include "base/debug/trace_event.h"
#include "base/message_loop.h"
#include "base/message_loop_proxy.h"
#include "base/posix/eintr_wrapper.h"
#include "base/threading/thread_restrictions.h"
#include "content/common/gpu/client/command_buffer_proxy_impl.h"
#include "content/common/gpu/gpu_messages.h"
#include "googleurl/src/gurl.h"
#include "gpu/command_buffer/common/mailbox.h"
#include "ipc/ipc_sync_message_filter.h"

#if defined(OS_WIN)
#include "content/public/common/sandbox_init.h"
#endif

using base::AutoLock;
using base::MessageLoopProxy;

namespace content {

GpuListenerInfo::GpuListenerInfo() {}

GpuListenerInfo::~GpuListenerInfo() {}

GpuChannelHost::GpuChannelHost(
    GpuChannelHostFactory* factory, int gpu_host_id, int client_id)
    : factory_(factory),
      client_id_(client_id),
      gpu_host_id_(gpu_host_id),
      state_(kUnconnected) {
  next_transfer_buffer_id_.GetNext();
}

void GpuChannelHost::Connect(
    const IPC::ChannelHandle& channel_handle) {
  DCHECK(factory_->IsMainThread());
  // Open a channel to the GPU process. We pass NULL as the main listener here
  // since we need to filter everything to route it to the right thread.
  scoped_refptr<base::MessageLoopProxy> io_loop = factory_->GetIOLoopProxy();
  channel_.reset(new IPC::SyncChannel(
      channel_handle, IPC::Channel::MODE_CLIENT, NULL,
      io_loop, true,
      factory_->GetShutDownEvent()));

  sync_filter_ = new IPC::SyncMessageFilter(
      factory_->GetShutDownEvent());

  channel_->AddFilter(sync_filter_.get());

  channel_filter_ = new MessageFilter(AsWeakPtr(), factory_);

  // Install the filter last, because we intercept all leftover
  // messages.
  channel_->AddFilter(channel_filter_.get());

  // It is safe to send IPC messages before the channel completes the connection
  // and receives the hello message from the GPU process. The messages get
  // cached.
  state_ = kConnected;
}

void GpuChannelHost::set_gpu_info(const GPUInfo& gpu_info) {
  gpu_info_ = gpu_info;
}

void GpuChannelHost::SetStateLost() {
  state_ = kLost;
}

const GPUInfo& GpuChannelHost::gpu_info() const {
  return gpu_info_;
}

void GpuChannelHost::OnMessageReceived(const IPC::Message& message) {
    bool handled = true;

    IPC_BEGIN_MESSAGE_MAP(GpuChannelHost, message)
      IPC_MESSAGE_HANDLER(GpuChannelMsg_GenerateMailboxNamesReply,
                          OnGenerateMailboxNamesReply)
    IPC_MESSAGE_UNHANDLED(handled = false)
    IPC_END_MESSAGE_MAP()

    DCHECK(handled);
}

void GpuChannelHost::OnChannelError() {
  state_ = kLost;

  // Channel is invalid and will be reinitialized if this host is requested
  // again.
  channel_.reset();
}

bool GpuChannelHost::Send(IPC::Message* message) {
  // The GPU process never sends synchronous IPCs so clear the unblock flag to
  // preserve order.
  message->set_unblock(false);

  // Currently we need to choose between two different mechanisms for sending.
  // On the main thread we use the regular channel Send() method, on another
  // thread we use SyncMessageFilter. We also have to be careful interpreting
  // IsMainThread() since it might return false during shutdown,
  // impl we are actually calling from the main thread (discard message then).
  //
  // TODO: Can we just always use sync_filter_ since we setup the channel
  //       without a main listener?
  if (factory_->IsMainThread()) {
    if (channel_) {
      // http://crbug.com/125264
      base::ThreadRestrictions::ScopedAllowWait allow_wait;
      return channel_->Send(message);
    }
  } else if (MessageLoop::current()) {
    return sync_filter_->Send(message);
  }

  // Callee takes ownership of message, regardless of whether Send is
  // successful. See IPC::Sender.
  delete message;
  return false;
}

CommandBufferProxyImpl* GpuChannelHost::CreateViewCommandBuffer(
    int32 surface_id,
    CommandBufferProxyImpl* share_group,
    const std::string& allowed_extensions,
    const std::vector<int32>& attribs,
    const GURL& active_url,
    gfx::GpuPreference gpu_preference) {
  TRACE_EVENT1("gpu",
               "GpuChannelHost::CreateViewCommandBuffer",
               "surface_id",
               surface_id);

  AutoLock lock(context_lock_);
  // An error occurred. Need to get the host again to reinitialize it.
  if (!channel_)
    return NULL;

  GPUCreateCommandBufferConfig init_params;
  init_params.share_group_id =
      share_group ? share_group->GetRouteID() : MSG_ROUTING_NONE;
  init_params.allowed_extensions = allowed_extensions;
  init_params.attribs = attribs;
  init_params.active_url = active_url;
  init_params.gpu_preference = gpu_preference;
  int32 route_id = factory_->CreateViewCommandBuffer(surface_id, init_params);
  if (route_id == MSG_ROUTING_NONE)
    return NULL;

  CommandBufferProxyImpl* command_buffer =
      new CommandBufferProxyImpl(this, route_id);
  AddRoute(route_id, command_buffer->AsWeakPtr());
  proxies_[route_id] = command_buffer;
  return command_buffer;
}

CommandBufferProxyImpl* GpuChannelHost::CreateOffscreenCommandBuffer(
    const gfx::Size& size,
    CommandBufferProxyImpl* share_group,
    const std::string& allowed_extensions,
    const std::vector<int32>& attribs,
    const GURL& active_url,
    gfx::GpuPreference gpu_preference) {
  TRACE_EVENT0("gpu", "GpuChannelHost::CreateOffscreenCommandBuffer");

  AutoLock lock(context_lock_);
  // An error occurred. Need to get the host again to reinitialize it.
  if (!channel_)
    return NULL;

  GPUCreateCommandBufferConfig init_params;
  init_params.share_group_id =
      share_group ? share_group->GetRouteID() : MSG_ROUTING_NONE;
  init_params.allowed_extensions = allowed_extensions;
  init_params.attribs = attribs;
  init_params.active_url = active_url;
  init_params.gpu_preference = gpu_preference;
  int32 route_id;
  if (!Send(new GpuChannelMsg_CreateOffscreenCommandBuffer(size,
                                                           init_params,
                                                           &route_id))) {
    return NULL;
  }

  // We should test OpenCL API calling here, please visit http://helloracer.com/webgl/ to activate the test.
  cl_int errcode_ret;
  cl_uint num_platforms, num_devices;
  cl_platform_id* platform_ids = 0;
  cl_device_id * device_ids = 0;
  cl_context context = NULL;
//  char temp[1000];
  platform_ids = new cl_platform_id[1];
  errcode_ret = CallclGetPlatformIDs(0,NULL,NULL);
  errcode_ret = CallclGetPlatformIDs(2,NULL,NULL);
  errcode_ret = CallclGetPlatformIDs(0,platform_ids,NULL);
  errcode_ret = CallclGetPlatformIDs(2,platform_ids,NULL);
  errcode_ret = CallclGetPlatformIDs(0,NULL,&num_platforms);
//  errcode_ret = CallclGetPlatformIDs(2,NULL,&num_platforms);
  errcode_ret = CallclGetPlatformIDs(0,platform_ids,&num_platforms);
  errcode_ret = CallclGetPlatformIDs(2,platform_ids,&num_platforms);

  errcode_ret = CallclGetPlatformIDs(0,NULL,&num_platforms);
  if (errcode_ret == 0 && num_platforms > 0)
  {
    platform_ids = new cl_platform_id[num_platforms];
    errcode_ret = CallclGetPlatformIDs(num_platforms, platform_ids, &num_platforms);
  }

  if (errcode_ret == 0)
  {
    errcode_ret = CallclGetDeviceIDs(platform_ids[0], CL_DEVICE_TYPE_GPU, 0, NULL, &num_devices);
  }

  if (errcode_ret == 0 && num_devices > 0)
  {
    device_ids = new cl_device_id[num_devices];
    errcode_ret = CallclGetDeviceIDs(platform_ids[0], CL_DEVICE_TYPE_GPU, num_devices, device_ids, &num_devices);
  }

  if (errcode_ret == 0)
  {
    context = CallclCreateContext(NULL, 1, device_ids, NULL, NULL, &errcode_ret);
  }

  cl_mem memobj;
  memobj = CallclCreateBuffer(context,NULL,900,NULL, &errcode_ret);
  
  errcode_ret = CallclGetPlatformIDs(0,NULL,&num_platforms);
  errcode_ret = CallclGetPlatformIDs(0,NULL,&num_platforms);
  // The test has been completed.

  if (route_id == MSG_ROUTING_NONE)
    return NULL;

  CommandBufferProxyImpl* command_buffer =
      new CommandBufferProxyImpl(this, route_id);
  AddRoute(route_id, command_buffer->AsWeakPtr());
  proxies_[route_id] = command_buffer;
  return command_buffer;
}

scoped_ptr<media::VideoDecodeAccelerator> GpuChannelHost::CreateVideoDecoder(
    int command_buffer_route_id,
    media::VideoCodecProfile profile,
    media::VideoDecodeAccelerator::Client* client) {
  AutoLock lock(context_lock_);
  ProxyMap::iterator it = proxies_.find(command_buffer_route_id);
  DCHECK(it != proxies_.end());
  CommandBufferProxyImpl* proxy = it->second;
  return proxy->CreateVideoDecoder(profile, client).Pass();
}

void GpuChannelHost::DestroyCommandBuffer(
    CommandBufferProxyImpl* command_buffer) {
  TRACE_EVENT0("gpu", "GpuChannelHost::DestroyCommandBuffer");

  AutoLock lock(context_lock_);
  int route_id = command_buffer->GetRouteID();
  Send(new GpuChannelMsg_DestroyCommandBuffer(route_id));
  // Check the proxy has not already been removed after a channel error.
  if (proxies_.find(route_id) != proxies_.end())
    proxies_.erase(route_id);
  RemoveRoute(route_id);
  delete command_buffer;
}

bool GpuChannelHost::CollectRenderingStatsForSurface(
    int surface_id, GpuRenderingStats* stats) {
  TRACE_EVENT0("gpu", "GpuChannelHost::CollectRenderingStats");

  return Send(new GpuChannelMsg_CollectRenderingStatsForSurface(surface_id,
                                                                stats));
}

void GpuChannelHost::AddRoute(
    int route_id, base::WeakPtr<IPC::Listener> listener) {
  DCHECK(MessageLoopProxy::current());

  scoped_refptr<base::MessageLoopProxy> io_loop = factory_->GetIOLoopProxy();
  io_loop->PostTask(FROM_HERE,
                    base::Bind(&GpuChannelHost::MessageFilter::AddRoute,
                               channel_filter_.get(), route_id, listener,
                               MessageLoopProxy::current()));
}

void GpuChannelHost::RemoveRoute(int route_id) {
  scoped_refptr<base::MessageLoopProxy> io_loop = factory_->GetIOLoopProxy();
  io_loop->PostTask(FROM_HERE,
                    base::Bind(&GpuChannelHost::MessageFilter::RemoveRoute,
                               channel_filter_.get(), route_id));
}

base::SharedMemoryHandle GpuChannelHost::ShareToGpuProcess(
    base::SharedMemoryHandle source_handle) {
  AutoLock lock(context_lock_);

  if (!channel_)
    return base::SharedMemory::NULLHandle();

#if defined(OS_WIN)
  // Windows needs to explicitly duplicate the handle out to another process.
  base::SharedMemoryHandle target_handle;
  if (!BrokerDuplicateHandle(source_handle,
                             channel_->peer_pid(),
                             &target_handle,
                             0,
                             DUPLICATE_SAME_ACCESS)) {
    return base::SharedMemory::NULLHandle();
  }

  return target_handle;
#else
  int duped_handle = HANDLE_EINTR(dup(source_handle.fd));
  if (duped_handle < 0)
    return base::SharedMemory::NULLHandle();

  return base::FileDescriptor(duped_handle, true);
#endif
}

bool GpuChannelHost::GenerateMailboxNames(unsigned num,
                                          std::vector<gpu::Mailbox>* names) {
  TRACE_EVENT0("gpu", "GenerateMailboxName");
  AutoLock lock(context_lock_);

  if (num > mailbox_name_pool_.size()) {
    if (!Send(new GpuChannelMsg_GenerateMailboxNames(num, names)))
      return false;
  } else {
    names->insert(names->begin(),
                  mailbox_name_pool_.end() - num,
                  mailbox_name_pool_.end());
    mailbox_name_pool_.erase(mailbox_name_pool_.end() - num,
                             mailbox_name_pool_.end());
  }

  const unsigned ideal_mailbox_pool_size = 100;
  if (mailbox_name_pool_.size() < ideal_mailbox_pool_size / 2) {
    Send(new GpuChannelMsg_GenerateMailboxNamesAsync(
        ideal_mailbox_pool_size - mailbox_name_pool_.size()));
  }

  return true;
}

void GpuChannelHost::OnGenerateMailboxNamesReply(
    const std::vector<gpu::Mailbox>& names) {
  TRACE_EVENT0("gpu", "OnGenerateMailboxNamesReply");
  AutoLock lock(context_lock_);

  mailbox_name_pool_.insert(mailbox_name_pool_.end(),
                            names.begin(),
                            names.end());
}

int32 GpuChannelHost::ReserveTransferBufferId() {
  return next_transfer_buffer_id_.GetNext();
}

GpuChannelHost::~GpuChannelHost() {}


GpuChannelHost::MessageFilter::MessageFilter(
    base::WeakPtr<GpuChannelHost> parent,
    GpuChannelHostFactory* factory)
    : parent_(parent),
      main_thread_loop_(factory->GetMainLoop()->message_loop_proxy()) {
}

GpuChannelHost::MessageFilter::~MessageFilter() {}

void GpuChannelHost::MessageFilter::AddRoute(
    int route_id,
    base::WeakPtr<IPC::Listener> listener,
    scoped_refptr<MessageLoopProxy> loop) {
  DCHECK(listeners_.find(route_id) == listeners_.end());
  GpuListenerInfo info;
  info.listener = listener;
  info.loop = loop;
  listeners_[route_id] = info;
}

void GpuChannelHost::MessageFilter::RemoveRoute(int route_id) {
  ListenerMap::iterator it = listeners_.find(route_id);
  if (it != listeners_.end())
    listeners_.erase(it);
}

bool GpuChannelHost::MessageFilter::OnMessageReceived(
    const IPC::Message& message) {
  // Never handle sync message replies or we will deadlock here.
  if (message.is_reply())
    return false;

  if (message.routing_id() == MSG_ROUTING_CONTROL) {
    main_thread_loop_->PostTask(
        FROM_HERE, base::Bind(
            &GpuChannelHost::OnMessageReceived, parent_, message));
    return true;
  }

  ListenerMap::iterator it = listeners_.find(message.routing_id());

  if (it != listeners_.end()) {
    const GpuListenerInfo& info = it->second;
    info.loop->PostTask(
        FROM_HERE,
        base::Bind(
            base::IgnoreResult(&IPC::Listener::OnMessageReceived),
            info.listener,
            message));
  }

  return true;
}

void GpuChannelHost::MessageFilter::OnChannelError() {
  // Post the task to signal the GpuChannelHost before the proxies. That way, if
  // they themselves post a task to recreate the context, they will not try to
  // re-use this channel host before it has a chance to mark itself lost.
  main_thread_loop_->PostTask(
      FROM_HERE,
      base::Bind(&GpuChannelHost::OnChannelError, parent_));
  // Inform all the proxies that an error has occurred. This will be reported
  // via OpenGL as a lost context.
  for (ListenerMap::iterator it = listeners_.begin();
       it != listeners_.end();
       it++) {
    const GpuListenerInfo& info = it->second;
    info.loop->PostTask(
        FROM_HERE,
        base::Bind(&IPC::Listener::OnChannelError, info.listener));
  }

  listeners_.clear();
}

// Adding the implement of OpenCL API calling.

cl_int GpuChannelHost::CallclGetPlatformIDs(
    cl_uint num_entries,
    cl_platform_id* platforms,
    cl_uint* num_platforms) {
  // Sending a Sync IPC Message, to call a clGetPlatformIDs API
  // in other process, and getting the results of the API.
  cl_int errcode_ret;
  cl_uint num_platforms_inter;
  std::vector<cl_point> point_platform_list;
  std::vector<bool> return_variable_null_status;

  return_variable_null_status.resize(2);
  return_variable_null_status[0] = return_variable_null_status[1] = false;

  // The Sync Message can't get value back by NULL ptr, so if a
  // return back ptr is NULL, we must instead it using another
  // no-NULL ptr.
  if (NULL == num_platforms) {
    num_platforms = &num_platforms_inter;
    return_variable_null_status[0] = true;
  }
  if (NULL == platforms)
    return_variable_null_status[1] = true;

  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_GetPlatformIDs(
           num_entries,
           return_variable_null_status,
           &point_platform_list,
           num_platforms,
           &errcode_ret))) {
    return CL_SEND_IPC_MESSAGE_FAILURE;
  }

  // Dump the results of the Sync IPC Message calling.
  if (CL_SUCCESS == errcode_ret)
    for (cl_uint index = 0; index < num_entries; ++index)
      platforms[index] = (cl_platform_id) point_platform_list[index];

  return errcode_ret;
}

cl_int GpuChannelHost::CallclGetDeviceIDs(
    cl_platform_id platform,
    cl_device_type device_type,
    cl_uint num_entries,
    cl_device_id* devices,
    cl_uint* num_devices) {
  // Sending a Sync IPC Message, to call a clGetDeviceIDs API
  // in other process, and getting the results of the API.
  cl_int errcode_ret;
  cl_uint num_devices_inter;
  cl_point point_platform = (cl_point) platform;
  std::vector<cl_point> point_device_list;
  std::vector<bool> return_variable_null_status;

  return_variable_null_status.resize(2);
  return_variable_null_status[0] = return_variable_null_status[1] = false;

  // The Sync Message can't get value back by NULL ptr, so if a
  // return back ptr is NULL, we must instead it using another
  // no-NULL ptr.
  if (NULL == num_devices) {
    num_devices = &num_devices_inter;
    return_variable_null_status[0] = true;
  }
  if (NULL == devices)
    return_variable_null_status[1] = true;

  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_GetDeviceIDs(
           point_platform,
           device_type,
           num_entries,
           return_variable_null_status,
           &point_device_list,
           num_devices,
           &errcode_ret))) {
    return CL_SEND_IPC_MESSAGE_FAILURE;
  }

  // Dump the results of the Sync IPC Message calling.
  if (CL_SUCCESS == errcode_ret)
    for (cl_uint index = 0; index < num_entries; ++index)
      devices[index] = (cl_device_id) point_device_list[index];

  return errcode_ret;
}

cl_int GpuChannelHost::CallclCreateSubDevices(
    cl_device_id in_device,
    const cl_device_partition_property* properties,
    cl_uint num_devices,
    cl_device_id* out_devices,
    cl_uint* num_devices_ret) {
  // Sending a Sync IPC Message, to call a clCreateSubDevices API
  // in other process, and getting the results of the API.
  cl_int errcode_ret;
  cl_uint num_devices_ret_inter;
  cl_point point_in_device = (cl_point) in_device;
  std::vector<cl_point> point_out_device_list;
  std::vector<cl_device_partition_property> property_list;
  std::vector<bool> return_variable_null_status;

  return_variable_null_status.resize(2);
  return_variable_null_status[0] = return_variable_null_status[1] = false;

  // The Sync Message can't get value back by NULL ptr, so if a
  // return back ptr is NULL, we must instead it using another
  // no-NULL ptr.
  if (NULL == num_devices_ret) {
    num_devices_ret = &num_devices_ret_inter;
    return_variable_null_status[0] = true;
  }
  if (out_devices)
    return_variable_null_status[1] = true;

  // Dump the inputs of the Sync IPC Message calling.
  property_list.clear();
  if (NULL != properties) {
    while (0 != *properties)
      property_list.push_back(*properties++);
    property_list.push_back(0);
  }

  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_CreateSubDevices(
           point_in_device,
           property_list,
           num_devices,
           return_variable_null_status,
           &point_out_device_list,
           num_devices_ret,
           &errcode_ret))) {
    return CL_SEND_IPC_MESSAGE_FAILURE;
  }

  // Dump the results of the Sync IPC Message calling.
  if (CL_SUCCESS == errcode_ret)
    for (cl_uint index = 0; index < num_devices; ++index)
      out_devices[index] = (cl_device_id)(point_out_device_list[index]);

  return errcode_ret;
}

cl_int GpuChannelHost::CallclRetainDevice(cl_device_id device) {
  // Sending a Sync IPC Message, to call a clRetainDevice API
  // in other process, and getting the results of the API.
  cl_int errcode_ret;
  cl_point point_device = (cl_point) device;

  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_RetainDevice(
           point_device,
           &errcode_ret))) {
    return CL_SEND_IPC_MESSAGE_FAILURE;
  }
  return errcode_ret;
}

cl_int GpuChannelHost::CallclReleaseDevice(cl_device_id device) {
  // Sending a Sync IPC Message, to call a clReleaseDevice API
  // in other process, and getting the results of the API.
  cl_int errcode_ret;
  cl_point point_device = (cl_point) device;

  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_ReleaseDevice(
           point_device,
           &errcode_ret))) {
    return CL_SEND_IPC_MESSAGE_FAILURE;
  }
  return errcode_ret;
}

cl_context GpuChannelHost::CallclCreateContext(
    const cl_context_properties* properties,
    cl_uint num_devices,
    const cl_device_id* devices,
    void (CL_CALLBACK* pfn_notify)(const char*, const void*, size_t, void*),
    void* user_data,
    cl_int* errcode_ret) {
  // Sending a Sync IPC Message, to call a CallclCreateContext API
  // in other process, and getting the results of the API.
  cl_int errcode_ret_inter;
  cl_point point_context_ret;
  std::vector<cl_device_partition_property> property_list;
  std::vector<cl_point> point_device_list;
  std::vector<cl_point> point_pfn_list;
  std::vector<bool> return_variable_null_status;

  return_variable_null_status.resize(1);
  return_variable_null_status[0] = false;

  // The Sync Message can't get value back by NULL ptr, so if a
  // return back ptr is NULL, we must instead it using another
  // no-NULL ptr.
  if (NULL == errcode_ret) {
    errcode_ret = &errcode_ret_inter;
    return_variable_null_status[0] = true;
  }

  // Dump the inputs of the Sync IPC Message calling.
  point_pfn_list.push_back((cl_point) pfn_notify);
  point_pfn_list.push_back((cl_point) user_data);

  property_list.clear();
  if (NULL != properties) {
    while (0 != *properties)
      property_list.push_back(*properties++);
    property_list.push_back(0);
  }

  point_device_list.clear();
  for (cl_uint index = 0; devices && index < num_devices; ++index)
    point_device_list.push_back((cl_point) devices[index]);

  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_CreateContext(
            property_list,
            num_devices,
            point_device_list,
            point_pfn_list,
            return_variable_null_status,
            errcode_ret,
            &point_context_ret))) {
    return NULL;
  }
  return (cl_context) point_context_ret;
}

cl_context GpuChannelHost::CallclCreateContextFromType(
    const cl_context_properties *properties,
    cl_device_type device_type,
    void (CL_CALLBACK *pfn_notify)(const char *, const void *,size_t, void *),
    void *user_data,
    cl_int *errcode_ret) {
  // Sending a Sync IPC Message, to call a clCreateContextFromType API
  // in other process, and getting the results of the API.
  cl_int errcode_ret_inter;
  cl_point point_context_ret;
  std::vector<cl_device_partition_property> property_list;
  cl_point point_pfn_notify = (cl_point) pfn_notify;
  cl_point point_user_data = (cl_point) user_data;
  std::vector<bool> return_variable_null_status;

  return_variable_null_status.resize(1);
  return_variable_null_status[0] = false;

  // The Sync Message can't get value back by NULL ptr, so if a
  // return back ptr is NULL, we must instead it using another
  // no-NULL ptr.
  if (NULL == errcode_ret) {
    errcode_ret = &errcode_ret_inter;
    return_variable_null_status[0] = true;
  }

  // Dump the inputs of the Sync IPC Message calling.
  property_list.clear();
  if (NULL != properties) {
    while (0 != *properties)
      property_list.push_back(*properties++);
    property_list.push_back(0);
  }

  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_CreateContextFromType(
           property_list,
           device_type,
           point_pfn_notify,
           point_user_data,
           return_variable_null_status,
           errcode_ret,
           &point_context_ret))) {
    return NULL;
  }
  return (cl_context) point_context_ret;
}

cl_int GpuChannelHost::CallclRetainContext(cl_context context) {
  // Sending a Sync IPC Message, to call a clRetainContext API
  // in other process, and getting the results of the API.
  cl_int errcode_ret;
  cl_point point_context = (cl_point) context;

  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_RetainContext(
           point_context,
           &errcode_ret))) {
    return CL_SEND_IPC_MESSAGE_FAILURE;
  }
  return errcode_ret;
}

cl_int GpuChannelHost::CallclReleaseContext(cl_context context) {
  // Sending a Sync IPC Message, to call a clReleaseContext API
  // in other process, and getting the results of the API.
  cl_int errcode_ret;
  cl_point point_context = (cl_point) context;

  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_ReleaseContext(
           point_context,
           &errcode_ret))) {
    return CL_SEND_IPC_MESSAGE_FAILURE;
  }
  return errcode_ret;
}

cl_command_queue GpuChannelHost::CallclCreateCommandQueue(
    cl_context context,
    cl_device_id device,
    cl_command_queue_properties properties,
    cl_int *errcode_ret) {
  // Sending a Sync IPC Message, to call a clCreateCommandQueue API
  // in other process, and getting the results of the API.
  cl_int errcode_ret_inter;
  cl_point point_context = (cl_point) context;
  cl_point point_device = (cl_point) device;
  cl_point point_command_queue_ret;
  std::vector<bool> return_variable_null_status;

  return_variable_null_status.resize(1);
  return_variable_null_status[0] = false;

  // The Sync Message can't get value back by NULL ptr, so if a
  // return back ptr is NULL, we must instead it using another
  // no-NULL ptr.
  if (NULL == errcode_ret) {
    errcode_ret = &errcode_ret_inter;
    return_variable_null_status[0] = true;
  }

  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_CreateCommandQueue(
           point_context,
           point_device,
           properties,
           return_variable_null_status,
           errcode_ret,
           &point_command_queue_ret))) {
    return NULL;
  }
  return (cl_command_queue) point_command_queue_ret;
}

cl_int GpuChannelHost::CallclRetainCommandQueue(
    cl_command_queue command_queue) {
  // Sending a Sync IPC Message, to call a clRetainCommandQueue API
  // in other process, and getting the results of the API.
  cl_int errcode_ret;
  cl_point point_command_queue = (cl_point) command_queue;

  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_RetainCommandQueue(
           point_command_queue,
           &errcode_ret))) {
    return CL_SEND_IPC_MESSAGE_FAILURE;
  }
  return errcode_ret;
}

cl_int GpuChannelHost::CallclReleaseCommandQueue(
    cl_command_queue command_queue) {
  // Sending a Sync IPC Message, to call a clReleaseCommandQueue API
  // in other process, and getting the results of the API.
  cl_int errcode_ret;
  cl_point point_command_queue = (cl_point) command_queue;

  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_ReleaseCommandQueue(
           point_command_queue,
           &errcode_ret))) {
    return CL_SEND_IPC_MESSAGE_FAILURE;
  }
  return errcode_ret;
}

cl_mem GpuChannelHost::CallclCreateBuffer(
    cl_context context,
    cl_mem_flags flags,
    size_t size,
    void *host_ptr,
    cl_int *errcode_ret) {
  // Sending a Sync IPC Message, to call a clCreateBuffer API
  // in other process, and getting the results of the API.
  cl_int errcode_ret_inter;
  cl_point point_context = (cl_point) context;
  cl_point point_host_ptr = (cl_point) host_ptr;
  cl_point point_memobj_ret;
  std::vector<bool> return_variable_null_status;

  return_variable_null_status.resize(1);
  return_variable_null_status[0] = false;

  // The Sync Message can't get value back by NULL ptr, so if a
  // return back ptr is NULL, we must instead it using another
  // no-NULL ptr.
  if (NULL == errcode_ret) {
    errcode_ret = &errcode_ret_inter;
    return_variable_null_status[0] = true;
  }

  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_CreateBuffer(
           point_context,
           flags,
           size,
           point_host_ptr,
		   return_variable_null_status,
           errcode_ret,
           &point_memobj_ret))) {
    return NULL;
  }
  return (cl_mem) point_memobj_ret;
}

cl_mem GpuChannelHost::CallclCreateSubBuffer(
    cl_mem buffer,
    cl_mem_flags flags,
    cl_buffer_create_type buffer_create_type,
    const void *buffer_create_info,
    cl_int *errcode_ret) {
  // Sending a Sync IPC Message, to call a clCreateSubBuffer API
  // in other process, and getting the results of the API.
  cl_int errcode_ret_inter;
  cl_point point_buffer = (cl_point) buffer;
  cl_point point_buffer_create_info = (cl_point) buffer_create_info;
  cl_point point_memobj_ret;
  std::vector<bool> return_variable_null_status;

  return_variable_null_status.resize(1);
  return_variable_null_status[0] = false;

  // The Sync Message can't get value back by NULL ptr, so if a
  // return back ptr is NULL, we must instead it using another
  // no-NULL ptr.
  if (NULL == errcode_ret) {
    errcode_ret = &errcode_ret_inter;
    return_variable_null_status[0] = true;
  }

  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_CreateSubBuffer(
           point_buffer,
           flags,
           buffer_create_type,
           point_buffer_create_info,
		   return_variable_null_status,
           errcode_ret,
           &point_memobj_ret))) {
    return NULL;
  }
  return (cl_mem) point_memobj_ret;
}

cl_mem GpuChannelHost::CallclCreateImage(
    cl_context context,
    cl_mem_flags flags,
    const cl_image_format *image_format,
    const cl_image_desc *image_desc,
    void *host_ptr,
    cl_int *errcode_ret) {
  // Sending a Sync IPC Message, to call a clCreateImage API
  // in other process, and getting the results of the API.
  cl_int errcode_ret_inter;
  cl_point point_context = (cl_point) context;
  cl_point point_host_ptr = (cl_point) host_ptr;
  cl_point point_memobj_ret;
  std::vector<cl_uint> image_format_list;
  std::vector<bool> return_variable_null_status;

  return_variable_null_status.resize(1);
  return_variable_null_status[0] = false;

  // The Sync Message can't get value back by NULL ptr, so if a
  // return back ptr is NULL, we must instead it using another
  // no-NULL ptr.
  if (NULL == errcode_ret) {
    errcode_ret = &errcode_ret_inter;
    return_variable_null_status[0] = true;
  }

  // Dump the inputs of the Sync IPC Message calling.
  image_format_list.clear();
  image_format_list.push_back(image_format->image_channel_order);
  image_format_list.push_back(image_format->image_channel_data_type);
  // There are some bugs here, we must add some code to fully support it.

  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_CreateImage(
           point_context,
           flags,
           image_format_list,
           point_host_ptr,
		   return_variable_null_status,
           errcode_ret,
           &point_memobj_ret))) {
    return NULL;
  }
  return (cl_mem) point_memobj_ret;
}

cl_int GpuChannelHost::CallclRetainMemObject(cl_mem memobj) {
  // Sending a Sync IPC Message, to call a clRetainMemObject API
  // in other process, and getting the results of the API.
  cl_int errcode_ret;
  cl_point point_memobj = (cl_point) memobj;

  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_RetainMemObject(
           point_memobj,
           &errcode_ret))) {
    return CL_SEND_IPC_MESSAGE_FAILURE;
  }
  return errcode_ret;
}

cl_int GpuChannelHost::CallclReleaseMemObject(cl_mem memobj) {
  // Sending a Sync IPC Message, to call a clReleaseMemObject API
  // in other process, and getting the results of the API.
  cl_int errcode_ret;
  cl_point point_memobj = (cl_point) memobj;

  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_ReleaseMemObject(
           point_memobj,
           &errcode_ret))) {
      return CL_SEND_IPC_MESSAGE_FAILURE;
  }
  return errcode_ret;
}

cl_int GpuChannelHost::CallclGetSupportedImageFormats(
    cl_context context,
    cl_mem_flags flags,
    cl_mem_object_type image_type,
    cl_uint num_entries,
    cl_image_format *image_formats,
    cl_uint *num_image_formats) {
  // Sending a Sync IPC Message, to call a clGetSupportedImageFormat API
  // in other process, and getting the results of the API.
  cl_int errcode_ret;
  cl_uint num_image_formats_inter = (cl_uint) -1;
  cl_point point_context = (cl_point) context;
  std::vector<cl_uint> image_format_list;
  std::vector<bool> return_variable_null_status;

  return_variable_null_status.resize(2);
  return_variable_null_status[0] = return_variable_null_status[1] = false;

  // The Sync Message can't get value back by NULL ptr, so if a
  // return back ptr is NULL, we must instead it using another
  // no-NULL ptr.
  if (NULL == num_image_formats) {
    num_image_formats = &num_image_formats_inter;
	return_variable_null_status[0] = true;
  }
  if (NULL == image_formats)
    return_variable_null_status[1] = true;

  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_GetSupportedImageFormats(
           point_context,
           flags,
           image_type,
           num_entries,
		   return_variable_null_status,
           &image_format_list,
           num_image_formats,
           &errcode_ret))) {
    return CL_SEND_IPC_MESSAGE_FAILURE;
  }

  // Dump the results of the Sync IPC Message calling.
  if (CL_SUCCESS == errcode_ret && image_formats) {
    for (cl_uint index = 0; index < num_entries; ++index) {
      image_formats[index].image_channel_data_type = image_format_list[index * 2];
      image_formats[index].image_channel_order = image_format_list[index * 2 + 1];
    }
  }

  return errcode_ret;
}

cl_int GpuChannelHost::CallclSetMemObjectDestructorCallback(
    cl_mem memobj,
    void (CL_CALLBACK *pfn_notify)(cl_mem, void*),
    void *user_data) {
  // Sending a Sync IPC Message, to call a clSetMemObjectDestructorCallback
  // API in other process, and getting the results of the API.
  cl_int errcode_ret;
  cl_point point_memobj = (cl_point) memobj;
  cl_point point_pfn_notify = (cl_point) pfn_notify;
  cl_point point_user_data = (cl_point) user_data;

  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_SetMemObjectDestructorCallback(
           point_memobj,
           point_pfn_notify,
           point_user_data,
           &errcode_ret))) {
    return CL_SEND_IPC_MESSAGE_FAILURE;
  }
  return errcode_ret;
}

cl_sampler GpuChannelHost::CallclCreateSampler(
    cl_context context,
    cl_bool normalized_coords,
    cl_addressing_mode addressing_mode,
    cl_filter_mode filter_mode,
    cl_int *errcode_ret) {
  // Sending a Sync IPC Message, to call a clCreateSampler API
  // in other process, and getting the results of the API.
  cl_int errcode_ret_inter;
  cl_point point_context = (cl_point) context;
  cl_point point_sampler_ret;
  std::vector<bool> return_variable_null_status;

  return_variable_null_status.resize(1);
  return_variable_null_status[0] = false;

  // The Sync Message can't get value back by NULL ptr, so if a
  // return back ptr is NULL, we must instead it using another
  // no-NULL ptr.
  if (NULL == errcode_ret) {
    errcode_ret = &errcode_ret_inter;
    return_variable_null_status[0] = true;
  }

  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_CreateSampler(
           point_context,
           normalized_coords,
           addressing_mode,
           filter_mode,
		   return_variable_null_status,
           errcode_ret,
           &point_sampler_ret))) {
    return NULL;
  }
  return (cl_sampler) point_sampler_ret;
}

cl_int GpuChannelHost::CallclRetainSampler(cl_sampler sampler)
{
  // Sending a Sync IPC Message, to call a clRetainSampler API
  // in other process, and getting the results of the API.
  cl_int errcode_ret;
  cl_point point_sampler = (cl_point) sampler;

  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_RetainSampler(
           point_sampler,
           &errcode_ret))) {
    return CL_SEND_IPC_MESSAGE_FAILURE;
  }
  return errcode_ret;
}

cl_int GpuChannelHost::CallclReleaseSampler(cl_sampler sampler)
{
  // Sending a Sync IPC Message, to call a clReleaseSampler API
  // in other process, and getting the results of the API.
  cl_int errcode_ret;
  cl_point point_sampler = (cl_point) sampler;

  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_ReleaseSampler(
           point_sampler,
           &errcode_ret))) {
    return CL_SEND_IPC_MESSAGE_FAILURE;
  }
  return errcode_ret;
}

cl_program GpuChannelHost::CallclCreateProgramWithSource(
    cl_context context,
    cl_uint count,
    const char **strings,
    const size_t *lengths,
    cl_int *errcode_ret) {
  // Sending a Sync IPC Message, to call a clCreateProgramWithSource API
  // in other process, and getting the results of the API.
  cl_int errcode_ret_inter = 0xFFFFFFF;
  cl_point point_program_ret;
  cl_point point_context = (cl_point) context;
  std::vector<std::string> string_list;
  std::vector<size_t> length_list;

  // The Sync Message can't get value back by NULL ptr, so if a
  // return back ptr is NULL, we must instead it using another
  // no-NULL ptr.
  if (NULL == errcode_ret)
    errcode_ret = &errcode_ret_inter;
  else if (0xFFFFFFF == *errcode_ret)
    *errcode_ret = 0;

  // Dump the inputs of the Sync IPC Message calling.
  string_list.clear();
  length_list.clear();
  for (cl_uint index = 0; index < count; ++index) {
    string_list.push_back(std::string(strings[index]));
    length_list.push_back(lengths[index]);
  }

  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_CreateProgramWithSource(
           point_context,
           count,
           string_list,
           length_list,
           errcode_ret,
           &point_program_ret))) {
    return NULL;
  }
  return (cl_program) point_program_ret;
}

cl_program GpuChannelHost::CallclCreateProgramWithBinary(
    cl_context context,
    cl_uint num_devices,
    const cl_device_id *device_list,
    const size_t *lengths,
    const unsigned char **binaries,
    cl_int *binary_status,
    cl_int *errcode_ret) {
  // Sending a Sync IPC Message, to call a clCreateProgramWithBinary API
  // in other process, and getting the results of the API.
  cl_int errcode_ret_inter = 0xFFFFFFF;
  cl_point point_program_ret;
  cl_point point_context = (cl_point) context;
  std::vector<cl_point> point_device_list;
  std::vector<size_t> length_list;
  std::vector<std::vector<unsigned char>> binary_list;
  std::vector<cl_int> binary_status_list;

  // The Sync Message can't get value back by NULL ptr, so if a
  // return back ptr is NULL, we must instead it using another
  // no-NULL ptr.
  if (NULL == errcode_ret)
    errcode_ret = &errcode_ret_inter;
  else if (0xFFFFFFF == *errcode_ret)
    *errcode_ret = 0;

  // Dump the inputs of the Sync IPC Message calling.
  // We need to add some better ways to improve
  // the performance of transfer kernel.
  // So this API is not fully supported.

  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_CreateProgramWithBinary(
           point_context,
           num_devices,
           point_device_list,
           length_list,
           binary_list,
           &binary_status_list,
           errcode_ret,
           &point_program_ret))) {
    return NULL;
  }
  return (cl_program) point_program_ret;
}

cl_program GpuChannelHost::CallclCreateProgramWithBuiltInKernels(
    cl_context context,
    cl_uint num_devices,
    const cl_device_id *device_list,
    const char *kernel_names,
    cl_int *errcode_ret) {
  // Sending a Sync IPC Message, to call a clCreateProgramWithBuiltInKernels
  // API in other process, and getting the results of the API.
  cl_int errcode_ret_inter = 0xFFFFFFF;
  cl_point point_program_ret;
  cl_point point_context = (cl_point) context;
  std::vector<cl_point> point_device_list;
  std::string str_kernel_names(kernel_names); // Here may crash.

  // The Sync Message can't get value back by NULL ptr, so if a
  // return back ptr is NULL, we must instead it using another
  // no-NULL ptr.
  if (NULL == errcode_ret)
    errcode_ret = &errcode_ret_inter;
  else if (0xFFFFFFF == *errcode_ret)
    *errcode_ret = 0;

  // Dump the inputs of the Sync IPC Message calling.
  point_device_list.clear();
  for (cl_uint index = 0; index < num_devices; ++index)
    point_device_list.push_back((cl_point) device_list[index]);

  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_CreateProgramWithBuiltInKernels(
           point_context,
           num_devices,
           point_device_list,
           str_kernel_names,
           errcode_ret,
           &point_program_ret))) {
    return NULL;
  }
  return (cl_program) point_program_ret;
}

cl_int GpuChannelHost::CallclRetainProgram(cl_program program)
{
  // Sending a Sync IPC Message, to call a clRetainProgram
  // API in other process, and getting the results of the API.
  cl_int errcode_ret;
  cl_point point_program = (cl_point) program;

  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_RetainProgram(
           point_program,
           &errcode_ret))) {
    return CL_SEND_IPC_MESSAGE_FAILURE;
  }
  return errcode_ret;
}

cl_int GpuChannelHost::CallclReleaseProgram(cl_program program)
{
  // Sending a Sync IPC Message, to call a clReleaseProgram
  // API in other process, and getting the results of the API.
  cl_int errcode_ret;
  cl_point point_program = (cl_point) program;

  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_ReleaseProgram(
           point_program,
           &errcode_ret))) {
    return CL_SEND_IPC_MESSAGE_FAILURE;
  }
  return errcode_ret;
}

cl_int GpuChannelHost::CallclBuildProgram(
    cl_program program,
    cl_uint num_devices,
    const cl_device_id *device_list,
    const char *options,
    void (CL_CALLBACK* pfn_notify)(cl_program, void*),
    void *user_data) {
  // Sending a Sync IPC Message, to call a clBuildProgram API
  // in other process, and getting the results of the API.
  cl_int errcode_ret;
  cl_point point_program = (cl_point) program;
  std::vector<cl_point> point_device_list;
  std::string str_options(options); // if option == NULL, what will happen??
  std::vector<cl_point> point_pfn_list;

  // Dump the inputs of the Sync IPC Message calling.
  point_pfn_list.push_back((cl_point) pfn_notify);
  point_pfn_list.push_back((cl_point) user_data);

  point_device_list.clear();
  for (cl_uint index = 0; index < num_devices; ++index)
    point_device_list.push_back((cl_point) device_list[index]);
  
  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_BuildProgram(
           point_program,
           num_devices,
           point_device_list,
           str_options,
           point_pfn_list,
           &errcode_ret))) {
    return CL_SEND_IPC_MESSAGE_FAILURE;
  }
  return errcode_ret;
}

cl_int GpuChannelHost::CallclCompileProgram(
    cl_program program,
    cl_uint num_devices,
    const cl_device_id *device_list,
    const char *options,
    cl_uint num_input_headers,
    const cl_program *input_headers,
    const char **header_include_names,
    void (CL_CALLBACK* pfn_notify)(cl_program, void*),
    void *user_data) {
  // Sending a Sync IPC Message, to call a clCompileProgram API
  // in other process, and getting the results of the API.
  cl_int errcode_ret;
  std::vector<cl_point> point_parameter_list;
  std::vector<cl_point> point_device_list;
  std::vector<cl_point> point_input_header_list;
  std::vector<cl_uint> num_list;
  std::vector<std::string> options_header_include_name_list;

  // Dump the inputs of the Sync IPC Message calling.
  point_parameter_list.clear();
  point_parameter_list.push_back((cl_point) program);
  point_parameter_list.push_back((cl_point) pfn_notify);
  point_parameter_list.push_back((cl_point) user_data);

  num_list.clear();
  num_list.push_back(num_devices);
  num_list.push_back(num_input_headers);

  point_device_list.clear();
  for (cl_uint index = 0; index < num_devices; ++index)
    point_device_list.push_back((cl_point) device_list[index]);

  point_input_header_list.clear();
  options_header_include_name_list.clear();
  options_header_include_name_list.push_back(std::string(options));
  for (cl_uint index = 0; index < num_input_headers; ++index) {
    point_input_header_list.push_back((cl_point) input_headers[index]);
    options_header_include_name_list.push_back(
        std::string(header_include_names[index]));
  }

  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_CompileProgram(
           point_parameter_list,
           num_list,
           point_device_list,
           options_header_include_name_list,
           point_input_header_list,
           &errcode_ret))) {
    return CL_SEND_IPC_MESSAGE_FAILURE;
  }
  return errcode_ret;
}

cl_program GpuChannelHost::CallclLinkProgram(
    cl_context context,
    cl_uint num_devices,
    const cl_device_id *device_list,
    const char *options,
    cl_uint num_input_programs,
    const cl_program *input_programs,
    void (CL_CALLBACK* pfn_notify)(cl_program, void*),
    void *user_data,
    cl_int *errcode_ret) {
  // Sending a Sync IPC Message, to call a clLinkProgram API
  // in other process, and getting the results of the API.
  cl_int errcode_ret_inter = 0xFFFFFFF;
  cl_point point_program_ret;
  std::vector<cl_point> point_parameter_list;
  std::vector<cl_point> point_device_list;
  std::vector<cl_point> point_input_program_list;
  std::vector<cl_uint> num_list;
  std::string str_options(options); // Will it Crash?

  // The Sync Message can't get value back by NULL ptr, so if a
  // return back ptr is NULL, we must instead it using another
  // no-NULL ptr.
  if (NULL == errcode_ret)
    errcode_ret = &errcode_ret_inter;
  else if (0xFFFFFFF == *errcode_ret)
    *errcode_ret = 0;

  // Dump the inputs of the Sync IPC Message calling.
  point_parameter_list.clear();
  point_parameter_list.push_back((cl_point) context);
  point_parameter_list.push_back((cl_point) pfn_notify);
  point_parameter_list.push_back((cl_point) user_data);

  num_list.clear();
  num_list.push_back(num_devices);
  num_list.push_back(num_input_programs);

  point_device_list.clear();
  for (cl_uint index = 0; index < num_devices; ++index)
    point_device_list.push_back((cl_point) device_list[index]);

  point_input_program_list.clear();
  for (cl_uint index = 0; index < num_input_programs; ++index)
    point_input_program_list.push_back((cl_point) input_programs[index]);

  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_LinkProgram(
           point_parameter_list,
           num_list,
           point_device_list,
           point_input_program_list,
           str_options,
           errcode_ret,
           &point_program_ret))) {
    return NULL;
  }
  return (cl_program) point_program_ret;
}

cl_int GpuChannelHost::CallclUnloadPlatformCompiler(cl_platform_id platform) {
  // Sending a Sync IPC Message, to call a clUnloadPlatformCompiler API
  // in other process, and getting the results of the API.
  cl_int errcode_ret;
  cl_point point_platform = (cl_point) platform;

  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_UnloadPlatformCompiler(
           point_platform,
           &errcode_ret))) {
    return CL_SEND_IPC_MESSAGE_FAILURE;
  }
  return errcode_ret;
}

cl_kernel GpuChannelHost::CallclCreateKernel(
    cl_program program,
    const char *kernel_name,
    cl_int *errcode_ret) {
  // Sending a Sync IPC Message, to call a clCreateKernel API
  // in other process, and getting the results of the API.
  cl_int errcode_ret_inter = 0xFFFFFFF;
  cl_point point_kernel_ret;
  cl_point point_program = (cl_point) program;
  std::string str_kernel_name(kernel_name);

  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_CreateKernel(
           point_program,
           str_kernel_name,
           errcode_ret,
           &point_kernel_ret))) {
    return NULL;
  }
  return (cl_kernel) point_kernel_ret;
}

cl_int GpuChannelHost::CallclCreateKernelsInProgram(
    cl_program program,
    cl_uint num_kernels,
    cl_kernel *kernels,
    cl_uint *num_kernels_ret) {
  // Sending a Sync IPC Message, to call a clCreateKernel API
  // in other process, and getting the results of the API.
  cl_int errcode_ret;
  cl_uint num_kernels_ret_inter = (cl_uint) -1;
  cl_point point_program = (cl_point) program;
  std::vector<cl_point> point_kernel_list;


  // The Sync Message can't get value back by NULL ptr, so if a
  // return back ptr is NULL, we must instead it using another
  // no-NULL ptr.
  if (NULL == num_kernels_ret)
    num_kernels_ret = &num_kernels_ret_inter;
  else if ((cl_uint) -1 == *num_kernels_ret)
    *num_kernels_ret = 0;

  // Dump the inputs of the Sync IPC Message calling.
  point_kernel_list.clear();
  for (cl_uint index = 0; index < num_kernels; ++index)
    point_kernel_list.push_back((cl_point) kernels[index]);

  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_CreateKernelsInProgram(
           point_program,
           num_kernels,
           point_kernel_list,
           num_kernels_ret,
           &errcode_ret))) {
    return CL_SEND_IPC_MESSAGE_FAILURE;
  }
  return errcode_ret;
}

cl_int GpuChannelHost::CallclRetainKernel(cl_kernel kernel) {
  // Sending a Sync IPC Message, to call a clRetainKernel
  // API in other process, and getting the results of the API.
  cl_int errcode_ret;
  cl_point point_kernel = (cl_point) kernel;

  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_RetainKernel(
           point_kernel,
           &errcode_ret))) {
    return CL_SEND_IPC_MESSAGE_FAILURE;
  }
  return errcode_ret;
}

cl_int GpuChannelHost::CallclReleaseKernel(cl_kernel kernel) {
  // Sending a Sync IPC Message, to call a clReleaseKernel
  // API in other process, and getting the results of the API.
  cl_int errcode_ret;
  cl_point point_kernel = (cl_point) kernel;

  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_ReleaseKernel(
           point_kernel,
           &errcode_ret))) {
    return CL_SEND_IPC_MESSAGE_FAILURE;
  }
  return errcode_ret;
}

cl_int GpuChannelHost::CallclSetKernelArg(
    cl_kernel kernel,
    cl_uint arg_index,
    size_t arg_size,
    const void *arg_value) {
  // Sending a Sync IPC Message, to call a clSetKernelArg API
  // in other process, and getting the results of the API.
  cl_int errcode_ret;
  cl_point point_kernel = (cl_point) kernel;
  cl_point point_arg_value = (cl_point) arg_value;

  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_SetKernelArg(
           point_kernel,
           arg_index,
           arg_size,
           point_arg_value,
           &errcode_ret))) {
    return CL_SEND_IPC_MESSAGE_FAILURE;
  }
  return errcode_ret;
}

cl_int GpuChannelHost::CallclWaitForEvents(
     cl_uint num_events,
     const cl_event *event_list) {
  // Sending a Sync IPC Message, to call a clWaitForEvents API
  // in other process, and getting the results of the API.
  cl_int errcode_ret;
  std::vector<cl_point> point_event_list;

  // Dump the inputs of the Sync IPC Message calling.
  point_event_list.clear();
  for (cl_uint index = 0; index < num_events; ++index)
    point_event_list.push_back((cl_point) event_list[index]);

  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_WaitForEvents(
           num_events,
           point_event_list,
           &errcode_ret))) {
    return CL_SEND_IPC_MESSAGE_FAILURE;
  }
  return errcode_ret;
}

cl_event GpuChannelHost::CallclCreateUserEvent(
       cl_context context,
       cl_int *errcode_ret) {
  // Sending a Sync IPC Message, to call a clCreateUserEvent
  // API in other process, and getting the results of the API.
  cl_int errcode_ret_inter = 0xFFFFFFF;
  cl_point point_out_context;
  cl_point point_in_context = (cl_point) context;

  // The Sync Message can't get value back by NULL ptr, so if a
  // return back ptr is NULL, we must instead it using another
  // no-NULL ptr.
  if (NULL == errcode_ret)
    errcode_ret = &errcode_ret_inter;
  else if (0xFFFFFFF == *errcode_ret)
    *errcode_ret = 0;

  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_CreateUserEvent(
           point_in_context,
           errcode_ret,
           &point_out_context))) {
    return NULL;
  }
  return (cl_event) point_out_context;
}

cl_int GpuChannelHost::CallclRetainEvent(cl_event clevent) {
  // Sending a Sync IPC Message, to call a clRetainEvent
  // API in other process, and getting the results of the API.
  cl_int errcode_ret;
  cl_point point_event = (cl_point) clevent;

  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_RetainEvent(
           point_event,
           &errcode_ret))) {
    return CL_SEND_IPC_MESSAGE_FAILURE;
  }
  return errcode_ret;
}

cl_int GpuChannelHost::CallclReleaseEvent(cl_event clevent) {
  // Sending a Sync IPC Message, to call a clReleaseEvent
  // API in other process, and getting the results of the API.
  cl_int errcode_ret;
  cl_point point_event = (cl_point) clevent;
  
  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_ReleaseEvent(
           point_event,
           &errcode_ret))) {
    return CL_SEND_IPC_MESSAGE_FAILURE;
  }
  return errcode_ret;
}

cl_int GpuChannelHost::CallclSetUserEventStatus(
     cl_event clevent,
     cl_int execution_status) {
  // Sending a Sync IPC Message, to call a clSetUserEventStatus
  // API in other process, and getting the results of the API.
  cl_int errcode_ret;
  cl_point point_event = (cl_point) clevent;

  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_SetUserEventStatus(
           point_event,
           execution_status,
           &errcode_ret))) {
      return CL_SEND_IPC_MESSAGE_FAILURE;
  }
  return errcode_ret;
}

cl_int GpuChannelHost::CallclSetEventCallback(
    cl_event clevent,
    cl_int command_exec_callback_type,
    void (CL_CALLBACK *pfn_event_notify)(cl_event, cl_int,void *),
    void *user_data) {
  // Sending a Sync IPC Message, to call a clSetEventCallback
  // API in other process, and getting the results of the API.
  cl_int errcode_ret;
  cl_point point_event = (cl_point) clevent;
  cl_point point_pfn_notify = (cl_point) pfn_event_notify;
  cl_point point_user_data = (cl_point) user_data;

  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_SetEventCallback(
           point_event,
           command_exec_callback_type,
           point_pfn_notify,
           point_user_data,
           &errcode_ret))) {
    return CL_SEND_IPC_MESSAGE_FAILURE;
  }
  return errcode_ret;
}

cl_int GpuChannelHost::CallclFlush(cl_command_queue command_queue) {
  // Sending a Sync IPC Message, to call a clFlush
  // API in other process, and getting the results of the API.
  cl_int errcode_ret;
  cl_point point_command_queue = (cl_point) command_queue;
 
  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_Flush(
           point_command_queue,
           &errcode_ret))) {
    return CL_SEND_IPC_MESSAGE_FAILURE;
  }
  return errcode_ret;
}

cl_int GpuChannelHost::CallclFinish(cl_command_queue command_queue) {
  // Sending a Sync IPC Message, to call a clFinish
  // API in other process, and getting the results of the API.
  cl_int errcode_ret;
  cl_point point_command_queue = (cl_point) command_queue;

  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_Finish(
           point_command_queue,
           &errcode_ret))) {
    return CL_SEND_IPC_MESSAGE_FAILURE;
  }
  return errcode_ret;
}

cl_int GpuChannelHost::CallclGetPlatformInfo(
    cl_platform_id platform,
    cl_platform_info param_name,
    size_t param_value_size,
    void* param_value,
    size_t* param_value_size_ret) {
  // Sending a Sync IPC Message, to call a clCreateCommandQueue API
  // in other process, and getting the results of the API.
  cl_int errcode_ret;
  cl_point point_platform = (cl_point) platform;
  std::string string_ret;
  size_t param_value_size_ret_inter = (size_t) -1;
  cl_bool is_param_null = CL_FALSE;

  // The Sync Message can't get value back by NULL ptr, so if a
  // return back ptr is NULL, we must instead it using another
  // no-NULL ptr.
  if (param_value_size_ret == NULL)
    param_value_size_ret = &param_value_size_ret_inter;
  else if ((size_t) -1 == *param_value_size_ret)
    *param_value_size_ret = 0;

  if (NULL == param_value)
    is_param_null = CL_TRUE;

  // Send a Sync IPC Message and wait for the results.
  if (!Send(new OpenCLChannelMsg_GetPlatformInfo_string(
           point_platform,
           param_name,
           param_value_size,
           is_param_null,
           &string_ret,
           param_value_size_ret,
           &errcode_ret))) {
    return CL_SEND_IPC_MESSAGE_FAILURE;
  }

  // Dump the results of the Sync IPC Message calling.
  if (!is_param_null && CL_SUCCESS == errcode_ret)
    strcpy((char*)param_value,string_ret.c_str());

  return errcode_ret;
}

cl_int GpuChannelHost::CallclGetDeviceInfo(
    cl_device_id device,
    cl_device_info param_name,
    size_t param_value_size, 
    void* param_value,
    size_t* param_value_size_ret) {
  // Sending a Sync IPC Message, to call a clCreateSubDevices API
  // in other process, and getting the results of the API.
  cl_int errcode_ret;
  size_t param_value_size_ret_inter = (size_t) -1;
  cl_point point_device = (cl_point) device;  
  cl_uint cl_uint_ret;
  std::vector<size_t> size_t_list_ret;
  size_t size_t_ret;
  cl_ulong cl_ulong_ret;
  std::string string_ret;
  cl_point cl_point_ret;
  std::vector<intptr_t> intptr_t_list_ret;  
  cl_bool is_param_null = CL_FALSE;

  // The Sync Message can't get value back by NULL ptr, so if a
  // return back ptr is NULL, we must instead it using another
  // no-NULL ptr.
  if (param_value_size_ret == NULL)
    param_value_size_ret = &param_value_size_ret_inter;
  else if ((size_t) -1 == *param_value_size_ret)
    *param_value_size_ret = 0;
 
  if (NULL == param_value)
    is_param_null = CL_TRUE;

  switch(param_name) {
    case CL_DEVICE_TYPE:
    case CL_DEVICE_VENDOR_ID:
    case CL_DEVICE_MAX_COMPUTE_UNITS:
    case CL_DEVICE_MAX_WORK_ITEM_DIMENSIONS:
    case CL_DEVICE_PREFERRED_VECTOR_WIDTH_CHAR:
    case CL_DEVICE_PREFERRED_VECTOR_WIDTH_SHORT:
    case CL_DEVICE_PREFERRED_VECTOR_WIDTH_INT:
    case CL_DEVICE_PREFERRED_VECTOR_WIDTH_LONG:
    case CL_DEVICE_PREFERRED_VECTOR_WIDTH_FLOAT:
    case CL_DEVICE_PREFERRED_VECTOR_WIDTH_DOUBLE:
    case CL_DEVICE_PREFERRED_VECTOR_WIDTH_HALF:
    case CL_DEVICE_NATIVE_VECTOR_WIDTH_CHAR:
    case CL_DEVICE_NATIVE_VECTOR_WIDTH_SHORT:
    case CL_DEVICE_NATIVE_VECTOR_WIDTH_INT:
    case CL_DEVICE_NATIVE_VECTOR_WIDTH_LONG:
    case CL_DEVICE_NATIVE_VECTOR_WIDTH_FLOAT:
    case CL_DEVICE_NATIVE_VECTOR_WIDTH_DOUBLE:
    case CL_DEVICE_NATIVE_VECTOR_WIDTH_HALF:
    case CL_DEVICE_ADDRESS_BITS:
    case CL_DEVICE_MAX_READ_IMAGE_ARGS:
    case CL_DEVICE_MAX_WRITE_IMAGE_ARGS:
    case CL_DEVICE_MAX_SAMPLERS:
    case CL_DEVICE_MEM_BASE_ADDR_ALIGN:
    case CL_DEVICE_GLOBAL_MEM_CACHELINE_SIZE:
    case CL_DEVICE_MAX_CONSTANT_ARGS:
    case CL_DEVICE_PARTITION_MAX_SUB_DEVICES:
    case CL_DEVICE_REFERENCE_COUNT:
    case CL_DEVICE_IMAGE_SUPPORT:
    case CL_DEVICE_LOCAL_MEM_TYPE:
    case CL_DEVICE_ERROR_CORRECTION_SUPPORT:
    case CL_DEVICE_HOST_UNIFIED_MEMORY:
    case CL_DEVICE_ENDIAN_LITTLE:
    case CL_DEVICE_AVAILABLE:
    case CL_DEVICE_COMPILER_AVAILABLE:
    case CL_DEVICE_LINKER_AVAILABLE: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetDeviceInfo_cl_uint(
               point_device,
               param_name,
               param_value_size,
               is_param_null,
               &cl_uint_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }

      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        *(cl_uint*) param_value = cl_uint_ret;

      return errcode_ret;
    }
    case CL_DEVICE_MAX_WORK_ITEM_SIZES: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetDeviceInfo_size_t_list(
               point_device,
               param_name,
               param_value_size,
               is_param_null,
               &size_t_list_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }

      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        for (cl_uint index = 0; index < param_value_size/sizeof(size_t); ++index)
          ((size_t*) (param_value))[index] = size_t_list_ret[index];

      return errcode_ret;
    }
    case CL_DEVICE_MAX_WORK_GROUP_SIZE:
    case CL_DEVICE_IMAGE2D_MAX_WIDTH:
    case CL_DEVICE_IMAGE2D_MAX_HEIGHT:
    case CL_DEVICE_IMAGE3D_MAX_WIDTH:
    case CL_DEVICE_IMAGE3D_MAX_HEIGHT:
    case CL_DEVICE_IMAGE3D_MAX_DEPTH:
    case CL_DEVICE_IMAGE_MAX_BUFFER_SIZE:
    case CL_DEVICE_IMAGE_MAX_ARRAY_SIZE:
    case CL_DEVICE_MAX_PARAMETER_SIZE:
    case CL_DEVICE_PROFILING_TIMER_RESOLUTION:
    case CL_DEVICE_PRINTF_BUFFER_SIZE: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetDeviceInfo_size_t(
               point_device,
               param_name,
               param_value_size,
               is_param_null,
               &size_t_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }

      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        *(size_t*) param_value = size_t_ret;

      return errcode_ret;
    }
    case CL_DEVICE_MAX_MEM_ALLOC_SIZE:
    case CL_DEVICE_SINGLE_FP_CONFIG:
    case CL_DEVICE_DOUBLE_FP_CONFIG:
    case CL_DEVICE_GLOBAL_MEM_CACHE_SIZE:
    case CL_DEVICE_GLOBAL_MEM_SIZE:
    case CL_DEVICE_MAX_CONSTANT_BUFFER_SIZE:
    case CL_DEVICE_LOCAL_MEM_SIZE:
    case CL_DEVICE_EXECUTION_CAPABILITIES:
    case CL_DEVICE_QUEUE_PROPERTIES:
    case CL_DEVICE_PARTITION_AFFINITY_DOMAIN: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetDeviceInfo_cl_ulong(
               point_device,
               param_name,
               param_value_size,
               is_param_null,
               &cl_ulong_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }
      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        *(cl_ulong*) param_value = cl_ulong_ret;

      return errcode_ret;
    }
    case CL_DEVICE_BUILT_IN_KERNELS:
    case CL_DEVICE_NAME:
    case CL_DEVICE_VENDOR:
    case CL_DRIVER_VERSION:
    case CL_DEVICE_PROFILE:
    case CL_DEVICE_VERSION:
    case CL_DEVICE_OPENCL_C_VERSION:
    case CL_DEVICE_EXTENSIONS: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetDeviceInfo_string(
               point_device,
               param_name,
               param_value_size,
               is_param_null,
               &string_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }

      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        strcpy((char*)param_value,string_ret.c_str());

      return errcode_ret;
    }
    case CL_DEVICE_PARENT_DEVICE: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetDeviceInfo_cl_point(
               point_device,
               param_name,
               param_value_size,
               is_param_null,
               &cl_point_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }
    
      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        *(cl_device_id*) param_value = (cl_device_id) cl_point_ret;
    
      return errcode_ret;
    }
    case CL_DEVICE_PARTITION_PROPERTIES:
    case CL_DEVICE_PARTITION_TYPE: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetDeviceInfo_intptr_t_list(
               point_device,
               param_name,
               param_value_size,
               is_param_null,
               &intptr_t_list_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }

      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        for (cl_uint index = 0; index < param_value_size/sizeof(intptr_t); ++index)
          ((intptr_t*) (param_value))[index] = intptr_t_list_ret[index];

      return errcode_ret;
    }
    default: return CL_SEND_IPC_MESSAGE_FAILURE;
  }
}

cl_int GpuChannelHost::CallclGetContextInfo(
    cl_context context, 
    cl_context_info param_name, 
    size_t param_value_size, 
    void *param_value, 
    size_t *param_value_size_ret) {
  // Sending a Sync IPC Message, to call a clCreateSubDevices API
  // in other process, and getting the results of the API.
  cl_int errcode_ret;
  cl_point point_context = (cl_point) context;  
  cl_uint cl_uint_ret;
  std::vector<cl_point> cl_point_list_ret;
  std::vector<intptr_t> intptr_t_list_ret;
  cl_bool is_param_null = CL_FALSE;  
  size_t param_value_size_ret_inter = (size_t) -1;
  
  // The Sync Message can't get value back by NULL ptr, so if a
  // return back ptr is NULL, we must instead it using another
  // no-NULL ptr.
  if (param_value_size_ret == NULL)
    param_value_size_ret = &param_value_size_ret_inter;
  else if ((size_t) -1 == *param_value_size_ret)
    *param_value_size_ret = 0;
 
  if (NULL == param_value)
    is_param_null = CL_TRUE;

  switch(param_name) {
    case CL_CONTEXT_REFERENCE_COUNT:
    case CL_CONTEXT_NUM_DEVICES: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetContextInfo_cl_uint(
               point_context,
               param_name,
               param_value_size,
               is_param_null,
               &cl_uint_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }
    
      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        *(cl_uint*) param_value = cl_uint_ret;
      
      return errcode_ret;
    }
    case CL_CONTEXT_DEVICES: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetContextInfo_cl_point_list(
               point_context,
               param_name,
               param_value_size,
               is_param_null,
               &cl_point_list_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }
    
      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        for (cl_uint index = 0; index < param_value_size/sizeof(cl_point); ++index)
          ((cl_device_id*) (param_value))[index] = (cl_device_id)cl_point_list_ret[index];
    
      return errcode_ret;
    }
    case CL_CONTEXT_PROPERTIES: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetContextInfo_intptr_t_list(
               point_context,
               param_name,
               param_value_size,
               is_param_null,
               &intptr_t_list_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }
    
      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)    
        for (cl_uint index = 0; index < param_value_size/sizeof(intptr_t); ++index)
          ((cl_context_properties*) (param_value))[index] = (cl_context_properties)intptr_t_list_ret[index];
    
      return errcode_ret;
    }
    default: return CL_SEND_IPC_MESSAGE_FAILURE;
  }
}
cl_int GpuChannelHost::CallclGetCommandQueueInfo(
    cl_command_queue command_queue, 
    cl_command_queue_info param_name, 
    size_t param_value_size,
    void *param_value,
    size_t *param_value_size_ret) {
  // Sending a Sync IPC Message, to call a clCreateSubDevices API
  // in other process, and getting the results of the API.
  cl_int errcode_ret; 
  cl_point point_command_queue = (cl_point) command_queue;
  cl_point cl_point_ret;
  cl_uint cl_uint_ret;
  cl_ulong cl_ulong_ret;
  size_t param_value_size_ret_inter = (size_t) -1;
  cl_bool is_param_null = CL_FALSE;

  // The Sync Message can't get value back by NULL ptr, so if a
  // return back ptr is NULL, we must instead it using another
  // no-NULL ptr.
  if (param_value_size_ret == NULL)
    param_value_size_ret = &param_value_size_ret_inter;
  else if ((size_t) -1 == *param_value_size_ret)
    *param_value_size_ret = 0;

  if (NULL == param_value)
    is_param_null = CL_TRUE;

  switch(param_name) {
    case CL_QUEUE_CONTEXT: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetCommandQueueInfo_cl_point(
               point_command_queue,
               param_name,
               param_value_size,
               is_param_null,
               &cl_point_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }
    
      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        *(cl_context*) param_value = (cl_context) cl_point_ret;
     
      return errcode_ret;
    }
    case CL_QUEUE_DEVICE: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetCommandQueueInfo_cl_point(
               point_command_queue,
               param_name,
               param_value_size,
               is_param_null,
               &cl_point_ret, 
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }
    
      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        *(cl_device_id*) param_value = (cl_device_id) cl_point_ret;
    
      return errcode_ret;
    }
    case CL_QUEUE_REFERENCE_COUNT: {
      if (!Send(new OpenCLChannelMsg_GetCommandQueueInfo_cl_uint(
               point_command_queue,
               param_name, 
               param_value_size,
               is_param_null,
               &cl_uint_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }
    
      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        *(cl_uint*) param_value = cl_uint_ret;
    
      return errcode_ret;
    }
    case CL_QUEUE_PROPERTIES: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetCommandQueueInfo_cl_ulong(
               point_command_queue, 
               param_name,
               param_value_size,
               is_param_null,
               &cl_ulong_ret,
               param_value_size_ret, 
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }
      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        *(cl_ulong*) param_value = cl_ulong_ret;
    
      return errcode_ret;
    }
    default: return CL_SEND_IPC_MESSAGE_FAILURE;
  }
}

cl_int GpuChannelHost::CallclGetMemObjectInfo(
    cl_mem memobj,
    cl_mem_info param_name,
    size_t param_value_size,
    void *param_value,
    size_t *param_value_size_ret) {
  // Sending a Sync IPC Message, to call a clCreateSubDevices API
  // in other process, and getting the results of the API.
  cl_int errcode_ret;
  cl_point point_memobj = (cl_point) memobj;
  cl_uint cl_uint_ret;
  cl_ulong cl_ulong_ret;
  size_t size_t_ret;
  cl_point cl_point_ret;
  size_t param_value_size_ret_inter = (size_t) -1;
  cl_bool is_param_null = CL_FALSE;

  // The Sync Message can't get value back by NULL ptr, so if a
  // return back ptr is NULL, we must instead it using another
  // no-NULL ptr.
  if (param_value_size_ret == NULL)
    param_value_size_ret = &param_value_size_ret_inter;
  else if ((size_t) -1 == *param_value_size_ret)
    *param_value_size_ret = 0;

  if (NULL == param_value)
    is_param_null = CL_TRUE;

  switch(param_name) {
    case CL_MEM_TYPE:
    case CL_MEM_MAP_COUNT:
    case CL_MEM_REFERENCE_COUNT: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetMemObjectInfo_cl_uint(
               point_memobj,
               param_name,
               param_value_size, 
               is_param_null,
               &cl_uint_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }
    
      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        *(cl_uint*) param_value = cl_uint_ret;
    
      return errcode_ret;
    }
    case CL_MEM_FLAGS: {
      if (!Send(new OpenCLChannelMsg_GetMemObjectInfo_cl_ulong(
               point_memobj, 
               param_name,
               param_value_size,
               is_param_null,
               &cl_ulong_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }
    
      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        *(cl_ulong*) param_value = cl_ulong_ret;

      return errcode_ret;
    }
    case CL_MEM_SIZE:
    case CL_MEM_OFFSET: {
      if (!Send(new OpenCLChannelMsg_GetMemObjectInfo_size_t(
               point_memobj,
               param_name,
               param_value_size,
               is_param_null,
               &size_t_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }
    
      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        *(size_t*) param_value = size_t_ret;
    
      return errcode_ret;
    }
    case CL_MEM_HOST_PTR: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetMemObjectInfo_cl_point(
               point_memobj,
               param_name,
               param_value_size,
               is_param_null,
               &cl_point_ret,
               param_value_size_ret,
               &errcode_ret))) {
       return CL_SEND_IPC_MESSAGE_FAILURE;
     }
    
      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        *(cl_int**) param_value = (cl_int*) cl_point_ret;
    
      return errcode_ret;
    }
    case CL_MEM_CONTEXT: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetMemObjectInfo_cl_point(
               point_memobj,
               param_name, 
               param_value_size,
               is_param_null,
               &cl_point_ret, 
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }
    
      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        *(cl_context*) param_value = (cl_context) cl_point_ret;

      return errcode_ret;
    }
    case CL_MEM_ASSOCIATED_MEMOBJECT: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetMemObjectInfo_cl_point(
               point_memobj,
               param_name,
               param_value_size,
               is_param_null,
               &cl_point_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }
    
      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        *(cl_mem *) param_value = (cl_mem) cl_point_ret;
    
      return errcode_ret;
    }
    default: return CL_SEND_IPC_MESSAGE_FAILURE;
  }
}
cl_int GpuChannelHost::CallclGetImageInfo(
    cl_mem image,
    cl_image_info param_name,
    size_t param_value_size,
    void *param_value,
    size_t *param_value_size_ret) {
  // Sending a Sync IPC Message, to call a clCreateSubDevices API
  // in other process, and getting the results of the API.
  cl_int errcode_ret;
  cl_point point_image = (cl_point) image;
  size_t size_t_ret;
  cl_point cl_point_ret;
  cl_uint cl_uint_ret;
  std::vector<cl_uint> image_format_list_ret;
  size_t param_value_size_ret_inter = (size_t) -1;
  cl_bool is_param_null = CL_FALSE;

  // The Sync Message can't get value back by NULL ptr, so if a
  // return back ptr is NULL, we must instead it using another
  // no-NULL ptr. 
  if (param_value_size_ret == NULL)
    param_value_size_ret = &param_value_size_ret_inter;
  else if ((size_t) -1 == *param_value_size_ret) 
    *param_value_size_ret = 0;

  if (NULL == param_value)
    is_param_null = CL_TRUE;

  switch(param_name) {
    case CL_IMAGE_FORMAT: {
      if (!Send(new OpenCLChannelMsg_GetImageInfo_cl_image_format(
               point_image,
               param_name,
               param_value_size,
               is_param_null,
               &image_format_list_ret,
               param_value_size_ret, 
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }

      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret) {  
        (*(cl_image_format*) param_value).image_channel_data_type = image_format_list_ret[0];           
        (*(cl_image_format*) param_value).image_channel_order = image_format_list_ret[1];
      }

      return errcode_ret;
    }
    case CL_IMAGE_ELEMENT_SIZE:
    case CL_IMAGE_ROW_PITCH:
    case CL_IMAGE_SLICE_PITCH:
    case CL_IMAGE_WIDTH:
    case CL_IMAGE_HEIGHT:
    case CL_IMAGE_DEPTH:
    case CL_IMAGE_ARRAY_SIZE: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetImageInfo_size_t(
               point_image,
               param_name,
               param_value_size,
               is_param_null,
               &size_t_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }
      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        *(size_t*) param_value = size_t_ret;

      return errcode_ret;
    }
    case CL_IMAGE_BUFFER: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetImageInfo_cl_point(
               point_image, 
               param_name,
               param_value_size,
               is_param_null,
               &cl_point_ret,
               param_value_size_ret, 
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }

      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        *(cl_mem*) param_value = (cl_mem) cl_point_ret;

      return errcode_ret;
    }
    case CL_IMAGE_NUM_MIP_LEVELS:
    case CL_IMAGE_NUM_SAMPLES: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetImageInfo_cl_uint(
               point_image,
               param_name,
               param_value_size,
               is_param_null,
               &cl_uint_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }
    
      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        *(cl_uint*) param_value = cl_uint_ret;
    
      return errcode_ret;
    }
    default:return CL_SEND_IPC_MESSAGE_FAILURE;
  }
}
cl_int GpuChannelHost::CallclGetSamplerInfo(
    cl_sampler sampler,
    cl_sampler_info param_name,
    size_t param_value_size,
    void *param_value,
    size_t *param_value_size_ret) {
  // Sending a Sync IPC Message, to call a clCreateSubDevices API
  // in other process, and getting the results of the API.
  cl_int errcode_ret;
  cl_point point_sampler = (cl_point) sampler;
  cl_uint cl_uint_ret;
  cl_point cl_point_ret;
  size_t param_value_size_ret_inter = (size_t) -1;
  cl_bool is_param_null = CL_FALSE;

  // The Sync Message can't get value back by NULL ptr, so if a
  // return back ptr is NULL, we must instead it using another
  // no-NULL ptr.
  if (param_value_size_ret == NULL)
    param_value_size_ret = &param_value_size_ret_inter;
  else if ((size_t) -1 == *param_value_size_ret)
    *param_value_size_ret = 0;

  if (NULL == param_value)
    is_param_null = CL_TRUE;

  switch(param_name) {
    case CL_SAMPLER_REFERENCE_COUNT:
    case CL_SAMPLER_NORMALIZED_COORDS:
    case CL_SAMPLER_ADDRESSING_MODE:
    case CL_SAMPLER_FILTER_MODE: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetSamplerInfo_cl_uint(
               point_sampler,
               param_name,
               param_value_size,
               is_param_null,
               &cl_uint_ret,
               param_value_size_ret,
               &errcode_ret))) {
      return CL_SEND_IPC_MESSAGE_FAILURE;
      }

      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        *(cl_uint*) param_value = cl_uint_ret;

      return errcode_ret;
    }
    case CL_SAMPLER_CONTEXT: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetSamplerInfo_cl_point(
               point_sampler,
               param_name,
               param_value_size,
               is_param_null,
               &cl_point_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }

      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        *(cl_context*) param_value = (cl_context) cl_point_ret;

      return errcode_ret;
    }
    default: return CL_SEND_IPC_MESSAGE_FAILURE;
  }
}
cl_int GpuChannelHost::CallclGetProgramInfo(
    cl_program program,
    cl_program_info param_name,
    size_t param_value_size,
    void *param_value, 
    size_t *param_value_size_ret) {
  // Sending a Sync IPC Message, to call a clCreateSubDevices API
  // in other process, and getting the results of the API.
  cl_int errcode_ret;
  cl_point point_program = (cl_point) program;
  cl_uint cl_uint_ret;
  cl_point cl_point_ret;
  std::vector<cl_point> cl_point_list_ret;
  std::string string_ret;
  std::vector<size_t> size_t_list_ret;
  std::vector<std::string> string_list_ret;
  size_t size_t_ret;
  size_t param_value_size_ret_inter = (size_t) -1;
  cl_bool is_param_null = CL_FALSE;

  // The Sync Message can't get value back by NULL ptr, so if a
  // return back ptr is NULL, we must instead it using another
  // no-NULL ptr.
  if (param_value_size_ret == NULL)
    param_value_size_ret = &param_value_size_ret_inter;
  else if ((size_t) -1 == *param_value_size_ret)
    *param_value_size_ret = 0;

  if (NULL == param_value)
    is_param_null = CL_TRUE;

  switch(param_name) {
    case CL_PROGRAM_REFERENCE_COUNT:
    case CL_PROGRAM_NUM_DEVICES: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetProgramInfo_cl_uint(
               point_program,
               param_name,
               param_value_size,
               is_param_null,
               &cl_uint_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }

      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        *(cl_uint*) param_value = cl_uint_ret;

      return errcode_ret;
    }
    case CL_PROGRAM_CONTEXT: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetProgramInfo_cl_point(
               point_program,
               param_name,
               param_value_size,
               is_param_null,
               &cl_point_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }

      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        *(cl_context*) param_value = (cl_context) cl_point_ret;

      return errcode_ret;
    }
    case CL_PROGRAM_DEVICES: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetProgramInfo_cl_point_list(
               point_program,
               param_name,
               param_value_size,
               is_param_null,
               &cl_point_list_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }

      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        for (cl_uint index = 0; index < param_value_size/sizeof(cl_point); ++index)
          ((cl_device_id*) (param_value))[index] = (cl_device_id)cl_point_list_ret[index];

      return errcode_ret;
    }
    case CL_PROGRAM_SOURCE:
    case CL_PROGRAM_KERNEL_NAMES: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetProgramInfo_string(
               point_program,
               param_name,
               param_value_size,
               is_param_null,
               &string_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }

      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        strcpy((char*)param_value,string_ret.c_str());

      return errcode_ret;
    }
    case CL_PROGRAM_BINARY_SIZES: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetProgramInfo_size_t_list(
               point_program,
               param_name,
               param_value_size,
               is_param_null,
               &size_t_list_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }

      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        for (cl_uint index = 0; index < param_value_size/sizeof(size_t); ++index)
          ((size_t*) (param_value))[index] = size_t_list_ret[index];

      return errcode_ret;
    }
    case CL_PROGRAM_BINARIES: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetProgramInfo_string_list(
               point_program,
               param_name,
               param_value_size,
               is_param_null,
               &string_list_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }

      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
      for (cl_uint index = 0; index < param_value_size/sizeof(std::string); ++index)
        strcpy(((char **)(param_value))[index],string_list_ret[index].c_str());

      return errcode_ret;
    }
    case CL_PROGRAM_NUM_KERNELS: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetProgramInfo_size_t(
               point_program,
               param_name,
               param_value_size,
               is_param_null,
               &size_t_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }

      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        *(size_t*) param_value = size_t_ret;

      return errcode_ret;
    }
    default:return CL_SEND_IPC_MESSAGE_FAILURE;
  }
}
cl_int GpuChannelHost::CallclGetProgramBuildInfo(
    cl_program program,
    cl_device_id device,
    cl_program_build_info param_name,
    size_t param_value_size,
    void *param_value,
    size_t *param_value_size_ret) {
  // Sending a Sync IPC Message, to call a clCreateSubDevices API
  // in other process, and getting the results of the API.
  cl_int errcode_ret;
  cl_point point_program = (cl_point) program;
  cl_point point_device  = (cl_point) device;
  cl_int cl_int_ret;
  std::string string_ret;
  cl_uint cl_uint_ret;
  size_t param_value_size_ret_inter = (size_t) -1;
  cl_bool is_param_null = CL_FALSE;

  // The Sync Message can't get value back by NULL ptr, so if a
  // return back ptr is NULL, we must instead it using another
  // no-NULL ptr.
  if (param_value_size_ret == NULL)
    param_value_size_ret = &param_value_size_ret_inter;
  else if ((size_t) -1 == *param_value_size_ret)
    *param_value_size_ret = 0;

  if (NULL == param_value)
    is_param_null = CL_TRUE;

  switch(param_name) {
    case CL_PROGRAM_BUILD_STATUS: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetProgramBuildInfo_cl_int(
               point_program,
               point_device,
               param_name,
               param_value_size,
               is_param_null,
               &cl_int_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }

      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        *(cl_int*) param_value = cl_int_ret;

      return errcode_ret;
    }
    case CL_PROGRAM_BUILD_OPTIONS:
    case CL_PROGRAM_BUILD_LOG: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetProgramBuildInfo_string(
               point_program,
               point_device,
               param_name,
               param_value_size,
               is_param_null,
               &string_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }

      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        strcpy((char*) param_value,string_ret.c_str());
      
      return errcode_ret;
    }
    case CL_PROGRAM_BINARY_TYPE: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetProgramBuildInfo_cl_uint(
               point_program,
               point_device,
               param_name,
               param_value_size,
               is_param_null,
               &cl_uint_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }
     
      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        *(cl_program_binary_type*) param_value = (cl_program_binary_type) cl_uint_ret;
      
      return errcode_ret;
    }
    default: return CL_SEND_IPC_MESSAGE_FAILURE;
  }
}
cl_int GpuChannelHost::CallclGetKernelInfo(
    cl_kernel kernel,
    cl_kernel_info param_name,
    size_t param_value_size,
    void *param_value,
    size_t *param_value_size_ret) {
  // Sending a Sync IPC Message, to call a clCreateSubDevices API
  // in other process, and getting the results of the API.
  cl_int errcode_ret;
  cl_point point_kernel = (cl_point) kernel;
  std::string string_ret;
  cl_uint cl_uint_ret;
  cl_point cl_point_ret;
  size_t param_value_size_ret_inter = (size_t) -1;
  cl_bool is_param_null = CL_FALSE;

  // The Sync Message can't get value back by NULL ptr, so if a
  // return back ptr is NULL, we must instead it using another
  // no-NULL ptr.    if ((size_t)-1 == *param_value_size_ret)
  if (param_value_size_ret == NULL)
    param_value_size_ret = &param_value_size_ret_inter;
  else if ((size_t) -1 == *param_value_size_ret)
    *param_value_size_ret = 0;

  if (NULL == param_value)
    is_param_null = CL_TRUE;

  switch(param_name) {
    case CL_KERNEL_FUNCTION_NAME:
    case CL_KERNEL_ATTRIBUTES: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetKernelInfo_string(
               point_kernel,
               param_name,
               param_value_size,
               is_param_null,
               &string_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }

      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        strcpy((char*)param_value,string_ret.c_str());
      
      return errcode_ret;
    }
    case CL_KERNEL_NUM_ARGS:
    case CL_KERNEL_REFERENCE_COUNT: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetKernelInfo_cl_uint(
               point_kernel,
               param_name,
               param_value_size,
               is_param_null,
               &cl_uint_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }

      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        *(cl_uint*) param_value = cl_uint_ret;

      return errcode_ret;
    }
    case CL_KERNEL_CONTEXT: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetKernelInfo_cl_point(
               point_kernel,
               param_name,
               param_value_size,
               is_param_null,
               &cl_point_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }
     
      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        *(cl_context*) param_value = (cl_context) cl_point_ret;

      return errcode_ret;
    }
    case CL_KERNEL_PROGRAM: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetKernelInfo_cl_point(
               point_kernel,
               param_name,
               param_value_size,
               is_param_null,
               &cl_point_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }

      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        *(cl_program*) param_value = (cl_program) cl_point_ret;

      return errcode_ret;
    }
    default: return CL_SEND_IPC_MESSAGE_FAILURE;
  }
}
cl_int GpuChannelHost::CallclGetKernelArgInfo(
    cl_kernel kernel,
    cl_uint arg_indx,
    cl_kernel_arg_info param_name,
    size_t param_value_size,
    void *param_value,
    size_t *param_value_size_ret) {
  // Sending a Sync IPC Message, to call a clCreateSubDevices API
  // in other process, and getting the results of the API.
  cl_int errcode_ret;
  cl_point point_kernel = (cl_point) kernel;
  cl_uint cl_uint_arg_indx = arg_indx;
  cl_uint cl_uint_ret;
  std::string string_ret;
  cl_ulong cl_ulong_ret;
  size_t param_value_size_ret_inter = (size_t) -1;
  cl_bool is_param_null = CL_FALSE;

  // The Sync Message can't get value back by NULL ptr, so if a
  // return back ptr is NULL, we must instead it using another
  // no-NULL ptr.
  if (param_value_size_ret == NULL)
    param_value_size_ret = &param_value_size_ret_inter;
  else if ((size_t) -1 == *param_value_size_ret)
    *param_value_size_ret = 0;

  if (NULL == param_value)
    is_param_null = CL_TRUE;

  switch(param_name) {
    case CL_KERNEL_ARG_ADDRESS_QUALIFIER:
    case CL_KERNEL_ARG_ACCESS_QUALIFIER: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetKernelArgInfo_cl_uint(
               point_kernel,
               cl_uint_arg_indx,
               param_name,
               param_value_size,
               is_param_null,
               &cl_uint_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }
  
      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        *(cl_uint*) param_value = cl_uint_ret;
      return errcode_ret;
    }
    case CL_KERNEL_ARG_TYPE_NAME:
    case CL_KERNEL_ARG_NAME: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetKernelArgInfo_string(
               point_kernel,
               cl_uint_arg_indx,
               param_name,
               param_value_size,
               is_param_null,
               &string_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }
      
      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        strcpy((char*)param_value,string_ret.c_str());
      
      return errcode_ret;
    }
    case CL_KERNEL_ARG_TYPE_QUALIFIER: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetKernelArgInfo_cl_ulong(
               point_kernel,
               cl_uint_arg_indx,
               param_name,
               param_value_size,
               is_param_null,
               &cl_ulong_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }

      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        *(cl_ulong*) param_value = cl_ulong_ret;

      return errcode_ret;
    }
    default: return CL_SEND_IPC_MESSAGE_FAILURE;
  }
}
cl_int GpuChannelHost::CallclGetKernelWorkGroupInfo(
    cl_kernel kernel,
    cl_device_id device,
    cl_kernel_work_group_info param_name,
    size_t param_value_size, 
    void *param_value,
    size_t *param_value_size_ret) {
  // Sending a Sync IPC Message, to call a clCreateSubDevices API
  // in other process, and getting the results of the API.
  cl_int errcode_ret;
  cl_point point_kernel = (cl_point) kernel;
  cl_point point_device = (cl_point) device;
  std::vector<size_t> size_t_list_ret;
  size_t size_t_ret;
  cl_ulong cl_ulong_ret;
  size_t param_value_size_ret_inter = (size_t) -1;
  cl_bool is_param_null = CL_FALSE;

  // The Sync Message can't get value back by NULL ptr, so if a
  // return back ptr is NULL, we must instead it using another
  // no-NULL ptr.
  if (param_value_size_ret == NULL)
    param_value_size_ret = &param_value_size_ret_inter;
  else if ((size_t) -1 == *param_value_size_ret)
    *param_value_size_ret = 0;

  if (NULL == param_value)
    is_param_null = CL_TRUE;

  switch(param_name) {
    case CL_KERNEL_GLOBAL_WORK_SIZE:
    case CL_KERNEL_COMPILE_WORK_GROUP_SIZE: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetKernelWorkGroupInfo_size_t_list(
               point_kernel,
               point_device,
               param_name,
               param_value_size,
               is_param_null,
               &size_t_list_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }
    
      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        for (cl_uint index = 0; index < param_value_size/sizeof(size_t); ++index)
          ((size_t*) (param_value))[index] = size_t_list_ret[index];

      return errcode_ret;
    }
    case CL_KERNEL_WORK_GROUP_SIZE:
    case CL_KERNEL_PREFERRED_WORK_GROUP_SIZE_MULTIPLE: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetKernelWorkGroupInfo_size_t(
               point_kernel,
               point_device,
               param_name,
               param_value_size,
               is_param_null,
               &size_t_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }
    
      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
    *(size_t*) param_value = size_t_ret;

      return errcode_ret;
    }
    case CL_KERNEL_LOCAL_MEM_SIZE: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetKernelWorkGroupInfo_cl_ulong(
               point_kernel,
               point_device,
               param_name,
               param_value_size,
               is_param_null,
               &cl_ulong_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }
 
      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
       *(cl_ulong*) param_value = cl_ulong_ret;

      return errcode_ret;
    }
    default: return CL_SEND_IPC_MESSAGE_FAILURE;
  }
}
cl_int GpuChannelHost::CallclGetEventInfo(
    cl_event clevent,
    cl_event_info param_name,
    size_t param_value_size,
    void *param_value,
    size_t *param_value_size_ret) {
  // Sending a Sync IPC Message, to call a clCreateSubDevices API
  // in other process, and getting the results of the API.
  cl_int errcode_ret;
  cl_point point_event = (cl_point) clevent;  
  cl_point cl_point_ret;
  cl_uint cl_uint_ret;
  cl_int cl_int_ret;
  size_t param_value_size_ret_inter = (size_t) -1;
  cl_bool is_param_null = CL_FALSE;

  // The Sync Message can't get value back by NULL ptr, so if a
  // return back ptr is NULL, we must instead it using another
  // no-NULL ptr.
  if (param_value_size_ret == NULL)
    param_value_size_ret = &param_value_size_ret_inter;
  else if ((size_t) -1 == *param_value_size_ret)
    *param_value_size_ret = 0;
  
  if (NULL == param_value)
    is_param_null = CL_TRUE;

  switch(param_name) {
    case CL_EVENT_COMMAND_QUEUE: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetEventInfo_cl_point(
               point_event,
               param_name,
               param_value_size,
               is_param_null,
               &cl_point_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }
  
      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        *(cl_command_queue*) param_value = (cl_command_queue) cl_point_ret;

      return errcode_ret;
    }
    case CL_EVENT_CONTEXT: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetEventInfo_cl_point(
               point_event,
               param_name,
               param_value_size,
               is_param_null,
               &cl_point_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }

      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
      *(cl_context*) param_value = (cl_context) cl_point_ret;

      return errcode_ret;
    }
    case CL_EVENT_COMMAND_TYPE: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetEventInfo_cl_uint(
               point_event,
               param_name,
               param_value_size,
               is_param_null,
               &cl_uint_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }

      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        *(cl_command_type*) param_value = cl_uint_ret;

      return errcode_ret;
    }
    case CL_EVENT_COMMAND_EXECUTION_STATUS: {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetEventInfo_cl_int(
               point_event,
               param_name,
               param_value_size,
               is_param_null,
               &cl_int_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }

      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        *(cl_int*) param_value = cl_int_ret;

      return errcode_ret;
    }
    default: return CL_SEND_IPC_MESSAGE_FAILURE;
  }
}
cl_int GpuChannelHost::CallclGetEventProfilingInfo(
    cl_event clevent,
    cl_profiling_info param_name,
    size_t param_value_size,
    void *param_value,
    size_t *param_value_size_ret) {
  // Sending a Sync IPC Message, to call a clCreateSubDevices API
  // in other process, and getting the results of the API.
  cl_int errcode_ret;
  cl_point point_event = (cl_point) clevent;
  cl_ulong cl_ulong_ret;
  size_t param_value_size_ret_inter = (size_t) -1;
  cl_bool is_param_null = CL_FALSE;

  // The Sync Message can't get value back by NULL ptr, so if a
  // return back ptr is NULL, we must instead it using another
  // no-NULL ptr.
  if (param_value_size_ret == NULL)
    param_value_size_ret = &param_value_size_ret_inter;
  else if ((size_t) -1 == *param_value_size_ret)
    *param_value_size_ret = 0;
  
  if (NULL == param_value)
    is_param_null = CL_TRUE;

  switch(param_name) {
    case CL_PROFILING_COMMAND_QUEUED:
    case CL_PROFILING_COMMAND_SUBMIT:
    case CL_PROFILING_COMMAND_START:
    case CL_PROFILING_COMMAND_END:
    {
      // Send a Sync IPC Message and wait for the results.
      if (!Send(new OpenCLChannelMsg_GetEventProfilingInfo_cl_ulong(
               point_event,
               param_name,
               param_value_size,
               is_param_null,
               &cl_ulong_ret,
               param_value_size_ret,
               &errcode_ret))) {
        return CL_SEND_IPC_MESSAGE_FAILURE;
      }

      // Dump the results of the Sync IPC Message calling.
      if (CL_SUCCESS == errcode_ret)
        *(cl_ulong*) param_value = cl_ulong_ret;

      return errcode_ret;
    }
    default: return CL_SEND_IPC_MESSAGE_FAILURE;
  }
}

}  // namespace content
