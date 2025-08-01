; NOTE: Assertions have been autogenerated by utils/update_test_checks.py
; RUN: opt -passes=loop-vectorize -mtriple=arm64-apple-macos -mcpu=cortex-a57 -S %s | FileCheck --check-prefix=INTERLEAVE-4 %s
; RUN: opt -passes=loop-vectorize -mtriple=arm64-apple-macos -mcpu=cortex-a75 -S %s | FileCheck --check-prefix=INTERLEAVE-2 %s
; RUN: opt -passes=loop-vectorize -mtriple=arm64-apple-macos -mcpu=apple-m1 -S %s | FileCheck --check-prefix=INTERLEAVE-4 %s
; RUN: opt -passes=loop-vectorize -mtriple=arm64-apple-macos -mcpu=apple-a14 -S %s | FileCheck --check-prefix=INTERLEAVE-4 %s
; RUN: opt -passes=loop-vectorize -mtriple=arm64-apple-macos -mcpu=apple-a15 -S %s | FileCheck --check-prefix=INTERLEAVE-4 %s
; RUN: opt -passes=loop-vectorize -mtriple=arm64-apple-macos -mcpu=apple-a16 -S %s | FileCheck --check-prefix=INTERLEAVE-4 %s
; RUN: opt -passes=loop-vectorize -mtriple=arm64 -mcpu=neoverse-v2 -S %s | FileCheck --check-prefix=INTERLEAVE-4 %s
; RUN: opt -passes=loop-vectorize -mtriple=arm64 -mcpu=neoverse-v3 -S %s | FileCheck --check-prefix=INTERLEAVE-4-VLA %s

; Tests for selecting the interleave count for loops with reductions.

