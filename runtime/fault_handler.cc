/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "fault_handler.h"

#include <atomic>
#include <string.h>
#include <sys/mman.h>
#include <sys/ucontext.h>

#include "art_method-inl.h"
#include "base/logging.h"  // For VLOG
#include "base/membarrier.h"
#include "base/safe_copy.h"
#include "base/stl_util.h"
#include "dex/dex_file_types.h"
#include "jit/jit.h"
#include "jit/jit_code_cache.h"
#include "mirror/class.h"
#include "mirror/object_reference.h"
#include "oat_file.h"
#include "oat_quick_method_header.h"
#include "sigchain.h"
#include "thread-current-inl.h"
#include "verify_object-inl.h"

namespace art {
// Static fault manger object accessed by signal handler.
FaultManager fault_manager;

// This needs to be NO_INLINE since some debuggers do not read the inline-info to set a breakpoint
// if it isn't.
extern "C" NO_INLINE __attribute__((visibility("default"))) void art_sigsegv_fault() {
  // Set a breakpoint here to be informed when a SIGSEGV is unhandled by ART.
  VLOG(signals)<< "Caught unknown SIGSEGV in ART fault handler - chaining to next handler.";
}

// Signal handler called on SIGSEGV.
static bool art_fault_handler(int sig, siginfo_t* info, void* context) {
  return fault_manager.HandleFault(sig, info, context);
}

struct FaultManager::GeneratedCodeRange {
  std::atomic<GeneratedCodeRange*> next;
  const void* start;
  size_t size;
};

FaultManager::FaultManager()
    : generated_code_ranges_lock_("FaultHandler generated code ranges lock",
                                  LockLevel::kGenericBottomLock),
      initialized_(false) {
  sigaction(SIGSEGV, nullptr, &oldaction_);
}

FaultManager::~FaultManager() {
}

void FaultManager::Init() {
  CHECK(!initialized_);
  sigset_t mask;
  sigfillset(&mask);
  sigdelset(&mask, SIGABRT);
  sigdelset(&mask, SIGBUS);
  sigdelset(&mask, SIGFPE);
  sigdelset(&mask, SIGILL);
  sigdelset(&mask, SIGSEGV);

  SigchainAction sa = {
    .sc_sigaction = art_fault_handler,
    .sc_mask = mask,
    .sc_flags = 0UL,
  };

  AddSpecialSignalHandlerFn(SIGSEGV, &sa);

  // Notify the kernel that we intend to use a specific `membarrier()` command.
  int result = art::membarrier(MembarrierCommand::kRegisterPrivateExpedited);
  if (result != 0) {
    LOG(WARNING) << "FaultHandler: MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED failed: "
                 << errno << " " << strerror(errno);
  }

  initialized_ = true;
}

void FaultManager::Release() {
  if (initialized_) {
    RemoveSpecialSignalHandlerFn(SIGSEGV, art_fault_handler);
    initialized_ = false;
  }
}

void FaultManager::Shutdown() {
  if (initialized_) {
    Release();

    // Free all handlers.
    STLDeleteElements(&generated_code_handlers_);
    STLDeleteElements(&other_handlers_);

    // Delete remaining code ranges if any (such as nterp code or oat code from
    // oat files that have not been unloaded, including boot image oat files).
    GeneratedCodeRange* range;
    {
      MutexLock lock(Thread::Current(), generated_code_ranges_lock_);
      range = generated_code_ranges_.load(std::memory_order_acquire);
      generated_code_ranges_.store(nullptr, std::memory_order_release);
    }
    while (range != nullptr) {
      GeneratedCodeRange* next_range = range->next.load(std::memory_order_relaxed);
      delete range;
      range = next_range;
    }
  }
}

bool FaultManager::HandleFaultByOtherHandlers(int sig, siginfo_t* info, void* context) {
  if (other_handlers_.empty()) {
    return false;
  }

  Thread* self = Thread::Current();

  DCHECK(self != nullptr);
  DCHECK(Runtime::Current() != nullptr);
  DCHECK(Runtime::Current()->IsStarted());
  for (const auto& handler : other_handlers_) {
    if (handler->Action(sig, info, context)) {
      return true;
    }
  }
  return false;
}

static const char* SignalCodeName(int sig, int code) {
  if (sig != SIGSEGV) {
    return "UNKNOWN";
  } else {
    switch (code) {
      case SEGV_MAPERR: return "SEGV_MAPERR";
      case SEGV_ACCERR: return "SEGV_ACCERR";
      case 8:           return "SEGV_MTEAERR";
      case 9:           return "SEGV_MTESERR";
      default:          return "UNKNOWN";
    }
  }
}
static std::ostream& PrintSignalInfo(std::ostream& os, siginfo_t* info) {
  os << "  si_signo: " << info->si_signo << " (" << strsignal(info->si_signo) << ")\n"
     << "  si_code: " << info->si_code
     << " (" << SignalCodeName(info->si_signo, info->si_code) << ")";
  if (info->si_signo == SIGSEGV) {
    os << "\n" << "  si_addr: " << info->si_addr;
  }
  return os;
}

bool FaultManager::HandleFault(int sig, siginfo_t* info, void* context) {
  if (VLOG_IS_ON(signals)) {
    PrintSignalInfo(VLOG_STREAM(signals) << "Handling fault:" << "\n", info);
  }

#ifdef TEST_NESTED_SIGNAL
  // Simulate a crash in a handler.
  raise(SIGSEGV);
#endif

  if (IsInGeneratedCode(info, context)) {
    VLOG(signals) << "in generated code, looking for handler";
    for (const auto& handler : generated_code_handlers_) {
      VLOG(signals) << "invoking Action on handler " << handler;
      if (handler->Action(sig, info, context)) {
        // We have handled a signal so it's time to return from the
        // signal handler to the appropriate place.
        return true;
      }
    }
  }

  // We hit a signal we didn't handle.  This might be something for which
  // we can give more information about so call all registered handlers to
  // see if it is.
  if (HandleFaultByOtherHandlers(sig, info, context)) {
    return true;
  }

  // Set a breakpoint in this function to catch unhandled signals.
  art_sigsegv_fault();
  return false;
}

void FaultManager::AddHandler(FaultHandler* handler, bool generated_code) {
  DCHECK(initialized_);
  if (generated_code) {
    generated_code_handlers_.push_back(handler);
  } else {
    other_handlers_.push_back(handler);
  }
}

void FaultManager::RemoveHandler(FaultHandler* handler) {
  auto it = std::find(generated_code_handlers_.begin(), generated_code_handlers_.end(), handler);
  if (it != generated_code_handlers_.end()) {
    generated_code_handlers_.erase(it);
    return;
  }
  auto it2 = std::find(other_handlers_.begin(), other_handlers_.end(), handler);
  if (it2 != other_handlers_.end()) {
    other_handlers_.erase(it2);
    return;
  }
  LOG(FATAL) << "Attempted to remove non existent handler " << handler;
}

void FaultManager::AddGeneratedCodeRange(const void* start, size_t size) {
  GeneratedCodeRange* new_range = new GeneratedCodeRange{nullptr, start, size};
  {
    MutexLock lock(Thread::Current(), generated_code_ranges_lock_);
    GeneratedCodeRange* old_head = generated_code_ranges_.load(std::memory_order_relaxed);
    new_range->next.store(old_head, std::memory_order_relaxed);
    generated_code_ranges_.store(new_range, std::memory_order_release);
  }

  // The above release operation on `generated_code_ranges_` with an acquire operation
  // on the same atomic object in `IsInGeneratedCode()` ensures the correct memory
  // visibility for the contents of `*new_range` for any thread that loads the value
  // written above (or a value written by a release sequence headed by that write).
  //
  // However, we also need to ensure that any thread that encounters a segmentation
  // fault in the provided range shall actually see the written value. For JIT code
  // cache and nterp, the registration happens while the process is single-threaded
  // but the synchronization is more complicated for code in oat files.
  //
  // Threads that load classes register dex files under the `Locks::dex_lock_` and
  // the first one to register a dex file with a given oat file shall add the oat
  // code range; the memory visibility for these threads is guaranteed by the lock.
  // However a thread that did not try to load a class with oat code can execute the
  // code if a direct or indirect reference to such class escapes from one of the
  // threads that loaded it. Use `membarrier()` for memory visibility in this case.
  art::membarrier(MembarrierCommand::kPrivateExpedited);
}

void FaultManager::RemoveGeneratedCodeRange(const void* start, size_t size) {
  Thread* self = Thread::Current();
  GeneratedCodeRange* range = nullptr;
  {
    MutexLock lock(self, generated_code_ranges_lock_);
    std::atomic<GeneratedCodeRange*>* before = &generated_code_ranges_;
    range = before->load(std::memory_order_relaxed);
    while (range != nullptr && range->start != start) {
      before = &range->next;
      range = before->load(std::memory_order_relaxed);
    }
    if (range != nullptr) {
      GeneratedCodeRange* next = range->next.load(std::memory_order_relaxed);
      if (before == &generated_code_ranges_) {
        // Relaxed store directly to `generated_code_ranges_` would not satisfy
        // conditions for a release sequence, so we need to use store-release.
        before->store(next, std::memory_order_release);
      } else {
        // In the middle of the list, we can use a relaxed store as we're not
        // publishing any newly written memory to potential reader threads.
        // Whether they see the removed node or not is unimportant as we should
        // not execute that code anymore. We're keeping the `next` link of the
        // removed node, so that concurrent walk can use it to reach remaining
        // retained nodes, if any.
        before->store(next, std::memory_order_relaxed);
      }
    }
  }
  CHECK(range != nullptr);
  DCHECK_EQ(range->start, start);
  CHECK_EQ(range->size, size);

  Runtime* runtime = Runtime::Current();
  CHECK(runtime != nullptr);
  if (runtime->IsStarted() && runtime->GetThreadList() != nullptr) {
    // Run a checkpoint before deleting the range to ensure that no thread holds a
    // pointer to the removed range while walking the list in `IsInGeneratedCode()`.
    // That walk is guarded by checking that the thread is `Runnable`, so any walk
    // started before the removal shall be done when running the checkpoint and the
    // checkpoint also ensures the correct memory visibility of `next` links,
    // so the thread shall not see the pointer during future walks.
    runtime->GetThreadList()->RunEmptyCheckpoint();
  }
  delete range;
}

// This function is called within the signal handler. It checks that the thread
// is `Runnable`, the `mutator_lock_` is held (shared) and the fault PC is in one
// of the registered generated code ranges. No annotalysis is done.
bool FaultManager::IsInGeneratedCode(siginfo_t* siginfo, void* context) {
  // We can only be running Java code in the current thread if it
  // is in Runnable state.
  VLOG(signals) << "Checking for generated code";
  Thread* thread = Thread::Current();
  if (thread == nullptr) {
    VLOG(signals) << "no current thread";
    return false;
  }

  ThreadState state = thread->GetState();
  if (state != ThreadState::kRunnable) {
    VLOG(signals) << "not runnable";
    return false;
  }

  // Current thread is runnable.
  // Make sure it has the mutator lock.
  if (!Locks::mutator_lock_->IsSharedHeld(thread)) {
    VLOG(signals) << "no lock";
    return false;
  }

  uintptr_t fault_pc = GetFaultPc(siginfo, context);
  if (fault_pc == 0u) {
    VLOG(signals) << "no fault PC";
    return false;
  }

  // Walk over the list of registered code ranges.
  GeneratedCodeRange* range = generated_code_ranges_.load(std::memory_order_acquire);
  while (range != nullptr) {
    if (fault_pc - reinterpret_cast<uintptr_t>(range->start) < range->size) {
      return true;
    }
    // We may or may not see ranges that were concurrently removed, depending
    // on when the relaxed writes of the `next` links become visible. However,
    // even if we're currently at a node that is being removed, we shall visit
    // all remaining ranges that are not being removed as the removed nodes
    // retain the `next` link at the time of removal (which may lead to other
    // removed nodes before reaching remaining retained nodes, if any). Correct
    // memory visibility of `start` and `size` fields of the visited ranges is
    // ensured by the release and acquire operations on `generated_code_ranges_`.
    range = range->next.load(std::memory_order_relaxed);
  }
  return false;
}

FaultHandler::FaultHandler(FaultManager* manager) : manager_(manager) {
}

//
// Null pointer fault handler
//
NullPointerHandler::NullPointerHandler(FaultManager* manager) : FaultHandler(manager) {
  manager_->AddHandler(this, true);
}

bool NullPointerHandler::IsValidMethod(ArtMethod* method) {
  // At this point we know that the thread is `Runnable` and the PC is in one of
  // the registered code ranges. The `method` was read from the top of the stack
  // and should really point to an actual `ArtMethod`, unless we're crashing during
  // prologue or epilogue, or somehow managed to jump to the compiled code by some
  // unexpected path, other than method invoke or exception delivery. We do a few
  // quick checks without guarding from another fault.
  VLOG(signals) << "potential method: " << method;

  static_assert(IsAligned<sizeof(void*)>(ArtMethod::Size(kRuntimePointerSize)));
  if (method == nullptr || !IsAligned<sizeof(void*)>(method)) {
    VLOG(signals) << ((method == nullptr) ? "null method" : "unaligned method");
    return false;
  }

  // Check that the presumed method actually points to a class. Read barriers
  // are not needed (and would be undesirable in a signal handler) when reading
  // a chain of constant references to get to a non-movable `Class.class` object.

  // Note: Allowing nested faults. Checking that the method is in one of the
  // `LinearAlloc` spaces, or that objects we look at are in the `Heap` would be
  // slow and require locking a mutex, which is undesirable in a signal handler.
  // (Though we could register valid ranges similarly to the generated code ranges.)

  mirror::Object* klass =
      method->GetDeclaringClassAddressWithoutBarrier()->AsMirrorPtr();
  if (klass == nullptr || !IsAligned<kObjectAlignment>(klass)) {
    VLOG(signals) << ((klass == nullptr) ? "null class" : "unaligned class");
    return false;
  }

  mirror::Class* class_class = klass->GetClass<kVerifyNone, kWithoutReadBarrier>();
  if (class_class == nullptr || !IsAligned<kObjectAlignment>(class_class)) {
    VLOG(signals) << ((klass == nullptr) ? "null class_class" : "unaligned class_class");
    return false;
  }

  if (class_class != class_class->GetClass<kVerifyNone, kWithoutReadBarrier>()) {
    VLOG(signals) << "invalid class_class";
    return false;
  }

  return true;
}

bool NullPointerHandler::IsValidReturnPc(ArtMethod** sp, uintptr_t return_pc) {
  // Check if we can associate a dex PC with the return PC, whether from Nterp,
  // or with an existing stack map entry for a compiled method.
  // Note: Allowing nested faults if `IsValidMethod()` returned a false positive.
  // Note: The `ArtMethod::GetOatQuickMethodHeader()` can acquire locks (at least
  // `Locks::jit_lock_`) and if the thread already held such a lock, the signal
  // handler would deadlock. However, if a thread is holding one of the locks
  // below the mutator lock, the PC should be somewhere in ART code and should
  // not match any registered generated code range, so such as a deadlock is
  // unlikely. If it happens anyway, the worst case is that an internal ART crash
  // would be reported as ANR.
  ArtMethod* method = *sp;
  const OatQuickMethodHeader* method_header = method->GetOatQuickMethodHeader(return_pc);
  if (method_header == nullptr) {
    VLOG(signals) << "No method header.";
    return false;
  }
  VLOG(signals) << "looking for dex pc for return pc 0x" << std::hex << return_pc
                << " pc offset: 0x" << std::hex
                << (return_pc - reinterpret_cast<uintptr_t>(method_header->GetEntryPoint()));
  uint32_t dexpc = method_header->ToDexPc(reinterpret_cast<ArtMethod**>(sp), return_pc, false);
  VLOG(signals) << "dexpc: " << dexpc;
  return dexpc != dex::kDexNoIndex;
}

//
// Suspension fault handler
//
SuspensionHandler::SuspensionHandler(FaultManager* manager) : FaultHandler(manager) {
  manager_->AddHandler(this, true);
}

//
// Stack overflow fault handler
//
StackOverflowHandler::StackOverflowHandler(FaultManager* manager) : FaultHandler(manager) {
  manager_->AddHandler(this, true);
}

//
// Stack trace handler, used to help get a stack trace from SIGSEGV inside of compiled code.
//
JavaStackTraceHandler::JavaStackTraceHandler(FaultManager* manager) : FaultHandler(manager) {
  manager_->AddHandler(this, false);
}

bool JavaStackTraceHandler::Action(int sig ATTRIBUTE_UNUSED, siginfo_t* siginfo, void* context) {
  // Make sure that we are in the generated code, but we may not have a dex pc.
  bool in_generated_code = manager_->IsInGeneratedCode(siginfo, context);
  if (in_generated_code) {
    LOG(ERROR) << "Dumping java stack trace for crash in generated code";
    Thread* self = Thread::Current();

    uintptr_t sp = FaultManager::GetFaultSp(context);
    CHECK_NE(sp, 0u);  // Otherwise we should not have reached this handler.
    // Inside of generated code, sp[0] is the method, so sp is the frame.
    self->SetTopOfStack(reinterpret_cast<ArtMethod**>(sp));
    self->DumpJavaStack(LOG_STREAM(ERROR));
  }

  return false;  // Return false since we want to propagate the fault to the main signal handler.
}

}   // namespace art
