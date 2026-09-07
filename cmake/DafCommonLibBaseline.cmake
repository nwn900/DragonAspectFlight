# Portable CommonLibSSE-NG compatibility contract for Dragon Aspect Flight.
#
# The commit is a known-good baseline, not a repository path or a checkout
# requirement. Newer CommonLibSSE-NG commits are supported when they expose
# at least this project version and retain the capability checks in the main
# CMake project and Compatibility.h.
# Keep these as project-owned values rather than cache variables: a command
# line override must not be able to lower the minimum compatibility contract.
set(DAF_MINIMUM_COMMONLIB_VERSION "6.5.0")
set(DAF_KNOWN_GOOD_COMMONLIB_COMMIT
    "c3d106cc14bfbc5db36f92345c0e20aa5dad42b8")
