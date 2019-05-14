/*
 * Copyright (c) 2011, 2019, Oracle and/or its affiliates. All rights reserved.
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
 */

#include "precompiled.hpp"
#include "classfile/javaClasses.inline.hpp"
#include "classfile/stringTable.hpp"
#include "classfile/symbolTable.hpp"
#include "code/scopeDesc.hpp"
#include "compiler/compileBroker.hpp"
#include "compiler/disassembler.hpp"
#include "interpreter/linkResolver.hpp"
#include "interpreter/bytecodeStream.hpp"
#include "jvmci/jvmciCompilerToVM.hpp"
#include "jvmci/jvmciCodeInstaller.hpp"
#include "jvmci/jvmciRuntime.hpp"
#include "memory/oopFactory.hpp"
#include "memory/universe.hpp"
#include "oops/constantPool.inline.hpp"
#include "oops/method.inline.hpp"
#include "oops/typeArrayOop.inline.hpp"
#include "prims/nativeLookup.hpp"
#include "runtime/deoptimization.hpp"
#include "runtime/fieldDescriptor.inline.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/jniHandles.inline.hpp"
#include "runtime/timerTrace.hpp"
#include "runtime/vframe_hp.hpp"

JVMCIKlassHandle::JVMCIKlassHandle(Thread* thread, Klass* klass) {
  _thread = thread;
  _klass = klass;
  if (klass != NULL) {
    _holder = Handle(_thread, klass->klass_holder());
  }
}

JVMCIKlassHandle& JVMCIKlassHandle::operator=(Klass* klass) {
  _klass = klass;
  if (klass != NULL) {
    _holder = Handle(_thread, klass->klass_holder());
  }
  return *this;
}

static void requireInHotSpot(const char* caller, JVMCI_TRAPS) {
  if (!JVMCIENV->is_hotspot()) {
    JVMCI_THROW_MSG(IllegalStateException, err_msg("Cannot call %s from JVMCI shared library", caller));
  }
}

void JNIHandleMark::push_jni_handle_block(JavaThread* thread) {
  if (thread != NULL) {
    // Allocate a new block for JNI handles.
    // Inlined code from jni_PushLocalFrame()
    JNIHandleBlock* java_handles = thread->active_handles();
    JNIHandleBlock* compile_handles = JNIHandleBlock::allocate_block(thread);
    assert(compile_handles != NULL && java_handles != NULL, "should not be NULL");
    compile_handles->set_pop_frame_link(java_handles);
    thread->set_active_handles(compile_handles);
  }
}

void JNIHandleMark::pop_jni_handle_block(JavaThread* thread) {
  if (thread != NULL) {
    // Release our JNI handle block
    JNIHandleBlock* compile_handles = thread->active_handles();
    JNIHandleBlock* java_handles = compile_handles->pop_frame_link();
    thread->set_active_handles(java_handles);
    compile_handles->set_pop_frame_link(NULL);
    JNIHandleBlock::release_block(compile_handles, thread); // may block
  }
}

class JVMCITraceMark : public StackObj {
  const char* _msg;
 public:
  JVMCITraceMark(const char* msg) {
    _msg = msg;
    if (JVMCITraceLevel >= 1) {
      tty->print_cr(PTR_FORMAT " JVMCITrace-1: Enter %s", p2i(JavaThread::current()), _msg);
    }
  }
  ~JVMCITraceMark() {
    if (JVMCITraceLevel >= 1) {
      tty->print_cr(PTR_FORMAT " JVMCITrace-1: Exit %s", p2i(JavaThread::current()), _msg);
    }
  }
};


Handle JavaArgumentUnboxer::next_arg(BasicType expectedType) {
  assert(_index < _args->length(), "out of bounds");
  oop arg=((objArrayOop) (_args))->obj_at(_index++);
  assert(expectedType == T_OBJECT || java_lang_boxing_object::is_instance(arg, expectedType), "arg type mismatch");
  return Handle(Thread::current(), arg);
}

// Bring the JVMCI compiler thread into the VM state.
#define JVMCI_VM_ENTRY_MARK                   \
  ThreadInVMfromNative __tiv(thread);         \
  ResetNoHandleMark rnhm;                     \
  HandleMarkCleaner __hm(thread);             \
  Thread* THREAD = thread;                    \
  debug_only(VMNativeEntryWrapper __vew;)

// Native method block that transitions current thread to '_thread_in_vm'.
#define C2V_BLOCK(result_type, name, signature)      \
  TRACE_CALL(result_type, jvmci_ ## name signature)  \
  JVMCI_VM_ENTRY_MARK;                               \
  ResourceMark rm;                                   \
  JNI_JVMCIENV(thread, env);

static Thread* get_current_thread() {
  return Thread::current_or_null_safe();
}

// Entry to native method implementation that transitions
// current thread to '_thread_in_vm'.
#define C2V_VMENTRY(result_type, name, signature)        \
  JNIEXPORT result_type JNICALL c2v_ ## name signature { \
  Thread* base_thread = get_current_thread();            \
  if (base_thread == NULL) {                             \
    env->ThrowNew(JNIJVMCI::InternalError::clazz(),      \
        err_msg("Cannot call into HotSpot from JVMCI shared library without attaching current thread")); \
    return;                                              \
  }                                                      \
  assert(base_thread->is_Java_thread(), "just checking");\
  JavaThread* thread = (JavaThread*) base_thread;        \
  JVMCITraceMark jtm("CompilerToVM::" #name);            \
  C2V_BLOCK(result_type, name, signature)

#define C2V_VMENTRY_(result_type, name, signature, result) \
  JNIEXPORT result_type JNICALL c2v_ ## name signature { \
  Thread* base_thread = get_current_thread();            \
  if (base_thread == NULL) {                             \
    env->ThrowNew(JNIJVMCI::InternalError::clazz(),      \
        err_msg("Cannot call into HotSpot from JVMCI shared library without attaching current thread")); \
    return result;                                       \
  }                                                      \
  assert(base_thread->is_Java_thread(), "just checking");\
  JavaThread* thread = (JavaThread*) base_thread;        \
  JVMCITraceMark jtm("CompilerToVM::" #name);            \
  C2V_BLOCK(result_type, name, signature)

#define C2V_VMENTRY_NULL(result_type, name, signature) C2V_VMENTRY_(result_type, name, signature, NULL)
#define C2V_VMENTRY_0(result_type, name, signature) C2V_VMENTRY_(result_type, name, signature, 0)

// Entry to native method implementation that does not transition
// current thread to '_thread_in_vm'.
#define C2V_VMENTRY_PREFIX(result_type, name, signature) \
  JNIEXPORT result_type JNICALL c2v_ ## name signature { \
  Thread* base_thread = get_current_thread();

#define C2V_END }

#define JNI_THROW(caller, name, msg) do {                                         \
    jint __throw_res = env->ThrowNew(JNIJVMCI::name::clazz(), msg);               \
    if (__throw_res != JNI_OK) {                                                  \
      tty->print_cr("Throwing " #name " in " caller " returned %d", __throw_res); \
    }                                                                             \
    return;                                                                       \
  } while (0);

#define JNI_THROW_(caller, name, msg, result) do {                                \
    jint __throw_res = env->ThrowNew(JNIJVMCI::name::clazz(), msg);               \
    if (__throw_res != JNI_OK) {                                                  \
      tty->print_cr("Throwing " #name " in " caller " returned %d", __throw_res); \
    }                                                                             \
    return result;                                                                \
  } while (0)

jobjectArray readConfiguration0(JNIEnv *env, JVMCI_TRAPS);

C2V_VMENTRY_NULL(jobjectArray, readConfiguration, (JNIEnv* env))
  jobjectArray config = readConfiguration0(env, JVMCI_CHECK_NULL);
  return config;
}

C2V_VMENTRY_NULL(jobject, getFlagValue, (JNIEnv* env, jobject c2vm, jobject name_handle))
#define RETURN_BOXED_LONG(value) jvalue p; p.j = (jlong) (value); JVMCIObject box = JVMCIENV->create_box(T_LONG, &p, JVMCI_CHECK_NULL); return box.as_jobject();
#define RETURN_BOXED_DOUBLE(value) jvalue p; p.d = (jdouble) (value); JVMCIObject box = JVMCIENV->create_box(T_DOUBLE, &p, JVMCI_CHECK_NULL); return box.as_jobject();
  JVMCIObject name = JVMCIENV->wrap(name_handle);
  if (name.is_null()) {
    JVMCI_THROW_NULL(NullPointerException);
  }
  const char* cstring = JVMCIENV->as_utf8_string(name);
  JVMFlag* flag = JVMFlag::find_flag(cstring, strlen(cstring), /* allow_locked */ true, /* return_flag */ true);
  if (flag == NULL) {
    return c2vm;
  }
  if (flag->is_bool()) {
    jvalue prim;
    prim.z = flag->get_bool();
    JVMCIObject box = JVMCIENV->create_box(T_BOOLEAN, &prim, JVMCI_CHECK_NULL);
    return JVMCIENV->get_jobject(box);
  } else if (flag->is_ccstr()) {
    JVMCIObject value = JVMCIENV->create_string(flag->get_ccstr(), JVMCI_CHECK_NULL);
    return JVMCIENV->get_jobject(value);
  } else if (flag->is_intx()) {
    RETURN_BOXED_LONG(flag->get_intx());
  } else if (flag->is_int()) {
    RETURN_BOXED_LONG(flag->get_int());
  } else if (flag->is_uint()) {
    RETURN_BOXED_LONG(flag->get_uint());
  } else if (flag->is_uint64_t()) {
    RETURN_BOXED_LONG(flag->get_uint64_t());
  } else if (flag->is_size_t()) {
    RETURN_BOXED_LONG(flag->get_size_t());
  } else if (flag->is_uintx()) {
    RETURN_BOXED_LONG(flag->get_uintx());
  } else if (flag->is_double()) {
    RETURN_BOXED_DOUBLE(flag->get_double());
  } else {
    JVMCI_ERROR_NULL("VM flag %s has unsupported type %s", flag->_name, flag->_type);
  }
#undef RETURN_BOXED_LONG
#undef RETURN_BOXED_DOUBLE
C2V_END

C2V_VMENTRY_NULL(jobject, getObjectAtAddress, (JNIEnv* env, jobject c2vm, jlong oop_address))
  requireInHotSpot("getObjectAtAddress", JVMCI_CHECK_NULL);
  if (oop_address == 0) {
    JVMCI_THROW_MSG_NULL(InternalError, "Handle must be non-zero");
  }
  oop obj = *((oopDesc**) oop_address);
  if (obj != NULL) {
    oopDesc::verify(obj);
  }
  return JNIHandles::make_local(obj);
C2V_END

C2V_VMENTRY_NULL(jbyteArray, getBytecode, (JNIEnv* env, jobject, jobject jvmci_method))
  methodHandle method = JVMCIENV->asMethod(jvmci_method);

  int code_size = method->code_size();
  jbyte* reconstituted_code = NEW_RESOURCE_ARRAY(jbyte, code_size);

  guarantee(method->method_holder()->is_rewritten(), "Method's holder should be rewritten");
  // iterate over all bytecodes and replace non-Java bytecodes

  for (BytecodeStream s(method); s.next() != Bytecodes::_illegal; ) {
    Bytecodes::Code code = s.code();
    Bytecodes::Code raw_code = s.raw_code();
    int bci = s.bci();
    int len = s.instruction_size();

    // Restore original byte code.
    reconstituted_code[bci] =  (jbyte) (s.is_wide()? Bytecodes::_wide : code);
    if (len > 1) {
      memcpy(reconstituted_code + (bci + 1), s.bcp()+1, len-1);
    }

    if (len > 1) {
      // Restore the big-endian constant pool indexes.
      // Cf. Rewriter::scan_method
      switch (code) {
        case Bytecodes::_getstatic:
        case Bytecodes::_putstatic:
        case Bytecodes::_getfield:
        case Bytecodes::_putfield:
        case Bytecodes::_invokevirtual:
        case Bytecodes::_invokespecial:
        case Bytecodes::_invokestatic:
        case Bytecodes::_invokeinterface:
        case Bytecodes::_invokehandle: {
          int cp_index = Bytes::get_native_u2((address) reconstituted_code + (bci + 1));
          Bytes::put_Java_u2((address) reconstituted_code + (bci + 1), (u2) cp_index);
          break;
        }

        case Bytecodes::_invokedynamic: {
          int cp_index = Bytes::get_native_u4((address) reconstituted_code + (bci + 1));
          Bytes::put_Java_u4((address) reconstituted_code + (bci + 1), (u4) cp_index);
          break;
        }

        default:
          break;
      }

      // Not all ldc byte code are rewritten.
      switch (raw_code) {
        case Bytecodes::_fast_aldc: {
          int cpc_index = reconstituted_code[bci + 1] & 0xff;
          int cp_index = method->constants()->object_to_cp_index(cpc_index);
          assert(cp_index < method->constants()->length(), "sanity check");
          reconstituted_code[bci + 1] = (jbyte) cp_index;
          break;
        }

        case Bytecodes::_fast_aldc_w: {
          int cpc_index = Bytes::get_native_u2((address) reconstituted_code + (bci + 1));
          int cp_index = method->constants()->object_to_cp_index(cpc_index);
          assert(cp_index < method->constants()->length(), "sanity check");
          Bytes::put_Java_u2((address) reconstituted_code + (bci + 1), (u2) cp_index);
          break;
        }

        default:
          break;
      }
    }
  }

  JVMCIPrimitiveArray result = JVMCIENV->new_byteArray(code_size, JVMCI_CHECK_NULL);
  JVMCIENV->copy_bytes_from(reconstituted_code, result, 0, code_size);
  return JVMCIENV->get_jbyteArray(result);
C2V_END

C2V_VMENTRY_0(jint, getExceptionTableLength, (JNIEnv* env, jobject, jobject jvmci_method))
  methodHandle method = JVMCIENV->asMethod(jvmci_method);
  return method->exception_table_length();
C2V_END

C2V_VMENTRY_0(jlong, getExceptionTableStart, (JNIEnv* env, jobject, jobject jvmci_method))
  methodHandle method = JVMCIENV->asMethod(jvmci_method);
  if (method->exception_table_length() == 0) {
    return 0L;
  }
  return (jlong) (address) method->exception_table_start();
C2V_END

C2V_VMENTRY_NULL(jobject, asResolvedJavaMethod, (JNIEnv* env, jobject, jobject executable_handle))
  requireInHotSpot("asResolvedJavaMethod", JVMCI_CHECK_NULL);
  oop executable = JNIHandles::resolve(executable_handle);
  oop mirror = NULL;
  int slot = 0;

  if (executable->klass() == SystemDictionary::reflect_Constructor_klass()) {
    mirror = java_lang_reflect_Constructor::clazz(executable);
    slot = java_lang_reflect_Constructor::slot(executable);
  } else {
    assert(executable->klass() == SystemDictionary::reflect_Method_klass(), "wrong type");
    mirror = java_lang_reflect_Method::clazz(executable);
    slot = java_lang_reflect_Method::slot(executable);
  }
  Klass* holder = java_lang_Class::as_Klass(mirror);
  methodHandle method = InstanceKlass::cast(holder)->method_with_idnum(slot);
  JVMCIObject result = JVMCIENV->get_jvmci_method(method, JVMCI_CHECK_NULL);
  return JVMCIENV->get_jobject(result);
}

C2V_VMENTRY_NULL(jobject, getResolvedJavaMethod, (JNIEnv* env, jobject, jobject base, jlong offset))
  methodHandle method;
  JVMCIObject base_object = JVMCIENV->wrap(base);
  if (base_object.is_null()) {
    method = *((Method**)(offset));
  } else if (JVMCIENV->isa_HotSpotObjectConstantImpl(base_object)) {
    Handle obj = JVMCIENV->asConstant(base_object, JVMCI_CHECK_NULL);
    if (obj->is_a(SystemDictionary::ResolvedMethodName_klass())) {
      method = (Method*) (intptr_t) obj->long_field(offset);
    } else {
      JVMCI_THROW_MSG_NULL(IllegalArgumentException, err_msg("Unexpected type: %s", obj->klass()->external_name()));
    }
  } else if (JVMCIENV->isa_HotSpotResolvedJavaMethodImpl(base_object)) {
    method = JVMCIENV->asMethod(base_object);
  }
  if (method.is_null()) {
    JVMCI_THROW_MSG_NULL(IllegalArgumentException, err_msg("Unexpected type: %s", JVMCIENV->klass_name(base_object)));
  }
  assert (method.is_null() || method->is_method(), "invalid read");
  JVMCIObject result = JVMCIENV->get_jvmci_method(method, JVMCI_CHECK_NULL);
  return JVMCIENV->get_jobject(result);
}

C2V_VMENTRY_NULL(jobject, getConstantPool, (JNIEnv* env, jobject, jobject object_handle))
  constantPoolHandle cp;
  JVMCIObject object = JVMCIENV->wrap(object_handle);
  if (object.is_null()) {
    JVMCI_THROW_NULL(NullPointerException);
  }
  if (JVMCIENV->isa_HotSpotResolvedJavaMethodImpl(object)) {
    cp = JVMCIENV->asMethod(object)->constMethod()->constants();
  } else if (JVMCIENV->isa_HotSpotResolvedObjectTypeImpl(object)) {
    cp = InstanceKlass::cast(JVMCIENV->asKlass(object))->constants();
  } else {
    JVMCI_THROW_MSG_NULL(IllegalArgumentException,
                err_msg("Unexpected type: %s", JVMCIENV->klass_name(object)));
  }
  assert(!cp.is_null(), "npe");

  JVMCIObject result = JVMCIENV->get_jvmci_constant_pool(cp, JVMCI_CHECK_NULL);
  return JVMCIENV->get_jobject(result);
}

