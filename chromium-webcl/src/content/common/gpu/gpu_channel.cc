// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if defined(OS_WIN)
#include <windows.h>
#endif

#include "content/common/gpu/gpu_channel.h"

#include <queue>
#include <vector>

#include "base/bind.h"
#include "base/command_line.h"
#include "base/debug/trace_event.h"
#include "base/message_loop_proxy.h"
#include "base/process_util.h"
#include "base/rand_util.h"
#include "base/string_util.h"
#include "base/timer.h"
#include "content/common/child_process.h"
#include "content/common/gpu/gpu_channel_manager.h"
#include "content/common/gpu/gpu_messages.h"
#include "content/common/gpu/sync_point_manager.h"
#include "content/public/common/content_switches.h"
#include "crypto/hmac.h"
#include "gpu/command_buffer/common/mailbox.h"
#include "gpu/command_buffer/service/gpu_scheduler.h"
#include "gpu/command_buffer/service/image_manager.h"
#include "gpu/command_buffer/service/mailbox_manager.h"
#include "ipc/ipc_channel.h"
#include "ipc/ipc_channel_proxy.h"
#include "ui/gl/gl_context.h"
#include "ui/gl/gl_image.h"
#include "ui/gl/gl_surface.h"

#if defined(OS_POSIX)
#include "ipc/ipc_channel_posix.h"
#endif

#if defined(OS_ANDROID)
#include "content/common/gpu/stream_texture_manager_android.h"
#endif

namespace content {
namespace {

// Number of milliseconds between successive vsync. Many GL commands block
// on vsync, so thresholds for preemption should be multiples of this.
const int64 kVsyncIntervalMs = 17;

// Amount of time that we will wait for an IPC to be processed before
// preempting. After a preemption, we must wait this long before triggering
// another preemption.
const int64 kPreemptWaitTimeMs = 2 * kVsyncIntervalMs;

// Once we trigger a preemption, the maximum duration that we will wait
// before clearing the preemption.
const int64 kMaxPreemptTimeMs = kVsyncIntervalMs;

// Stop the preemption once the time for the longest pending IPC drops
// below this threshold.
const int64 kStopPreemptThresholdMs = kVsyncIntervalMs;

}  // anonymous namespace

// This filter does three things:
// - it counts and timestamps each message forwarded to the channel
//   so that we can preempt other channels if a message takes too long to
//   process. To guarantee fairness, we must wait a minimum amount of time
//   before preempting and we limit the amount of time that we can preempt in
//   one shot (see constants above).
// - it handles the GpuCommandBufferMsg_InsertSyncPoint message on the IO
//   thread, generating the sync point ID and responding immediately, and then
//   posting a task to insert the GpuCommandBufferMsg_RetireSyncPoint message
//   into the channel's queue.
// - it generates mailbox names for clients of the GPU process on the IO thread.
class GpuChannelMessageFilter : public IPC::ChannelProxy::MessageFilter {
 public:
  // Takes ownership of gpu_channel (see below).
  GpuChannelMessageFilter(const std::string& private_key,
                          base::WeakPtr<GpuChannel>* gpu_channel,
                          scoped_refptr<SyncPointManager> sync_point_manager,
                          scoped_refptr<base::MessageLoopProxy> message_loop)
      : preemption_state_(IDLE),
        gpu_channel_(gpu_channel),
        channel_(NULL),
        sync_point_manager_(sync_point_manager),
        message_loop_(message_loop),
        messages_forwarded_to_channel_(0),
        a_stub_is_descheduled_(false),
        hmac_(crypto::HMAC::SHA256) {
    bool success = hmac_.Init(base::StringPiece(private_key));
    DCHECK(success);
  }

  virtual void OnFilterAdded(IPC::Channel* channel) OVERRIDE {
    DCHECK(!channel_);
    channel_ = channel;
  }

  virtual void OnFilterRemoved() OVERRIDE {
    DCHECK(channel_);
    channel_ = NULL;
  }

  virtual bool OnMessageReceived(const IPC::Message& message) OVERRIDE {
    DCHECK(channel_);

    bool handled = true;
    IPC_BEGIN_MESSAGE_MAP(GpuChannelMessageFilter, message)
      IPC_MESSAGE_HANDLER(GpuChannelMsg_GenerateMailboxNames,
                          OnGenerateMailboxNames)
      IPC_MESSAGE_HANDLER(GpuChannelMsg_GenerateMailboxNamesAsync,
                          OnGenerateMailboxNamesAsync)
      IPC_MESSAGE_UNHANDLED(handled = false)
    IPC_END_MESSAGE_MAP()

    if (message.type() == GpuCommandBufferMsg_RetireSyncPoint::ID) {
      // This message should not be sent explicitly by the renderer.
      NOTREACHED();
      handled = true;
    }

    // All other messages get processed by the GpuChannel.
    if (!handled) {
      messages_forwarded_to_channel_++;
      if (preempting_flag_)
        pending_messages_.push(PendingMessage(messages_forwarded_to_channel_));
      UpdatePreemptionState();
    }

    if (message.type() == GpuCommandBufferMsg_InsertSyncPoint::ID) {
      uint32 sync_point = sync_point_manager_->GenerateSyncPoint();
      IPC::Message* reply = IPC::SyncMessage::GenerateReply(&message);
      GpuCommandBufferMsg_InsertSyncPoint::WriteReplyParams(reply, sync_point);
      Send(reply);
      message_loop_->PostTask(FROM_HERE, base::Bind(
          &GpuChannelMessageFilter::InsertSyncPointOnMainThread,
          gpu_channel_,
          sync_point_manager_,
          message.routing_id(),
          sync_point));
      handled = true;
    }
    return handled;
  }

  void MessageProcessed(uint64 messages_processed) {
    while (!pending_messages_.empty() &&
           pending_messages_.front().message_number <= messages_processed)
      pending_messages_.pop();
    UpdatePreemptionState();
  }

  void SetPreemptingFlagAndSchedulingState(
      gpu::PreemptionFlag* preempting_flag,
      bool a_stub_is_descheduled) {
    preempting_flag_ = preempting_flag;
    a_stub_is_descheduled_ = a_stub_is_descheduled;
  }

  void UpdateStubSchedulingState(bool a_stub_is_descheduled) {
    a_stub_is_descheduled_ = a_stub_is_descheduled;
    UpdatePreemptionState();
  }

  bool Send(IPC::Message* message) {
    return channel_->Send(message);
  }

 protected:
  virtual ~GpuChannelMessageFilter() {
    message_loop_->PostTask(FROM_HERE, base::Bind(
        &GpuChannelMessageFilter::DeleteWeakPtrOnMainThread, gpu_channel_));
  }

 private:
  // Message handlers.
  void OnGenerateMailboxNames(unsigned num, std::vector<gpu::Mailbox>* result) {
    TRACE_EVENT1("gpu", "OnGenerateMailboxNames", "num", num);

    result->resize(num);

    for (unsigned i = 0; i < num; ++i) {
      char name[GL_MAILBOX_SIZE_CHROMIUM];
      base::RandBytes(name, sizeof(name) / 2);

      bool success = hmac_.Sign(
          base::StringPiece(name, sizeof(name) / 2),
          reinterpret_cast<unsigned char*>(name) + sizeof(name) / 2,
          sizeof(name) / 2);
      DCHECK(success);

      (*result)[i].SetName(reinterpret_cast<int8*>(name));
    }
  }

  void OnGenerateMailboxNamesAsync(unsigned num) {
    std::vector<gpu::Mailbox> names;
    OnGenerateMailboxNames(num, &names);
    Send(new GpuChannelMsg_GenerateMailboxNamesReply(names));
  }

  enum PreemptionState {
    // Either there's no other channel to preempt, there are no messages
    // pending processing, or we just finished preempting and have to wait
    // before preempting again.
    IDLE,
    // We are waiting kPreemptWaitTimeMs before checking if we should preempt.
    WAITING,
    // We can preempt whenever any IPC processing takes more than
    // kPreemptWaitTimeMs.
    CHECKING,
    // We are currently preempting (i.e. no stub is descheduled).
    PREEMPTING,
    // We would like to preempt, but some stub is descheduled.
    WOULD_PREEMPT_DESCHEDULED,
  };

  PreemptionState preemption_state_;

  // Maximum amount of time that we can spend in PREEMPTING.
  // It is reset when we transition to IDLE.
  base::TimeDelta max_preemption_time_;

  struct PendingMessage {
    uint64 message_number;
    base::TimeTicks time_received;

    explicit PendingMessage(uint64 message_number)
        : message_number(message_number),
          time_received(base::TimeTicks::Now()) {
    }
  };

