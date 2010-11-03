/* Copyright (c) 2008-2009, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */
   
#include "math.h"
#include "stdlib.h"
#include "time.h"
#include "string.h"
#include "stdio.h"
#include "jni.h"
#include "jni-util.h"
#include "errno.h"
#include "fcntl.h"

#ifdef PLATFORM_WINDOWS

#  include "windows.h"
#  include "winbase.h"
#  include "io.h"
#  include "tchar.h"
#  include "float.h"
#  include "sys/types.h"
#  include "sys/timeb.h"
#  define SO_PREFIX ""
#  define SO_SUFFIX ".dll"

#  ifdef _MSC_VER
#    define snprintf sprintf_s
#    define isnan _isnan
#    define isfinite _finite
#    define strtof strtod
#  endif

#else // not PLATFORM_WINDOWS

#  define SO_PREFIX "lib"
#  ifdef __APPLE__
#    define SO_SUFFIX ".jnilib"
#    include <CoreServices/CoreServices.h>
#  else
#    define SO_SUFFIX ".so"
#  endif
#  include "unistd.h"
#  include "sys/time.h"
#  include "sys/sysctl.h"
#  include "sys/utsname.h"
#  include "sys/wait.h"

#endif // not PLATFORM_WINDOWS

namespace {
#ifdef PLATFORM_WINDOWS
  char* getErrorStr(DWORD err){
    // The poor man's error string, just print the error code 
    char * errStr = (char*) malloc(9 * sizeof(char));
    snprintf(errStr, 9, "%d", (int) err);
    return errStr;
    
    // The better way to do this, if I could figure out how to convert LPTSTR to char*
    //char* errStr;
    //LPTSTR s;
    //if(FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
    //                 FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err, 0, &s, 0, NULL) == 0)
    //{
    //  errStr.Format("Unknown error occurred (%08x)", err);
    //} else {
    //  errStr = s;
    //}
    //return errStr;
  }

  void makePipe(JNIEnv* e, HANDLE p[2])
  {
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = 1;
    sa.lpSecurityDescriptor = 0;
  
    BOOL success = CreatePipe(p, p + 1, &sa, 0);
    if (not success) {
      throwNew(e, "java/io/IOException", getErrorStr(GetLastError()));
    }
  }
  
  int descriptor(JNIEnv* e, HANDLE h)
  {
    int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h), 0);
    if (fd == -1) {
      throwNewErrno(e, "java/io/IOException");
    }
    return fd;
  }
#else
  void makePipe(JNIEnv* e, int p[2])
  {
    if(pipe(p) != 0) {
      throwNewErrno(e, "java/io/IOException");
    }
  }
  
  void safeClose(int &fd)
  {
    if(fd != -1) close(fd);
    fd = -1;
  }
  
  void close(int p[2])
  {
    ::close(p[0]);
    ::close(p[1]);
  }
  
  void clean(JNIEnv* e, jobjectArray command, char** p)
  {
    int i = 0;
    for(char** x = p; *x; ++x, ++i){
      jstring element = (jstring) e->GetObjectArrayElement(command, i);
      e->ReleaseStringUTFChars(element, *x);
    }
    free(p);
  }
#endif  
}