C2V_VMENTRY_NULL(jobject, getResolvedJavaType0, (JNIEnv* env, jobject, jobject base, jlong offset, jboolean compressed))
  JVMCIKlassHandle klass(THREAD);
  JVMCIObject base_object = JVMCIENV->wrap(base);
  jlong base_address = 0;
  if (base_object.is_non_null() && offset == oopDesc::klass_offset_in_bytes()) {
    // klass = JVMCIENV->unhandle(base_object)->klass();
    if (JVMCIENV->isa_HotSpotObjectConstantImpl(base_object)) {
      Handle base_oop = JVMCIENV->asConstant(base_object, JVMCI_CHECK_NULL);
      klass = base_oop->klass();
    } else {
      assert(false, "What types are we actually expecting here?");
    }
  } else if (!compressed) {
    if (base_object.is_non_null()) {
      if (JVMCIENV->isa_HotSpotResolvedJavaMethodImpl(base_object)) {
        base_address = (intptr_t) JVMCIENV->asMethod(base_object);
      } else if (JVMCIENV->isa_HotSpotConstantPool(base_object)) {
        base_address = (intptr_t) JVMCIENV->asConstantPool(base_object);
      } else if (JVMCIENV->isa_HotSpotResolvedObjectTypeImpl(base_object)) {
        base_address = (intptr_t) JVMCIENV->asKlass(base_object);
      } else if (JVMCIENV->isa_HotSpotObjectConstantImpl(base_object)) {
        Handle base_oop = JVMCIENV->asConstant(base_object, JVMCI_CHECK_NULL);
        if (base_oop->is_a(SystemDictionary::Class_klass())) {
          base_address = (jlong) (address) base_oop();
        }
      }
      if (base_address == 0) {
        JVMCI_THROW_MSG_NULL(IllegalArgumentException,
                    err_msg("Unexpected arguments: %s " JLONG_FORMAT " %s", JVMCIENV->klass_name(base_object), offset, compressed ? "true" : "false"));
      }
    }
    klass = *((Klass**) (intptr_t) (base_address + offset));
  } else {
    JVMCI_THROW_MSG_NULL(IllegalArgumentException,
                err_msg("Unexpected arguments: %s " JLONG_FORMAT " %s",
                        base_object.is_non_null() ? JVMCIENV->klass_name(base_object) : "null",
                        offset, compressed ? "true" : "false"));
  }
  assert (klass == NULL || klass->is_klass(), "invalid read");
  JVMCIObject result = JVMCIENV->get_jvmci_type(klass, JVMCI_CHECK_NULL);
  return JVMCIENV->get_jobject(result);
}

C2V_VMENTRY_NULL(jobject, findUniqueConcreteMethod, (JNIEnv* env, jobject, jobject jvmci_type, jobject jvmci_method))
  methodHandle method = JVMCIENV->asMethod(jvmci_method);
  Klass* holder = JVMCIENV->asKlass(jvmci_type);
  if (holder->is_interface()) {
    JVMCI_THROW_MSG_NULL(InternalError, err_msg("Interface %s should be handled in Java code", holder->external_name()));
  }

  methodHandle ucm;
  {
    MutexLocker locker(Compile_lock);
    ucm = Dependencies::find_unique_concrete_method(holder, method());
  }
  JVMCIObject result = JVMCIENV->get_jvmci_method(ucm, JVMCI_CHECK_NULL);
  return JVMCIENV->get_jobject(result);
C2V_END

C2V_VMENTRY_NULL(jobject, getImplementor, (JNIEnv* env, jobject, jobject jvmci_type))
  Klass* klass = JVMCIENV->asKlass(jvmci_type);
  if (!klass->is_interface()) {
    THROW_MSG_0(vmSymbols::java_lang_IllegalArgumentException(),
        err_msg("Expected interface type, got %s", klass->external_name()));
  }
  InstanceKlass* iklass = InstanceKlass::cast(klass);
  JVMCIKlassHandle handle(THREAD);
  {
    // Need Compile_lock around implementor()
    MutexLocker locker(Compile_lock);
    handle = iklass->implementor();
  }
  JVMCIObject implementor = JVMCIENV->get_jvmci_type(handle, JVMCI_CHECK_NULL);
  return JVMCIENV->get_jobject(implementor);
C2V_END

C2V_VMENTRY_0(jboolean, methodIsIgnoredBySecurityStackWalk,(JNIEnv* env, jobject, jobject jvmci_method))
  methodHandle method = JVMCIENV->asMethod(jvmci_method);
  return method->is_ignored_by_security_stack_walk();
C2V_END

C2V_VMENTRY_0(jboolean, isCompilable,(JNIEnv* env, jobject, jobject jvmci_method))
  methodHandle method = JVMCIENV->asMethod(jvmci_method);
  constantPoolHandle cp = method->constMethod()->constants();
  assert(!cp.is_null(), "npe");
  // don't inline method when constant pool contains a CONSTANT_Dynamic
  return !method->is_not_compilable(CompLevel_full_optimization) && !cp->has_dynamic_constant();
C2V_END

C2V_VMENTRY_0(jboolean, hasNeverInlineDirective,(JNIEnv* env, jobject, jobject jvmci_method))
  methodHandle method = JVMCIENV->asMethod(jvmci_method);
  return !Inline || CompilerOracle::should_not_inline(method) || method->dont_inline();
C2V_END

C2V_VMENTRY_0(jboolean, shouldInlineMethod,(JNIEnv* env, jobject, jobject jvmci_method))
  methodHandle method = JVMCIENV->asMethod(jvmci_method);
  return CompilerOracle::should_inline(method) || method->force_inline();
C2V_END

C2V_VMENTRY_NULL(jobject, lookupType, (JNIEnv* env, jobject, jstring jname, jclass accessing_class, jboolean resolve))
  JVMCIObject name = JVMCIENV->wrap(jname);
  const char* str = JVMCIENV->as_utf8_string(name);
  TempNewSymbol class_name = SymbolTable::new_symbol(str);

  if (class_name->utf8_length() <= 1) {
    JVMCI_THROW_MSG_0(InternalError, err_msg("Primitive type %s should be handled in Java code", class_name->as_C_string()));
  }

  JVMCIKlassHandle resolved_klass(THREAD);
  Klass* accessing_klass = NULL;
  Handle class_loader;
  Handle protection_domain;
  if (accessing_class != NULL) {
    accessing_klass = JVMCIENV->asKlass(accessing_class);
    class_loader = Handle(THREAD, accessing_klass->class_loader());
    protection_domain = Handle(THREAD, accessing_klass->protection_domain());
  } else {
    // Use the System class loader
    class_loader = Handle(THREAD, SystemDictionary::java_system_loader());
    JVMCIENV->runtime()->initialize(JVMCIENV);
  }

  if (resolve) {
    resolved_klass = SystemDictionary::resolve_or_null(class_name, class_loader, protection_domain, CHECK_0);
    if (resolved_klass == NULL) {
      JVMCI_THROW_MSG_NULL(ClassNotFoundException, str);
    }
  } else {
    if (class_name->char_at(0) == 'L' &&
      class_name->char_at(class_name->utf8_length()-1) == ';') {
      // This is a name from a signature.  Strip off the trimmings.
      // Call recursive to keep scope of strippedsym.
      TempNewSymbol strippedsym = SymbolTable::new_symbol(class_name->as_utf8()+1,
                                                          class_name->utf8_length()-2);
      resolved_klass = SystemDictionary::find(strippedsym, class_loader, protection_domain, CHECK_0);
    } else if (FieldType::is_array(class_name)) {
      FieldArrayInfo fd;
      // dimension and object_key in FieldArrayInfo are assigned as a side-effect
      // of this call
      BasicType t = FieldType::get_array_info(class_name, fd, CHECK_0);
      if (t == T_OBJECT) {
        TempNewSymbol strippedsym = SymbolTable::new_symbol(class_name->as_utf8()+1+fd.dimension(),
                                                            class_name->utf8_length()-2-fd.dimension());
        resolved_klass = SystemDictionary::find(strippedsym,
                                                             class_loader,
                                                             protection_domain,
                                                             CHECK_0);
        if (!resolved_klass.is_null()) {
          resolved_klass = resolved_klass->array_klass(fd.dimension(), CHECK_0);
        }
      } else {
        resolved_klass = TypeArrayKlass::cast(Universe::typeArrayKlassObj(t))->array_klass(fd.dimension(), CHECK_0);
      }
    } else {
      resolved_klass = SystemDictionary::find(class_name, class_loader, protection_domain, CHECK_0);
    }
  }
  JVMCIObject result = JVMCIENV->get_jvmci_type(resolved_klass, JVMCI_CHECK_NULL);
  return JVMCIENV->get_jobject(result);
C2V_END

C2V_VMENTRY_NULL(jobject, lookupClass, (JNIEnv* env, jobject, jclass mirror))
  requireInHotSpot("lookupClass", JVMCI_CHECK_NULL);
  if (mirror == NULL) {
    return NULL;
  }
  JVMCIKlassHandle klass(THREAD);
  klass = java_lang_Class::as_Klass(JNIHandles::resolve(mirror));
  if (klass == NULL) {
    JVMCI_THROW_MSG_NULL(IllegalArgumentException, "Primitive classes are unsupported");
  }
  JVMCIObject result = JVMCIENV->get_jvmci_type(klass, JVMCI_CHECK_NULL);
  return JVMCIENV->get_jobject(result);
}

C2V_VMENTRY_NULL(jobject, resolveConstantInPool, (JNIEnv* env, jobject, jobject jvmci_constant_pool, jint index))
  constantPoolHandle cp = JVMCIENV->asConstantPool(jvmci_constant_pool);
  oop result = cp->resolve_constant_at(index, CHECK_NULL);
  return JVMCIENV->get_jobject(JVMCIENV->get_object_constant(result));
C2V_END

C2V_VMENTRY_NULL(jobject, resolvePossiblyCachedConstantInPool, (JNIEnv* env, jobject, jobject jvmci_constant_pool, jint index))
  constantPoolHandle cp = JVMCIENV->asConstantPool(jvmci_constant_pool);
  oop result = cp->resolve_possibly_cached_constant_at(index, CHECK_NULL);
  return JVMCIENV->get_jobject(JVMCIENV->get_object_constant(result));
C2V_END

C2V_VMENTRY_0(jint, lookupNameAndTypeRefIndexInPool, (JNIEnv* env, jobject, jobject jvmci_constant_pool, jint index))
  constantPoolHandle cp = JVMCIENV->asConstantPool(jvmci_constant_pool);
  return cp->name_and_type_ref_index_at(index);
C2V_END

C2V_VMENTRY_NULL(jobject, lookupNameInPool, (JNIEnv* env, jobject, jobject jvmci_constant_pool, jint which))
  constantPoolHandle cp = JVMCIENV->asConstantPool(jvmci_constant_pool);
  JVMCIObject sym = JVMCIENV->create_string(cp->name_ref_at(which), JVMCI_CHECK_NULL);
  return JVMCIENV->get_jobject(sym);
C2V_END

C2V_VMENTRY_NULL(jobject, lookupSignatureInPool, (JNIEnv* env, jobject, jobject jvmci_constant_pool, jint which))
  constantPoolHandle cp = JVMCIENV->asConstantPool(jvmci_constant_pool);
  JVMCIObject sym = JVMCIENV->create_string(cp->signature_ref_at(which), JVMCI_CHECK_NULL);
  return JVMCIENV->get_jobject(sym);
C2V_END

C2V_VMENTRY_0(jint, lookupKlassRefIndexInPool, (JNIEnv* env, jobject, jobject jvmci_constant_pool, jint index))
  constantPoolHandle cp = JVMCIENV->asConstantPool(jvmci_constant_pool);
  return cp->klass_ref_index_at(index);
C2V_END

C2V_VMENTRY_NULL(jobject, resolveTypeInPool, (JNIEnv* env, jobject, jobject jvmci_constant_pool, jint index))
  constantPoolHandle cp = JVMCIENV->asConstantPool(jvmci_constant_pool);
  Klass* klass = cp->klass_at(index, CHECK_NULL);
  JVMCIKlassHandle resolved_klass(THREAD, klass);
  if (resolved_klass->is_instance_klass()) {
    InstanceKlass::cast(resolved_klass())->link_class(CHECK_NULL);
    if (!InstanceKlass::cast(resolved_klass())->is_linked()) {
      // link_class() should not return here if there is an issue.
      JVMCI_THROW_MSG_NULL(InternalError, err_msg("Class %s must be linked", resolved_klass()->external_name()));
    }
  }
  JVMCIObject klassObject = JVMCIENV->get_jvmci_type(resolved_klass, JVMCI_CHECK_NULL);
  return JVMCIENV->get_jobject(klassObject);
C2V_END

C2V_VMENTRY_NULL(jobject, lookupKlassInPool, (JNIEnv* env, jobject, jobject jvmci_constant_pool, jint index, jbyte opcode))
  constantPoolHandle cp = JVMCIENV->asConstantPool(jvmci_constant_pool);
  Klass* loading_klass = cp->pool_holder();
  bool is_accessible = false;
  JVMCIKlassHandle klass(THREAD, JVMCIRuntime::get_klass_by_index(cp, index, is_accessible, loading_klass));
  Symbol* symbol = NULL;
  if (klass.is_null()) {
    constantTag tag = cp->tag_at(index);
    if (tag.is_klass()) {
      // The klass has been inserted into the constant pool
      // very recently.
      klass = cp->resolved_klass_at(index);
    } else if (tag.is_symbol()) {
      symbol = cp->symbol_at(index);
    } else {
      assert(cp->tag_at(index).is_unresolved_klass(), "wrong tag");
      symbol = cp->klass_name_at(index);
    }
  }
  JVMCIObject result;
  if (!klass.is_null()) {
    result = JVMCIENV->get_jvmci_type(klass, JVMCI_CHECK_NULL);
  } else {
    result = JVMCIENV->create_string(symbol, JVMCI_CHECK_NULL);
  }
  return JVMCIENV->get_jobject(result);
C2V_END

C2V_VMENTRY_NULL(jobject, lookupAppendixInPool, (JNIEnv* env, jobject, jobject jvmci_constant_pool, jint index))
  constantPoolHandle cp = JVMCIENV->asConstantPool(jvmci_constant_pool);
  oop appendix_oop = ConstantPool::appendix_at_if_loaded(cp, index);
  return JVMCIENV->get_jobject(JVMCIENV->get_object_constant(appendix_oop));
C2V_END

C2V_VMENTRY_NULL(jobject, lookupMethodInPool, (JNIEnv* env, jobject, jobject jvmci_constant_pool, jint index, jbyte opcode))
  constantPoolHandle cp = JVMCIENV->asConstantPool(jvmci_constant_pool);
  InstanceKlass* pool_holder = cp->pool_holder();
  Bytecodes::Code bc = (Bytecodes::Code) (((int) opcode) & 0xFF);
  methodHandle method = JVMCIRuntime::get_method_by_index(cp, index, bc, pool_holder);
  JVMCIObject result = JVMCIENV->get_jvmci_method(method, JVMCI_CHECK_NULL);
  return JVMCIENV->get_jobject(result);
C2V_END

C2V_VMENTRY_0(jint, constantPoolRemapInstructionOperandFromCache, (JNIEnv* env, jobject, jobject jvmci_constant_pool, jint index))
  constantPoolHandle cp = JVMCIENV->asConstantPool(jvmci_constant_pool);
  return cp->remap_instruction_operand_from_cache(index);
C2V_END

C2V_VMENTRY_NULL(jobject, resolveFieldInPool, (JNIEnv* env, jobject, jobject jvmci_constant_pool, jint index, jobject jvmci_method, jbyte opcode, jintArray info_handle))
  constantPoolHandle cp = JVMCIENV->asConstantPool(jvmci_constant_pool);
  Bytecodes::Code code = (Bytecodes::Code)(((int) opcode) & 0xFF);
  fieldDescriptor fd;
  LinkInfo link_info(cp, index, (jvmci_method != NULL) ? JVMCIENV->asMethod(jvmci_method) : NULL, CHECK_0);
  LinkResolver::resolve_field(fd, link_info, Bytecodes::java_code(code), false, CHECK_0);
  JVMCIPrimitiveArray info = JVMCIENV->wrap(info_handle);
  if (info.is_null() || JVMCIENV->get_length(info) != 3) {
    JVMCI_ERROR_NULL("info must not be null and have a length of 3");
  }
  JVMCIENV->put_int_at(info, 0, fd.access_flags().as_int());
  JVMCIENV->put_int_at(info, 1, fd.offset());
  JVMCIENV->put_int_at(info, 2, fd.index());
  JVMCIKlassHandle handle(THREAD, fd.field_holder());
  JVMCIObject field_holder = JVMCIENV->get_jvmci_type(handle, JVMCI_CHECK_NULL);
  return JVMCIENV->get_jobject(field_holder);
C2V_END

C2V_VMENTRY_0(jint, getVtableIndexForInterfaceMethod, (JNIEnv* env, jobject, jobject jvmci_type, jobject jvmci_method))
  Klass* klass = JVMCIENV->asKlass(jvmci_type);
  Method* method = JVMCIENV->asMethod(jvmci_method);
  if (klass->is_interface()) {
    JVMCI_THROW_MSG_0(InternalError, err_msg("Interface %s should be handled in Java code", klass->external_name()));
  }
  if (!method->method_holder()->is_interface()) {
    JVMCI_THROW_MSG_0(InternalError, err_msg("Method %s is not held by an interface, this case should be handled in Java code", method->name_and_sig_as_C_string()));
  }
  if (!klass->is_instance_klass()) {
    JVMCI_THROW_MSG_0(InternalError, err_msg("Class %s must be instance klass", klass->external_name()));
  }
  if (!InstanceKlass::cast(klass)->is_linked()) {
    JVMCI_THROW_MSG_0(InternalError, err_msg("Class %s must be linked", klass->external_name()));
  }
  return LinkResolver::vtable_index_of_interface_method(klass, method);
C2V_END