  void UpdatePreemptionState() {
    switch (preemption_state_) {
      case IDLE:
        if (preempting_flag_.get() && !pending_messages_.empty())
          TransitionToWaiting();
        break;
      case WAITING:
        // A timer will transition us to CHECKING.
        DCHECK(timer_.IsRunning());
        break;
      case CHECKING:
        if (!pending_messages_.empty()) {
          base::TimeDelta time_elapsed =
              base::TimeTicks::Now() - pending_messages_.front().time_received;
          if (time_elapsed.InMilliseconds() < kPreemptWaitTimeMs) {
            // Schedule another check for when the IPC may go long.
            timer_.Start(
                FROM_HERE,
                base::TimeDelta::FromMilliseconds(kPreemptWaitTimeMs) -
                    time_elapsed,
                this, &GpuChannelMessageFilter::UpdatePreemptionState);
          } else {
            if (a_stub_is_descheduled_)
              TransitionToWouldPreemptDescheduled();
            else
              TransitionToPreempting();
          }
        }
        break;
      case PREEMPTING:
        // A TransitionToIdle() timer should always be running in this state.
        DCHECK(timer_.IsRunning());
        if (a_stub_is_descheduled_)
          TransitionToWouldPreemptDescheduled();
        else
          TransitionToIdleIfCaughtUp();
        break;
      case WOULD_PREEMPT_DESCHEDULED:
        // A TransitionToIdle() timer should never be running in this state.
        DCHECK(!timer_.IsRunning());
        if (!a_stub_is_descheduled_)
          TransitionToPreempting();
        else
          TransitionToIdleIfCaughtUp();
        break;
      default:
        NOTREACHED();
    }
  }

  void TransitionToIdleIfCaughtUp() {
    DCHECK(preemption_state_ == PREEMPTING ||
           preemption_state_ == WOULD_PREEMPT_DESCHEDULED);
    if (pending_messages_.empty()) {
      TransitionToIdle();
    } else {
      base::TimeDelta time_elapsed =
          base::TimeTicks::Now() - pending_messages_.front().time_received;
      if (time_elapsed.InMilliseconds() < kStopPreemptThresholdMs)
        TransitionToIdle();
    }
  }

  void TransitionToIdle() {
    DCHECK(preemption_state_ == PREEMPTING ||
           preemption_state_ == WOULD_PREEMPT_DESCHEDULED);
    // Stop any outstanding timer set to force us from PREEMPTING to IDLE.
    timer_.Stop();

    preemption_state_ = IDLE;
    preempting_flag_->Reset();
    TRACE_COUNTER_ID1("gpu", "GpuChannel::Preempting", this, 0);

    UpdatePreemptionState();
  }

  void TransitionToWaiting() {
    DCHECK_EQ(preemption_state_, IDLE);
    DCHECK(!timer_.IsRunning());

    preemption_state_ = WAITING;
    timer_.Start(
        FROM_HERE,
        base::TimeDelta::FromMilliseconds(kPreemptWaitTimeMs),
        this, &GpuChannelMessageFilter::TransitionToChecking);
  }

  void TransitionToChecking() {
    DCHECK_EQ(preemption_state_, WAITING);
    DCHECK(!timer_.IsRunning());

    preemption_state_ = CHECKING;
    max_preemption_time_ = base::TimeDelta::FromMilliseconds(kMaxPreemptTimeMs);
    UpdatePreemptionState();
  }

  void TransitionToPreempting() {
    DCHECK(preemption_state_ == CHECKING ||
           preemption_state_ == WOULD_PREEMPT_DESCHEDULED);
    DCHECK(!a_stub_is_descheduled_);

    // Stop any pending state update checks that we may have queued
    // while CHECKING.
    if (preemption_state_ == CHECKING)
      timer_.Stop();

    preemption_state_ = PREEMPTING;
    preempting_flag_->Set();
    TRACE_COUNTER_ID1("gpu", "GpuChannel::Preempting", this, 1);

    timer_.Start(
       FROM_HERE,
       max_preemption_time_,
       this, &GpuChannelMessageFilter::TransitionToIdle);

    UpdatePreemptionState();
  }

  void TransitionToWouldPreemptDescheduled() {
    DCHECK(preemption_state_ == CHECKING ||
           preemption_state_ == PREEMPTING);
    DCHECK(a_stub_is_descheduled_);

    if (preemption_state_ == CHECKING) {
      // Stop any pending state update checks that we may have queued
      // while CHECKING.
      timer_.Stop();
    } else {
      // Stop any TransitionToIdle() timers that we may have queued
      // while PREEMPTING.
      timer_.Stop();
      max_preemption_time_ = timer_.desired_run_time() - base::TimeTicks::Now();
      if (max_preemption_time_ < base::TimeDelta()) {
        TransitionToIdle();
        return;
      }
    }

    preemption_state_ = WOULD_PREEMPT_DESCHEDULED;
    preempting_flag_->Reset();
    TRACE_COUNTER_ID1("gpu", "GpuChannel::Preempting", this, 0);

    UpdatePreemptionState();
  }

  static void InsertSyncPointOnMainThread(
      base::WeakPtr<GpuChannel>* gpu_channel,
      scoped_refptr<SyncPointManager> manager,
      int32 routing_id,
      uint32 sync_point) {
    // This function must ensure that the sync point will be retired. Normally
    // we'll find the stub based on the routing ID, and associate the sync point
    // with it, but if that fails for any reason (channel or stub already
    // deleted, invalid routing id), we need to retire the sync point
    // immediately.
    if (gpu_channel->get()) {
      GpuCommandBufferStub* stub = gpu_channel->get()->LookupCommandBuffer(
          routing_id);
      if (stub) {
        stub->AddSyncPoint(sync_point);
        GpuCommandBufferMsg_RetireSyncPoint message(routing_id, sync_point);
        gpu_channel->get()->OnMessageReceived(message);
        return;
      } else {
        gpu_channel->get()->MessageProcessed();
      }
    }
    manager->RetireSyncPoint(sync_point);
  }

  static void DeleteWeakPtrOnMainThread(
      base::WeakPtr<GpuChannel>* gpu_channel) {
    delete gpu_channel;
  }

  // NOTE: this is a pointer to a weak pointer. It is never dereferenced on the
  // IO thread, it's only passed through - therefore the WeakPtr assumptions are
  // respected.
  base::WeakPtr<GpuChannel>* gpu_channel_;
  IPC::Channel* channel_;
  scoped_refptr<SyncPointManager> sync_point_manager_;
  scoped_refptr<base::MessageLoopProxy> message_loop_;
  scoped_refptr<gpu::PreemptionFlag> preempting_flag_;

  std::queue<PendingMessage> pending_messages_;

  // Count of the number of IPCs forwarded to the GpuChannel.
  uint64 messages_forwarded_to_channel_;

  base::OneShotTimer<GpuChannelMessageFilter> timer_;

  bool a_stub_is_descheduled_;

