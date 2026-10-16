/* Play-test reports: what the game was doing when it crashed, stopped showing frames, or the
 * player marked a moment. Each report is a folder in GOW3_REPORT_DIR (tools/probe_uncharted.py:
 * analysis/reports) named "<date> <time> <game> <kind>", holding report.txt (and minidump.dmp
 * after a crash); the start script adds the end of the log and a screenshot.
 *
 * - Crash (probe.c's fatal_exception, or abort()): the fault and what lies at its address, the
 *   thread and its guest name, the call stack as module+offset (host code unwound with its unwind
 *   tables, guest code by its frame pointers, as gow3_profiler.cpp does), game return addresses
 *   found on the stack, registers and the bytes at the instruction.
 * - Freeze: no new frame for GOW3_FREEZE_SECONDS (default 20, 0 turns it off) once the game has
 *   shown 600: every thread's call stack and CPU use, once per freeze. The game keeps running.
 * - Mark: the same as a freeze report, when the player presses GOW3_MARK_KEY (default F8;
 *   runtime_pad.c) at something that looks wrong.
 * Every report starts with the game (GOW3_REPORT_GAME), the time played, the frames shown, the
 * save last written or read (the games name the chapter in it) and the memory in use, and ends
 * with the last 128 events (files opened, saves, marks), newest last.
 *
 * The crash path runs inside the exception handler: it allocates nothing, takes no lock another
 * thread may hold, and reads memory with ReadProcessMemory so that a bad pointer cannot fault
 * again. GOW3_REPORT_TEST=crash:<s> or freeze:<s> makes the pad thread crash or stop after <s>
 * seconds, to test the reports. */
#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#else
#include <sched.h>
#endif
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "runtime.h"
#include "gpu/gow3gpu.h"

#define EVENT_COUNT 128
typedef struct { uint64_t ms; unsigned repeat; char text[176]; } Event;
static Event events[EVENT_COUNT];
static unsigned event_count; /* events recorded; the newest is event_count-1 */
static int event_lock;
static char save_text[128], save_how[16];
static char levels[4][64]; /* the last level packages opened, newest last */
static uint64_t save_ms, start_ms, last_flip_ms;

static uint64_t clock_ms(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;
#endif
}
static uint64_t elapsed_ms(void) { return clock_ms()-start_ms; }
static void lock_events(void) {
    while (__atomic_exchange_n(&event_lock,1,__ATOMIC_ACQUIRE))
#ifdef _WIN32
        SwitchToThread();
#else
        sched_yield();
#endif
}
static void unlock_events(void) { __atomic_store_n(&event_lock,0,__ATOMIC_RELEASE); }

void runtime_event(const char *format, ...) {
    char text[sizeof(events[0].text)];
    va_list args;
    va_start(args,format); vsnprintf(text,sizeof(text),format,args); va_end(args);
    uint64_t ms=elapsed_ms();
    lock_events();
    Event *last=event_count ? &events[(event_count-1)%EVENT_COUNT] : NULL;
    if (last && !strcmp(last->text,text)) { ++last->repeat; last->ms=ms; }
    else { Event *e=&events[event_count%EVENT_COUNT]; e->ms=ms; e->repeat=1; memcpy(e->text,text,sizeof(text)); ++event_count; }
    unlock_events();
}
/* The games' saves name the place in the story ("Campaign 12% Complete - Another Round");
 * the settings saves ("Profile") do not. */
void runtime_report_save(const char *how, const char *subtitle) {
    if (!subtitle || !*subtitle) return;
    runtime_event("save %s: %s",how,subtitle);
    if (strstr(subtitle,"Profile")) return;
    lock_events();
    snprintf(save_text,sizeof(save_text),"%s",subtitle);
    snprintf(save_how,sizeof(save_how),"%s",how);
    save_ms=elapsed_ms();
    unlock_events();
}
/* Level packages (runtime_file.c) name the place: "airport-2.pak". */
void runtime_report_level(const char *name) {
    if (!name || !*name) return;
    lock_events();
    if (strcmp(levels[3],name)) {
        memmove(levels[0],levels[1],sizeof(levels)-sizeof(levels[0]));
        snprintf(levels[3],sizeof(levels[3]),"%s",name);
    }
    unlock_events();
}

