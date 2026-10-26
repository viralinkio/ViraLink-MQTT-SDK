# PlatformIO extra script: sanitizers must be passed to the linker too.
Import("env")  # noqa: F821

env.Append(LINKFLAGS=["-fsanitize=address,undefined"])  # noqa: F821
