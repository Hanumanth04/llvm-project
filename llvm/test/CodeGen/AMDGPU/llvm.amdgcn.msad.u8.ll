; RUN: llc -mtriple=amdgcn < %s | FileCheck -check-prefix=GCN %s
; RUN: llc -mtriple=amdgcn -mcpu=fiji < %s | FileCheck -check-prefix=GCN %s

declare i32 @llvm.amdgcn.msad.u8(i32, i32, i32) #0

; GCN-LABEL: {{^}}v_msad_u8:
; GCN: v_msad_u8 v{{[0-9]+}}, v{{[0-9]+}}, s{{[0-9]+}}, s{{[0-9]+}}
define amdgpu_kernel void @v_msad_u8(ptr addrspace(1) %out, i32 %src) {
  %result= call i32 @llvm.amdgcn.msad.u8(i32 %src, i32 100, i32 100) #0
  store i32 %result, ptr addrspace(1) %out, align 4
  ret void
}

; GCN-LABEL: {{^}}v_msad_u8_non_immediate:
; GCN: v_msad_u8 v{{[0-9]+}}, s{{[0-9]+}}, v{{[0-9]+}}, v{{[0-9]+}}
define amdgpu_kernel void @v_msad_u8_non_immediate(ptr addrspace(1) %out, i32 %src, i32 %a, i32 %b) {
  %result= call i32 @llvm.amdgcn.msad.u8(i32 %src, i32 %a, i32 %b) #0
  store i32 %result, ptr addrspace(1) %out, align 4
  ret void
}

attributes #0 = { nounwind readnone }