#ifdef PLATFORM_WINDOWS
extern "C" JNIEXPORT void JNICALL 
Java_java_lang_Runtime_exec(JNIEnv* e, jclass, 
                            jobjectArray command, jlongArray process)
{
  
  int size = 0;
  for (int i = 0; i < e->GetArrayLength(command); ++i){
    jstring element = (jstring) e->GetObjectArrayElement(command, i);
    size += e->GetStringUTFLength(element) + 1;
  } 
   
  RUNTIME_ARRAY(char, line, size);
  char* linep = RUNTIME_ARRAY_BODY(line);
  for (int i = 0; i < e->GetArrayLength(command); ++i) {
    if (i) *(linep++) = _T(' ');
    jstring element = (jstring) e->GetObjectArrayElement(command, i);
    const char* s =  e->GetStringUTFChars(element, 0);
#ifdef _MSC_VER
    _tcscpy_s(linep, size - (linep - RUNTIME_ARRAY_BODY(line)), s);
#else
    _tcscpy(linep, s);
#endif
    e->ReleaseStringUTFChars(element, s);
    linep += e->GetStringUTFLength(element);
  }
  *(linep++) = _T('\0');
 
  HANDLE in[] = { 0, 0 };
  HANDLE out[] = { 0, 0 };
  HANDLE err[] = { 0, 0 };
  
  makePipe(e, in);
  SetHandleInformation(in[0], HANDLE_FLAG_INHERIT, 0);
  jlong inDescriptor = static_cast<jlong>(descriptor(e, in[0]));
  if(e->ExceptionCheck()) return;
  e->SetLongArrayRegion(process, 1, 1, &inDescriptor);
  makePipe(e, out);
  SetHandleInformation(out[1], HANDLE_FLAG_INHERIT, 0);
  jlong outDescriptor = static_cast<jlong>(descriptor(e, out[1]));
  if(e->ExceptionCheck()) return;
  e->SetLongArrayRegion(process, 2, 1, &outDescriptor);
  makePipe(e, err);
  SetHandleInformation(err[0], HANDLE_FLAG_INHERIT, 0);
  jlong errDescriptor = static_cast<jlong>(descriptor(e, err[0]));
  if(e->ExceptionCheck()) return;
  e->SetLongArrayRegion(process, 3, 1, &errDescriptor);
  
  PROCESS_INFORMATION pi;
  ZeroMemory(&pi, sizeof(pi));
 
  STARTUPINFO si;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = in[1];
  si.hStdInput = out[0];
  si.hStdError = err[1];
 
  BOOL success = CreateProcess(0, (LPSTR) RUNTIME_ARRAY_BODY(line), 0, 0, 1,
                               CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                               0, 0, &si, &pi);
  
  if (not success) {
    throwNew(e, "java/io/IOException", getErrorStr(GetLastError()));
    return;
  }
  
  jlong pid = reinterpret_cast<jlong>(pi.hProcess);
  e->SetLongArrayRegion(process, 0, 1, &pid);
  
}

extern "C" JNIEXPORT jint JNICALL 
Java_java_lang_Runtime_exitValue(JNIEnv* e, jclass, jlong pid)
{
  DWORD exitCode;
  BOOL success = GetExitCodeProcess(reinterpret_cast<HANDLE>(pid), &exitCode);
  if(not success){
    throwNew(e, "java/lang/Exception", getErrorStr(GetLastError()));
  } else if(exitCode == STILL_ACTIVE){
    throwNew(e, "java/lang/IllegalThreadStateException", "Process is still active");
  }
  return exitCode;
}