#ifdef _WIN32
typedef struct { uintptr_t base; uint64_t size; char name[48]; } ReportModule;
static ReportModule report_modules[24];
static int report_module_count;
typedef BOOL (WINAPI *WriteDumpFn)(HANDLE,DWORD,HANDLE,MINIDUMP_TYPE,PMINIDUMP_EXCEPTION_INFORMATION,
                                   PMINIDUMP_USER_STREAM_INFORMATION,PMINIDUMP_CALLBACK_INFORMATION);
typedef HRESULT (WINAPI *ThreadDescriptionFn)(HANDLE,PWSTR *);
typedef struct { DWORD cb, faults; SIZE_T peak_working_set, working_set, quota[4], pagefile, peak_pagefile; } MemoryCounters;
typedef BOOL (WINAPI *MemoryInfoFn)(HANDLE,MemoryCounters *,DWORD);
static WriteDumpFn write_dump;
static ThreadDescriptionFn thread_description;
static MemoryInfoFn memory_info;
static HANDLE mark_event;
static uint64_t freeze_ms=20000;

void runtime_report_module(const char *name, const void *base, uint64_t size) {
    if (!base || !size || report_module_count==(int)(sizeof(report_modules)/sizeof(*report_modules))) return;
    ReportModule *m=&report_modules[report_module_count++];
    m->base=(uintptr_t)base; m->size=size;
    snprintf(m->name,sizeof(m->name),"%s",name ? name : "game");
}
static int guest_code(uintptr_t address) {
    for (int i=0;i<report_module_count;++i) if (address-report_modules[i].base<report_modules[i].size) return 1;
    return 0;
}
/* <module>+<offset>: guest code by the executable it came from (offset = address in its ELF). */
static void describe(char *out, size_t size, uintptr_t address) {
    for (int i=0;i<report_module_count;++i) {
        const ReportModule *m=&report_modules[i];
        if (address-m->base<m->size) { snprintf(out,size,"%s+0x%llx",m->name,(unsigned long long)(address-m->base)); return; }
    }
    HMODULE module=NULL;
    char name[MAX_PATH];
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)address,&module) &&
        module && GetModuleFileNameA(module,name,sizeof(name))) {
        const char *base=strrchr(name,'\\');
        snprintf(out,size,"%s+0x%llx",base ? base+1 : name,(unsigned long long)(address-(uintptr_t)module));
        return;
    }
    snprintf(out,size,"0x%llx",(unsigned long long)address);
}
static int read_memory(uintptr_t address, void *out, size_t size) {
    SIZE_T done=0;
    return address && ReadProcessMemory(GetCurrentProcess(),(LPCVOID)address,out,size,&done) && done==size;
}
/* Innermost first: host frames by their unwind tables, guest code by its saved frame pointers. */
static int walk(CONTEXT context, uint64_t *pcs, int max) {
    const uint64_t low=context.Rsp, high=context.Rsp+(8ull<<20);
    int depth=0;
    while (depth<max && context.Rip) {
        pcs[depth++]=context.Rip;
        DWORD64 image_base=0;
        PRUNTIME_FUNCTION function=RtlLookupFunctionEntry(context.Rip,&image_base,NULL);
        if (function) {
            void *handler_data=NULL; DWORD64 establisher=0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER,image_base,context.Rip,function,&context,&handler_data,&establisher,NULL);
        } else if (image_base) { /* a leaf function: the return address is on top of the stack */
            uint64_t ret=0;
            if (!read_memory(context.Rsp,&ret,sizeof(ret))) break;
            context.Rip=ret; context.Rsp+=8;
        } else {
            uint64_t frame[2];
            if (context.Rbp<low || context.Rbp>=high || !read_memory(context.Rbp,frame,sizeof(frame))) break;
            context.Rip=frame[1]; context.Rsp=context.Rbp+16; context.Rbp=frame[0];
        }
        if (context.Rsp<low || context.Rsp>=high) break;
    }
    return depth;
}
/* 1 when the bytes before `ret` end in a call instruction (rel32, or FF /2 through a register or
 * memory operand): `ret` is then a return address, not a stray pointer into code. */
static int after_call(uintptr_t ret) {
    unsigned char b[7];
    if (!read_memory(ret-7,b,sizeof(b))) return 0;
    if (b[2]==0xe8) return 1;
    for (int length=2;length<=7;++length) {
        const unsigned char *op=b+7-length, modrm=op[1];
        if (op[0]!=0xff || (modrm&0x38)!=0x10) continue;
        const int mod=modrm>>6, rm=modrm&7, sib=rm==4 && mod!=3;
        const int need=2+sib+(mod==1 ? 1 : mod==2 || (mod==0 && rm==5) ? 4 : 0);
        if (need==length) return 1;
    }
    return 0;
}

