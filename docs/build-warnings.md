# Successful build warning audit

Inspected `.git/logs_111688283183` using `tools/summarize_build_logs.py` before
reading diagnostics. The processor streams files, strips timestamps and checkout
prefixes, counts exact repeats, and groups by dependency and diagnostic code.
It prefers combined job logs over duplicate per-step logs. `--details` emits
each distinct diagnostic with its count and short CMake warning context.

| Job | Warning occurrences | Distinct diagnostic lines |
| --- | ---: | ---: |
| Windows CLI | 14,212 | 1,783 |
| macOS CLI | 8,737 | 2,402 |
| Ubuntu CLI | 18,920 | 2,900 |
| Linux desktop, latest | 18,936 | 2,914 |
| Linux desktop, distro | 18,933 | 2,911 |

These counts describe the supplied logs, before the changes below.

## Changes

* libaom's CMake helpers forcibly append compiler flags to the global CMake
  cache, including `-Wfloat-conversion`, `-Wmissing-declarations`, stack-size
  warnings and MSVC warning levels. Those settings were applied to unrelated
  codecs and application files. Preserve the caller's cache flags and apply
  libaom's added flags to libaom targets only. This also prevents repeated
  configuration from accumulating dependency warning policies.
* The reported libjxl `fast_math-inl.h` C4305 warnings come from double literals
  passed to float SIMD operations. The same headers compile repeatedly for SIMD
  targets. Scope MSVC C4305/C4244/C4267 suppression to libjxl's encoder, decoder
  and CMS targets; retain their numerical implementation and other diagnostics.
  Application targets retain their own compiler diagnostics.
* Move roughly 750 KiB LibRaw objects from the stack to RAII-managed heap storage
  during inspection and RAW decode. This reduces worker-thread stack pressure.
* Make intended float and bounded integer conversions explicit in conversion
  controls, gain-map metadata, lookup-table construction and row processing.
  Separate statements that triggered misleading-indentation warnings.
* Use Qt's `beginFilterChange`/`endFilterChange` API on Qt 6.10+, preserving the
  older filter API for supported distro Qt versions. Remove a shadowed decoder
  status and mark AVIF dimensions used only by disabled logging.

## Remaining upstream diagnostics

The original logs also contain legacy JPEG XR macro redefinitions, negative
shift and `tmpnam` warnings; LibRaw signedness/deprecation warnings; and upstream
Highway CMake compatibility notices. These require separate upstream review.
No global warning disable or codec arithmetic rewrite is applied to hide them.
The warning audit does not establish that these upstream paths are bug-free.

For an existing build directory with libaom's old cached flags, use a fresh build
directory, or clear the affected `CMAKE_C_FLAGS_<CONFIG>` and
`CMAKE_CXX_FLAGS_<CONFIG>` cache entries once before configuring. Preserve any
custom compiler flags you intentionally supplied.
