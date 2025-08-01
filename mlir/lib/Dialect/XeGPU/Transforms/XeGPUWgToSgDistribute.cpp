//===- XeGPUWgToSgDistribute.cpp - XeGPU Workgroup to Subgroup Pass -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "mlir/Dialect/XeGPU/Transforms/Passes.h"

#include "mlir/Dialect/Affine/Utils.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Arith/Utils/Utils.h"
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/Dialect/Index/IR/IndexDialect.h"
#include "mlir/Dialect/Index/IR/IndexOps.h"
#include "mlir/Dialect/Math/IR/Math.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/Transforms/Patterns.h"
#include "mlir/Dialect/Utils/IndexingUtils.h"
#include "mlir/Dialect/XeGPU/IR/XeGPU.h"
#include "mlir/Dialect/XeGPU/Transforms/Transforms.h"
#include "mlir/Dialect/XeGPU/Utils/XeGPUUtils.h"
#include "mlir/Transforms/DialectConversion.h"
#include <optional>

namespace mlir {
namespace xegpu {
#define GEN_PASS_DEF_XEGPUWGTOSGDISTRIBUTE
#include "mlir/Dialect/XeGPU/Transforms/Passes.h.inc"
} // namespace xegpu
} // namespace mlir

using namespace mlir;

namespace {

// Check if there is sg id range attached to the scf.if op.
static bool isSgIdRangeSpecified(Operation *op, int64_t &startOfRange,
                                 int64_t &endOfRange) {
  Operation *parent = op->getParentOp();
  // Find the outermost scf::IfOp with xegpu.sg_id_range.
  while (parent) {
    if (auto ifOp = dyn_cast<scf::IfOp>(parent)) {
      if (auto attr = llvm::dyn_cast_or_null<xegpu::RangeAttr>(
              ifOp->getAttr("sg_id_range"))) {
        startOfRange = attr.getStart().getInt();
        endOfRange = attr.getEnd().getInt();
        break;
      }
    }
    parent = parent->getParentOp();
  }
  // Return false if startOfRange is 0
  return (startOfRange > 0 && endOfRange > startOfRange);
}

static std::pair<SmallVector<int64_t>, int>
getSgShapeAndCount(ArrayRef<int64_t> shape, xegpu::LayoutAttr layout) {
  int count = 1;
  SmallVector<int64_t> sgShape(shape);

  if (layout && layout.isWgLayout()) {
    DenseI32ArrayAttr sgLayoutAttr = layout.getSgLayout();
    auto sgLayout = llvm::to_vector_of<int64_t>(sgLayoutAttr.asArrayRef());
    if (DenseI32ArrayAttr sgDataAttr = layout.getSgData())
      sgShape = llvm::to_vector_of<int64_t>(sgDataAttr.asArrayRef());
    else
      sgShape = computeShapeRatio(shape, sgLayout).value_or(sgShape);
    SmallVector<int64_t> distUnit = computeElementwiseMul(sgLayout, sgShape);
    // Clamp distUnit to the original shape to handle cases where data is
    // shared among subgroups, which may cause distUnit to exceed the original
    // shape.
    for (size_t i = 0; i < distUnit.size(); ++i)
      distUnit[i] = std::min(shape[i], distUnit[i]);
    count = computeProduct(shape) / computeProduct(distUnit);
  }
  return std::make_pair(sgShape, count);
}

/// This pattern transforms the CreateNdDescOp to create a subgroup descriptor
/// from a workgroup descriptor. It replaces the offsets and sizes with
/// appropriate values for the subgroup.
/// It uses round-robin assignment to distribute the work to the subgroups.
/// Following create_nd_desc operation:,
///    %tdesc = xegpu.create_nd_tdesc %src[0, 0] : memref<24x24xf32>
///       -> !xegpu.tensor_desc<24x24xf32, #xegpu.layout<sg_layout = [4, 4],
///           sg_data = [2, 2], lane_layout = [2, 2], lane_data = [1, 1]>>
/// is converted to 9 subgroup level operations based on the sg_layout &
/// sg_data:
///    %tdesc = xegpu.create_nd_tdesc %src[off1, off2] : memref<24x24xf32> ->
///           !xegpu.tensor_desc<2x2xf32, #xegpu.layout<lane_layout = [2, 2],
///           lane_data = [1, 1]>>
///
/// The sg_layout and sg_data attributes are dropped after the pass as they are
/// no longer needed.
///
/// 24x24 matrix distribution example:
/// sg_layout = [4, 4], sg_data = [2, 2]
/// Each 8x8 matrix within the 24x24 matrix is called a distribution unit.
/// dist_unit_shape = [8, 8] --> sg_layout[i] * sg_data[i]
///
/// +------------------------+
/// | 8x8 | 8x8 | 8x8 |      <- 3 tiles across
/// |-----+-----+-----|
/// | 8x8 | 8x8 | 8x8 |      <- 3 tiles down
/// |-----+-----+-----|
/// | 8x8 | 8x8 | 8x8 |
/// +------------------------+
///
/// Each 8x8 tile is further subdivided among subgroups:
/// +------------------------+
/// | 2x2 2x2 2x2 2x2 |  <- 4 subgroups across (each handles 2 columns)
/// | 2x2 2x2 2x2 2x2 |  <- 4 subgroups down (each handles 2 rows)
/// | 2x2 2x2 2x2 2x2 |
/// | 2x2 2x2 2x2 2x2 |
/// +------------------------+
///
/// Since the 24x24 matrix is divided into 8x8 distribution units, there will be
/// 9 distribution units (3x3) in total. Hence the 9 subgroup level operations.

/// The pass currently has entire distribution logic in the WgToSgCreateNdOp
/// pattern and all the other ops just follow.
/// TODO: Decouple the distribution logic from WgToSgCreateNdOp for all the
/// ops in the pass.
struct WgToSgCreateNdOp : public OpConversionPattern<xegpu::CreateNdDescOp> {
  using OpConversionPattern<xegpu::CreateNdDescOp>::OpConversionPattern;

