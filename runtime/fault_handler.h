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


#ifndef ART_RUNTIME_FAULT_HANDLER_H_
#define ART_RUNTIME_FAULT_HANDLER_H_

#include <signal.h>
#include <stdint.h>

#include <vector>

#include "base/locks.h"  // For annotalysis.
#include "runtime_globals.h"  // For CanDoImplicitNullCheckOn.

namespace art {

class ArtMethod;
class FaultHandler;

namespace gc {
class Heap;
}  // namespace gc

class FaultManager {
 public:
  FaultManager();
  ~FaultManager();

  // Use libsigchain if use_sig_chain is true. Otherwise, setup SIGBUS directly
  // using sigaction().
  void Init(bool use_sig_chain);

  // Unclaim signals.
  void Release();

  // Unclaim signals and delete registered handlers.
  void Shutdown();

  // Try to handle a SIGSEGV fault, returns true if successful.
  bool HandleSigsegvFault(int sig, siginfo_t* info, void* context);

  // Try to handle a SIGBUS fault, returns true if successful.
  bool HandleSigbusFault(int sig, siginfo_t* info, void* context);

  // Added handlers are owned by the fault handler and will be freed on Shutdown().
  void AddHandler(FaultHandler* handler, bool generated_code);
  void RemoveHandler(FaultHandler* handler);

  // Note that the following two functions are called in the context of a signal handler.
  // The IsInGeneratedCode() function checks that the mutator lock is held before it
  // calls GetMethodAndReturnPCAndSP().
  // TODO: think about adding lock assertions and fake lock and unlock functions.
  void GetMethodAndReturnPcAndSp(siginfo_t* siginfo,
                                 void* context,
                                 ArtMethod** out_method,
                                 uintptr_t* out_return_pc,
                                 uintptr_t* out_sp,
                                 bool* out_is_stack_overflow)
                                 NO_THREAD_SAFETY_ANALYSIS;
  bool IsInGeneratedCode(siginfo_t* siginfo, void *context, bool check_dex_pc)
                         NO_THREAD_SAFETY_ANALYSIS;

 private:
  // The HandleFaultByOtherHandlers function is only called by HandleFault function for generated code.
  bool HandleFaultByOtherHandlers(int sig, siginfo_t* info, void* context)
                                  NO_THREAD_SAFETY_ANALYSIS;

  std::vector<FaultHandler*> generated_code_handlers_;
  std::vector<FaultHandler*> other_handlers_;
  bool initialized_;
  DISALLOW_COPY_AND_ASSIGN(FaultManager);
};

class FaultHandler {
 public:
  explicit FaultHandler(FaultManager* manager);
  virtual ~FaultHandler() {}
  FaultManager* GetFaultManager() {
    return manager_;
  }

  virtual bool Action(int sig, siginfo_t* siginfo, void* context) = 0;

 protected:
  FaultManager* const manager_;

 private:
  DISALLOW_COPY_AND_ASSIGN(FaultHandler);
};

class NullPointerHandler final : public FaultHandler {
 public:
  explicit NullPointerHandler(FaultManager* manager);

  bool Action(int sig, siginfo_t* siginfo, void* context) override;

  static bool IsValidImplicitCheck(siginfo_t* siginfo) {
    // Our implicit NPE checks always limit the range to a page.
    // Note that the runtime will do more exhaustive checks (that we cannot
    // reasonably do in signal processing code) based on the dex instruction
    // faulting.
    return CanDoImplicitNullCheckOn(reinterpret_cast<uintptr_t>(siginfo->si_addr));
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(NullPointerHandler);
};

class SuspensionHandler final : public FaultHandler {
 public:
  explicit SuspensionHandler(FaultManager* manager);

  bool Action(int sig, siginfo_t* siginfo, void* context) override;

 private:
  DISALLOW_COPY_AND_ASSIGN(SuspensionHandler);
};

class StackOverflowHandler final : public FaultHandler {
 public:
  explicit StackOverflowHandler(FaultManager* manager);

  bool Action(int sig, siginfo_t* siginfo, void* context) override;

 private:
  DISALLOW_COPY_AND_ASSIGN(StackOverflowHandler);
};

class JavaStackTraceHandler final : public FaultHandler {
 public:
  explicit JavaStackTraceHandler(FaultManager* manager);

  bool Action(int sig, siginfo_t* siginfo, void* context) override NO_THREAD_SAFETY_ANALYSIS;

 private:
  DISALLOW_COPY_AND_ASSIGN(JavaStackTraceHandler);
};

// Statically allocated so the the signal handler can Get access to it.
extern FaultManager fault_manager;

}       // namespace art
#endif  // ART_RUNTIME_FAULT_HANDLER_H_

