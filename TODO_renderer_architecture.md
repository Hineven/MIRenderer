# RDG Compile-Time Pool Resource Aliasing

## 背景

我们需要在RDG编译时，就确定每个涉及到的RDGResource底层Mapping到哪一个RDGAllocation - 到哪一个RHIResource。

这样，我们就能真正的ahead of time在RDG执行前创建所有param table，并正确进行resource aliasing了。

目前，我们只能 / 还不能搞这玩意。