C2V_VMENTRY_NULL(jobject, resolveMethod, (JNIEnv* env, jobject, jobject receiver_jvmci_type, jobject jvmci_method, jobject caller_jvmci_type))
  Klass* recv_klass = JVMCIENV->asKlass(receiver_jvmci_type);
  Klass* caller_klass = JVMCIENV->asKlass(caller_jvmci_type);
  methodHandle method = JVMCIENV->asMethod(jvmci_method);

  Klass* resolved     = method->method_holder();
  Symbol* h_name      = method->name();
  Symbol* h_signature = method->signature();

  if (MethodHandles::is_signature_polymorphic_method(method())) {
      // Signature polymorphic methods are already resolved, JVMCI just returns NULL in this case.
      return NULL;
  }

  if (method->name() == vmSymbols::clone_name() &&
      resolved == SystemDictionary::Object_klass() &&
      recv_klass->is_array_klass()) {
    // Resolution of the clone method on arrays always returns Object.clone even though that method
    // has protected access.  There's some trickery in the access checking to make this all work out
    // so it's necessary to pass in the array class as the resolved class to properly trigger this.
    // Otherwise it's impossible to resolve the array clone methods through JVMCI.  See
    // LinkResolver::check_method_accessability for the matching logic.
    resolved = recv_klass;
  }

  LinkInfo link_info(resolved, h_name, h_signature, caller_klass);
  methodHandle m;
  // Only do exact lookup if receiver klass has been linked.  Otherwise,
  // the vtable has not been setup, and the LinkResolver will fail.
  if (recv_klass->is_array_klass() ||
      (InstanceKlass::cast(recv_klass)->is_linked() && !recv_klass->is_interface())) {
    if (resolved->is_interface()) {
      m = LinkResolver::resolve_interface_call_or_null(recv_klass, link_info);
    } else {
      m = LinkResolver::resolve_virtual_call_or_null(recv_klass, link_info);
    }
  }

  if (m.is_null()) {
    // Return NULL if there was a problem with lookup (uninitialized class, etc.)
    return NULL;
  }

  JVMCIObject result = JVMCIENV->get_jvmci_method(m, JVMCI_CHECK_NULL);
  return JVMCIENV->get_jobject(result);
C2V_END

C2V_VMENTRY_0(jboolean, hasFinalizableSubclass,(JNIEnv* env, jobject, jobject jvmci_type))
  Klass* klass = JVMCIENV->asKlass(jvmci_type);
  assert(klass != NULL, "method must not be called for primitive types");
  return Dependencies::find_finalizable_subclass(klass) != NULL;
C2V_END

C2V_VMENTRY_NULL(jobject, getClassInitializer, (JNIEnv* env, jobject, jobject jvmci_type))
  Klass* klass = JVMCIENV->asKlass(jvmci_type);
  if (!klass->is_instance_klass()) {
    return NULL;
  }
  InstanceKlass* iklass = InstanceKlass::cast(klass);
  JVMCIObject result = JVMCIENV->get_jvmci_method(iklass->class_initializer(), JVMCI_CHECK_NULL);
  return JVMCIENV->get_jobject(result);
C2V_END

C2V_VMENTRY_0(jlong, getMaxCallTargetOffset, (JNIEnv* env, jobject, jlong addr))
  address target_addr = (address) addr;
  if (target_addr != 0x0) {
    int64_t off_low = (int64_t)target_addr - ((int64_t)CodeCache::low_bound() + sizeof(int));
    int64_t off_high = (int64_t)target_addr - ((int64_t)CodeCache::high_bound() + sizeof(int));
    return MAX2(ABS(off_low), ABS(off_high));
  }
  return -1;
C2V_END

C2V_VMENTRY(void, setNotInlinableOrCompilable,(JNIEnv* env, jobject,  jobject jvmci_method))
  methodHandle method = JVMCIENV->asMethod(jvmci_method);
  method->set_not_c1_compilable();
  method->set_not_c2_compilable();
  method->set_dont_inline(true);
C2V_END

C2V_VMENTRY_0(jint, installCode, (JNIEnv *env, jobject, jobject target, jobject compiled_code,
            jobject installed_code, jlong failed_speculations_address, jbyteArray speculations_obj))
  HandleMark hm;
  JNIHandleMark jni_hm(thread);

  JVMCIObject target_handle = JVMCIENV->wrap(target);
  JVMCIObject compiled_code_handle = JVMCIENV->wrap(compiled_code);
  CodeBlob* cb = NULL;
  JVMCIObject installed_code_handle = JVMCIENV->wrap(installed_code);
  JVMCIPrimitiveArray speculations_handle = JVMCIENV->wrap(speculations_obj);

  int speculations_len = JVMCIENV->get_length(speculations_handle);
  char* speculations = NEW_RESOURCE_ARRAY(char, speculations_len);
  JVMCIENV->copy_bytes_to(speculations_handle, (jbyte*) speculations, 0, speculations_len);

  JVMCICompiler* compiler = JVMCICompiler::instance(true, CHECK_JNI_ERR);

  TraceTime install_time("installCode", JVMCICompiler::codeInstallTimer());
  bool is_immutable_PIC = JVMCIENV->get_HotSpotCompiledCode_isImmutablePIC(compiled_code_handle) > 0;

  CodeInstaller installer(JVMCIENV, is_immutable_PIC);
  JVMCI::CodeInstallResult result = installer.install(compiler,
      target_handle,
      compiled_code_handle,
      cb,
      installed_code_handle,
      (FailedSpeculation**)(address) failed_speculations_address,
      speculations,
      speculations_len,
      JVMCI_CHECK_0);

  if (PrintCodeCacheOnCompilation) {
    stringStream s;
    // Dump code cache into a buffer before locking the tty,
    {
      MutexLocker mu(CodeCache_lock, Mutex::_no_safepoint_check_flag);
      CodeCache::print_summary(&s, false);
    }
    ttyLocker ttyl;
    tty->print_raw_cr(s.as_string());
  }

  if (result != JVMCI::ok) {
    assert(cb == NULL, "should be");
  } else {
    if (installed_code_handle.is_non_null()) {
      if (cb->is_nmethod()) {
        assert(JVMCIENV->isa_HotSpotNmethod(installed_code_handle), "wrong type");
        // Clear the link to an old nmethod first
        JVMCIObject nmethod_mirror = installed_code_handle;
        JVMCIENV->invalidate_nmethod_mirror(nmethod_mirror, JVMCI_CHECK_0);
      } else {
        assert(JVMCIENV->isa_InstalledCode(installed_code_handle), "wrong type");
      }
      // Initialize the link to the new code blob
      JVMCIENV->initialize_installed_code(installed_code_handle, cb, JVMCI_CHECK_0);
    }
  }
  return result;
C2V_END

C2V_VMENTRY_0(jint, getMetadata, (JNIEnv *env, jobject, jobject target, jobject compiled_code, jobject metadata))
#if INCLUDE_AOT
  HandleMark hm;
  assert(JVMCIENV->is_hotspot(), "AOT code is executed only in HotSpot mode");

  JVMCIObject target_handle = JVMCIENV->wrap(target);
  JVMCIObject compiled_code_handle = JVMCIENV->wrap(compiled_code);
  JVMCIObject metadata_handle = JVMCIENV->wrap(metadata);

  CodeMetadata code_metadata;

  CodeInstaller installer(JVMCIENV, true /* immutable PIC compilation */);
  JVMCI::CodeInstallResult result = installer.gather_metadata(target_handle, compiled_code_handle, code_metadata, JVMCI_CHECK_0);
  if (result != JVMCI::ok) {
    return result;
  }

  if (code_metadata.get_nr_pc_desc() > 0) {
    int size = sizeof(PcDesc) * code_metadata.get_nr_pc_desc();
    JVMCIPrimitiveArray array = JVMCIENV->new_byteArray(size, JVMCI_CHECK_(JVMCI::cache_full));
    JVMCIENV->copy_bytes_from((jbyte*) code_metadata.get_pc_desc(), array, 0, size);
    HotSpotJVMCI::HotSpotMetaData::set_pcDescBytes(JVMCIENV, metadata_handle, array);
  }

  if (code_metadata.get_scopes_size() > 0) {
    int size = code_metadata.get_scopes_size();
    JVMCIPrimitiveArray array = JVMCIENV->new_byteArray(size, JVMCI_CHECK_(JVMCI::cache_full));
    JVMCIENV->copy_bytes_from((jbyte*) code_metadata.get_scopes_desc(), array, 0, size);
    HotSpotJVMCI::HotSpotMetaData::set_scopesDescBytes(JVMCIENV, metadata_handle, array);
  }

  RelocBuffer* reloc_buffer = code_metadata.get_reloc_buffer();
  int size = (int) reloc_buffer->size();
  JVMCIPrimitiveArray array = JVMCIENV->new_byteArray(size, JVMCI_CHECK_(JVMCI::cache_full));
  JVMCIENV->copy_bytes_from((jbyte*) reloc_buffer->begin(), array, 0, size);
  HotSpotJVMCI::HotSpotMetaData::set_relocBytes(JVMCIENV, metadata_handle, array);

  const OopMapSet* oopMapSet = installer.oopMapSet();
  {
    ResourceMark mark;
    ImmutableOopMapBuilder builder(oopMapSet);
    int size = builder.heap_size();
    JVMCIPrimitiveArray array = JVMCIENV->new_byteArray(size, JVMCI_CHECK_(JVMCI::cache_full));
    builder.generate_into((address) HotSpotJVMCI::resolve(array)->byte_at_addr(0));
    HotSpotJVMCI::HotSpotMetaData::set_oopMaps(JVMCIENV, metadata_handle, array);
  }

  AOTOopRecorder* recorder = code_metadata.get_oop_recorder();

  int nr_meta_refs = recorder->nr_meta_refs();
  JVMCIObjectArray metadataArray = JVMCIENV->new_Object_array(nr_meta_refs, JVMCI_CHECK_(JVMCI::cache_full));
  for (int i = 0; i < nr_meta_refs; ++i) {
    jobject element = recorder->meta_element(i);
    if (element == NULL) {
      return JVMCI::cache_full;
    }
    JVMCIENV->put_object_at(metadataArray, i, JVMCIENV->wrap(element));
  }
  HotSpotJVMCI::HotSpotMetaData::set_metadata(JVMCIENV, metadata_handle, metadataArray);

  ExceptionHandlerTable* handler = code_metadata.get_exception_table();
  int table_size = handler->size_in_bytes();
  JVMCIPrimitiveArray exceptionArray = JVMCIENV->new_byteArray(table_size, JVMCI_CHECK_(JVMCI::cache_full));
  if (table_size > 0) {
    handler->copy_bytes_to((address) HotSpotJVMCI::resolve(exceptionArray)->byte_at_addr(0));
  }
  HotSpotJVMCI::HotSpotMetaData::set_exceptionBytes(JVMCIENV, metadata_handle, exceptionArray);

  return result;
#else
  JVMCI_THROW_MSG_0(InternalError, "unimplemented");
#endif
C2V_END

C2V_VMENTRY(void, resetCompilationStatistics, (JNIEnv* env, jobject))
  JVMCICompiler* compiler = JVMCICompiler::instance(true, CHECK);
  CompilerStatistics* stats = compiler->stats();
  stats->_standard.reset();
  stats->_osr.reset();
C2V_END

C2V_VMENTRY_NULL(jobject, disassembleCodeBlob, (JNIEnv* env, jobject, jobject installedCode))
  HandleMark hm;

  if (installedCode == NULL) {
    JVMCI_THROW_MSG_NULL(NullPointerException, "installedCode is null");
  }

  JVMCIObject installedCodeObject = JVMCIENV->wrap(installedCode);
  CodeBlob* cb = JVMCIENV->asCodeBlob(installedCodeObject);
  if (cb == NULL) {
    return NULL;
  }

  // We don't want the stringStream buffer to resize during disassembly as it
  // uses scoped resource memory. If a nested function called during disassembly uses
  // a ResourceMark and the buffer expands within the scope of the mark,
  // the buffer becomes garbage when that scope is exited. Experience shows that
  // the disassembled code is typically about 10x the code size so a fixed buffer
  // sized to 20x code size plus a fixed amount for header info should be sufficient.
  int bufferSize = cb->code_size() * 20 + 1024;
  char* buffer = NEW_RESOURCE_ARRAY(char, bufferSize);
  stringStream st(buffer, bufferSize);
  if (cb->is_nmethod()) {
    nmethod* nm = (nmethod*) cb;
    if (!nm->is_alive()) {
      return NULL;
    }
  }
  Disassembler::decode(cb, &st);
  if (st.size() <= 0) {
    return NULL;
  }

  JVMCIObject result = JVMCIENV->create_string(st.as_string(), JVMCI_CHECK_NULL);
  return JVMCIENV->get_jobject(result);
C2V_END

C2V_VMENTRY_NULL(jobject, getStackTraceElement, (JNIEnv* env, jobject, jobject jvmci_method, int bci))
  HandleMark hm;

  methodHandle method = JVMCIENV->asMethod(jvmci_method);
  JVMCIObject element = JVMCIENV->new_StackTraceElement(method, bci, JVMCI_CHECK_NULL);
  return JVMCIENV->get_jobject(element);
C2V_END

C2V_VMENTRY_NULL(jobject, executeHotSpotNmethod, (JNIEnv* env, jobject, jobject args, jobject hs_nmethod))
  // The incoming arguments array would have to contain JavaConstants instead of regular objects
  // and the return value would have to be wrapped as a JavaConstant.
  requireInHotSpot("executeHotSpotNmethod", JVMCI_CHECK_NULL);

  HandleMark hm;

  JVMCIObject nmethod_mirror = JVMCIENV->wrap(hs_nmethod);
  nmethod* nm = JVMCIENV->asNmethod(nmethod_mirror);
  if (nm == NULL) {
    JVMCI_THROW_NULL(InvalidInstalledCodeException);
  }
  methodHandle mh = nm->method();
  Symbol* signature = mh->signature();
  JavaCallArguments jca(mh->size_of_parameters());

  JavaArgumentUnboxer jap(signature, &jca, (arrayOop) JNIHandles::resolve(args), mh->is_static());
  JavaValue result(jap.get_ret_type());
  jca.set_alternative_target(nm);
  JavaCalls::call(&result, mh, &jca, CHECK_NULL);

  if (jap.get_ret_type() == T_VOID) {
    return NULL;
  } else if (jap.get_ret_type() == T_OBJECT || jap.get_ret_type() == T_ARRAY) {
    return JNIHandles::make_local((oop) result.get_jobject());
  } else {
    jvalue *value = (jvalue *) result.get_value_addr();
    // Narrow the value down if required (Important on big endian machines)
    switch (jap.get_ret_type()) {
      case T_BOOLEAN:
       value->z = (jboolean) value->i;
       break;
      case T_BYTE:
       value->b = (jbyte) value->i;
       break;
      case T_CHAR:
       value->c = (jchar) value->i;
       break;
      case T_SHORT:
       value->s = (jshort) value->i;
       break;
      default:
        break;
    }
    JVMCIObject o = JVMCIENV->create_box(jap.get_ret_type(), value, JVMCI_CHECK_NULL);
    return JVMCIENV->get_jobject(o);
  }
C2V_END

C2V_VMENTRY_NULL(jlongArray, getLineNumberTable, (JNIEnv* env, jobject, jobject jvmci_method))
  Method* method = JVMCIENV->asMethod(jvmci_method);
  if (!method->has_linenumber_table()) {
    return NULL;
  }
  u2 num_entries = 0;
  CompressedLineNumberReadStream streamForSize(method->compressed_linenumber_table());
  while (streamForSize.read_pair()) {
    num_entries++;
  }

  CompressedLineNumberReadStream stream(method->compressed_linenumber_table());
  JVMCIPrimitiveArray result = JVMCIENV->new_longArray(2 * num_entries, JVMCI_CHECK_NULL);

  int i = 0;
  jlong value;
  while (stream.read_pair()) {
    value = ((long) stream.bci());
    JVMCIENV->put_long_at(result, i, value);
    value = ((long) stream.line());
    JVMCIENV->put_long_at(result, i + 1, value);
    i += 2;
  }

  return (jlongArray) JVMCIENV->get_jobject(result);
C2V_END

C2V_VMENTRY_0(jlong, getLocalVariableTableStart, (JNIEnv* env, jobject, jobject jvmci_method))
  Method* method = JVMCIENV->asMethod(jvmci_method);
  if (!method->has_localvariable_table()) {
    return 0;
  }
  return (jlong) (address) method->localvariable_table_start();
C2V_END

C2V_VMENTRY_0(jint, getLocalVariableTableLength, (JNIEnv* env, jobject, jobject jvmci_method))
  Method* method = JVMCIENV->asMethod(jvmci_method);
  return method->localvariable_table_length();
C2V_END

C2V_VMENTRY(void, reprofile, (JNIEnv* env, jobject, jobject jvmci_method))
  Method* method = JVMCIENV->asMethod(jvmci_method);
  MethodCounters* mcs = method->method_counters();
  if (mcs != NULL) {
    mcs->clear_counters();
  }
  NOT_PRODUCT(method->set_compiled_invocation_count(0));

  CompiledMethod* code = method->code();
  if (code != NULL) {
    code->make_not_entrant();
  }

  MethodData* method_data = method->method_data();
  if (method_data == NULL) {
    ClassLoaderData* loader_data = method->method_holder()->class_loader_data();
    method_data = MethodData::allocate(loader_data, method, CHECK);
    method->set_method_data(method_data);
  } else {
    method_data->initialize();
  }
C2V_END


C2V_VMENTRY(void, invalidateHotSpotNmethod, (JNIEnv* env, jobject, jobject hs_nmethod))
  JVMCIObject nmethod_mirror = JVMCIENV->wrap(hs_nmethod);
  JVMCIENV->invalidate_nmethod_mirror(nmethod_mirror, JVMCI_CHECK);
C2V_END

C2V_VMENTRY_NULL(jobject, readUncompressedOop, (JNIEnv* env, jobject, jlong addr))
  oop ret = RawAccess<>::oop_load((oop*)(address)addr);
  return JVMCIENV->get_jobject(JVMCIENV->get_object_constant(ret));
 C2V_END

C2V_VMENTRY_NULL(jlongArray, collectCounters, (JNIEnv* env, jobject))
  // Returns a zero length array if counters aren't enabled
  JVMCIPrimitiveArray array = JVMCIENV->new_longArray(JVMCICounterSize, JVMCI_CHECK_NULL);
  if (JVMCICounterSize > 0) {
    jlong* temp_array = NEW_RESOURCE_ARRAY(jlong, JVMCICounterSize);
    JavaThread::collect_counters(temp_array, JVMCICounterSize);
    JVMCIENV->copy_longs_from(temp_array, array, 0, JVMCICounterSize);
  }
  return (jlongArray) JVMCIENV->get_jobject(array);
C2V_END

C2V_VMENTRY_0(int, allocateCompileId, (JNIEnv* env, jobject, jobject jvmci_method, int entry_bci))
  HandleMark hm;
  if (jvmci_method == NULL) {
    JVMCI_THROW_0(NullPointerException);
  }
  Method* method = JVMCIENV->asMethod(jvmci_method);
  if (entry_bci >= method->code_size() || entry_bci < -1) {
    JVMCI_THROW_MSG_0(IllegalArgumentException, err_msg("Unexpected bci %d", entry_bci));
  }
  return CompileBroker::assign_compile_id_unlocked(THREAD, method, entry_bci);
