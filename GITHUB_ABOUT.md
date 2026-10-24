# Proposed GitHub About description

ISO-WCX — native C++20 Total Commander WCX plugin for ISO/UDF, WIM/ESD/SWM, optical images and RAW/VHD/VHDX. Win32/x64, no external extractors.

The repository About field is separate from README. The release workflow
updates it only if the optional repository Actions secret
ISO_WCX_REPO_ADMIN_TOKEN has Administration: write permission. The default
GitHub Actions token and current GitHub connector do not expose write access
to repository About metadata. README and release notes are updated regardless.

Suggested topics: total-commander, wcx, iso9660, udf, wim, vhdx,
disk-images, cpp20.
