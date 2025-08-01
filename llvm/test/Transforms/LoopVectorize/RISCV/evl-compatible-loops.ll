; NOTE: Assertions have been autogenerated by utils/update_test_checks.py UTC_ARGS: --version 4
; RUN: opt -passes=loop-vectorize -force-tail-folding-style=data-with-evl \
; RUN: -prefer-predicate-over-epilogue=predicate-dont-vectorize \
; RUN: -mtriple=riscv64 -mattr=+v -S < %s | FileCheck %s

; Make sure we do not vectorize a loop with a widened int induction.
define void @test_wide_integer_induction(ptr noalias %a, i64 %N) {
; CHECK-LABEL: define void @test_wide_integer_induction(
; CHECK-SAME: ptr noalias [[A:%.*]], i64 [[N:%.*]]) #[[ATTR0:[0-9]+]] {
; CHECK-NEXT:  entry:
; CHECK-NEXT:    br i1 false, label [[SCALAR_PH:%.*]], label [[ENTRY:%.*]]
; CHECK:       vector.ph:
; CHECK-NEXT:    [[TMP4:%.*]] = call i64 @llvm.vscale.i64()
; CHECK-NEXT:    [[TMP5:%.*]] = mul nuw i64 [[TMP4]], 2
; CHECK-NEXT:    [[TMP6:%.*]] = sub i64 [[TMP5]], 1
; CHECK-NEXT:    [[N_RND_UP:%.*]] = add i64 [[N]], [[TMP6]]
; CHECK-NEXT:    [[N_MOD_VF:%.*]] = urem i64 [[N_RND_UP]], [[TMP5]]
; CHECK-NEXT:    [[N_VEC:%.*]] = sub i64 [[N_RND_UP]], [[N_MOD_VF]]
; CHECK-NEXT:    [[TMP7:%.*]] = call i64 @llvm.vscale.i64()
; CHECK-NEXT:    [[TMP8:%.*]] = mul nuw i64 [[TMP7]], 2
; CHECK-NEXT:    [[TMP9:%.*]] = call <vscale x 2 x i64> @llvm.stepvector.nxv2i64()
; CHECK-NEXT:    [[TMP10:%.*]] = mul <vscale x 2 x i64> [[TMP9]], splat (i64 1)
; CHECK-NEXT:    [[INDUCTION:%.*]] = add <vscale x 2 x i64> zeroinitializer, [[TMP10]]
; CHECK-NEXT:    br label [[FOR_BODY:%.*]]
; CHECK:       vector.body:
; CHECK-NEXT:    [[IV:%.*]] = phi i64 [ 0, [[ENTRY]] ], [ [[IV_NEXT:%.*]], [[FOR_BODY]] ]
; CHECK-NEXT:    [[EVL_BASED_IV:%.*]] = phi i64 [ 0, [[ENTRY]] ], [ [[INDEX_EVL_NEXT:%.*]], [[FOR_BODY]] ]
; CHECK-NEXT:    [[VEC_IND:%.*]] = phi <vscale x 2 x i64> [ [[INDUCTION]], [[ENTRY]] ], [ [[VEC_IND_NEXT:%.*]], [[FOR_BODY]] ]
; CHECK-NEXT:    [[AVL:%.*]] = sub i64 [[N]], [[EVL_BASED_IV]]
; CHECK-NEXT:    [[TMP11:%.*]] = call i32 @llvm.experimental.get.vector.length.i64(i64 [[AVL]], i32 2, i1 true)
; CHECK-NEXT:    [[TMP12:%.*]] = zext i32 [[TMP11]] to i64
; CHECK-NEXT:    [[TMP13:%.*]] = mul i64 1, [[TMP12]]
; CHECK-NEXT:    [[BROADCAST_SPLATINSERT:%.*]] = insertelement <vscale x 2 x i64> poison, i64 [[TMP13]], i64 0
; CHECK-NEXT:    [[BROADCAST_SPLAT:%.*]] = shufflevector <vscale x 2 x i64> [[BROADCAST_SPLATINSERT]], <vscale x 2 x i64> poison, <vscale x 2 x i32> zeroinitializer
; CHECK-NEXT:    [[TMP14:%.*]] = getelementptr inbounds i64, ptr [[A]], i64 [[EVL_BASED_IV]]
; CHECK-NEXT:    call void @llvm.vp.store.nxv2i64.p0(<vscale x 2 x i64> [[VEC_IND]], ptr align 8 [[TMP14]], <vscale x 2 x i1> splat (i1 true), i32 [[TMP11]])
; CHECK-NEXT:    [[TMP16:%.*]] = zext i32 [[TMP11]] to i64
; CHECK-NEXT:    [[INDEX_EVL_NEXT]] = add i64 [[TMP16]], [[EVL_BASED_IV]]
; CHECK-NEXT:    [[IV_NEXT]] = add i64 [[IV]], [[TMP8]]
; CHECK-NEXT:    [[VEC_IND_NEXT]] = add <vscale x 2 x i64> [[VEC_IND]], [[BROADCAST_SPLAT]]
; CHECK-NEXT:    [[TMP17:%.*]] = icmp eq i64 [[IV_NEXT]], [[N_VEC]]
; CHECK-NEXT:    br i1 [[TMP17]], label [[MIDDLE_BLOCK:%.*]], label [[FOR_BODY]], !llvm.loop [[LOOP0:![0-9]+]]
; CHECK:       middle.block:
; CHECK-NEXT:    br label [[FOR_COND_CLEANUP:%.*]]
; CHECK:       scalar.ph:
; CHECK-NEXT:    [[BC_RESUME_VAL:%.*]] = phi i64 [ 0, [[ENTRY1:%.*]] ]
; CHECK-NEXT:    br label [[FOR_BODY1:%.*]]
; CHECK:       for.body:
; CHECK-NEXT:    [[IV1:%.*]] = phi i64 [ [[BC_RESUME_VAL]], [[SCALAR_PH]] ], [ [[IV_NEXT1:%.*]], [[FOR_BODY1]] ]
; CHECK-NEXT:    [[ARRAYIDX:%.*]] = getelementptr inbounds i64, ptr [[A]], i64 [[IV1]]
; CHECK-NEXT:    store i64 [[IV1]], ptr [[ARRAYIDX]], align 8
; CHECK-NEXT:    [[IV_NEXT1]] = add nuw nsw i64 [[IV1]], 1
; CHECK-NEXT:    [[EXITCOND_NOT:%.*]] = icmp eq i64 [[IV_NEXT1]], [[N]]
; CHECK-NEXT:    br i1 [[EXITCOND_NOT]], label [[FOR_COND_CLEANUP]], label [[FOR_BODY1]], !llvm.loop [[LOOP4:![0-9]+]]
; CHECK:       for.cond.cleanup:
; CHECK-NEXT:    ret void
;
entry:
  br label %for.body

for.body:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %for.body ]
  %arrayidx = getelementptr inbounds i64, ptr %a, i64 %iv
  store i64 %iv, ptr %arrayidx, align 8
  %iv.next = add nuw nsw i64 %iv, 1
  %exitcond.not = icmp eq i64 %iv.next, %N
  br i1 %exitcond.not, label %for.cond.cleanup, label %for.body

