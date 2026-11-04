#pragma once

// Queue jobs for .ffpkg images.
//
//  image.ffpkg.build   {source, output, replaceExisting?}
//      Scanning -> Writing image (to <output>.part) -> Verifying; the image
//      gets its final name only after verification passed (audit H6).
//  image.ffpkg.verify  {image, source?}
//      Checking structure -> Comparing (with `source`, or reading every file).
//  image.ffpkg.extract {image, output, path?}
//      Reading image -> Extracting (into <output>.part) -> Verifying.
//  image.ffpkg.edit    {image, operations: [...]}  (see Ufs2Edit.h)
//      Reading image -> Writing image -> Verifying -> Replacing original; the
//      original is replaced only after the rebuilt image verified.

#include "jobs/Job.h"

namespace fw::ufs2 {

void registerJobs(JobFactory& jobs);

} // namespace fw::ufs2
