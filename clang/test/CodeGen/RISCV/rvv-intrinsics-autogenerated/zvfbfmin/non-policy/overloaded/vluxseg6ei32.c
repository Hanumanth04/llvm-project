// NOTE: Assertions have been autogenerated by utils/update_cc_test_checks.py UTC_ARGS: --version 4
// REQUIRES: riscv-registered-target
// RUN: %clang_cc1 -triple riscv64 -target-feature +zve64x \
// RUN:   -target-feature +zvfbfmin -disable-O0-optnone \
// RUN:   -emit-llvm %s -o - | opt -S -passes=mem2reg | \
// RUN:   FileCheck --check-prefix=CHECK-RV64 %s

#include <riscv_vector.h>

// CHECK-RV64-LABEL: define dso_local target("riscv.vector.tuple", <vscale x 2 x i8>, 6) @test_vluxseg6ei32_v_bf16mf4x6(
// CHECK-RV64-SAME: ptr noundef [[RS1:%.*]], <vscale x 1 x i32> [[RS2:%.*]], i64 noundef [[VL:%.*]]) #[[ATTR0:[0-9]+]] {
// CHECK-RV64-NEXT:  entry:
// CHECK-RV64-NEXT:    [[TMP0:%.*]] = call target("riscv.vector.tuple", <vscale x 2 x i8>, 6) @llvm.riscv.vluxseg6.triscv.vector.tuple_nxv2i8_6t.p0.nxv1i32.i64(target("riscv.vector.tuple", <vscale x 2 x i8>, 6) poison, ptr [[RS1]], <vscale x 1 x i32> [[RS2]], i64 [[VL]], i64 4)
// CHECK-RV64-NEXT:    ret target("riscv.vector.tuple", <vscale x 2 x i8>, 6) [[TMP0]]
//
vbfloat16mf4x6_t test_vluxseg6ei32_v_bf16mf4x6(const __bf16 *rs1,
                                               vuint32mf2_t rs2, size_t vl) {
  return __riscv_vluxseg6ei32(rs1, rs2, vl);
}

// CHECK-RV64-LABEL: define dso_local target("riscv.vector.tuple", <vscale x 4 x i8>, 6) @test_vluxseg6ei32_v_bf16mf2x6(
// CHECK-RV64-SAME: ptr noundef [[RS1:%.*]], <vscale x 2 x i32> [[RS2:%.*]], i64 noundef [[VL:%.*]]) #[[ATTR0]] {
// CHECK-RV64-NEXT:  entry:
// CHECK-RV64-NEXT:    [[TMP0:%.*]] = call target("riscv.vector.tuple", <vscale x 4 x i8>, 6) @llvm.riscv.vluxseg6.triscv.vector.tuple_nxv4i8_6t.p0.nxv2i32.i64(target("riscv.vector.tuple", <vscale x 4 x i8>, 6) poison, ptr [[RS1]], <vscale x 2 x i32> [[RS2]], i64 [[VL]], i64 4)
// CHECK-RV64-NEXT:    ret target("riscv.vector.tuple", <vscale x 4 x i8>, 6) [[TMP0]]
//
vbfloat16mf2x6_t test_vluxseg6ei32_v_bf16mf2x6(const __bf16 *rs1,
                                               vuint32m1_t rs2, size_t vl) {
  return __riscv_vluxseg6ei32(rs1, rs2, vl);
}

// CHECK-RV64-LABEL: define dso_local target("riscv.vector.tuple", <vscale x 8 x i8>, 6) @test_vluxseg6ei32_v_bf16m1x6(
// CHECK-RV64-SAME: ptr noundef [[RS1:%.*]], <vscale x 4 x i32> [[RS2:%.*]], i64 noundef [[VL:%.*]]) #[[ATTR0]] {
// CHECK-RV64-NEXT:  entry:
// CHECK-RV64-NEXT:    [[TMP0:%.*]] = call target("riscv.vector.tuple", <vscale x 8 x i8>, 6) @llvm.riscv.vluxseg6.triscv.vector.tuple_nxv8i8_6t.p0.nxv4i32.i64(target("riscv.vector.tuple", <vscale x 8 x i8>, 6) poison, ptr [[RS1]], <vscale x 4 x i32> [[RS2]], i64 [[VL]], i64 4)
// CHECK-RV64-NEXT:    ret target("riscv.vector.tuple", <vscale x 8 x i8>, 6) [[TMP0]]
//
vbfloat16m1x6_t test_vluxseg6ei32_v_bf16m1x6(const __bf16 *rs1, vuint32m2_t rs2,
                                             size_t vl) {
  return __riscv_vluxseg6ei32(rs1, rs2, vl);
}

