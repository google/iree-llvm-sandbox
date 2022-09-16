#include "IteratorAnalysis.h"

#include "iterators/Dialect/Iterators/IR/Iterators.h"
#include "iterators/Utils/NameAssigner.h"
#include "mlir/Dialect/LLVMIR/LLVMTypes.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/Transforms/DialectConversion.h"
#include "llvm/ADT/TypeSwitch.h"

using namespace mlir;
using namespace mlir::iterators;
using namespace LLVM;

using SymbolTriple = std::tuple<SymbolRefAttr, SymbolRefAttr, SymbolRefAttr>;

/// Pre-assigns names for the Open/Next/Close functions of the given iterator
/// op. The conversion is expected to create these names in the lowering of
/// the corresponding op and can look them up in the lowering of downstream
/// iterators.
static SymbolTriple assignFunctionNames(Operation *op,
                                        NameAssigner &nameAssigner) {
  SmallVector<SymbolRefAttr, 3> symbols;
  for (auto suffix : {"open", "next", "close"}) {
    // Construct base name from op type and Open/Next/Close.
    auto baseName = StringAttr::get(
        op->getContext(),
        (op->getName().getStringRef() + Twine(".") + suffix).str());

    // Make name unique. This may increment uniqueNumber.
    StringAttr uniqueName = nameAssigner.assignName(baseName);

    auto symbol = SymbolRefAttr::get(op->getContext(), uniqueName);
    symbols.push_back(symbol);
  }
  return std::make_tuple(symbols[0], symbols[1], symbols[2]);
}

/// Helper for computing the iterator states as part of the IteratorAnalysis.
/// The main objective if this class is to hold context that is required for
/// computing each particular state type (currently a TypeConverter). The actual
/// computation is implemented by instances of the `template operator()(...)`.
class StateTypeComputer {
public:
  explicit StateTypeComputer(TypeConverter &typeConverter)
      : typeConverter(typeConverter) {}

  StateTypeComputer(const StateTypeComputer &other) = default;
  StateTypeComputer(StateTypeComputer &&other) = default;
  StateTypeComputer &operator=(const StateTypeComputer &other) = default;
  StateTypeComputer &operator=(StateTypeComputer &&other) = default;

  /// Computes the state type of the given op whose upstream iterator ops have
  /// the state types given in upstreamStateTypes.
  template <typename OpType>
  LLVMStructType
  operator()(OpType op,
             llvm::SmallVector<LLVMStructType> upstreamStateTypes) const;

private:
  TypeConverter typeConverter;
};

/// The state of ConstantStreamOp consists of a single number that corresponds
/// to the index of the next struct returned by the iterator.
template <>
LLVMStructType StateTypeComputer::operator()(
    ConstantStreamOp op,
    llvm::SmallVector<LLVMStructType> /*upstreamStateTypes*/) const {
  MLIRContext *context = op->getContext();
  Type i32 = IntegerType::get(context, /*width=*/32);
  return LLVMStructType::getNewIdentified(
      context, "iterators.constant_stream_state", {i32});
}

/// The state of FilterOp only consists of the state of its upstream iterator,
/// i.e., the state of the iterator that produces its input stream.
template <>
LLVMStructType StateTypeComputer::operator()(
    FilterOp op, llvm::SmallVector<LLVMStructType> upstreamStateTypes) const {
  return LLVMStructType::getNewIdentified(
      op->getContext(), "iterators.filter_state", {upstreamStateTypes[0]});
}

/// The state of MapOp only consists of the state of its upstream iterator,
/// i.e., the state of the iterator that produces its input stream.
template <>
LLVMStructType StateTypeComputer::operator()(
    MapOp op, llvm::SmallVector<LLVMStructType> upstreamStateTypes) const {
  return LLVMStructType::getNewIdentified(
      op->getContext(), "iterators.map_state", {upstreamStateTypes[0]});
}

/// The state of ReduceOp only consists of the state of its upstream iterator,
/// i.e., the state of the iterator that produces its input stream.
template <>
LLVMStructType StateTypeComputer::operator()(
    ReduceOp op, llvm::SmallVector<LLVMStructType> upstreamStateTypes) const {
  assert(upstreamStateTypes.size() == 1);
  return LLVMStructType::getNewIdentified(
      op->getContext(), "iterators.reduce_state", {upstreamStateTypes[0]});
}

/// Build IteratorInfo, assigning new unique names as needed. Takes the
/// `LLVMStructType` as a parameter, to ensure proper build order (all uses are
/// visited before any def).
mlir::iterators::IteratorInfo::IteratorInfo(IteratorOpInterface op,
                                            NameAssigner &nameAssigner,
                                            LLVMStructType t) {
  std::tie(openFunc, nextFunc, closeFunc) =
      assignFunctionNames(op, nameAssigner);
  stateType = t;
}

IteratorInfo mlir::iterators::IteratorAnalysis::getExpectedIteratorInfo(
    IteratorOpInterface op) const {
  auto it = opMap.find(op);
  assert(it != opMap.end() && "analysis does not contain this op");
  return it->getSecond();
}

void mlir::iterators::IteratorAnalysis::setIteratorInfo(
    IteratorOpInterface op, const IteratorInfo &info) {
  assert(info.stateType && "state type must be computed");
  auto inserted = opMap.insert({op, info});
  assert(inserted.second && "IteratorInfo already present");
}

template <typename OpTy>
static OpTy getSelfOrParentOfType(Operation *op) {
  auto maybe = dyn_cast<OpTy>(op);
  return maybe ? maybe : op->getParentOfType<OpTy>();
}

mlir::iterators::IteratorAnalysis::IteratorAnalysis(
    Operation *rootOp, TypeConverter &typeConverter)
    : rootOp(rootOp), nameAssigner(getSelfOrParentOfType<ModuleOp>(rootOp)) {
  /// This needs to be built in use-def order so that all uses are visited
  /// before any def.
  StateTypeComputer stateTypeComputer(typeConverter);
  rootOp->walk([&](IteratorOpInterface iteratorOp) {
    llvm::TypeSwitch<Operation *, void>(iteratorOp)
        // TODO: Verify that operands do not come from bbArgs.
        .Case<
            // clang-format off
            ConstantStreamOp,
            FilterOp,
            MapOp,
            ReduceOp
            // clang-format on
            >([&](auto op) {
          llvm::SmallVector<LLVMStructType> upstreamStateTypes;
          llvm::transform(op->getOperands(),
                          std::back_inserter(upstreamStateTypes),
                          [&](auto operand) {
                            Operation *def = operand.getDefiningOp();
                            if (!def || !llvm::isa<IteratorOpInterface>(def))
                              return LLVMStructType();
                            return getExpectedIteratorInfo(def).stateType;
                          });
          LLVMStructType stateType = stateTypeComputer(op, upstreamStateTypes);
          setIteratorInfo(op, IteratorInfo(op, nameAssigner, stateType));
        })
        .Default([&](auto op) { assert(false && "Unexpected op"); });
  });
}
