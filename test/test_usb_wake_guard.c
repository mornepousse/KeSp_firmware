/* Tests for the USB-enumeration-vs-automatic-light-sleep guard (pure logic). */
#include "test_framework.h"
#include "../main/comm/usb/usb_wake_guard.h"

static void test_first_activity_asks_for_the_lock(void)
{
    usb_wake_guard_t g = {0};
    TEST_ASSERT(usb_wake_guard_activity(&g, 1000) == true, "first activity: caller must acquire");
    TEST_ASSERT(g.holding, "now holding");
    TEST_ASSERT_EQ(g.deadline_ms, 1000 + USB_WAKE_GUARD_TIMEOUT_MS, "deadline armed from now");
}

static void test_repeated_activity_does_not_double_acquire(void)
{
    /* A burst (reset condition, many bit edges) must not ask the caller to
     * acquire the lock more than once — esp_pm locks are refcounted and an
     * unbalanced acquire would leak the lock held forever. */
    usb_wake_guard_t g = {0};
    usb_wake_guard_activity(&g, 1000);
    TEST_ASSERT(usb_wake_guard_activity(&g, 1001) == false, "second call in the same burst: no re-acquire");
    TEST_ASSERT(usb_wake_guard_activity(&g, 1900) == false, "still within the burst: no re-acquire");
}

static void test_activity_refreshes_the_deadline(void)
{
    /* A host that keeps talking must not get cut off mid-handshake. */
    usb_wake_guard_t g = {0};
    usb_wake_guard_activity(&g, 1000);
    usb_wake_guard_activity(&g, 2500);   /* well past the first deadline (1000+2000=3000)? no: 2500 < 3000, still mid-handshake */
    TEST_ASSERT_EQ(g.deadline_ms, 2500 + USB_WAKE_GUARD_TIMEOUT_MS, "deadline pushed out by renewed activity");
}

static void test_mount_releases_the_lock(void)
{
    usb_wake_guard_t g = {0};
    usb_wake_guard_activity(&g, 1000);
    TEST_ASSERT(usb_wake_guard_mounted(&g) == true, "mounted while holding: release owed");
    TEST_ASSERT(!g.holding, "no longer holding after mount");
}

static void test_mount_without_prior_activity_is_a_noop(void)
{
    /* e.g. the host was already mounted before the guard was ever armed:
     * nothing to release, and the caller must NOT be told to release a lock
     * it never acquired (unbalanced esp_pm_lock_release would assert/leak). */
    usb_wake_guard_t g = {0};
    TEST_ASSERT(usb_wake_guard_mounted(&g) == false, "nothing held: no release owed");
}

static void test_timeout_before_deadline_does_nothing(void)
{
    usb_wake_guard_t g = {0};
    usb_wake_guard_activity(&g, 1000);
    TEST_ASSERT(usb_wake_guard_timeout(&g, 1000 + USB_WAKE_GUARD_TIMEOUT_MS - 1) == false, "1ms before deadline: still holding");
    TEST_ASSERT(g.holding, "still holding");
}

static void test_timeout_at_deadline_releases_and_rearms(void)
{
    usb_wake_guard_t g = {0};
    usb_wake_guard_activity(&g, 1000);
    uint32_t deadline = 1000 + USB_WAKE_GUARD_TIMEOUT_MS;
    TEST_ASSERT(usb_wake_guard_timeout(&g, deadline) == true, "at the deadline: release owed");
    TEST_ASSERT(!g.holding, "released");
    /* re-armed: a later activity is a fresh "first" again */
    TEST_ASSERT(usb_wake_guard_activity(&g, deadline + 1) == true, "re-armed after timeout release");
}

static void test_timeout_when_not_holding_is_a_noop(void)
{
    usb_wake_guard_t g = {0};
    TEST_ASSERT(usb_wake_guard_timeout(&g, 999999) == false, "nothing held: no release owed, no repeated firing");
}

static void test_timeout_survives_uint32_wraparound(void)
{
    /* now_ms is esp_timer_get_time()/1000 cast to uint32_t: it wraps after
     * ~49 days of uptime. The signed-difference comparison must still order
     * "deadline" and "now" correctly across the wrap. */
    usb_wake_guard_t g = {0};
    uint32_t near_wrap = 0xFFFFFFFFu - 500u;
    usb_wake_guard_activity(&g, near_wrap);   /* deadline wraps past UINT32_MAX */
    uint32_t after_wrap = (uint32_t)(near_wrap + USB_WAKE_GUARD_TIMEOUT_MS - 1);  /* still "before" deadline, wrapped */
    TEST_ASSERT(usb_wake_guard_timeout(&g, after_wrap) == false, "1ms before deadline, across the wrap: still holding");
    uint32_t at_wrap_deadline = (uint32_t)(near_wrap + USB_WAKE_GUARD_TIMEOUT_MS);
    TEST_ASSERT(usb_wake_guard_timeout(&g, at_wrap_deadline) == true, "at the wrapped deadline: release owed");
}

/* VEILLE_VETO_USB_ENUM (veille_veto.h) must track the guard exactly: posted
 * the moment activity is seen, cleared the moment mount or timeout releases
 * the hold — across the whole lifecycle, not just at the endpoints, since
 * usb_wake_guard_tick() calls usb_wake_guard_veto_active() every ~1 Hz and a
 * single missed transition would leave the explicit sleep either unblocked
 * mid-handshake or permanently vetoed after a timeout. */
static void test_veto_follows_the_guard_state(void)
{
    usb_wake_guard_t g = {0};
    TEST_ASSERT(!usb_wake_guard_veto_active(&g), "idle: no veto");

    usb_wake_guard_activity(&g, 1000);
    TEST_ASSERT(usb_wake_guard_veto_active(&g), "activity: veto posted");

    usb_wake_guard_activity(&g, 1500);   /* mid-burst, still well before timeout */
    TEST_ASSERT(usb_wake_guard_veto_active(&g), "still mid-handshake: veto stays");

    TEST_ASSERT(usb_wake_guard_mounted(&g), "mounted: release owed");
    TEST_ASSERT(!usb_wake_guard_veto_active(&g), "mounted: veto cleared");

    usb_wake_guard_activity(&g, 2000);
    TEST_ASSERT(usb_wake_guard_veto_active(&g), "a later attempt: veto posted again");
    uint32_t deadline = 2000 + USB_WAKE_GUARD_TIMEOUT_MS;
    TEST_ASSERT(usb_wake_guard_timeout(&g, deadline), "host went away: timeout fires");
    TEST_ASSERT(!usb_wake_guard_veto_active(&g), "timed out: veto cleared, explicit sleep allowed again");
}

void test_usb_wake_guard(void)
{
    printf("\n-- usb_wake_guard --\n");
    test_first_activity_asks_for_the_lock();
    test_repeated_activity_does_not_double_acquire();
    test_activity_refreshes_the_deadline();
    test_mount_releases_the_lock();
    test_mount_without_prior_activity_is_a_noop();
    test_timeout_before_deadline_does_nothing();
    test_timeout_at_deadline_releases_and_rearms();
    test_timeout_when_not_holding_is_a_noop();
    test_timeout_survives_uint32_wraparound();
    test_veto_follows_the_guard_state();
}
