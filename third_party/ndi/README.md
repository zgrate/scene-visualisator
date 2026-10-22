NDI SDK 6 headers (Processing.NDI.*.h), copied unchanged from the NDI SDK for Linux 6.2.1.
The headers alone are MIT-licensed (notice at the top of each file). The NDI runtime is not shipped:
`src/media/ndi.cpp` loads it at run time from `NDI_RUNTIME_DIR_V6` or the system library path.
"NDI" is a registered trademark of Vizrt NDI AB.
