/*
 * test_iop_hle_modules.c - host-native test for source/hw/iop_hle_modules.c
 *
 * A plain registry, so a plain test: register/query roundtrip,
 * re-registering the same name updates its version in place instead
 * of duplicating it, unknown names correctly report as not
 * registered, and basic input validation (NULL/empty name).
 */
#include <stdio.h>
#include <string.h>

#include "hw/iop_hle_modules.c"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else printf("ok:   %s\n", msg); \
} while (0)

int main(void)
{
    iop_hle_modules_init();

    uint32_t version;

    CHECK(iop_hle_module_is_registered("sio2man", NULL) == 0,
          "nothing is registered right after init");

    CHECK(iop_hle_module_register("sio2man", 0x0101) == 1, "registering a new module succeeds");
    CHECK(iop_hle_module_register("mcman", 0x0200) == 1, "registering a second module succeeds");
    CHECK(g_registry.count == 2, "registry count reflects both registrations");

    CHECK(iop_hle_module_is_registered("sio2man", &version) == 1 && version == 0x0101u,
          "sio2man is registered with the correct version");
    CHECK(iop_hle_module_is_registered("mcman", &version) == 1 && version == 0x0200u,
          "mcman is registered with the correct version");
    CHECK(iop_hle_module_is_registered("padman", NULL) == 0,
          "a module that was never registered correctly reports as not registered");

    /* Re-registering the same name updates in place, doesn't duplicate */
    CHECK(iop_hle_module_register("sio2man", 0x0102) == 1, "re-registering an existing module succeeds");
    CHECK(g_registry.count == 2, "re-registering an existing module does NOT grow the registry");
    iop_hle_module_is_registered("sio2man", &version);
    CHECK(version == 0x0102u, "re-registering updates the version in place");

    /* Input validation */
    CHECK(iop_hle_module_register(NULL, 1) == 0, "registering a NULL name is rejected");
    CHECK(iop_hle_module_register("", 1) == 0, "registering an empty name is rejected");
    CHECK(iop_hle_module_is_registered(NULL, NULL) == 0, "querying a NULL name returns not-registered rather than crashing");

    /* Long name truncation doesn't overflow/crash */
    char long_name[128];
    memset(long_name, 'x', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    CHECK(iop_hle_module_register(long_name, 5) == 1, "registering an over-long name succeeds (gets truncated)");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
