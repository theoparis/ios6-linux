#include "ios6/runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <dirent.h>
#include <sys/statvfs.h>
#include <sys/socket.h>
#include <sys/syscall.h>

static const char *s_host_functions[] = {
    "_getopt",
    "_exit",
    "_puts",
    "_fwrite",
    "_strrchr",
    "_fprintf",
    "_strlen",
    "_strcmp",
    "_strncmp",
    "_memset",
    "_memmove",
    "_memcpy",
    "_bcopy",
    "_strcpy",
    "_strlcpy",
    "_sync",
    "_sleep",
    "_time",
    "_close",
    "_free",
    "_malloc",
    "_calloc",
    "_realloc",
    "__dyld_image_count",
    "__dyld_get_image_header",
    "__dyld_get_image_name",
    "__dyld_get_image_vmaddr_slide",
    "__dyld_func_lookup",
    "__dyld_dlopen",
    "__dyld_dlsym",
    "__dyld_dlclose",
    "__dyld_dlerror",
    "__dyld_dyld_register_image_state_change_handler",
    "__dyld_register_func_for_remove_image",
    "__dyld_image_path_containing_address",
    "__dyld_get_image_slide",
    "_malloc_default_zone",
    "_malloc_zone_malloc",
    "_malloc_zone_calloc",
    "_malloc_zone_realloc",
    "_malloc_zone_free",
    "_malloc_zone_valloc",
    "_malloc_zone_memalign",
    "_malloc_size",
    "_malloc_zone_size",
    "_malloc_good_size",
    "_malloc_zone_from_ptr",
    "_signal",
    "_setfsent",
    "_wait",
    "_waitpid",
    "_endfsent",
    "_getfsent",
    "_stat",
    "_strdup",
    "_posix_memalign",
    "_valloc",
    "__simple_asl_log",
    "_strlcat",
    "_strncpy",
    "_strtol",
    "_open",
    "_perror",
    "_printf",
    "_fork",
    "_execv",
    "_errx",
    "_kqueue",
    "_kevent",
    "_pthread_mutex_lock",
    "_pthread_mutex_unlock",
    "_pthread_mutex_trylock",
    "_pthread_mutex_init",
    "_pthread_mutex_destroy",
    "_pthread_rwlock_rdlock",
    "_pthread_rwlock_wrlock",
    "_pthread_rwlock_unlock",
    "_pthread_rwlock_init",
    "_pthread_rwlock_destroy",
    "_pthread_cond_wait",
    "_pthread_cond_timedwait",
    "_pthread_cond_signal",
    "_pthread_cond_broadcast",
    "_pthread_cond_init",
    "_pthread_cond_destroy",
    "_pthread_once",
    "_pthread_self",
    "_pthread_getspecific",
    "_pthread_setspecific",
    "_pthread_key_create",
    "_pthread_main_np",
    "_pthread_is_threaded_np",
    "__NSGetEnviron",
    "_NSGetArgc",
    "_NSGetArgv",
    "__NSGetMachExecuteHeader",
    NULL
};

#define HOST_FUNCS_COUNT (sizeof(s_host_functions)/sizeof(s_host_functions[0]) - 1)

int runtime_install_stubs(guest_arena_t *arena, host_stubs_t *stubs) {
    memset(stubs, 0, sizeof(*stubs));
    stubs->arena = arena;

    // Allocate memory pages for stdio, guards, TLS, process info, trampolines
    guest_arena_mprotect(arena, 0x21000000, 0x100000, PROT_READ | PROT_WRITE);
    guest_arena_mprotect(arena, HOST_TRAMPOLINE_BASE, 0x10000, PROT_READ | PROT_WRITE | PROT_EXEC);

    // Guard word
    uint32_t *guard = (uint32_t *)guest_arena_to_host(arena, 0x21002000);
    if (guard) *guard = 0xdeadbeef;

    // optind = 1
    uint32_t *optind = (uint32_t *)guest_arena_to_host(arena, 0x21003000);
    if (optind) *optind = 1;

    // TLS pointer
    uint32_t *tls_reg = (uint32_t *)guest_arena_to_host(arena, TLS_REGISTER_ADDR);
    if (tls_reg) *tls_reg = 0x21008000;

    // dyld's fixed-address lookup entry (0x2fe01008, ARM): ldr pc,[pc,#-4]; .word tramp|1
    guest_arena_mprotect(arena, 0x2fe00000, 0x2000, PROT_READ | PROT_WRITE);
    for (size_t i = 0; s_host_functions[i] != NULL; i++) {
        if (strcmp(s_host_functions[i], "__dyld_func_lookup") == 0) {
            uint32_t *stub = (uint32_t *)guest_arena_to_host(arena, 0x2fe01008);
            if (stub) {
                stub[0] = 0xE51FF004;
                stub[1] = (HOST_TRAMPOLINE_BASE + (uint32_t)i * HOST_TRAMPOLINE_STRIDE) | 1u;
            }
        }
    }

    // Commpage at 0xffff0000
    guest_arena_mprotect(arena, 0xffff0000, 0x4000, PROT_READ | PROT_WRITE);
    void *commpage = guest_arena_to_host(arena, 0xffff0000);
    if (commpage) memset(commpage, 0, 0x4000);

    // Install trampoline code: `svc #0x80; b .` in Thumb mode (0xdf80, 0xe7fe)
    uint8_t tramp_code[6] = { 0x80, 0xdf, 0xfe, 0xe7, 0x00, 0xbf };

    for (size_t i = 0; s_host_functions[i] != NULL; i++) {
        uint32_t addr = HOST_TRAMPOLINE_BASE + (uint32_t)i * HOST_TRAMPOLINE_STRIDE;
        void *dst = guest_arena_to_host(arena, addr);
        if (dst) {
            memcpy(dst, tramp_code, sizeof(tramp_code));
        }
        stubs->tramp_addrs[i] = addr;
        stubs->tramp_count++;
    }

    return 0;
}

/*
 * Emulate the parts of libSystem_initializer the real dyld would have run, once the
 * shared cache is mapped. Cache specific: iPhone/iPad iOS 6.1 (10B329) armv7.
 */
#define ERRNO_SETTER_CODE_ADDR  0x21009000u
#define FAKE_PTHREAD_ADDR       0x21008100u
#define FAKE_TPIDR_BLOCK        0x21008000u
#define PTHREAD_ERRNO_OFFSET    0x44u
/* libsystem_kernel's `_cerror` calls through this cell (set by __libkernel_init) */
#define CACHE_CERROR_ERRNO_CELL 0x3afb5000u

