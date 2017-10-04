; ModuleID = 'nsync.bc'
source_filename = "nsync.ll"
target datalayout = "e-p:64:64-p1:64:64-p2:64:64-p3:32:32-p4:32:32-p5:32:32-i64:64-v16:16-v24:32-v32:32-v48:64-v96:128-v192:256-v256:256-v512:512-v1024:1024-v2048:2048-n32:64-A5"
target triple = "amdgcn--cuda"

; Function Attrs: convergent nounwind
declare void @llvm.amdgcn.s.barrier() #0

; Function Attrs: alwaysinline nounwind
define void @__named_sync(i32,i32) local_unnamed_addr #1 {
  tail call void @llvm.amdgcn.s.barrier() #0
  ret void
}

attributes #0 = { convergent nounwind }
attributes #1 = { alwaysinline nounwind }

