#include <gtest/gtest.h>

#include <tuple>
#include <vector>

#include "operators/column_scan.h"
#include "operators/reduce.h"

TEST(ReduceTest, SingleColumnSum) {
  std::vector<int32_t> numbers = {1, 2, 3, 4};
  auto scan = makeColumnScanOperator(numbers);
  auto reduce = makeReduceOperator(&scan, [](auto t1, auto t2) {
    return std::make_tuple(std::get<0>(t1) + std::get<0>(t2));
  });
  reduce.open();

  decltype(reduce)::ReturnType tuple;

  // Consume one value
  tuple = reduce.computeNext();
  EXPECT_TRUE(tuple);
  EXPECT_EQ(std::get<0>(tuple.value()), 10);

  // Check that we have reached the end
  tuple = reduce.computeNext();
  EXPECT_FALSE(tuple);

  // Check that we can test for the end again
  tuple = reduce.computeNext();
  EXPECT_FALSE(tuple);

  reduce.close();
}

TEST(ReduceTest, MulticolumnMinMax) {
  std::vector<int32_t> numbers = {1, 2, 3, 4};
  auto scan = makeColumnScanOperator(numbers, numbers);
  auto reduce = makeReduceOperator(&scan, [](auto t1, auto t2) {
    return std::make_tuple(std::min(std::get<0>(t1), std::get<0>(t2)),
                           std::max(std::get<1>(t1), std::get<1>(t2)));
  });
  reduce.open();

  decltype(reduce)::ReturnType tuple;

  // Consume one value
  tuple = reduce.computeNext();
  EXPECT_TRUE(tuple);
  EXPECT_EQ(std::get<0>(tuple.value()), 1);
  EXPECT_EQ(std::get<1>(tuple.value()), 4);

  // Check that we have reached the end
  tuple = reduce.computeNext();
  EXPECT_FALSE(tuple);

  reduce.close();
}
