#include <gtest/gtest.h>

#include "app.hpp"

TEST(App, Banner)
{
    EXPECT_EQ(banner(), "feed-api");
}