extern "C" JNIEXPORT jint JNICALL 
Java_java_lang_Runtime_waitFor(JNIEnv* e, jclass, jlong pid)
{
  DWORD exitCode;
  WaitForSingleObject(reinterpret_cast<HANDLE>(pid), INFINITE);
  BOOL success = GetExitCodeProcess(reinterpret_cast<HANDLE>(pid), &exitCode);
  if(not success){
    throwNew(e, "java/lang/Exception", getErrorStr(GetLastError()));
  }
  return exitCode;
}
#else
extern "C" JNIEXPORT void JNICALL 
Java_java_lang_Runtime_exec(JNIEnv* e, jclass, 
                            jobjectArray command, jlongArray process)
{
  char** argv = static_cast<char**>
    (malloc((e->GetArrayLength(command) + 1) * sizeof(char*)));
  int i;
  for(i = 0; i < e->GetArrayLength(command); i++){
    jstring element = (jstring) e->GetObjectArrayElement(command, i);
    char* s = const_cast<char*>(e->GetStringUTFChars(element, 0));
    argv[i] = s;
  }
  argv[i] = 0;
  
  int in[] = { -1, -1 };
  int out[] = { -1, -1 };
  int err[] = { -1, -1 };
  int msg[] = { -1, -1 };
  
  makePipe(e, in);
  if(e->ExceptionCheck()) return;
  jlong inDescriptor = static_cast<jlong>(in[0]);
  e->SetLongArrayRegion(process, 1, 1, &inDescriptor);
  makePipe(e, out);
  if(e->ExceptionCheck()) return;
  jlong outDescriptor = static_cast<jlong>(out[1]);
  e->SetLongArrayRegion(process, 2, 1, &outDescriptor);
  makePipe(e, err);
  if(e->ExceptionCheck()) return;
  jlong errDescriptor = static_cast<jlong>(err[0]);
  e->SetLongArrayRegion(process, 3, 1, &errDescriptor);
  makePipe(e, msg);
  if(e->ExceptionCheck()) return;
  if(fcntl(msg[1], F_SETFD, FD_CLOEXEC) != 0) {
    throwNewErrno(e, "java/io/IOException");
    return;
  }
  
  pid_t pid = fork();
  switch(pid){
  case -1: // error
    throwNewErrno(e, "java/io/IOException");
    return;
  case 0: { // child
    // Setup stdin, stdout and stderr
    dup2(in[1], 1);
    close(in);
    dup2(out[0], 0);
    close(out);
    dup2(err[1], 2);
    close(err);
    close(msg[0]);
    
    execvp(argv[0], argv);
    
    // Error if here
    char c = errno;
    ssize_t rv UNUSED = write(msg[1], &c, 1);
    exit(127);
  } break;
    
  default: { //parent
    jlong JNIPid = static_cast<jlong>(pid);
    e->SetLongArrayRegion(process, 0, 1, &JNIPid);
    
    safeClose(in[1]);
    safeClose(out[0]);
    safeClose(err[1]);
    safeClose(msg[1]);
      
    char c;
    int r = read(msg[0], &c, 1);
    if(r == -1) {
      throwNewErrno(e, "java/io/IOException");
      return;
    } else if(r) {
      throwNewErrno(e, "java/io/IOException");
      return;
    }
  } break;
  }
  
  safeClose(msg[0]);
  clean(e, command, argv);
  
  fcntl(in[0], F_SETFD, FD_CLOEXEC);
  fcntl(out[1], F_SETFD, FD_CLOEXEC);
  fcntl(err[0], F_SETFD, FD_CLOEXEC);
}

extern "C" JNIEXPORT jint JNICALL 
Java_java_lang_Runtime_exitValue(JNIEnv* e, jclass, jlong pid)
{
  int status;
  pid_t returned = waitpid(pid, &status, WNOHANG);
  if(returned == 0){
    throwNewErrno(e, "java/lang/IllegalThreadStateException");
  } else if(returned == -1){
    throwNewErrno(e, "java/lang/Exception");
  } 
  
  return WEXITSTATUS(status);
}

extern "C" JNIEXPORT jint JNICALL 
Java_java_lang_Runtime_waitFor(JNIEnv*, jclass, jlong pid)
{
  bool finished = false;
  int status;
  int exitCode;
  while(!finished){
    waitpid(pid, &status, 0);
    if(WIFEXITED(status)){
      finished = true;
      exitCode = WEXITSTATUS(status);
    } else if(WIFSIGNALED(status)){
      finished = true;
      exitCode = -1;
    }
  }
  
  return exitCode;
}
#endif