C2V_END


C2V_VMENTRY_0(jboolean, isMature, (JNIEnv* env, jobject, jlong metaspace_method_data))
  MethodData* mdo = JVMCIENV->asMethodData(metaspace_method_data);
  return mdo != NULL && mdo->is_mature();
C2V_END

C2V_VMENTRY_0(jboolean, hasCompiledCodeForOSR, (JNIEnv* env, jobject, jobject jvmci_method, int entry_bci, int comp_level))
  Method* method = JVMCIENV->asMethod(jvmci_method);
  return method->lookup_osr_nmethod_for(entry_bci, comp_level, true) != NULL;
C2V_END

C2V_VMENTRY_NULL(jobject, getSymbol, (JNIEnv* env, jobject, jlong symbol))
  JVMCIObject sym = JVMCIENV->create_string((Symbol*)(address)symbol, JVMCI_CHECK_NULL);
  return JVMCIENV->get_jobject(sym);
C2V_END

bool matches(jobjectArray methods, Method* method, JVMCIEnv* JVMCIENV) {
  objArrayOop methods_oop = (objArrayOop) JNIHandles::resolve(methods);

  for (int i = 0; i < methods_oop->length(); i++) {
    oop resolved = methods_oop->obj_at(i);
    if ((resolved->klass() == HotSpotJVMCI::HotSpotResolvedJavaMethodImpl::klass()) && HotSpotJVMCI::asMethod(JVMCIENV, resolved) == method) {
      return true;
    }
  }
  return false;
}

void call_interface(JavaValue* result, Klass* spec_klass, Symbol* name, Symbol* signature, JavaCallArguments* args, TRAPS) {
  CallInfo callinfo;
  Handle receiver = args->receiver();
  Klass* recvrKlass = receiver.is_null() ? (Klass*)NULL : receiver->klass();
  LinkInfo link_info(spec_klass, name, signature);
  LinkResolver::resolve_interface_call(
          callinfo, receiver, recvrKlass, link_info, true, CHECK);
  methodHandle method = callinfo.selected_method();
  assert(method.not_null(), "should have thrown exception");

  // Invoke the method
  JavaCalls::call(result, method, args, CHECK);
}

C2V_VMENTRY_NULL(jobject, iterateFrames, (JNIEnv* env, jobject compilerToVM, jobjectArray initial_methods, jobjectArray match_methods, jint initialSkip, jobject visitor_handle))

  if (!thread->has_last_Java_frame()) {
    return NULL;
  }
  Handle visitor(THREAD, JNIHandles::resolve_non_null(visitor_handle));

  requireInHotSpot("iterateFrames", JVMCI_CHECK_NULL);

  HotSpotJVMCI::HotSpotStackFrameReference::klass()->initialize(CHECK_NULL);
  Handle frame_reference = HotSpotJVMCI::HotSpotStackFrameReference::klass()->allocate_instance_handle(CHECK_NULL);

  StackFrameStream fst(thread);
  jobjectArray methods = initial_methods;

  int frame_number = 0;
  vframe* vf = vframe::new_vframe(fst.current(), fst.register_map(), thread);

  while (true) {
    // look for the given method
    bool realloc_called = false;
    while (true) {
      StackValueCollection* locals = NULL;
      if (vf->is_compiled_frame()) {
        // compiled method frame
        compiledVFrame* cvf = compiledVFrame::cast(vf);
        if (methods == NULL || matches(methods, cvf->method(), JVMCIENV)) {
          if (initialSkip > 0) {
            initialSkip--;
          } else {
            ScopeDesc* scope = cvf->scope();
            // native wrappers do not have a scope
            if (scope != NULL && scope->objects() != NULL) {
              GrowableArray<ScopeValue*>* objects;
              if (!realloc_called) {
                objects = scope->objects();
              } else {
                // some object might already have been re-allocated, only reallocate the non-allocated ones
                objects = new GrowableArray<ScopeValue*>(scope->objects()->length());
                for (int i = 0; i < scope->objects()->length(); i++) {
                  ObjectValue* sv = (ObjectValue*) scope->objects()->at(i);
                  if (sv->value().is_null()) {
                    objects->append(sv);
                  }
                }
              }
              bool realloc_failures = Deoptimization::realloc_objects(thread, fst.current(), objects, CHECK_NULL);
              Deoptimization::reassign_fields(fst.current(), fst.register_map(), objects, realloc_failures, false);
              realloc_called = true;

              GrowableArray<ScopeValue*>* local_values = scope->locals();
              assert(local_values != NULL, "NULL locals");
              typeArrayOop array_oop = oopFactory::new_boolArray(local_values->length(), CHECK_NULL);
              typeArrayHandle array(THREAD, array_oop);
              for (int i = 0; i < local_values->length(); i++) {
                ScopeValue* value = local_values->at(i);
                if (value->is_object()) {
                  array->bool_at_put(i, true);
                }
              }
              HotSpotJVMCI::HotSpotStackFrameReference::set_localIsVirtual(JVMCIENV, frame_reference(), array());
            } else {
              HotSpotJVMCI::HotSpotStackFrameReference::set_localIsVirtual(JVMCIENV, frame_reference(), NULL);
            }

            locals = cvf->locals();
            HotSpotJVMCI::HotSpotStackFrameReference::set_bci(JVMCIENV, frame_reference(), cvf->bci());
            JVMCIObject method = JVMCIENV->get_jvmci_method(cvf->method(), JVMCI_CHECK_NULL);
            HotSpotJVMCI::HotSpotStackFrameReference::set_method(JVMCIENV, frame_reference(), JNIHandles::resolve(method.as_jobject()));
          }
        }
      } else if (vf->is_interpreted_frame()) {
        // interpreted method frame
        interpretedVFrame* ivf = interpretedVFrame::cast(vf);
        if (methods == NULL || matches(methods, ivf->method(), JVMCIENV)) {
          if (initialSkip > 0) {
            initialSkip--;
          } else {
            locals = ivf->locals();
            HotSpotJVMCI::HotSpotStackFrameReference::set_bci(JVMCIENV, frame_reference(), ivf->bci());
            JVMCIObject method = JVMCIENV->get_jvmci_method(ivf->method(), JVMCI_CHECK_NULL);
            HotSpotJVMCI::HotSpotStackFrameReference::set_method(JVMCIENV, frame_reference(), JNIHandles::resolve(method.as_jobject()));
            HotSpotJVMCI::HotSpotStackFrameReference::set_localIsVirtual(JVMCIENV, frame_reference(), NULL);
          }
        }
      }

      // locals != NULL means that we found a matching frame and result is already partially initialized
      if (locals != NULL) {
        methods = match_methods;
        HotSpotJVMCI::HotSpotStackFrameReference::set_compilerToVM(JVMCIENV, frame_reference(), JNIHandles::resolve(compilerToVM));
        HotSpotJVMCI::HotSpotStackFrameReference::set_stackPointer(JVMCIENV, frame_reference(), (jlong) fst.current()->sp());
        HotSpotJVMCI::HotSpotStackFrameReference::set_frameNumber(JVMCIENV, frame_reference(), frame_number);

        // initialize the locals array
        objArrayOop array_oop = oopFactory::new_objectArray(locals->size(), CHECK_NULL);
        objArrayHandle array(THREAD, array_oop);
        for (int i = 0; i < locals->size(); i++) {
          StackValue* var = locals->at(i);
          if (var->type() == T_OBJECT) {
            array->obj_at_put(i, locals->at(i)->get_obj()());
          }
        }
        HotSpotJVMCI::HotSpotStackFrameReference::set_locals(JVMCIENV, frame_reference(), array());
        HotSpotJVMCI::HotSpotStackFrameReference::set_objectsMaterialized(JVMCIENV, frame_reference(), JNI_FALSE);

        JavaValue result(T_OBJECT);
        JavaCallArguments args(visitor);
        args.push_oop(frame_reference);
        call_interface(&result, HotSpotJVMCI::InspectedFrameVisitor::klass(), vmSymbols::visitFrame_name(), vmSymbols::visitFrame_signature(), &args, CHECK_NULL);
        if (result.get_jobject() != NULL) {
          return JNIHandles::make_local(thread, (oop) result.get_jobject());
        }
        assert(initialSkip == 0, "There should be no match before initialSkip == 0");
        if (HotSpotJVMCI::HotSpotStackFrameReference::objectsMaterialized(JVMCIENV, frame_reference()) == JNI_TRUE) {
          // the frame has been deoptimized, we need to re-synchronize the frame and vframe
          intptr_t* stack_pointer = (intptr_t*) HotSpotJVMCI::HotSpotStackFrameReference::stackPointer(JVMCIENV, frame_reference());
          fst = StackFrameStream(thread);
          while (fst.current()->sp() != stack_pointer && !fst.is_done()) {
            fst.next();
          }
          if (fst.current()->sp() != stack_pointer) {
            THROW_MSG_NULL(vmSymbols::java_lang_IllegalStateException(), "stack frame not found after deopt")
          }
          vf = vframe::new_vframe(fst.current(), fst.register_map(), thread);
          if (!vf->is_compiled_frame()) {
            THROW_MSG_NULL(vmSymbols::java_lang_IllegalStateException(), "compiled stack frame expected")
          }
          for (int i = 0; i < frame_number; i++) {
            if (vf->is_top()) {
              THROW_MSG_NULL(vmSymbols::java_lang_IllegalStateException(), "vframe not found after deopt")
            }
            vf = vf->sender();
            assert(vf->is_compiled_frame(), "Wrong frame type");
          }
        }
        frame_reference = HotSpotJVMCI::HotSpotStackFrameReference::klass()->allocate_instance_handle(CHECK_NULL);
        HotSpotJVMCI::HotSpotStackFrameReference::klass()->initialize(CHECK_NULL);
      }

      if (vf->is_top()) {
        break;
      }
      frame_number++;
      vf = vf->sender();
    } // end of vframe loop

    if (fst.is_done()) {
      break;
    }
    fst.next();
    vf = vframe::new_vframe(fst.current(), fst.register_map(), thread);
    frame_number = 0;
  } // end of frame loop

  // the end was reached without finding a matching method
  return NULL;
C2V_END

C2V_VMENTRY(void, resolveInvokeDynamicInPool, (JNIEnv* env, jobject, jobject jvmci_constant_pool, jint index))
  constantPoolHandle cp = JVMCIENV->asConstantPool(jvmci_constant_pool);
  CallInfo callInfo;
  LinkResolver::resolve_invoke(callInfo, Handle(), cp, index, Bytecodes::_invokedynamic, CHECK);
  ConstantPoolCacheEntry* cp_cache_entry = cp->invokedynamic_cp_cache_entry_at(index);
  cp_cache_entry->set_dynamic_call(cp, callInfo);
C2V_END

C2V_VMENTRY(void, resolveInvokeHandleInPool, (JNIEnv* env, jobject, jobject jvmci_constant_pool, jint index))
  constantPoolHandle cp = JVMCIENV->asConstantPool(jvmci_constant_pool);
  Klass* holder = cp->klass_ref_at(index, CHECK);
  Symbol* name = cp->name_ref_at(index);
  if (MethodHandles::is_signature_polymorphic_name(holder, name)) {
    CallInfo callInfo;
    LinkResolver::resolve_invoke(callInfo, Handle(), cp, index, Bytecodes::_invokehandle, CHECK);
    ConstantPoolCacheEntry* cp_cache_entry = cp->cache()->entry_at(cp->decode_cpcache_index(index));
    cp_cache_entry->set_method_handle(cp, callInfo);
  }
C2V_END

C2V_VMENTRY_0(jint, isResolvedInvokeHandleInPool, (JNIEnv* env, jobject, jobject jvmci_constant_pool, jint index))
  constantPoolHandle cp = JVMCIENV->asConstantPool(jvmci_constant_pool);
  ConstantPoolCacheEntry* cp_cache_entry = cp->cache()->entry_at(cp->decode_cpcache_index(index));
  if (cp_cache_entry->is_resolved(Bytecodes::_invokehandle)) {
    // MethodHandle.invoke* --> LambdaForm?
    ResourceMark rm;

    LinkInfo link_info(cp, index, CATCH);

    Klass* resolved_klass = link_info.resolved_klass();

    Symbol* name_sym = cp->name_ref_at(index);

    vmassert(MethodHandles::is_method_handle_invoke_name(resolved_klass, name_sym), "!");
    vmassert(MethodHandles::is_signature_polymorphic_name(resolved_klass, name_sym), "!");

    methodHandle adapter_method(cp_cache_entry->f1_as_method());

    methodHandle resolved_method(adapter_method);

    // Can we treat it as a regular invokevirtual?
    if (resolved_method->method_holder() == resolved_klass && resolved_method->name() == name_sym) {
      vmassert(!resolved_method->is_static(),"!");
      vmassert(MethodHandles::is_signature_polymorphic_method(resolved_method()),"!");
      vmassert(!MethodHandles::is_signature_polymorphic_static(resolved_method->intrinsic_id()), "!");
      vmassert(cp_cache_entry->appendix_if_resolved(cp) == NULL, "!");

      methodHandle m(LinkResolver::linktime_resolve_virtual_method_or_null(link_info));
      vmassert(m == resolved_method, "!!");
      return -1;
    }

    return Bytecodes::_invokevirtual;
  }
  if (cp_cache_entry->is_resolved(Bytecodes::_invokedynamic)) {
    return Bytecodes::_invokedynamic;
  }
  return -1;
C2V_END


C2V_VMENTRY_NULL(jobject, getSignaturePolymorphicHolders, (JNIEnv* env, jobject))
  JVMCIObjectArray holders = JVMCIENV->new_String_array(2, JVMCI_CHECK_NULL);
  JVMCIObject mh = JVMCIENV->create_string("Ljava/lang/invoke/MethodHandle;", JVMCI_CHECK_NULL);
  JVMCIObject vh = JVMCIENV->create_string("Ljava/lang/invoke/VarHandle;", JVMCI_CHECK_NULL);
  JVMCIENV->put_object_at(holders, 0, mh);
  JVMCIENV->put_object_at(holders, 1, vh);
  return JVMCIENV->get_jobject(holders);
C2V_END

C2V_VMENTRY_0(jboolean, shouldDebugNonSafepoints, (JNIEnv* env, jobject))
  //see compute_recording_non_safepoints in debugInfroRec.cpp
  if (JvmtiExport::should_post_compiled_method_load() && FLAG_IS_DEFAULT(DebugNonSafepoints)) {
    return true;
  }
  return DebugNonSafepoints;
C2V_END

// public native void materializeVirtualObjects(HotSpotStackFrameReference stackFrame, boolean invalidate);
C2V_VMENTRY(void, materializeVirtualObjects, (JNIEnv* env, jobject, jobject _hs_frame, bool invalidate))
  JVMCIObject hs_frame = JVMCIENV->wrap(_hs_frame);
  if (hs_frame.is_null()) {
    JVMCI_THROW_MSG(NullPointerException, "stack frame is null");
  }

  requireInHotSpot("materializeVirtualObjects", JVMCI_CHECK);

  JVMCIENV->HotSpotStackFrameReference_initialize(JVMCI_CHECK);

  // look for the given stack frame
  StackFrameStream fst(thread);
  intptr_t* stack_pointer = (intptr_t*) JVMCIENV->get_HotSpotStackFrameReference_stackPointer(hs_frame);
  while (fst.current()->sp() != stack_pointer && !fst.is_done()) {
    fst.next();
  }
  if (fst.current()->sp() != stack_pointer) {
    JVMCI_THROW_MSG(IllegalStateException, "stack frame not found");
  }

  if (invalidate) {
    if (!fst.current()->is_compiled_frame()) {
      JVMCI_THROW_MSG(IllegalStateException, "compiled stack frame expected");
    }
    assert(fst.current()->cb()->is_nmethod(), "nmethod expected");
    ((nmethod*) fst.current()->cb())->make_not_entrant();
  }
  Deoptimization::deoptimize(thread, *fst.current(), fst.register_map(), Deoptimization::Reason_none);
  // look for the frame again as it has been updated by deopt (pc, deopt state...)
  StackFrameStream fstAfterDeopt(thread);
  while (fstAfterDeopt.current()->sp() != stack_pointer && !fstAfterDeopt.is_done()) {
    fstAfterDeopt.next();
  }
  if (fstAfterDeopt.current()->sp() != stack_pointer) {
    JVMCI_THROW_MSG(IllegalStateException, "stack frame not found after deopt");
  }

  vframe* vf = vframe::new_vframe(fstAfterDeopt.current(), fstAfterDeopt.register_map(), thread);
  if (!vf->is_compiled_frame()) {
    JVMCI_THROW_MSG(IllegalStateException, "compiled stack frame expected");
  }

  GrowableArray<compiledVFrame*>* virtualFrames = new GrowableArray<compiledVFrame*>(10);
  while (true) {
    assert(vf->is_compiled_frame(), "Wrong frame type");
    virtualFrames->push(compiledVFrame::cast(vf));
    if (vf->is_top()) {
      break;
    }
    vf = vf->sender();
  }

  int last_frame_number = JVMCIENV->get_HotSpotStackFrameReference_frameNumber(hs_frame);
  if (last_frame_number >= virtualFrames->length()) {
    JVMCI_THROW_MSG(IllegalStateException, "invalid frame number");
  }

  // Reallocate the non-escaping objects and restore their fields.
  assert (virtualFrames->at(last_frame_number)->scope() != NULL,"invalid scope");
  GrowableArray<ScopeValue*>* objects = virtualFrames->at(last_frame_number)->scope()->objects();

  if (objects == NULL) {
    // no objects to materialize
    return;
  }

  bool realloc_failures = Deoptimization::realloc_objects(thread, fstAfterDeopt.current(), objects, CHECK);
  Deoptimization::reassign_fields(fstAfterDeopt.current(), fstAfterDeopt.register_map(), objects, realloc_failures, false);

  for (int frame_index = 0; frame_index < virtualFrames->length(); frame_index++) {
    compiledVFrame* cvf = virtualFrames->at(frame_index);

    GrowableArray<ScopeValue*>* scopeLocals = cvf->scope()->locals();
    StackValueCollection* locals = cvf->locals();
    if (locals != NULL) {
      for (int i2 = 0; i2 < locals->size(); i2++) {
        StackValue* var = locals->at(i2);
        if (var->type() == T_OBJECT && scopeLocals->at(i2)->is_object()) {
          jvalue val;
          val.l = (jobject) locals->at(i2)->get_obj()();
          cvf->update_local(T_OBJECT, i2, val);
        }
      }
    }

    GrowableArray<ScopeValue*>* scopeExpressions = cvf->scope()->expressions();
    StackValueCollection* expressions = cvf->expressions();
    if (expressions != NULL) {
      for (int i2 = 0; i2 < expressions->size(); i2++) {
        StackValue* var = expressions->at(i2);
        if (var->type() == T_OBJECT && scopeExpressions->at(i2)->is_object()) {
          jvalue val;
          val.l = (jobject) expressions->at(i2)->get_obj()();
          cvf->update_stack(T_OBJECT, i2, val);
        }
      }
    }

    GrowableArray<MonitorValue*>* scopeMonitors = cvf->scope()->monitors();
    GrowableArray<MonitorInfo*>* monitors = cvf->monitors();
    if (monitors != NULL) {
      for (int i2 = 0; i2 < monitors->length(); i2++) {
        cvf->update_monitor(i2, monitors->at(i2));
      }
    }
  }

  // all locals are materialized by now
  JVMCIENV->set_HotSpotStackFrameReference_localIsVirtual(hs_frame, NULL);
  // update the locals array
  JVMCIObjectArray array = JVMCIENV->get_HotSpotStackFrameReference_locals(hs_frame);
  StackValueCollection* locals = virtualFrames->at(last_frame_number)->locals();
  for (int i = 0; i < locals->size(); i++) {
    StackValue* var = locals->at(i);
    if (var->type() == T_OBJECT) {
      JVMCIENV->put_object_at(array, i, HotSpotJVMCI::wrap(locals->at(i)->get_obj()()));
    }
  }
  HotSpotJVMCI::HotSpotStackFrameReference::set_objectsMaterialized(JVMCIENV, hs_frame, JNI_TRUE);
