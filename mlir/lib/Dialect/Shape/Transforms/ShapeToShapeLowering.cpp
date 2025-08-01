//===- ShapeToShapeLowering.cpp - Prepare for lowering to Standard --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "mlir/Dialect/Shape/Transforms/Passes.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Shape/IR/Shape.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Transforms/DialectConversion.h"

namespace mlir {
#define GEN_PASS_DEF_SHAPETOSHAPELOWERINGPASS
#include "mlir/Dialect/Shape/Transforms/Passes.h.inc"
} // namespace mlir

using namespace mlir;
using namespace mlir::shape;

namespace {
/// Converts `shape.num_elements` to `shape.reduce`.
struct NumElementsOpConverter : public OpRewritePattern<NumElementsOp> {
public:
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(NumElementsOp op,
                                PatternRewriter &rewriter) const final;
};
} // namespace

LogicalResult
NumElementsOpConverter::matchAndRewrite(NumElementsOp op,
                                        PatternRewriter &rewriter) const {
  auto loc = op.getLoc();
  Type valueType = op.getResult().getType();
  Value init = op->getDialect()
                   ->materializeConstant(rewriter, rewriter.getIndexAttr(1),
                                         valueType, loc)
                   ->getResult(0);
  ReduceOp reduce = ReduceOp::create(rewriter, loc, op.getShape(), init);

  // Generate reduce operator.
  Block *body = reduce.getBody();
  OpBuilder b = OpBuilder::atBlockEnd(body);
  Value product = MulOp::create(b, loc, valueType, body->getArgument(1),
                                body->getArgument(2));
  shape::YieldOp::create(b, loc, product);

  rewriter.replaceOp(op, reduce.getResult());
  return success();
}

namespace {
struct ShapeToShapeLowering
    : public impl::ShapeToShapeLoweringPassBase<ShapeToShapeLowering> {
  void runOnOperation() override;
};
} // namespace

void ShapeToShapeLowering::runOnOperation() {
  MLIRContext &ctx = getContext();

  RewritePatternSet patterns(&ctx);
  populateShapeRewritePatterns(patterns);

  ConversionTarget target(getContext());
  target.addLegalDialect<arith::ArithDialect, ShapeDialect>();
  target.addIllegalOp<NumElementsOp>();
  if (failed(mlir::applyPartialConversion(getOperation(), target,
                                          std::move(patterns))))
    signalPassFailure();
}

void mlir::populateShapeRewritePatterns(RewritePatternSet &patterns) {
  patterns.add<NumElementsOpConverter>(patterns.getContext());
}