extern "C" JNIEXPORT jstring JNICALL
Java_java_lang_System_getProperty(JNIEnv* e, jclass, jstring name,
                                  jbooleanArray found)
{
  jstring r = 0;
  const char* chars = e->GetStringUTFChars(name, 0);
  if (chars) {
#ifdef PLATFORM_WINDOWS 
    if (strcmp(chars, "line.separator") == 0) {
      r = e->NewStringUTF("\r\n");
    } else if (strcmp(chars, "file.separator") == 0) {
      r = e->NewStringUTF("\\");
    } else if (strcmp(chars, "os.name") == 0) {
      r = e->NewStringUTF("Windows");
    } else if (strcmp(chars, "os.version") == 0) {
      unsigned size = 32;
      RUNTIME_ARRAY(char, buffer, size);
      OSVERSIONINFO OSversion;
      OSversion.dwOSVersionInfoSize=sizeof(OSVERSIONINFO);
      ::GetVersionEx(&OSversion);
      snprintf(RUNTIME_ARRAY_BODY(buffer), size, "%i.%i", (int)OSversion.dwMajorVersion, (int)OSversion.dwMinorVersion);
      r = e->NewStringUTF(RUNTIME_ARRAY_BODY(buffer));
    } else if (strcmp(chars, "os.arch") == 0) {
#ifdef ARCH_x86_32
      r = e->NewStringUTF("x86");
#elif defined ARCH_x86_64
      r = e->NewStringUTF("x86_64");
#elif defined ARCH_powerpc
      r = e->NewStringUTF("ppc");
#elif defined ARCH_arm
      r = e->NewStringUTF("arm");
#endif
    } else if (strcmp(chars, "java.io.tmpdir") == 0) {
      TCHAR buffer[MAX_PATH];
      GetTempPath(MAX_PATH, buffer);
      r = e->NewStringUTF(buffer);
    } else if (strcmp(chars, "user.dir") == 0) {
      TCHAR buffer[MAX_PATH];
      GetCurrentDirectory(MAX_PATH, buffer);
      r = e->NewStringUTF(buffer);
    } else if (strcmp(chars, "user.home") == 0) {
#  ifdef _MSC_VER
      WCHAR buffer[MAX_PATH];
      size_t needed;
      if (_wgetenv_s(&needed, buffer, MAX_PATH, L"USERPROFILE") == 0) {
        r = e->NewString(reinterpret_cast<jchar*>(buffer), lstrlenW(buffer));
      } else {
        r = 0;
      }
#  else
      LPWSTR home = _wgetenv(L"USERPROFILE");
      r = e->NewString(reinterpret_cast<jchar*>(home), lstrlenW(home));
#  endif
    }
#else
    if (strcmp(chars, "line.separator") == 0) {
      r = e->NewStringUTF("\n");
    } else if (strcmp(chars, "file.separator") == 0) {
      r = e->NewStringUTF("/");
    } else if (strcmp(chars, "os.name") == 0) {
#ifdef __APPLE__
      r = e->NewStringUTF("Mac OS X");
#else
      r = e->NewStringUTF("Linux");
#endif
    } else if (strcmp(chars, "os.version") == 0) {
#ifdef __APPLE__
      unsigned size = 32;
      char buffer[size];
#ifdef ARCH_x86_64
      int32_t minorVersion, majorVersion;
#else
      long minorVersion, majorVersion;
#endif
      
      Gestalt(gestaltSystemVersionMajor, &majorVersion);
      Gestalt(gestaltSystemVersionMinor, &minorVersion);
      
      snprintf(buffer, size, "%d.%d", static_cast<int32_t>(majorVersion),
               static_cast<int32_t>(minorVersion));
      r = e->NewStringUTF(buffer);
#else
      struct utsname system_id; 
      uname(&system_id);
      r = e->NewStringUTF(system_id.release);
#endif
    } else if (strcmp(chars, "os.arch") == 0) {
#ifdef ARCH_x86_32
      r = e->NewStringUTF("x86");
#elif defined ARCH_x86_64
      r = e->NewStringUTF("x86_64");
#elif defined ARCH_powerpc
      r = e->NewStringUTF("ppc");
#elif defined ARCH_arm
      r = e->NewStringUTF("arm");
#endif
    } else if (strcmp(chars, "java.io.tmpdir") == 0) {
      r = e->NewStringUTF("/tmp");
    } else if (strcmp(chars, "user.dir") == 0) {
      r = e->NewStringUTF(getenv("PWD"));
    } else if (strcmp(chars, "user.home") == 0) {
      r = e->NewStringUTF(getenv("HOME"));
    }
#endif

    e->ReleaseStringUTFChars(name, chars);
  }

  if (r) {
    jboolean v = true;
    e->SetBooleanArrayRegion(found, 0, 1, &v);
  }

  return r;
}