C2V_END

// Creates a scope where the current thread is attached and detached
// from HotSpot if it wasn't already attached when entering the scope.
extern "C" void jio_printf(const char *fmt, ...);
class AttachDetach : public StackObj {
 public:
  bool _attached;
  AttachDetach(JNIEnv* env, Thread* current_thread) {
    if (current_thread == NULL) {
      extern struct JavaVM_ main_vm;
      JNIEnv* hotspotEnv;
      jint res = main_vm.AttachCurrentThread((void**)&hotspotEnv, NULL);
      _attached = res == JNI_OK;
      static volatile int report_attach_error = 0;
      if (res != JNI_OK && report_attach_error == 0 && Atomic::cmpxchg(1, &report_attach_error, 0) == 0) {
        // Only report an attach error once
        jio_printf("Warning: attaching current thread to VM failed with %d (future attach errors are suppressed)\n", res);
      }
    } else {
      _attached = false;
    }
  }
  ~AttachDetach() {
    if (_attached && get_current_thread() != NULL) {
      extern struct JavaVM_ main_vm;
      jint res = main_vm.DetachCurrentThread();
      static volatile int report_detach_error = 0;
      if (res != JNI_OK && report_detach_error == 0 && Atomic::cmpxchg(1, &report_detach_error, 0) == 0) {
        // Only report an attach error once
        jio_printf("Warning: detaching current thread from VM failed with %d (future attach errors are suppressed)\n", res);
      }
    }
  }
};

C2V_VMENTRY_PREFIX(jint, writeDebugOutput, (JNIEnv* env, jobject, jbyteArray bytes, jint offset, jint length, bool flush, bool can_throw))
  AttachDetach ad(env, base_thread);
  bool use_tty = true;
  if (base_thread == NULL) {
    if (!ad._attached) {
      // Can only use tty if the current thread is attached
      return 0;
    }
    base_thread = get_current_thread();
  }
  JVMCITraceMark jtm("writeDebugOutput");
  assert(base_thread->is_Java_thread(), "just checking");
  JavaThread* thread = (JavaThread*) base_thread;
  C2V_BLOCK(void, writeDebugOutput, (JNIEnv* env, jobject, jbyteArray bytes, jint offset, jint length))
  if (bytes == NULL) {
    if (can_throw) {
      JVMCI_THROW_0(NullPointerException);
    }
    return -1;
  }
  JVMCIPrimitiveArray array = JVMCIENV->wrap(bytes);

  // Check if offset and length are non negative.
  if (offset < 0 || length < 0) {
    if (can_throw) {
      JVMCI_THROW_0(ArrayIndexOutOfBoundsException);
    }
    return -2;
  }
  // Check if the range is valid.
  int array_length = JVMCIENV->get_length(array);
  if ((((unsigned int) length + (unsigned int) offset) > (unsigned int) array_length)) {
    if (can_throw) {
      JVMCI_THROW_0(ArrayIndexOutOfBoundsException);
    }
    return -2;
  }
  jbyte buffer[O_BUFLEN];
  while (length > 0) {
    int copy_len = MIN2(length, (jint)O_BUFLEN);
    JVMCIENV->copy_bytes_to(array, buffer, offset, copy_len);
    tty->write((char*) buffer, copy_len);
    length -= O_BUFLEN;
    offset += O_BUFLEN;
  }
  if (flush) {
    tty->flush();
  }
  return 0;
C2V_END

C2V_VMENTRY(void, flushDebugOutput, (JNIEnv* env, jobject))
  tty->flush();
C2V_END

C2V_VMENTRY_0(int, methodDataProfileDataSize, (JNIEnv* env, jobject, jlong metaspace_method_data, jint position))
  MethodData* mdo = JVMCIENV->asMethodData(metaspace_method_data);
  ProfileData* profile_data = mdo->data_at(position);
  if (mdo->is_valid(profile_data)) {
    return profile_data->size_in_bytes();
  }
  DataLayout* data    = mdo->extra_data_base();
  DataLayout* end   = mdo->extra_data_limit();
  for (;; data = mdo->next_extra(data)) {
    assert(data < end, "moved past end of extra data");
    profile_data = data->data_in();
    if (mdo->dp_to_di(profile_data->dp()) == position) {
      return profile_data->size_in_bytes();
    }
  }
  JVMCI_THROW_MSG_0(IllegalArgumentException, err_msg("Invalid profile data position %d", position));
C2V_END

C2V_VMENTRY_0(jlong, getFingerprint, (JNIEnv* env, jobject, jlong metaspace_klass))
#if INCLUDE_AOT
  Klass *k = (Klass*) (address) metaspace_klass;
  if (k->is_instance_klass()) {
    return InstanceKlass::cast(k)->get_stored_fingerprint();
  } else {
    return 0;
  }
#else
  JVMCI_THROW_MSG_0(InternalError, "unimplemented");
#endif
C2V_END

C2V_VMENTRY_NULL(jobject, getHostClass, (JNIEnv* env, jobject, jobject jvmci_type))
  InstanceKlass* k = InstanceKlass::cast(JVMCIENV->asKlass(jvmci_type));
  InstanceKlass* host = k->unsafe_anonymous_host();
  JVMCIKlassHandle handle(THREAD, host);
  JVMCIObject result = JVMCIENV->get_jvmci_type(handle, JVMCI_CHECK_NULL);
  return JVMCIENV->get_jobject(result);
C2V_END

C2V_VMENTRY_NULL(jobject, getInterfaces, (JNIEnv* env, jobject, jobject jvmci_type))
  if (jvmci_type == NULL) {
    JVMCI_THROW_0(NullPointerException);
  }

  Klass* klass = JVMCIENV->asKlass(jvmci_type);
  if (klass == NULL) {
    JVMCI_THROW_0(NullPointerException);
  }
  if (!klass->is_instance_klass()) {
    JVMCI_THROW_MSG_0(InternalError, err_msg("Class %s must be instance klass", klass->external_name()));
  }
  InstanceKlass* iklass = InstanceKlass::cast(klass);

  // Regular instance klass, fill in all local interfaces
  int size = iklass->local_interfaces()->length();
  JVMCIObjectArray interfaces = JVMCIENV->new_HotSpotResolvedObjectTypeImpl_array(size, JVMCI_CHECK_NULL);
  for (int index = 0; index < size; index++) {
    JVMCIKlassHandle klass(THREAD);
    Klass* k = iklass->local_interfaces()->at(index);
    klass = k;
    JVMCIObject type = JVMCIENV->get_jvmci_type(klass, JVMCI_CHECK_NULL);
    JVMCIENV->put_object_at(interfaces, index, type);
  }
  return JVMCIENV->get_jobject(interfaces);
C2V_END

C2V_VMENTRY_NULL(jobject, getComponentType, (JNIEnv* env, jobject, jobject jvmci_type))
  if (jvmci_type == NULL) {
    JVMCI_THROW_0(NullPointerException);
  }

  Klass* klass = JVMCIENV->asKlass(jvmci_type);
  oop mirror = klass->java_mirror();
  if (java_lang_Class::is_primitive(mirror) ||
      !java_lang_Class::as_Klass(mirror)->is_array_klass()) {
    return NULL;
  }

  oop component_mirror = java_lang_Class::component_mirror(mirror);
  if (component_mirror == NULL) {
    return NULL;
  }
  Klass* component_klass = java_lang_Class::as_Klass(component_mirror);
  if (component_klass != NULL) {
    JVMCIKlassHandle klass_handle(THREAD);
    klass_handle = component_klass;
    JVMCIObject result = JVMCIENV->get_jvmci_type(klass_handle, JVMCI_CHECK_NULL);
    return JVMCIENV->get_jobject(result);
  }
  BasicType type = java_lang_Class::primitive_type(component_mirror);
  JVMCIObject result = JVMCIENV->get_jvmci_primitive_type(type);
  return JVMCIENV->get_jobject(result);
C2V_END

C2V_VMENTRY(void, ensureInitialized, (JNIEnv* env, jobject, jobject jvmci_type))
  if (jvmci_type == NULL) {
    JVMCI_THROW(NullPointerException);
  }

  Klass* klass = JVMCIENV->asKlass(jvmci_type);
  if (klass != NULL && klass->should_be_initialized()) {
    InstanceKlass* k = InstanceKlass::cast(klass);
    k->initialize(CHECK);
  }
C2V_END

C2V_VMENTRY_0(int, interpreterFrameSize, (JNIEnv* env, jobject, jobject bytecode_frame_handle))
  if (bytecode_frame_handle == NULL) {
    JVMCI_THROW_0(NullPointerException);
  }

  JVMCIObject top_bytecode_frame = JVMCIENV->wrap(bytecode_frame_handle);
  JVMCIObject bytecode_frame = top_bytecode_frame;
  int size = 0;
  int callee_parameters = 0;
  int callee_locals = 0;
  Method* method = JVMCIENV->asMethod(JVMCIENV->get_BytecodePosition_method(bytecode_frame));
  int extra_args = method->max_stack() - JVMCIENV->get_BytecodeFrame_numStack(bytecode_frame);

  while (bytecode_frame.is_non_null()) {
    int locks = JVMCIENV->get_BytecodeFrame_numLocks(bytecode_frame);
    int temps = JVMCIENV->get_BytecodeFrame_numStack(bytecode_frame);
    bool is_top_frame = (JVMCIENV->equals(bytecode_frame, top_bytecode_frame));
    Method* method = JVMCIENV->asMethod(JVMCIENV->get_BytecodePosition_method(bytecode_frame));

    int frame_size = BytesPerWord * Interpreter::size_activation(method->max_stack(),
                                                                 temps + callee_parameters,
                                                                 extra_args,
                                                                 locks,
                                                                 callee_parameters,
                                                                 callee_locals,
                                                                 is_top_frame);
    size += frame_size;

    callee_parameters = method->size_of_parameters();
    callee_locals = method->max_locals();
    extra_args = 0;
    bytecode_frame = JVMCIENV->get_BytecodePosition_caller(bytecode_frame);
  }
  return size + Deoptimization::last_frame_adjust(0, callee_locals) * BytesPerWord;
C2V_END

C2V_VMENTRY(void, compileToBytecode, (JNIEnv* env, jobject, jobject lambda_form_handle))
  Handle lambda_form = JVMCIENV->asConstant(JVMCIENV->wrap(lambda_form_handle), JVMCI_CHECK);
  if (lambda_form->is_a(SystemDictionary::LambdaForm_klass())) {
    TempNewSymbol compileToBytecode = SymbolTable::new_symbol("compileToBytecode");
    JavaValue result(T_VOID);
    JavaCalls::call_special(&result, lambda_form, SystemDictionary::LambdaForm_klass(), compileToBytecode, vmSymbols::void_method_signature(), CHECK);
  } else {
    JVMCI_THROW_MSG(IllegalArgumentException,
                    err_msg("Unexpected type: %s", lambda_form->klass()->external_name()))
  }
C2V_END

C2V_VMENTRY_0(int, getIdentityHashCode, (JNIEnv* env, jobject, jobject object))
  Handle obj = JVMCIENV->asConstant(JVMCIENV->wrap(object), JVMCI_CHECK_0);
  return obj->identity_hash();
C2V_END

C2V_VMENTRY_0(jboolean, isInternedString, (JNIEnv* env, jobject, jobject object))
  Handle str = JVMCIENV->asConstant(JVMCIENV->wrap(object), JVMCI_CHECK_0);
  if (!java_lang_String::is_instance(str())) {
    return false;
  }
  int len;
  jchar* name = java_lang_String::as_unicode_string(str(), len, CHECK_0);
  return (StringTable::lookup(name, len) != NULL);
C2V_END


C2V_VMENTRY_NULL(jobject, unboxPrimitive, (JNIEnv* env, jobject, jobject object))
  if (object == NULL) {
    JVMCI_THROW_0(NullPointerException);
  }
  Handle box = JVMCIENV->asConstant(JVMCIENV->wrap(object), JVMCI_CHECK_NULL);
  BasicType type = java_lang_boxing_object::basic_type(box());
  jvalue result;
  if (java_lang_boxing_object::get_value(box(), &result) == T_ILLEGAL) {
    return NULL;
  }
  JVMCIObject boxResult = JVMCIENV->create_box(type, &result, JVMCI_CHECK_NULL);
  return JVMCIENV->get_jobject(boxResult);
C2V_END

C2V_VMENTRY_NULL(jobject, boxPrimitive, (JNIEnv* env, jobject, jobject object))
  if (object == NULL) {
    JVMCI_THROW_0(NullPointerException);
  }
  JVMCIObject box = JVMCIENV->wrap(object);
  BasicType type = JVMCIENV->get_box_type(box);
  if (type == T_ILLEGAL) {
    return NULL;
  }
  jvalue value = JVMCIENV->get_boxed_value(type, box);
  JavaValue box_result(T_OBJECT);
  JavaCallArguments jargs;
  Klass* box_klass = NULL;
  Symbol* box_signature = NULL;
#define BOX_CASE(bt, v, argtype, name)           \
  case bt: \
    jargs.push_##argtype(value.v); \
    box_klass = SystemDictionary::name##_klass(); \
    box_signature = vmSymbols::name##_valueOf_signature(); \
    break

  switch (type) {
    BOX_CASE(T_BOOLEAN, z, int, Boolean);
    BOX_CASE(T_BYTE, b, int, Byte);
    BOX_CASE(T_CHAR, c, int, Character);
    BOX_CASE(T_SHORT, s, int, Short);
    BOX_CASE(T_INT, i, int, Integer);
    BOX_CASE(T_LONG, j, long, Long);
    BOX_CASE(T_FLOAT, f, float, Float);
    BOX_CASE(T_DOUBLE, d, double, Double);
    default:
      ShouldNotReachHere();
  }
#undef BOX_CASE

  JavaCalls::call_static(&box_result,
                         box_klass,
                         vmSymbols::valueOf_name(),
                         box_signature, &jargs, CHECK_NULL);
  oop hotspot_box = (oop) box_result.get_jobject();
  JVMCIObject result = JVMCIENV->get_object_constant(hotspot_box, false);
  return JVMCIENV->get_jobject(result);
C2V_END

C2V_VMENTRY_NULL(jobjectArray, getDeclaredConstructors, (JNIEnv* env, jobject, jobject holder))
  if (holder == NULL) {
    JVMCI_THROW_0(NullPointerException);
  }
  Klass* klass = JVMCIENV->asKlass(holder);
  if (!klass->is_instance_klass()) {
    JVMCIObjectArray methods = JVMCIENV->new_ResolvedJavaMethod_array(0, JVMCI_CHECK_NULL);
    return JVMCIENV->get_jobjectArray(methods);
  }

  InstanceKlass* iklass = InstanceKlass::cast(klass);
  // Ensure class is linked
  iklass->link_class(CHECK_NULL);

  GrowableArray<Method*> constructors_array;
  for (int i = 0; i < iklass->methods()->length(); i++) {
    Method* m = iklass->methods()->at(i);
    if (m->is_initializer() && !m->is_static()) {
      constructors_array.append(m);
    }
  }
  JVMCIObjectArray methods = JVMCIENV->new_ResolvedJavaMethod_array(constructors_array.length(), JVMCI_CHECK_NULL);
  for (int i = 0; i < constructors_array.length(); i++) {
    JVMCIObject method = JVMCIENV->get_jvmci_method(constructors_array.at(i), JVMCI_CHECK_NULL);
    JVMCIENV->put_object_at(methods, i, method);
  }
  return JVMCIENV->get_jobjectArray(methods);
C2V_END

C2V_VMENTRY_NULL(jobjectArray, getDeclaredMethods, (JNIEnv* env, jobject, jobject holder))
  if (holder == NULL) {
    JVMCI_THROW_0(NullPointerException);
  }
  Klass* klass = JVMCIENV->asKlass(holder);
  if (!klass->is_instance_klass()) {
    JVMCIObjectArray methods = JVMCIENV->new_ResolvedJavaMethod_array(0, JVMCI_CHECK_NULL);
    return JVMCIENV->get_jobjectArray(methods);
  }

  InstanceKlass* iklass = InstanceKlass::cast(klass);
  // Ensure class is linked
  iklass->link_class(CHECK_NULL);

  GrowableArray<Method*> methods_array;
  for (int i = 0; i < iklass->methods()->length(); i++) {
    Method* m = iklass->methods()->at(i);
    if (!m->is_initializer() && !m->is_overpass()) {
      methods_array.append(m);
    }
  }
  JVMCIObjectArray methods = JVMCIENV->new_ResolvedJavaMethod_array(methods_array.length(), JVMCI_CHECK_NULL);
  for (int i = 0; i < methods_array.length(); i++) {
    JVMCIObject method = JVMCIENV->get_jvmci_method(methods_array.at(i), JVMCI_CHECK_NULL);
    JVMCIENV->put_object_at(methods, i, method);
  }
  return JVMCIENV->get_jobjectArray(methods);
