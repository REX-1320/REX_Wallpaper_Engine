#include <gtest/gtest.h>
#include "modules/DependencyResolver.hpp"

using namespace rex::modules;

TEST(DependencyResolverTest, LinearOrder) {
    DependencyResolver resolver;
    std::vector<DependencyResolver::ModuleInfo> modules = {
        {"A", {}}, {"B", {"A"}}, {"C", {"B"}}
    };
    auto order = resolver.resolveOrder(modules);
    ASSERT_EQ(order.size(), 3);
    EXPECT_EQ(order[0], "A");
    EXPECT_EQ(order[1], "B");
    EXPECT_EQ(order[2], "C");
}

TEST(DependencyResolverTest, DiamondOrder) {
    DependencyResolver resolver;
    std::vector<DependencyResolver::ModuleInfo> modules = {
        {"A", {}}, {"B", {"A"}}, {"C", {"A"}}, {"D", {"B", "C"}}
    };
    auto order = resolver.resolveOrder(modules);
    ASSERT_EQ(order.size(), 4);
    EXPECT_EQ(order[0], "A");
    EXPECT_NE(std::find(order.begin(), order.end(), "D"), order.end());
}

TEST(DependencyResolverTest, NoDependencies) {
    DependencyResolver resolver;
    std::vector<DependencyResolver::ModuleInfo> modules = {
        {"A", {}}, {"B", {}}, {"C", {}}
    };
    auto order = resolver.resolveOrder(modules);
    ASSERT_EQ(order.size(), 3);
}

TEST(DependencyResolverTest, CycleDetection) {
    DependencyResolver resolver;
    std::vector<DependencyResolver::ModuleInfo> modules = {
        {"A", {"B"}}, {"B", {"C"}}, {"C", {"A"}}
    };
    EXPECT_THROW(resolver.resolveOrder(modules), std::runtime_error);
}

TEST(DependencyResolverTest, SingleModule) {
    DependencyResolver resolver;
    std::vector<DependencyResolver::ModuleInfo> modules = {
        {"A", {}}
    };
    auto order = resolver.resolveOrder(modules);
    ASSERT_EQ(order.size(), 1);
    EXPECT_EQ(order[0], "A");
}

TEST(DependencyResolverTest, MissingDependencyIsSkipped) {
    DependencyResolver resolver;
    std::vector<DependencyResolver::ModuleInfo> modules = {
        {"A", {"missing_dep"}}
    };
    EXPECT_NO_THROW(resolver.resolveOrder(modules));
}