// CHECK-RV64-LABEL: define dso_local target("riscv.vector.tuple", <vscale x 2 x i8>, 6) @test_vluxseg6ei32_v_bf16mf4x6_m(
// CHECK-RV64-SAME: <vscale x 1 x i1> [[VM:%.*]], ptr noundef [[RS1:%.*]], <vscale x 1 x i32> [[RS2:%.*]], i64 noundef [[VL:%.*]]) #[[ATTR0]] {
// CHECK-RV64-NEXT:  entry:
// CHECK-RV64-NEXT:    [[TMP0:%.*]] = call target("riscv.vector.tuple", <vscale x 2 x i8>, 6) @llvm.riscv.vluxseg6.mask.triscv.vector.tuple_nxv2i8_6t.p0.nxv1i32.nxv1i1.i64(target("riscv.vector.tuple", <vscale x 2 x i8>, 6) poison, ptr [[RS1]], <vscale x 1 x i32> [[RS2]], <vscale x 1 x i1> [[VM]], i64 [[VL]], i64 3, i64 4)
// CHECK-RV64-NEXT:    ret target("riscv.vector.tuple", <vscale x 2 x i8>, 6) [[TMP0]]
//
vbfloat16mf4x6_t test_vluxseg6ei32_v_bf16mf4x6_m(vbool64_t vm,
                                                 const __bf16 *rs1,
                                                 vuint32mf2_t rs2, size_t vl) {
  return __riscv_vluxseg6ei32(vm, rs1, rs2, vl);
}

// CHECK-RV64-LABEL: define dso_local target("riscv.vector.tuple", <vscale x 4 x i8>, 6) @test_vluxseg6ei32_v_bf16mf2x6_m(
// CHECK-RV64-SAME: <vscale x 2 x i1> [[VM:%.*]], ptr noundef [[RS1:%.*]], <vscale x 2 x i32> [[RS2:%.*]], i64 noundef [[VL:%.*]]) #[[ATTR0]] {
// CHECK-RV64-NEXT:  entry:
// CHECK-RV64-NEXT:    [[TMP0:%.*]] = call target("riscv.vector.tuple", <vscale x 4 x i8>, 6) @llvm.riscv.vluxseg6.mask.triscv.vector.tuple_nxv4i8_6t.p0.nxv2i32.nxv2i1.i64(target("riscv.vector.tuple", <vscale x 4 x i8>, 6) poison, ptr [[RS1]], <vscale x 2 x i32> [[RS2]], <vscale x 2 x i1> [[VM]], i64 [[VL]], i64 3, i64 4)
// CHECK-RV64-NEXT:    ret target("riscv.vector.tuple", <vscale x 4 x i8>, 6) [[TMP0]]
//
vbfloat16mf2x6_t test_vluxseg6ei32_v_bf16mf2x6_m(vbool32_t vm,
                                                 const __bf16 *rs1,
                                                 vuint32m1_t rs2, size_t vl) {
  return __riscv_vluxseg6ei32(vm, rs1, rs2, vl);
}

// CHECK-RV64-LABEL: define dso_local target("riscv.vector.tuple", <vscale x 8 x i8>, 6) @test_vluxseg6ei32_v_bf16m1x6_m(
// CHECK-RV64-SAME: <vscale x 4 x i1> [[VM:%.*]], ptr noundef [[RS1:%.*]], <vscale x 4 x i32> [[RS2:%.*]], i64 noundef [[VL:%.*]]) #[[ATTR0]] {
// CHECK-RV64-NEXT:  entry:
// CHECK-RV64-NEXT:    [[TMP0:%.*]] = call target("riscv.vector.tuple", <vscale x 8 x i8>, 6) @llvm.riscv.vluxseg6.mask.triscv.vector.tuple_nxv8i8_6t.p0.nxv4i32.nxv4i1.i64(target("riscv.vector.tuple", <vscale x 8 x i8>, 6) poison, ptr [[RS1]], <vscale x 4 x i32> [[RS2]], <vscale x 4 x i1> [[VM]], i64 [[VL]], i64 3, i64 4)
// CHECK-RV64-NEXT:    ret target("riscv.vector.tuple", <vscale x 8 x i8>, 6) [[TMP0]]
//
vbfloat16m1x6_t test_vluxseg6ei32_v_bf16m1x6_m(vbool16_t vm, const __bf16 *rs1,
                                               vuint32m2_t rs2, size_t vl) {
  return __riscv_vluxseg6ei32(vm, rs1, rs2, vl);
}