define i32 @interleave_integer_reduction(ptr %src, i64 %N) {
; INTERLEAVE-4-LABEL: @interleave_integer_reduction(
; INTERLEAVE-4-NEXT:  iter.check:
; INTERLEAVE-4-NEXT:    [[MIN_ITERS_CHECK:%.*]] = icmp ult i64 [[N:%.*]], 4
; INTERLEAVE-4-NEXT:    br i1 [[MIN_ITERS_CHECK]], label [[VEC_EPILOG_SCALAR_PH:%.*]], label [[VECTOR_MAIN_LOOP_ITER_CHECK:%.*]]
; INTERLEAVE-4:       vector.main.loop.iter.check:
; INTERLEAVE-4-NEXT:    [[MIN_ITERS_CHECK1:%.*]] = icmp ult i64 [[N]], 16
; INTERLEAVE-4-NEXT:    br i1 [[MIN_ITERS_CHECK1]], label [[VEC_EPILOG_PH:%.*]], label [[VECTOR_PH:%.*]]
; INTERLEAVE-4:       vector.ph:
; INTERLEAVE-4-NEXT:    [[N_MOD_VF:%.*]] = urem i64 [[N]], 16
; INTERLEAVE-4-NEXT:    [[N_VEC:%.*]] = sub i64 [[N]], [[N_MOD_VF]]
; INTERLEAVE-4-NEXT:    br label [[VECTOR_BODY:%.*]]
; INTERLEAVE-4:       vector.body:
; INTERLEAVE-4-NEXT:    [[INDEX:%.*]] = phi i64 [ 0, [[VECTOR_PH]] ], [ [[INDEX_NEXT:%.*]], [[VECTOR_BODY]] ]
; INTERLEAVE-4-NEXT:    [[VEC_PHI:%.*]] = phi <4 x i32> [ zeroinitializer, [[VECTOR_PH]] ], [ [[TMP12:%.*]], [[VECTOR_BODY]] ]
; INTERLEAVE-4-NEXT:    [[VEC_PHI2:%.*]] = phi <4 x i32> [ zeroinitializer, [[VECTOR_PH]] ], [ [[TMP13:%.*]], [[VECTOR_BODY]] ]
; INTERLEAVE-4-NEXT:    [[VEC_PHI3:%.*]] = phi <4 x i32> [ zeroinitializer, [[VECTOR_PH]] ], [ [[TMP14:%.*]], [[VECTOR_BODY]] ]
; INTERLEAVE-4-NEXT:    [[VEC_PHI4:%.*]] = phi <4 x i32> [ zeroinitializer, [[VECTOR_PH]] ], [ [[TMP15:%.*]], [[VECTOR_BODY]] ]
; INTERLEAVE-4-NEXT:    [[TMP4:%.*]] = getelementptr inbounds i32, ptr [[SRC:%.*]], i64 [[INDEX]]
; INTERLEAVE-4-NEXT:    [[TMP9:%.*]] = getelementptr inbounds i32, ptr [[TMP4]], i32 4
; INTERLEAVE-4-NEXT:    [[TMP10:%.*]] = getelementptr inbounds i32, ptr [[TMP4]], i32 8
; INTERLEAVE-4-NEXT:    [[TMP11:%.*]] = getelementptr inbounds i32, ptr [[TMP4]], i32 12
; INTERLEAVE-4-NEXT:    [[WIDE_LOAD:%.*]] = load <4 x i32>, ptr [[TMP4]], align 1
; INTERLEAVE-4-NEXT:    [[WIDE_LOAD5:%.*]] = load <4 x i32>, ptr [[TMP9]], align 1
; INTERLEAVE-4-NEXT:    [[WIDE_LOAD6:%.*]] = load <4 x i32>, ptr [[TMP10]], align 1
; INTERLEAVE-4-NEXT:    [[WIDE_LOAD7:%.*]] = load <4 x i32>, ptr [[TMP11]], align 1
; INTERLEAVE-4-NEXT:    [[TMP12]] = add <4 x i32> [[VEC_PHI]], [[WIDE_LOAD]]
; INTERLEAVE-4-NEXT:    [[TMP13]] = add <4 x i32> [[VEC_PHI2]], [[WIDE_LOAD5]]
; INTERLEAVE-4-NEXT:    [[TMP14]] = add <4 x i32> [[VEC_PHI3]], [[WIDE_LOAD6]]
; INTERLEAVE-4-NEXT:    [[TMP15]] = add <4 x i32> [[VEC_PHI4]], [[WIDE_LOAD7]]
; INTERLEAVE-4-NEXT:    [[INDEX_NEXT]] = add nuw i64 [[INDEX]], 16
; INTERLEAVE-4-NEXT:    [[TMP16:%.*]] = icmp eq i64 [[INDEX_NEXT]], [[N_VEC]]
; INTERLEAVE-4-NEXT:    br i1 [[TMP16]], label [[MIDDLE_BLOCK:%.*]], label [[VECTOR_BODY]], !llvm.loop [[LOOP0:![0-9]+]]
; INTERLEAVE-4:       middle.block:
; INTERLEAVE-4-NEXT:    [[BIN_RDX:%.*]] = add <4 x i32> [[TMP13]], [[TMP12]]
; INTERLEAVE-4-NEXT:    [[BIN_RDX8:%.*]] = add <4 x i32> [[TMP14]], [[BIN_RDX]]
; INTERLEAVE-4-NEXT:    [[BIN_RDX9:%.*]] = add <4 x i32> [[TMP15]], [[BIN_RDX8]]
; INTERLEAVE-4-NEXT:    [[TMP17:%.*]] = call i32 @llvm.vector.reduce.add.v4i32(<4 x i32> [[BIN_RDX9]])
; INTERLEAVE-4-NEXT:    [[CMP_N:%.*]] = icmp eq i64 [[N]], [[N_VEC]]
; INTERLEAVE-4-NEXT:    br i1 [[CMP_N]], label [[EXIT:%.*]], label [[VEC_EPILOG_ITER_CHECK:%.*]]
; INTERLEAVE-4:       vec.epilog.iter.check:
; INTERLEAVE-4-NEXT:    [[N_VEC_REMAINING:%.*]] = sub i64 [[N]], [[N_VEC]]
; INTERLEAVE-4-NEXT:    [[MIN_EPILOG_ITERS_CHECK:%.*]] = icmp ult i64 [[N_VEC_REMAINING]], 4
; INTERLEAVE-4-NEXT:    br i1 [[MIN_EPILOG_ITERS_CHECK]], label [[VEC_EPILOG_SCALAR_PH]], label [[VEC_EPILOG_PH]]
; INTERLEAVE-4:       vec.epilog.ph:
; INTERLEAVE-4-NEXT:    [[VEC_EPILOG_RESUME_VAL:%.*]] = phi i64 [ [[N_VEC]], [[VEC_EPILOG_ITER_CHECK]] ], [ 0, [[VECTOR_MAIN_LOOP_ITER_CHECK]] ]
; INTERLEAVE-4-NEXT:    [[BC_MERGE_RDX:%.*]] = phi i32 [ [[TMP17]], [[VEC_EPILOG_ITER_CHECK]] ], [ 0, [[VECTOR_MAIN_LOOP_ITER_CHECK]] ]
; INTERLEAVE-4-NEXT:    [[N_MOD_VF10:%.*]] = urem i64 [[N]], 4
; INTERLEAVE-4-NEXT:    [[N_VEC11:%.*]] = sub i64 [[N]], [[N_MOD_VF10]]
; INTERLEAVE-4-NEXT:    [[TMP18:%.*]] = insertelement <4 x i32> zeroinitializer, i32 [[BC_MERGE_RDX]], i32 0
; INTERLEAVE-4-NEXT:    br label [[VEC_EPILOG_VECTOR_BODY:%.*]]
; INTERLEAVE-4:       vec.epilog.vector.body:
; INTERLEAVE-4-NEXT:    [[INDEX12:%.*]] = phi i64 [ [[VEC_EPILOG_RESUME_VAL]], [[VEC_EPILOG_PH]] ], [ [[INDEX_NEXT15:%.*]], [[VEC_EPILOG_VECTOR_BODY]] ]
; INTERLEAVE-4-NEXT:    [[VEC_PHI13:%.*]] = phi <4 x i32> [ [[TMP18]], [[VEC_EPILOG_PH]] ], [ [[TMP22:%.*]], [[VEC_EPILOG_VECTOR_BODY]] ]
; INTERLEAVE-4-NEXT:    [[TMP20:%.*]] = getelementptr inbounds i32, ptr [[SRC]], i64 [[INDEX12]]
; INTERLEAVE-4-NEXT:    [[WIDE_LOAD14:%.*]] = load <4 x i32>, ptr [[TMP20]], align 1
; INTERLEAVE-4-NEXT:    [[TMP22]] = add <4 x i32> [[VEC_PHI13]], [[WIDE_LOAD14]]
; INTERLEAVE-4-NEXT:    [[INDEX_NEXT15]] = add nuw i64 [[INDEX12]], 4
; INTERLEAVE-4-NEXT:    [[TMP23:%.*]] = icmp eq i64 [[INDEX_NEXT15]], [[N_VEC11]]
; INTERLEAVE-4-NEXT:    br i1 [[TMP23]], label [[VEC_EPILOG_MIDDLE_BLOCK:%.*]], label [[VEC_EPILOG_VECTOR_BODY]], !llvm.loop [[LOOP3:![0-9]+]]
; INTERLEAVE-4:       vec.epilog.middle.block:
; INTERLEAVE-4-NEXT:    [[TMP24:%.*]] = call i32 @llvm.vector.reduce.add.v4i32(<4 x i32> [[TMP22]])
; INTERLEAVE-4-NEXT:    [[CMP_N16:%.*]] = icmp eq i64 [[N]], [[N_VEC11]]
; INTERLEAVE-4-NEXT:    br i1 [[CMP_N16]], label [[EXIT]], label [[VEC_EPILOG_SCALAR_PH]]
; INTERLEAVE-4:       vec.epilog.scalar.ph:
; INTERLEAVE-4-NEXT:    [[BC_RESUME_VAL:%.*]] = phi i64 [ [[N_VEC11]], [[VEC_EPILOG_MIDDLE_BLOCK]] ], [ [[N_VEC]], [[VEC_EPILOG_ITER_CHECK]] ], [ 0, [[ITER_CHECK:%.*]] ]
; INTERLEAVE-4-NEXT:    [[BC_MERGE_RDX17:%.*]] = phi i32 [ [[TMP24]], [[VEC_EPILOG_MIDDLE_BLOCK]] ], [ [[TMP17]], [[VEC_EPILOG_ITER_CHECK]] ], [ 0, [[ITER_CHECK]] ]
; INTERLEAVE-4-NEXT:    br label [[LOOP:%.*]]
; INTERLEAVE-4:       loop:
; INTERLEAVE-4-NEXT:    [[IV:%.*]] = phi i64 [ [[BC_RESUME_VAL]], [[VEC_EPILOG_SCALAR_PH]] ], [ [[IV_NEXT:%.*]], [[LOOP]] ]
; INTERLEAVE-4-NEXT:    [[RED:%.*]] = phi i32 [ [[BC_MERGE_RDX17]], [[VEC_EPILOG_SCALAR_PH]] ], [ [[RED_NEXT:%.*]], [[LOOP]] ]
; INTERLEAVE-4-NEXT:    [[GEP_SRC:%.*]] = getelementptr inbounds i32, ptr [[SRC]], i64 [[IV]]
; INTERLEAVE-4-NEXT:    [[L:%.*]] = load i32, ptr [[GEP_SRC]], align 1
; INTERLEAVE-4-NEXT:    [[RED_NEXT]] = add i32 [[RED]], [[L]]
; INTERLEAVE-4-NEXT:    [[IV_NEXT]] = add nuw nsw i64 [[IV]], 1
; INTERLEAVE-4-NEXT:    [[EC:%.*]] = icmp eq i64 [[IV_NEXT]], [[N]]
; INTERLEAVE-4-NEXT:    br i1 [[EC]], label [[EXIT]], label [[LOOP]], !llvm.loop [[LOOP4:![0-9]+]]
; INTERLEAVE-4:       exit:
; INTERLEAVE-4-NEXT:    [[RED_NEXT_LCSSA:%.*]] = phi i32 [ [[RED_NEXT]], [[LOOP]] ], [ [[TMP17]], [[MIDDLE_BLOCK]] ], [ [[TMP24]], [[VEC_EPILOG_MIDDLE_BLOCK]] ]
; INTERLEAVE-4-NEXT:    ret i32 [[RED_NEXT_LCSSA]]
;
; INTERLEAVE-2-LABEL: @interleave_integer_reduction(
; INTERLEAVE-2-NEXT:  entry:
; INTERLEAVE-2-NEXT:    [[MIN_ITERS_CHECK:%.*]] = icmp ult i64 [[N:%.*]], 8
; INTERLEAVE-2-NEXT:    br i1 [[MIN_ITERS_CHECK]], label [[SCALAR_PH:%.*]], label [[VECTOR_PH:%.*]]
; INTERLEAVE-2:       vector.ph:
; INTERLEAVE-2-NEXT:    [[N_MOD_VF:%.*]] = urem i64 [[N]], 8
; INTERLEAVE-2-NEXT:    [[N_VEC:%.*]] = sub i64 [[N]], [[N_MOD_VF]]
; INTERLEAVE-2-NEXT:    br label [[VECTOR_BODY:%.*]]
; INTERLEAVE-2:       vector.body:
; INTERLEAVE-2-NEXT:    [[INDEX:%.*]] = phi i64 [ 0, [[VECTOR_PH]] ], [ [[INDEX_NEXT:%.*]], [[VECTOR_BODY]] ]
; INTERLEAVE-2-NEXT:    [[VEC_PHI:%.*]] = phi <4 x i32> [ zeroinitializer, [[VECTOR_PH]] ], [ [[TMP6:%.*]], [[VECTOR_BODY]] ]
; INTERLEAVE-2-NEXT:    [[VEC_PHI1:%.*]] = phi <4 x i32> [ zeroinitializer, [[VECTOR_PH]] ], [ [[TMP7:%.*]], [[VECTOR_BODY]] ]
; INTERLEAVE-2-NEXT:    [[TMP2:%.*]] = getelementptr inbounds i32, ptr [[SRC:%.*]], i64 [[INDEX]]
; INTERLEAVE-2-NEXT:    [[TMP5:%.*]] = getelementptr inbounds i32, ptr [[TMP2]], i32 4
; INTERLEAVE-2-NEXT:    [[WIDE_LOAD:%.*]] = load <4 x i32>, ptr [[TMP2]], align 1
; INTERLEAVE-2-NEXT:    [[WIDE_LOAD2:%.*]] = load <4 x i32>, ptr [[TMP5]], align 1
; INTERLEAVE-2-NEXT:    [[TMP6]] = add <4 x i32> [[VEC_PHI]], [[WIDE_LOAD]]
; INTERLEAVE-2-NEXT:    [[TMP7]] = add <4 x i32> [[VEC_PHI1]], [[WIDE_LOAD2]]
; INTERLEAVE-2-NEXT:    [[INDEX_NEXT]] = add nuw i64 [[INDEX]], 8
; INTERLEAVE-2-NEXT:    [[TMP8:%.*]] = icmp eq i64 [[INDEX_NEXT]], [[N_VEC]]
; INTERLEAVE-2-NEXT:    br i1 [[TMP8]], label [[MIDDLE_BLOCK:%.*]], label [[VECTOR_BODY]], !llvm.loop [[LOOP0:![0-9]+]]
; INTERLEAVE-2:       middle.block:
; INTERLEAVE-2-NEXT:    [[BIN_RDX:%.*]] = add <4 x i32> [[TMP7]], [[TMP6]]
; INTERLEAVE-2-NEXT:    [[TMP9:%.*]] = call i32 @llvm.vector.reduce.add.v4i32(<4 x i32> [[BIN_RDX]])
; INTERLEAVE-2-NEXT:    [[CMP_N:%.*]] = icmp eq i64 [[N]], [[N_VEC]]
; INTERLEAVE-2-NEXT:    br i1 [[CMP_N]], label [[EXIT:%.*]], label [[SCALAR_PH]]
; INTERLEAVE-2:       scalar.ph:
; INTERLEAVE-2-NEXT:    [[BC_RESUME_VAL:%.*]] = phi i64 [ [[N_VEC]], [[MIDDLE_BLOCK]] ], [ 0, [[ENTRY:%.*]] ]
; INTERLEAVE-2-NEXT:    [[BC_MERGE_RDX:%.*]] = phi i32 [ [[TMP9]], [[MIDDLE_BLOCK]] ], [ 0, [[ENTRY]] ]
; INTERLEAVE-2-NEXT:    br label [[LOOP:%.*]]
; INTERLEAVE-2:       loop:
; INTERLEAVE-2-NEXT:    [[IV:%.*]] = phi i64 [ [[BC_RESUME_VAL]], [[SCALAR_PH]] ], [ [[IV_NEXT:%.*]], [[LOOP]] ]
; INTERLEAVE-2-NEXT:    [[RED:%.*]] = phi i32 [ [[BC_MERGE_RDX]], [[SCALAR_PH]] ], [ [[RED_NEXT:%.*]], [[LOOP]] ]
; INTERLEAVE-2-NEXT:    [[GEP_SRC:%.*]] = getelementptr inbounds i32, ptr [[SRC]], i64 [[IV]]
; INTERLEAVE-2-NEXT:    [[L:%.*]] = load i32, ptr [[GEP_SRC]], align 1
; INTERLEAVE-2-NEXT:    [[RED_NEXT]] = add i32 [[RED]], [[L]]
; INTERLEAVE-2-NEXT:    [[IV_NEXT]] = add nuw nsw i64 [[IV]], 1
; INTERLEAVE-2-NEXT:    [[EC:%.*]] = icmp eq i64 [[IV_NEXT]], [[N]]
; INTERLEAVE-2-NEXT:    br i1 [[EC]], label [[EXIT]], label [[LOOP]], !llvm.loop [[LOOP3:![0-9]+]]
; INTERLEAVE-2:       exit:
; INTERLEAVE-2-NEXT:    [[RED_NEXT_LCSSA:%.*]] = phi i32 [ [[RED_NEXT]], [[LOOP]] ], [ [[TMP9]], [[MIDDLE_BLOCK]] ]
; INTERLEAVE-2-NEXT:    ret i32 [[RED_NEXT_LCSSA]]
;
; INTERLEAVE-4-VLA-LABEL: @interleave_integer_reduction(
; INTERLEAVE-4-VLA-NEXT:  entry:
; INTERLEAVE-4-VLA-NEXT:    [[TMP0:%.*]] = call i64 @llvm.vscale.i64()
; INTERLEAVE-4-VLA-NEXT:    [[TMP1:%.*]] = mul nuw i64 [[TMP0]], 16
; INTERLEAVE-4-VLA-NEXT:    [[MIN_ITERS_CHECK:%.*]] = icmp ult i64 [[N:%.*]], [[TMP1]]
; INTERLEAVE-4-VLA-NEXT:    br i1 [[MIN_ITERS_CHECK]], label [[SCALAR_PH:%.*]], label [[VECTOR_PH:%.*]]
; INTERLEAVE-4-VLA:       vector.ph:
; INTERLEAVE-4-VLA-NEXT:    [[TMP2:%.*]] = call i64 @llvm.vscale.i64()
; INTERLEAVE-4-VLA-NEXT:    [[TMP3:%.*]] = mul nuw i64 [[TMP2]], 16
; INTERLEAVE-4-VLA-NEXT:    [[N_MOD_VF:%.*]] = urem i64 [[N]], [[TMP3]]
; INTERLEAVE-4-VLA-NEXT:    [[N_VEC:%.*]] = sub i64 [[N]], [[N_MOD_VF]]
; INTERLEAVE-4-VLA-NEXT:    [[TMP4:%.*]] = call i64 @llvm.vscale.i64()
; INTERLEAVE-4-VLA-NEXT:    [[TMP5:%.*]] = mul nuw i64 [[TMP4]], 16
; INTERLEAVE-4-VLA-NEXT:    br label [[VECTOR_BODY:%.*]]
; INTERLEAVE-4-VLA:       vector.body:
; INTERLEAVE-4-VLA-NEXT:    [[INDEX:%.*]] = phi i64 [ 0, [[VECTOR_PH]] ], [ [[INDEX_NEXT:%.*]], [[VECTOR_BODY]] ]
; INTERLEAVE-4-VLA-NEXT:    [[VEC_PHI:%.*]] = phi <vscale x 4 x i32> [ zeroinitializer, [[VECTOR_PH]] ], [ [[TMP16:%.*]], [[VECTOR_BODY]] ]
; INTERLEAVE-4-VLA-NEXT:    [[VEC_PHI1:%.*]] = phi <vscale x 4 x i32> [ zeroinitializer, [[VECTOR_PH]] ], [ [[TMP17:%.*]], [[VECTOR_BODY]] ]
; INTERLEAVE-4-VLA-NEXT:    [[VEC_PHI2:%.*]] = phi <vscale x 4 x i32> [ zeroinitializer, [[VECTOR_PH]] ], [ [[TMP18:%.*]], [[VECTOR_BODY]] ]
; INTERLEAVE-4-VLA-NEXT:    [[VEC_PHI3:%.*]] = phi <vscale x 4 x i32> [ zeroinitializer, [[VECTOR_PH]] ], [ [[TMP19:%.*]], [[VECTOR_BODY]] ]
; INTERLEAVE-4-VLA-NEXT:    [[TMP6:%.*]] = getelementptr inbounds i32, ptr [[SRC:%.*]], i64 [[INDEX]]
; INTERLEAVE-4-VLA-NEXT:    [[TMP7:%.*]] = call i64 @llvm.vscale.i64()
; INTERLEAVE-4-VLA-NEXT:    [[TMP8:%.*]] = mul nuw i64 [[TMP7]], 4
; INTERLEAVE-4-VLA-NEXT:    [[TMP9:%.*]] = getelementptr inbounds i32, ptr [[TMP6]], i64 [[TMP8]]
; INTERLEAVE-4-VLA-NEXT:    [[TMP10:%.*]] = call i64 @llvm.vscale.i64()
; INTERLEAVE-4-VLA-NEXT:    [[TMP11:%.*]] = mul nuw i64 [[TMP10]], 8
; INTERLEAVE-4-VLA-NEXT:    [[TMP12:%.*]] = getelementptr inbounds i32, ptr [[TMP6]], i64 [[TMP11]]
; INTERLEAVE-4-VLA-NEXT:    [[TMP13:%.*]] = call i64 @llvm.vscale.i64()
; INTERLEAVE-4-VLA-NEXT:    [[TMP14:%.*]] = mul nuw i64 [[TMP13]], 12
; INTERLEAVE-4-VLA-NEXT:    [[TMP15:%.*]] = getelementptr inbounds i32, ptr [[TMP6]], i64 [[TMP14]]
; INTERLEAVE-4-VLA-NEXT:    [[WIDE_LOAD:%.*]] = load <vscale x 4 x i32>, ptr [[TMP6]], align 1
; INTERLEAVE-4-VLA-NEXT:    [[WIDE_LOAD4:%.*]] = load <vscale x 4 x i32>, ptr [[TMP9]], align 1
; INTERLEAVE-4-VLA-NEXT:    [[WIDE_LOAD5:%.*]] = load <vscale x 4 x i32>, ptr [[TMP12]], align 1
; INTERLEAVE-4-VLA-NEXT:    [[WIDE_LOAD6:%.*]] = load <vscale x 4 x i32>, ptr [[TMP15]], align 1
; INTERLEAVE-4-VLA-NEXT:    [[TMP16]] = add <vscale x 4 x i32> [[VEC_PHI]], [[WIDE_LOAD]]
; INTERLEAVE-4-VLA-NEXT:    [[TMP17]] = add <vscale x 4 x i32> [[VEC_PHI1]], [[WIDE_LOAD4]]
; INTERLEAVE-4-VLA-NEXT:    [[TMP18]] = add <vscale x 4 x i32> [[VEC_PHI2]], [[WIDE_LOAD5]]
; INTERLEAVE-4-VLA-NEXT:    [[TMP19]] = add <vscale x 4 x i32> [[VEC_PHI3]], [[WIDE_LOAD6]]
; INTERLEAVE-4-VLA-NEXT:    [[INDEX_NEXT]] = add nuw i64 [[INDEX]], [[TMP5]]
; INTERLEAVE-4-VLA-NEXT:    [[TMP20:%.*]] = icmp eq i64 [[INDEX_NEXT]], [[N_VEC]]
; INTERLEAVE-4-VLA-NEXT:    br i1 [[TMP20]], label [[MIDDLE_BLOCK:%.*]], label [[VECTOR_BODY]], !llvm.loop [[LOOP0:![0-9]+]]
; INTERLEAVE-4-VLA:       middle.block:
; INTERLEAVE-4-VLA-NEXT:    [[BIN_RDX:%.*]] = add <vscale x 4 x i32> [[TMP17]], [[TMP16]]
; INTERLEAVE-4-VLA-NEXT:    [[BIN_RDX7:%.*]] = add <vscale x 4 x i32> [[TMP18]], [[BIN_RDX]]
; INTERLEAVE-4-VLA-NEXT:    [[BIN_RDX8:%.*]] = add <vscale x 4 x i32> [[TMP19]], [[BIN_RDX7]]
; INTERLEAVE-4-VLA-NEXT:    [[TMP21:%.*]] = call i32 @llvm.vector.reduce.add.nxv4i32(<vscale x 4 x i32> [[BIN_RDX8]])
; INTERLEAVE-4-VLA-NEXT:    [[CMP_N:%.*]] = icmp eq i64 [[N]], [[N_VEC]]
; INTERLEAVE-4-VLA-NEXT:    br i1 [[CMP_N]], label [[EXIT:%.*]], label [[SCALAR_PH]]
; INTERLEAVE-4-VLA:       scalar.ph:
; INTERLEAVE-4-VLA-NEXT:    [[BC_RESUME_VAL:%.*]] = phi i64 [ [[N_VEC]], [[MIDDLE_BLOCK]] ], [ 0, [[ENTRY:%.*]] ]
; INTERLEAVE-4-VLA-NEXT:    [[BC_MERGE_RDX:%.*]] = phi i32 [ [[TMP21]], [[MIDDLE_BLOCK]] ], [ 0, [[ENTRY]] ]
; INTERLEAVE-4-VLA-NEXT:    br label [[LOOP:%.*]]
; INTERLEAVE-4-VLA:       loop:
; INTERLEAVE-4-VLA-NEXT:    [[IV:%.*]] = phi i64 [ [[BC_RESUME_VAL]], [[SCALAR_PH]] ], [ [[IV_NEXT:%.*]], [[LOOP]] ]
; INTERLEAVE-4-VLA-NEXT:    [[RED:%.*]] = phi i32 [ [[BC_MERGE_RDX]], [[SCALAR_PH]] ], [ [[RED_NEXT:%.*]], [[LOOP]] ]
; INTERLEAVE-4-VLA-NEXT:    [[GEP_SRC:%.*]] = getelementptr inbounds i32, ptr [[SRC]], i64 [[IV]]
; INTERLEAVE-4-VLA-NEXT:    [[L:%.*]] = load i32, ptr [[GEP_SRC]], align 1
; INTERLEAVE-4-VLA-NEXT:    [[RED_NEXT]] = add i32 [[RED]], [[L]]
; INTERLEAVE-4-VLA-NEXT:    [[IV_NEXT]] = add nuw nsw i64 [[IV]], 1
; INTERLEAVE-4-VLA-NEXT:    [[EC:%.*]] = icmp eq i64 [[IV_NEXT]], [[N]]
; INTERLEAVE-4-VLA-NEXT:    br i1 [[EC]], label [[EXIT]], label [[LOOP]], !llvm.loop [[LOOP3:![0-9]+]]
; INTERLEAVE-4-VLA:       exit:
; INTERLEAVE-4-VLA-NEXT:    [[RED_NEXT_LCSSA:%.*]] = phi i32 [ [[RED_NEXT]], [[LOOP]] ], [ [[TMP21]], [[MIDDLE_BLOCK]] ]
; INTERLEAVE-4-VLA-NEXT:    ret i32 [[RED_NEXT_LCSSA]]
;
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop]
  %red = phi i32 [ 0, %entry ], [ %red.next, %loop ]
  %gep.src = getelementptr inbounds i32, ptr %src, i64 %iv
  %l = load i32, ptr %gep.src, align 1
  %red.next = add i32 %red, %l
  %iv.next = add nuw nsw i64 %iv, 1
  %ec = icmp eq i64 %iv.next, %N
  br i1 %ec, label %exit, label %loop

exit:
  ret i32 %red.next
}

declare i8 @llvm.smax.i8(i8, i8)
