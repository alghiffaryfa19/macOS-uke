; uke macOS: the compositor draws half the panel (native-gop-scale=2: a 1600x1068 BGRA surface for the 3200x2136 scanout), so the GPU
; scanout doubles it: every scanout pixel takes the source pixel at (x / 2, y / 2), with alpha 0xff as scanout_opaque stores it
; (drivers/AdrenoMetal/shaders/scanout_convert.ll). The grid covers the scanout.
;   kernel void scanout_opaque_x2(device const uint *src [[buffer(0)]], device uint *dst [[buffer(1)]], constant uint4 &p [[buffer(3)]],
;                                 uint2 gid [[thread_position_in_grid]])
;   p = {scanout width, scanout height, source stride in words, scanout stride in words}
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.0.0"

define void @scanout_opaque_x2(ptr addrspace(1) noundef readonly "air-buffer-no-alias" %src, ptr addrspace(1) noundef "air-buffer-no-alias" %dst, ptr addrspace(2) noundef readonly align 16 dereferenceable(16) "air-buffer-no-alias" %p, <2 x i32> noundef %gid) local_unnamed_addr #0 {
entry:
  %w = load i32, ptr addrspace(2) %p, align 16
  %ph = getelementptr inbounds i32, ptr addrspace(2) %p, i64 1
  %h = load i32, ptr addrspace(2) %ph, align 4
  %pss = getelementptr inbounds i32, ptr addrspace(2) %p, i64 2
  %ss = load i32, ptr addrspace(2) %pss, align 8
  %pds = getelementptr inbounds i32, ptr addrspace(2) %p, i64 3
  %ds = load i32, ptr addrspace(2) %pds, align 4
  %x = extractelement <2 x i32> %gid, i64 0
  %y = extractelement <2 x i32> %gid, i64 1
  %inx = icmp ult i32 %x, %w
  %iny = icmp ult i32 %y, %h
  %in = and i1 %inx, %iny
  br i1 %in, label %body, label %done

body:
  %sx = lshr i32 %x, 1
  %sy = lshr i32 %y, 1
  %sx64 = zext i32 %sx to i64
  %sy64 = zext i32 %sy to i64
  %ss64 = zext i32 %ss to i64
  %srow = mul i64 %sy64, %ss64
  %si = add i64 %srow, %sx64
  %sp = getelementptr inbounds i32, ptr addrspace(1) %src, i64 %si
  %v = load i32, ptr addrspace(1) %sp, align 4
  %px = or i32 %v, -16777216
  %x64 = zext i32 %x to i64
  %y64 = zext i32 %y to i64
  %ds64 = zext i32 %ds to i64
  %drow = mul i64 %y64, %ds64
  %di = add i64 %drow, %x64
  %dp = getelementptr inbounds i32, ptr addrspace(1) %dst, i64 %di
  store i32 %px, ptr addrspace(1) %dp, align 4
  br label %done

done:
  ret void
}

attributes #0 = { mustprogress nofree norecurse nosync nounwind willreturn "frame-pointer"="all" "min-legal-vector-width"="0" "no-builtins" "no-trapping-math"="true" "stack-protector-buffer-size"="8" }

!llvm.module.flags = !{!0, !1, !2, !3, !4, !5, !6, !7, !8}
!air.kernel = !{!9}
!air.compile_options = !{!17, !18}
!llvm.ident = !{!19}
!air.version = !{!20}
!air.language_version = !{!21}

!0 = !{i32 2, !"SDK Version", [2 x i32] [i32 27, i32 0]}
!1 = !{i32 1, !"wchar_size", i32 4}
!2 = !{i32 7, !"frame-pointer", i32 2}
!3 = !{i32 7, !"air.max_device_buffers", i32 31}
!4 = !{i32 7, !"air.max_constant_buffers", i32 31}
!5 = !{i32 7, !"air.max_threadgroup_buffers", i32 31}
!6 = !{i32 7, !"air.max_textures", i32 128}
!7 = !{i32 7, !"air.max_read_write_textures", i32 8}
!8 = !{i32 7, !"air.max_samplers", i32 16}
!9 = !{ptr @scanout_opaque_x2, !10, !11}
!10 = !{}
!11 = !{!12, !13, !15, !16}
!12 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"src"}
!13 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"dst"}
!15 = !{i32 2, !"air.buffer", !"air.buffer_size", i32 16, !"air.location_index", i32 3, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"uint4", !"air.arg_name", !"p"}
!16 = !{i32 3, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint2", !"air.arg_name", !"gid"}
!17 = !{!"air.compile.denorms_disable"}
!18 = !{!"air.compile.fast_math_enable"}
!19 = !{!"Apple metal version 32023.921 (metalfe-32023.921.6)"}
!20 = !{i32 2, i32 8, i32 0}
!21 = !{!"Metal", i32 4, i32 0, i32 0}