typedef struct { HANDLE file; size_t used; char line[1024]; char data[32768]; } Out;
static Out crash_out, watch_out;
static void flush_out(Out *o) {
    DWORD written;
    if (o->used && o->file!=INVALID_HANDLE_VALUE) WriteFile(o->file,o->data,(DWORD)o->used,&written,NULL);
    o->used=0;
}
static void __attribute__((format(printf,2,3))) put(Out *o, const char *format, ...) {
    va_list args;
    va_start(args,format); int n=vsnprintf(o->line,sizeof(o->line),format,args); va_end(args);
    if (n<0) return;
    if ((size_t)n>=sizeof(o->line)) n=(int)sizeof(o->line)-1;
    if (o->used+(size_t)n>sizeof(o->data)) flush_out(o);
    memcpy(o->data+o->used,o->line,(size_t)n);
    o->used+=(size_t)n;
}
static void close_out(Out *o) {
    flush_out(o);
    if (o->file!=INVALID_HANDLE_VALUE) CloseHandle(o->file);
    o->file=INVALID_HANDLE_VALUE;
}
/* Creates "<GOW3_REPORT_DIR>\<date> <time> <short game name> <kind>" and its report.txt. */
static int open_report(Out *o, const char *kind, char *folder, size_t size) {
    const char *root=getenv("GOW3_REPORT_DIR"), *game=getenv("GOW3_REPORT_SHORT");
    if (!root || !*root) root="reports";
    SYSTEMTIME t; GetLocalTime(&t);
    CreateDirectoryA(root,NULL);
    snprintf(folder,size,"%s\\%04hu-%02hu-%02hu %02hu-%02hu-%02hu%s%s %s",root,t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,
             game && *game ? " " : "",game && *game ? game : "",kind);
    if (!CreateDirectoryA(folder,NULL) && GetLastError()!=ERROR_ALREADY_EXISTS) return 0;
    char path[MAX_PATH*2];
    snprintf(path,sizeof(path),"%s\\report.txt",folder);
    o->used=0;
    o->file=CreateFileA(path,GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    return o->file!=INVALID_HANDLE_VALUE;
}
static void duration(char *out, size_t size, uint64_t ms) {
    const uint64_t s=ms/1000;
    if (s>=3600) snprintf(out,size,"%llu:%02llu:%02llu",s/3600,s/60%60,s%60);
    else snprintf(out,size,"%llu:%02llu",s/60,s%60);
}
static const char *game_name(void) {
    const char *game=getenv("GOW3_REPORT_GAME");
    return game && *game ? game : "The game";
}
static void put_header(Out *o, const char *title, uint64_t now) {
    SYSTEMTIME t; GetLocalTime(&t);
    char played[32], ago[32];
    duration(played,sizeof(played),now);
    put(o,"%s\n\n",title);
    const char *exe=getenv("GOW3_REPORT_EXE");
    put(o,"Game:      %s%s%s%s\n",game_name(),exe && *exe ? " (" : "",exe && *exe ? exe : "",exe && *exe ? ")" : "");
    put(o,"When:      %04hu-%02hu-%02hu %02hu:%02hu:%02hu, %s after the start\n",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,played);
    const uint64_t flips=gow3gpu_flip_count(), last=__atomic_load_n(&last_flip_ms,__ATOMIC_RELAXED);
    if (flips) put(o,"Frames:    %llu shown, the last new one %.1f s before this report\n",(unsigned long long)flips,
                   now>last ? (now-last)/1000.0 : 0.0);
    else put(o,"Frames:    none shown yet\n");
    if (save_text[0]) {
        duration(ago,sizeof(ago),now>save_ms ? now-save_ms : 0);
        put(o,"Save:      \"%s\" (%s %s before)\n",save_text,save_how,ago);
    } else put(o,"Save:      none written or read since the start\n");
    if (levels[3][0]) {
        put(o,"Level:     ");
        for (int i=0,first=1;i<4;++i) if (levels[i][0]) { put(o,"%s%s",first ? "" : ", ",levels[i]); first=0; }
        put(o,"  (the last level packages loaded, newest last)\n");
    }
    MemoryCounters m={.cb=sizeof(m)};
    if (memory_info && memory_info(GetCurrentProcess(),&m,sizeof(m)))
        put(o,"Memory:    %.2f GB in use, %.2f GB committed\n",m.working_set/1073741824.0,m.pagefile/1073741824.0);
}
/* The settings that change how the GPU work is emulated (all of them: settings.txt). */
static void put_settings(Out *o) {
    static const char *const keys[]={"GOW3_ASYNC_READBACK","GOW3_READBACKS","GOW3_SYNC_GPU_CONSTANTS","GOW3_DRAW_PIPE",
        "GOW3_VK_RECORD_THREAD","GOW3_ASYNC_SHADERS","GOW3_DMA_SYNC","GOW3_DIRECT_MEMORY_ACCESS","GOW3_VBLANK_HZ",
        "GOW3_DMEM_MB","GOW3_MOVIE_SIZE","GOW3_PIPELINE_CACHE"};
    put(o,"\nSettings\n");
    for (size_t i=0;i<sizeof(keys)/sizeof(*keys);++i) {
        const char *value=getenv(keys[i]);
        put(o,"  %s=%s\n",keys[i],value ? value : "(not set)");
    }
    const char *readback=getenv("GOW3_ASYNC_READBACK");
    put(o,"  GPU readbacks: %s\n",readback && readback[0]=='0' ? "the CPU waits for them" :
        readback && readback[0]=='1' ? "asynchronous, pages read-protected until they arrive" :
        "asynchronous and unprotected (the CPU may read data the GPU has not written back yet)");
}
static void put_events(Out *o, uint64_t now, int locked) {
    put(o,"\nLast events (newest last; time before this report)\n");
    if (locked) lock_events();
    const unsigned total=event_count, first=total>EVENT_COUNT ? total-EVENT_COUNT : 0;
    if (first==total) put(o,"  none\n");
    for (unsigned i=first;i<total;++i) {
        const Event *e=&events[i%EVENT_COUNT];
        char when[32];
        duration(when,sizeof(when),now>e->ms ? now-e->ms : 0);
        if (e->repeat>1) put(o,"  -%-8s %s (%u times)\n",when,e->text,e->repeat);
        else put(o,"  -%-8s %s\n",when,e->text);
    }
    if (locked) unlock_events();
}

static const char *exception_name(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction (ud2: also a failed check in the runtime)";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer division by zero";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "misaligned data";
    case EXCEPTION_BREAKPOINT: return "breakpoint (int3: a failed check)";
    case 0xc0000008: return "invalid handle";
    case 0xc0000374: return "heap corruption";
    case 0xe06d7363: return "C++ exception not caught";
    case 0x40000015: return "abort() was called";
    case 0xe0000017: return "the port stopped the game";
    default: return "exception";
    }
}
/* What lies at a faulting data address: a garbage value, a null pointer, or memory in what state. */
static void describe_target(char *out, size_t size, uintptr_t address) {
    MEMORY_BASIC_INFORMATION m;
    if (address>=0x800000000000ull) { snprintf(out,size,"not a valid user address (garbage, or -1 used as a pointer)"); return; }
    if (address<0x10000) { snprintf(out,size,"a null pointer plus 0x%llx",(unsigned long long)address); return; }
    if (!VirtualQuery((LPCVOID)address,&m,sizeof(m))) { snprintf(out,size,"outside the address space"); return; }
    char where[160];
    describe(where,sizeof(where),address);
    if (m.State==MEM_FREE) snprintf(out,size,"nothing is mapped there (a stale or wrong pointer)");
    else if (m.State==MEM_RESERVE) snprintf(out,size,"reserved but not mapped (part of the PS4 address range the game never mapped, or freed)");
    else if (m.Protect&PAGE_GUARD) snprintf(out,size,"a guard page (%s)",where);
    else if (m.Protect&PAGE_NOACCESS) snprintf(out,size,"mapped with no access: protected by the runtime (GPU page tracking or a readback in flight) (%s)",where);
    else if (m.Protect&(PAGE_READONLY|PAGE_EXECUTE_READ)) snprintf(out,size,"mapped read-only (%s)",where);
    else snprintf(out,size,"mapped and accessible now, protection 0x%lx (%s)",m.Protect,where);
}
static void put_stack_scan(Out *o, uintptr_t rsp) {
    static uint64_t words[512];
    put(o,"\nGame return addresses on the stack (callers, also of functions without a frame pointer)\n");
    int found=0;
    for (unsigned chunk=0;chunk<8 && found<16;++chunk) {
        if (!read_memory(rsp+chunk*4096,words,sizeof(words))) break;
        for (unsigned i=0;i<512 && found<16;++i) {
            const uintptr_t value=(uintptr_t)words[i];
            if (!guest_code(value) || !after_call(value)) continue;
            char where[160];
            describe(where,sizeof(where),value);
            put(o,"  rsp+0x%04x  %s\n",chunk*4096+i*8,where);
            ++found;
        }
    }
    if (!found) put(o,"  none found\n");
}
static void put_code(Out *o, uintptr_t rip) {
    unsigned char before[16], at[16];
    const int have_before=read_memory(rip-sizeof(before),before,sizeof(before)), have_at=read_memory(rip,at,sizeof(at));
    put(o,"\nCode (16 bytes before the instruction | from the instruction)\n  ");
    if (have_before) for (unsigned i=0;i<sizeof(before);++i) put(o,"%02x ",before[i]);
    else put(o,"(unreadable) ");
    put(o,"| ");
    if (have_at) for (unsigned i=0;i<sizeof(at);++i) put(o,"%02x ",at[i]);
    else put(o,"(unreadable)");
    put(o,"\n");
}