  // Calculate offset for each subgroup
  static SmallVector<OpFoldResult>
  calculateGlobalOffsets(ConversionPatternRewriter &rewriter, Location loc,
                         const SmallVector<OpFoldResult> &originalOffsets,
                         const SmallVector<Value> &localOffset,
                         const SmallVector<int64_t> &distUnitBaseAddr,
                         const SmallVector<int64_t> &distUnitShape) {
    assert(localOffset.size() == distUnitBaseAddr.size() &&
           "localOffset and distUnitBaseAddr must have the same rank");

    SmallVector<OpFoldResult> globalOffsets(originalOffsets.begin(),
                                            originalOffsets.end());
    size_t rank = localOffset.size();
    for (size_t i = 0; i < rank; ++i) {
      size_t dimIdx = originalOffsets.size() - rank + i;
      Value constOffset =
          arith::ConstantIndexOp::create(rewriter, loc, distUnitBaseAddr[i]);
      Value offset =
          rewriter.createOrFold<index::AddOp>(loc, localOffset[i], constOffset);
      Value modValue =
          arith::ConstantIndexOp::create(rewriter, loc, distUnitShape[i]);
      Value offsetMod =
          rewriter.createOrFold<index::RemUOp>(loc, offset, modValue);
      Value origOffset = getValueOrCreateConstantIndexOp(
          rewriter, loc, originalOffsets[dimIdx]);
      Value globalOffset =
          rewriter.createOrFold<index::AddOp>(loc, origOffset, offsetMod);
      globalOffsets[dimIdx] = globalOffset;
    }

    return globalOffsets;
  }