C2V_END

C2V_VMENTRY_NULL(jobject, readFieldValue, (JNIEnv* env, jobject, jobject object, jobject field, jboolean is_volatile))
  if (object == NULL || field == NULL) {
    JVMCI_THROW_0(NullPointerException);
  }
  JVMCIObject field_object = JVMCIENV->wrap(field);
  JVMCIObject java_type = JVMCIENV->get_HotSpotResolvedJavaFieldImpl_type(field_object);
  int modifiers = JVMCIENV->get_HotSpotResolvedJavaFieldImpl_modifiers(field_object);
  Klass* holder = JVMCIENV->asKlass(JVMCIENV->get_HotSpotResolvedJavaFieldImpl_holder(field_object));
  if (!holder->is_instance_klass()) {
    JVMCI_THROW_MSG_0(InternalError, err_msg("Holder %s must be instance klass", holder->external_name()));
  }
  InstanceKlass* ik = InstanceKlass::cast(holder);
  BasicType constant_type;
  if (JVMCIENV->isa_HotSpotResolvedPrimitiveType(java_type)) {
    constant_type = JVMCIENV->kindToBasicType(JVMCIENV->get_HotSpotResolvedPrimitiveType_kind(java_type), JVMCI_CHECK_NULL);
  } else {
    constant_type = T_OBJECT;
  }
  int displacement = JVMCIENV->get_HotSpotResolvedJavaFieldImpl_offset(field_object);
  fieldDescriptor fd;
  if (!ik->find_local_field_from_offset(displacement, (modifiers & JVM_ACC_STATIC) != 0, &fd)) {
    JVMCI_THROW_MSG_0(InternalError, err_msg("Can't find field with displacement %d", displacement));
  }
  JVMCIObject base = JVMCIENV->wrap(object);
  Handle obj;
  if (JVMCIENV->isa_HotSpotObjectConstantImpl(base)) {
    obj = JVMCIENV->asConstant(base, JVMCI_CHECK_NULL);
  } else if (JVMCIENV->isa_HotSpotResolvedObjectTypeImpl(base)) {
    Klass* klass = JVMCIENV->asKlass(base);
    obj = Handle(THREAD, klass->java_mirror());
  } else {
    JVMCI_THROW_MSG_NULL(IllegalArgumentException,
                         err_msg("Unexpected type: %s", JVMCIENV->klass_name(base)));
  }
  jlong value = 0;
  JVMCIObject kind;
  switch (constant_type) {
    case T_OBJECT: {
      oop object = is_volatile ? obj->obj_field_acquire(displacement) : obj->obj_field(displacement);
      JVMCIObject result = JVMCIENV->get_object_constant(object);
      if (result.is_null()) {
        return JVMCIENV->get_jobject(JVMCIENV->get_JavaConstant_NULL_POINTER());
      }
      return JVMCIENV->get_jobject(result);
    }
    case T_FLOAT: {
      float f = is_volatile ? obj->float_field_acquire(displacement) : obj->float_field(displacement);
      JVMCIObject result = JVMCIENV->call_JavaConstant_forFloat(f, JVMCI_CHECK_NULL);
      return JVMCIENV->get_jobject(result);
    }
    case T_DOUBLE: {
      double f = is_volatile ? obj->double_field_acquire(displacement) : obj->double_field(displacement);
      JVMCIObject result = JVMCIENV->call_JavaConstant_forDouble(f, JVMCI_CHECK_NULL);
      return JVMCIENV->get_jobject(result);
    }
    case T_BOOLEAN: value = is_volatile ? obj->bool_field_acquire(displacement) : obj->bool_field(displacement); break;
    case T_BYTE: value = is_volatile ? obj->byte_field_acquire(displacement) : obj->byte_field(displacement); break;
    case T_SHORT: value = is_volatile ? obj->short_field_acquire(displacement) : obj->short_field(displacement); break;
    case T_CHAR: value = is_volatile ? obj->char_field_acquire(displacement) : obj->char_field(displacement); break;
    case T_INT: value = is_volatile ? obj->int_field_acquire(displacement) : obj->int_field(displacement); break;
    case T_LONG: value = is_volatile ? obj->long_field_acquire(displacement) : obj->long_field(displacement); break;
    default:
      ShouldNotReachHere();
  }
  JVMCIObject result = JVMCIENV->call_PrimitiveConstant_forTypeChar(type2char(constant_type), value, JVMCI_CHECK_NULL);
  return JVMCIENV->get_jobject(result);
C2V_END

C2V_VMENTRY_0(jboolean, isInstance, (JNIEnv* env, jobject, jobject holder, jobject object))
  if (object == NULL || holder == NULL) {
    JVMCI_THROW_0(NullPointerException);
  }
  Handle obj = JVMCIENV->asConstant(JVMCIENV->wrap(object), JVMCI_CHECK_0);
  Klass* klass = JVMCIENV->asKlass(JVMCIENV->wrap(holder));
  return obj->is_a(klass);
C2V_END

C2V_VMENTRY_0(jboolean, isAssignableFrom, (JNIEnv* env, jobject, jobject holder, jobject otherHolder))
  if (holder == NULL || otherHolder == NULL) {
    JVMCI_THROW_0(NullPointerException);
  }
  Klass* klass = JVMCIENV->asKlass(JVMCIENV->wrap(holder));
  Klass* otherKlass = JVMCIENV->asKlass(JVMCIENV->wrap(otherHolder));
  return otherKlass->is_subtype_of(klass);
C2V_END

C2V_VMENTRY_0(jboolean, isTrustedForIntrinsics, (JNIEnv* env, jobject, jobject holder))
  if (holder == NULL) {
    JVMCI_THROW_0(NullPointerException);
  }
  InstanceKlass* ik = InstanceKlass::cast(JVMCIENV->asKlass(JVMCIENV->wrap(holder)));
  if (ik->class_loader_data()->is_builtin_class_loader_data()) {
    return true;
  }
  return false;
C2V_END

C2V_VMENTRY_NULL(jobject, asJavaType, (JNIEnv* env, jobject, jobject object))
  if (object == NULL) {
    JVMCI_THROW_0(NullPointerException);
  }
  Handle obj = JVMCIENV->asConstant(JVMCIENV->wrap(object), JVMCI_CHECK_NULL);
  if (java_lang_Class::is_instance(obj())) {
    if (java_lang_Class::is_primitive(obj())) {
      JVMCIObject type = JVMCIENV->get_jvmci_primitive_type(java_lang_Class::primitive_type(obj()));
      return JVMCIENV->get_jobject(type);
    }
    Klass* klass = java_lang_Class::as_Klass(obj());
    JVMCIKlassHandle klass_handle(THREAD);
    klass_handle = klass;
    JVMCIObject type = JVMCIENV->get_jvmci_type(klass_handle, JVMCI_CHECK_NULL);
    return JVMCIENV->get_jobject(type);
  }
  return NULL;
C2V_END


C2V_VMENTRY_NULL(jobject, asString, (JNIEnv* env, jobject, jobject object))
  if (object == NULL) {
    JVMCI_THROW_0(NullPointerException);
  }
  Handle obj = JVMCIENV->asConstant(JVMCIENV->wrap(object), JVMCI_CHECK_NULL);
  const char* str = java_lang_String::as_utf8_string(obj());
  JVMCIObject result = JVMCIENV->create_string(str, JVMCI_CHECK_NULL);
  return JVMCIENV->get_jobject(result);
C2V_END


C2V_VMENTRY_0(jboolean, equals, (JNIEnv* env, jobject, jobject x, jlong xHandle, jobject y, jlong yHandle))
  if (x == NULL || y == NULL) {
    JVMCI_THROW_0(NullPointerException);
  }
  return JVMCIENV->resolve_handle(xHandle) == JVMCIENV->resolve_handle(yHandle);
C2V_END

C2V_VMENTRY_NULL(jobject, getJavaMirror, (JNIEnv* env, jobject, jobject object))
  if (object == NULL) {
    JVMCI_THROW_0(NullPointerException);
  }
  JVMCIObject base_object = JVMCIENV->wrap(object);
  Handle mirror;
  if (JVMCIENV->isa_HotSpotResolvedObjectTypeImpl(base_object)) {
    mirror = Handle(THREAD, JVMCIENV->asKlass(base_object)->java_mirror());
  } else if (JVMCIENV->isa_HotSpotResolvedPrimitiveType(base_object)) {
    mirror = JVMCIENV->asConstant(JVMCIENV->get_HotSpotResolvedPrimitiveType_mirror(base_object), JVMCI_CHECK_NULL);
  } else {
    JVMCI_THROW_MSG_NULL(IllegalArgumentException,
                         err_msg("Unexpected type: %s", JVMCIENV->klass_name(base_object)));
 }
  JVMCIObject result = JVMCIENV->get_object_constant(mirror());
  return JVMCIENV->get_jobject(result);
C2V_END


C2V_VMENTRY_0(jint, getArrayLength, (JNIEnv* env, jobject, jobject x))
  if (x == NULL) {
    JVMCI_THROW_0(NullPointerException);
  }
  Handle xobj = JVMCIENV->asConstant(JVMCIENV->wrap(x), JVMCI_CHECK_0);
  if (xobj->klass()->is_array_klass()) {
    return arrayOop(xobj())->length();
  }
  return -1;
 C2V_END


C2V_VMENTRY_NULL(jobject, readArrayElement, (JNIEnv* env, jobject, jobject x, int index))
  if (x == NULL) {
    JVMCI_THROW_0(NullPointerException);
  }
  Handle xobj = JVMCIENV->asConstant(JVMCIENV->wrap(x), JVMCI_CHECK_NULL);
  if (xobj->klass()->is_array_klass()) {
    arrayOop array = arrayOop(xobj());
    BasicType element_type = ArrayKlass::cast(array->klass())->element_type();
    if (index < 0 || index >= array->length()) {
      return NULL;
    }
    JVMCIObject result;

    if (element_type == T_OBJECT) {
      result = JVMCIENV->get_object_constant(objArrayOop(xobj())->obj_at(index));
      if (result.is_null()) {
        result = JVMCIENV->get_JavaConstant_NULL_POINTER();
      }
    } else {
      jvalue value;
      switch (element_type) {
        case T_DOUBLE:        value.d = typeArrayOop(xobj())->double_at(index);        break;
        case T_FLOAT:         value.f = typeArrayOop(xobj())->float_at(index);         break;
        case T_LONG:          value.j = typeArrayOop(xobj())->long_at(index);          break;
        case T_INT:           value.i = typeArrayOop(xobj())->int_at(index);            break;
        case T_SHORT:         value.s = typeArrayOop(xobj())->short_at(index);          break;
        case T_CHAR:          value.c = typeArrayOop(xobj())->char_at(index);           break;
        case T_BYTE:          value.b = typeArrayOop(xobj())->byte_at(index);           break;
        case T_BOOLEAN:       value.z = typeArrayOop(xobj())->byte_at(index) & 1;       break;
        default:              ShouldNotReachHere();
      }
      result = JVMCIENV->create_box(element_type, &value, JVMCI_CHECK_NULL);
    }
    assert(!result.is_null(), "must have a value");
    return JVMCIENV->get_jobject(result);
  }
  return NULL;;
C2V_END


C2V_VMENTRY_0(jint, arrayBaseOffset, (JNIEnv* env, jobject, jobject kind))
  if (kind == NULL) {
    JVMCI_THROW_0(NullPointerException);
  }
  BasicType type = JVMCIENV->kindToBasicType(JVMCIENV->wrap(kind), JVMCI_CHECK_0);
  return arrayOopDesc::header_size(type) * HeapWordSize;
C2V_END

C2V_VMENTRY_0(jint, arrayIndexScale, (JNIEnv* env, jobject, jobject kind))
  if (kind == NULL) {
    JVMCI_THROW_0(NullPointerException);
  }
  BasicType type = JVMCIENV->kindToBasicType(JVMCIENV->wrap(kind), JVMCI_CHECK_0);
  return type2aelembytes(type);
C2V_END

C2V_VMENTRY_0(jbyte, getByte, (JNIEnv* env, jobject, jobject x, long displacement))
  if (x == NULL) {
    JVMCI_THROW_0(NullPointerException);
  }
  Handle xobj = JVMCIENV->asConstant(JVMCIENV->wrap(x), JVMCI_CHECK_0);
  return xobj->byte_field(displacement);
}

C2V_VMENTRY_0(jshort, getShort, (JNIEnv* env, jobject, jobject x, long displacement))
  if (x == NULL) {
    JVMCI_THROW_0(NullPointerException);
  }
  Handle xobj = JVMCIENV->asConstant(JVMCIENV->wrap(x), JVMCI_CHECK_0);
  return xobj->short_field(displacement);
}

C2V_VMENTRY_0(jint, getInt, (JNIEnv* env, jobject, jobject x, long displacement))
  if (x == NULL) {
    JVMCI_THROW_0(NullPointerException);
  }
  Handle xobj = JVMCIENV->asConstant(JVMCIENV->wrap(x), JVMCI_CHECK_0);
  return xobj->int_field(displacement);
}

C2V_VMENTRY_0(jlong, getLong, (JNIEnv* env, jobject, jobject x, long displacement))
  if (x == NULL) {
    JVMCI_THROW_0(NullPointerException);
  }
  Handle xobj = JVMCIENV->asConstant(JVMCIENV->wrap(x), JVMCI_CHECK_0);
  return xobj->long_field(displacement);
}

C2V_VMENTRY_NULL(jobject, getObject, (JNIEnv* env, jobject, jobject x, long displacement))
  if (x == NULL) {
    JVMCI_THROW_0(NullPointerException);
  }
  Handle xobj = JVMCIENV->asConstant(JVMCIENV->wrap(x), JVMCI_CHECK_0);
  oop res = xobj->obj_field(displacement);
  JVMCIObject result = JVMCIENV->get_object_constant(res);
  return JVMCIENV->get_jobject(result);
}

C2V_VMENTRY(void, deleteGlobalHandle, (JNIEnv* env, jobject, jlong h))
  jobject handle = (jobject)(address)h;
  if (handle != NULL) {
    assert(JVMCI::is_global_handle(handle), "Invalid delete of global JNI handle");
    *((oop*)handle) = NULL; // Mark the handle as deleted, allocate will reuse it
  }
}

static void requireJVMCINativeLibrary(JVMCI_TRAPS) {
  if (!UseJVMCINativeLibrary) {
    JVMCI_THROW_MSG(UnsupportedOperationException, "JVMCI shared library is not enabled (requires -XX:+UseJVMCINativeLibrary)");
  }
}

static JavaVM* requireNativeLibraryJavaVM(const char* caller, JVMCI_TRAPS) {
  JavaVM* javaVM = JVMCIEnv::get_shared_library_javavm();
  if (javaVM == NULL) {
    JVMCI_THROW_MSG_NULL(IllegalStateException, err_msg("Require JVMCI shared library to be initialized in %s", caller));
  }
  return javaVM;
}

C2V_VMENTRY_NULL(jlongArray, registerNativeMethods, (JNIEnv* env, jobject, jclass mirror))
  requireJVMCINativeLibrary(JVMCI_CHECK_NULL);
  requireInHotSpot("registerNativeMethods", JVMCI_CHECK_NULL);
  void* shared_library = JVMCIEnv::get_shared_library_handle();
  if (shared_library == NULL) {
    // Ensure the JVMCI shared library runtime is initialized.
    JVMCIEnv __peer_jvmci_env__(thread, false, __FILE__, __LINE__);
    JVMCIEnv* peerEnv = &__peer_jvmci_env__;
    HandleMark hm;
    JVMCIRuntime* runtime = JVMCI::compiler_runtime();
    JVMCIObject receiver = runtime->get_HotSpotJVMCIRuntime(peerEnv);
    if (peerEnv->has_pending_exception()) {
      peerEnv->describe_pending_exception(true);
    }
    shared_library = JVMCIEnv::get_shared_library_handle();
    if (shared_library == NULL) {
      JVMCI_THROW_MSG_0(InternalError, "Error initializing JVMCI runtime");
    }
  }

  if (mirror == NULL) {
    JVMCI_THROW_0(NullPointerException);
  }
  Klass* klass = java_lang_Class::as_Klass(JNIHandles::resolve(mirror));
  if (klass == NULL || !klass->is_instance_klass()) {
    JVMCI_THROW_MSG_0(IllegalArgumentException, "clazz is for primitive type");
  }

  InstanceKlass* iklass = InstanceKlass::cast(klass);
  for (int i = 0; i < iklass->methods()->length(); i++) {
    Method* method = iklass->methods()->at(i);
    if (method->is_native()) {

      // Compute argument size
      int args_size = 1                             // JNIEnv
                    + (method->is_static() ? 1 : 0) // class for static methods
                    + method->size_of_parameters(); // actual parameters

      // 1) Try JNI short style
      stringStream st;
      char* pure_name = NativeLookup::pure_jni_name(method);
      os::print_jni_name_prefix_on(&st, args_size);
      st.print_raw(pure_name);
      os::print_jni_name_suffix_on(&st, args_size);
      char* jni_name = st.as_string();

      address entry = (address) os::dll_lookup(shared_library, jni_name);
      if (entry == NULL) {
        // 2) Try JNI long style
        st.reset();
        char* long_name = NativeLookup::long_jni_name(method);
        os::print_jni_name_prefix_on(&st, args_size);
        st.print_raw(pure_name);
        st.print_raw(long_name);
        os::print_jni_name_suffix_on(&st, args_size);
        char* jni_long_name = st.as_string();
        entry = (address) os::dll_lookup(shared_library, jni_long_name);
        if (entry == NULL) {
          JVMCI_THROW_MSG_0(UnsatisfiedLinkError, err_msg("%s [neither %s nor %s exist in %s]",
              method->name_and_sig_as_C_string(),
              jni_name, jni_long_name, JVMCIEnv::get_shared_library_path()));
        }
      }

      if (method->has_native_function() && entry != method->native_function()) {
        JVMCI_THROW_MSG_0(UnsatisfiedLinkError, err_msg("%s [cannot re-link from " PTR_FORMAT " to " PTR_FORMAT "]",
            method->name_and_sig_as_C_string(), p2i(method->native_function()), p2i(entry)));
      }
      method->set_native_function(entry, Method::native_bind_event_is_interesting);
      if (PrintJNIResolving) {
        tty->print_cr("[Dynamic-linking native method %s.%s ... JNI]",
          method->method_holder()->external_name(),
          method->name()->as_C_string());
      }
    }
  }

  JavaVM* javaVM = JVMCIEnv::get_shared_library_javavm();
  JVMCIPrimitiveArray result = JVMCIENV->new_longArray(4, JVMCI_CHECK_NULL);
  JVMCIENV->put_long_at(result, 0, (jlong) (address) javaVM);
  JVMCIENV->put_long_at(result, 1, (jlong) (address) javaVM->functions->reserved0);
  JVMCIENV->put_long_at(result, 2, (jlong) (address) javaVM->functions->reserved1);
  JVMCIENV->put_long_at(result, 3, (jlong) (address) javaVM->functions->reserved2);
  return (jlongArray) JVMCIENV->get_jobject(result);
}

