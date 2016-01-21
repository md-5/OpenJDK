/*
 * Copyright (c) 1999, 2015, Oracle and/or its affiliates. All rights reserved.
 * Copyright 2013, 2015 SAP AG. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#ifndef OS_AIX_VM_OS_AIX_HPP
#define OS_AIX_VM_OS_AIX_HPP

// Information about the protection of the page at address '0' on this os.
static bool zero_page_read_protected() { return false; }

// Class Aix defines the interface to the Aix operating systems.

class Aix {
  friend class os;

  static bool libjsig_is_loaded;        // libjsig that interposes sigaction(),
                                        // __sigaction(), signal() is loaded
  static struct sigaction *(*get_signal_action)(int);
  static struct sigaction *get_preinstalled_handler(int);
  static void save_preinstalled_handler(int, struct sigaction&);

  static void check_signal_handler(int sig);

 private:

  static julong _physical_memory;
  static pthread_t _main_thread;
  static Mutex* _createThread_lock;
  static int _page_size;

  // Page size of newly created pthreads.
  static int _stack_page_size;

  // -1 = uninitialized, 0 = AIX, 1 = OS/400 (PASE)
  static int _on_pase;

  // 0 = uninitialized, otherwise 16 bit number:
  //  lower 8 bit - minor version
  //  higher 8 bit - major version
  //  For AIX, e.g. 0x0601 for AIX 6.1
  //  for OS/400 e.g. 0x0504 for OS/400 V5R4
  static uint32_t _os_version;

  // -1 = uninitialized,
  //  0 - SPEC1170 not requested (XPG_SUS_ENV is OFF or not set)
  //  1 - SPEC1170 requested (XPG_SUS_ENV is ON)
  static int _xpg_sus_mode;

  // -1 = uninitialized,
  //  0 - EXTSHM=OFF or not set
  //  1 - EXTSHM=ON
  static int _extshm;

  static julong available_memory();
  static julong physical_memory() { return _physical_memory; }
  static void initialize_system_info();

  // OS recognitions (PASE/AIX, OS level) call this before calling any
  // one of Aix::on_pase(), Aix::os_version().
  static void initialize_os_info();

  // Scan environment for important settings which might effect the
  // VM. Trace out settings. Warn about invalid settings and/or
  // correct them.
  //
  // Must run after os::Aix::initialue_os_info().
  static void scan_environment();

  // Initialize libo4 (on PASE) and libperfstat (on AIX). Call this
  // before relying on functions from either lib, e.g. Aix::get_meminfo().
  static void initialize_libo4();
  static void initialize_libperfstat();

 public:
  static void init_thread_fpu_state();
  static pthread_t main_thread(void)                                { return _main_thread; }
  static void set_createThread_lock(Mutex* lk)                      { _createThread_lock = lk; }
  static Mutex* createThread_lock(void)                             { return _createThread_lock; }
  static void hotspot_sigmask(Thread* thread);

  // Given an address, returns the size of the page backing that address
  static size_t query_pagesize(void* p);

  // Return `true' if the calling thread is the primordial thread. The
  // primordial thread is the thread which contains the main function,
  // *not* necessarily the thread which initialized the VM by calling
  // JNI_CreateJavaVM.
  static bool is_primordial_thread(void);

  static int page_size(void) {
    assert(_page_size != -1, "not initialized");
    return _page_size;
  }

  // Accessor methods for stack page size which may be different from usual page size.
  static int stack_page_size(void) {
    assert(_stack_page_size != -1, "not initialized");
    return _stack_page_size;
  }

  // This is used to scale stack space (guard pages etc.). The name is somehow misleading.
  static int vm_default_page_size(void ) { return 8*K; }

  static address   ucontext_get_pc(const ucontext_t* uc);
  static intptr_t* ucontext_get_sp(const ucontext_t* uc);
  static intptr_t* ucontext_get_fp(const ucontext_t* uc);
  // Set PC into context. Needed for continuation after signal.
  static void ucontext_set_pc(ucontext_t* uc, address pc);

  // This boolean allows users to forward their own non-matching signals
  // to JVM_handle_aix_signal, harmlessly.
  static bool signal_handlers_are_installed;

  static int get_our_sigflags(int);
  static void set_our_sigflags(int, int);
  static void signal_sets_init();
  static void install_signal_handlers();
  static void set_signal_handler(int, bool);
  static bool is_sig_ignored(int sig);

  static sigset_t* unblocked_signals();
  static sigset_t* vm_signals();
  static sigset_t* allowdebug_blocked_signals();

  // For signal-chaining
  static struct sigaction *get_chained_signal_action(int sig);
  static bool chained_handler(int sig, siginfo_t* siginfo, void* context);

  // libpthread version string
  static void libpthread_init();

  // Minimum stack size a thread can be created with (allowing
  // the VM to completely create the thread and enter user code)
  static size_t min_stack_allowed;

  // Return default stack size or guard size for the specified thread type
  static size_t default_stack_size(os::ThreadType thr_type);
  static size_t default_guard_size(os::ThreadType thr_type);

  // Function returns true if we run on OS/400 (pase), false if we run
  // on AIX.
  static bool on_pase() {
    assert(_on_pase != -1, "not initialized");
    return _on_pase ? true : false;
  }

  // Function returns true if we run on AIX, false if we run on OS/400
  // (pase).
  static bool on_aix() {
    assert(_on_pase != -1, "not initialized");
    return _on_pase ? false : true;
  }

  // Get 4 byte AIX kernel version number:
  // highest 2 bytes: Version, Release
  // if available: lowest 2 bytes: Tech Level, Service Pack.
  static uint32_t os_version() {
    assert(_os_version != 0, "not initialized");
    return _os_version;
  }

  // 0 = uninitialized, otherwise 16 bit number:
  // lower 8 bit - minor version
  // higher 8 bit - major version
  // For AIX, e.g. 0x0601 for AIX 6.1
  // for OS/400 e.g. 0x0504 for OS/400 V5R4
  static int os_version_short() {
    return os_version() >> 16;
  }

  // Convenience method: returns true if running on PASE V5R4 or older.
  static bool on_pase_V5R4_or_older() {
    return on_pase() && os_version_short() <= 0x0504;
  }

  // Convenience method: returns true if running on AIX 5.3 or older.
  static bool on_aix_53_or_older() {
    return on_aix() && os_version_short() <= 0x0503;
  }

  // Returns true if we run in SPEC1170 compliant mode (XPG_SUS_ENV=ON).
  static bool xpg_sus_mode() {
    assert(_xpg_sus_mode != -1, "not initialized");
    return _xpg_sus_mode;
  }

  // Returns true if EXTSHM=ON.
  static bool extshm() {
    assert(_extshm != -1, "not initialized");
    return _extshm;
  }

  // result struct for get_meminfo()
  struct meminfo_t {

    // Amount of virtual memory (in units of 4 KB pages)
    unsigned long long virt_total;

    // Amount of real memory, in bytes
    unsigned long long real_total;

    // Amount of free real memory, in bytes
    unsigned long long real_free;

    // Total amount of paging space, in bytes
    unsigned long long pgsp_total;

    // Amount of free paging space, in bytes
    unsigned long long pgsp_free;

  };

  // Functions to retrieve memory information on AIX, PASE.
  // (on AIX, using libperfstat, on PASE with libo4.so).
  // Returns true if ok, false if error.
  static bool get_meminfo(meminfo_t* pmi);

};


class PlatformEvent : public CHeapObj<mtInternal> {
  private:
    double CachePad [4];   // increase odds that _mutex is sole occupant of cache line
    volatile int _Event;
    volatile int _nParked;
    pthread_mutex_t _mutex  [1];
    pthread_cond_t  _cond   [1];
    double PostPad  [2];
    Thread * _Assoc;

  public:       // TODO-FIXME: make dtor private
    ~PlatformEvent() { guarantee (0, "invariant"); }

  public:
    PlatformEvent() {
      int status;
      status = pthread_cond_init (_cond, NULL);
      assert_status(status == 0, status, "cond_init");
      status = pthread_mutex_init (_mutex, NULL);
      assert_status(status == 0, status, "mutex_init");
      _Event   = 0;
      _nParked = 0;
      _Assoc   = NULL;
    }

    // Use caution with reset() and fired() -- they may require MEMBARs
    void reset() { _Event = 0; }
    int  fired() { return _Event; }
    void park ();
    void unpark ();
    int  TryPark ();
    int  park (jlong millis);
    void SetAssociation (Thread * a) { _Assoc = a; }
};

class PlatformParker : public CHeapObj<mtInternal> {
  protected:
    pthread_mutex_t _mutex [1];
    pthread_cond_t  _cond  [1];

  public:       // TODO-FIXME: make dtor private
    ~PlatformParker() { guarantee (0, "invariant"); }

  public:
    PlatformParker() {
      int status;
      status = pthread_cond_init (_cond, NULL);
      assert_status(status == 0, status, "cond_init");
      status = pthread_mutex_init (_mutex, NULL);
      assert_status(status == 0, status, "mutex_init");
    }
};

#endif // OS_AIX_VM_OS_AIX_HPP
