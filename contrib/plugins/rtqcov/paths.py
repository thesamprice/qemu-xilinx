"""Source-path normalization shared by the covered and coverable producers.

Identical normalization on both sides is what lets coverage merge by
(source path + line) instead of by address.
"""

import os

# Source-path markers, in priority order. The first marker found in the raw
# path determines the normalized (repo-relative) path.
DEFAULT_MARKERS = ("/cpukit/", "/bsps/")
TESTSUITES_MARKER = "/testsuites/"

# Top-level RTEMS source trees that count as OS coverage. cpukit/bsps are the
# core OS; contrib holds vendored OS code (e.g. contrib/cpukit/jffs2). Anything
# else under the source root (build/, spec/, testsuites/, ...) is excluded.
_OS_ROOTS = ("cpukit", "bsps", "contrib")

# Cache: realpath() does filesystem stats, and normalize_path is called once
# per addr2line frame (tens of thousands of times) but over few unique paths.
_norm_cache = {}
_MISS = object()


def normalize_path(raw, source_root=None, include_testsuites=False,
                   extra_markers=(), all_paths=False):
    """Normalize an absolute build path to repo-relative form.

    Returns None for paths that should be excluded from coverage.

    Modes:
      * default (RTEMS OS coverage): source-root-relative via realpath (which
        resolves '..' and symlink prefixes like /local), keeping only the
        cpukit/bsps/contrib trees; otherwise /cpukit//bsps (+ any extra_markers)
        substring stripping. So '.../build/x/../../../contrib/cpukit/jffs2/n.c'
        becomes 'contrib/cpukit/jffs2/n.c'.
      * extra_markers: additional "keep from here" substrings (e.g. a project
        directory name) so non-RTEMS source is kept too, normalized relative to
        that marker.
      * all_paths=True: keep every real source file by its ABSOLUTE path. Use
        this for application ELFs that mix RTEMS and project code living in
        different trees -- genhtml then reads each file from its real location.
        (Absolute paths defeat cross-ELF merge-by-source, so only use it for
        single-ELF reports.)
    """
    if not raw or raw == "??":
        return None

    key = (raw, source_root, include_testsuites, extra_markers, all_paths)
    cached = _norm_cache.get(key, _MISS)
    if cached is not _MISS:
        return cached

    result = _normalize_uncached(raw, source_root, include_testsuites,
                                 extra_markers, all_paths)
    _norm_cache[key] = result
    return result


def _normalize_uncached(raw, source_root, include_testsuites,
                        extra_markers, all_paths):
    real = os.path.realpath(raw)
    sep = os.sep

    if all_paths:
        # Keep every source file by absolute path; still drop the testsuites
        # tree unless explicitly requested.
        if not include_testsuites and (sep + "testsuites" + sep) in real:
            return None
        return real

    if source_root:
        sr = os.path.realpath(source_root)
        if real.startswith(sr + sep):
            rel = os.path.relpath(real, sr)
            top = rel.split(sep, 1)[0]
            if top == "testsuites":
                return rel if include_testsuites else None
            if top in _OS_ROOTS:
                return rel
            # Otherwise fall through to marker stripping (e.g. extra_markers
            # may keep project code that lives under the source root).

    slashed = real if real.startswith("/") else "/" + real
    for marker in DEFAULT_MARKERS + tuple(extra_markers):
        idx = slashed.find(marker)
        if idx != -1:
            return slashed[idx + 1:]
    idx = slashed.find(TESTSUITES_MARKER)
    if idx != -1:
        return slashed[idx + 1:] if include_testsuites else None

    # Unknown system path (newlib, crt objects, toolchain headers): exclude.
    return None