C2V_VMENTRY_PREFIX(jboolean, isCurrentThreadAttached, (JNIEnv* env, jobject c2vm))
  if (base_thread == NULL) {
    // Called from unattached JVMCI shared library thread
    return false;
  }
  JVMCITraceMark jtm("isCurrentThreadAttached");
  assert(base_thread->is_Java_thread(), "just checking");
  JavaThread* thread = (JavaThread*) base_thread;
  if (thread->jni_environment() == env) {
    C2V_BLOCK(jboolean, isCurrentThreadAttached, (JNIEnv* env, jobject))
    requireJVMCINativeLibrary(JVMCI_CHECK_0);
    JavaVM* javaVM = requireNativeLibraryJavaVM("isCurrentThreadAttached", JVMCI_CHECK_0);
    JNIEnv* peerEnv;
    return javaVM->GetEnv((void**)&peerEnv, JNI_VERSION_1_2) == JNI_OK;
  }
  return true;
C2V_END

C2V_VMENTRY_PREFIX(jboolean, attachCurrentThread, (JNIEnv* env, jobject c2vm, jboolean as_daemon))
  if (base_thread == NULL) {
    // Called from unattached JVMCI shared library thread
    extern struct JavaVM_ main_vm;
    JNIEnv* hotspotEnv;
    jint res = as_daemon ? main_vm.AttachCurrentThreadAsDaemon((void**)&hotspotEnv, NULL) :
                           main_vm.AttachCurrentThread((void**)&hotspotEnv, NULL);
    if (res != JNI_OK) {
      JNI_THROW_("attachCurrentThread", InternalError, err_msg("Trying to attach thread returned %d", res), false);
    }
    return true;
  }
  JVMCITraceMark jtm("attachCurrentThread");
  assert(base_thread->is_Java_thread(), "just checking");\
  JavaThread* thread = (JavaThread*) base_thread;
  if (thread->jni_environment() == env) {
    // Called from HotSpot
    C2V_BLOCK(jboolean, attachCurrentThread, (JNIEnv* env, jobject, jboolean))
    requireJVMCINativeLibrary(JVMCI_CHECK_0);
    JavaVM* javaVM = requireNativeLibraryJavaVM("attachCurrentThread", JVMCI_CHECK_0);
    JavaVMAttachArgs attach_args;
    attach_args.version = JNI_VERSION_1_2;
    attach_args.name = thread->name();
    attach_args.group = NULL;
    JNIEnv* peerEnv;
    if (javaVM->GetEnv((void**)&peerEnv, JNI_VERSION_1_2) == JNI_OK) {
      return false;
    }
    jint res = as_daemon ? javaVM->AttachCurrentThreadAsDaemon((void**)&peerEnv, &attach_args) :
                           javaVM->AttachCurrentThread((void**)&peerEnv, &attach_args);
    if (res == JNI_OK) {
      guarantee(peerEnv != NULL, "must be");
      return true;
    }
    JVMCI_THROW_MSG_0(InternalError, err_msg("Error %d while attaching %s", res, attach_args.name));
  }
  // Called from JVMCI shared library
  return false;
C2V_END

C2V_VMENTRY_PREFIX(void, detachCurrentThread, (JNIEnv* env, jobject c2vm))
  if (base_thread == NULL) {
    // Called from unattached JVMCI shared library thread
    JNI_THROW("detachCurrentThread", IllegalStateException, err_msg("Cannot detach non-attached thread"));
  }
  JVMCITraceMark jtm("detachCurrentThread");
  assert(base_thread->is_Java_thread(), "just checking");\
  JavaThread* thread = (JavaThread*) base_thread;
  if (thread->jni_environment() == env) {
    // Called from HotSpot
    C2V_BLOCK(void, detachCurrentThread, (JNIEnv* env, jobject))
    requireJVMCINativeLibrary(JVMCI_CHECK);
    requireInHotSpot("detachCurrentThread", JVMCI_CHECK);
    JavaVM* javaVM = requireNativeLibraryJavaVM("detachCurrentThread", JVMCI_CHECK);
    JNIEnv* peerEnv;
    if (javaVM->GetEnv((void**)&peerEnv, JNI_VERSION_1_2) != JNI_OK) {
      JVMCI_THROW_MSG(IllegalStateException, err_msg("Cannot detach non-attached thread: %s", thread->name()));
    }
    jint res = javaVM->DetachCurrentThread();
    if (res != JNI_OK) {
      JVMCI_THROW_MSG(InternalError, err_msg("Error %d while attaching %s", res, thread->name()));
    }
  } else {
    // Called from attached JVMCI shared library thread
    extern struct JavaVM_ main_vm;
    jint res = main_vm.DetachCurrentThread();
    if (res != JNI_OK) {
      JNI_THROW("detachCurrentThread", InternalError, err_msg("Cannot detach non-attached thread"));
    }
  }
C2V_END

C2V_VMENTRY_0(jlong, translate, (JNIEnv* env, jobject, jobject obj_handle))
  requireJVMCINativeLibrary(JVMCI_CHECK_0);
  if (obj_handle == NULL) {
    return 0L;
  }
  JVMCIEnv __peer_jvmci_env__(thread, !JVMCIENV->is_hotspot(), __FILE__, __LINE__);
  JVMCIEnv* peerEnv = &__peer_jvmci_env__;
  JVMCIEnv* thisEnv = JVMCIENV;

  JVMCIObject obj = thisEnv->wrap(obj_handle);
  JVMCIObject result;
  if (thisEnv->isa_HotSpotResolvedJavaMethodImpl(obj)) {
    Method* method = thisEnv->asMethod(obj);
    result = peerEnv->get_jvmci_method(method, JVMCI_CHECK_0);
  } else if (thisEnv->isa_HotSpotResolvedObjectTypeImpl(obj)) {
    Klass* klass = thisEnv->asKlass(obj);
    JVMCIKlassHandle klass_handle(THREAD);
    klass_handle = klass;
    result = peerEnv->get_jvmci_type(klass_handle, JVMCI_CHECK_0);
  } else if (thisEnv->isa_HotSpotResolvedPrimitiveType(obj)) {
    BasicType type = JVMCIENV->kindToBasicType(JVMCIENV->get_HotSpotResolvedPrimitiveType_kind(obj), JVMCI_CHECK_0);
    result = peerEnv->get_jvmci_primitive_type(type);
  } else if (thisEnv->isa_IndirectHotSpotObjectConstantImpl(obj) ||
             thisEnv->isa_DirectHotSpotObjectConstantImpl(obj)) {
    Handle constant = thisEnv->asConstant(obj, JVMCI_CHECK_0);
    result = peerEnv->get_object_constant(constant());
  } else if (thisEnv->isa_HotSpotNmethod(obj)) {
    nmethod* nm = thisEnv->asNmethod(obj);
    if (nm != NULL) {
      JVMCINMethodData* data = nm->jvmci_nmethod_data();
      if (data != NULL) {
        if (peerEnv->is_hotspot()) {
          // Only the mirror in the HotSpot heap is accessible
          // through JVMCINMethodData
          oop nmethod_mirror = data->get_nmethod_mirror(nm);
          if (nmethod_mirror != NULL) {
            result = HotSpotJVMCI::wrap(nmethod_mirror);
          }
        }
      }
    }
    if (result.is_null()) {
      JVMCIObject methodObject = thisEnv->get_HotSpotNmethod_method(obj);
      methodHandle mh = thisEnv->asMethod(methodObject);
      jboolean isDefault = thisEnv->get_HotSpotNmethod_isDefault(obj);
      jlong compileIdSnapshot = thisEnv->get_HotSpotNmethod_compileIdSnapshot(obj);
      JVMCIObject name_string = thisEnv->get_InstalledCode_name(obj);
      const char* cstring = name_string.is_null() ? NULL : thisEnv->as_utf8_string(name_string);
      // Create a new HotSpotNmethod instance in the peer runtime
      result = peerEnv->new_HotSpotNmethod(mh(), cstring, isDefault, compileIdSnapshot, JVMCI_CHECK_0);
      if (nm == NULL) {
        // nmethod must have been unloaded
      } else {
        // Link the new HotSpotNmethod to the nmethod
        peerEnv->initialize_installed_code(result, nm, JVMCI_CHECK_0);
        // Only HotSpotNmethod instances in the HotSpot heap are tracked directly by the runtime.
        if (peerEnv->is_hotspot()) {
          JVMCINMethodData* data = nm->jvmci_nmethod_data();
          if (data == NULL) {
            JVMCI_THROW_MSG_0(IllegalArgumentException, "Cannot set HotSpotNmethod mirror for default nmethod");
          }
          if (data->get_nmethod_mirror(nm) != NULL) {
            JVMCI_THROW_MSG_0(IllegalArgumentException, "Cannot overwrite existing HotSpotNmethod mirror for nmethod");
          }
          oop nmethod_mirror = HotSpotJVMCI::resolve(result);
          data->set_nmethod_mirror(nm, nmethod_mirror);
        }
      }
    }
  } else {
    JVMCI_THROW_MSG_0(IllegalArgumentException,
                err_msg("Cannot translate object of type: %s", thisEnv->klass_name(obj)));
  }
  return (jlong) peerEnv->make_global(result).as_jobject();
}

C2V_VMENTRY_NULL(jobject, unhand, (JNIEnv* env, jobject, jlong obj_handle))
  requireJVMCINativeLibrary(JVMCI_CHECK_NULL);
  if (obj_handle == 0L) {
    return NULL;
  }
  jobject global_handle = (jobject) obj_handle;
  JVMCIObject global_handle_obj = JVMCIENV->wrap((jobject) obj_handle);
  jobject result = JVMCIENV->make_local(global_handle_obj).as_jobject();

  JVMCIENV->destroy_global(global_handle_obj);
  return result;
}

C2V_VMENTRY(void, updateHotSpotNmethod, (JNIEnv* env, jobject, jobject code_handle))
  JVMCIObject code = JVMCIENV->wrap(code_handle);
  // Execute this operation for the side effect of updating the InstalledCode state
  JVMCIENV->asNmethod(code);
}

C2V_VMENTRY_NULL(jbyteArray, getCode, (JNIEnv* env, jobject, jobject code_handle))
  JVMCIObject code = JVMCIENV->wrap(code_handle);
  CodeBlob* cb = JVMCIENV->asCodeBlob(code);
  if (cb == NULL) {
    return NULL;
  }
  int code_size = cb->code_size();
  JVMCIPrimitiveArray result = JVMCIENV->new_byteArray(code_size, JVMCI_CHECK_NULL);
  JVMCIENV->copy_bytes_from((jbyte*) cb->code_begin(), result, 0, code_size);
  return JVMCIENV->get_jbyteArray(result);
}

C2V_VMENTRY_NULL(jobject, asReflectionExecutable, (JNIEnv* env, jobject, jobject jvmci_method))
  requireInHotSpot("asReflectionExecutable", JVMCI_CHECK_NULL);
  methodHandle m = JVMCIENV->asMethod(jvmci_method);
  oop executable;
  if (m->is_initializer()) {
    if (m->is_static_initializer()) {
      JVMCI_THROW_MSG_NULL(IllegalArgumentException,
          "Cannot create java.lang.reflect.Method for class initializer");
    }
    executable = Reflection::new_constructor(m, CHECK_NULL);
  } else {
    executable = Reflection::new_method(m, false, CHECK_NULL);
  }
  return JNIHandles::make_local(THREAD, executable);
}

C2V_VMENTRY_NULL(jobject, asReflectionField, (JNIEnv* env, jobject, jobject jvmci_type, jint index))
  requireInHotSpot("asReflectionField", JVMCI_CHECK_NULL);
  Klass* klass = JVMCIENV->asKlass(jvmci_type);
  if (!klass->is_instance_klass()) {
    JVMCI_THROW_MSG_NULL(IllegalArgumentException,
        err_msg("Expected non-primitive type, got %s", klass->external_name()));
  }
  InstanceKlass* iklass = InstanceKlass::cast(klass);
  Array<u2>* fields = iklass->fields();
  if (index < 0 ||index > fields->length()) {
    JVMCI_THROW_MSG_NULL(IllegalArgumentException,
        err_msg("Field index %d out of bounds for %s", index, klass->external_name()));
  }
  fieldDescriptor fd(iklass, index);
  oop reflected = Reflection::new_field(&fd, CHECK_NULL);
  return JNIHandles::make_local(env, reflected);
}

C2V_VMENTRY_NULL(jobjectArray, getFailedSpeculations, (JNIEnv* env, jobject, jlong failed_speculations_address, jobjectArray current))
  FailedSpeculation* head = *((FailedSpeculation**)(address) failed_speculations_address);
  int result_length = 0;
  for (FailedSpeculation* fs = head; fs != NULL; fs = fs->next()) {
    result_length++;
  }
  int current_length = 0;
  JVMCIObjectArray current_array = NULL;
  if (current != NULL) {
    current_array = JVMCIENV->wrap(current);
    current_length = JVMCIENV->get_length(current_array);
    if (current_length == result_length) {
      // No new failures
      return current;
    }
  }
  JVMCIObjectArray result = JVMCIENV->new_byte_array_array(result_length, JVMCI_CHECK_NULL);
  int result_index = 0;
  for (FailedSpeculation* fs = head; result_index < result_length; fs = fs->next()) {
    assert(fs != NULL, "npe");
    JVMCIPrimitiveArray entry;
    if (result_index < current_length) {
      entry = (JVMCIPrimitiveArray) JVMCIENV->get_object_at(current_array, result_index);
    } else {
      entry = JVMCIENV->new_byteArray(fs->data_len(), JVMCI_CHECK_NULL);
      JVMCIENV->copy_bytes_from((jbyte*) fs->data(), entry, 0, fs->data_len());
    }
    JVMCIENV->put_object_at(result, result_index++, entry);
  }
  return JVMCIENV->get_jobjectArray(result);
}

C2V_VMENTRY_0(jlong, getFailedSpeculationsAddress, (JNIEnv* env, jobject, jobject jvmci_method))
  methodHandle method = JVMCIENV->asMethod(jvmci_method);
  MethodData* method_data = method->method_data();
  if (method_data == NULL) {
    ClassLoaderData* loader_data = method->method_holder()->class_loader_data();
    method_data = MethodData::allocate(loader_data, method, CHECK_0);
    method->set_method_data(method_data);
  }
  return (jlong) method_data->get_failed_speculations_address();
}

C2V_VMENTRY(void, releaseFailedSpeculations, (JNIEnv* env, jobject, jlong failed_speculations_address))
  FailedSpeculation::free_failed_speculations((FailedSpeculation**)(address) failed_speculations_address);
}

C2V_VMENTRY_0(bool, addFailedSpeculation, (JNIEnv* env, jobject, jlong failed_speculations_address, jbyteArray speculation_obj))
  JVMCIPrimitiveArray speculation_handle = JVMCIENV->wrap(speculation_obj);
  int speculation_len = JVMCIENV->get_length(speculation_handle);
  char* speculation = NEW_RESOURCE_ARRAY(char, speculation_len);
  JVMCIENV->copy_bytes_to(speculation_handle, (jbyte*) speculation, 0, speculation_len);
  return FailedSpeculation::add_failed_speculation(NULL, (FailedSpeculation**)(address) failed_speculations_address, (address) speculation, speculation_len);
}

#define CC (char*)  /*cast a literal from (const char*)*/
#define FN_PTR(f) CAST_FROM_FN_PTR(void*, &(c2v_ ## f))

#define STRING                  "Ljava/lang/String;"
#define OBJECT                  "Ljava/lang/Object;"
#define CLASS                   "Ljava/lang/Class;"
#define OBJECTCONSTANT          "Ljdk/vm/ci/hotspot/HotSpotObjectConstantImpl;"
#define HANDLECONSTANT          "Ljdk/vm/ci/hotspot/IndirectHotSpotObjectConstantImpl;"
#define EXECUTABLE              "Ljava/lang/reflect/Executable;"
#define STACK_TRACE_ELEMENT     "Ljava/lang/StackTraceElement;"
#define INSTALLED_CODE          "Ljdk/vm/ci/code/InstalledCode;"
#define TARGET_DESCRIPTION      "Ljdk/vm/ci/code/TargetDescription;"
#define BYTECODE_FRAME          "Ljdk/vm/ci/code/BytecodeFrame;"
#define JAVACONSTANT            "Ljdk/vm/ci/meta/JavaConstant;"
#define INSPECTED_FRAME_VISITOR "Ljdk/vm/ci/code/stack/InspectedFrameVisitor;"
#define RESOLVED_METHOD         "Ljdk/vm/ci/meta/ResolvedJavaMethod;"
#define HS_RESOLVED_METHOD      "Ljdk/vm/ci/hotspot/HotSpotResolvedJavaMethodImpl;"
#define HS_RESOLVED_KLASS       "Ljdk/vm/ci/hotspot/HotSpotResolvedObjectTypeImpl;"
#define HS_RESOLVED_TYPE        "Ljdk/vm/ci/hotspot/HotSpotResolvedJavaType;"
#define HS_RESOLVED_FIELD       "Ljdk/vm/ci/hotspot/HotSpotResolvedJavaField;"
#define HS_INSTALLED_CODE       "Ljdk/vm/ci/hotspot/HotSpotInstalledCode;"
#define HS_NMETHOD              "Ljdk/vm/ci/hotspot/HotSpotNmethod;"
#define HS_CONSTANT_POOL        "Ljdk/vm/ci/hotspot/HotSpotConstantPool;"
#define HS_COMPILED_CODE        "Ljdk/vm/ci/hotspot/HotSpotCompiledCode;"
#define HS_CONFIG               "Ljdk/vm/ci/hotspot/HotSpotVMConfig;"
#define HS_METADATA             "Ljdk/vm/ci/hotspot/HotSpotMetaData;"
#define HS_STACK_FRAME_REF      "Ljdk/vm/ci/hotspot/HotSpotStackFrameReference;"
#define HS_SPECULATION_LOG      "Ljdk/vm/ci/hotspot/HotSpotSpeculationLog;"
#define METASPACE_OBJECT        "Ljdk/vm/ci/hotspot/MetaspaceObject;"
#define REFLECTION_EXECUTABLE   "Ljava/lang/reflect/Executable;"
#define REFLECTION_FIELD        "Ljava/lang/reflect/Field;"
#define METASPACE_METHOD_DATA   "J"

