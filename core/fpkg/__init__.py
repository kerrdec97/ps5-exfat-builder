"""PS5 FPKG (.pkg) builder.

An independent implementation written for the exFAT Image Builder. It
prepares a Publishing Tools project (GP5) from a PS5 app folder and drives
the user's OWN ``prospero-pub-cmd.exe`` to pack it, then checks the result.

Nothing from Sony is included or downloaded: the user selects a Publishing
Tools build they are entitled to use. A stock Publishing Tools install does
not produce an installable debug FPKG; the builder detects that from the
output header and reports a failure rather than a success.

Modules:
    toolchain  find and validate the user's prospero-pub-cmd.exe
    param      read/normalise sce_sys/param.json, Content ID, output name
    scan       walk the app folder: exclusions, junk, order, path rules
    playgo     PlayGo chunk/scenario layout and placeholder payloads
    gp5        write the GP5 project XML
    runner     run img_create / img_verify / img_extract with progress
    verify     structural checks of a built .pkg
    backend    the end-to-end build used by the Build queue
"""

from core.fpkg.backend import FpkgBuildError, FpkgOptions, build_fpkg  # noqa: F401