for.cond.cleanup:
  ret void
}

; Make sure we do not vectorize a loop with a widened ptr induction.
define void @test_wide_ptr_induction(ptr noalias %a, ptr noalias %b, i64 %N) {
; CHECK-LABEL: define void @test_wide_ptr_induction(
; CHECK-SAME: ptr noalias [[A:%.*]], ptr noalias [[B:%.*]], i64 [[N:%.*]]) #[[ATTR0]] {
; CHECK-NEXT:  entry:
; CHECK-NEXT:    br label [[VECTOR_BODY:%.*]]
; CHECK:       for.body:
; CHECK-NEXT:    [[EVL_BASED_IV:%.*]] = phi i64 [ 0, [[VECTOR_PH:%.*]] ], [ [[INDEX_EVL_NEXT:%.*]], [[VECTOR_BODY]] ]
; CHECK-NEXT:    [[ADDR:%.*]] = phi ptr [ [[INCDEC_PTR:%.*]], [[VECTOR_BODY]] ], [ [[B]], [[VECTOR_PH]] ]
; CHECK-NEXT:    [[INCDEC_PTR]] = getelementptr inbounds i8, ptr [[ADDR]], i64 8
; CHECK-NEXT:    [[ARRAYIDX:%.*]] = getelementptr inbounds i64, ptr [[A]], i64 [[EVL_BASED_IV]]
; CHECK-NEXT:    store ptr [[ADDR]], ptr [[ARRAYIDX]], align 8
; CHECK-NEXT:    [[INDEX_EVL_NEXT]] = add nuw nsw i64 [[EVL_BASED_IV]], 1
; CHECK-NEXT:    [[EXITCOND_NOT:%.*]] = icmp eq i64 [[INDEX_EVL_NEXT]], [[N]]
; CHECK-NEXT:    br i1 [[EXITCOND_NOT]], label [[FOR_COND_CLEANUP:%.*]], label [[VECTOR_BODY]]
; CHECK:       for.cond.cleanup:
; CHECK-NEXT:    ret void
;
entry:
  br label %for.body

for.body:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %for.body ]
  %addr = phi ptr [ %incdec.ptr, %for.body ], [ %b, %entry ]
  %incdec.ptr = getelementptr inbounds i8, ptr %addr, i64 8
  %arrayidx = getelementptr inbounds i64, ptr %a, i64 %iv
  store ptr %addr, ptr %arrayidx, align 8
  %iv.next = add nuw nsw i64 %iv, 1
  %exitcond.not = icmp eq i64 %iv.next, %N
  br i1 %exitcond.not, label %for.cond.cleanup, label %for.body

for.cond.cleanup:
  ret void
}
;.
; CHECK: [[LOOP0]] = distinct !{[[LOOP0]], [[META1:![0-9]+]], [[META2:![0-9]+]], [[META3:![0-9]+]]}
; CHECK: [[META1]] = !{!"llvm.loop.isvectorized", i32 1}
; CHECK: [[META2]] = !{!"llvm.loop.isvectorized.tailfoldingstyle", !"evl"}
; CHECK: [[META3]] = !{!"llvm.loop.unroll.runtime.disable"}
; CHECK: [[LOOP4]] = distinct !{[[LOOP4]], [[META3]], [[META1]]}
;.
