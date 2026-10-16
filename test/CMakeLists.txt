enable_testing()

# 测试 lru 缓存
add_executable(test_lru "./test_lru.cpp")
target_link_libraries(test_lru PRIVATE GTest::gtest_main kylecache_core)

# 测试一致性哈希
add_executable(test_cons_hash "./test_consistent_hash.cpp")
target_link_libraries(test_cons_hash PRIVATE GTest::gtest_main kylecache_core)

# 测试 cache group
add_executable(test_group "./test_group.cpp")
target_link_libraries(test_group PRIVATE GTest::gtest_main kylecache_core)

include(GoogleTest)
gtest_discover_tests(test_lru)
gtest_discover_tests(test_cons_hash)
gtest_discover_tests(test_group)

# 简易基准测试（手动计时，不注册进 ctest）
add_executable(bench_cache "./bench_cache.cpp")
target_link_libraries(bench_cache PRIVATE kylecache_core)