int runtime_patch_for_cache(guest_arena_t *arena) {
    // Fake pthread_t (iOS 6 layout): sig 'THRD' at +0, errno at +0x44, TSD array at +0x48.
    // TPIDRURW points at &self->tsd[0] and tsd[0] holds self.
    uint32_t *tlsreg = (uint32_t *)guest_arena_to_host(arena, TLS_REGISTER_ADDR);
    uint32_t *pth = (uint32_t *)guest_arena_to_host(arena, FAKE_PTHREAD_ADDR);
    if (!tlsreg || !pth) return -1;
    memset(pth, 0, 0x900);
    pth[0] = 0x54485244; // 'THRD'
    pth[0x48 / 4] = FAKE_PTHREAD_ADDR;
    *tlsreg = FAKE_PTHREAD_ADDR + 0x48;

    // errno setter: mrc p15,0,r1,c13,c0,3; ldr r1,[r1]; str r0,[r1,#0x44]; bx lr
    uint32_t *code = (uint32_t *)guest_arena_to_host(arena, ERRNO_SETTER_CODE_ADDR);
    uint32_t *cell = (uint32_t *)guest_arena_to_host(arena, CACHE_CERROR_ERRNO_CELL);
    if (!code || !cell) return -1;
    code[0] = 0xEE1D1F70;
    code[1] = 0xE5911000;
    code[2] = 0xE5810000 | PTHREAD_ERRNO_OFFSET;
    code[3] = 0xE12FFF1E;
    *cell = ERRNO_SETTER_CODE_ADDR;
    return 0;
}

uint32_t runtime_resolve_symbol(const char *name, void *user_data) {
    (void)user_data;
    if (strcmp(name, "___stderrp") == 0) return 0x21000000;
    if (strcmp(name, "___stdoutp") == 0) return 0x21001000;
    if (strcmp(name, "___stdinp") == 0) return 0x21001800;
    if (strcmp(name, "___stack_chk_guard") == 0) return 0x21002000;
    if (strcmp(name, "_optind") == 0) return 0x21003000;
    if (strcmp(name, "_optarg") == 0) return 0x21004000;
    if (strcmp(name, "__DefaultRuneLocale") == 0) return 0;

    for (size_t i = 0; s_host_functions[i] != NULL; i++) {
        if (strcmp(name, s_host_functions[i]) == 0) {
            return (HOST_TRAMPOLINE_BASE + (uint32_t)i * HOST_TRAMPOLINE_STRIDE) | 1;
        }
    }
    if (strcmp(name, "__exit") == 0 || strcmp(name, "__Exit") == 0) {
        return runtime_resolve_symbol("_exit", NULL);
    }
    if (strcmp(name, "_printf") == 0 || strcmp(name, "_errx") == 0 || strcmp(name, "_perror") == 0) {
        return runtime_resolve_symbol("_puts", NULL);
    }
    return 0;
}

int runtime_setup_stack(guest_arena_t *arena, int argc, char **argv,
                        uint32_t *out_sp, uint32_t *out_argv_addr) {
    uint32_t stack_top = 0x20000000;
    guest_arena_mprotect(arena, stack_top - 0x100000, 0x100000, PROT_READ | PROT_WRITE);
    guest_arena_mprotect(arena, 0x20100000, 0x200000, PROT_READ | PROT_WRITE);

    uint32_t str_ptr = 0x20100000;
    uint32_t argv_array = 0x20200000;

    uint32_t *argv_ptrs = (uint32_t *)guest_arena_to_host(arena, argv_array);

    for (int i = 0; i < argc; i++) {
        size_t len = strlen(argv[i]);
        char *dst = (char *)guest_arena_to_host(arena, str_ptr);
        memcpy(dst, argv[i], len + 1);
        argv_ptrs[i] = str_ptr;
        str_ptr += (uint32_t)(len + 1);
    }
    argv_ptrs[argc] = 0; // NULL terminator

    // Set process info block
    uint32_t *pinfo = (uint32_t *)guest_arena_to_host(arena, PROCESS_INFO_BASE);
    if (pinfo) {
        pinfo[0] = (uint32_t)argc;
        pinfo[1] = argv_array;
        pinfo[2] = argv_array + (argc + 1) * 4; // environ
        argv_ptrs[argc + 1] = 0; // envp = {NULL}
        pinfo[3] = 0x10000;
        pinfo[3] = 0x10000; // exec_header
    }

    *out_sp = stack_top;
    *out_argv_addr = argv_array;
    return 0;
}