  LogicalResult
  matchAndRewrite(xegpu::CreateNdDescOp op, OneToNOpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = op.getLoc();
    MLIRContext *ctx = op.getContext();
    xegpu::TensorDescType tdescTy = op.getType();
    auto layout = dyn_cast<xegpu::LayoutAttr>(tdescTy.getLayout());
    if (!layout)
      return failure();
    Type elemTy = tdescTy.getElementType();
    ArrayRef<int64_t> wgShape = tdescTy.getShape();
    // sgLayout must be present for workgroup-level distribution.
    SmallVector<int64_t> sgLayout;
    if (auto sgLayoutAttr = layout.getSgLayout())
      sgLayout = llvm::to_vector_of<int64_t>(sgLayoutAttr.asArrayRef());
    else
      return rewriter.notifyMatchFailure(
          op, "sgLayout attribute is required in layout");

    SmallVector<int64_t> sgShape = getSgShapeAndCount(wgShape, layout).first;

    // TODO : Handle order attribute
    // Get the subgroup ID
    auto linearSgId =
        gpu::SubgroupIdOp::create(rewriter, loc, /*upper_bound=*/nullptr);

    // Create constants for layout dimensions
    SmallVector<Value> sgLayoutDim(sgLayout.size());
    SmallVector<Value> sgDataDim(sgShape.size());

    for (size_t i = 0; i < sgLayout.size(); i++) {
      sgLayoutDim[i] =
          arith::ConstantIndexOp::create(rewriter, loc, sgLayout[i]);
      sgDataDim[i] = arith::ConstantIndexOp::create(rewriter, loc, sgShape[i]);
    }

    int64_t startOfRange = -1, endOfRange = -1;
    bool sgIdRangeSpecified =
        isSgIdRangeSpecified(op, startOfRange, endOfRange);

    Value adjustedSgId = linearSgId;
    if (sgIdRangeSpecified) {
      int64_t sgCount = endOfRange - startOfRange;
      if (computeProduct(sgLayout) != sgCount)
        return rewriter.notifyMatchFailure(
            op, "sg_layout size must match the sg_id_range");
      // Subtract startOfRange from the original subgroup id to get the adjusted
      // sg id
      Value startOfRangeVal =
          arith::ConstantIndexOp::create(rewriter, loc, startOfRange);
      adjustedSgId =
          rewriter.createOrFold<index::SubOp>(loc, linearSgId, startOfRangeVal);
    }

    auto deLinearizeSgId =
        affine::delinearizeIndex(rewriter, loc, adjustedSgId, sgLayoutDim);
    if (failed(deLinearizeSgId))
      return failure();
    SmallVector<Value> sgIds = *deLinearizeSgId;

    // Calculate distribution unit shape and local offsets for subgroup
    SmallVector<int64_t> distUnitShape(sgLayout.size());
    SmallVector<Value> localOffset(sgLayout.size());
    for (size_t i = 0; i < sgLayout.size(); i++) {
      distUnitShape[i] = std::min(sgLayout[i] * sgShape[i], wgShape[i]);
      localOffset[i] =
          rewriter.createOrFold<index::MulOp>(loc, sgIds[i], sgDataDim[i]);
    }

    SmallVector<OpFoldResult> originalOffsets = op.getMixedOffsets();

    xegpu::TensorDescType newTdescTy =
        xegpu::TensorDescType::get(ctx, sgShape, elemTy, tdescTy.getEncoding(),
                                   layout.dropSgLayoutAndData());
    SmallVector<Value> newCreateNdOps;
    for (SmallVector<int64_t> distUnitBaseAddr :
         StaticTileOffsetRange(wgShape, distUnitShape)) {
      SmallVector<OpFoldResult> globalOffsets =
          calculateGlobalOffsets(rewriter, loc, originalOffsets, localOffset,
                                 distUnitBaseAddr, distUnitShape);

      auto newCreateNdOp = xegpu::CreateNdDescOp::create(
          rewriter, loc, newTdescTy, op.getSource(), globalOffsets,
          op.getMixedSizes(), op.getMixedStrides());
      newCreateNdOps.push_back(newCreateNdOp);
    }

    rewriter.replaceOpWithMultiple(op, {newCreateNdOps});
    return success();
  }
};

/// This pattern transforms the LoadNdOp to load subgroup data.
struct WgToSgLoadNdOp : public OpConversionPattern<xegpu::LoadNdOp> {
  using OpConversionPattern<xegpu::LoadNdOp>::OpConversionPattern;
  LogicalResult
  matchAndRewrite(xegpu::LoadNdOp op, OneToNOpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    SmallVector<Value> newLoadOps;

    int64_t offsetSize = static_cast<int64_t>(op.getOffsets().size());
    if ((offsetSize != 0) || op.getConstOffsetsAttr())
      return failure();

    for (auto src : adaptor.getTensorDesc()) {
      xegpu::TensorDescType tdescTy =
          dyn_cast<xegpu::TensorDescType>(src.getType());
      ArrayRef<int64_t> srcShape = tdescTy.getShape();
      VectorType newResTy = VectorType::get(srcShape, tdescTy.getElementType());
      auto newLoadOp = xegpu::LoadNdOp::create(rewriter, op.getLoc(), newResTy,
                                               src, op->getAttrs());
      newLoadOps.push_back(newLoadOp);
    }
    rewriter.replaceOpWithMultiple(op, {newLoadOps});
    return mlir::success();
  }
};

/// This pattern transforms the StoreNdOp to store to a subgroup descriptor
/// It creates a StoreNdOp op to store the updated values to the new subgroup
/// src tensor descriptors.
struct WgToSgStoreNdOp : public OpConversionPattern<xegpu::StoreNdOp> {
  using OpConversionPattern<xegpu::StoreNdOp>::OpConversionPattern;
  LogicalResult
  matchAndRewrite(xegpu::StoreNdOp op, OneToNOpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {

    int64_t offsetSize = static_cast<int64_t>(op.getOffsets().size());
    if ((offsetSize != 0) || op.getConstOffsetsAttr())
      return failure();

    for (auto [v, t] : llvm::zip(adaptor.getValue(), adaptor.getTensorDesc()))
      xegpu::StoreNdOp::create(rewriter, op.getLoc(), v, t, op.getL1HintAttr(),
                               op.getL2HintAttr(), op.getL3HintAttr());

    rewriter.eraseOp(op);
    return success();
  }
};

/// This pattern transforms the UpdateNdOffsetOp to update the offsets of a
/// subgroup descriptor. It creates an UpdateNdOffsetOp op to update the
/// offsets of the new subgroup src tensor descriptors.
struct WgToSgUpdateNdOffsetOp
    : public OpConversionPattern<xegpu::UpdateNdOffsetOp> {
  using OpConversionPattern<xegpu::UpdateNdOffsetOp>::OpConversionPattern;
  LogicalResult
  matchAndRewrite(xegpu::UpdateNdOffsetOp op, OneToNOpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    llvm::SmallVector<Value> newUpdateTileOffsetOps;
    for (auto tDesc : adaptor.getTensorDesc()) {
      auto newUpdateTileOffsetOp = xegpu::UpdateNdOffsetOp::create(
          rewriter, op.getLoc(), tDesc.getType(), tDesc, op.getOffsets(),
          op.getConstOffsets());
      newUpdateTileOffsetOps.push_back(newUpdateTileOffsetOp);
    }

    rewriter.replaceOpWithMultiple(op, {newUpdateTileOffsetOps});
    return success();
  }
};

/// This pattern transforms the DpasOp to work at subgroup level.
struct WgToSgDpasOp : public OpConversionPattern<xegpu::DpasOp> {
  using OpConversionPattern<xegpu::DpasOp>::OpConversionPattern;
  LogicalResult
  matchAndRewrite(xegpu::DpasOp op, OneToNOpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = op.getLoc();
    VectorType resultTy = op.getResult().getType();
    if (resultTy.getRank() != 2)
      return failure();

    auto originalLayout = xegpu::getLayoutAttr(op.getResult());
    if (!originalLayout)
      return failure();

    size_t i = 0;
    SmallVector<Value> newDpasOps;
    for (auto aVec : adaptor.getLhs()) {
      for (auto bVec : adaptor.getRhs()) {

        llvm::SmallVector<Value> operands({aVec, bVec});
        Value tmpC;
        if (op.getAcc()) {
          tmpC = adaptor.getAcc()[i++];
          operands.push_back(tmpC);
        }

        ArrayRef<int64_t> aVecShape =
            llvm::cast<VectorType>(aVec.getType()).getShape();
        ArrayRef<int64_t> bVecShape =
            llvm::cast<VectorType>(bVec.getType()).getShape();
        VectorType resTy = VectorType::get({aVecShape[0], bVecShape[1]},
                                           resultTy.getElementType());
        tmpC = xegpu::DpasOp::create(rewriter, loc, resTy, operands);
        xegpu::setLayoutAttr(cast<OpResult>(tmpC),
                             originalLayout.dropSgLayoutAndData());

        newDpasOps.push_back(tmpC);
      }
    }
    rewriter.replaceOpWithMultiple(op, {newDpasOps});
    return success();
  }
};

/// This pattern transforms the PrefetchNdOp to prefetch the subgroup data.
struct WgToSgPrefetchNdOp : public OpConversionPattern<xegpu::PrefetchNdOp> {
  using OpConversionPattern<xegpu::PrefetchNdOp>::OpConversionPattern;
  LogicalResult
  matchAndRewrite(xegpu::PrefetchNdOp op, OneToNOpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {

    int64_t offsetSize = static_cast<int64_t>(op.getOffsets().size());
    if ((offsetSize != 0) || op.getConstOffsetsAttr())
      return failure();

    for (auto src : adaptor.getTensorDesc())
      xegpu::PrefetchNdOp::create(rewriter, op.getLoc(), TypeRange(), src,
                                  op->getAttrs());
    rewriter.eraseOp(op);
    return success();
  }
};

/// This pattern transforms vector.broadcast ops to work at subgroup level.
struct WgToSgVectorBroadcastOp
    : public OpConversionPattern<vector::BroadcastOp> {
  using OpConversionPattern<vector::BroadcastOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(vector::BroadcastOp op, OneToNOpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    VectorType resultType = op.getResult().getType();
    ArrayRef<int64_t> wgShape = resultType.getShape();

    xegpu::LayoutAttr layout = xegpu::getLayoutAttr(op.getResult());
    if (!layout || !layout.getSgLayout())
      return failure();

    // TODO: Currently only supports cases where the source and result ranks
    // are the same.
    auto srcType =
        dyn_cast<VectorType>(adaptor.getOperands().front()[0].getType());
    if (!srcType || srcType.getRank() != resultType.getRank())
      return failure();

    SmallVector<int64_t> sgShape = getSgShapeAndCount(wgShape, layout).first;
    VectorType newResultType =
        VectorType::get(sgShape, resultType.getElementType());

    // Check if the output layout is distributable
    SmallVector<int64_t> sgLayout;
    if (auto sgLayoutAttr = layout.getSgLayout())
      sgLayout = llvm::to_vector_of<int64_t>(sgLayoutAttr.asArrayRef());
    else
      return failure();

    if (!xegpu::XeGPUDialect::isEvenlyDistributable(wgShape, layout))
      return failure();

    // Check if the srcShape has unit dim in dimensions being broadcasted,
    // and the other dimensions are the same as the destination type
    // TODO: Generalize it
    auto srcShape = srcType.getShape();
    for (size_t i = 0; i < srcShape.size(); ++i) {
      if (srcShape[i] != 1 && srcShape[i] != sgShape[i])
        return failure();
    }

    SmallVector<Value> newBroadcastOps;
    for (auto operand : adaptor.getOperands().front()) {
      auto newBroadcast = vector::BroadcastOp::create(rewriter, op.getLoc(),
                                                      newResultType, operand);
      xegpu::setLayoutAttr(newBroadcast->getResult(0),
                           layout.dropSgLayoutAndData());
      newBroadcastOps.push_back(newBroadcast.getResult());
    }

    rewriter.replaceOpWithMultiple(op, {newBroadcastOps});
    return success();
  }
};

// This pattern transforms elementwise ops to work at subgroup level.
struct WgToSgElementwiseOp : public ConversionPattern {
  WgToSgElementwiseOp(MLIRContext *ctx)
      : ConversionPattern(MatchAnyOpTypeTag(), /*benefit=*/1, ctx) {}

