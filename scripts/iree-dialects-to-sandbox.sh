#!/bin/bash

set -ex

function repopulate_iree_dialect() {
  rm -Rf include/Dialect/$1 lib/Dialect/$1
  cp -R -f ../iree/llvm-external-projects/iree-dialects/include/iree-dialects/Dialect/$1 include/Dialect/
  cp -R -f ../iree/llvm-external-projects/iree-dialects/lib/Dialect/$1 lib/Dialect/
}

function repopulate_iree_dialect_test() {
  rm -Rf test/Dialect/$1
  cp -R -f ../iree/llvm-external-projects/iree-dialects/test/Dialect/$1 test/Dialect/
}

function repopulate_iree_dir() {
  rm -Rf include/$1 lib/$1
  cp -R -f ../iree/llvm-external-projects/iree-dialects/include/iree-dialects/$1 include/
  cp -R -f ../iree/llvm-external-projects/iree-dialects/lib/$1 lib/
}

if [ -z "$(git status --porcelain)" ]; then
  echo "Start synchronizing from IREE"
else
  echo "Git repository unclean, abort synchronizing form IREE."
  exit 1
fi

repopulate_iree_dialect LinalgExt
repopulate_iree_dialect LinalgTransform

repopulate_iree_dialect_test iree_linalg_ext
repopulate_iree_dialect_test linalg_transform

repopulate_iree_dir Transforms


# Copy python files.
cp ../iree/llvm-external-projects/iree-dialects/python/iree/compiler/dialects/_iree_linalg_transform_ops_ext.py python/sandbox/dialects/
cp ../iree/llvm-external-projects/iree-dialects/python/iree/compiler/dialects/iree_linalg_ext.py python/sandbox/dialects/
cp ../iree/llvm-external-projects/iree-dialects/python/iree/compiler/dialects/iree_linalg_transform.py python/sandbox/dialects/
cp ../iree/llvm-external-projects/iree-dialects/python/iree/compiler/dialects/IreeLinalgExtBinding.td python/sandbox/dialects/
cp ../iree/llvm-external-projects/iree-dialects/python/iree/compiler/dialects/LinalgTransformBinding.td python/sandbox/dialects/

# Fix include paths.
git grep -l iree-dialects/Dialect/ | grep -v scripts | xargs sed -i "s:iree-dialects/Dialect/:Dialect/:g"
git grep -l iree-dialects/Transforms/ | grep -v scripts | xargs sed -i "s:iree-dialects/Transforms/:Transforms/:g"

# Drop building of IREE's LinalgExt/Passes that depends on the IREEInputDialect.
git grep -l "add_subdirectory(Passes)" | grep LinalgExt | xargs sed -i "s:add_subdirectory(Passes)::g"

# Drop IREE python import, we do our own registration.
git grep -l "from .._mlir_libs._ireeDialects" | grep -v scripts | xargs sed -i "s:from .._mlir:\# from .._mlir:g"

# Run a formatting pass.
git diff --name-only | egrep "*.(\.cpp|\.h)" | xargs -i clang-format --style=file -i {} || true

# Fix tests.
git grep -l iree-dialects-opt | grep -v scripts | xargs sed -i "s:iree-dialects-opt:mlir-proto-opt:g"

# Post-hoc removal of known not-sandbox files.
rm test/Dialect/iree_linalg_ext/pad_contraction_to_block_size.mlir
rm test/Dialect/iree_linalg_ext/convert_to_loops.mlir
rm test/Dialect/linalg_transform/scoped.mlir
