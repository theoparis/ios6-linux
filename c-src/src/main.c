#include "ios6/guestmem.h"
#include "ios6/macho.h"
#include "ios6/shared_cache.h"
#include "ios6/runtime.h"
#include "ios6/hooks.h"
#include "ios6/jit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    shared_cache_t *cache;
    loaded_image_t *alpm_image;
} multi_resolver_ctx_t;

static uint32_t multi_resolver(const char *name, void *user_data) {
    multi_resolver_ctx_t *ctx = (multi_resolver_ctx_t *)user_data;

    // 1. Shared cache (real iOS libSystem) takes priority
    if (ctx && ctx->cache) {
        uint32_t c_addr = shared_cache_resolve(ctx->cache, "/usr/lib/libSystem.B.dylib", name);
        if (c_addr != 0) {
            if (getenv("IOS6_TRACE_BIND")) fprintf(stderr, "bind %s -> cache 0x%08x\n", name, c_addr);
            return c_addr;
        }
    }

    // 1b. Other system dylibs in the cache that the target-root libraries link against
    if (ctx && ctx->cache) {
        static const char *extra_dylibs[] = {
            "/usr/lib/libz.1.dylib", "/usr/lib/libbz2.1.0.dylib", "/usr/lib/libiconv.2.dylib",
            "/usr/lib/libresolv.9.dylib", "/usr/lib/libxml2.2.dylib", "/usr/lib/libsqlite3.dylib",
        };
        for (size_t i = 0; i < sizeof(extra_dylibs) / sizeof(extra_dylibs[0]); i++) {
            uint32_t a = shared_cache_resolve(ctx->cache, extra_dylibs[i], name);
            if (a != 0) {
                if (getenv("IOS6_TRACE_BIND")) fprintf(stderr, "bind %s -> cache(%s) 0x%08x\n", name, extra_dylibs[i], a);
                return a;
            }
        }
    }

    // 1c. Previously loaded target-root dylib (libalpm)
    if (ctx && ctx->alpm_image) {
        uint32_t off = export_table_lookup(&ctx->alpm_image->exports, name);
        if (off != 0) {
            uint32_t a = ctx->alpm_image->slide + off;
            if (getenv("IOS6_TRACE_BIND")) fprintf(stderr, "bind %s -> libalpm 0x%08x\n", name, a);
            return a;
        }
    }

    // 2. Host runtime builtins / stubs for anything the cache lacks
    uint32_t r_addr = runtime_resolve_symbol(name, NULL);
    if (r_addr != 0) {
        if (getenv("IOS6_TRACE_BIND")) fprintf(stderr, "bind %s -> host 0x%08x\n", name, r_addr);
        return r_addr;
    }

    if (getenv("IOS6_TRACE_BIND")) fprintf(stderr, "bind %s -> UNRESOLVED\n", name);
    return 0;
}

/*
 * Run the cache's libSystem initialisers (__mod_init_func) in dependency order, the way dyld
 * would before jumping to the executable's entry point. Each is run to completion.
 */
