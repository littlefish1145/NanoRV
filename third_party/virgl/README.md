# VirGL Windows runtime

Native x64 UCRT builds from [MSYS2](https://packages.msys2.org/packages/mingw-w64-ucrt-x86_64-virglrenderer), dynamically loaded only with `--gpu virgl`. WGL contexts use the installed Windows OpenGL driver. No QEMU process or Linux host runtime is involved.

| Archive in this directory | Package | SHA-256 |
|---|---|---|
| virgl.pkg.tar.zst | virglrenderer 1.3.0-1 | a3fa9f88cb49f769bfc1129276ad2a73bcc2f76972e4bb7e6ad32be2c40eaefb |
| epoxy.pkg.tar.zst | libepoxy 1.5.10-7 | c6c6994e8ec49071f06cc1fae475406bdf17603d4687ad0bcac2e11078cfd7c8 |
| gcc.pkg.tar.zst | libgcc 16.2.0-4 | de65b4adae899d9278427402e29d03860bacba481794460f3a1d078610cbb783 |
| pthread.pkg.tar.zst | libwinpthread 14.0.0.r426.g4564ee4b5-1 | f8de8153bbc0e47ba244a423c12c426fa1d9f56395117ecaa34c6ad6ebed6ca3 |

Download prefix: `https://mirror.msys2.org/mingw/ucrt64/mingw-w64-ucrt-x86_64-`, followed by the package name/version and `-any.pkg.tar.zst`. Retained archives allow hash verification and recovery. License texts are in `ucrt64/share/licenses/`: VirGL and epoxy MIT; winpthreads MIT/BSD; GCC runtime GPL with GCC Runtime Library Exception.

`virglrenderer.h` has one local portability patch: `VIRGL_EXPORT` is empty under MSVC. No library binary was modified. Its public structs are used for the DLL ABI; calls resolve dynamically. DLL lookup is restricted to the bundled DLL's directory and standard Windows DLL directories.

This backend exposes the VirGL 1/2 OpenGL capsets. Venus, resource blobs, shared VRAM, and native Vulkan acceleration are not advertised. Guest Vulkan can use Mesa lavapipe software rendering independently of VirGL.
