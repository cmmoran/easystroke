### CLion Setup Guide for Easystroke

If you are having trouble building or debugging Easystroke in CLion, follow these steps to ensure the IDE is correctly configured to use the existing Makefile-based build system.

#### 1. Toolchain and Makefile
Easystroke uses a `Makefile` for its build process. CLion supports this natively.

- **Makefile Project:** When you open the project, CLion should automatically detect the `Makefile`. If it doesn't, go to **File | Open** and select the `Makefile` in the root directory. Choose **Open as Project**.
- **Compiler Information:** To help CLion index the project correctly, we have updated `debug.mk` to use plain `g++` instead of `ccache g++.` While `ccache` is great for speed, it can sometimes interfere with CLion's ability to parse compiler flags and include paths.

#### 2. Run/Debug Configuration
The most common point of confusion is setting up the Run/Debug configuration.

1. Go to **Run | Edit Configurations...**
2. Click the **+** (Add New Configuration) and select **Native Application**.
3. Configure it as follows:
    - **Name:** `easystroke`
    - **Target:** `all` (This maps to the `all` target in the Makefile).
    - **Executable:** Select the `easystroke` binary in the project root.
    - **Before launch:** Ensure **Build** is in the list.
    - **Working directory:** `$PROJECT_DIR$`
    - **Program arguments:** `-vvvv` (optional, for verbose logging during debug).

#### 3. Debugging
Once the configuration is set up:
- Set breakpoints in `.cc` files (e.g., `main.cc` or `handler.cc`).
- Use **Run | Debug 'easystroke'** (or the bug icon).
- CLion will use `gdb` to debug the process.

#### 4. Troubleshooting Build Issues
If CLion still has trouble:
- **Corrupted Config:** CLion's Makefile support sometimes creates duplicate or incomplete configurations. We have cleaned up the internal project settings (`.idea/workspace.xml`) to ensure that the `easystroke` configuration correctly points to the `all` build target. If issues persist, try deleting the `.idea` folder and reopening the project (though you will lose other IDE-specific settings).
- **Toolchain:** Check **Settings | Build, Execution, Deployment | Toolchains**. Ensure a valid C++ toolchain is selected.
- Check **Settings | Build, Execution, Deployment | Makefile**. Ensure the correct `make` executable is being used.
- Try **Build | Clean** followed by **Build | Rebuild Project** within CLion.

#### Note on `debug.mk`
We have modified `debug.mk` to ensure maximum compatibility with CLion:
```make
DFLAGS   = -ggdb
OFLAGS   = 
CXX      = g++
```
This ensures that `-ggdb` (debug symbols) is always included and `g++` is used directly, which CLion prefers for extracting project structure.