  LogicalResult
  matchAndRewrite(Operation *op, ArrayRef<ValueRange> operands,
                  ConversionPatternRewriter &rewriter) const override {
    // Only match ops with elementwise trait and single result.
    if (!OpTrait::hasElementwiseMappableTraits(op) || op->getNumResults() != 1)
      return failure();

    auto resultType = dyn_cast<VectorType>(op->getResult(0).getType());
    assert(resultType && "Expected result to be a VectorType");

    ArrayRef<int64_t> wgShape = resultType.getShape();

    xegpu::LayoutAttr layout = xegpu::getLayoutAttr(op->getResult(0));
    if (!layout || !layout.getSgLayout())
      return failure();

    SmallVector<int64_t> sgShape = getSgShapeAndCount(wgShape, layout).first;

    size_t numVariants = operands.empty() ? 0 : operands.front().size();

    if (llvm::any_of(operands, [&](const ValueRange &operandVec) {
          return operandVec.size() != numVariants;
        }))
      return failure();

    SmallVector<Value> newResults;
    VectorType newResultType =
        VectorType::get(sgShape, resultType.getElementType());

    for (size_t i = 0; i < numVariants; ++i) {
      SmallVector<Value> opOperands;
      for (auto &operandVec : operands)
        opOperands.push_back(operandVec[i]);

      OperationState state(op->getLoc(), op->getName());
      state.addOperands(opOperands);
      state.addTypes(newResultType);
      // Copy all attributes, but update "layout_result_0" to drop
      // sgLayout/sgData
      for (auto attr : op->getAttrs()) {
        if (auto layout = dyn_cast<xegpu::LayoutAttr>(attr.getValue())) {
          if (auto newLayout = layout.dropSgLayoutAndData())
            state.addAttribute(attr.getName(), newLayout);
        } else {
          state.addAttribute(attr.getName(), attr.getValue());
        }
      }
      Operation *newOp = rewriter.create(state);
      newResults.push_back(newOp->getResult(0));
    }

    rewriter.replaceOpWithMultiple(op, {newResults});
    return success();
  }
};

// clang-format off
// Pattern for lowering ConvertLayoutOp based on sg_layout and sg_data.
// If input_layout and target_layout have identical sg_layout and sg_data,
// the op is rewritten to a subgroup-level ConvertLayoutOp with these fields
// dropped. For example:
//   #a = #xegpu.layout<sg_layout = [2, 2], sg_data = [16, 16], inst_data = [16, 16]>
//   #b = #xegpu.layout<sg_layout = [2, 2], sg_data = [16, 16], inst_data = [8, 16]>
//   xegpu.convert_layout %1 <{input_layout = #a, target_layout = #b}> : vector<32x64xf32>
// becomes:
//   #a = #xegpu.layout<inst_data = [16, 16]>
//   #b = #xegpu.layout<inst_data = [8, 16]>
//   xegpu.convert_layout %1 <{input_layout = #a, target_layout = #b}> : vector<16x16xf32>
// (vector<16x16xf32> is determined by sg_data = [16, 16])
//
// If sg_layout or sg_data differ, SLM is used to redistribute data across subgroups.
// For example:
//   #a = #xegpu.layout<sg_layout = [1, 4], sg_data = [32, 16], inst_data = [16, 16]>
//   #b = #xegpu.layout<sg_layout = [2, 2], sg_data = [16, 32], inst_data = [8, 16]>
//   xegpu.convert_layout %1 <{input_layout = #a, target_layout = #b}> : vector<32x64xf32>
// is lowered to:
//   #a = #xegpu.layout<inst_data = [16, 16]>
//   #b = #xegpu.layout<inst_data = [8, 16]>
//   store_matrix %1, %slm <{layout_input_0 = #a}> : vector<32x16>, matrix_desc<32x64xf32>
//   %d = load_matrix %slm <{layout_result_0 = #a}> : matrix_desc<32x64xf32> -> vector<16x32xf32>
//   xegpu.convert_layout %d <{input_layout = #a, target_layout = #b}> : vector<16x32xf32>
// clang-format on
struct WgToSgConvertLayoutOp
    : public OpConversionPattern<xegpu::ConvertLayoutOp> {
  using OpConversionPattern<xegpu::ConvertLayoutOp>::OpConversionPattern;
  LogicalResult
  matchAndRewrite(xegpu::ConvertLayoutOp op, OneToNOpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    xegpu::LayoutAttr input = op.getInputLayout();
    xegpu::LayoutAttr target = op.getTargetLayout();

    if (!input || !target || !input.isWgLayout() || !target.isWgLayout())
      return rewriter.notifyMatchFailure(
          op, "Input and target layouts must have subgroup layout");

    DenseI32ArrayAttr inputSgLayout = input.getSgLayout();
    DenseI32ArrayAttr inputSgData = input.getSgData();
    DenseI32ArrayAttr inputOrder = input.getOrder();
    DenseI32ArrayAttr targetSgLayout = target.getSgLayout();
    DenseI32ArrayAttr targetSgData = target.getSgData();
    DenseI32ArrayAttr targetOrder = target.getOrder();

    // TODO: currently we only support for optimal case, where input and
    // output has the same sg_layout and sg_data, so SLM is not involved.
    if (inputSgLayout != targetSgLayout || inputSgData != targetSgData ||
        inputOrder != targetOrder)
      return failure();

    input = input.dropSgLayoutAndData();
    target = target.dropSgLayoutAndData();

    SmallVector<Value> newOps(adaptor.getSource());
    if (input && target) {
      // keep the ConvertLayoutOp for rest fields, e.g., inst_data.
      for (auto [i, src] : llvm::enumerate(adaptor.getSource())) {
        auto newOp = xegpu::ConvertLayoutOp::create(
            rewriter, op.getLoc(), src.getType(), src, input, target);
        newOps[i] = newOp;
      }
    }
    rewriter.replaceOpWithMultiple(op, {newOps});
    return success();
  }
};

// Handles UnrealizedConversionCastOp generated during
// SCFStructuralTypeConversions (step 1). This op may appear as either a
// target or source materialization for Vector values, e.g.:
// 1. unrealized_cast %1 : vector<256xf32> to vector<16xf32>, ...
// 2. unrealized_cast %1 : vector<16xf32>, ... to vector<256xf32>
// it could be either 1:N or N:1 cast. In both cases, the pattern
// simply forwards the inputs to the outputs using 1:1 or 1:N interface.
// for example, the following scf::forOp
// ```
// %for = scf.for ... iter_args(%arg1 = %0)->(vector<128x128xf16>) {
//     %n = use(%arg1): vector<128x128xf16>
//     scf.yield %n : vector<128x128xf16>
// }
// ```
// Could be converted to:
// ```
// %1 = unrealized_conversion_cast %0
//          : vector<128x128xf16> to vector<16x16xf16>, vector<16x16xf16>
// %for:2 = scf.for ... iter_args(%arg1 = %1#1, %arg2 = %1#2)
//                    -> (vector<16x16xf16>, vector<16x16xf16) {
//     %m = unrealized_conversion_cast %arg1, %arg2
//            : vector<16x16xf16>, vector<16x16xf16> to vector<128x128xf16>
//     %n = use(%m): vector<128x128xf16>
//     %b = unrealized_conversion_cast %n
//            : vector<128x128xf16> to vector<16x16xf16>, vector<16x16xf16>
//     scf.yield %b#1, %b#2 : vector<16x16xf16>, vector<16x16xf16>
// }
// %cast = unrealized_conversion_cast %for:2
//          : vector<16x16xf16>, vector<16x16xf16> to vector<128x128xf16>
// ```
// TODO: remove it when context-aware type converter is ready.
struct UnrealizedConversionCastOpPattern
    : public OpConversionPattern<mlir::UnrealizedConversionCastOp> {
  using OpConversionPattern<
      mlir::UnrealizedConversionCastOp>::OpConversionPattern;

  mlir::LogicalResult
  matchAndRewrite(mlir::UnrealizedConversionCastOp op, OneToNOpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    SmallVector<Value> inputs = xegpu::flattenValues(adaptor.getInputs());

    auto inputTy = dyn_cast<VectorType>(inputs[0].getType());
    auto outputTy = dyn_cast<VectorType>(op->getOpResult(0).getType());

    if (!inputTy || !outputTy || !llvm::all_equal(op->getResultTypes()) ||
        !llvm::all_equal(ValueRange(inputs).getTypes()))
      return failure();

    // Handles the case "cast %1 : vector<256xf32> to vector<16xf32>, ...".
    // It is generated by source materialization (e.g., inits to scf forOp).
    // The input values provided by the adaptor should already be distributed,
    // and their types should correspond exactly to the result types of the
    // operation.
    if (op.getNumOperands() == 1 &&
        llvm::equal(ValueRange(inputs).getTypes(), op->getResultTypes())) {
      rewriter.replaceOp(op, inputs);
      return success();
    }

    // Handles the case "cast %1 : vector<16xf32>, ... to vector<256xf32>".
    // It is generated by target materialization (e.g., arguments/results
    // of scf forOp). All input values must have the same vector type, and
    // their shape must be evenly divisible by the output vector's shape
    // (determined by the nature of the workgroup to subgroup distribution).
    // TODO: it is not safe to do such forward, since such N:1 cast could be
    // from others.
    if (op.getNumResults() == 1 &&
        computeShapeRatio(outputTy.getShape(), inputTy.getShape())) {
      rewriter.replaceOpWithMultiple(op, {inputs});
      return success();
    }

    return mlir::failure();
  }
};

} // namespace

namespace mlir {
namespace xegpu {
void populateXeGPUWgToSgDistributePatterns(RewritePatternSet &patterns) {
  patterns.add<WgToSgCreateNdOp, WgToSgLoadNdOp, WgToSgStoreNdOp,
               WgToSgUpdateNdOffsetOp, WgToSgDpasOp, WgToSgPrefetchNdOp,
               UnrealizedConversionCastOpPattern, WgToSgElementwiseOp,
               WgToSgVectorBroadcastOp, WgToSgConvertLayoutOp>(
      patterns.getContext());
}
} // namespace xegpu
} // namespace mlir

namespace {
struct XeGPUWgToSgDistributePass
    : public xegpu::impl::XeGPUWgToSgDistributeBase<XeGPUWgToSgDistributePass> {
  void runOnOperation() override;
};
} // namespace

void XeGPUWgToSgDistributePass::runOnOperation() {
  // Track existing UnrealizedConversionCastOps
  SmallVector<Operation *> existingCastOps;
  getOperation()->walk([&](UnrealizedConversionCastOp castOp) {
    existingCastOps.push_back(castOp.getOperation());
  });

  {
    // Step 1: Apply SCFStructuralTypeConversions to SCF operations with
    // VectorType operands. This first converts such operands to
    // RankedTensorType, propagates the layout attribute into the encoding
    // attribute, and finally converts the RankedTensorType to VectorType based
    // on the encoding.

    TypeConverter converter;
    converter.addConversion([&](Type type) -> Type { return type; });
    converter.addConversion(
        [&](RankedTensorType type,
            SmallVectorImpl<Type> &result) -> std::optional<LogicalResult> {
          Type elemTy = type.getElementType();
          ArrayRef<int64_t> shape = type.getShape();

          int count;
          SmallVector<int64_t> subShape;
          std::tie(subShape, count) = getSgShapeAndCount(
              shape,
              dyn_cast_if_present<xegpu::LayoutAttr>(type.getEncoding()));

          auto newTy = VectorType::get(subShape, elemTy);
          result.append(count, newTy);
          return success();
        });

    xegpu::doSCFStructuralTypeConversionWithTensorType(getOperation(),
                                                       converter);
  }

  // Step 2: Perform workgroup to subgroup distribution for TensorDesc values,
  // as well as XeGPU, Arith, and Vector operations.
  MLIRContext *ctx = &getContext();
  RewritePatternSet patterns(ctx);
  ConversionTarget target(*ctx);
  TypeConverter converter;
  converter.addConversion([&](Type type) -> Type { return type; });
  converter.addConversion(
      [&](xegpu::TensorDescType type,
          SmallVectorImpl<Type> &result) -> std::optional<LogicalResult> {
        Type elemTy = type.getElementType();
        ArrayRef<int64_t> shape = type.getShape();

        int count;
        SmallVector<int64_t> subShape;
        xegpu::LayoutAttr layout = type.getLayoutAttr();
        std::tie(subShape, count) = getSgShapeAndCount(shape, layout);

        if (layout)
          layout = layout.dropSgLayoutAndData();

        auto newTy = xegpu::TensorDescType::get(
            type.getContext(), subShape, elemTy, type.getEncoding(), layout);
        result.append(count, newTy);
        return success();
      });

  auto getTensorDescType = [](Operation *op) -> xegpu::TensorDescType {
    if (auto createOp = dyn_cast<xegpu::CreateNdDescOp>(op))
      return createOp.getType();
    if (auto loadOp = dyn_cast<xegpu::LoadNdOp>(op))
      return loadOp.getTensorDescType();
    if (auto storeOp = dyn_cast<xegpu::StoreNdOp>(op))
      return storeOp.getTensorDescType();
    if (auto updateOp = dyn_cast<xegpu::UpdateNdOffsetOp>(op))
      return updateOp.getType();
    if (auto prefetchOp = dyn_cast<xegpu::PrefetchNdOp>(op))
      return prefetchOp.getTensorDescType();
    return xegpu::TensorDescType();
  };

  auto isLegal = [&](xegpu::LayoutAttr layout) -> bool {
    return !layout || !layout.isWgLayout();
  };

  target.addDynamicallyLegalOp<xegpu::CreateNdDescOp, xegpu::LoadNdOp,
                               xegpu::StoreNdOp, xegpu::UpdateNdOffsetOp,
                               xegpu::PrefetchNdOp>([=](Operation *op) -> bool {
    auto tdescTy = getTensorDescType(op);
    auto layout = dyn_cast_if_present<xegpu::LayoutAttr>(tdescTy.getLayout());
    return isLegal(layout);
  });

  target.addDynamicallyLegalOp<xegpu::DpasOp>([=](xegpu::DpasOp op) -> bool {
    auto layout = xegpu::getLayoutAttr(op.getResult());
    return isLegal(layout);
  });

  target.addDynamicallyLegalOp<vector::BroadcastOp>(
      [=](vector::BroadcastOp op) -> bool {
        return isLegal(xegpu::getLayoutAttr(op.getResult()));
      });

  target.addDynamicallyLegalOp<xegpu::ConvertLayoutOp>(
      [=](xegpu::ConvertLayoutOp op) -> bool {
        return isLegal(op.getInputLayout()) && isLegal(op.getTargetLayout());
      });

  target.addDynamicallyLegalDialect<math::MathDialect, arith::ArithDialect>(
      [=](Operation *op) -> std::optional<bool> {
        // Only handle elementwise mappable ops
        if (!OpTrait::hasElementwiseMappableTraits(op))
          return true;

        VectorType resultType =
            dyn_cast<VectorType>(op->getResult(0).getType());
        if (!resultType)
          return true;

        // Check if all operands are vectors of the same shape
        // TODO: Support other types.
        for (Value operand : op->getOperands()) {
          VectorType operandType = dyn_cast<VectorType>(operand.getType());
          if (!operandType || operandType.getShape() != resultType.getShape()) {
            return true;
          }
        }

        xegpu::LayoutAttr layout = xegpu::getLayoutAttr(op->getResult(0));
        return isLegal(layout);
      });

  target.addDynamicallyLegalOp<UnrealizedConversionCastOp>(
      [=](UnrealizedConversionCastOp op) {
        return llvm::is_contained(existingCastOps, op.getOperation());
      });

  target.markUnknownOpDynamicallyLegal([](Operation *) { return true; });

  scf::populateSCFStructuralTypeConversionsAndLegality(converter, patterns,
                                                       target);
  xegpu::populateXeGPUWgToSgDistributePatterns(patterns);
  if (failed(
          applyPartialConversion(getOperation(), target, std::move(patterns))))
    return signalPassFailure();

  // Remove sg_layout and sg_data attributes from the Layout
  // attribute for each VectorType result of the operation.
  // For Structured Control Flow ops, the layout is simply removed,
  // since in 1:N case, the layout for new results are missing.
  // Layout propagation pass will activated.
  getOperation()->walk([](Operation *op) {
    for (OpResult result : op->getOpResults()) {
      std::string name = xegpu::getLayoutName(result);
      if (auto layout = op->getAttrOfType<xegpu::LayoutAttr>(name)) {
        op->removeAttr(name);
        if (!isa<scf::IfOp, scf::ForOp, scf::WhileOp, scf::ConditionOp>(op)) {
          if (auto newLayout = layout.dropSgLayoutAndData())
            op->setAttr(name, newLayout);
        }
      }
    }
  });
}