extern "C" JNIEXPORT jlong JNICALL
Java_java_lang_System_currentTimeMillis(JNIEnv*, jclass)
{
#ifdef PLATFORM_WINDOWS
  // We used to use _ftime here, but that only gives us 1-second
  // resolution on Windows 7.  _ftime_s might work better, but MinGW
  // doesn't have it as of this writing.  So we use this mess instead:
  FILETIME time;
  GetSystemTimeAsFileTime(&time);
  return (((static_cast<jlong>(time.dwHighDateTime) << 32)
           | time.dwLowDateTime) / 10000) - 11644473600000LL;
#else
  timeval tv = { 0, 0 };
  gettimeofday(&tv, 0);
  return (static_cast<jlong>(tv.tv_sec) * 1000) +
    (static_cast<jlong>(tv.tv_usec) / 1000);
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_java_lang_System_doMapLibraryName(JNIEnv* e, jclass, jstring name)
{
  jstring r = 0;
  const char* chars = e->GetStringUTFChars(name, 0);
  if (chars) {
    unsigned nameLength = strlen(chars);
    unsigned size = sizeof(SO_PREFIX) + nameLength + sizeof(SO_SUFFIX);
    RUNTIME_ARRAY(char, buffer, size);
    snprintf
      (RUNTIME_ARRAY_BODY(buffer), size, SO_PREFIX "%s" SO_SUFFIX, chars);
    r = e->NewStringUTF(RUNTIME_ARRAY_BODY(buffer));

    e->ReleaseStringUTFChars(name, chars);
  }
  return r;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_java_lang_Double_isInfinite(JNIEnv*, jclass, jdouble val)
{
  return !isfinite(val);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_java_lang_Double_isNaN(JNIEnv*, jclass, jdouble val)
{
  return isnan(val);
}

extern "C" JNIEXPORT jdouble JNICALL
Java_java_lang_Double_doubleFromString(JNIEnv*e, jclass, jstring s,
				       jintArray numDoublesRead)
{
  const char* chars = e->GetStringUTFChars(s, 0);
  double d = 0.0;
  jint numRead = 0;

  if (chars) {
    char* lastRead;
    d = strtod(chars, &lastRead);
    if ((lastRead != chars) && ((chars + strlen(chars)) == lastRead)) {
      numRead = 1;
    }
    e->ReleaseStringUTFChars(s, chars);
  }
  e->SetIntArrayRegion(numDoublesRead, 0, 1, &numRead);
  return d;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_java_lang_Float_isInfinite(JNIEnv*, jclass, jfloat val)
{
  return !isfinite(val);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_java_lang_Float_isNaN(JNIEnv*, jclass, jfloat val)
{
  return isnan(val);
}

extern "C" JNIEXPORT jfloat JNICALL
Java_java_lang_Float_floatFromString(JNIEnv*e, jclass, jstring s,
				     jintArray numFloatsRead)
{
  const char* chars = e->GetStringUTFChars(s, 0);
  float f = 0.0;
  jint numRead = 0;

  if (chars) {
    char* lastRead;
    f = strtof(chars, &lastRead);
    if ((lastRead != chars) && ((chars + strlen(chars)) == lastRead)) {
      numRead = 1;
    }
    e->ReleaseStringUTFChars(s, chars);
  }
  e->SetIntArrayRegion(numFloatsRead, 0, 1, &numRead);
  return f;
}

extern "C" JNIEXPORT jdouble JNICALL
Java_java_lang_Math_sin(JNIEnv*, jclass, jdouble val)
{
  return sin(val);
}

extern "C" JNIEXPORT jdouble JNICALL
Java_java_lang_Math_cos(JNIEnv*, jclass, jdouble val)
{
  return cos(val);
}

extern "C" JNIEXPORT jdouble JNICALL
Java_java_lang_Math_sqrt(JNIEnv*, jclass, jdouble val)
{
  return sqrt(val);
}

extern "C" JNIEXPORT jdouble JNICALL
Java_java_lang_Math_pow(JNIEnv*, jclass, jdouble val, jdouble exp)
{
  return pow(val, exp);
}

extern "C" JNIEXPORT jdouble JNICALL
Java_java_lang_Math_floor(JNIEnv*, jclass, jdouble val)
{
  return floor(val);
}

extern "C" JNIEXPORT jdouble JNICALL
Java_java_lang_Math_ceil(JNIEnv*, jclass, jdouble val)
{
  return ceil(val);
}

extern "C" JNIEXPORT jint JNICALL
Java_java_lang_Double_fillBufferWithDouble(JNIEnv* e, jclass, jdouble val,
					   jbyteArray buffer, jint bufferSize) {
  jboolean isCopy;
  jbyte* buf = e->GetByteArrayElements(buffer, &isCopy);
  jint count = snprintf(reinterpret_cast<char*>(buf), bufferSize, "%g", val);
  e->ReleaseByteArrayElements(buffer, buf, 0);
  return count;
}
