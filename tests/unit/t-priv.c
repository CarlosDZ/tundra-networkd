#include "priv.c"
#include "test.h"

#define BIT(c) (1ULL << (c))
#define CHILD                                                                  \
	(BIT(CAP_NET_ADMIN) | BIT(CAP_NET_RAW) | BIT(CAP_NET_BIND_SERVICE))

int main(void) {
	struct priv_caps unit = {CHILD, CHILD, CHILD};
	struct priv_caps k = priv_shed_sets(unit);
	CHECK(k.effective == 0 && k.permitted == 0,
	      "nothing the daemon can use is left");
	CHECK(k.inheritable == CHILD,
	      "the capabilities of the unit stay as inheritable for the children");

	struct priv_caps more = {~0ULL, ~0ULL, 0};
	k = priv_shed_sets(more);
	CHECK(k.inheritable == CHILD && k.permitted == 0 && k.effective == 0,
	      "any other capability is dropped, inheritable too");

	struct priv_caps one = {BIT(CAP_NET_RAW), BIT(CAP_NET_RAW), 0};
	k = priv_shed_sets(one);
	CHECK(k.inheritable == BIT(CAP_NET_RAW),
	      "a capability the daemon never had is not invented");

	struct priv_caps inh = {0, 0, BIT(CAP_NET_ADMIN) | BIT(CAP_SYS_ADMIN)};
	k = priv_shed_sets(inh);
	CHECK(k.inheritable == BIT(CAP_NET_ADMIN),
	      "an inheritable one is kept only if a child needs it");

	struct priv_caps none = {0, 0, 0};
	k = priv_shed_sets(none);
	CHECK(k.effective == 0 && k.permitted == 0 && k.inheritable == 0,
	      "without capabilities nothing changes");
	return test_summary();
}