static void run_libsystem_initializers(jit_runner_t *runner, shared_cache_t *cache) {
    static const char *order[] = {
        "/usr/lib/system/libsystem_kernel.dylib",
        "/usr/lib/system/libsystem_c.dylib",
        "/usr/lib/system/libsystem_blocks.dylib",
        "/usr/lib/system/libdyld.dylib",
        "/usr/lib/system/libdispatch.dylib",
        "/usr/lib/system/libsystem_info.dylib",
        "/usr/lib/system/libsystem_notify.dylib",
        "/usr/lib/libSystem.B.dylib",
    };
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        uint32_t fns[64];
        int n = shared_cache_mod_init_funcs(cache, order[i], fns, 64);
        if (n <= 0) {
            printf("  init %s: no mod_init funcs\n", order[i]);
            continue;
        }
        for (int k = 0; k < n; k++) {
            uint32_t rv = 0;
            // dyld signature: init(argc, argv, envp, apple, const ProgramVars *vars)
            uint32_t *pinfo = (uint32_t *)guest_arena_to_host(runner->arena, PROCESS_INFO_BASE);
            uint32_t argv_a = pinfo[1], envp_a = pinfo[2];
            uint32_t apple_a = envp_a + 4; // envp is just {NULL}; apple follows
            uint32_t *apple = (uint32_t *)guest_arena_to_host(runner->arena, apple_a);
            apple[0] = ((uint32_t *)guest_arena_to_host(runner->arena, argv_a))[0];
            apple[1] = 0;
            // ProgramVars {mh, &NXArgc, &NXArgv, &environ, &__progname}
            uint32_t vars_a = PROCESS_INFO_BASE + 0x100;
            uint32_t *vars = (uint32_t *)guest_arena_to_host(runner->arena, vars_a);
            vars[0] = pinfo[3];
            vars[1] = PROCESS_INFO_BASE;
            vars[2] = PROCESS_INFO_BASE + 4;
            vars[3] = PROCESS_INFO_BASE + 8;
            vars[4] = PROCESS_INFO_BASE + 16;
            pinfo[4] = apple[0]; // __progname
            uint32_t sp_save = runner->gtc[GTC_SP];
            runner->gtc[GTC_SP] -= 16;
            *(uint32_t *)guest_arena_to_host(runner->arena, runner->gtc[GTC_SP]) = vars_a;
            uint32_t args[4] = { pinfo[0], argv_a, envp_a, apple_a };
            int rc = jit_runner_call(runner, fns[k], args, 4, 5000000, &rv);
            runner->gtc[GTC_SP] = sp_save;
            printf("  init %s[%d] @0x%08x -> %s (r0=0x%x)\n", order[i], k, fns[k],
                   rc == 0 ? "ok" : "FAILED", rv);
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <path-to-armv7-mach-o> [args...]\n", argv[0]);
        return 1;
    }

    const char *binary_path = argv[1];
    // Default guest sysroot: absolute guest paths are resolved under it.
    if (!getenv("IOS6_ROOT")) {
        char rbuf[512];
        const char *home = getenv("HOME");
        snprintf(rbuf, sizeof rbuf, "%s/src/target-root", home ? home : "");
        struct stat rst;
        if (stat(rbuf, &rst) == 0) setenv("IOS6_ROOT", rbuf, 0);
    }

    printf("=== iOS 6 C + GNU Lightning JIT Runner ===\n");
    printf("Initializing 4 GiB guest memory arena...\n");

    guest_arena_t arena;
    if (guest_arena_reserve(&arena) != 0) {
        fprintf(stderr, "Error: failed to reserve guest arena [0x10000, 4 GiB)\n");
        return 1;
    }

    host_stubs_t stubs;
    if (runtime_install_stubs(&arena, &stubs) != 0) {
        fprintf(stderr, "Error: failed to install host stubs and runtime environment\n");
        return 1;
    }

    multi_resolver_ctx_t resolver_ctx = {0};

    // If dyld shared cache exists, map it in
    const char *dsc_path = "/home/theo/.cache/dmgref/BrightonMaps10B329.K93OS/System/Library/Caches/com.apple.dyld/dyld_shared_cache_armv7";
    shared_cache_t *cache = (shared_cache_t *)malloc(sizeof(shared_cache_t));
    if (cache && shared_cache_open(cache, dsc_path) == 0) {
        printf("Found iOS 6 dyld shared cache! Mapping into arena...\n");
        shared_cache_map_into_arena(cache, &arena);
        resolver_ctx.cache = cache;
        runtime_patch_for_cache(&arena);
        hooks_init(&arena);
        int nh = hooks_install_from_cache(cache);
        printf("Installed %d native libc hooks.\n", nh);
        printf("Shared cache mapped successfully.\n");
    }

    // If executing pacman, load libalpm first
    loaded_image_t alpm_image;
    memset(&alpm_image, 0, sizeof(alpm_image));
    if (strstr(binary_path, "pacman")) {
        const char *alpm_path = "/home/theo/src/target-root/lib/libalpm.16.dylib";
        printf("Loading dependent dylib: %s\n", alpm_path);
        if (macho_load_file(&arena, alpm_path, multi_resolver, &resolver_ctx, &alpm_image) == 0) {
            printf("Loaded libalpm successfully at base 0x%08x\n", alpm_image.base_addr);
            resolver_ctx.alpm_image = &alpm_image;
        } else {
            fprintf(stderr, "Warning: failed to load %s\n", alpm_path);
        }
    }

    printf("Loading Mach-O binary: %s\n", binary_path);
    loaded_image_t image;
    if (macho_load_file(&arena, binary_path, multi_resolver, &resolver_ctx, &image) != 0) {
        fprintf(stderr, "Error: failed to load Mach-O binary %s\n", binary_path);
        return 1;
    }

    printf("Loaded image successfully: base=0x%08x entry_point=0x%08x (slide=0x%08x)\n",
           image.base_addr, image.entry_point, image.slide);

    uint32_t sp = 0, argv_addr = 0;
    runtime_setup_stack(&arena, argc - 1, &argv[1], &sp, &argv_addr);

    printf("Initializing GNU Lightning JIT compilation engine...\n");
    jit_runner_t runner;
    if (jit_runner_init(&runner, &arena, image.entry_point, sp, image.slide) != 0) {
        fprintf(stderr, "Error: failed to initialize JIT runner\n");
        return 1;
    }

    if (cache) {
        run_libsystem_initializers(&runner, cache);
        if (getenv("IOS6_SELFTEST")) {
            // Call guest snprintf(buf, 64, fmt, "XY", 42) and show the result.
            uint32_t fn = shared_cache_resolve(cache, "/usr/lib/libSystem.B.dylib", "_snprintf");
            uint32_t scratch = 0x21009c00;
            char *h = (char *)guest_arena_to_host(&arena, scratch);
            const char *fmt = getenv("IOS6_SELFTEST");
            if (!*fmt || !strcmp(fmt, "1")) fmt = "a%sb%dc|%5d|%x";
            strcpy(h + 0x100, fmt);
            strcpy(h + 0x180, "XY");
            uint32_t sp_save = runner.gtc[GTC_SP];
            runner.gtc[GTC_SP] -= 16;
            uint32_t *stk = (uint32_t *)guest_arena_to_host(&arena, runner.gtc[GTC_SP]);
            stk[0] = 42; stk[1] = 7; stk[2] = 0xbeef;
            uint32_t args[4] = { scratch, 64, scratch + 0x100, scratch + 0x180 };
            if (getenv("IOS6_SELFTEST_D")) { // pass a double in r2:r3 (AAPCS variadic)
                double dv = atof(getenv("IOS6_SELFTEST_D"));
                uint64_t bits; memcpy(&bits, &dv, 8);
                // Darwin armv7: variadic doubles are not pair-aligned -> low word in r3, high on stack
                args[3] = (uint32_t)bits;
                stk[0] = (uint32_t)(bits >> 32);
                stk[1] = 99;
            }
            uint32_t rv = 0;
            int rc = jit_runner_call(&runner, fn, args, 4, 5000000, &rv);
            runner.gtc[GTC_SP] = sp_save;
            printf("SELFTEST snprintf(\"%s\") rc=%d ret=%u -> \"%s\"\n", fmt, rc, rv, h);
            return 0;
        }
    }

    runner.gtc[GTC_R0] = (uint32_t)(argc - 1); // argc
    runner.gtc[GTC_R1] = argv_addr;            // argv

    printf("Executing binary natively via GNU Lightning JIT from entry point 0x%08x...\n\n", image.entry_point);

    int result = jit_runner_run(&runner, (size_t)-1);
    printf("\nExecution completed with result code: %d\n", result);

    jit_runner_destroy(&runner);
    if (cache) {
        shared_cache_close(cache);
        free(cache);
    }
    return 0;
}