int runtime_handle_trampoline(guest_arena_t *arena, uint32_t *gtc) {
    uint32_t pc = gtc[GTC_FIELD_NEXT_PC] & ~1u;
    if (pc < HOST_TRAMPOLINE_BASE) return -1;
    uint32_t idx = (pc - HOST_TRAMPOLINE_BASE) / HOST_TRAMPOLINE_STRIDE;
    if (idx >= HOST_FUNCS_COUNT) return -1;

    const char *fn = s_host_functions[idx];

    if (strcmp(fn, "_exit") == 0) {
        int code = (int)gtc[GTC_R0];
        printf("\n>>> Program exited via _exit with code: %d <<<\n", code);
        exit(code);
    } else if (strcmp(fn, "__dyld_func_lookup") == 0) {
        // _dyld_func_lookup(name, &fn): 0 on success, nonzero if unknown.
        const char *name = (const char *)guest_arena_to_host(arena, gtc[GTC_R0]);
        uint32_t *slot = (uint32_t *)guest_arena_to_host(arena, gtc[GTC_R1]);
        uint32_t addr = name ? runtime_resolve_symbol(name, NULL) : 0;
        if (getenv("IOS6_TRACE")) fprintf(stderr, "[dyld] func_lookup %s -> 0x%x\n", name ? name : "(null)", addr);
        if (addr && slot) { *slot = addr; gtc[GTC_R0] = 0; }
        else gtc[GTC_R0] = 1;
    } else if (strcmp(fn, "_getopt") == 0) {
        gtc[GTC_R0] = (uint32_t)-1; // No more options
    } else if (strcmp(fn, "_setfsent") == 0) {
        gtc[GTC_R0] = 0; // Cannot open /etc/fstab
    } else if (strcmp(fn, "_endfsent") == 0) {
        gtc[GTC_R0] = 0;
    } else if (strcmp(fn, "_fprintf") == 0) {
        const char *fmt = (const char *)guest_arena_to_host(arena, gtc[GTC_R1]);
        if (fmt) {
            const char *arg = (const char *)guest_arena_to_host(arena, gtc[GTC_R2]);
            if (strstr(fmt, "%s") && arg) {
                fprintf(stderr, fmt, arg);
            } else {
                fprintf(stderr, "%s", fmt);
            }
        }
        gtc[GTC_R0] = 0;
    } else if (strcmp(fn, "_puts") == 0 || strcmp(fn, "_printf") == 0 || strcmp(fn, "_perror") == 0) {
        const char *s = (const char *)guest_arena_to_host(arena, gtc[GTC_R0]);
        if (s) {
            printf("%s\n", s);
        }
        gtc[GTC_R0] = 0;
    } else if (strcmp(fn, "_strlen") == 0) {
        const char *s = (const char *)guest_arena_to_host(arena, gtc[GTC_R0]);
        gtc[GTC_R0] = s ? (uint32_t)strlen(s) : 0;
    } else if (strcmp(fn, "_strcmp") == 0) {
        const char *s1 = (const char *)guest_arena_to_host(arena, gtc[GTC_R0]);
        const char *s2 = (const char *)guest_arena_to_host(arena, gtc[GTC_R1]);
        int cmp = (s1 && s2) ? strcmp(s1, s2) : 0;
        gtc[GTC_R0] = (uint32_t)cmp;
        if (cmp == 0) {
            gtc[GTC_FIELD_CPSR] |= CPSR_Z;
            gtc[GTC_FIELD_CPSR] &= ~CPSR_N;
        } else if (cmp < 0) {
            gtc[GTC_FIELD_CPSR] &= ~CPSR_Z;
            gtc[GTC_FIELD_CPSR] |= CPSR_N;
        } else {
            gtc[GTC_FIELD_CPSR] &= ~(CPSR_Z | CPSR_N);
        }
    } else if (strcmp(fn, "_strncmp") == 0) {
        const char *s1 = (const char *)guest_arena_to_host(arena, gtc[GTC_R0]);
        const char *s2 = (const char *)guest_arena_to_host(arena, gtc[GTC_R1]);
        size_t n = gtc[GTC_R2];
        int cmp = (s1 && s2) ? strncmp(s1, s2, n) : 0;
        gtc[GTC_R0] = (uint32_t)cmp;
        if (cmp == 0) {
            gtc[GTC_FIELD_CPSR] |= CPSR_Z;
            gtc[GTC_FIELD_CPSR] &= ~CPSR_N;
        } else if (cmp < 0) {
            gtc[GTC_FIELD_CPSR] &= ~CPSR_Z;
            gtc[GTC_FIELD_CPSR] |= CPSR_N;
        } else {
            gtc[GTC_FIELD_CPSR] &= ~(CPSR_Z | CPSR_N);
        }
    } else if (strcmp(fn, "_strrchr") == 0) {
        const char *s = (const char *)guest_arena_to_host(arena, gtc[GTC_R0]);
        int c = (int)gtc[GTC_R1];
        if (s) {
            const char *p = strrchr(s, c);
            if (p) {
                gtc[GTC_R0] = gtc[GTC_R0] + (uint32_t)(p - s);
                gtc[GTC_FIELD_CPSR] &= ~CPSR_Z;
            } else {
                gtc[GTC_R0] = 0;
                gtc[GTC_FIELD_CPSR] |= CPSR_Z;
            }
        } else {
            gtc[GTC_R0] = 0;
            gtc[GTC_FIELD_CPSR] |= CPSR_Z;
        }
    } else if (strcmp(fn, "_malloc") == 0 || strcmp(fn, "_malloc_zone_malloc") == 0) {
        static uint32_t guest_heap = 0x25000000;
        uint32_t req = (strcmp(fn, "_malloc") == 0) ? gtc[GTC_R0] : gtc[GTC_R1];
        uint32_t sz = (req + 15) & ~15u;
        guest_arena_mprotect(arena, guest_heap, sz + 4096, PROT_READ | PROT_WRITE);
        gtc[GTC_R0] = guest_heap;
        guest_heap += sz;
    } else if (strcmp(fn, "_free") == 0 || strcmp(fn, "_malloc_zone_free") == 0) {
        gtc[GTC_R0] = 0;
    } else if (strcmp(fn, "_signal") == 0) {
        gtc[GTC_R0] = 0;
    } else if (strcmp(fn, "__NSGetEnviron") == 0) {
        gtc[GTC_R0] = PROCESS_INFO_BASE + 8;
    } else if (strcmp(fn, "_NSGetArgc") == 0) {
        gtc[GTC_R0] = PROCESS_INFO_BASE;
    } else if (strcmp(fn, "_NSGetArgv") == 0) {
        gtc[GTC_R0] = PROCESS_INFO_BASE + 4;
    } else if (strcmp(fn, "__NSGetMachExecuteHeader") == 0) {
        gtc[GTC_R0] = PROCESS_INFO_BASE + 12;
    } else {
        gtc[GTC_R0] = 0;
    }

    // Return to caller via LR
    gtc[GTC_FIELD_NEXT_PC] = gtc[GTC_LR];
    return 0;
}

#define FAKE_PORT_BASE 0x1000

