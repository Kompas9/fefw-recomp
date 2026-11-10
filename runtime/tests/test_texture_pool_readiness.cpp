// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_framework.h"
#include "../src/gfx/nvn/texture_upload_readiness.h"

NXTEST_GROUP(texture_pool_readiness)

using nxrt::nvn::be::gpu::pool_preparation_required;

NXTEST(depth_already_in_sampled_layout_does_not_request_unsupported_upload) {
    CHECK(!pool_preparation_required(false, true, true));
    CHECK(!pool_preparation_required(true, true, true));
}

NXTEST(colour_cpu_writes_still_request_upload) {
    CHECK(!pool_preparation_required(false, false, true));
    CHECK(pool_preparation_required(true, false, true));
}

NXTEST(layout_dependencies_are_retained_for_colour_and_depth) {
    CHECK(pool_preparation_required(false, false, false));
    CHECK(pool_preparation_required(true, false, false));
    CHECK(pool_preparation_required(false, true, false));
    CHECK(pool_preparation_required(true, true, false));
}