  crypto::HMAC hmac_;
};

GpuChannel::GpuChannel(GpuChannelManager* gpu_channel_manager,
                       GpuWatchdog* watchdog,
                       gfx::GLShareGroup* share_group,
                       gpu::gles2::MailboxManager* mailbox,
                       int client_id,
                       bool software)
    : gpu_channel_manager_(gpu_channel_manager),
      messages_processed_(0),
      client_id_(client_id),
      share_group_(share_group ? share_group : new gfx::GLShareGroup),
      mailbox_manager_(mailbox ? mailbox : new gpu::gles2::MailboxManager),
      image_manager_(new gpu::gles2::ImageManager),
      watchdog_(watchdog),
      software_(software),
      handle_messages_scheduled_(false),
      processed_get_state_fast_(false),
      currently_processing_message_(NULL),
      weak_factory_(this),
      num_stubs_descheduled_(0) {
  DCHECK(gpu_channel_manager);
  DCHECK(client_id);

  channel_id_ = IPC::Channel::GenerateVerifiedChannelID("gpu");
  const CommandLine* command_line = CommandLine::ForCurrentProcess();
  log_messages_ = command_line->HasSwitch(switches::kLogPluginMessages);
  disallowed_features_.multisampling =
      command_line->HasSwitch(switches::kDisableGLMultisampling);
#if defined(OS_ANDROID)
  stream_texture_manager_.reset(new StreamTextureManagerAndroid(this));
#endif
}


bool GpuChannel::Init(base::MessageLoopProxy* io_message_loop,
                      base::WaitableEvent* shutdown_event) {
  DCHECK(!channel_.get());

  // Map renderer ID to a (single) channel to that process.
  channel_.reset(new IPC::SyncChannel(
      channel_id_,
      IPC::Channel::MODE_SERVER,
      this,
      io_message_loop,
      false,
      shutdown_event));

  base::WeakPtr<GpuChannel>* weak_ptr(new base::WeakPtr<GpuChannel>(
      weak_factory_.GetWeakPtr()));

  filter_ = new GpuChannelMessageFilter(
      mailbox_manager_->private_key(),
      weak_ptr,
      gpu_channel_manager_->sync_point_manager(),
      base::MessageLoopProxy::current());
  io_message_loop_ = io_message_loop;
  channel_->AddFilter(filter_);

  return true;
}

std::string GpuChannel::GetChannelName() {
  return channel_id_;
}

#if defined(OS_POSIX)
int GpuChannel::TakeRendererFileDescriptor() {
  if (!channel_) {
    NOTREACHED();
    return -1;
  }
  return channel_->TakeClientFileDescriptor();
}
#endif  // defined(OS_POSIX)

bool GpuChannel::OnMessageReceived(const IPC::Message& message) {
  bool message_processed = true;
  if (log_messages_) {
    DVLOG(1) << "received message @" << &message << " on channel @" << this
             << " with type " << message.type();
  }

  if (message.type() == GpuCommandBufferMsg_GetStateFast::ID) {
    if (processed_get_state_fast_) {
      // Require a non-GetStateFast message in between two GetStateFast
      // messages, to ensure progress is made.
      std::deque<IPC::Message*>::iterator point = deferred_messages_.begin();

      while (point != deferred_messages_.end() &&
             (*point)->type() == GpuCommandBufferMsg_GetStateFast::ID) {
        ++point;
      }

      if (point != deferred_messages_.end()) {
        ++point;
      }

      deferred_messages_.insert(point, new IPC::Message(message));
      message_processed = false;
    } else {
      // Move GetStateFast commands to the head of the queue, so the renderer
      // doesn't have to wait any longer than necessary.
      deferred_messages_.push_front(new IPC::Message(message));
      message_processed = false;
    }
  } else {
    deferred_messages_.push_back(new IPC::Message(message));
    message_processed = false;
  }

  if (message_processed)
    MessageProcessed();

  OnScheduled();

  return true;
}

void GpuChannel::OnChannelError() {
  gpu_channel_manager_->RemoveChannel(client_id_);
}

bool GpuChannel::Send(IPC::Message* message) {
  // The GPU process must never send a synchronous IPC message to the renderer
  // process. This could result in deadlock.
  DCHECK(!message->is_sync());
  if (log_messages_) {
    DVLOG(1) << "sending message @" << message << " on channel @" << this
             << " with type " << message->type();
  }

  if (!channel_) {
    delete message;
    return false;
  }

  return channel_->Send(message);
}

void GpuChannel::RequeueMessage() {
  DCHECK(currently_processing_message_);
  deferred_messages_.push_front(
      new IPC::Message(*currently_processing_message_));
  messages_processed_--;
  currently_processing_message_ = NULL;
}

void GpuChannel::OnScheduled() {
  if (handle_messages_scheduled_)
    return;
  // Post a task to handle any deferred messages. The deferred message queue is
  // not emptied here, which ensures that OnMessageReceived will continue to
  // defer newly received messages until the ones in the queue have all been
  // handled by HandleMessage. HandleMessage is invoked as a
  // task to prevent reentrancy.
  MessageLoop::current()->PostTask(
      FROM_HERE,
      base::Bind(&GpuChannel::HandleMessage, weak_factory_.GetWeakPtr()));
  handle_messages_scheduled_ = true;
}

void GpuChannel::StubSchedulingChanged(bool scheduled) {
  bool a_stub_was_descheduled = num_stubs_descheduled_ > 0;
  if (scheduled) {
    num_stubs_descheduled_--;
    OnScheduled();
  } else {
    num_stubs_descheduled_++;
  }
  DCHECK_LE(num_stubs_descheduled_, stubs_.size());
  bool a_stub_is_descheduled = num_stubs_descheduled_ > 0;

  if (a_stub_is_descheduled != a_stub_was_descheduled) {
    if (preempting_flag_) {
      io_message_loop_->PostTask(
          FROM_HERE,
          base::Bind(&GpuChannelMessageFilter::UpdateStubSchedulingState,
                     filter_, a_stub_is_descheduled));
    }
  }
}

void GpuChannel::CreateViewCommandBuffer(
    const gfx::GLSurfaceHandle& window,
    int32 surface_id,
    const GPUCreateCommandBufferConfig& init_params,
    int32* route_id) {
  TRACE_EVENT1("gpu",
               "GpuChannel::CreateViewCommandBuffer",
               "surface_id",
               surface_id);

  *route_id = MSG_ROUTING_NONE;

#if defined(ENABLE_GPU)

  GpuCommandBufferStub* share_group = stubs_.Lookup(init_params.share_group_id);

  // Virtualize compositor contexts on OS X to prevent performance regressions
  // when enabling FCM.
  // http://crbug.com/180463
  bool use_virtualized_gl_context = false;
#if defined(OS_MACOSX)
  use_virtualized_gl_context = true;
#endif

  *route_id = GenerateRouteID();
  scoped_ptr<GpuCommandBufferStub> stub(new GpuCommandBufferStub(
      this,
      share_group,
      window,
      mailbox_manager_,
      image_manager_,
      gfx::Size(),
      disallowed_features_,
      init_params.allowed_extensions,
      init_params.attribs,
      init_params.gpu_preference,
      use_virtualized_gl_context,
      *route_id,
      surface_id,
      watchdog_,
      software_,
      init_params.active_url));
  if (preempted_flag_)
    stub->SetPreemptByFlag(preempted_flag_);
  router_.AddRoute(*route_id, stub.get());
  stubs_.AddWithID(stub.release(), *route_id);
#endif  // ENABLE_GPU
}

GpuCommandBufferStub* GpuChannel::LookupCommandBuffer(int32 route_id) {
  return stubs_.Lookup(route_id);
}

void GpuChannel::CreateImage(
    gfx::PluginWindowHandle window,
    int32 image_id,
    gfx::Size* size) {
  TRACE_EVENT1("gpu",
               "GpuChannel::CreateImage",
               "image_id",
               image_id);

  *size = gfx::Size();

  if (image_manager_->LookupImage(image_id)) {
    LOG(ERROR) << "CreateImage failed, image_id already in use.";
    return;
  }

  scoped_refptr<gfx::GLImage> image = gfx::GLImage::CreateGLImage(window);
  if (!image)
    return;

  image_manager_->AddImage(image.get(), image_id);
  *size = image->GetSize();
}

void GpuChannel::DeleteImage(int32 image_id) {
  TRACE_EVENT1("gpu",
               "GpuChannel::DeleteImage",
               "image_id",
               image_id);

  image_manager_->RemoveImage(image_id);
}

void GpuChannel::LoseAllContexts() {
  gpu_channel_manager_->LoseAllContexts();
}

void GpuChannel::DestroySoon() {
  MessageLoop::current()->PostTask(
      FROM_HERE, base::Bind(&GpuChannel::OnDestroy, this));
}

int GpuChannel::GenerateRouteID() {
  static int last_id = 0;
  return ++last_id;
}

void GpuChannel::AddRoute(int32 route_id, IPC::Listener* listener) {
  router_.AddRoute(route_id, listener);
}

void GpuChannel::RemoveRoute(int32 route_id) {
  router_.RemoveRoute(route_id);
}

gpu::PreemptionFlag* GpuChannel::GetPreemptionFlag() {
  if (!preempting_flag_) {
    preempting_flag_ = new gpu::PreemptionFlag;
    io_message_loop_->PostTask(
        FROM_HERE, base::Bind(
            &GpuChannelMessageFilter::SetPreemptingFlagAndSchedulingState,
            filter_, preempting_flag_, num_stubs_descheduled_ > 0));
  }
  return preempting_flag_.get();
}

void GpuChannel::SetPreemptByFlag(
    scoped_refptr<gpu::PreemptionFlag> preempted_flag) {
  preempted_flag_ = preempted_flag;

  for (StubMap::Iterator<GpuCommandBufferStub> it(&stubs_);
       !it.IsAtEnd(); it.Advance()) {
    it.GetCurrentValue()->SetPreemptByFlag(preempted_flag_);
  }
}

GpuChannel::~GpuChannel() {
  if (preempting_flag_)
    preempting_flag_->Reset();
}

void GpuChannel::OnDestroy() {
  TRACE_EVENT0("gpu", "GpuChannel::OnDestroy");
  gpu_channel_manager_->RemoveChannel(client_id_);
}

bool GpuChannel::OnControlMessageReceived(const IPC::Message& msg) {
  bool handled = true;
  IPC_BEGIN_MESSAGE_MAP(GpuChannel, msg)
    IPC_MESSAGE_HANDLER(GpuChannelMsg_CreateOffscreenCommandBuffer,
                                    OnCreateOffscreenCommandBuffer)
    IPC_MESSAGE_HANDLER(GpuChannelMsg_DestroyCommandBuffer,
                                    OnDestroyCommandBuffer)
    // Adding OpenCL API calling handle.
    IPC_MESSAGE_HANDLER(OpenCLChannelMsg_GetPlatformIDs,
                                    OnCallclGetPlatformIDs)
    IPC_MESSAGE_HANDLER(OpenCLChannelMsg_GetDeviceIDs,
                                    OnCallclGetDeviceIDs)
    IPC_MESSAGE_HANDLER(OpenCLChannelMsg_CreateSubDevices,
                                    OnCallclCreateSubDevices)
    IPC_MESSAGE_HANDLER(OpenCLChannelMsg_RetainDevice,
                                    OnCallclRetainDevice)
    IPC_MESSAGE_HANDLER(OpenCLChannelMsg_ReleaseDevice,
                                    OnCallclReleaseDevice)
    IPC_MESSAGE_HANDLER(OpenCLChannelMsg_CreateContext,
                                    OnCallclCreateContext)
    IPC_MESSAGE_HANDLER(OpenCLChannelMsg_CreateContextFromType,
                                    OnCallclCreateContextFromType)
    IPC_MESSAGE_HANDLER(OpenCLChannelMsg_RetainContext,
                                    OnCallclRetainContext)
    IPC_MESSAGE_HANDLER(OpenCLChannelMsg_ReleaseContext,
                                    OnCallclReleaseContext)
    IPC_MESSAGE_HANDLER(OpenCLChannelMsg_CreateCommandQueue,
                                    OnCallclCreateCommandQueue)
    IPC_MESSAGE_HANDLER(OpenCLChannelMsg_RetainCommandQueue,
                                    OnCallclRetainCommandQueue)
    IPC_MESSAGE_HANDLER(OpenCLChannelMsg_ReleaseCommandQueue,
                                    OnCallclReleaseCommandQueue)
    IPC_MESSAGE_HANDLER(OpenCLChannelMsg_CreateBuffer,
                                    OnCallclCreateBuffer)
    IPC_MESSAGE_HANDLER(OpenCLChannelMsg_CreateSubBuffer,
                                    OnCallclCreateSubBuffer)
    IPC_MESSAGE_HANDLER(OpenCLChannelMsg_CreateImage,
                                    OnCallclCreateImage)
    IPC_MESSAGE_HANDLER(OpenCLChannelMsg_RetainMemObject,
                                    OnCallclRetainMemObject)
    IPC_MESSAGE_HANDLER(OpenCLChannelMsg_ReleaseMemObject,
                                    OnCallclReleaseMemObject)
    IPC_MESSAGE_HANDLER(OpenCLChannelMsg_GetSupportedImageFormats,
                                    OnCallclGetSupportedImageFormats)
    IPC_MESSAGE_HANDLER(OpenCLChannelMsg_SetMemObjectDestructorCallback,
                                    OnCallclSetMemObjectDestructorCallback)
    IPC_MESSAGE_HANDLER(OpenCLChannelMsg_CreateSampler,
                                    OnCallclCreateSampler)
    IPC_MESSAGE_HANDLER(OpenCLChannelMsg_RetainSampler,
                                    OnCallclRetainSampler)
    IPC_MESSAGE_HANDLER(OpenCLChannelMsg_ReleaseSampler,
                                    OnCallclReleaseSampler)
    IPC_MESSAGE_HANDLER(OpenCLChannelMsg_CreateProgramWithSource,
                                    OnCallclCreateProgramWithSource)
    IPC_MESSAGE_HANDLER(OpenCLChannelMsg_CreateProgramWithBinary,
                                    OnCallclCreateProgramWithBinary)
    IPC_MESSAGE_HANDLER(OpenCLChannelMsg_CreateProgramWithBuiltInKernels,
                                    OnCallclCreateProgramWithBuiltInKernels)
    IPC_MESSAGE_HANDLER(OpenCLChannelMsg_RetainProgram,
                                    OnCallclRetainProgram)
    IPC_MESSAGE_HANDLER(OpenCLChannelMsg_ReleaseProgram,
                                    OnCallclReleaseProgram)
    IPC_MESSAGE_HANDLER(OpenCLChannelMsg_BuildProgram,
                                    OnCallclBuildProgram)
    IPC_MESSAGE_HANDLER(OpenCLChannelMsg_CompileProgram,
                                    OnCallclCompileProgram)
    IPC_MESSAGE_HANDLER(OpenCLChannelMsg_LinkProgram,
                                    OnCallclLinkProgram)
    IPC_MESSAGE_HANDLER(OpenCLChannelMsg_UnloadPlatformCompiler,
                                    OnCallclUnloadPlatformCompiler)
    IPC_MESSAGE_HANDLER(OpenCLChannelMsg_CreateKernel,
                                    OnCallclCreateKernel)
    IPC_MESSAGE_HANDLER(OpenCLChannelMsg_CreateKernelsInProgram,
                                    OnCallclCreateKernelsInProgram)
    IPC_MESSAGE_HANDLER(OpenCLChannelMsg_RetainKernel,
                                    OnCallclRetainKernel)
    IPC_MESSAGE_HANDLER(OpenCLChannelMsg_ReleaseKernel,
                                    OnCallclReleaseKernel)
    IPC_MESSAGE_HANDLER(OpenCLChannelMsg_SetKernelArg,
                                    OnCallclSetKernelArg)
    // Adding OK.
#if defined(OS_ANDROID)
    IPC_MESSAGE_HANDLER(GpuChannelMsg_RegisterStreamTextureProxy,
                        OnRegisterStreamTextureProxy)
    IPC_MESSAGE_HANDLER(GpuChannelMsg_EstablishStreamTexture,
                        OnEstablishStreamTexture)
#endif
    IPC_MESSAGE_HANDLER(
        GpuChannelMsg_CollectRenderingStatsForSurface,
        OnCollectRenderingStatsForSurface)
    IPC_MESSAGE_UNHANDLED(handled = false)
  IPC_END_MESSAGE_MAP()
  DCHECK(handled) << msg.type();
  return handled;
}

void GpuChannel::HandleMessage() {
  handle_messages_scheduled_ = false;
  if (deferred_messages_.empty())
    return;

  bool should_fast_track_ack = false;
  IPC::Message* m = deferred_messages_.front();
  GpuCommandBufferStub* stub = stubs_.Lookup(m->routing_id());

  do {
    if (stub) {
      if (!stub->IsScheduled())
        return;
      if (stub->IsPreempted()) {
        OnScheduled();
        return;
      }
    }

    scoped_ptr<IPC::Message> message(m);
    deferred_messages_.pop_front();
    bool message_processed = true;

    processed_get_state_fast_ =
        (message->type() == GpuCommandBufferMsg_GetStateFast::ID);

    currently_processing_message_ = message.get();
    bool result;
    if (message->routing_id() == MSG_ROUTING_CONTROL)
      result = OnControlMessageReceived(*message);
    else
      result = router_.RouteMessage(*message);
    currently_processing_message_ = NULL;

    if (!result) {
      // Respond to sync messages even if router failed to route.
      if (message->is_sync()) {
        IPC::Message* reply = IPC::SyncMessage::GenerateReply(&*message);
        reply->set_reply_error();
        Send(reply);
      }
    } else {
      // If the command buffer becomes unscheduled as a result of handling the
      // message but still has more commands to process, synthesize an IPC
      // message to flush that command buffer.
      if (stub) {
        if (stub->HasUnprocessedCommands()) {
          deferred_messages_.push_front(new GpuCommandBufferMsg_Rescheduled(
              stub->route_id()));
          message_processed = false;
        }
      }
    }
    if (message_processed)
      MessageProcessed();

    // We want the EchoACK following the SwapBuffers to be sent as close as
    // possible, avoiding scheduling other channels in the meantime.
    should_fast_track_ack = false;
    if (!deferred_messages_.empty()) {
      m = deferred_messages_.front();
      stub = stubs_.Lookup(m->routing_id());
      should_fast_track_ack =
          (m->type() == GpuCommandBufferMsg_Echo::ID) &&
          stub && stub->IsScheduled();
    }
  } while (should_fast_track_ack);

  if (!deferred_messages_.empty()) {
    OnScheduled();
  }
}

void GpuChannel::OnCreateOffscreenCommandBuffer(
    const gfx::Size& size,
    const GPUCreateCommandBufferConfig& init_params,
    int32* route_id) {
  TRACE_EVENT0("gpu", "GpuChannel::OnCreateOffscreenCommandBuffer");
  GpuCommandBufferStub* share_group = stubs_.Lookup(init_params.share_group_id);

  *route_id = GenerateRouteID();

  scoped_ptr<GpuCommandBufferStub> stub(new GpuCommandBufferStub(
      this,
      share_group,
      gfx::GLSurfaceHandle(),
      mailbox_manager_.get(),
      image_manager_.get(),
      size,
      disallowed_features_,
      init_params.allowed_extensions,
      init_params.attribs,
      init_params.gpu_preference,
      false,
      *route_id,
      0,
      watchdog_,
      software_,
      init_params.active_url));
  if (preempted_flag_)
    stub->SetPreemptByFlag(preempted_flag_);
  router_.AddRoute(*route_id, stub.get());
  stubs_.AddWithID(stub.release(), *route_id);
  TRACE_EVENT1("gpu", "GpuChannel::OnCreateOffscreenCommandBuffer",
               "route_id", route_id);
}

void GpuChannel::OnDestroyCommandBuffer(int32 route_id) {
  TRACE_EVENT1("gpu", "GpuChannel::OnDestroyCommandBuffer",
               "route_id", route_id);

  GpuCommandBufferStub* stub = stubs_.Lookup(route_id);
  if (!stub)
    return;
  bool need_reschedule = (stub && !stub->IsScheduled());
  router_.RemoveRoute(route_id);
  stubs_.Remove(route_id);
  // In case the renderer is currently blocked waiting for a sync reply from the
  // stub, we need to make sure to reschedule the GpuChannel here.
  if (need_reschedule) {
    // This stub won't get a chance to reschedule, so update the count now.
    StubSchedulingChanged(true);
  }
}

#if defined(OS_ANDROID)
void GpuChannel::OnRegisterStreamTextureProxy(
    int32 stream_id,  const gfx::Size& initial_size, int32* route_id) {
  // Note that route_id is only used for notifications sent out from here.
  // StreamTextureManager owns all texture objects and for incoming messages
  // it finds the correct object based on stream_id.
  *route_id = GenerateRouteID();
  stream_texture_manager_->RegisterStreamTextureProxy(
      stream_id, initial_size, *route_id);
}

void GpuChannel::OnEstablishStreamTexture(
    int32 stream_id, int32 primary_id, int32 secondary_id) {
  stream_texture_manager_->EstablishStreamTexture(
      stream_id, primary_id, secondary_id);
}
#endif

void GpuChannel::OnCollectRenderingStatsForSurface(
    int32 surface_id, GpuRenderingStats* stats) {
  for (StubMap::Iterator<GpuCommandBufferStub> it(&stubs_);
       !it.IsAtEnd(); it.Advance()) {
    int texture_upload_count =
        it.GetCurrentValue()->decoder()->GetTextureUploadCount();
    base::TimeDelta total_texture_upload_time =
        it.GetCurrentValue()->decoder()->GetTotalTextureUploadTime();
    base::TimeDelta total_processing_commands_time =
        it.GetCurrentValue()->decoder()->GetTotalProcessingCommandsTime();

    stats->global_texture_upload_count += texture_upload_count;
    stats->global_total_texture_upload_time += total_texture_upload_time;
    stats->global_total_processing_commands_time +=
        total_processing_commands_time;
    if (it.GetCurrentValue()->surface_id() == surface_id) {
      stats->texture_upload_count += texture_upload_count;
      stats->total_texture_upload_time += total_texture_upload_time;
      stats->total_processing_commands_time += total_processing_commands_time;
    }
  }
}

void GpuChannel::MessageProcessed() {
  messages_processed_++;
  if (preempting_flag_) {
    io_message_loop_->PostTask(
        FROM_HERE,
        base::Bind(&GpuChannelMessageFilter::MessageProcessed,
                   filter_, messages_processed_));
  }
}

void GpuChannel::CacheShader(const std::string& key,
                             const std::string& shader) {
  gpu_channel_manager_->Send(
      new GpuHostMsg_CacheShader(client_id_, key, shader));
}

// Adding the implement of OpenCL API calling handle.

void GpuChannel::OnCallclGetPlatformIDs(
    const cl_uint& num_entries,
    std::vector<cl_point>* point_platform_list,
    cl_uint* num_platforms,
    cl_int* errcode_ret) {
  // Receiving and responding the Sync IPC Message from another process
  // and return the results of clGetPlatformIDs OpenCL API calling.
  cl_platform_id* platforms = NULL;
  cl_uint* num_platforms_inter = num_platforms;

  // If the caller wishes to pass a NULL.
  if ((cl_uint) -1 == *num_platforms)
    num_platforms_inter = NULL;

  // Dump the inputs of the Sync IPC Message calling.
  if (num_entries > 0)
    platforms = new cl_platform_id[num_entries];

  // Call the OpenCL API.
  *errcode_ret = clGetPlatformIDs(
                     num_entries,
                     platforms,
                     num_platforms_inter);

  // Dump the results of OpenCL API calling.
  if (num_entries > 0) {
    (*point_platform_list).clear();
    for (cl_uint index = 0; index < num_entries; ++index)
      (*point_platform_list).push_back((cl_point) platforms[index]);
    delete[] platforms;
  }
}

void GpuChannel::OnCallclGetDeviceIDs(
    const cl_point& point_platform,
    const cl_device_type& device_type,
    const cl_uint& num_entries,
    std::vector<cl_point>* point_device_list,
    cl_uint* num_devices,
    cl_int* errcode_ret) {
  // Receiving and responding the Sync IPC Message from another process
  // and return the results of clGetDeviceIDs OpenCL API calling.
  cl_platform_id platform = (cl_platform_id) point_platform;
  cl_device_id* devices = NULL;
  cl_uint *num_devices_inter = num_devices;

  // If the caller wishes to pass a NULL.
  if ((cl_uint)-1 == *num_devices)
    num_devices_inter = NULL;

  // Dump the inputs of the Sync IPC Message calling.
  if (num_entries > 0)
    devices = new cl_device_id[num_entries];

  // Call the OpenCL API.
  *errcode_ret = clGetDeviceIDs(
                     platform,
                     device_type,
                     num_entries,
                     devices,
                     num_devices_inter);

  // Dump the results of OpenCL API calling.
  if (num_entries > 0) {
    (*point_device_list).clear();
    for (cl_uint index = 0; index < num_entries; ++index)
      (*point_device_list).push_back((cl_point) devices[index]);
    delete[] devices;
  }
}

void GpuChannel::OnCallclCreateSubDevices(
    const cl_point& point_in_device,
    const std::vector<cl_device_partition_property>& property_list,
    const cl_uint& num_devices,
    std::vector<cl_point>* point_out_device_list,
    cl_uint* num_devices_ret,
    cl_int* errcode_ret) {
  // Receiving and responding the Sync IPC Message from another process
  // and return the results of clCreateSubDevices OpenCL API calling.
  cl_device_id in_device = (cl_device_id) point_in_device;
  cl_uint *num_devices_ret_inter = num_devices_ret;
  cl_device_partition_property* properties = NULL;
  cl_device_id* out_devices = NULL;

  // If the caller wishes to pass a NULL.
  if ((cl_uint)-1 == *num_devices_ret)
    num_devices_ret_inter = NULL;

  // Dump the inputs of the Sync IPC Message calling.
  if (!property_list.empty()) {
    properties = new cl_context_properties[property_list.size()];
    for (cl_uint index = 0; index < property_list.size(); ++index)
      properties[index] = property_list[index];
  }

  if (num_devices > 0)
    out_devices = new cl_device_id[num_devices];

  // Call the OpenCL API.
  *errcode_ret = clCreateSubDevices(
                     in_device,
                     properties,
                     num_devices,
                     out_devices,
                     num_devices_ret_inter);

  if (!property_list.empty())
    delete[] properties;

  // Dump the results of OpenCL API calling.
  if (num_devices > 0) {
    (*point_out_device_list).clear();
    for (cl_uint index = 0; index < num_devices; ++index)
      (*point_out_device_list).push_back((cl_point) out_devices[index]);
    delete[] out_devices;
  }
}

void GpuChannel::OnCallclRetainDevice(
    const cl_point& point_device,
    cl_int* errcode_ret) {
  // Receiving and responding the Sync IPC Message from another process
  // and return the results of clRetainDevice OpenCL API calling.
  cl_device_id device = (cl_device_id) point_device;

  // Call the OpenCL API.
  *errcode_ret = clRetainDevice(device);
}

void GpuChannel::OnCallclReleaseDevice(
    const cl_point& point_device,
    cl_int* errcode_ret) {
  // Receiving and responding the Sync IPC Message from another process
  // and return the results of clReleaseDevice OpenCL API calling.
  cl_device_id device = (cl_device_id) point_device;

  // Call the OpenCL API.
  *errcode_ret = clReleaseDevice(device);
}

void GpuChannel::OnCallclCreateContext(
    const std::vector<cl_context_properties>& property_list,
    const cl_uint& num_devices,
    const std::vector<cl_point>& point_device_list,
    const cl_point& point_pfn_notify,
    const cl_point& point_user_data,
    cl_int* errcode_ret,
    cl_point* point_context_ret) {
  // Receiving and responding the Sync IPC Message from another process
  // and return the results of clCreateContext OpenCL API calling.
  cl_context_properties* properties = NULL;
  cl_device_id* devices = NULL;
  cl_context context_ret;
  cl_int* errcode_ret_inter = errcode_ret;
  void* user_data = (void*) point_user_data;
  void (CL_CALLBACK* pfn_notify)(const char*, const void*, size_t, void*) =
    (void (CL_CALLBACK*)(const char*, const void*, size_t, void*))
      point_pfn_notify;  

  // If the caller wishes to pass a NULL.
  if (0xFFFFFFF == *errcode_ret)
    errcode_ret_inter = NULL;

  // Dump the inputs of the Sync IPC Message calling.
  if (!property_list.empty()) {
    properties = new cl_context_properties[property_list.size()];
    for (cl_uint index = 0; index < property_list.size(); ++index)
      properties[index] = property_list[index];
  }

  if (num_devices > 0)
  {
    devices = new cl_device_id[num_devices];
    for (cl_uint index = 0; index < num_devices; ++index)
      devices[index] = (cl_device_id) point_device_list[index];
  }

  // Call the OpenCL API.
  context_ret = clCreateContext(
                    properties,
                    num_devices,
                    devices,
                    pfn_notify,
                    user_data,
                    errcode_ret_inter);

  if (!property_list.empty())
    delete[] properties;

  if (num_devices > 0)
    delete[] devices;

  *point_context_ret = (cl_point) context_ret;
}

void GpuChannel::OnCallclCreateContextFromType(
    const std::vector<cl_context_properties>& property_list,
    const cl_device_type& device_type,
    const cl_point& point_pfn_notify,
    const cl_point& point_user_data,
    cl_int* errcode_ret,
    cl_point* point_context_ret) {
  // Receiving and responding the Sync IPC Message from another process
  // and return the results of clCreateContextFromType OpenCL API calling.
  cl_context_properties* properties = NULL;
  cl_context context_ret;
  cl_int* errcode_ret_inter = errcode_ret;
  void* user_data = (void*) point_user_data;
  void (CL_CALLBACK* pfn_notify)(const char*, const void*, size_t, void*) =
    (void (CL_CALLBACK*)(const char*, const void*, size_t, void*))
      point_pfn_notify;  

  // If the caller wishes to pass a NULL.
  if (0xFFFFFFF == *errcode_ret)
    errcode_ret_inter = NULL;

  // Dump the inputs of the Sync IPC Message calling.
  if (!property_list.empty()) {
    properties = new cl_context_properties[property_list.size()];
    for (cl_uint index = 0; index < property_list.size(); ++index)
      properties[index] = property_list[index];
  }

  // Call the OpenCL API.
  context_ret = clCreateContextFromType(
                    properties,
                    device_type,
                    pfn_notify,
                    user_data,
                    errcode_ret_inter);

  if (!property_list.empty())
    delete[] properties;

  // Dump the results of OpenCL API calling.
  *point_context_ret = (cl_point) context_ret;
}

void GpuChannel::OnCallclRetainContext (
    const cl_point& point_context,
    cl_int* errcode_ret)
{
  // Receiving and responding the Sync IPC Message from another process
  // and return the results of clRetainContext OpenCL API calling.
  cl_context context = (cl_context) point_context;

  // Call the OpenCL API.
  *errcode_ret = clRetainContext(context);
}

void GpuChannel::OnCallclReleaseContext (
    const cl_point& point_context,
    cl_int* errcode_ret)
{
  // Receiving and responding the Sync IPC Message from another process
  // and return the results of clReleaseContext OpenCL API calling.
  cl_context context = (cl_context) point_context;

  // Call the OpenCL API.
  *errcode_ret = clReleaseContext(context);
}

void GpuChannel::OnCallclCreateCommandQueue(
    const cl_point& point_context,
    const cl_point& point_device,
    const cl_command_queue_properties& properties,
    cl_int* errcode_ret,
    cl_point* point_command_queue_ret) {
  // Receiving and responding the Sync IPC Message from another process
  // and return the results of clCreateCommandQueue OpenCL API calling.
  cl_context context = (cl_context) point_context;
  cl_device_id device = (cl_device_id) point_device;
  cl_command_queue command_queue_ret;
  cl_int* errcode_ret_inter = errcode_ret;

  // If the caller wishes to pass a NULL.
  if (0xFFFFFFF == *errcode_ret)
    errcode_ret_inter = NULL;

  // Call the OpenCL API.
  command_queue_ret = clCreateCommandQueue(
                          context,
                          device,
                          properties,
                          errcode_ret_inter);

  // Dump the results of OpenCL API calling.
  *point_command_queue_ret = (cl_point) command_queue_ret;
}

void GpuChannel::OnCallclRetainCommandQueue(
    const cl_point& point_command_queue,
    cl_int* errcode_ret) {
  // Receiving and responding the Sync IPC Message from another process
  // and return the results of clRetainCommandQueue OpenCL API calling.
  cl_command_queue command_queue = (cl_command_queue) point_command_queue;

  // Call the OpenCL API.
  *errcode_ret = clRetainCommandQueue(command_queue);
}

void GpuChannel::OnCallclReleaseCommandQueue(
    const cl_point& point_command_queue,
    cl_int* errcode_ret) {
  // Receiving and responding the Sync IPC Message from another process
  // and return the results of clReleaseCommandQueue OpenCL API calling.
  cl_command_queue command_queue = (cl_command_queue) point_command_queue;

  // Call the OpenCL API.
  *errcode_ret = clReleaseCommandQueue(command_queue);
}

void GpuChannel::OnCallclCreateBuffer(
    const cl_point& point_context,
    const cl_mem_flags& flags,
    const size_t& size,
    const cl_point& point_host_ptr,
    cl_int* errcode_ret,
    cl_point* point_memobj_ret) {
  // Receiving and responding the Sync IPC Message from another process
  // and return the results of clCreateBuffer OpenCL API calling.
  cl_context context = (cl_context) point_context;
  cl_mem memobj_ret;
  cl_int* errcode_ret_inter = errcode_ret;
  void* host_ptr = (void*) point_host_ptr;

  // If the caller wishes to pass a NULL.
  if (0xFFFFFFF == *errcode_ret)
    errcode_ret_inter = NULL;

  // Call the OpenCL API.
  memobj_ret = clCreateBuffer(
                   context,
                   flags,
                   size,
                   host_ptr,
                   errcode_ret_inter);

  // Dump the results of OpenCL API calling.
  *point_memobj_ret = (cl_point) memobj_ret;
}

void GpuChannel::OnCallclCreateSubBuffer(
    const cl_point& point_buffer,
    const cl_mem_flags& flags,
    const cl_buffer_create_type& buffer_create_type,
    const cl_point& point_buffer_create_info,
    cl_int* errcode_ret,
    cl_point* point_memobj_ret) {
  // Receiving and responding the Sync IPC Message from another process
  // and return the results of clCreateSubBuffer OpenCL API calling.
  cl_mem buffer = (cl_mem) point_buffer;
  cl_mem memobj_ret;
  cl_int* errcode_ret_inter = errcode_ret;
  void* buffer_create_info = (void*) point_buffer_create_info;

  // If the caller wishes to pass a NULL.
  if (0xFFFFFFF == *errcode_ret)
    errcode_ret_inter = NULL;

  // Call the OpenCL API.
  memobj_ret = clCreateSubBuffer(
                   buffer,
                   flags,
                   buffer_create_type,
                   buffer_create_info,
                   errcode_ret_inter);

  // Dump the results of OpenCL API calling.
  *point_memobj_ret = (cl_point) memobj_ret;
}

void GpuChannel::OnCallclCreateImage(
    const cl_point& point_context,
    const cl_mem_flags& flags,
    const std::vector<cl_uint>& image_format_list,
    const cl_point& point_host_ptr,
    cl_int* errcode_ret,
    cl_point* point_memobj_ret) {
  // Receiving and responding the Sync IPC Message from another process
  // and return the results of clCreateImage OpenCL API calling.
  cl_context context = (cl_context) point_context;
  cl_mem memobj_ret;
  cl_int* errcode_ret_inter = errcode_ret;
  void* host_ptr = (void*) point_host_ptr;
  cl_image_format image_format;
  cl_image_desc image_desc;

  // If the caller wishes to pass a NULL.
  if (0xFFFFFFF == *errcode_ret)
    errcode_ret_inter = NULL;

  // Dump the inputs of the Sync IPC Message calling.
  image_format.image_channel_order = image_format_list[0];
  image_format.image_channel_data_type = image_format_list[1];

  // Call the OpenCL API.
  // There are some bugs here, we must add some code to fully support it.
  memobj_ret = clCreateImage(
                   context,
                   flags,
                   &image_format,
                   &image_desc,
                   host_ptr,
                   errcode_ret_inter);

  // Dump the results of OpenCL API calling.
  *point_memobj_ret = (cl_point) memobj_ret;
}

void GpuChannel::OnCallclRetainMemObject(
    const cl_point& point_memobj,
    cl_int* errcode_ret) {
  // Receiving and responding the Sync IPC Message from another process
  // and return the results of clRetainMemObject OpenCL API calling.
  cl_mem memobj = (cl_mem) point_memobj;

  // Call the OpenCL API.
  *errcode_ret = clRetainMemObject(memobj);
}

void GpuChannel::OnCallclReleaseMemObject(
    const cl_point& point_memobj,
    cl_int* errcode_ret) {
  // Receiving and responding the Sync IPC Message from another process
  // and return the results of clReleaseMemObject OpenCL API calling.
  cl_mem memobj = (cl_mem) point_memobj;

  // Call the OpenCL API.
  *errcode_ret = clReleaseMemObject(memobj);
}

void GpuChannel::OnCallclGetSupportedImageFormats(
    const cl_point& point_context,
    const cl_mem_flags& flags,
    const cl_mem_object_type& image_type,
    const cl_uint& num_entries,
    std::vector<cl_uint>* image_format_list,
    cl_uint* num_image_formats,
    cl_int* errcode_ret) {
  // Receiving and responding the Sync IPC Message from another process
  // and return the results of clGetSupportedImageFormats OpenCL API calling.
  cl_context context = (cl_context) point_context;
  cl_uint *num_image_formats_inter = num_image_formats;
  cl_image_format* image_formats = NULL;

  // If the caller wishes to pass a NULL.
  if((cl_uint)-1 == *num_image_formats)
    num_image_formats_inter = NULL;

  // Dump the inputs of the Sync IPC Message calling.
  if (num_entries > 0)
    image_formats = new cl_image_format[num_entries];

  // Call the OpenCL API.
  *errcode_ret = clGetSupportedImageFormats(
                     context,
                     flags,
                     image_type,
                     num_entries,
                     image_formats,
                     num_image_formats_inter);

  // Dump the results of OpenCL API calling.
  if (num_entries > 0) {
    (*image_format_list).clear();
    for (cl_uint index = 0; index < num_entries; ++index) {
      (*image_format_list).push_back(
                               image_formats[index].image_channel_data_type);
      (*image_format_list).push_back(
                               image_formats[index].image_channel_order);
    }
    delete[] image_formats;
  }
}

void GpuChannel::OnCallclSetMemObjectDestructorCallback(
    const cl_point& point_memobj,
    const cl_point& point_pfn_notify,
    const cl_point& point_user_data,
    cl_int * errcode_ret) {
  // Receiving and responding the Sync IPC Message from another process
  // and return the results of clSetMemObjectDestructorCallback
  // OpenCL API calling.
  cl_mem memobj = (cl_mem) point_memobj;
  void (CL_CALLBACK* pfn_notify)(cl_mem, void* ) =
    (void (CL_CALLBACK*)(cl_mem, void* )) point_pfn_notify;
  void* user_data = (void*) point_user_data;

  // Call the OpenCL API.
  *errcode_ret = clSetMemObjectDestructorCallback(
                     memobj,
                     pfn_notify,
                     user_data);
}

void GpuChannel::OnCallclCreateSampler (
    const cl_point& point_context,
    const cl_bool& normalized_coords,
    const cl_addressing_mode& addressing_mode,
    const cl_filter_mode& filter_mode,
    cl_int* errcode_ret,
    cl_point* point_sampler_ret) {
  // Receiving and responding the Sync IPC Message from another process
  // and return the results of clCreateSampler OpenCL API calling.
  cl_context context = (cl_context) point_context;
  cl_sampler sampler_ret;
  cl_int* errcode_ret_inter = errcode_ret;

  // If the caller wishes to pass a NULL.
  if (0xFFFFFFF == *errcode_ret)
    errcode_ret_inter = NULL;

  // Call the OpenCL API.
  sampler_ret = clCreateSampler(
                    context,
                    normalized_coords,
                    addressing_mode,
                    filter_mode,
                    errcode_ret_inter);

  // Dump the results of OpenCL API calling.
  *point_sampler_ret = (cl_point) sampler_ret;
}

void GpuChannel::OnCallclRetainSampler(
    const cl_point& point_sampler,
    cl_int* errcode_ret) {
  // Receiving and responding the Sync IPC Message from another process
  // and return the results of clRetainSampler OpenCL API calling.
  cl_sampler sampler = (cl_sampler) point_sampler;

  // Call the OpenCL API.
  *errcode_ret = clRetainSampler(sampler);
}

void GpuChannel::OnCallclReleaseSampler(
    const cl_point& point_sampler,
    cl_int* errcode_ret) {
  // Receiving and responding the Sync IPC Message from another process
  // and return the results of clReleaseSampler OpenCL API calling.
  cl_sampler sampler = (cl_sampler) point_sampler;

  // Call the OpenCL API.
  *errcode_ret = clReleaseSampler(sampler);
}

void GpuChannel::OnCallclCreateProgramWithSource(
    const cl_point& point_context,
    const cl_uint& count,
    const std::vector<std::string>& string_list,
    const std::vector<size_t>& length_list,
    cl_int* errcode_ret,
    cl_point* point_program_ret) {
  // Receiving and responding the Sync IPC Message from another process
  // and return the results of clCreateProgramWithSource OpenCL API calling.
  cl_context context = (cl_context) point_context;
  const char **strings = NULL;
  size_t *lengths = NULL;
  cl_program program_ret;
  cl_int* errcode_ret_inter = errcode_ret;

  // If the caller wishes to pass a NULL.
  if (0xFFFFFFF == *errcode_ret)
    errcode_ret_inter = NULL;

  // Dump the inputs of the Sync IPC Message calling.
  if (count > 0) {
    strings = new const char*[count];
    lengths = new size_t[count];
    for(cl_uint index = 0; index < count; ++index) {
      strings[index] = string_list[index].c_str();
      lengths[index] = length_list[index];
    }
  }

  // Call the OpenCL API.
  program_ret = clCreateProgramWithSource(
                    context,
                    count,
                    strings,
                    lengths,
                    errcode_ret);

  if (count > 0) {
    delete[] strings;
    delete[] lengths;
  }

  // Dump the results of OpenCL API calling.
  *point_program_ret = (cl_point) program_ret;
}

void GpuChannel::OnCallclCreateProgramWithBinary(
    const cl_point& point_context,
    const cl_uint& num_devices,
    const std::vector<cl_point>& point_device_list,
    const std::vector<size_t>& length_list,
    const std::vector<std::vector<unsigned char>>& binary_list,
    std::vector<cl_int>* binary_status_list,
    cl_int* errcode_ret,
    cl_point* point_out_val) {
  // Receiving and responding the Sync IPC Message from another process
  // and return the results of clCreateProgramWithBinary OpenCL API calling.
  cl_context context = (cl_context) point_context;
  cl_device_id* device_list = NULL;
  size_t* lengths = NULL;
  const unsigned char** binaries = NULL;
  cl_int* binary_status = NULL;
  cl_program program_ret = NULL;
  cl_int* errcode_ret_inter = errcode_ret;

  // If the caller wishes to pass a NULL.
  if (0xFFFFFFF == *errcode_ret)
    errcode_ret_inter = NULL;

  // Dump the inputs of the Sync IPC Message calling.
  if (num_devices > 0) {
    device_list = new cl_device_id[num_devices];
    lengths = new size_t[num_devices];
    binary_status = new cl_int[num_devices];
    for(cl_uint index = 0; index < num_devices; ++index) {
      device_list[index] = (cl_device_id) point_device_list[index];
      lengths[index] = length_list[index];
      // We need to add some better ways to improve
      // the performance of transfer kernel.
      // So this API is not fully supported.
    }
  }

  // Call the OpenCL API.
  program_ret = clCreateProgramWithBinary(
                    context,
                    num_devices,
                    device_list,
                    lengths,
                    binaries,
                    binary_status,
                    errcode_ret);

  // Dump the results of OpenCL API calling.
  if (num_devices > 0) {
    (*binary_status_list).clear();
    for(cl_uint index = 0; index < num_devices; ++index)
      (*binary_status_list).push_back(binary_status[index]);

    delete[] device_list;
    delete[] lengths;
    delete[] binary_status;
  }
  *point_out_val = (cl_point) program_ret;
}

void GpuChannel::OnCallclCreateProgramWithBuiltInKernels(
    const cl_point& point_context,
    const cl_uint& num_devices,
    const std::vector<cl_point>& point_device_list,
    const std::string& string_kernel_names,
    cl_int* errcode_ret,
    cl_point* point_progrem_ret) {
  // Receiving and responding the Sync IPC Message from another process
  // and return the results of clCreateProgramWithBuiltInKernels
  // OpenCL API calling.
  cl_context context = (cl_context) point_context;
  size_t *lengths = NULL;
  cl_device_id* device_list = NULL;
  cl_program program_ret;
  cl_int* errcode_ret_inter = errcode_ret;

  // If the caller wishes to pass a NULL.
  if (0xFFFFFFF == *errcode_ret)
    errcode_ret_inter = NULL;

  // Dump the inputs of the Sync IPC Message calling.
  if (num_devices > 0) {
    device_list = new cl_device_id[num_devices];
    for(cl_uint index = 0; index < num_devices; ++index)
      device_list[index] = (cl_device_id) point_device_list[index];
  }

  // Call the OpenCL API.
  program_ret = clCreateProgramWithBuiltInKernels(
                    context,
                    num_devices,
                    device_list,
                    string_kernel_names.c_str(),
                    errcode_ret_inter);

  if (num_devices > 0)
    delete[] device_list;

  // Dump the results of OpenCL API calling.
  *point_progrem_ret = (cl_point) program_ret;
}

void GpuChannel::OnCallclRetainProgram(
    const cl_point& point_program,
    cl_int* errcode_ret) {
  // Receiving and responding the Sync IPC Message from another process
  // and return the results of clRetainProgram OpenCL API calling.
  cl_program program = (cl_program) point_program;

  // Call the OpenCL API.
  *errcode_ret = clRetainProgram(program);
}

void GpuChannel::OnCallclReleaseProgram(
    const cl_point& point_program,
    cl_int* errcode_ret) {
  // Receiving and responding the Sync IPC Message from another process
  // and return the results of clReleaseProgram OpenCL API calling.
  cl_program program = (cl_program) point_program;

  // Call the OpenCL API.
  *errcode_ret = clReleaseProgram(program);
}

void GpuChannel::OnCallclBuildProgram(
    const cl_point& point_program,
    const cl_uint& num_devices,
    const std::vector<cl_point>& point_device_list,
    const std::string& str_options,
    const std::vector<cl_point>& point_pfn_list,
    cl_int* errcode_ret) {
  // Receiving and responding the Sync IPC Message from another process
  // and return the results of clBuildProgram OpenCL API calling.
  cl_program program = (cl_program) point_program;
  cl_device_id* device_list = NULL;
  void (CL_CALLBACK* pfn_notify)(cl_program, void*) =
      (void (CL_CALLBACK*)(cl_program, void*)) point_pfn_list[0];
  void* user_data = (void*) point_pfn_list[1];

  // Dump the inputs of the Sync IPC Message calling.
  if (num_devices) {
    device_list = new cl_device_id[num_devices];
    for (cl_uint index = 0; index < num_devices; ++index)
      device_list[index] = (cl_device_id) point_device_list[index];
  }

  // Call the OpenCL API.
  *errcode_ret = clBuildProgram(
                     program,
                     num_devices,
                     device_list,
                     str_options.c_str(),
                     pfn_notify,
                     user_data);

  if (num_devices)
    delete[] device_list;
}

void GpuChannel::OnCallclCompileProgram(
    const std::vector<cl_point>& point_parameter_list,
    const std::vector<cl_uint>& num_list,
    const std::vector<cl_point>& point_device_list,
    const std::vector<std::string>& options_header_include_name_list,
    const std::vector<cl_point>& point_input_header_list,
    cl_int* errcode_ret) {
  // Receiving and responding the Sync IPC Message from another process
  // and return the results of clCompileProgram OpenCL API calling.
  cl_program program = (cl_program) point_parameter_list[0];
  void (CL_CALLBACK* pfn_notify)(cl_program, void*) =
      (void (CL_CALLBACK*)(cl_program, void*)) point_parameter_list[1];
  void* user_data = (void*) point_parameter_list[2];
  cl_uint num_devices = num_list[0];
  cl_uint num_input_headers = num_list[1];
  cl_device_id* device_list = NULL;
  cl_program* input_headers = NULL;
  const char** header_include_names = NULL;

  // Dump the inputs of the Sync IPC Message calling.
  if (num_devices > 0) {
    device_list = new cl_device_id[num_devices];
    for (cl_uint index = 0; index < num_devices; ++index)
      device_list[index] = (cl_device_id) point_device_list[index];
  }

  if (num_input_headers > 0) {
    input_headers = new cl_program[num_input_headers];
    for (cl_uint index = 0; index < num_input_headers; ++index) {
      input_headers[index] = (cl_program) point_input_header_list[index];
      header_include_names[index] =
          options_header_include_name_list[index + 1].c_str();
    }
  }

  // Call the OpenCL API.
  *errcode_ret = clCompileProgram(
                     program,
                     num_devices,
                     device_list,
                     options_header_include_name_list[0].c_str(),
                     num_input_headers,
                     input_headers,
                     header_include_names,
                     pfn_notify, user_data);
}

void GpuChannel::OnCallclLinkProgram(
    const std::vector<cl_point>& point_parameter_list,
    const std::vector<cl_uint>& num_list,
    const std::vector<cl_point>& point_device_list,
    const std::vector<cl_point>& point_input_program_list,
    const std::string& str_options,
    cl_int* errcode_ret,
    cl_point* point_program_ret) {
  // Receiving and responding the Sync IPC Message from another process
  // and return the results of clLinkProgram OpenCL API calling.
  cl_context context = (cl_context) point_parameter_list[0];
  void (CL_CALLBACK* pfn_notify)(cl_program, void*) =
    (void (CL_CALLBACK*)(cl_program, void*)) point_parameter_list[1];
  void* user_data = (void*) point_parameter_list[2];
  cl_uint num_devices = num_list[0];
  cl_uint num_input_programs = num_list[1];
  cl_device_id* device_list = NULL;
  cl_program* input_programs = NULL;
  cl_program program_ret;
  cl_int* errcode_ret_inter = errcode_ret;

  // If the caller wishes to pass a NULL.
  if (0xFFFFFFF == *errcode_ret)
    errcode_ret_inter = NULL;

  // Dump the inputs of the Sync IPC Message calling.
  if (num_devices > 0) {
    device_list = new cl_device_id[num_devices];
    for (cl_uint index = 0; index < num_devices; ++index)
      device_list[index] = (cl_device_id) point_device_list[index];
  }

  if (num_input_programs > 0) {
    input_programs = new cl_program[num_input_programs];
    for (cl_uint index = 0; index < num_input_programs; ++index)
      input_programs[index] = (cl_program) point_input_program_list[index];
  }

  // Call the OpenCL API.
  program_ret = clLinkProgram(
                    context,
                    num_devices,
                    device_list,
                    str_options.c_str(),
                    num_input_programs,
                    input_programs,
                    pfn_notify,
                    user_data,
                    errcode_ret_inter);

  // Dump the results of OpenCL API calling.
  *point_program_ret = (cl_point) program_ret;
}

void GpuChannel::OnCallclUnloadPlatformCompiler(
    const cl_point& point_platform,
    cl_int* errcode_ret) {
  // Receiving and responding the Sync IPC Message from another process
  // and return the results of clUnloadPlatformCompiler OpenCL API calling.
  cl_platform_id platform = (cl_platform_id) point_platform;

  // Call the OpenCL API.
  *errcode_ret = clUnloadPlatformCompiler(platform);
}

void GpuChannel::OnCallclCreateKernel(
    const cl_point& point_program,
    const std::string& string_kernel_name,
    cl_int* errcode_ret,
    cl_point* point_kernel_ret) {
  // Receiving and responding the Sync IPC Message from another process
  // and return the results of clCreateKernel OpenCL API calling.
  cl_program program = (cl_program) point_program;
  cl_kernel kernel_ret;
  cl_int* errcode_ret_inter = errcode_ret;

  // If the caller wishes to pass a NULL.
  if (0xFFFFFFF == *errcode_ret)
    errcode_ret_inter = NULL;

  // Call the OpenCL API.
  kernel_ret = clCreateKernel(
                   program,
                   string_kernel_name.c_str(),
                   errcode_ret_inter);

  // Dump the inputs of the Sync IPC Message calling.
  *point_kernel_ret = (cl_point) kernel_ret;
}

void GpuChannel::OnCallclCreateKernelsInProgram(
    const cl_point& point_program,
    const cl_uint& num_kernels,
    const std::vector<cl_point>& point_kernel_list,
    cl_uint* num_kernels_ret,
    cl_int* errcode_ret) {
  // Receiving and responding the Sync IPC Message from another process
  // and return the results of clCreateKernelsInProgram OpenCL API calling.
  cl_program program = (cl_program) point_program;
  cl_kernel* kernels = NULL;
  cl_uint *num_kernels_ret_inter = num_kernels_ret;

  // If the caller wishes to pass a NULL.
  if ((cl_uint) -1 == *num_kernels_ret)
    num_kernels_ret_inter = NULL;

  // Dump the inputs of the Sync IPC Message calling.
  if (num_kernels > 0) {
    cl_kernel* kernels = new cl_kernel[num_kernels];
    for (cl_uint index = 0; index < num_kernels; ++index)
      kernels[index] = (cl_kernel) point_kernel_list[index];
  }

  // Call the OpenCL API.
  *errcode_ret = clCreateKernelsInProgram(
                     program,
                     num_kernels,
                     kernels,
                     num_kernels_ret_inter);

  if (num_kernels > 0)
    delete[] kernels;
}

void GpuChannel::OnCallclRetainKernel(
    const cl_point& point_kernel,
    cl_int* errcode_ret) {
  // Receiving and responding the Sync IPC Message from another process
  // and return the results of clRetainKernel OpenCL API calling.
  cl_kernel kernel = (cl_kernel) point_kernel;

  // Call the OpenCL API.
  *errcode_ret = clRetainKernel(kernel);
}

void GpuChannel::OnCallclReleaseKernel(
    const cl_point& point_kernel,
    cl_int* errcode_ret) {
  // Receiving and responding the Sync IPC Message from another process
  // and return the results of clReleaseKernel OpenCL API calling.
  cl_kernel kernel = (cl_kernel) point_kernel;

  // Call the OpenCL API.
  *errcode_ret = clReleaseKernel(kernel);
}

void GpuChannel::OnCallclSetKernelArg(
    const cl_point& point_kernel,
    const cl_uint& arg_index,
    const size_t& arg_size,
    const cl_point& point_arg_value,
    cl_int* errcode_ret)
{
  // Receiving and responding the Sync IPC Message from another process
  // and return the results of clSetKernelArg OpenCL API calling.
  cl_kernel kernel = (cl_kernel) point_kernel;
  const void* arg_value = (void*) point_arg_value;
 
  // Call the OpenCL API.
  *errcode_ret = clSetKernelArg(
                     kernel,
                     arg_index,
                     arg_size,
                     arg_value);
}
}  // namespace content