static void dispatch_mach_trap(guest_arena_t *arena, uint32_t trap, uint32_t *gtc) {
    switch (trap) {
        case 31: // mach_msg_trap
        case 32: {
            uint32_t msg = gtc[GTC_R0];
            uint32_t *header = (uint32_t *)guest_arena_to_host(arena, msg);
            if (header && (header[5] == 3409 || header[5] == 206 || header[5] == 3418)) {
                // Port-returning RPCs: task_get_special_port, host_get_clock_service,
                // semaphore_create. Reply id = request id + 100.
                uint32_t reply_id = header[5] + 100;
                memset(header, 0, 40);
                header[0] = 0x80000012;          // MACH_MSGH_BITS_COMPLEX | reply bits
                header[1] = 40;                  // msgh_size
                header[5] = reply_id;            // msgh_id = request + 100
                header[6] = 1;                   // descriptor count
                header[7] = FAKE_PORT_BASE + 5;  // port name
                header[9] = 17u << 16;           // pad2:16, disposition MOVE_SEND:8, type PORT_DESCRIPTOR:8
                gtc[GTC_R0] = 0; // KERN_SUCCESS
            } else if (header && header[5] == 200) { // host_info(host, flavor, info, &count)
                uint32_t flavor = header[8];
                uint32_t count = header[9];
                if (count > 64) count = 64;
                uint32_t info[64] = {0};
                if (flavor == 1) { // HOST_BASIC_INFO
                    info[0] = 1; info[1] = 1;          // max_cpus, avail_cpus
                    info[2] = 512u << 20;              // memory_size
                    info[3] = 12; info[4] = 9;         // CPU_TYPE_ARM, CPU_SUBTYPE_ARM_V7
                    info[5] = 0;                       // cpu_threadtype
                    info[6] = 1; info[7] = 1;          // physical_cpu(_max)
                    info[8] = 1; info[9] = 1;          // logical_cpu(_max)
                    info[10] = 512u << 20; info[11] = 0; // max_mem (64-bit)
                } else if (flavor == 5) { // HOST_PRIORITY_INFO
                    for (uint32_t k = 0; k < count; k++) info[k] = 31;
                }
                memset(header, 0, 40 + count * 4);
                header[0] = 0x12;
                header[1] = 40 + count * 4;
                header[5] = 300;
                header[7] = 1;                         // NDR record (little-endian int rep)
                header[8] = 0;                         // RetCode = KERN_SUCCESS
                header[9] = count;
                memcpy(&header[10], info, count * 4);
                gtc[GTC_R0] = 0;
            } else if (header) {
                // Generic MIG routine: reply {header, NDR, RetCode = KERN_SUCCESS}
                uint32_t id = header[5];
                if (getenv("IOS6_TRACE_SYS")) fprintf(stderr, "mach_msg: generic success reply for msgh_id %u\n", id);
                memset(header, 0, 36);
                header[0] = 0x12;
                header[1] = 36;
                header[5] = id + 100;
                header[7] = 1;
                header[8] = 0;
                gtc[GTC_R0] = 0;
            } else {
                gtc[GTC_R0] = 5; // KERN_FAILURE
            }
            break;
        }
        case 26: gtc[GTC_R0] = FAKE_PORT_BASE + 1; break; // mach_reply_port
        case 27: gtc[GTC_R0] = FAKE_PORT_BASE + 2; break; // thread_self_trap
        case 28: gtc[GTC_R0] = FAKE_PORT_BASE + 3; break; // task_self_trap
        case 29: gtc[GTC_R0] = FAKE_PORT_BASE + 4; break; // host_self_trap
        case 89: { // mach_timebase_info
            uint32_t *info = (uint32_t *)guest_arena_to_host(arena, gtc[GTC_R0]);
            if (info) {
                info[0] = 1; // numer
                info[1] = 1; // denom
            }
            gtc[GTC_R0] = 0;
            break;
        }
        case 10: { // mach_absolute_time
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            gtc[GTC_R0] = (uint32_t)(ts.tv_sec * 1000000000ULL + ts.tv_nsec);
            break;
        }
        default:
            gtc[GTC_R0] = 0;
            break;
    }
    gtc[GTC_FIELD_CPSR] &= ~CPSR_C;
}

/* ---- Darwin ABI translation helpers ---- */
static uint32_t darwin_errno(int e) {
    switch (e) {
        case EAGAIN: return 35;
        case EINPROGRESS: return 36;
        case EALREADY: return 37;
        case ENOTSOCK: return 38;
        case ENAMETOOLONG: return 63;
        case ENOTEMPTY: return 66;
        case ELOOP: return 62;
        case ENOSYS: return 78;
        case ETIMEDOUT: return 60;
        case ECONNREFUSED: return 61;
        case EOVERFLOW: return 84;
        default: return (uint32_t)e; /* 1..34 are identical */
    }
}

static int linux_open_flags(uint32_t f) {
    int o = (int)(f & 3);
    if (f & 0x4) o |= O_NONBLOCK;
    if (f & 0x8) o |= O_APPEND;
    if (f & 0x100) o |= O_NOFOLLOW;
    if (f & 0x200) o |= O_CREAT;
    if (f & 0x400) o |= O_TRUNC;
    if (f & 0x800) o |= O_EXCL;
    if (f & 0x100000) o |= O_DIRECTORY;
    if (f & 0x1000000) o |= O_CLOEXEC;
    return o;
}

/* Map an absolute guest path into IOS6_ROOT (the target sysroot), if one is configured. */
static const char *guest_path(const char *p, char *buf, size_t n) {
    const char *root = getenv("IOS6_ROOT");
    if (!p || p[0] != '/' || !root || !*root) return p;
    if (!strncmp(p, "/dev/", 5) || !strcmp(p, "/dev") || !strncmp(p, "/tmp/", 5)) return p;
    size_t rl = strlen(root);
    if (!strncmp(p, root, rl) && (p[rl] == '/' || p[rl] == '\0')) return p; // already in sysroot
    snprintf(buf, n, "%s%s", root, p);
    return buf;
}

static void set_result(uint32_t *gtc, long r) {
    if (r < 0) { gtc[GTC_R0] = darwin_errno(errno); gtc[GTC_FIELD_CPSR] |= CPSR_C; }
    else { gtc[GTC_R0] = (uint32_t)r; gtc[GTC_FIELD_CPSR] &= ~CPSR_C; }
}

