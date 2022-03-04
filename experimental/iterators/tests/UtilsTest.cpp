#include <gtest/gtest.h>

#include <sstream>
#include <tuple>

#include "Utils/Tuple.h"

TEST(ExtractHeadTest, Head0) {
  std::tuple<int16_t, int32_t> tuple = {1, 2};
  auto const head = takeFront<0>(tuple);
  static_assert(std::is_same_v<std::remove_cv_t<decltype(head)>, std::tuple<>>,
                "Expected head to be of type std::tuple<>.");
  EXPECT_EQ(head, std::make_tuple());
}

TEST(ExtractHeadTest, Head1) {
  std::tuple<int16_t, int32_t> tuple = {1, 2};
  auto const head = takeFront<1>(tuple);
  static_assert(
      std::is_same_v<std::remove_cv_t<decltype(head)>, std::tuple<int16_t>>,
      "Expected head to be of type std::tuple<int16_t>.");
  EXPECT_EQ(head, std::make_tuple(1));
}

TEST(ExtractHeadTest, Head2) {
  std::tuple<int16_t, int32_t> tuple = {1, 2};
  auto const head = takeFront<2>(tuple);
  static_assert(std::is_same_v<std::remove_cv_t<decltype(head)>,
                               std::tuple<int16_t, int32_t>>,
                "Expected head to be of type std::tuple<int16_t, int32_t>.");
  EXPECT_EQ(head, std::make_tuple(1, 2));
}

TEST(ExtractTailTest, Tail0) {
  std::tuple<int16_t, int32_t> tuple = {1, 2};
  auto const tail = dropFront<0>(tuple);
  static_assert(std::is_same_v<std::remove_cv_t<decltype(tail)>,
                               std::tuple<int16_t, int32_t>>,
                "Expected tail to be of type std::tuple<int16_t, int32_t>.");
  EXPECT_EQ(tail, std::make_tuple(1, 2));
}

TEST(ExtractTailTest, Tail1) {
  std::tuple<int16_t, int32_t> tuple = {1, 2};
  auto const tail = dropFront<1>(tuple);
  static_assert(
      std::is_same_v<std::remove_cv_t<decltype(tail)>, std::tuple<int32_t>>,
      "Expected tail to be of type std::tuple<int32_t>.");
  EXPECT_EQ(tail, std::make_tuple(2));
}

TEST(ExtractTailTest, Tail2) {
  std::tuple<int16_t, int32_t> tuple = {1, 2};
  auto const tail = dropFront<2>(tuple);
  static_assert(std::is_same_v<std::remove_cv_t<decltype(tail)>, std::tuple<>>,
                "Expected tail to be of type std::tuple<>.");
  EXPECT_EQ(tail, std::make_tuple());
}

TEST(HashTupleTest, SimpleTests) {
  std::hash<uint32_t> hasher;
  EXPECT_EQ(hashTuple(std::make_tuple(1)), hasher(1));
  EXPECT_EQ(hashTuple(std::make_tuple(1, 2)), hasher(1) ^ hasher(2));
}

TEST(PrintTupleTest, SingleField) {
  std::stringstream string_buffer;
  printTuple(string_buffer, std::make_tuple(1));
  EXPECT_EQ(string_buffer.str(), "(1)");
}

TEST(PrintTupleTest, MultipleFields) {
  std::stringstream string_buffer;
  printTuple(string_buffer, std::make_tuple(1, 2, 3));
  EXPECT_EQ(string_buffer.str(), "(1, 2, 3)");
}