/* The minidump is written by a thread of its own, while the crashing thread waits at most 30 s:
 * whatever dbghelp waits for cannot keep the process from ending. */
static struct { EXCEPTION_POINTERS *info; DWORD thread; uintptr_t rip; int added; char path[MAX_PATH*2]; } dump_job;
static BOOL CALLBACK dump_callback(PVOID param, const PMINIDUMP_CALLBACK_INPUT input, PMINIDUMP_CALLBACK_OUTPUT output) {
    (void)param;
    if (input->CallbackType==MemoryCallback) { /* the game code around the instruction: no module holds it */
        if (dump_job.added || !guest_code(dump_job.rip)) return FALSE;
        dump_job.added=1;
        output->MemoryBase=(dump_job.rip&~(uintptr_t)0xfff)-0x1000;
        output->MemorySize=0x3000;
        return TRUE;
    }
    if (input->CallbackType==ReadMemoryFailureCallback) { output->Status=S_OK; return TRUE; }
    return TRUE;
}
static DWORD WINAPI dump_thread(LPVOID unused) {
    (void)unused;
    HANDLE file=CreateFileA(dump_job.path,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if (file==INVALID_HANDLE_VALUE) return 1;
    MINIDUMP_EXCEPTION_INFORMATION exception={dump_job.thread,dump_job.info,FALSE};
    MINIDUMP_CALLBACK_INFORMATION callback={dump_callback,NULL};
    const MINIDUMP_TYPE type=(MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory|MiniDumpWithThreadInfo|
                                            MiniDumpWithUnloadedModules|MiniDumpWithFullMemoryInfo|MiniDumpWithProcessThreadData);
    const BOOL ok=write_dump(GetCurrentProcess(),GetCurrentProcessId(),file,type,&exception,NULL,&callback);
    CloseHandle(file);
    if (!ok) DeleteFileA(dump_job.path);
    return ok ? 0 : 1;
}
static void write_minidump(const char *folder, EXCEPTION_POINTERS *info) {
    const char *env=getenv("GOW3_MINIDUMP");
    if (!write_dump || (env && env[0]=='0')) return;
    snprintf(dump_job.path,sizeof(dump_job.path),"%s\\minidump.dmp",folder);
    dump_job.info=info; dump_job.thread=GetCurrentThreadId(); dump_job.rip=(uintptr_t)info->ContextRecord->Rip;
    HANDLE thread=CreateThread(NULL,0,dump_thread,NULL,0,NULL);
    if (thread) { WaitForSingleObject(thread,30000); CloseHandle(thread); }
}

static int crash_reported;
void runtime_report_crash(void *exception_pointers, const char *note) {
    EXCEPTION_POINTERS *info=exception_pointers;
    if (!info || __atomic_exchange_n(&crash_reported,1,__ATOMIC_ACQ_REL)) return;
    const EXCEPTION_RECORD *record=info->ExceptionRecord;
    const CONTEXT *c=info->ContextRecord;
    static char folder[MAX_PATH*2], title[256], where[160], target[256];
    Out *o=&crash_out;
    if (!open_report(o,"crash",folder,sizeof(folder))) {
        fprintf(stderr,"Crash report: cannot write to %s\n",folder);
        return;
    }
    const uint64_t now=elapsed_ms();
    snprintf(title,sizeof(title),"%s crashed",game_name());
    put_header(o,title,now);
    describe(where,sizeof(where),(uintptr_t)c->Rip);
    put(o,"\nWhat happened\n");
    if (record->ExceptionCode==EXCEPTION_ACCESS_VIOLATION && record->NumberParameters>=2) {
        const uintptr_t address=(uintptr_t)record->ExceptionInformation[1];
        const ULONG_PTR kind=record->ExceptionInformation[0];
        describe_target(target,sizeof(target),address);
        put(o,"  The %s of address 0x%llx failed: %s.\n",kind==1 ? "write" : kind==8 ? "execution" : "read",
            (unsigned long long)address,target);
    } else put(o,"  %s (exception 0x%08lx).\n",exception_name(record->ExceptionCode),record->ExceptionCode);
    if (note) put(o,"  %s\n",note);
    put(o,"  Instruction: %s (%s)\n",where,guest_code((uintptr_t)c->Rip) ? "game code" : "runtime, GPU or driver code");
    const char *thread=runtime_thread_current_name();
    static char host_name[64];
    PWSTR description=NULL;
    if (!thread && thread_description && SUCCEEDED(thread_description(GetCurrentThread(),&description)) && description) {
        WideCharToMultiByte(CP_UTF8,0,description,-1,host_name,sizeof(host_name),NULL,NULL);
        LocalFree(description);
    }
    if (thread) put(o,"  Thread: %lu \"%s\" (a game thread)\n",GetCurrentThreadId(),thread);
    else put(o,"  Thread: %lu%s%s%s (a runtime or driver thread)\n",GetCurrentThreadId(),host_name[0] ? " \"" : "",
             host_name,host_name[0] ? "\"" : "");
    put(o,"\nCall stack (innermost first; game code as <executable>+<address in its ELF>)\n");
    uint64_t pcs[32];
    const int depth=walk(*c,pcs,32);
    for (int i=0;i<depth;++i) { describe(where,sizeof(where),(uintptr_t)pcs[i]); put(o,"  #%-2d %s\n",i,where); }
    put_stack_scan(o,(uintptr_t)c->Rsp);
    put(o,"\nRegisters\n");
    put(o,"  rax %016llx  rbx %016llx  rcx %016llx  rdx %016llx\n",c->Rax,c->Rbx,c->Rcx,c->Rdx);
    put(o,"  rsi %016llx  rdi %016llx  rbp %016llx  rsp %016llx\n",c->Rsi,c->Rdi,c->Rbp,c->Rsp);
    put(o,"  r8  %016llx  r9  %016llx  r10 %016llx  r11 %016llx\n",c->R8,c->R9,c->R10,c->R11);
    put(o,"  r12 %016llx  r13 %016llx  r14 %016llx  r15 %016llx\n",c->R12,c->R13,c->R14,c->R15);
    put(o,"  rip %016llx  eflags %08lx\n",c->Rip,c->EFlags);
    put_code(o,(uintptr_t)c->Rip);
    put_settings(o);
    put_events(o,now,0); /* the crashing thread may hold the event lock */
    close_out(o);
    fprintf(stderr,"Crash report: %s\n",folder);
    fflush(NULL);
    write_minidump(folder,info);
}
/* A report of the calling thread, for the ways the port ends a game itself: a failed check
 * (exit 22, 23), an unsupported import (20), abort(). */
static void report_here(DWORD code, const char *why) {
    static CONTEXT context;
    static EXCEPTION_RECORD record;
    RtlCaptureContext(&context);
    record.ExceptionCode=code;
    record.ExceptionAddress=(PVOID)(uintptr_t)context.Rip;
    EXCEPTION_POINTERS pointers={&record,&context};
    runtime_report_crash(&pointers,why);
}
void runtime_report_stop(const char *why) { report_here(0xe0000017,why); }
static void abort_report(int sig) {
    (void)sig;
    report_here(0x40000015,"A check in the runtime or the GPU code failed; its message is at the end of log-tail.txt.");
}

/* Every thread's stack and CPU use over `sample_ms`, busiest first (freeze and mark reports;
 * runs on the watchdog thread, which suspends each other thread just to read its context). */
typedef struct { DWORD id; HANDLE handle; uint64_t cpu; int depth, guest; uint64_t pcs[24]; char name[64]; } ThreadSample;
static ThreadSample samples[512];
static unsigned long guest_ids[512];
static char guest_names[512][32];
static uint64_t thread_cpu(HANDLE thread) {
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(thread,&created,&exited,&kernel,&user)) return 0;
    return ((uint64_t)kernel.dwHighDateTime<<32|kernel.dwLowDateTime)+((uint64_t)user.dwHighDateTime<<32|user.dwLowDateTime);
}
static void put_threads(Out *o, DWORD sample_ms) {
    int named=-1, count=0;
    for (int i=0;i<40 && (named=runtime_thread_names(guest_ids,guest_names,512))<0;++i) Sleep(5);
    const DWORD self=GetCurrentThreadId(), pid=GetCurrentProcessId();
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
    if (snap!=INVALID_HANDLE_VALUE) {
        THREADENTRY32 entry={.dwSize=sizeof(entry)};
        for (BOOL ok=Thread32First(snap,&entry); ok && count<512; ok=Thread32Next(snap,&entry)) {
            if (entry.th32OwnerProcessID!=pid || entry.th32ThreadID==self) continue;
            HANDLE thread=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,entry.th32ThreadID);
            if (!thread) continue;
            ThreadSample *s=&samples[count++];
            memset(s,0,sizeof(*s));
            s->id=entry.th32ThreadID; s->handle=thread; s->cpu=thread_cpu(thread);
        }
        CloseHandle(snap);
    }
    Sleep(sample_ms);
    for (int i=0;i<count;++i) {
        ThreadSample *s=&samples[i];
        s->cpu=thread_cpu(s->handle)-s->cpu;
        if (SuspendThread(s->handle)!=(DWORD)-1) {
            CONTEXT context;
            memset(&context,0,sizeof(context));
            context.ContextFlags=CONTEXT_FULL;
            if (GetThreadContext(s->handle,&context)) s->depth=walk(context,s->pcs,24);
            ResumeThread(s->handle);
        }
        for (int g=0;g<named;++g) if (guest_ids[g]==s->id) { snprintf(s->name,sizeof(s->name),"%s",guest_names[g]); s->guest=1; break; }
        PWSTR description=NULL;
        if (!s->guest && thread_description && SUCCEEDED(thread_description(s->handle,&description)) && description) {
            WideCharToMultiByte(CP_UTF8,0,description,-1,s->name,sizeof(s->name),NULL,NULL);
            LocalFree(description);
        }
        CloseHandle(s->handle);
    }
    for (int i=1;i<count;++i) /* busiest first */
        for (int j=i;j>0 && samples[j].cpu>samples[j-1].cpu;--j) { ThreadSample t=samples[j]; samples[j]=samples[j-1]; samples[j-1]=t; }
    put(o,"\nThreads (%d, busiest first; CPU use over %.1f s; game code as <executable>+<address in its ELF>)\n",count,sample_ms/1000.0);
    for (int i=0;i<count;++i) {
        const ThreadSample *s=&samples[i];
        const double cpu=s->cpu/(sample_ms*100.0); /* 100 ns units; the clock ticks every ~15.6 ms */
        put(o,"\n%s thread %lu \"%s\": %.0f%% CPU\n",s->guest ? "Game" : "Host",s->id,s->name[0] ? s->name : "?",
            cpu>100.0 ? 100.0 : cpu);
        for (int d=0;d<s->depth;++d) {
            char where[160];
            describe(where,sizeof(where),(uintptr_t)s->pcs[d]);
            put(o,"  #%-2d %s\n",d,where);
        }
    }
}
static void thread_report(const char *kind, const char *title, const char *label, DWORD sample_ms) {
    static char folder[MAX_PATH*2];
    Out *o=&watch_out;
    if (!open_report(o,kind,folder,sizeof(folder))) return;
    const uint64_t now=elapsed_ms();
    put_header(o,title,now);
    put_threads(o,sample_ms);
    put_settings(o);
    put_events(o,now,1);
    close_out(o);
    printf("%s report: %s\n",label,folder);
    fflush(stdout);
}
static DWORD WINAPI watchdog(LPVOID unused) {
    (void)unused;
    uint64_t seen=gow3gpu_flip_count(), changed=clock_ms();
    int frozen=0, marks=0;
    char kind[32], title[256];
    for (;;) {
        if (WaitForSingleObject(mark_event,250)==WAIT_OBJECT_0) {
            ++marks;
            runtime_event("mark %d",marks);
            snprintf(kind,sizeof(kind),"mark %d",marks);
            snprintf(title,sizeof(title),"%s: mark %d (the player marked this moment)",game_name(),marks);
            thread_report(kind,title,"Mark",200);
            snprintf(title,sizeof(title),"Mark %d saved (analysis\\reports)",marks);
            gow3gpu_notice(title);
        }
        const uint64_t flips=gow3gpu_flip_count(), now=clock_ms();
        if (flips!=seen) {
            if (frozen) {
                printf("Freeze over: new frames after %.1f s\n",(now-changed)/1000.0);
                runtime_event("new frames after %.1f s without",(now-changed)/1000.0);
                frozen=0;
            }
            seen=flips; changed=now;
            __atomic_store_n(&last_flip_ms,now-start_ms,__ATOMIC_RELAXED);
        } else if (!frozen && freeze_ms && flips>=600 && now-changed>=freeze_ms) {
            frozen=1;
            runtime_event("no new frame for %llu s",(unsigned long long)(now-changed)/1000);
            snprintf(title,sizeof(title),"%s froze: no new frame for %llu s",game_name(),(unsigned long long)(now-changed)/1000);
            thread_report("freeze",title,"Freeze",1000);
        }
    }
    return 0;
}

