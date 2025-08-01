// RUN: mlir-opt %s -convert-linalg-to-std -split-input-file | FileCheck %s

func.func private @printMemrefF32(memref<*xf32>)

// CHECK:  func private @linalg_fill_f32_viewsxsxf32(f32, memref<?x?xf32, {{.*}}>) attributes {llvm.emit_c_interface}
// CHECK:  func private @linalg_matmul_viewsxsxf32_viewsxsxf32_viewsxsxf32(memref<?x?xf32, {{.*}}>, memref<?x?xf32, {{.*}}>, memref<?x?xf32, {{.*}}>) attributes {llvm.emit_c_interface}

func.func @matmul(%A: memref<?x?xf32>, %B: memref<?x?xf32>) -> (memref<?x?xf32>) {
  %c0 = arith.constant 0 : index
  %c1 = arith.constant 1 : index
  %f0 = arith.constant 0.0 : f32
  %x = memref.dim %A, %c0 : memref<?x?xf32>
  %y = memref.dim %B, %c1 : memref<?x?xf32>
  %C = memref.alloc(%x, %y) : memref<?x?xf32>

  // CHECK: call @linalg_fill_f32_viewsxsxf32({{.*}}) : (f32, memref<?x?xf32, {{.*}}>)
  linalg.fill ins(%f0 : f32) outs(%C : memref<?x?xf32>)

  // CHECK:  call @linalg_matmul_viewsxsxf32_viewsxsxf32_viewsxsxf32({{.*}}) : (memref<?x?xf32, {{.*}}>, memref<?x?xf32, {{.*}}>, memref<?x?xf32, {{.*}}>) -> ()
  linalg.matmul ins(%A, %B: memref<?x?xf32>, memref<?x?xf32>)
                outs(%C: memref<?x?xf32>)
  return %C : memref<?x?xf32>
}

// -----

#accesses = [
        affine_map<(d0) -> (d0)>,
        affine_map<(d0) -> (d0)>,
        affine_map<(d0) -> (d0)>
    ]
#trait = {
    doc = "...",
    indexing_maps = #accesses,
    library_call = "test",
    iterator_types = ["parallel"]
}

// CHECK: func.func private @linalg_copy_view32xf16as1_view32xf16as6(memref<32xf16, strided<[?], offset: ?>, 1>, memref<32xf16, strided<[?], offset: ?>, 6>) attributes {llvm.emit_c_interface}
// CHECK: func.func private @linalg_copy_view32xf16as6_view32xf16as1(memref<32xf16, strided<[?], offset: ?>, 6>, memref<32xf16, strided<[?], offset: ?>, 1>) attributes {llvm.emit_c_interface}

module {
  func.func @helper(%arg7: memref<32xf16, 1>, %arg8: memref<32xf16, 1>, %arg9: memref<32xf16, 1>) {
    %localA = memref.alloca() : memref<32xf16, 6>
    %localB = memref.alloca() : memref<32xf16, 6>
    %localOut = memref.alloca() : memref<32xf16, 6>
    linalg.copy ins(%arg8 : memref<32xf16, 1>) outs(%localA : memref<32xf16, 6>)
    linalg.copy ins(%arg9 : memref<32xf16, 1>) outs(%localB : memref<32xf16, 6>)

    linalg.generic #trait
    ins(%localA, %localB : memref<32xf16, 6>, memref<32xf16, 6>)
    outs(%localOut : memref<32xf16, 6>) {
      ^bb0(%0: f16, %1: f16, %2: f16) :
        %e = arith.addf %1, %0: f16
        linalg.yield %e : f16
    }

    linalg.copy ins(%localOut : memref<32xf16, 6>) outs(%arg7 : memref<32xf16, 1>)
    return
  }
}
