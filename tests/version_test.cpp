#include <gtest/gtest.h>

#include "epollo/version.hpp"

TEST(Version, NotEmpty) { EXPECT_FALSE(epollo::version().empty()); }