JNINativeMethod CompilerToVM::methods[] = {
  {CC "getBytecode",                                  CC "(" HS_RESOLVED_METHOD ")[B",                                                      FN_PTR(getBytecode)},
  {CC "getExceptionTableStart",                       CC "(" HS_RESOLVED_METHOD ")J",                                                       FN_PTR(getExceptionTableStart)},
  {CC "getExceptionTableLength",                      CC "(" HS_RESOLVED_METHOD ")I",                                                       FN_PTR(getExceptionTableLength)},
  {CC "findUniqueConcreteMethod",                     CC "(" HS_RESOLVED_KLASS HS_RESOLVED_METHOD ")" HS_RESOLVED_METHOD,                   FN_PTR(findUniqueConcreteMethod)},
  {CC "getImplementor",                               CC "(" HS_RESOLVED_KLASS ")" HS_RESOLVED_KLASS,                                       FN_PTR(getImplementor)},
  {CC "getStackTraceElement",                         CC "(" HS_RESOLVED_METHOD "I)" STACK_TRACE_ELEMENT,                                   FN_PTR(getStackTraceElement)},
  {CC "methodIsIgnoredBySecurityStackWalk",           CC "(" HS_RESOLVED_METHOD ")Z",                                                       FN_PTR(methodIsIgnoredBySecurityStackWalk)},
  {CC "setNotInlinableOrCompilable",                  CC "(" HS_RESOLVED_METHOD ")V",                                                       FN_PTR(setNotInlinableOrCompilable)},
  {CC "isCompilable",                                 CC "(" HS_RESOLVED_METHOD ")Z",                                                       FN_PTR(isCompilable)},
  {CC "hasNeverInlineDirective",                      CC "(" HS_RESOLVED_METHOD ")Z",                                                       FN_PTR(hasNeverInlineDirective)},
  {CC "shouldInlineMethod",                           CC "(" HS_RESOLVED_METHOD ")Z",                                                       FN_PTR(shouldInlineMethod)},
  {CC "lookupType",                                   CC "(" STRING HS_RESOLVED_KLASS "Z)" HS_RESOLVED_TYPE,                                FN_PTR(lookupType)},
  {CC "lookupClass",                                  CC "(" CLASS ")" HS_RESOLVED_TYPE,                                                    FN_PTR(lookupClass)},
  {CC "lookupNameInPool",                             CC "(" HS_CONSTANT_POOL "I)" STRING,                                                  FN_PTR(lookupNameInPool)},
  {CC "lookupNameAndTypeRefIndexInPool",              CC "(" HS_CONSTANT_POOL "I)I",                                                        FN_PTR(lookupNameAndTypeRefIndexInPool)},
  {CC "lookupSignatureInPool",                        CC "(" HS_CONSTANT_POOL "I)" STRING,                                                  FN_PTR(lookupSignatureInPool)},
  {CC "lookupKlassRefIndexInPool",                    CC "(" HS_CONSTANT_POOL "I)I",                                                        FN_PTR(lookupKlassRefIndexInPool)},
  {CC "lookupKlassInPool",                            CC "(" HS_CONSTANT_POOL "I)Ljava/lang/Object;",                                       FN_PTR(lookupKlassInPool)},
  {CC "lookupAppendixInPool",                         CC "(" HS_CONSTANT_POOL "I)" OBJECTCONSTANT,                                          FN_PTR(lookupAppendixInPool)},
  {CC "lookupMethodInPool",                           CC "(" HS_CONSTANT_POOL "IB)" HS_RESOLVED_METHOD,                                     FN_PTR(lookupMethodInPool)},
  {CC "constantPoolRemapInstructionOperandFromCache", CC "(" HS_CONSTANT_POOL "I)I",                                                        FN_PTR(constantPoolRemapInstructionOperandFromCache)},
  {CC "resolveConstantInPool",                        CC "(" HS_CONSTANT_POOL "I)" OBJECTCONSTANT,                                          FN_PTR(resolveConstantInPool)},
  {CC "resolvePossiblyCachedConstantInPool",          CC "(" HS_CONSTANT_POOL "I)" OBJECTCONSTANT,                                          FN_PTR(resolvePossiblyCachedConstantInPool)},
  {CC "resolveTypeInPool",                            CC "(" HS_CONSTANT_POOL "I)" HS_RESOLVED_KLASS,                                       FN_PTR(resolveTypeInPool)},
  {CC "resolveFieldInPool",                           CC "(" HS_CONSTANT_POOL "I" HS_RESOLVED_METHOD "B[I)" HS_RESOLVED_KLASS,              FN_PTR(resolveFieldInPool)},
  {CC "resolveInvokeDynamicInPool",                   CC "(" HS_CONSTANT_POOL "I)V",                                                        FN_PTR(resolveInvokeDynamicInPool)},
  {CC "resolveInvokeHandleInPool",                    CC "(" HS_CONSTANT_POOL "I)V",                                                        FN_PTR(resolveInvokeHandleInPool)},
  {CC "isResolvedInvokeHandleInPool",                 CC "(" HS_CONSTANT_POOL "I)I",                                                        FN_PTR(isResolvedInvokeHandleInPool)},
  {CC "resolveMethod",                                CC "(" HS_RESOLVED_KLASS HS_RESOLVED_METHOD HS_RESOLVED_KLASS ")" HS_RESOLVED_METHOD, FN_PTR(resolveMethod)},
  {CC "getSignaturePolymorphicHolders",               CC "()[" STRING,                                                                      FN_PTR(getSignaturePolymorphicHolders)},
  {CC "getVtableIndexForInterfaceMethod",             CC "(" HS_RESOLVED_KLASS HS_RESOLVED_METHOD ")I",                                     FN_PTR(getVtableIndexForInterfaceMethod)},
  {CC "getClassInitializer",                          CC "(" HS_RESOLVED_KLASS ")" HS_RESOLVED_METHOD,                                      FN_PTR(getClassInitializer)},
  {CC "hasFinalizableSubclass",                       CC "(" HS_RESOLVED_KLASS ")Z",                                                        FN_PTR(hasFinalizableSubclass)},
  {CC "getMaxCallTargetOffset",                       CC "(J)J",                                                                            FN_PTR(getMaxCallTargetOffset)},
  {CC "asResolvedJavaMethod",                         CC "(" EXECUTABLE ")" HS_RESOLVED_METHOD,                                             FN_PTR(asResolvedJavaMethod)},
  {CC "getResolvedJavaMethod",                        CC "(" OBJECTCONSTANT "J)" HS_RESOLVED_METHOD,                                        FN_PTR(getResolvedJavaMethod)},
  {CC "getConstantPool",                              CC "(" METASPACE_OBJECT ")" HS_CONSTANT_POOL,                                         FN_PTR(getConstantPool)},
  {CC "getResolvedJavaType0",                         CC "(Ljava/lang/Object;JZ)" HS_RESOLVED_KLASS,                                        FN_PTR(getResolvedJavaType0)},
  {CC "readConfiguration",                            CC "()[" OBJECT,                                                                      FN_PTR(readConfiguration)},
  {CC "installCode",                                  CC "(" TARGET_DESCRIPTION HS_COMPILED_CODE INSTALLED_CODE "J[B)I",                    FN_PTR(installCode)},
  {CC "getMetadata",                                  CC "(" TARGET_DESCRIPTION HS_COMPILED_CODE HS_METADATA ")I",                          FN_PTR(getMetadata)},
  {CC "resetCompilationStatistics",                   CC "()V",                                                                             FN_PTR(resetCompilationStatistics)},
  {CC "disassembleCodeBlob",                          CC "(" INSTALLED_CODE ")" STRING,                                                     FN_PTR(disassembleCodeBlob)},
  {CC "executeHotSpotNmethod",                        CC "([" OBJECT HS_NMETHOD ")" OBJECT,                                                 FN_PTR(executeHotSpotNmethod)},
  {CC "getLineNumberTable",                           CC "(" HS_RESOLVED_METHOD ")[J",                                                      FN_PTR(getLineNumberTable)},
  {CC "getLocalVariableTableStart",                   CC "(" HS_RESOLVED_METHOD ")J",                                                       FN_PTR(getLocalVariableTableStart)},
  {CC "getLocalVariableTableLength",                  CC "(" HS_RESOLVED_METHOD ")I",                                                       FN_PTR(getLocalVariableTableLength)},
  {CC "reprofile",                                    CC "(" HS_RESOLVED_METHOD ")V",                                                       FN_PTR(reprofile)},
  {CC "invalidateHotSpotNmethod",                     CC "(" HS_NMETHOD ")V",                                                               FN_PTR(invalidateHotSpotNmethod)},
  {CC "readUncompressedOop",                          CC "(J)" OBJECTCONSTANT,                                                              FN_PTR(readUncompressedOop)},
  {CC "collectCounters",                              CC "()[J",                                                                            FN_PTR(collectCounters)},
  {CC "allocateCompileId",                            CC "(" HS_RESOLVED_METHOD "I)I",                                                      FN_PTR(allocateCompileId)},
  {CC "isMature",                                     CC "(" METASPACE_METHOD_DATA ")Z",                                                    FN_PTR(isMature)},
  {CC "hasCompiledCodeForOSR",                        CC "(" HS_RESOLVED_METHOD "II)Z",                                                     FN_PTR(hasCompiledCodeForOSR)},
  {CC "getSymbol",                                    CC "(J)" STRING,                                                                      FN_PTR(getSymbol)},
  {CC "iterateFrames",                                CC "([" RESOLVED_METHOD "[" RESOLVED_METHOD "I" INSPECTED_FRAME_VISITOR ")" OBJECT,   FN_PTR(iterateFrames)},
  {CC "materializeVirtualObjects",                    CC "(" HS_STACK_FRAME_REF "Z)V",                                                      FN_PTR(materializeVirtualObjects)},
  {CC "shouldDebugNonSafepoints",                     CC "()Z",                                                                             FN_PTR(shouldDebugNonSafepoints)},
  {CC "writeDebugOutput",                             CC "([BIIZZ)I",                                                                       FN_PTR(writeDebugOutput)},
  {CC "flushDebugOutput",                             CC "()V",                                                                             FN_PTR(flushDebugOutput)},
  {CC "methodDataProfileDataSize",                    CC "(JI)I",                                                                           FN_PTR(methodDataProfileDataSize)},
  {CC "getFingerprint",                               CC "(J)J",                                                                            FN_PTR(getFingerprint)},
  {CC "getHostClass",                                 CC "(" HS_RESOLVED_KLASS ")" HS_RESOLVED_KLASS,                                       FN_PTR(getHostClass)},
  {CC "interpreterFrameSize",                         CC "(" BYTECODE_FRAME ")I",                                                           FN_PTR(interpreterFrameSize)},
  {CC "compileToBytecode",                            CC "(" OBJECTCONSTANT ")V",                                                           FN_PTR(compileToBytecode)},
  {CC "getFlagValue",                                 CC "(" STRING ")" OBJECT,                                                             FN_PTR(getFlagValue)},
  {CC "getObjectAtAddress",                           CC "(J)" OBJECT,                                                                      FN_PTR(getObjectAtAddress)},
  {CC "getInterfaces",                                CC "(" HS_RESOLVED_KLASS ")[" HS_RESOLVED_KLASS,                                      FN_PTR(getInterfaces)},
  {CC "getComponentType",                             CC "(" HS_RESOLVED_KLASS ")" HS_RESOLVED_TYPE,                                        FN_PTR(getComponentType)},
  {CC "ensureInitialized",                            CC "(" HS_RESOLVED_KLASS ")V",                                                        FN_PTR(ensureInitialized)},
  {CC "getIdentityHashCode",                          CC "(" OBJECTCONSTANT ")I",                                                           FN_PTR(getIdentityHashCode)},
  {CC "isInternedString",                             CC "(" OBJECTCONSTANT ")Z",                                                           FN_PTR(isInternedString)},
  {CC "unboxPrimitive",                               CC "(" OBJECTCONSTANT ")" OBJECT,                                                     FN_PTR(unboxPrimitive)},
  {CC "boxPrimitive",                                 CC "(" OBJECT ")" OBJECTCONSTANT,                                                     FN_PTR(boxPrimitive)},
  {CC "getDeclaredConstructors",                      CC "(" HS_RESOLVED_KLASS ")[" RESOLVED_METHOD,                                        FN_PTR(getDeclaredConstructors)},
  {CC "getDeclaredMethods",                           CC "(" HS_RESOLVED_KLASS ")[" RESOLVED_METHOD,                                        FN_PTR(getDeclaredMethods)},
  {CC "readFieldValue",                               CC "(" HS_RESOLVED_KLASS HS_RESOLVED_FIELD "Z)" JAVACONSTANT,                         FN_PTR(readFieldValue)},
  {CC "readFieldValue",                               CC "(" OBJECTCONSTANT HS_RESOLVED_FIELD "Z)" JAVACONSTANT,                            FN_PTR(readFieldValue)},
  {CC "isInstance",                                   CC "(" HS_RESOLVED_KLASS OBJECTCONSTANT ")Z",                                         FN_PTR(isInstance)},
  {CC "isAssignableFrom",                             CC "(" HS_RESOLVED_KLASS HS_RESOLVED_KLASS ")Z",                                      FN_PTR(isAssignableFrom)},
  {CC "isTrustedForIntrinsics",                       CC "(" HS_RESOLVED_KLASS ")Z",                                                        FN_PTR(isTrustedForIntrinsics)},
  {CC "asJavaType",                                   CC "(" OBJECTCONSTANT ")" HS_RESOLVED_TYPE,                                           FN_PTR(asJavaType)},
  {CC "asString",                                     CC "(" OBJECTCONSTANT ")" STRING,                                                     FN_PTR(asString)},
  {CC "equals",                                       CC "(" OBJECTCONSTANT "J" OBJECTCONSTANT "J)Z",                                       FN_PTR(equals)},
  {CC "getJavaMirror",                                CC "(" HS_RESOLVED_TYPE ")" OBJECTCONSTANT,                                           FN_PTR(getJavaMirror)},
  {CC "getArrayLength",                               CC "(" OBJECTCONSTANT ")I",                                                           FN_PTR(getArrayLength)},
  {CC "readArrayElement",                             CC "(" OBJECTCONSTANT "I)Ljava/lang/Object;",                                         FN_PTR(readArrayElement)},
  {CC "arrayBaseOffset",                              CC "(Ljdk/vm/ci/meta/JavaKind;)I",                                                    FN_PTR(arrayBaseOffset)},
  {CC "arrayIndexScale",                              CC "(Ljdk/vm/ci/meta/JavaKind;)I",                                                    FN_PTR(arrayIndexScale)},
  {CC "getByte",                                      CC "(" OBJECTCONSTANT "J)B",                                                          FN_PTR(getByte)},
  {CC "getShort",                                     CC "(" OBJECTCONSTANT "J)S",                                                          FN_PTR(getShort)},
  {CC "getInt",                                       CC "(" OBJECTCONSTANT "J)I",                                                          FN_PTR(getInt)},
  {CC "getLong",                                      CC "(" OBJECTCONSTANT "J)J",                                                          FN_PTR(getLong)},
  {CC "getObject",                                    CC "(" OBJECTCONSTANT "J)" OBJECTCONSTANT,                                            FN_PTR(getObject)},
  {CC "deleteGlobalHandle",                           CC "(J)V",                                                                            FN_PTR(deleteGlobalHandle)},
  {CC "registerNativeMethods",                        CC "(" CLASS ")[J",                                                                   FN_PTR(registerNativeMethods)},
  {CC "isCurrentThreadAttached",                      CC "()Z",                                                                             FN_PTR(isCurrentThreadAttached)},
  {CC "attachCurrentThread",                          CC "(Z)Z",                                                                            FN_PTR(attachCurrentThread)},
  {CC "detachCurrentThread",                          CC "()V",                                                                             FN_PTR(detachCurrentThread)},
  {CC "translate",                                    CC "(" OBJECT ")J",                                                                   FN_PTR(translate)},
  {CC "unhand",                                       CC "(J)" OBJECT,                                                                      FN_PTR(unhand)},
  {CC "updateHotSpotNmethod",                         CC "(" HS_NMETHOD ")V",                                                               FN_PTR(updateHotSpotNmethod)},
  {CC "getCode",                                      CC "(" HS_INSTALLED_CODE ")[B",                                                       FN_PTR(getCode)},
  {CC "asReflectionExecutable",                       CC "(" HS_RESOLVED_METHOD ")" REFLECTION_EXECUTABLE,                                  FN_PTR(asReflectionExecutable)},
  {CC "asReflectionField",                            CC "(" HS_RESOLVED_KLASS "I)" REFLECTION_FIELD,                                       FN_PTR(asReflectionField)},
  {CC "getFailedSpeculations",                        CC "(J[[B)[[B",                                                                       FN_PTR(getFailedSpeculations)},
  {CC "getFailedSpeculationsAddress",                 CC "(" HS_RESOLVED_METHOD ")J",                                                       FN_PTR(getFailedSpeculationsAddress)},
  {CC "releaseFailedSpeculations",                    CC "(J)V",                                                                            FN_PTR(releaseFailedSpeculations)},
  {CC "addFailedSpeculation",                         CC "(J[B)Z",                                                                          FN_PTR(addFailedSpeculation)},
};

int CompilerToVM::methods_count() {
  return sizeof(methods) / sizeof(JNINativeMethod);
}
