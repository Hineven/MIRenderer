$cmake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake --build f:\CLionProjects\MIRendererDev\build --target macromc_world --config Debug -- /m:1 2>&1 | Select-String -Pattern "error C|macromc_world" | Select-Object -First 10