int runtime_handle_syscall(guest_arena_t *arena, uint32_t *gtc) {
    int32_t r12 = (int32_t)gtc[GTC_R12];

    if ((uint32_t)r12 == 0x80000000u) {
        // ARM platform trap: r3 selects the call
        uint32_t *tls = (uint32_t *)guest_arena_to_host(arena, TLS_REGISTER_ADDR);
        if (gtc[3] == 2 && tls) *tls = gtc[GTC_R0];       // thread_set_cthread_self
        else if (gtc[3] == 3 && tls) gtc[GTC_R0] = *tls;  // thread_get_cthread_self
        if (getenv("IOS6_TRACE_SYS")) fprintf(stderr, "platform trap r3=%u r0=%x\n", gtc[3], gtc[GTC_R0]);
        gtc[GTC_FIELD_CPSR] &= ~CPSR_C;
        gtc[GTC_FIELD_NEXT_PC] = gtc[GTC_FIELD_TRAP_RESUME];
        return 0;
    }

    if (r12 < 0) {
        dispatch_mach_trap(arena, (uint32_t)(-r12), gtc);
        gtc[GTC_FIELD_NEXT_PC] = gtc[GTC_FIELD_TRAP_RESUME];
        return 0;
    }

    uint32_t sysno = (uint32_t)r12;
    if (getenv("IOS6_TRACE_SYS")) fprintf(stderr, "syscall %d r0=%x r1=%x r2=%x\n", (int)sysno, gtc[0], gtc[1], gtc[2]);

    switch (sysno) {
        case 1: { // SYS_exit
            int code = (int)gtc[GTC_R0];
            printf("\n>>> Program exited via SYS_exit with code: %d <<<\n", code);
            exit(code);
        }
        case 396: // SYS_read_nocancel
        case 3: { // SYS_read
            int fd = (int)gtc[GTC_R0];
            void *buf = guest_arena_to_host(arena, gtc[GTC_R1]);
            size_t count = gtc[GTC_R2];
            if (buf) {
                ssize_t ret = read(fd, buf, count);
                if (ret >= 0) {
                    gtc[GTC_R0] = (uint32_t)ret;
                    gtc[GTC_FIELD_CPSR] &= ~CPSR_C;
                } else {
                    gtc[GTC_R0] = (uint32_t)errno;
                    gtc[GTC_FIELD_CPSR] |= CPSR_C;
                }
            } else {
                gtc[GTC_R0] = EFAULT;
                gtc[GTC_FIELD_CPSR] |= CPSR_C;
            }
            break;
        }
        case 4:   // SYS_write
        case 397: {
            int fd = (int)gtc[GTC_R0];
            const void *buf = guest_arena_to_host(arena, gtc[GTC_R1]);
            size_t count = gtc[GTC_R2];
            if (buf) {
                ssize_t ret = write(fd, buf, count);
                if (ret >= 0) {
                    gtc[GTC_R0] = (uint32_t)ret;
                    gtc[GTC_FIELD_CPSR] &= ~CPSR_C;
                } else {
                    gtc[GTC_R0] = (uint32_t)errno;
                    gtc[GTC_FIELD_CPSR] |= CPSR_C;
                }
            } else {
                gtc[GTC_R0] = EFAULT;
                gtc[GTC_FIELD_CPSR] |= CPSR_C;
            }
            break;
        }
        case 6:   // SYS_close
        case 399: { // SYS_close_nocancel
            int fd = (int)gtc[GTC_R0];
            set_result(gtc, fd > 2 ? close(fd) : 0);
            break;
        }
        case 5:   // SYS_open
        case 398: { // SYS_open_nocancel
            char pbuf[1024];
            const char *path = guest_path((const char *)guest_arena_to_host(arena, gtc[GTC_R0]), pbuf, sizeof pbuf);
            int fd = path ? open(path, linux_open_flags(gtc[GTC_R1]), (mode_t)gtc[GTC_R2]) : (errno = EFAULT, -1);
            if (getenv("IOS6_TRACE_SYS")) fprintf(stderr, "open(%s) -> %d\n", path ? path : "?", fd);
            set_result(gtc, fd);
            break;
        }
        case 116: { // SYS_gettimeofday: Darwin returns sec/usec in r0/r1
            struct timeval tv;
            gettimeofday(&tv, NULL);
            uint32_t *slot = gtc[GTC_R0] ? (uint32_t *)guest_arena_to_host(arena, gtc[GTC_R0]) : NULL;
            if (slot) { slot[0] = (uint32_t)tv.tv_sec; slot[1] = (uint32_t)tv.tv_usec; }
            gtc[GTC_R0] = (uint32_t)tv.tv_sec;
            gtc[GTC_R1] = (uint32_t)tv.tv_usec;
            gtc[GTC_FIELD_CPSR] &= ~CPSR_C;
            break;
        }
        case 20: // SYS_getpid
            gtc[GTC_R0] = (uint32_t)getpid();
            gtc[GTC_FIELD_CPSR] &= ~CPSR_C;
            break;
        case 24: // SYS_getuid
        case 25: // SYS_geteuid
        case 43: // SYS_getegid
        case 47: // SYS_getgid
            { // mobile user by default; IOS6_UID / IOS6_GID override
                const char *e = getenv((sysno == 24 || sysno == 25) ? "IOS6_UID" : "IOS6_GID");
                gtc[GTC_R0] = e ? (uint32_t)atoi(e) : 501;
            }
            gtc[GTC_FIELD_CPSR] &= ~CPSR_C;
            break;
        case 37: // SYS_ioctl
        case 54: // SYS_ioctl (alternative BSD number)
            gtc[GTC_R0] = 0;
            gtc[GTC_FIELD_CPSR] &= ~CPSR_C;
            break;
        case 39: // SYS_getppid
            gtc[GTC_R0] = 1;
            gtc[GTC_FIELD_CPSR] &= ~CPSR_C;
            break;
        case 46: // SYS_sigaction
        case 48: // SYS_sigprocmask
            gtc[GTC_R0] = 0;
            gtc[GTC_FIELD_CPSR] &= ~CPSR_C;
            break;
        case 73: // SYS_munmap
            gtc[GTC_R0] = 0;
            gtc[GTC_FIELD_CPSR] &= ~CPSR_C;
            break;
        case 74: { // SYS_mprotect
            uint32_t addr = gtc[GTC_R0];
            size_t len = (gtc[GTC_R1] + 4095) & ~4095u;
            int prot_bits = (int)gtc[GTC_R2];
            int prot = PROT_EXEC;
            if (prot_bits & 1) prot |= PROT_READ;
            if (prot_bits & 2) prot |= PROT_WRITE;
            if (guest_arena_mprotect(arena, addr, len, prot) == 0) {
                gtc[GTC_R0] = 0;
                gtc[GTC_FIELD_CPSR] &= ~CPSR_C;
            } else {
                gtc[GTC_R0] = ENOMEM;
                gtc[GTC_FIELD_CPSR] |= CPSR_C;
            }
            break;
        }
        case 78: { // SYS_gettimeofday
            struct timeval tv;
            gettimeofday(&tv, NULL);
            uint32_t *slot = (uint32_t *)guest_arena_to_host(arena, gtc[GTC_R0]);
            if (slot) {
                slot[0] = (uint32_t)tv.tv_sec;
                slot[1] = (uint32_t)tv.tv_usec;
            }
            gtc[GTC_R0] = 0;
            gtc[GTC_FIELD_CPSR] &= ~CPSR_C;
            break;
        }
        case 197: { // SYS_mmap
            static uint32_t mmap_bump = 0x24000000;
            uint32_t req = gtc[GTC_R0] & ~4095u;
            size_t len = (gtc[GTC_R1] + 4095) & ~4095u;
            int prot_bits = (int)gtc[GTC_R2];
            uint32_t addr = (req != 0) ? req : mmap_bump;
            if (req == 0) mmap_bump += (uint32_t)len;

            int prot = PROT_EXEC;
            if (prot_bits & 1) prot |= PROT_READ;
            if (prot_bits & 2) prot |= PROT_WRITE;

            if (guest_arena_mprotect(arena, addr, len, prot) == 0) {
                void *mem = guest_arena_to_host(arena, addr);
                if (mem) memset(mem, 0, len);
                gtc[GTC_R0] = addr;
                gtc[GTC_FIELD_CPSR] &= ~CPSR_C;
            } else {
                gtc[GTC_R0] = ENOMEM;
                gtc[GTC_FIELD_CPSR] |= CPSR_C;
            }
            break;
        }
        case 327: // SYS_issetugid
            gtc[GTC_R0] = 0;
            gtc[GTC_FIELD_CPSR] &= ~CPSR_C;
            break;
        case 372: // SYS_thread_selfid (iOS 6 numbering) -> uint64 tid in r0:r1
        case 423:
            gtc[GTC_R0] = 0x101;
            gtc[GTC_R1] = 0;
            gtc[GTC_FIELD_CPSR] &= ~CPSR_C;
            break;
        /* ---- filesystem (paths mapped into IOS6_ROOT) ---- */
#define GPATH(reg, buf) guest_path((const char *)guest_arena_to_host(arena, gtc[reg]), buf, sizeof buf)
        case 136: { char b[1024]; set_result(gtc, mkdir(GPATH(0, b), (mode_t)gtc[1])); break; }
        case 137: { char b[1024]; set_result(gtc, rmdir(GPATH(0, b))); break; }
        case 10:  { char b[1024]; set_result(gtc, unlink(GPATH(0, b))); break; }
        case 128: { char a[1024], b[1024]; const char *pa = GPATH(0, a); set_result(gtc, rename(pa, GPATH(1, b))); break; }
        case 33:  { char b[1024]; set_result(gtc, access(GPATH(0, b), (int)gtc[1] & 7)); break; }
        case 15:  { char b[1024]; set_result(gtc, chmod(GPATH(0, b), (mode_t)gtc[1])); break; }
        case 124: set_result(gtc, fchmod((int)gtc[0], (mode_t)gtc[1])); break;
        case 60:  gtc[GTC_R0] = (uint32_t)umask((mode_t)gtc[0]); gtc[GTC_FIELD_CPSR] &= ~CPSR_C; break;
        case 95:  set_result(gtc, fsync((int)gtc[0])); break;
        case 12:  { char b[1024]; set_result(gtc, chdir(GPATH(0, b))); break; }
        case 13:  set_result(gtc, fchdir((int)gtc[0])); break;
        case 57:  { char a[1024]; const char *tgt = (const char *)guest_arena_to_host(arena, gtc[0]);
                    set_result(gtc, symlink(tgt, GPATH(1, a))); break; }
        case 58:  { char b[1024]; char *out = (char *)guest_arena_to_host(arena, gtc[1]);
                    set_result(gtc, out ? readlink(GPATH(0, b), out, gtc[2]) : (errno = EFAULT, -1)); break; }
        case 201: { // ftruncate(fd, off64 r1:r2)
            int64_t off = (int64_t)((uint64_t)gtc[2] << 32 | gtc[1]);
            set_result(gtc, ftruncate((int)gtc[0], off)); break;
        }
        case 199: { // lseek(fd, off64 r1:r2, whence r3) -> off64 r0:r1
            int64_t off = (int64_t)((uint64_t)gtc[2] << 32 | gtc[1]);
            off_t r = lseek((int)gtc[0], off, (int)gtc[3]);
            if (r < 0) { gtc[GTC_R0] = darwin_errno(errno); gtc[GTC_FIELD_CPSR] |= CPSR_C; }
            else { gtc[GTC_R0] = (uint32_t)r; gtc[GTC_R1] = (uint32_t)((uint64_t)r >> 32); gtc[GTC_FIELD_CPSR] &= ~CPSR_C; }
            break;
        }
        case 153: case 154: { // pread/pwrite(fd, buf, n, off64 r3:r4)
            int64_t off = (int64_t)((uint64_t)gtc[4] << 32 | gtc[3]);
            void *buf = guest_arena_to_host(arena, gtc[1]);
            ssize_t r = !buf ? (errno = EFAULT, -1) : sysno == 153 ? pread((int)gtc[0], buf, gtc[2], off)
                                                                   : pwrite((int)gtc[0], buf, gtc[2], off);
            set_result(gtc, r); break;
        }
        case 92: case 406: { // fcntl / fcntl_nocancel
            int fd = (int)gtc[0], cmd = (int)gtc[1];
            long r;
            switch (cmd) {
                case 0: r = fcntl(fd, F_DUPFD, (int)gtc[2]); break;
                case 1: r = fcntl(fd, F_GETFD); break;
                case 2: r = fcntl(fd, F_SETFD, (int)gtc[2]); break;
                case 3: { // F_GETFL: translate Linux -> Darwin flags
                    long f = fcntl(fd, F_GETFL);
                    r = f < 0 ? f : (f & 3) | ((f & O_NONBLOCK) ? 4 : 0) | ((f & O_APPEND) ? 8 : 0);
                    break;
                }
                case 4: r = fcntl(fd, F_SETFL, linux_open_flags(gtc[2])); break;
                case 67: r = fcntl(fd, F_DUPFD_CLOEXEC, (int)gtc[2]); break;
                case 48: case 51: case 7: case 8: case 9: r = 0; break; // F_NOCACHE/F_FULLFSYNC/locks: accept
                case 50: { // F_GETPATH
                    char lnk[64], *out = (char *)guest_arena_to_host(arena, gtc[2]);
                    snprintf(lnk, sizeof lnk, "/proc/self/fd/%d", fd);
                    ssize_t n = out ? readlink(lnk, out, 1023) : -1;
                    if (n >= 0) out[n] = 0;
                    r = n < 0 ? -1 : 0;
                    break;
                }
                default: errno = EINVAL; r = -1; break;
            }
            set_result(gtc, r); break;
        }
        case 196: case 344: { // getdirentries / getdirentries64(fd, buf, n, long *basep)
            // Darwin struct dirent64: ino u64, seekoff u64, reclen u16, namlen u16, type u8, name[]
            int fd = (int)gtc[0];
            uint8_t *out = (uint8_t *)guest_arena_to_host(arena, gtc[1]);
            uint32_t cap = gtc[2];
            static uint8_t lbuf[32768];
            long got = syscall(SYS_getdents64, fd, lbuf, (cap < sizeof lbuf ? cap : sizeof lbuf) / 2);
            if (got < 0 || !out) { set_result(gtc, -1); break; }
            uint32_t w = 0;
            for (long o = 0; o < got;) {
                uint64_t ino = *(uint64_t *)(lbuf + o);
                int64_t  doff = *(int64_t *)(lbuf + o + 8);
                uint16_t lrec = *(uint16_t *)(lbuf + o + 16);
                uint8_t  type = lbuf[o + 18];
                const char *name = (const char *)lbuf + o + 19;
                uint16_t nl = (uint16_t)strlen(name);
                uint16_t rec = (uint16_t)((21 + nl + 1 + 7) & ~7);
                if (w + rec > cap) { lseek(fd, doff, SEEK_SET); break; }
                memset(out + w, 0, rec);
                memcpy(out + w, &ino, 8); memcpy(out + w + 8, &doff, 8);
                memcpy(out + w + 16, &rec, 2); memcpy(out + w + 18, &nl, 2);
                out[w + 20] = type; memcpy(out + w + 21, name, nl);
                w += rec; o += lrec;
            }
            uint32_t *basep = gtc[3] ? (uint32_t *)guest_arena_to_host(arena, gtc[3]) : NULL;
            if (basep) basep[0] = 0;
            set_result(gtc, w); break;
        }
#undef GPATH
        case 345: case 346: { // statfs64(path, buf) / fstatfs64(fd, buf): Darwin struct statfs (2168 B)
            struct statvfs v;
            int r;
            if (sysno == 346) r = fstatvfs((int)gtc[0], &v);
            else { char b[1024]; const char *pp = guest_path((const char *)guest_arena_to_host(arena, gtc[0]), b, sizeof b);
                   r = pp ? statvfs(pp, &v) : (errno = EFAULT, -1); }
            uint8_t *o = (uint8_t *)guest_arena_to_host(arena, gtc[1]);
            if (r != 0 || !o) { set_result(gtc, -1); break; }
            memset(o, 0, 2168);
            uint32_t bsize = (uint32_t)v.f_bsize ? (uint32_t)v.f_bsize : 4096;
            int32_t iosize = (int32_t)bsize;
            uint64_t blocks = v.f_blocks * (v.f_frsize / (v.f_bsize ? v.f_bsize : 1) ?: 1);
            uint64_t bfree = v.f_bfree, bavail = v.f_bavail, files = v.f_files, ffree = v.f_ffree;
            uint32_t type = 17, flags = 0x1000; // HFS-ish type, MNT_LOCAL
            memcpy(o + 0, &bsize, 4); memcpy(o + 4, &iosize, 4);
            memcpy(o + 8, &blocks, 8); memcpy(o + 16, &bfree, 8); memcpy(o + 24, &bavail, 8);
            memcpy(o + 32, &files, 8); memcpy(o + 40, &ffree, 8);
            memcpy(o + 60, &type, 4); memcpy(o + 64, &flags, 4);
            strcpy((char *)o + 72, "hfs");
            strcpy((char *)o + 88, "/");
            strcpy((char *)o + 1112, "/dev/disk0s1s1");
            set_result(gtc, 0); break;
        }
        case 347: { // getfsstat64(buf, bufsize, flags)
            uint32_t buf_addr = gtc[0];
            uint32_t bufsize = gtc[1];
            if (buf_addr == 0 || bufsize == 0) {
                gtc[GTC_R0] = 1;
                gtc[GTC_FIELD_CPSR] &= ~CPSR_C;
                break;
            }
            if (bufsize < 2168) {
                gtc[GTC_R0] = 0;
                gtc[GTC_FIELD_CPSR] &= ~CPSR_C;
                break;
            }
            uint8_t *o = (uint8_t *)guest_arena_to_host(arena, buf_addr);
            if (!o) { set_result(gtc, -1); break; }
            struct statvfs v;
            memset(&v, 0, sizeof v);
            statvfs("/", &v);
            memset(o, 0, 2168);
            uint32_t bsize = (uint32_t)v.f_bsize ? (uint32_t)v.f_bsize : 4096;
            int32_t iosize = (int32_t)bsize;
            uint64_t blocks = v.f_blocks * (v.f_frsize / (v.f_bsize ? v.f_bsize : 1) ?: 1);
            uint64_t bfree = v.f_bfree, bavail = v.f_bavail, files = v.f_files, ffree = v.f_ffree;
            uint32_t type = 17, flags = 0x1000;
            memcpy(o + 0, &bsize, 4); memcpy(o + 4, &iosize, 4);
            memcpy(o + 8, &blocks, 8); memcpy(o + 16, &bfree, 8); memcpy(o + 24, &bavail, 8);
            memcpy(o + 32, &files, 8); memcpy(o + 40, &ffree, 8);
            memcpy(o + 60, &type, 4); memcpy(o + 64, &flags, 4);
            strcpy((char *)o + 72, "hfs");
            strcpy((char *)o + 88, "/");
            strcpy((char *)o + 1112, "/dev/disk0s1s1");
            gtc[GTC_R0] = 1;
            gtc[GTC_FIELD_CPSR] &= ~CPSR_C;
            break;
        }
        case 97: { // SYS_socket(domain, type, protocol)
            int fd = socket((int)gtc[0], (int)gtc[1], (int)gtc[2]);
            set_result(gtc, fd);
            break;
        }
        case 220: // SYS_getattrlist: unsupported -> callers (realpath) fall back to stat
        case 221: // SYS_setattrlist
            gtc[GTC_R0] = 45; // ENOTSUP
            gtc[GTC_FIELD_CPSR] |= CPSR_C;
            break;
        case 42: { // SYS_pipe: Darwin returns the two descriptors in r0/r1
            int fds[2];
            if (pipe(fds) != 0) { set_result(gtc, -1); break; }
            gtc[GTC_R0] = (uint32_t)fds[0];
            gtc[GTC_R1] = (uint32_t)fds[1];
            gtc[GTC_FIELD_CPSR] &= ~CPSR_C;
            break;
        }
        case 35: // SYS_fchflags(fd, flags) - BSD file flags; harmless no-op on Linux
            set_result(gtc, 0);
            break;
        case 123: { // SYS_fchown(fd, uid, gid)
            int r = fchown((int)gtc[0], (uid_t)gtc[1], (gid_t)gtc[2]);
            if (r != 0 && errno == EPERM) r = 0; // ignore ownership errors in unprivileged mode
            set_result(gtc, r);
            break;
        }
        case 139: { // SYS_futimes(fd, tptr)
            const struct timeval *tv = NULL;
            struct timeval tv_buf[2];
            if (gtc[1]) {
                const uint32_t *src = (const uint32_t *)guest_arena_to_host(arena, gtc[1]);
                if (src) {
                    tv_buf[0].tv_sec = (time_t)src[0];
                    tv_buf[0].tv_usec = (suseconds_t)src[1];
                    tv_buf[1].tv_sec = (time_t)src[2];
                    tv_buf[1].tv_usec = (suseconds_t)src[3];
                    tv = tv_buf;
                }
            }
            int r = futimes((int)gtc[0], tv);
            set_result(gtc, r);
            break;
        }
        case 364: { // SYS_lchown(path, uid, gid)
            char buf[1024];
            const char *p = guest_path((const char *)guest_arena_to_host(arena, gtc[0]), buf, sizeof buf);
            int r = -1;
            if (p) {
                r = lchown(p, (uid_t)gtc[1], (gid_t)gtc[2]);
                if (r != 0 && errno == EPERM) r = 0;
            } else {
                errno = EFAULT;
            }
            set_result(gtc, r);
            break;
        }
        case 366: // SYS_bsdthread_register: accept, we never create kernel threads
            gtc[GTC_R0] = 0;
            gtc[GTC_FIELD_CPSR] &= ~CPSR_C;
            break;
        case 194: { // SYS_getrlimit(resource, struct rlimit *) : two 64-bit values
            uint32_t *rl = (uint32_t *)guest_arena_to_host(arena, gtc[GTC_R1]);
            uint32_t res = gtc[GTC_R0] & 0xff;
            uint64_t cur = ~0ull, max = ~0ull;
            if (res == 3) { cur = 8u << 20; max = 64u << 20; }   // RLIMIT_STACK
            if (res == 8) { cur = 256; max = ~0ull; }             // RLIMIT_NOFILE
            if (rl) { memcpy(rl, &cur, 8); memcpy(rl + 2, &max, 8); }
            gtc[GTC_R0] = 0;
            gtc[GTC_FIELD_CPSR] &= ~CPSR_C;
            break;
        }
        case 338: // SYS_stat64(path, st)
        case 339: // SYS_fstat64(fd, st)
        case 340: { // SYS_lstat64(path, st)
            struct stat hst;
            int r;
            if (sysno == 339) {
                r = fstat((int)gtc[GTC_R0], &hst);
            } else {
                const char *path = (const char *)guest_arena_to_host(arena, gtc[GTC_R0]);
                r = path ? (sysno == 338 ? stat(path, &hst) : lstat(path, &hst)) : -1;
            }
            uint8_t *st = (uint8_t *)guest_arena_to_host(arena, gtc[GTC_R1]);
            if (r != 0 || !st) {
                gtc[GTC_R0] = (uint32_t)(r != 0 ? errno : EFAULT);
                gtc[GTC_FIELD_CPSR] |= CPSR_C;
                break;
            }
            // Darwin struct stat64 (armv7): 108 bytes
            memset(st, 0, 108);
            uint32_t dev = (uint32_t)hst.st_dev; uint16_t mode = (uint16_t)hst.st_mode;
            uint16_t nlink = (uint16_t)hst.st_nlink; uint64_t ino = hst.st_ino;
            uint32_t uid = hst.st_uid, gid = hst.st_gid, rdev = (uint32_t)hst.st_rdev;
            int32_t ts[8] = { (int32_t)hst.st_atim.tv_sec, (int32_t)hst.st_atim.tv_nsec,
                              (int32_t)hst.st_mtim.tv_sec, (int32_t)hst.st_mtim.tv_nsec,
                              (int32_t)hst.st_ctim.tv_sec, (int32_t)hst.st_ctim.tv_nsec,
                              (int32_t)hst.st_ctim.tv_sec, (int32_t)hst.st_ctim.tv_nsec };
            int64_t size = hst.st_size, blocks = hst.st_blocks;
            int32_t blksize = (int32_t)hst.st_blksize;
            memcpy(st + 0, &dev, 4); memcpy(st + 4, &mode, 2); memcpy(st + 6, &nlink, 2);
            memcpy(st + 8, &ino, 8); memcpy(st + 16, &uid, 4); memcpy(st + 20, &gid, 4);
            memcpy(st + 24, &rdev, 4); memcpy(st + 28, ts, 32);
            memcpy(st + 60, &size, 8); memcpy(st + 68, &blocks, 8); memcpy(st + 76, &blksize, 4);
            gtc[GTC_R0] = 0;
            gtc[GTC_FIELD_CPSR] &= ~CPSR_C;
            break;
        }
        case 202: { // SYS___sysctl
            uint32_t *mib = (uint32_t *)guest_arena_to_host(arena, gtc[GTC_R0]);
            uint32_t n = gtc[GTC_R1];
            if (getenv("IOS6_TRACE")) {
                fprintf(stderr, "sysctl mib:");
                for (uint32_t i = 0; mib && i < n && i < 6; i++) fprintf(stderr, " %u", mib[i]);
                fprintf(stderr, "\n");
            }
            uint32_t oldp = gtc[GTC_R2], oldlenp = gtc[GTC_R3];
            uint32_t *olen = oldlenp ? (uint32_t *)guest_arena_to_host(arena, oldlenp) : NULL;
            uint32_t *out = oldp ? (uint32_t *)guest_arena_to_host(arena, oldp) : NULL;
            if (mib && n == 2 && mib[0] == 6 && (mib[1] == 3 || mib[1] == 65 || mib[1] == 25 || mib[1] == 103)) {
                // HW_NCPU / HW_AVAILCPU: 1
                if (out && olen && *olen >= 4) *out = 1;
                if (olen) *olen = 4;
                gtc[GTC_R0] = 0;
                gtc[GTC_FIELD_CPSR] &= ~CPSR_C;
            } else if (mib && n == 2 && mib[0] == 6 && mib[1] == 7) {
                // HW_PAGESIZE
                if (out && olen && *olen >= 4) *out = 4096;
                if (olen) *olen = 4;
                gtc[GTC_R0] = 0;
                gtc[GTC_FIELD_CPSR] &= ~CPSR_C;
            } else if (mib && n == 2 && mib[0] == 6 && (mib[1] == 1 || mib[1] == 2)) {
                // HW_MACHINE / HW_MODEL strings
                const char *str = mib[1] == 1 ? "iPhone4,1" : "N94AP";
                uint32_t need = (uint32_t)strlen(str) + 1;
                if (!out) {
                    if (olen) *olen = need;
                    gtc[GTC_R0] = 0;
                    gtc[GTC_FIELD_CPSR] &= ~CPSR_C;
                } else if (olen && *olen >= need) {
                    memcpy(out, str, need);
                    *olen = need;
                    gtc[GTC_R0] = 0;
                    gtc[GTC_FIELD_CPSR] &= ~CPSR_C;
                } else {
                    gtc[GTC_R0] = ENOMEM;
                    gtc[GTC_FIELD_CPSR] |= CPSR_C;
                }
            } else {
                gtc[GTC_R0] = 2; // ENOENT
                gtc[GTC_FIELD_CPSR] |= CPSR_C;
            }
            break;
        }
        default:
            fprintf(stderr, "Unhandled syscall %u (r12)\n", sysno);
            gtc[GTC_R0] = 78; // ENOSYS
            gtc[GTC_FIELD_CPSR] |= CPSR_C;
            break;
    }

    // Resume execution
    gtc[GTC_FIELD_NEXT_PC] = gtc[GTC_FIELD_TRAP_RESUME];
    return 0;
}