void runtime_report_configure(void) {
    start_ms=clock_ms();
    HMODULE dbghelp=LoadLibraryExA("dbghelp.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32), kernel=GetModuleHandleA("kernel32.dll");
    if (dbghelp) write_dump=(WriteDumpFn)(void (*)(void))GetProcAddress(dbghelp,"MiniDumpWriteDump");
    thread_description=(ThreadDescriptionFn)(void (*)(void))GetProcAddress(kernel,"GetThreadDescription");
    memory_info=(MemoryInfoFn)(void (*)(void))GetProcAddress(kernel,"K32GetProcessMemoryInfo");
    signal(SIGABRT,abort_report);
    gow3gpu_set_stop_hook(runtime_report_stop);
}
void runtime_report_start(void) {
    const char *seconds=getenv("GOW3_FREEZE_SECONDS");
    if (seconds && *seconds) freeze_ms=strtoull(seconds,NULL,10)*1000;
    mark_event=CreateEventA(NULL,FALSE,FALSE,NULL);
    HANDLE thread=CreateThread(NULL,0,watchdog,NULL,0,NULL);
    if (thread) CloseHandle(thread);
}
void runtime_report_mark(void) { if (mark_event) SetEvent(mark_event); }
/* GOW3_REPORT_TEST=crash:<s> or freeze:<s> (called by the pad thread on every read). */
void runtime_report_test(void) {
    static int kind=-1;
    static uint64_t due;
    if (kind<0) {
        const char *test=getenv("GOW3_REPORT_TEST");
        kind=0;
        if (test && !strncmp(test,"crash:",6)) { kind=1; due=strtoull(test+6,NULL,10)*1000; }
        else if (test && !strncmp(test,"freeze:",7)) { kind=2; due=strtoull(test+7,NULL,10)*1000; }
    }
    if (!kind || elapsed_ms()<due) return;
    const int test=kind;
    kind=0;
    if (test==1) {
        puts("Report test: a write through a bad pointer on the pad thread");
        *(volatile int *)(uintptr_t)0x10=1;
    } else {
        printf("Report test: the pad thread stops for %llu s\n",(unsigned long long)freeze_ms/1000+10);
        Sleep((DWORD)(freeze_ms+10000));
    }
}
#else
void runtime_report_module(const char *name, const void *base, uint64_t size) { (void)name; (void)base; (void)size; }
void runtime_report_crash(void *exception_pointers, const char *note) { (void)exception_pointers; (void)note; }
void runtime_report_stop(const char *why) { (void)why; }
void runtime_report_configure(void) { start_ms=clock_ms(); (void)save_ms; (void)last_flip_ms; }
void runtime_report_start(void) {}
void runtime_report_mark(void) {}
void runtime_report_test(void) {}
#endif
